#include "jaxlib/gpu/triton_kernels.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "absl/base/optimization.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/fixed_array.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "jaxlib/gpu/gpu_kernel_helpers.h"
#include "jaxlib/gpu/triton.pb.h"
#include "jaxlib/gpu/triton_utils.h"
#include "jaxlib/gpu/vendor.h"
#include "xla/service/custom_call_status.h"
#include "xla/stream_executor/gpu/asm_compiler.h"
#include "tsl/platform/env.h"

#define GPU_RETURN_IF_ERROR(expr) JAX_RETURN_IF_ERROR(JAX_AS_STATUS(expr))


namespace jax::JAX_GPU_NAMESPACE {
namespace {

constexpr float kBenchmarkTimeMillis = 10.;

struct gpuModuleDeleter {
  void operator()(gpuModule_t module) { gpuModuleUnload(module); }
};

using OwnedGPUmodule =
    std::unique_ptr<std::remove_pointer_t<gpuModule_t>, gpuModuleDeleter>;

absl::StatusOr<ModuleImage*> GetModuleImage(std::string kernel_name,
                                            uint32_t shared_mem_bytes,
                                            std::string_view ptx,
                                            int compute_capability) {
  auto key =
      std::make_tuple(kernel_name, shared_mem_bytes, ptx, compute_capability);

  static absl::Mutex mutex;
  static auto& module_images =
      *new absl::flat_hash_map<decltype(key), std::unique_ptr<ModuleImage>>
          ABSL_GUARDED_BY(mutex);

  absl::MutexLock lock(&mutex);
  auto it = module_images.find(key);
  if (it != module_images.end()) return it->second.get();

#ifdef JAX_GPU_HIP //For HIP/ROCM just read the hsaco file
  std::string result_blob;
  std::string fname{ptx}; 
  TF_RETURN_IF_ERROR(
      tsl::ReadFileToString(tsl::Env::Default(), fname, &result_blob));
  std::vector<uint8_t> module_image(result_blob.begin(), result_blob.end());
#else
  // TODO(cjfj): Support `TRITON_PTXAS_PATH` environment variable?
  int cc_major = compute_capability / 10;
  int cc_minor = compute_capability % 10;
  JAX_ASSIGN_OR_RETURN(
      std::vector<uint8_t> module_image,
      stream_executor::CompileGpuAsm(cc_major, cc_minor, ptx.data(),
                                     stream_executor::GpuAsmOpts{}));
#endif

  auto [it2, success] = module_images.insert(
      {std::move(key),
       std::make_unique<ModuleImage>(
           std::move(kernel_name), std::move(module_image), shared_mem_bytes)});
  CHECK(success);
  return it2->second.get();
}

absl::StatusOr<float> Benchmark(gpuStream_t stream, KernelCall& kernel_call,
                                void** buffers, int num_iterations) {
  gpuEvent_t start, stop;
  GPU_RETURN_IF_ERROR(gpuEventCreate(&start, /*Flags=*/GPU_EVENT_DEFAULT));
  GPU_RETURN_IF_ERROR(gpuEventCreate(&stop, /*Flags=*/GPU_EVENT_DEFAULT));
  JAX_RETURN_IF_ERROR(kernel_call.Launch(stream, buffers));  // Warm-up.
  GPU_RETURN_IF_ERROR(gpuEventRecord(start, stream));
  for (int i = 0; i < num_iterations; ++i) {
    JAX_RETURN_IF_ERROR(kernel_call.Launch(stream, buffers));
  }
  GPU_RETURN_IF_ERROR(gpuEventRecord(stop, stream));
  GPU_RETURN_IF_ERROR(gpuEventSynchronize(stop));
  float elapsed_ms;
  GPU_RETURN_IF_ERROR(gpuEventElapsedTime(&elapsed_ms, start, stop));
  GPU_RETURN_IF_ERROR(gpuEventDestroy(start));
  GPU_RETURN_IF_ERROR(gpuEventDestroy(stop));
  return elapsed_ms;
}

absl::StatusOr<KernelCall*> GetKernelCall(absl::string_view opaque,
                                          gpuStream_t stream, void** buffers) {
  static absl::Mutex mutex;
  static auto& kernel_calls =
      *new absl::flat_hash_map<std::string, std::unique_ptr<KernelCall>>
          ABSL_GUARDED_BY(mutex);

  size_t hash = kernel_calls.hash_function()(opaque);
  // TODO(cjfj): Use `ReaderMutexLock`?
  absl::MutexLock lock(&mutex);
  auto it = kernel_calls.find(opaque, hash);
  if (ABSL_PREDICT_TRUE(it != kernel_calls.end())) return it->second.get();

  // The opaque data is a zlib compressed protobuf.
  JAX_ASSIGN_OR_RETURN(std::string serialized, ZlibUncompress(opaque));

  jax_triton::TritonAnyKernelCall proto;
  if (!proto.ParseFromString(serialized)) {
    return absl::InvalidArgumentError("Failed to parse serialized data.");
  }

  std::unique_ptr<KernelCall> kernel_call;
  if (proto.has_kernel_call()) {
    JAX_ASSIGN_OR_RETURN(KernelCall kernel_call_,
                         KernelCall::FromProto(proto.kernel_call()));
    kernel_call = std::make_unique<KernelCall>(std::move(kernel_call_));
  } else if (proto.has_autotuned_kernel_call()) {
    JAX_ASSIGN_OR_RETURN(
        AutotunedKernelCall autotuned_call,
        AutotunedKernelCall::FromProto(proto.autotuned_kernel_call()));
    {
      JAX_ASSIGN_OR_RETURN(KernelCall kernel_call_,
                           AutotunedKernelCall::Autotune(
                               std::move(autotuned_call), stream, buffers));
      kernel_call = std::make_unique<KernelCall>(std::move(kernel_call_));
    }
  } else {
    return absl::InvalidArgumentError("Unknown kernel call type.");
  }

  auto [it2, success] =
      kernel_calls.insert({std::string(opaque), std::move(kernel_call)});
  CHECK(success);
  return it2->second.get();
}

}  // namespace

class ModuleImage {
 public:
  ModuleImage(std::string_view kernel_name, std::vector<uint8_t> module_image,
              uint32_t shared_mem_bytes)
      : kernel_name_(kernel_name),
        module_image_(std::move(module_image)),
        shared_mem_bytes_(shared_mem_bytes) {}

  absl::StatusOr<gpuFunction_t> GetFunctionForContext(gpuContext_t context) {
    absl::MutexLock lock(&mutex_);
    auto it = functions_.find(context);
    if (ABSL_PREDICT_TRUE(it != functions_.end())) {
      return it->second;
    }

    GPU_RETURN_IF_ERROR(gpuCtxPushCurrent(context));
    absl::Cleanup ctx_restorer = [] { gpuCtxPopCurrent(nullptr); };

    gpuModule_t module;
    GPU_RETURN_IF_ERROR(gpuModuleLoadData(&module, module_image_.data()));
    modules_.push_back(OwnedGPUmodule(module, gpuModuleDeleter()));

    gpuFunction_t function;
    GPU_RETURN_IF_ERROR(
        gpuModuleGetFunction(&function, module, kernel_name_.c_str()));
    auto [_, success] = functions_.insert({context, function});
    CHECK(success);

    // The maximum permitted static shared memory allocation in CUDA is 48kB,
    // but we can expose more to the kernel using dynamic shared memory.
    constexpr int kMaxStaticSharedMemBytes = 49152;
    if (shared_mem_bytes_ <= kMaxStaticSharedMemBytes) {
      return function;
    }

    // Set up dynamic shared memory.
    gpuDevice_t device;
    GPU_RETURN_IF_ERROR(gpuCtxGetDevice(&device));

    int shared_optin;
    GPU_RETURN_IF_ERROR(gpuDeviceGetAttribute(
        &shared_optin, GPU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK_OPTIN,
        device));

    if (shared_mem_bytes_ > shared_optin) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Shared memory requested (%d b) exceeds device resources (%d b).",
          shared_mem_bytes_, shared_optin));
    }

    if (shared_optin > kMaxStaticSharedMemBytes) {
#ifdef JAX_GPU_CUDA
      GPU_RETURN_IF_ERROR(
          gpuFuncSetCacheConfig(function, CU_FUNC_CACHE_PREFER_SHARED));
#endif
      int shared_total;
      GPU_RETURN_IF_ERROR(gpuDeviceGetAttribute(
          &shared_total,
          GPU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_MULTIPROCESSOR, device));
      int shared_static;
      GPU_RETURN_IF_ERROR(gpuFuncGetAttribute(
          &shared_static, GPU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES, function));
      #ifdef JAX_GPU_CUDA
        GPU_RETURN_IF_ERROR(cuFuncSetAttribute(
          function, GPU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES,
          shared_optin - shared_static));
      #endif
    }
    return function;
  }

 private:
  std::string kernel_name_;
  std::vector<uint8_t> module_image_;
  uint32_t shared_mem_bytes_;

  absl::Mutex mutex_;
  std::vector<OwnedGPUmodule> modules_ ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<gpuContext_t, gpuFunction_t> functions_ ABSL_GUARDED_BY(mutex_);
};

Kernel::Kernel(std::string kernel_name, uint32_t num_warps,
               uint32_t shared_mem_bytes, std::string ptx, std::string ttir,
               int compute_capability)
    : kernel_name_(std::move(kernel_name)),
      block_dim_x_(num_warps * kNumThreadsPerWarp),
      shared_mem_bytes_(shared_mem_bytes),
      ptx_(std::move(ptx)),
      ttir_(std::move(ttir)),
      compute_capability_(compute_capability) {}

absl::Status Kernel::Launch(gpuStream_t stream, uint32_t grid[3], void** params) {
  if (ABSL_PREDICT_FALSE(module_image_ == nullptr)) {
    JAX_ASSIGN_OR_RETURN(module_image_,
                         GetModuleImage(kernel_name_, shared_mem_bytes_, ptx_,
                                        compute_capability_));
  }

  gpuContext_t context; 
  #ifdef JAX_GPU_HIP
    int device_id = gpuGetStreamDeviceId(stream);
    gpuDevice_t device;
    GPU_RETURN_IF_ERROR(gpuDeviceGet(&device, device_id));
    GPU_RETURN_IF_ERROR(gpuDevicePrimaryCtxRetain(&context, device));
  #else //JAX_GPU_CUDA
    GPU_RETURN_IF_ERROR(gpuStreamGetCtx(stream, &context)); 
  #endif
  JAX_ASSIGN_OR_RETURN(gpuFunction_t kernel,
                       module_image_->GetFunctionForContext(context));
  return JAX_AS_STATUS(gpuLaunchKernel(
      kernel, grid[0], grid[1], grid[2], block_dim_x_,
      /*blockDimY=*/1, /*blockDimZ=*/1, shared_mem_bytes_, stream, params,
      /*extra=*/nullptr));
}

/*static*/ Kernel Kernel::FromProto(const jax_triton::TritonKernel& proto) {
  return Kernel(proto.kernel_name(), proto.num_warps(),
                proto.shared_mem_bytes(), proto.ptx(), proto.ttir(),
                proto.compute_capability());
}

jax_triton::TritonKernel Kernel::ToProto() const {
  jax_triton::TritonKernel proto;
  proto.set_kernel_name(kernel_name_);
  proto.set_num_warps(block_dim_x_ / kNumThreadsPerWarp);
  proto.set_shared_mem_bytes(shared_mem_bytes_);
  proto.set_ptx(ptx_);
  proto.set_ttir(ttir_);
  proto.set_compute_capability(compute_capability_);
  return proto;
}

/*static*/ absl::StatusOr<KernelCall::Parameter>
KernelCall::Parameter::FromProto(
    const jax_triton::TritonKernelCall_Parameter& proto) {
  using jax_triton::TritonKernelCall_Parameter;
  Parameter param;
  switch (proto.value_case()) {
    case TritonKernelCall_Parameter::kArray:
      param.value = Array{proto.array().bytes_to_zero(),
                          proto.array().ptr_divisibility()};
      break;
    case TritonKernelCall_Parameter::kBool:
      param.value = proto.bool_();
      break;
    case TritonKernelCall_Parameter::kI32:
      param.value = proto.i32();
      break;
    case TritonKernelCall_Parameter::kU32:
      param.value = proto.u32();
      break;
    case TritonKernelCall_Parameter::kI64:
      param.value = proto.i64();
      break;
    case TritonKernelCall_Parameter::kU64:
      param.value = proto.u64();
      break;
    case TritonKernelCall_Parameter::kF32:
      param.value = proto.f32();
      break;
    case TritonKernelCall_Parameter::kF64:
      param.value = proto.f64();
      break;
    default:
      return absl::InvalidArgumentError("Unknown scalar parameter type.");
  }
  return param;
}

jax_triton::TritonKernelCall_Parameter KernelCall::Parameter::ToProto() const {
  jax_triton::TritonKernelCall_Parameter proto;
  if (std::holds_alternative<Array>(value)) {
    proto.mutable_array()->set_bytes_to_zero(
        std::get<Array>(value).bytes_to_zero);
    proto.mutable_array()->set_ptr_divisibility(
        std::get<Array>(value).ptr_divisibility);
  } else if (std::holds_alternative<bool>(value)) {
    proto.set_bool_(std::get<bool>(value));
  } else if (std::holds_alternative<int32_t>(value)) {
    proto.set_i32(std::get<int32_t>(value));
  } else if (std::holds_alternative<uint32_t>(value)) {
    proto.set_u32(std::get<uint32_t>(value));
  } else if (std::holds_alternative<int64_t>(value)) {
    proto.set_i64(std::get<int64_t>(value));
  } else if (std::holds_alternative<uint64_t>(value)) {
    proto.set_u64(std::get<uint64_t>(value));
  } else if (std::holds_alternative<float>(value)) {
    proto.set_f32(std::get<float>(value));
  } else {
    CHECK(std::holds_alternative<double>(value));
    proto.set_f64(std::get<double>(value));
  }
  return proto;
}

KernelCall::KernelCall(Kernel kernel, uint32_t grid_0, uint32_t grid_1,
                       uint32_t grid_2, std::vector<Parameter> parameters)
    : kernel_(std::move(kernel)),
      grid_{grid_0, grid_1, grid_2},
      parameters_(std::move(parameters)) {}

absl::Status KernelCall::Launch(gpuStream_t stream, void** buffers) {
  absl::FixedArray<void*> params(parameters_.size());
  for (size_t i = 0; i < parameters_.size(); ++i) {
    const Parameter& param = parameters_[i];
    if (std::holds_alternative<Parameter::Array>(param.value)) {
      const auto& array = std::get<Parameter::Array>(param.value);
      void*& ptr = *(buffers++);
      auto cu_ptr = reinterpret_cast<gpuDevicePtr_t>(ptr);

      if (ABSL_PREDICT_FALSE((array.ptr_divisibility != 0) &&
                             ((size_t)cu_ptr % array.ptr_divisibility != 0))) {
        return absl::InvalidArgumentError(
            absl::StrFormat("Parameter %zu (%zu) is not divisible by %d.", i,
                            (size_t)ptr, array.ptr_divisibility));
      }

      if (array.bytes_to_zero > 0) {
        GPU_RETURN_IF_ERROR(
            gpuMemsetD8Async(cu_ptr, 0, array.bytes_to_zero, stream));
      }
      params[i] = &ptr;
    } else {
      params[i] = const_cast<void*>(std::visit(
          [](auto&& arg) { return reinterpret_cast<const void*>(&arg); },
          param.value));
    }
  }

  return kernel_.Launch(stream, grid_, params.data());
}

/*static*/ absl::StatusOr<KernelCall> KernelCall::FromProto(
    const jax_triton::TritonKernelCall& proto) {
  std::vector<KernelCall::Parameter> parameters;
  for (const jax_triton::TritonKernelCall_Parameter& parameter :
       proto.parameters()) {
    JAX_ASSIGN_OR_RETURN(Parameter p, Parameter::FromProto(parameter));
    parameters.push_back(p);
  }

  return KernelCall(Kernel::FromProto(proto.kernel()), proto.grid_0(),
                    proto.grid_1(), proto.grid_2(), std::move(parameters));
}

jax_triton::TritonKernelCall KernelCall::ToProto() const {
  jax_triton::TritonKernelCall proto;
  *proto.mutable_kernel() = kernel_.ToProto();
  proto.set_grid_0(grid_[0]);
  proto.set_grid_1(grid_[1]);
  proto.set_grid_2(grid_[2]);
  for (const Parameter& param : parameters_) {
    *proto.add_parameters() = param.ToProto();
  }
  return proto;
}

AutotunedKernelCall::AutotunedKernelCall(
    std::string name, std::vector<Config> configs,
    std::vector<std::tuple<size_t, size_t, size_t>> input_output_aliases)
    : name_(std::move(name)),
      configs_(std::move(configs)),
      input_output_aliases_(std::move(input_output_aliases)) {}

/*static*/ absl::StatusOr<AutotunedKernelCall> AutotunedKernelCall::FromProto(
    const jax_triton::TritonAutotunedKernelCall& proto) {
  std::vector<Config> configs;
  for (const jax_triton::TritonAutotunedKernelCall_Config& config :
       proto.configs()) {
    JAX_ASSIGN_OR_RETURN(auto kernel_call,
                         KernelCall::FromProto(config.kernel_call()));
    configs.push_back(Config{std::move(kernel_call), config.description()});
  }

  std::vector<std::tuple<size_t, size_t, size_t>> input_output_aliases;
  for (const jax_triton::TritonAutotunedKernelCall_InputOutputAlias& a :
       proto.input_output_aliases()) {
    input_output_aliases.push_back(std::make_tuple(
        a.input_buffer_idx(), a.output_buffer_idx(), a.buffer_size_bytes()));
  }

  return AutotunedKernelCall(proto.name(), std::move(configs),
                             std::move(input_output_aliases));
}

jax_triton::TritonAutotunedKernelCall AutotunedKernelCall::ToProto() const {
  jax_triton::TritonAutotunedKernelCall proto;
  proto.set_name(name_);
  for (const Config& config : configs_) {
    jax_triton::TritonAutotunedKernelCall_Config* c = proto.add_configs();
    *c->mutable_kernel_call() = config.kernel_call.ToProto();
    c->set_description(config.description);
  }
  for (const auto& [input_idx, output_idx, size] : input_output_aliases_) {
    jax_triton::TritonAutotunedKernelCall_InputOutputAlias* a =
        proto.add_input_output_aliases();
    a->set_input_buffer_idx(input_idx);
    a->set_output_buffer_idx(output_idx);
    a->set_buffer_size_bytes(size);
  }
  return proto;
}

/*static*/ absl::StatusOr<KernelCall> AutotunedKernelCall::Autotune(
    AutotunedKernelCall kernel_call, gpuStream_t stream, void** buffers) {
  // If an input aliases with an output, it will get overwritten during the
  // kernel execution. If the kernel is called repeatedly, as we do during
  // auto-tuning, the final result will be junk, so we take a copy of the
  // input to restore after auto-tuning.
  std::unordered_map<size_t, std::vector<uint8_t>> input_copies;
  for (auto [input_idx, output_idx, size] : kernel_call.input_output_aliases_) {
    if (buffers[input_idx] == buffers[output_idx]) {
      std::vector<uint8_t> input_copy(size);
      GPU_RETURN_IF_ERROR(gpuMemcpyDtoHAsync(
          input_copy.data(), reinterpret_cast<gpuDevicePtr_t>(buffers[input_idx]),
          size, stream));
      input_copies[input_idx] = std::move(input_copy);
    }
  }

  LOG(INFO) << "Autotuning function: " << kernel_call.name_;
  // First run a single iteration of each to config to determine how many
  // iterations to run for benchmarking.
  float best = std::numeric_limits<float>::infinity();
  for (Config& config : kernel_call.configs_) {
    JAX_ASSIGN_OR_RETURN(float t,
                         Benchmark(stream, config.kernel_call, buffers, 1));
    LOG(INFO) << config.description << ", ran 1 iter in " << t << " ms";
    best = std::min(best, t);
  }

  int timed_iters = std::max(static_cast<int>(kBenchmarkTimeMillis / best), 1);
  if (timed_iters > 100) {
    timed_iters = 100;
    LOG(INFO) << "Benchmarking with 100 iters (capped at 100)";
  } else {
    timed_iters = std::min(timed_iters, 100);
    LOG(INFO) << "Benchmarking with " << timed_iters
              << " iters (target time: " << kBenchmarkTimeMillis << " ms)";
  }

  best = std::numeric_limits<float>::infinity();
  for (Config& config : kernel_call.configs_) {
    JAX_ASSIGN_OR_RETURN(
        float t, Benchmark(stream, config.kernel_call, buffers, timed_iters));
    LOG(INFO) << config.description << ", ran " << timed_iters << " iters in "
              << t << " ms";

    if (t < best) {
      LOG(INFO) << config.description << " is the new best config";
      best = t;
      std::swap(config, kernel_call.configs_[0]);
    }
  }

  LOG(INFO) << "Finished autotuning function: " << kernel_call.name_
            << " best config " << kernel_call.configs_[0].description;

  // Restore aliased inputs to their original values.
  for (auto [input_idx, _, size] : kernel_call.input_output_aliases_) {
    GPU_RETURN_IF_ERROR(
        gpuMemcpyHtoDAsync(reinterpret_cast<gpuDevicePtr_t>(buffers[input_idx]),
                          input_copies[input_idx].data(), size, stream));
  }
  // Synchronize stream to ensure copies are complete before the host copy
  // is deleted.
  GPU_RETURN_IF_ERROR(gpuStreamSynchronize(stream));
  return std::move(kernel_call.configs_[0].kernel_call);
}

void TritonKernelCall(gpuStream_t stream, void** buffers, const char* opaque,
                      size_t opaque_len, XlaCustomCallStatus* status) {
  absl::Status result = [=] {
    JAX_ASSIGN_OR_RETURN(
        KernelCall * kernel_call,
        GetKernelCall(absl::string_view(opaque, opaque_len), stream, buffers));
    return kernel_call->Launch(stream, buffers);
  }();
  if (ABSL_PREDICT_FALSE(!result.ok())) {
    absl::string_view msg = result.message();
    XlaCustomCallStatusSetFailure(status, msg.data(), msg.length());
  }
}

}  // namespace jax::JAX_GPU_NAMESPACE
