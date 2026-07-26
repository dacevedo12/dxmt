#include "d3d12_pipeline.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "dxmt_format.hpp"
#include "dxmt_d3d12_test_path.hpp"
#include "dxmt_pipeline_diag.hpp"
#include "dxmt_perf_stats.hpp"
#include "dxmt_shader_cache.hpp"
#include "log/log.hpp"
#include "sha1/sha1_util.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <version.h>
#include <DXBCParser/DXBCUtils.h>
#include <DXBCParser/d3d12tokenizedprogramformat.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#if !defined(_WIN32)
#include <execinfo.h>
#endif
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <string_view>
#include <span>
#include <unordered_map>
#include <utility>

namespace dxmt::d3d12 {

static std::atomic<uint64_t> g_pipeline_state_cache_identity = 1;

struct PipelineNativeArtifact {
  explicit PipelineNativeArtifact(PipelineStateType pipeline_type)
      : type(pipeline_type) {}

  PipelineStateType type;
  PipelineMetalGraphicsState graphics;
  PipelineMetalComputeState compute;
};

// Completed entries are weak so the cache never extends an API object's
// lifetime. Pending entries are strong and single-flight one synchronous Metal
// compilation for every semantic key on this D3D12 device.
class PipelineNativeArtifactCache {
public:
  template <typename Factory>
  std::shared_ptr<PipelineNativeArtifact>
  Acquire(const std::string &key, dxmt::perf::PsoArtifactKind kind,
          Factory &&factory) {
    std::shared_ptr<Pending> pending;
    bool compile = false;
    {
      std::lock_guard lock(mutex_);
      if (const auto ready = ready_.find(key); ready != ready_.end()) {
        if (auto artifact = ready->second.lock()) {
          ++stats_.hits;
          dxmt::perf::recordPsoArtifactCacheLookup(
              kind, dxmt::perf::PsoArtifactCacheResult::Hit);
          return artifact;
        }
        ready_.erase(ready);
      }

      if (const auto in_flight = pending_.find(key);
          in_flight != pending_.end()) {
        pending = in_flight->second;
        ++stats_.waits;
      } else {
        pending = std::make_shared<Pending>();
        pending_.emplace(key, pending);
        compile = true;
        ++stats_.misses;
        PruneExpiredLocked();
      }
    }

    if (!compile) {
      const auto wait_start = std::chrono::steady_clock::now();
      std::unique_lock lock(pending->mutex);
      pending->condition.wait(lock, [&] { return pending->complete; });
      auto artifact = pending->artifact;
      lock.unlock();
      dxmt::perf::recordPsoArtifactCacheLookup(
          kind, dxmt::perf::PsoArtifactCacheResult::Wait,
          ElapsedMicroseconds(wait_start));
      return artifact;
    }

    dxmt::perf::recordPsoArtifactCacheLookup(
        kind, dxmt::perf::PsoArtifactCacheResult::Miss);
    const auto compile_start = std::chrono::steady_clock::now();
    std::shared_ptr<PipelineNativeArtifact> artifact;
    try {
      artifact = std::forward<Factory>(factory)();
    } catch (...) {
      dxmt::perf::recordPsoArtifactCompile(
          kind, ElapsedMicroseconds(compile_start), false);
      Publish(key, pending, nullptr);
      throw;
    }
    dxmt::perf::recordPsoArtifactCompile(
        kind, ElapsedMicroseconds(compile_start), bool(artifact));
    Publish(key, pending, artifact);
    return artifact;
  }

  PipelineNativeArtifactCacheStats Stats() {
    std::lock_guard lock(mutex_);
    return stats_;
  }

private:
  struct Pending {
    std::mutex mutex;
    std::condition_variable condition;
    std::shared_ptr<PipelineNativeArtifact> artifact;
    bool complete = false;
  };

  static uint64_t
  ElapsedMicroseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
  }

  void Publish(const std::string &key, const std::shared_ptr<Pending> &pending,
               const std::shared_ptr<PipelineNativeArtifact> &artifact) {
    {
      std::lock_guard lock(pending->mutex);
      pending->artifact = artifact;
      pending->complete = true;
    }
    {
      std::lock_guard lock(mutex_);
      ++stats_.compiles;
      if (!artifact)
        ++stats_.compile_failures;
      if (artifact) {
        try {
          ready_.insert_or_assign(key, artifact);
        } catch (...) {
          // The compiled artifact is still valid for the creator and current
          // waiters. A bookkeeping allocation failure only disables reuse for
          // the next Create call; it must not strand the pending wave.
        }
      }
      if (const auto entry = pending_.find(key);
          entry != pending_.end() && entry->second == pending)
        pending_.erase(entry);
    }
    pending->condition.notify_all();
  }

  void PruneExpiredLocked() {
    constexpr size_t kPruneThreshold = 4096;
    constexpr uint64_t kPruneInterval = 256;
    if (ready_.size() < kPruneThreshold ||
        ++insertion_count_ % kPruneInterval)
      return;
    for (auto entry = ready_.begin(); entry != ready_.end();) {
      if (entry->second.expired())
        entry = ready_.erase(entry);
      else
        ++entry;
    }
  }

  std::mutex mutex_;
  std::unordered_map<std::string, std::weak_ptr<PipelineNativeArtifact>> ready_;
  std::unordered_map<std::string, std::shared_ptr<Pending>> pending_;
  PipelineNativeArtifactCacheStats stats_;
  uint64_t insertion_count_ = 0;
};

std::shared_ptr<PipelineNativeArtifactCache>
CreatePipelineNativeArtifactCache() {
  return std::make_shared<PipelineNativeArtifactCache>();
}

PipelineNativeArtifactCacheStats
GetPipelineNativeArtifactCacheStats(PipelineNativeArtifactCache *cache) {
  return cache ? cache->Stats() : PipelineNativeArtifactCacheStats{};
}

namespace {

std::atomic<uint64_t> g_test_air_initialization_occurrence = 0;
std::atomic<uint64_t> g_test_metal_compute_pipeline_occurrence = 0;
std::atomic<uint64_t> g_test_pso_archive_lookup_occurrence = 0;

static bool
ShouldInjectPipelineFault(const char *environment_name,
                          std::atomic<uint64_t> &occurrence) {
  const auto value = env::getEnvVar(environment_name);
  if (value.empty())
    return false;
  if (value == "always" || value == "all") {
    occurrence.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  char *end = nullptr;
  const auto target = std::strtoull(value.c_str(), &end, 0);
  if (end == value.c_str() || *end || !target)
    return false;
  return occurrence.fetch_add(1, std::memory_order_relaxed) + 1 == target;
}

static void
RecordInjectedPipelineFault(const char *name) {
  const auto path = env::getEnvVar("DXMT_TEST_FAULT_MARKER");
  if (path.empty())
    return;
  FILE *marker = fopen(path.c_str(), "a");
  if (!marker)
    return;
  fprintf(marker, "%s\n", name);
  fclose(marker);
}

static bool
D3D12PipelineDiagEnabledEnv(const char *name) {
  auto value = env::getEnvVar(name);
  return value == "1" || value == "true" || value == "yes" || value == "trace";
}

static bool
D3D12PipelineDiagEnabled() {
  static const bool enabled =
      D3D12PipelineDiagEnabledEnv("DXMT_DIAG_PIPELINE") ||
      D3D12PipelineDiagEnabledEnv("DXMT_DIAG_D3D12_PIPELINE") ||
      D3D12PipelineDiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE");
  return enabled;
}

static bool
D3D12MetalPsoLabelsEnabled() {
  static const bool enabled =
      D3D12PipelineDiagEnabledEnv("DXMT_DIAG_METAL_PSO_LABELS") ||
      D3D12PipelineDiagEnabledEnv("DXMT_DIAG_ROOT_CAUSE_DENSE") ||
      D3D12PipelineDiagEnabledEnv("DXMT_DIAG_GPU_HANG_DENSE");
  return enabled;
}

template <typename PipelineInfo>
void SetMetalPsoDebugLabel(PipelineInfo &info, std::string_view kind,
                           std::string_view shader_cache_key) {
  if (!D3D12MetalPsoLabelsEnabled())
    return;
  const auto label = dxmt::BuildMetalPsoDebugLabel(
      kind, shader_cache_key, sizeof(info.debug_label));
  std::memcpy(info.debug_label, label.c_str(), label.size() + 1);
}

static bool
D3D12PipelineDiagShouldLog() {
  static std::atomic<uint32_t> count = 0;
  if (!D3D12PipelineDiagEnabled())
    return false;
  const auto occurrence = count.fetch_add(1, std::memory_order_relaxed) + 1;
  return occurrence <= 32 || (occurrence & (occurrence - 1)) == 0;
}

static bool
D3D12AttachmentlessPsoSampleCountWarnShouldLog() {
  static std::atomic<uint64_t> count = 0;
  const auto occurrence = count.fetch_add(1, std::memory_order_relaxed) + 1;
  return occurrence == 1 || (occurrence & (occurrence - 1)) == 0;
}

static bool
BindlessUsesSamplerMirror(const PipelineDxilShader &shader) {
  const auto *arguments = shader.resourceArgumentInfo();
  if (!arguments)
    return false;

  for (uint32_t i = 0; i < shader.reflection().NumArguments; i++) {
    if (arguments[i].Type == SM50BindingType::Sampler)
      return true;
  }

  return false;
}

static uint64_t
AirconvHandleValue(sm50_ptr64_t handle) {
#if defined(__LP64__) || defined(_WIN64)
  return reinterpret_cast<uintptr_t>(handle);
#else
  return static_cast<uint64_t>(handle);
#endif
}

static const void *
AirconvLocalPointer(sm50_ptr64_t handle) {
#if defined(__LP64__) || defined(_WIN64)
  return handle;
#else
  return nullptr;
#endif
}

static std::string
D3D12PipelineDumpDirectory() {
  std::string path = env::getEnvVar("DXMT_DUMP_PATH");
  if (path.empty())
    path = env::getEnvVar("DXMT_LOG_PATH");
  if (path.empty() || path == "none")
    path = ".";
  env::createDirectory(path);
  if (!path.empty() && path.back() != '/' && path.back() != '\\')
    path += '/';
  return path;
}

constexpr uint32_t kShaderKindPixel = 0;
constexpr uint32_t kShaderKindVertex = 1;
constexpr uint32_t kShaderKindGeometry = 2;
constexpr uint32_t kShaderKindHull = 3;
constexpr uint32_t kShaderKindDomain = 4;
constexpr uint32_t kShaderKindCompute = 5;

constexpr uint32_t kBindlessBufferTableBindIndex = 27;
constexpr uint32_t kBindlessRootOffsetBindIndex = 28;
constexpr uint32_t kBindlessSamplerMirrorBindIndex = 29;
constexpr uint32_t kBindlessTextureMirrorBindIndex = 30;

static uint32_t
BufferBindingBit(uint32_t index) {
  return 1u << index;
}

static bool
BindlessUsesTextureMirror(const PipelineDxilShader &shader) {
  const auto *arguments = shader.resourceArgumentInfo();
  if (!arguments)
    return false;

  for (uint32_t i = 0; i < shader.reflection().NumArguments; i++) {
    if ((arguments[i].Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE) &&
        !(arguments[i].Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER))
      return true;
  }

  return false;
}

static bool
BindlessUsesBufferTable(const PipelineDxilShader &shader) {
  if (shader.reflection().NumConstantBuffers)
    return true;

  const auto *arguments = shader.resourceArgumentInfo();
  if (!arguments)
    return false;

  for (uint32_t i = 0; i < shader.reflection().NumArguments; i++) {
    if (arguments[i].Flags & (MTL_SM50_SHADER_ARGUMENT_BUFFER |
                              MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER))
      return true;
  }

  return false;
}

static uint32_t
BindlessImmutableBufferMask(
    const PipelineDxilShader &shader,
    uint32_t buffer_table_index = kBindlessBufferTableBindIndex,
    uint32_t root_offset_index = kBindlessRootOffsetBindIndex,
    uint32_t sampler_mirror_index = kBindlessSamplerMirrorBindIndex,
    uint32_t texture_mirror_index = kBindlessTextureMirrorBindIndex) {
  uint32_t mask = BufferBindingBit(root_offset_index);
  if (BindlessUsesBufferTable(shader))
    mask |= BufferBindingBit(buffer_table_index);
  if (BindlessUsesSamplerMirror(shader))
    mask |= BufferBindingBit(sampler_mirror_index);
  if (BindlessUsesTextureMirror(shader))
    mask |= BufferBindingBit(texture_mirror_index);
  return mask;
}

static uint32_t
NativeDescriptorImmutableBufferMask(const PipelineDxilShader &shader) {
  uint32_t mask = 0;
  if (shader.reflection().NumConstantBuffers)
    mask |= BufferBindingBit(DXMT12_MTL4_NATIVE_NULL_CBUFFER_BIND_INDEX) |
            BufferBindingBit(
                DXMT12_MTL4_NATIVE_CBUFFER_ROOT_TABLE_BASE_BIND_INDEX) |
            BufferBindingBit(
                DXMT12_MTL4_NATIVE_BUFFER_RESOURCE_TABLE_BIND_INDEX) |
            BufferBindingBit(
                DXMT12_MTL4_NATIVE_BUFFER_DESCRIPTOR_RECORD_BIND_INDEX) |
            BufferBindingBit(DXMT12_MTL4_NATIVE_DESCRIPTOR_HEAP_BIND_INDEX);

  const auto *arguments = shader.resourceArgumentInfo();
  if (!arguments)
    return mask;

  for (uint32_t i = 0; i < shader.reflection().NumArguments; i++) {
    const auto &arg = arguments[i];
    mask |= BufferBindingBit(DXMT12_MTL4_NATIVE_ROOT_TABLE_BASE_BIND_INDEX);
    if (arg.Type == SM50BindingType::Sampler) {
      mask |= BufferBindingBit(DXMT12_MTL4_NATIVE_SAMPLER_HEAP_BIND_INDEX);
    } else {
      mask |= BufferBindingBit(DXMT12_MTL4_NATIVE_DESCRIPTOR_HEAP_BIND_INDEX);
      if (arg.Flags & (MTL_SM50_SHADER_ARGUMENT_BUFFER |
                       MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER)) {
        mask |= BufferBindingBit(
                    DXMT12_MTL4_NATIVE_BUFFER_RESOURCE_TABLE_BIND_INDEX) |
                BufferBindingBit(
                    DXMT12_MTL4_NATIVE_BUFFER_DESCRIPTOR_RECORD_BIND_INDEX);
        if (arg.Type == SM50BindingType::SRV &&
            (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER))
          mask |= BufferBindingBit(
              DXMT12_MTL4_NATIVE_NULL_BUFFER_BIND_INDEX);
      }
    }
  }

  return mask;
}

constexpr WMTCompareFunction kCompareFunctionMap[] = {
    WMTCompareFunctionNever, WMTCompareFunctionNever, WMTCompareFunctionLess,
    WMTCompareFunctionEqual, WMTCompareFunctionLessEqual,
    WMTCompareFunctionGreater, WMTCompareFunctionNotEqual,
    WMTCompareFunctionGreaterEqual, WMTCompareFunctionAlways};

constexpr WMTStencilOperation kStencilOperationMap[] = {
    WMTStencilOperationZero,
    WMTStencilOperationKeep,
    WMTStencilOperationZero,
    WMTStencilOperationReplace,
    WMTStencilOperationIncrementClamp,
    WMTStencilOperationDecrementClamp,
    WMTStencilOperationInvert,
    WMTStencilOperationIncrementWrap,
    WMTStencilOperationDecrementWrap,
};

constexpr WMTBlendOperation kBlendOpMap[] = {
    WMTBlendOperationAdd,
    WMTBlendOperationAdd,
    WMTBlendOperationSubtract,
    WMTBlendOperationReverseSubtract,
    WMTBlendOperationMin,
    WMTBlendOperationMax,
};

constexpr WMTBlendFactor kBlendFactorMap[] = {
    WMTBlendFactorZero,
    WMTBlendFactorZero,
    WMTBlendFactorOne,
    WMTBlendFactorSourceColor,
    WMTBlendFactorOneMinusSourceColor,
    WMTBlendFactorSourceAlpha,
    WMTBlendFactorOneMinusSourceAlpha,
    WMTBlendFactorDestinationAlpha,
    WMTBlendFactorOneMinusDestinationAlpha,
    WMTBlendFactorDestinationColor,
    WMTBlendFactorOneMinusDestinationColor,
    WMTBlendFactorSourceAlphaSaturated,
    WMTBlendFactorZero,
    WMTBlendFactorZero,
    WMTBlendFactorBlendColor,
    WMTBlendFactorOneMinusBlendColor,
    WMTBlendFactorSource1Color,
    WMTBlendFactorOneMinusSource1Color,
    WMTBlendFactorSource1Alpha,
    WMTBlendFactorOneMinusSource1Alpha,
};

constexpr WMTBlendFactor kBlendAlphaFactorMap[] = {
    WMTBlendFactorZero,
    WMTBlendFactorZero,
    WMTBlendFactorOne,
    WMTBlendFactorSourceColor,
    WMTBlendFactorOneMinusSourceColor,
    WMTBlendFactorSourceAlpha,
    WMTBlendFactorOneMinusSourceAlpha,
    WMTBlendFactorDestinationAlpha,
    WMTBlendFactorOneMinusDestinationAlpha,
    WMTBlendFactorDestinationColor,
    WMTBlendFactorOneMinusDestinationColor,
    WMTBlendFactorSourceAlphaSaturated,
    WMTBlendFactorZero,
    WMTBlendFactorZero,
    WMTBlendFactorBlendAlpha,
    WMTBlendFactorOneMinusBlendAlpha,
    WMTBlendFactorSource1Color,
    WMTBlendFactorOneMinusSource1Color,
    WMTBlendFactorSource1Alpha,
    WMTBlendFactorOneMinusSource1Alpha,
};

constexpr WMTColorWriteMask kColorWriteMaskMap[] = {
    WMTColorWriteMaskNone,
    WMTColorWriteMaskRed,
    WMTColorWriteMaskGreen,
    WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
    WMTColorWriteMaskBlue,
    WMTColorWriteMaskBlue | WMTColorWriteMaskRed,
    WMTColorWriteMaskBlue | WMTColorWriteMaskGreen,
    WMTColorWriteMaskBlue | WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
    WMTColorWriteMaskAlpha,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskRed,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskGreen,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue | WMTColorWriteMaskRed,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue | WMTColorWriteMaskGreen,
    WMTColorWriteMaskAll,
};

constexpr WMTLogicOperation kLogicOpMap[] = {
    WMTLogicOperationClear,      WMTLogicOperationSet,
    WMTLogicOperationCopy,       WMTLogicOperationCopyInverted,
    WMTLogicOperationNoOp,       WMTLogicOperationInvert,
    WMTLogicOperationAnd,        WMTLogicOperationNand,
    WMTLogicOperationOr,         WMTLogicOperationNor,
    WMTLogicOperationXor,        WMTLogicOperationEquiv,
    WMTLogicOperationAndReverse, WMTLogicOperationAndInverted,
    WMTLogicOperationOrReverse,  WMTLogicOperationOrInverted,
};

WMTPixelFormat
GetRenderTargetPixelFormat(WMT::Device device, DXGI_FORMAT format) {
  MTL_DXGI_FORMAT_DESC desc = {};
  if (FAILED(MTLQueryDXGIFormat(device, format, desc)))
    return WMTPixelFormatInvalid;
  return desc.PixelFormat;
}

class BlobImpl final : public ComObjectWithInitialRef<ID3DBlob> {
public:
  explicit BlobImpl(std::vector<std::byte> &&data) : data_(std::move(data)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D10Blob)) {
      *object = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  void *STDMETHODCALLTYPE GetBufferPointer() override {
    return data_.empty() ? nullptr : data_.data();
  }

  SIZE_T STDMETHODCALLTYPE GetBufferSize() override {
    return data_.size();
  }

private:
  std::vector<std::byte> data_;
};

Com<ID3DBlob>
CreateBlob(std::span<const uint8_t> data) {
  std::vector<std::byte> bytes;
  bytes.resize(data.size());
  if (!data.empty())
    std::memcpy(bytes.data(), data.data(), data.size());
  return Com<ID3DBlob>::transfer(new BlobImpl(std::move(bytes)));
}

const char *
ShaderStageName(PipelineShaderStage stage) {
  switch (stage) {
  case PipelineShaderStage::Vertex:
    return "VS";
  case PipelineShaderStage::Pixel:
    return "PS";
  case PipelineShaderStage::Geometry:
    return "GS";
  case PipelineShaderStage::Hull:
    return "HS";
  case PipelineShaderStage::Domain:
    return "DS";
  case PipelineShaderStage::Compute:
    return "CS";
  }
  return "unknown";
}

uint32_t
ExpectedShaderKind(PipelineShaderStage stage) {
  switch (stage) {
  case PipelineShaderStage::Vertex:
    return kShaderKindVertex;
  case PipelineShaderStage::Pixel:
    return kShaderKindPixel;
  case PipelineShaderStage::Geometry:
    return kShaderKindGeometry;
  case PipelineShaderStage::Hull:
    return kShaderKindHull;
  case PipelineShaderStage::Domain:
    return kShaderKindDomain;
  case PipelineShaderStage::Compute:
    return kShaderKindCompute;
  }
  return UINT32_MAX;
}

bool
HasBytecode(const D3D12_SHADER_BYTECODE &bytecode) {
  return bytecode.pShaderBytecode && bytecode.BytecodeLength;
}

bool
IsDxilContainer(const D3D12_SHADER_BYTECODE &bytecode) {
  dxil::ContainerInfo container = {};
  const auto status = dxil::ParseContainer(
      bytecode.pShaderBytecode, bytecode.BytecodeLength, container);
  return status == dxil::ParseStatus::Ok &&
         container.hasPart(dxil::fourcc::Dxil);
}

constexpr uint32_t
FourCC(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) |
         (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}

uint32_t
ReadLe32(const uint8_t *data) {
  return uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
         (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

bool
FindDxbcShaderBlob(const void *data, size_t size,
                   const uint8_t **blob, uint32_t *blob_size) {
  if (!data || size < 32)
    return false;

  const auto *bytes = static_cast<const uint8_t *>(data);
  if (ReadLe32(bytes) != FourCC('D', 'X', 'B', 'C'))
    return false;

  const auto container_size = ReadLe32(bytes + 24);
  const auto blob_count = ReadLe32(bytes + 28);
  // The blob-table budget below is `container_size - 32` in uint32 arithmetic.
  // Without the lower bound, a container claiming fewer than 32 bytes wraps it
  // to ~1G entries, so the table walk reads far past the caller's buffer. A
  // well-formed DXBC container always covers at least its own 32-byte header.
  if (container_size < 32 || container_size > size ||
      blob_count > (container_size - 32) / 4)
    return false;

  for (uint32_t i = 0; i < blob_count; i++) {
    const auto offset = ReadLe32(bytes + 32 + static_cast<size_t>(i) * 4);
    if (offset > container_size || container_size - offset < 8)
      return false;

    const auto fourcc = ReadLe32(bytes + offset);
    const auto length = ReadLe32(bytes + offset + 4);
    if (length > container_size - offset - 8)
      return false;

    if (fourcc == FourCC('S', 'H', 'D', 'R') ||
        fourcc == FourCC('S', 'H', 'E', 'X')) {
      *blob = bytes + offset + 8;
      *blob_size = length;
      return true;
    }
  }

  return false;
}

uint32_t
ExpectedDxbcShaderType(PipelineShaderStage stage) {
  switch (stage) {
  case PipelineShaderStage::Pixel:
    return 0;
  case PipelineShaderStage::Vertex:
    return 1;
  case PipelineShaderStage::Geometry:
    return 2;
  case PipelineShaderStage::Hull:
    return 3;
  case PipelineShaderStage::Domain:
    return 4;
  case PipelineShaderStage::Compute:
    return 5;
  }
  return UINT32_MAX;
}

bool
IsReservedDxbcOpcode(uint32_t opcode) {
  using namespace microsoft;
  return opcode == D3D10_SB_OPCODE_RESERVED0 ||
         opcode == D3D10_1_SB_OPCODE_RESERVED1 ||
         opcode == D3D11_SB_OPCODE_RESERVED0 ||
         opcode == D3D11_1_SB_OPCODE_RESERVED0 ||
         opcode == D3DWDDM1_3_SB_OPCODE_RESERVED0;
}

bool
IsGeometryOnlyDxbcOpcode(uint32_t opcode) {
  using namespace microsoft;
  switch (opcode) {
  case D3D10_SB_OPCODE_CUT:
  case D3D10_SB_OPCODE_EMIT:
  case D3D10_SB_OPCODE_EMITTHENCUT:
  case D3D10_SB_OPCODE_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY:
  case D3D10_SB_OPCODE_DCL_GS_INPUT_PRIMITIVE:
  case D3D10_SB_OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT:
  case D3D11_SB_OPCODE_EMIT_STREAM:
  case D3D11_SB_OPCODE_CUT_STREAM:
  case D3D11_SB_OPCODE_EMITTHENCUT_STREAM:
  case D3D11_SB_OPCODE_DCL_STREAM:
  case D3D11_SB_OPCODE_DCL_GS_INSTANCE_COUNT:
    return true;
  default:
    return false;
  }
}

bool
HasExactDxbcInstructionLength(uint32_t opcode, uint32_t length) {
  using namespace microsoft;
  switch (opcode) {
  case D3D10_SB_OPCODE_BREAK:
  case D3D10_SB_OPCODE_CONTINUE:
  case D3D10_SB_OPCODE_CUT:
  case D3D10_SB_OPCODE_DEFAULT:
  case D3D10_SB_OPCODE_ELSE:
  case D3D10_SB_OPCODE_EMIT:
  case D3D10_SB_OPCODE_EMITTHENCUT:
  case D3D10_SB_OPCODE_ENDIF:
  case D3D10_SB_OPCODE_ENDLOOP:
  case D3D10_SB_OPCODE_ENDSWITCH:
  case D3D10_SB_OPCODE_LOOP:
  case D3D10_SB_OPCODE_RET:
    return length == 1;
  case D3D11_SB_OPCODE_DCL_THREAD_GROUP:
    return length == 4;
  default:
    return true;
  }
}

bool
ValidateDxbcInstructionStream(PipelineShaderStage stage,
                              const uint8_t *shader_blob,
                              uint32_t shader_blob_size) {
  using namespace microsoft;
  if (!shader_blob || shader_blob_size < 3 * sizeof(uint32_t) ||
      shader_blob_size % sizeof(uint32_t))
    return false;

  const auto version = ReadLe32(shader_blob);
  const auto major = (version >> 4) & 0xf;
  const auto minor = version & 0xf;
  if (!((major == 4 && minor <= 1) || (major == 5 && minor <= 1)))
    return false;

  const auto token_count = ReadLe32(shader_blob + sizeof(uint32_t));
  if (token_count < 3 || token_count != shader_blob_size / sizeof(uint32_t))
    return false;

  enum class FlowKind : uint8_t { IfThen, IfElse, Loop, Switch };
  std::vector<FlowKind> flow;
  constexpr size_t kMaximumFlowDepth = 64;
  for (uint32_t index = 2; index < token_count;) {
    const auto token = ReadLe32(shader_blob + index * sizeof(uint32_t));
    const auto opcode = token & D3D10_SB_OPCODE_TYPE_MASK;
    if (opcode >= D3D10_SB_NUM_OPCODES || IsReservedDxbcOpcode(opcode) ||
        (IsGeometryOnlyDxbcOpcode(opcode) &&
         stage != PipelineShaderStage::Geometry))
      return false;

    uint32_t length =
        (token & D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH_MASK) >>
        D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH_SHIFT;
    if (opcode == D3D10_SB_OPCODE_CUSTOMDATA) {
      if (index + 1 >= token_count)
        return false;
      length = ReadLe32(shader_blob + (index + 1) * sizeof(uint32_t));
      if (length < 2)
        return false;
    }
    if (!length || length > token_count - index ||
        !HasExactDxbcInstructionLength(opcode, length))
      return false;

    switch (opcode) {
    case D3D10_SB_OPCODE_IF:
      flow.push_back(FlowKind::IfThen);
      break;
    case D3D10_SB_OPCODE_ELSE:
      if (flow.empty() || flow.back() != FlowKind::IfThen)
        return false;
      flow.back() = FlowKind::IfElse;
      break;
    case D3D10_SB_OPCODE_ENDIF:
      if (flow.empty() ||
          (flow.back() != FlowKind::IfThen &&
           flow.back() != FlowKind::IfElse))
        return false;
      flow.pop_back();
      break;
    case D3D10_SB_OPCODE_LOOP:
      flow.push_back(FlowKind::Loop);
      break;
    case D3D10_SB_OPCODE_ENDLOOP:
      if (flow.empty() || flow.back() != FlowKind::Loop)
        return false;
      flow.pop_back();
      break;
    case D3D10_SB_OPCODE_SWITCH:
      flow.push_back(FlowKind::Switch);
      break;
    case D3D10_SB_OPCODE_CASE:
    case D3D10_SB_OPCODE_DEFAULT:
      if (flow.empty() || flow.back() != FlowKind::Switch)
        return false;
      break;
    case D3D10_SB_OPCODE_ENDSWITCH:
      if (flow.empty() || flow.back() != FlowKind::Switch)
        return false;
      flow.pop_back();
      break;
    default:
      break;
    }
    if (flow.size() > kMaximumFlowDepth)
      return false;
    index += length;
  }
  return flow.empty();
}

bool
ValidateDxbcShaderStage(PipelineShaderStage stage,
                        const D3D12_SHADER_BYTECODE &bytecode) {
  const uint8_t *shader_blob = nullptr;
  uint32_t shader_blob_size = 0;
  if (!FindDxbcShaderBlob(bytecode.pShaderBytecode, bytecode.BytecodeLength,
                          &shader_blob, &shader_blob_size) ||
      shader_blob_size < 4)
    return false;

  const auto token = ReadLe32(shader_blob);
  const auto actual_type = token >> 16;
  const auto expected_type = ExpectedDxbcShaderType(stage);
  return (expected_type == UINT32_MAX || actual_type == expected_type) &&
         ValidateDxbcInstructionStream(stage, shader_blob, shader_blob_size);
}

bool
ValidateDirectVertexPixelLinkage(const D3D12_SHADER_BYTECODE &vertex,
                                 const D3D12_SHADER_BYTECODE &pixel) {
  if (IsDxilContainer(vertex) || IsDxilContainer(pixel))
    return true;
  if (!ValidateDxbcShaderStage(PipelineShaderStage::Vertex, vertex) ||
      !ValidateDxbcShaderStage(PipelineShaderStage::Pixel, pixel))
    return true;

  microsoft::CSignatureParser vertex_output;
  microsoft::CSignatureParser pixel_input;
  if (FAILED(microsoft::DXBCGetOutputSignature(
          vertex.pShaderBytecode, &vertex_output)) ||
      FAILED(microsoft::DXBCGetInputSignature(pixel.pShaderBytecode,
                                              &pixel_input)))
    return true;
  const microsoft::D3D11_SIGNATURE_PARAMETER *vertex_parameters = nullptr;
  const microsoft::D3D11_SIGNATURE_PARAMETER *pixel_parameters = nullptr;
  const UINT vertex_count = vertex_output.GetParameters(&vertex_parameters);
  const UINT pixel_count = pixel_input.GetParameters(&pixel_parameters);
  for (UINT i = 0; i < pixel_count; ++i) {
    const auto &consumer = pixel_parameters[i];
    if (consumer.SystemValue != microsoft::D3D10_SB_NAME_UNDEFINED)
      continue;
    bool matched = false;
    for (UINT j = 0; j < vertex_count; ++j) {
      const auto &producer = vertex_parameters[j];
      if (_stricmp(producer.SemanticName, consumer.SemanticName) == 0 &&
          producer.SemanticIndex == consumer.SemanticIndex &&
          producer.Register == consumer.Register &&
          producer.SystemValue == consumer.SystemValue &&
          producer.ComponentType == consumer.ComponentType &&
          (producer.Mask & consumer.Mask) == consumer.Mask) {
        matched = true;
        break;
      }
    }
    if (!matched)
      return false;
  }
  return true;
}

D3D12_ROOT_SIGNATURE_FLAGS
GetRootSignatureFlags(ID3D12RootSignature *root_signature) {
  auto *rs = GetDXMTRootSignature(root_signature);
  if (!rs)
    return D3D12_ROOT_SIGNATURE_FLAG_NONE;

  const auto &desc = rs->GetVersionedDesc();
  return desc.Version == D3D_ROOT_SIGNATURE_VERSION_1_0
             ? desc.Desc_1_0.Flags
             : desc.Desc_1_1.Flags;
}

std::string
PipelineShaderCacheKey(PipelineShaderStage stage,
                       const D3D12_SHADER_BYTECODE &bytecode) {
  std::string key = std::to_string(uint32_t(stage));
  key += ':';
  key += Sha1HashState::compute(
      static_cast<const uint8_t *>(bytecode.pShaderBytecode),
      bytecode.BytecodeLength).string();
  return key;
}

std::mutex &
PipelineShaderCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, std::shared_ptr<PipelineCachedShader>> &
PipelineShaderCache() {
  static std::unordered_map<std::string, std::shared_ptr<PipelineCachedShader>>
      cache;
  return cache;
}

HRESULT
InitializePipelineShaderHandle(PipelineShaderStage stage,
                               PipelineCachedShader &cached_shader,
                               dxmt12_airconv_shader_t *shader,
                               DXMT12_MTL4_SHADER_REFLECTION *reflection) {
  if (shader)
    *shader = nullptr;
  if (ShouldInjectPipelineFault("DXMT_TEST_FAIL_AIR_INITIALIZATION_AT",
                                g_test_air_initialization_occurrence)) {
    RecordInjectedPipelineFault("DXMT_TEST_FAIL_AIR_INITIALIZATION_AT");
    WARN("D3D12PipelineState: injected AIR initialization failure for ",
         ShaderStageName(stage));
    return E_OUTOFMEMORY;
  }

  sm50_error_t error = nullptr;
  const bool is_dxil = cached_shader.kind == PipelineShaderBytecodeKind::Dxil;
  const int initialize_failed =
      is_dxil ? DXMT12DXILInitialize(cached_shader.bytecode.data(),
                                     cached_shader.bytecode.size(), shader,
                                     reflection, &error)
              : DXMT12SM50Initialize(cached_shader.bytecode.data(),
                                     cached_shader.bytecode.size(), shader,
                                     reflection, &error);
  if (initialize_failed) {
    WARN("D3D12PipelineState: failed to initialize ", ShaderStageName(stage),
         is_dxil ? " DXIL shader: " : " DXBC shader: ",
         DXMT12SM50GetErrorMessageString(error));
    DXMT12SM50FreeError(error);
    return WARN_E_INVALIDARG(__func__);
  }

  return S_OK;
}

HRESULT
ParsePipelineShader(PipelineShaderStage stage,
                    IMTLD3D12Device *device,
                    const D3D12_SHADER_BYTECODE &bytecode,
                    PipelineDxilShader &shader) {
  if (!bytecode.pShaderBytecode && bytecode.BytecodeLength)
    return WARN_E_INVALIDARG(__func__);
  if (!HasBytecode(bytecode))
    return S_FALSE;

  shader = {};
  shader.stage = stage;
  const auto cache_key = PipelineShaderCacheKey(stage, bytecode);
  {
    std::lock_guard lock(PipelineShaderCacheMutex());
    auto &cache = PipelineShaderCache();
    auto cached = cache.find(cache_key);
    if (cached != cache.end()) {
      shader.cached_shader = cached->second;
      DXMT12_MTL4_SHADER_REFLECTION reflection = {};
      return InitializePipelineShaderHandle(stage, *shader.cached_shader,
                                            &shader.shader, &reflection);
    }
  }

  auto cached_shader = std::make_shared<PipelineCachedShader>();
  cached_shader->bytecode.resize(bytecode.BytecodeLength);
  std::memcpy(cached_shader->bytecode.data(), bytecode.pShaderBytecode,
              bytecode.BytecodeLength);

  const bool is_dxil = IsDxilContainer(bytecode);
  cached_shader->kind = is_dxil ? PipelineShaderBytecodeKind::Dxil
                                : PipelineShaderBytecodeKind::Dxbc;

  const auto status = cached_shader->parser.parse(
      cached_shader->bytecode.data(), cached_shader->bytecode.size());
  if (is_dxil && status != dxil::ParseStatus::Ok) {
    WARN("D3D12PipelineState: failed to parse ", ShaderStageName(stage),
         " DXIL bytecode: ", dxil::StatusName(status));
    return WARN_E_INVALIDARG(__func__);
  }

  const auto &program = cached_shader->parser.dxilProgram();
  if (is_dxil) {
    const auto expected_kind = ExpectedShaderKind(stage);
    if (program && expected_kind != UINT32_MAX &&
        program->shader_kind() != expected_kind) {
      WARN("D3D12PipelineState: ", ShaderStageName(stage),
           " bytecode contains ", dxil::PsvShaderKindName(program->shader_kind()),
           " shader");
      return WARN_E_INVALIDARG(__func__);
    }
  } else if (!ValidateDxbcShaderStage(stage, bytecode)) {
    WARN("D3D12PipelineState: invalid or mismatched ",
         ShaderStageName(stage), " DXBC bytecode");
    return WARN_E_INVALIDARG(__func__);
  }

  shader.cached_shader = cached_shader;
  auto hr = InitializePipelineShaderHandle(stage, *cached_shader,
                                           &shader.shader,
                                           &cached_shader->reflection);
  if (FAILED(hr))
    return hr;

  const auto argument_count =
      cached_shader->reflection.NumConstantBuffers +
      cached_shader->reflection.NumArguments;
  cached_shader->argument_info.resize(argument_count);
  if (argument_count) {
    auto *constant_buffers =
        cached_shader->argument_info.empty()
            ? nullptr
            : cached_shader->argument_info.data();
    auto *arguments =
        cached_shader->argument_info.size() <=
                cached_shader->reflection.NumConstantBuffers
            ? nullptr
            : cached_shader->argument_info.data() +
                  cached_shader->reflection.NumConstantBuffers;
    if (is_dxil)
      DXMT12DXILGetArgumentsInfo(shader.shader, constant_buffers,
                                 arguments);
    else
      DXMT12SM50GetArgumentsInfo(shader.shader, constant_buffers,
                                 arguments);
  }

  {
    std::lock_guard lock(PipelineShaderCacheMutex());
    auto &cache = PipelineShaderCache();
    auto [it, inserted] = cache.emplace(cache_key, cached_shader);
    shader.cached_shader = inserted ? cached_shader : it->second;
  }

  return S_OK;
}

HRESULT
AppendPipelineShader(PipelineShaderStage stage,
                     IMTLD3D12Device *device,
                     const D3D12_SHADER_BYTECODE &bytecode,
                     std::vector<PipelineDxilShader> &shaders) {
  PipelineDxilShader shader = {};
  const auto hr = ParsePipelineShader(stage, device, bytecode, shader);
  if (hr == S_FALSE)
    return S_OK;
  if (FAILED(hr))
    return hr;

  shaders.push_back(std::move(shader));
  return S_OK;
}

const PipelineDxilShader *
FindShader(const std::vector<PipelineDxilShader> &shaders,
           PipelineShaderStage stage, uint32_t *index = nullptr) {
  for (uint32_t i = 0; i < shaders.size(); i++) {
    if (shaders[i].stage == stage) {
      if (index)
        *index = i;
      return &shaders[i];
    }
  }
  return nullptr;
}

D3D12_SHADER_BYTECODE
ShaderBytecodeView(const std::vector<PipelineDxilShader> &shaders,
                   PipelineShaderStage stage) {
  const auto *shader = FindShader(shaders, stage);
  if (!shader)
    return {};

  return {
      .pShaderBytecode = shader->bytecode().empty() ? nullptr
                                                  : shader->bytecode().data(),
      .BytecodeLength = shader->bytecode().size(),
  };
}

bool
IsSystemGeneratedValue(const dxil::DxilTranslationSignatureElementInfo &sig) {
  return sig.semantic_kind != 0 || sig.semantic_key == "SV_POSITION0" ||
         sig.semantic_key == "SV_TARGET0" ||
         sig.semantic_key == "SV_DEPTH0" ||
         sig.semantic_key == "SV_COVERAGE0";
}

bool
AppendSignatureLinks(const std::vector<PipelineDxilShader> &shaders,
                     PipelineShaderStage producer_stage,
                     PipelineShaderStage consumer_stage,
                     std::vector<PipelineSignatureLink> &links) {
  uint32_t producer_shader_index = 0;
  uint32_t consumer_shader_index = 0;
  const auto *producer =
      FindShader(shaders, producer_stage, &producer_shader_index);
  const auto *consumer =
      FindShader(shaders, consumer_stage, &consumer_shader_index);
  if (!producer || !consumer)
    return true;

  const auto *producer_info = producer->translation();
  const auto *consumer_info = consumer->translation();
  if (!producer_info || !consumer_info)
    return true;

  size_t producer_search_start = 0;
  for (uint32_t consumer_sig_index = 0;
       consumer_sig_index < consumer_info->signatures.size();
       consumer_sig_index++) {
    const auto &consumer_sig = consumer_info->signatures[consumer_sig_index];
    if (consumer_sig.kind != dxil::DxilTranslationSignatureKind::Input ||
        IsSystemGeneratedValue(consumer_sig))
      continue;

    auto match = std::find_if(
        producer_info->signatures.begin() + producer_search_start,
        producer_info->signatures.end(),
        [&](const dxil::DxilTranslationSignatureElementInfo &producer_sig) {
          return producer_sig.kind ==
                     dxil::DxilTranslationSignatureKind::Output &&
                 producer_sig.output_stream == consumer_sig.output_stream &&
                 producer_sig.semantic_key == consumer_sig.semantic_key &&
                 producer_sig.component_type == consumer_sig.component_type &&
                 (producer_sig.component_mask & consumer_sig.component_mask) ==
                     consumer_sig.component_mask;
        });
    if (match == producer_info->signatures.end()) {
      WARN("D3D12PipelineState: unmatched ", ShaderStageName(consumer_stage),
           " input signature ", consumer_sig.semantic_key,
           " type=", uint32_t(consumer_sig.component_type),
           " mask=0x", std::hex, uint32_t(consumer_sig.component_mask),
           std::dec);
      return false;
    }

    producer_search_start =
        size_t(match - producer_info->signatures.begin()) + 1;
    links.push_back({
        .producer_shader_index = producer_shader_index,
        .producer_signature_index =
            uint32_t(match - producer_info->signatures.begin()),
        .consumer_shader_index = consumer_shader_index,
        .consumer_signature_index = consumer_sig_index,
        .semantic_key = consumer_sig.semantic_key,
        .producer_component_mask = match->component_mask,
        .consumer_component_mask = consumer_sig.component_mask,
    });
  }

  return true;
}

bool
BuildSignatureLinks(const std::vector<PipelineDxilShader> &shaders,
                    std::vector<PipelineSignatureLink> &links) {
  links.clear();

  if (FindShader(shaders, PipelineShaderStage::Hull) &&
      !AppendSignatureLinks(shaders, PipelineShaderStage::Vertex,
                            PipelineShaderStage::Hull, links))
    return false;
  if (FindShader(shaders, PipelineShaderStage::Domain) &&
      !AppendSignatureLinks(shaders, PipelineShaderStage::Hull,
                            PipelineShaderStage::Domain, links))
    return false;

  const auto raster_producer = FindShader(shaders, PipelineShaderStage::Domain)
                                   ? PipelineShaderStage::Domain
                                   : PipelineShaderStage::Vertex;
  if (FindShader(shaders, PipelineShaderStage::Geometry)) {
    if (!AppendSignatureLinks(shaders, raster_producer,
                              PipelineShaderStage::Geometry, links) ||
        !AppendSignatureLinks(shaders, PipelineShaderStage::Geometry,
                              PipelineShaderStage::Pixel, links))
      return false;
  } else {
    if (!AppendSignatureLinks(shaders, raster_producer,
                              PipelineShaderStage::Pixel, links))
      return false;
  }

  return true;
}

void
DebugLogDxilShaderInfo(std::string_view shader_cache_key,
                       const PipelineDxilShader &shader) {
  if (!D3D12PipelineDiagShouldLog())
    return;

  const auto *info = shader.translation();
  INFO("D3D12 diagnostic: pipeline shader",
       " pso=", shader_cache_key.substr(0, std::min<size_t>(16, shader_cache_key.size())),
       " stage=", ShaderStageName(shader.stage),
       " bytecode=", uint64_t(shader.bytecode().size()),
       " cbv=", uint32_t(shader.reflection().NumConstantBuffers),
       " resources=", uint32_t(shader.reflection().NumArguments),
       " argumentQwords=", uint32_t(shader.reflection().ArgumentTableQwords),
       " signatures=", info ? uint32_t(info->signatures.size()) : 0u,
       " hasRootSignature=", info && info->has_root_signature);

  if (!info)
    return;

  for (uint32_t i = 0; i < info->signatures.size(); i++) {
    const auto &sig = info->signatures[i];
    INFO("D3D12 diagnostic: pipeline signature",
         " pso=", shader_cache_key.substr(0, std::min<size_t>(16, shader_cache_key.size())),
         " stage=", ShaderStageName(shader.stage),
         " index=", i,
         " kind=", uint32_t(sig.kind),
         " semantic=", sig.semantic_key,
         " semanticKind=", uint32_t(sig.semantic_kind),
         " semanticIndex=", uint32_t(sig.semantic_index),
         " elementId=", uint32_t(sig.element_id),
         " stream=", uint32_t(sig.output_stream),
         " rows=", uint32_t(sig.rows),
         " cols=", uint32_t(sig.cols),
         " startRow=", uint32_t(sig.start_row),
         " startCol=", uint32_t(sig.start_col),
         " mask=0x", std::hex, uint32_t(sig.component_mask), std::dec);
  }
}

void
DebugLogSignatureLinks(std::string_view shader_cache_key,
                       const std::vector<PipelineSignatureLink> &links) {
  if (!D3D12PipelineDiagShouldLog())
    return;

  INFO("D3D12 diagnostic: pipeline signature links",
       " pso=", shader_cache_key.substr(0, std::min<size_t>(16, shader_cache_key.size())),
       " count=", uint32_t(links.size()));
  for (uint32_t i = 0; i < links.size(); i++) {
    const auto &link = links[i];
    INFO("D3D12 diagnostic: pipeline signature link",
         " pso=", shader_cache_key.substr(0, std::min<size_t>(16, shader_cache_key.size())),
         " index=", i,
         " semantic=", link.semantic_key,
         " producerShader=", link.producer_shader_index,
         " producerSig=", link.producer_signature_index,
         " consumerShader=", link.consumer_shader_index,
         " consumerSig=", link.consumer_signature_index,
         " producerMask=0x", std::hex, uint32_t(link.producer_component_mask),
         " consumerMask=0x", uint32_t(link.consumer_component_mask),
         std::dec);
  }
}

Com<ID3D12RootSignature>
ResolveRootSignature(IMTLD3D12Device *device,
                     ID3D12RootSignature *explicit_root_signature,
                     const std::vector<PipelineDxilShader> &shaders) {
  if (explicit_root_signature)
    return explicit_root_signature;

  for (const auto &shader : shaders) {
    const auto *info = shader.translation();
    if (!info || !info->has_root_signature || info->root_signature.empty())
      continue;

    auto blob = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(info->root_signature.data()),
        info->root_signature.size());
    auto root_signature = CreateRootSignatureFromBlob(device, blob);
    if (root_signature)
      return root_signature;
  }

  return nullptr;
}

bool
CopyCachedBlob(D3D12_CACHED_PIPELINE_STATE cached,
               std::vector<uint8_t> &out) {
  out.clear();
  if (!cached.CachedBlobSizeInBytes)
    return true;
  if (!cached.pCachedBlob)
    return false;

  const auto *bytes = static_cast<const uint8_t *>(cached.pCachedBlob);
  out.assign(bytes, bytes + cached.CachedBlobSizeInBytes);
  return true;
}

void
HashBytes(Sha1HashState &hash, const void *data, size_t size) {
  if (size)
    hash.update(data, size);
}

template <typename T>
void
HashValue(Sha1HashState &hash, const T &value) {
  hash.update(value);
}

void
HashString(Sha1HashState &hash, std::string_view value) {
  const auto size = uint32_t(value.size());
  HashValue(hash, size);
  HashBytes(hash, value.data(), value.size());
}

template <typename T>
void
HashVector(Sha1HashState &hash, const std::vector<T> &values) {
  const auto size = uint32_t(values.size());
  HashValue(hash, size);
  HashBytes(hash, values.data(), values.size() * sizeof(T));
}

void
HashPipelineShaders(Sha1HashState &hash,
                const std::vector<PipelineDxilShader> &shaders) {
  const auto count = uint32_t(shaders.size());
  HashValue(hash, count);
  for (const auto &shader : shaders) {
    HashValue(hash, shader.stage);
    HashVector(hash, shader.bytecode());
  }
}

std::string
PipelineShaderDigest(const PipelineDxilShader &shader) {
  return Sha1HashState::compute(shader.bytecode().data(), shader.bytecode().size())
      .string();
}

bool
DiagForceFullscreenPositionForShader(const PipelineDxilShader &shader) {
  (void)shader;
  return false;
}

bool
D3D12PipelineFilterMatches(std::string filter, std::string_view value) {
  if (filter.empty())
    return false;
  if (filter.starts_with("0x") || filter.starts_with("0X"))
    filter = filter.substr(2);

  size_t start = 0;
  for (;;) {
    const size_t end = filter.find_first_of(",; ", start);
    const auto item =
        filter.substr(start, end == std::string::npos ? end : end - start);
    if (!item.empty() &&
        (value == item || value.substr(0, item.size()) == item))
      return true;
    if (end == std::string::npos)
      return false;
    start = end + 1;
  }
}

std::string
TrimFilterToken(std::string_view token) {
  const auto begin = token.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos)
    return {};
  const auto end = token.find_last_not_of(" \t\r\n");
  return std::string(token.substr(begin, end - begin + 1));
}

size_t
ShaderBytecodeSize(const std::vector<PipelineDxilShader> &shaders,
                   PipelineShaderStage stage) {
  if (auto *shader = FindShader(shaders, stage))
    return shader->bytecode().size();
  return 0;
}

std::string
D3D12DebugShaderName(const std::vector<PipelineDxilShader> &shaders,
                     PipelineShaderStage stage) {
  auto *shader = FindShader(shaders, stage);
  if (!shader)
    return "null";

  std::stringstream stream;
  stream << PipelineShaderDigest(*shader) << "/" << shader->bytecode().size();
  return stream.str();
}

std::string
D3D12DebugPipelineDesc(const char *kind, std::string_view shader_cache_key,
                       const std::vector<PipelineDxilShader> &shaders,
                       const PipelineGraphicsState *graphics_state,
                       const PipelineComputeState *compute_state) {
  std::stringstream stream;
  stream << kind << " key=0x" << shader_cache_key
         << " VS=" << D3D12DebugShaderName(shaders, PipelineShaderStage::Vertex)
         << " HS=" << D3D12DebugShaderName(shaders, PipelineShaderStage::Hull)
         << " DS=" << D3D12DebugShaderName(shaders, PipelineShaderStage::Domain)
         << " GS=" << D3D12DebugShaderName(shaders, PipelineShaderStage::Geometry)
         << " PS=" << D3D12DebugShaderName(shaders, PipelineShaderStage::Pixel)
         << " CS=" << D3D12DebugShaderName(shaders, PipelineShaderStage::Compute)
         << " VS.bytes=" << ShaderBytecodeSize(shaders, PipelineShaderStage::Vertex)
         << " PS.bytes=" << ShaderBytecodeSize(shaders, PipelineShaderStage::Pixel)
         << " CS.bytes=" << ShaderBytecodeSize(shaders, PipelineShaderStage::Compute);

  if (graphics_state) {
    const auto &desc = graphics_state->desc;
    const auto &blend =
        desc.BlendState.RenderTarget[desc.BlendState.IndependentBlendEnable ? 0 : 0];
    stream << " inputs=" << graphics_state->input_elements.size()
           << " so_entries=" << graphics_state->stream_output_entries.size()
           << " rt_count=" << desc.NumRenderTargets
           << " dsv=" << uint32_t(desc.DSVFormat)
           << " topology=" << uint32_t(desc.PrimitiveTopologyType)
           << " sample_count=" << desc.SampleDesc.Count
           << " sample_quality=" << desc.SampleDesc.Quality
           << " sample_mask=0x" << std::hex << desc.SampleMask << std::dec
           << " alpha_to_coverage=" << uint32_t(desc.BlendState.AlphaToCoverageEnable)
           << " independent_blend=" << uint32_t(desc.BlendState.IndependentBlendEnable)
           << " depth_enable=" << uint32_t(desc.DepthStencilState.DepthEnable)
           << " depth_write=" << uint32_t(desc.DepthStencilState.DepthWriteMask)
           << " depth_func=" << uint32_t(desc.DepthStencilState.DepthFunc)
           << " stencil_enable=" << uint32_t(desc.DepthStencilState.StencilEnable)
           << " fill=" << uint32_t(desc.RasterizerState.FillMode)
           << " cull=" << uint32_t(desc.RasterizerState.CullMode)
           << " front_ccw=" << uint32_t(desc.RasterizerState.FrontCounterClockwise)
           << " depth_clip=" << uint32_t(desc.RasterizerState.DepthClipEnable)
           << " blend0=" << uint32_t(blend.BlendEnable)
           << " src0=" << uint32_t(blend.SrcBlend)
           << " dst0=" << uint32_t(blend.DestBlend)
           << " op0=" << uint32_t(blend.BlendOp)
           << " src_alpha0=" << uint32_t(blend.SrcBlendAlpha)
           << " dst_alpha0=" << uint32_t(blend.DestBlendAlpha)
           << " op_alpha0=" << uint32_t(blend.BlendOpAlpha)
           << " write0=" << uint32_t(blend.RenderTargetWriteMask)
           << " rtv=[";
    for (UINT i = 0; i < desc.NumRenderTargets && i < 8; i++) {
      if (i)
        stream << ",";
      stream << uint32_t(desc.RTVFormats[i]);
    }
    stream << "]";
    for (UINT i = 0; i < desc.NumRenderTargets && i < 8; i++)
      stream << " rtv" << i << "=" << uint32_t(desc.RTVFormats[i]);
    for (size_t i = 0; i < graphics_state->input_elements.size(); i++) {
      const auto &element = graphics_state->input_elements[i];
      stream << " input" << i << "="
             << (i < graphics_state->input_element_semantic_names.size()
                     ? graphics_state->input_element_semantic_names[i]
                     : "")
             << element.SemanticIndex << ":" << uint32_t(element.Format)
             << ":" << element.InputSlot << ":" << element.AlignedByteOffset
             << ":" << uint32_t(element.InputSlotClass) << ":"
             << element.InstanceDataStepRate;
    }
  }

  if (compute_state)
    stream << " flags=" << uint32_t(compute_state->desc.Flags);

  return stream.str();
}

bool
D3D12PipelineDescFilterMatches(std::string_view group,
                               std::string_view desc) {
  size_t start = 0;
  bool has_token = false;
  for (;;) {
    const size_t end = group.find_first_of("; ", start);
    const auto token = TrimFilterToken(group.substr(
        start, end == std::string_view::npos ? group.size() - start
                                             : end - start));
    if (!token.empty()) {
      has_token = true;
      if (desc.find(token) == std::string_view::npos)
        return false;
    }
    if (end == std::string_view::npos)
      return has_token;
    start = end + 1;
  }
}

bool
D3D12PipelineKeyFilterMatches(std::string filter, std::string_view key,
                              std::string_view desc) {
  if (filter.find('=') == std::string::npos)
    return D3D12PipelineFilterMatches(std::move(filter), key);

  size_t start = 0;
  for (;;) {
    const size_t end = filter.find(',', start);
    const auto group = std::string_view(filter).substr(
        start, end == std::string::npos ? filter.size() - start
                                        : end - start);
    const auto trimmed = TrimFilterToken(group);
    if (!trimmed.empty() &&
        (D3D12PipelineFilterMatches(trimmed, key) ||
         D3D12PipelineDescFilterMatches(trimmed, desc)))
      return true;
    if (end == std::string::npos)
      return false;
    start = end + 1;
  }
}

std::string
D3D12PipelineStageFilter(PipelineShaderStage stage) {
  switch (stage) {
  case PipelineShaderStage::Vertex:
    return env::getEnvVar("DXMT_DUMP_PIPELINE_VS");
  case PipelineShaderStage::Pixel:
    return env::getEnvVar("DXMT_DUMP_PIPELINE_PS");
  case PipelineShaderStage::Geometry:
    return env::getEnvVar("DXMT_DUMP_PIPELINE_GS");
  case PipelineShaderStage::Hull:
    return env::getEnvVar("DXMT_DUMP_PIPELINE_HS");
  case PipelineShaderStage::Domain:
    return env::getEnvVar("DXMT_DUMP_PIPELINE_DS");
  case PipelineShaderStage::Compute:
    return env::getEnvVar("DXMT_DUMP_PIPELINE_CS");
  }
  return {};
}

bool
D3D12PipelineShaderFilterMatches(
    const std::vector<PipelineDxilShader> &shaders, PipelineShaderStage stage,
    std::string filter) {
  if (filter.empty())
    return true;

  auto *shader = FindShader(shaders, stage);
  if (!shader)
    return filter == "null";

  return D3D12PipelineFilterMatches(std::move(filter), PipelineShaderDigest(*shader));
}

bool
D3D12PipelineGraphicsStageFiltersMatch(
    const std::vector<PipelineDxilShader> &shaders) {
  constexpr PipelineShaderStage stages[] = {
      PipelineShaderStage::Vertex, PipelineShaderStage::Pixel,
      PipelineShaderStage::Geometry, PipelineShaderStage::Hull,
      PipelineShaderStage::Domain};

  bool has_stage_filter = false;
  for (auto stage : stages) {
    if (!D3D12PipelineStageFilter(stage).empty()) {
      has_stage_filter = true;
      break;
    }
  }
  if (!has_stage_filter)
    return false;

  for (auto stage : stages) {
    if (!D3D12PipelineShaderFilterMatches(
            shaders, stage, D3D12PipelineStageFilter(stage)))
      return false;
  }
  return true;
}

bool
D3D12ShouldDumpPipeline(std::string_view shader_cache_key,
                        const std::vector<PipelineDxilShader> &shaders,
                        const PipelineGraphicsState *graphics_state,
                        const PipelineComputeState *compute_state,
                        bool compute) {
  std::string mode = compute ? env::getEnvVar("DXMT_DUMP_COMPUTE_SHADERS")
                             : env::getEnvVar("DXMT_DUMP_PIPELINES");
  if (mode == "0" || mode == "none" || mode == "false")
    return false;

  auto key = env::getEnvVar("DXMT_DUMP_PIPELINE_KEY");
  const auto desc = D3D12DebugPipelineDesc(compute ? "compute" : "graphics",
                                           shader_cache_key, shaders,
                                           graphics_state, compute_state);
  if (!key.empty())
    return D3D12PipelineKeyFilterMatches(key, shader_cache_key, desc);

  if (compute) {
    if (mode == "1" || mode == "all")
      return true;

    auto cs_filter = mode.empty()
                         ? D3D12PipelineStageFilter(PipelineShaderStage::Compute)
                         : mode;
    if (cs_filter.empty())
      return false;

    return D3D12PipelineShaderFilterMatches(
        shaders, PipelineShaderStage::Compute, std::move(cs_filter));
  }

  if (D3D12PipelineGraphicsStageFiltersMatch(shaders))
    return true;

  return mode == "1" || mode == "all";
}

void
D3D12DumpPipelineShaders(const char *kind, std::string_view shader_cache_key,
                         const std::vector<PipelineDxilShader> &shaders,
                         const PipelineGraphicsState *graphics_state,
                         const PipelineComputeState *compute_state) {
  const bool compute = std::string_view(kind) == "compute";
  if (!D3D12ShouldDumpPipeline(shader_cache_key, shaders, graphics_state,
                               compute_state, compute))
    return;

  const auto dir = D3D12PipelineDumpDirectory();
  const auto key_prefix =
      std::string(shader_cache_key.substr(0, std::min<size_t>(16, shader_cache_key.size())));
  const auto manifest_path =
      dir + env::getExeBaseName() + "_d3d12_pipeline_" + kind + "_" +
      key_prefix + ".txt";

  std::ofstream manifest(manifest_path, std::ios::out | std::ios::trunc);
  if (manifest)
    manifest << "kind=" << kind << "\n"
             << "pso=" << shader_cache_key << "\n"
             << D3D12DebugPipelineDesc(kind, shader_cache_key, shaders,
                                       graphics_state, compute_state)
             << "\n";

  for (const auto &shader : shaders) {
    const auto digest = PipelineShaderDigest(shader);
    const auto stage = ShaderStageName(shader.stage);
    const auto bytecode_kind =
        shader.kind() == PipelineShaderBytecodeKind::Dxil ? "dxil" : "dxbc";
    const auto filename = env::getExeBaseName() + "_d3d12_" + key_prefix +
                          "_" + stage + "_" + digest.substr(0, 16) + "." +
                          bytecode_kind;
    const auto path = dir + filename;
    std::ofstream dump(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (dump && !shader.bytecode().empty())
      dump.write(reinterpret_cast<const char *>(shader.bytecode().data()),
                 shader.bytecode().size());
    if (manifest)
      manifest << stage << "=" << digest << " kind=" << bytecode_kind << " "
               << filename << " bytes=" << shader.bytecode().size() << "\n";
  }

  WARN("D3D12PipelineState: dumped ", kind, " pipeline shaders to ",
       manifest_path);
}

void
D3D12DumpFailedPipelineShader(const PipelineDxilShader &shader,
                              const char *function_name,
                              std::string_view error_message,
                              int compile_result,
                              uint64_t error_handle) {
  static std::atomic<uint32_t> failure_count = 0;
  const auto index = failure_count.fetch_add(1, std::memory_order_relaxed);
  if (index >= 32)
    return;

  const auto dir = D3D12PipelineDumpDirectory();
  const auto digest = PipelineShaderDigest(shader);
  const auto stage = ShaderStageName(shader.stage);
  const auto kind =
      shader.kind() == PipelineShaderBytecodeKind::Dxil ? "dxil" : "dxbc";
  const auto prefix = env::getExeBaseName() + "_d3d12_airconv_fail_" +
                      std::to_string(index) + "_" + stage + "_" +
                      digest.substr(0, 16);
  const auto shader_filename = prefix + "." + kind;
  const auto shader_path = dir + shader_filename;
  const auto manifest_path = dir + prefix + ".txt";

  std::ofstream dump(shader_path,
                     std::ios::out | std::ios::binary | std::ios::trunc);
  if (dump && !shader.bytecode().empty())
    dump.write(reinterpret_cast<const char *>(shader.bytecode().data()),
               shader.bytecode().size());

  std::ofstream manifest(manifest_path, std::ios::out | std::ios::trunc);
  if (manifest) {
    manifest << "stage=" << stage << "\n"
             << "kind=" << kind << "\n"
             << "digest=" << digest << "\n"
             << "bytes=" << shader.bytecode().size() << "\n"
             << "function=shader_main\n"
             << "requestedFunction=" << function_name << "\n"
             << "compileResult=" << compile_result << "\n"
             << "errorHandle=0x" << std::hex << error_handle << std::dec
             << "\n"
             << "errorMessage=" << error_message << "\n"
             << "shaderFile=" << shader_filename << "\n";
  }

  WARN("D3D12PipelineState: dumped failed airconv shader to ",
       manifest_path);
}

void
HashGraphicsInputElements(Sha1HashState &hash,
                          const PipelineGraphicsState &graphics_state) {
  HashValue(hash, uint32_t(graphics_state.input_elements.size()));
  for (size_t i = 0; i < graphics_state.input_elements.size(); i++) {
    const auto &element = graphics_state.input_elements[i];
    HashString(hash, i < graphics_state.input_element_semantic_names.size()
                         ? graphics_state.input_element_semantic_names[i]
                         : std::string_view());
    HashValue(hash, element.SemanticIndex);
    HashValue(hash, element.Format);
    HashValue(hash, element.InputSlot);
    HashValue(hash, element.AlignedByteOffset);
    HashValue(hash, element.InputSlotClass);
    HashValue(hash, element.InstanceDataStepRate);
  }
}

void
HashGraphicsStreamOutput(Sha1HashState &hash,
                         const PipelineGraphicsState &graphics_state) {
  HashValue(hash, uint32_t(graphics_state.stream_output_entries.size()));
  for (size_t i = 0; i < graphics_state.stream_output_entries.size(); i++) {
    const auto &entry = graphics_state.stream_output_entries[i];
    HashValue(hash, entry.Stream);
    HashString(hash, i < graphics_state.stream_output_semantic_names.size()
                         ? graphics_state.stream_output_semantic_names[i]
                         : std::string_view());
    HashValue(hash, entry.SemanticIndex);
    HashValue(hash, entry.StartComponent);
    HashValue(hash, entry.ComponentCount);
    HashValue(hash, entry.OutputSlot);
  }
  HashVector(hash, graphics_state.stream_output_strides);
}

void
HashBoolean(Sha1HashState &hash, BOOL value) {
  HashValue(hash, uint8_t(value != FALSE));
}

void
HashRenderTargetBlendState(Sha1HashState &hash,
                           const D3D12_RENDER_TARGET_BLEND_DESC &desc) {
  HashBoolean(hash, desc.BlendEnable);
  HashBoolean(hash, desc.LogicOpEnable);
  if (desc.BlendEnable) {
    HashValue(hash, desc.SrcBlend);
    HashValue(hash, desc.DestBlend);
    HashValue(hash, desc.BlendOp);
    HashValue(hash, desc.SrcBlendAlpha);
    HashValue(hash, desc.DestBlendAlpha);
    HashValue(hash, desc.BlendOpAlpha);
  }
  if (desc.LogicOpEnable)
    HashValue(hash, desc.LogicOp);
  HashValue(hash, desc.RenderTargetWriteMask);
}

void
HashBlendState(Sha1HashState &hash, const D3D12_BLEND_DESC &desc,
               UINT render_target_count) {
  HashBoolean(hash, desc.AlphaToCoverageEnable);
  HashBoolean(hash, desc.IndependentBlendEnable);
  const UINT state_count = render_target_count
                               ? (desc.IndependentBlendEnable
                                      ? render_target_count
                                      : 1u)
                               : 0u;
  HashValue(hash, state_count);
  for (UINT i = 0; i < state_count; ++i)
    HashRenderTargetBlendState(hash, desc.RenderTarget[i]);
}

void
HashRasterizerState(Sha1HashState &hash,
                    const D3D12_RASTERIZER_DESC &desc) {
  HashValue(hash, desc.FillMode);
  HashValue(hash, desc.CullMode);
  HashBoolean(hash, desc.FrontCounterClockwise);
  HashValue(hash, desc.DepthBias);
  HashValue(hash, desc.DepthBiasClamp);
  HashValue(hash, desc.SlopeScaledDepthBias);
  HashBoolean(hash, desc.DepthClipEnable);
  HashBoolean(hash, desc.MultisampleEnable);
  HashBoolean(hash, desc.AntialiasedLineEnable);
  HashValue(hash, desc.ForcedSampleCount);
  HashValue(hash, desc.ConservativeRaster);
}

void
HashDepthStencilFaceState(Sha1HashState &hash,
                          const D3D12_DEPTH_STENCILOP_DESC &desc) {
  HashValue(hash, desc.StencilFailOp);
  HashValue(hash, desc.StencilDepthFailOp);
  HashValue(hash, desc.StencilPassOp);
  HashValue(hash, desc.StencilFunc);
}

void
HashDepthStencilState(Sha1HashState &hash,
                      const D3D12_DEPTH_STENCIL_DESC &desc) {
  HashBoolean(hash, desc.DepthEnable);
  if (desc.DepthEnable) {
    HashValue(hash, desc.DepthWriteMask);
    HashValue(hash, desc.DepthFunc);
  }
  HashBoolean(hash, desc.StencilEnable);
  if (desc.StencilEnable) {
    HashValue(hash, desc.StencilReadMask);
    HashValue(hash, desc.StencilWriteMask);
    HashDepthStencilFaceState(hash, desc.FrontFace);
    HashDepthStencilFaceState(hash, desc.BackFace);
  }
}

void
HashSampleState(Sha1HashState &hash, const DXGI_SAMPLE_DESC &desc) {
  HashValue(hash, desc.Count);
  HashValue(hash, desc.Quality);
}

std::string
BuildShaderCacheKey(PipelineStateType type,
                    const std::vector<PipelineDxilShader> &shaders,
                    const PipelineGraphicsState &graphics_state,
                    const PipelineComputeState &compute_state,
                    ID3D12RootSignature *root_signature) {
  Sha1HashState hash;
  HashString(hash, type == PipelineStateType::Graphics
                       ? "dxmt-d3d12-graphics-pipeline-cache-v3"
                       : "dxmt-d3d12-compute-pipeline-cache-v2");
  HashPipelineShaders(hash, shaders);
  if (type == PipelineStateType::Graphics) {
    HashGraphicsInputElements(hash, graphics_state);
    HashGraphicsStreamOutput(hash, graphics_state);
    HashBlendState(hash, graphics_state.desc.BlendState,
                   graphics_state.desc.NumRenderTargets);
    HashValue(hash, graphics_state.desc.SampleMask);
    HashRasterizerState(hash, graphics_state.desc.RasterizerState);
    HashDepthStencilState(hash, graphics_state.desc.DepthStencilState);
    HashValue(hash, graphics_state.desc.PrimitiveTopologyType);
    HashValue(hash, graphics_state.desc.NumRenderTargets);
    HashBytes(hash, graphics_state.desc.RTVFormats,
              sizeof(graphics_state.desc.RTVFormats));
    HashValue(hash, graphics_state.desc.DSVFormat);
    HashSampleState(hash, graphics_state.desc.SampleDesc);
    HashValue(hash, graphics_state.desc.Flags);
  } else {
    HashValue(hash, compute_state.desc.Flags);
  }
  if (auto *root = GetDXMTRootSignature(root_signature)) {
    const auto blob = root->GetSerializedBlob();
    HashValue(hash, uint32_t(blob.size()));
    HashBytes(hash, blob.data(), blob.size());
  }
  return hash.final().string();
}

std::vector<uint8_t>
BuildCachedShaderBlob(PipelineStateType type,
                      const PipelineGraphicsState &graphics_state,
                      const PipelineComputeState &compute_state,
                      std::string_view shader_cache_key) {
  if (type == PipelineStateType::Graphics && !graphics_state.cached_pso.empty())
    return graphics_state.cached_pso;
  if (type == PipelineStateType::Compute && !compute_state.cached_pso.empty())
    return compute_state.cached_pso;

  constexpr std::string_view magic = "DXMT:D3D12:ShaderCache:";
  std::vector<uint8_t> blob;
  blob.reserve(magic.size() + shader_cache_key.size());
  blob.insert(blob.end(), magic.begin(), magic.end());
  blob.insert(blob.end(), shader_cache_key.begin(), shader_cache_key.end());
  return blob;
}

void
DestroyPipelineShaders(std::vector<PipelineDxilShader> &shaders) {
  shaders.clear();
}

std::string
BuildFunctionName(const char *prefix, std::string_view key) {
  std::string name(prefix);
  name += "_";
  name += key.substr(0, std::min<size_t>(key.size(), 16));
  return name;
}

std::string
BuildGraphicsVariantShaderCacheKey(std::string_view shader_cache_key,
                                   uint64_t demote_msaa_srv_mask_lo,
                                   uint64_t demote_msaa_srv_mask_hi) {
  if (!demote_msaa_srv_mask_lo && !demote_msaa_srv_mask_hi)
    return std::string(shader_cache_key);

  std::ostringstream key;
  key << shader_cache_key << "_ps_msaa_demote_" << std::hex
      << demote_msaa_srv_mask_lo << "_" << demote_msaa_srv_mask_hi;
  return key.str();
}

SM50_SHADER_METAL_VERSION
GetMetalVersion(IMTLD3D12Device *device) {
  return static_cast<SM50_SHADER_METAL_VERSION>(
      device->GetDXMTDevice().metalVersion());
}

SM50_SHADER_FLAG
GetShaderFlags() {
  return SM50_SHADER_FLAG(0);
}

template <typename PipelineInfo>
bool AttachPSOBinaryArchive(
    IMTLD3D12Device *device, PipelineInfo &info,
    std::array<obj_handle_t, 1> &lookup_archives) {
  if (ShouldInjectPipelineFault("DXMT_TEST_FAIL_PSO_ARCHIVE_LOOKUP_AT",
                                g_test_pso_archive_lookup_occurrence)) {
    RecordInjectedPipelineFault("DXMT_TEST_FAIL_PSO_ARCHIVE_LOOKUP_AT");
    WARN("D3D12PipelineState: injected PSO binary archive lookup failure");
    return false;
  }
  auto *archive = device->GetPSOBinaryArchive();
  if (!archive)
    return false;

  lookup_archives[0] = archive->handle;
  info.binary_archive_for_serialization = archive->handle;
  info.binary_archives_for_lookup.set(lookup_archives.data());
  info.num_binary_archives_for_lookup = lookup_archives.size();
  info.fail_on_binary_archive_miss = false;
  return true;
}

std::mutex &
PipelineMetalFunctionCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, PipelineMetalShader> &
PipelineMetalFunctionCache() {
  static std::unordered_map<std::string, PipelineMetalShader> cache;
  return cache;
}

std::string
BuildMetalFunctionCacheKey(IMTLD3D12Device *device, PipelineShaderStage stage,
                           const char *function_name, const void *bitcode,
                           size_t bitcode_size,
                           DXMT12_MTL4_SHADER_ABI_VERSION shader_abi_version) {
  Sha1HashState hash;
  HashString(hash, "dxmt-d3d12-metal-function-cache-v2");
  const auto device_handle = static_cast<obj_handle_t>(device->GetMTLDevice());
  HashValue(hash, device_handle);
  HashValue(hash, stage);
  HashValue(hash, uint32_t(shader_abi_version));
  HashString(hash, function_name ? std::string_view(function_name)
                                 : std::string_view());
  HashValue(hash, uint64_t(bitcode_size));
  HashBytes(hash, bitcode, bitcode_size);
  return hash.final().string();
}

bool
CreateCachedMetalFunction(IMTLD3D12Device *device, PipelineShaderStage stage,
                          const char *function_name,
                          sm50_bitcode_t bitcode_handle,
                          PipelineMetalShader &out,
                          DXMT12_MTL4_SHADER_ABI_VERSION shader_abi_version) {
  SM50_COMPILED_BITCODE bitcode = {};
  DXMT12SM50GetCompiledBitcode(bitcode_handle, &bitcode);
  const auto bitcode_data = AirconvLocalPointer(bitcode.Data);
  const auto bitcode_data_native = AirconvHandleValue(bitcode.Data);
  const auto can_hash_bitcode = bitcode_data && bitcode.Size;

  if (!bitcode_data_native || !bitcode.Size) {
    WARN("D3D12PipelineState: empty AIR bitcode for ",
         ShaderStageName(stage), " function=", function_name);
    DXMT12SM50DestroyBitcode(bitcode_handle);
    return false;
  }

  std::string cache_key;
  if (can_hash_bitcode) {
    cache_key = BuildMetalFunctionCacheKey(
        device, stage, function_name, bitcode_data, bitcode.Size,
        shader_abi_version);
    {
      std::lock_guard lock(PipelineMetalFunctionCacheMutex());
      auto &cache = PipelineMetalFunctionCache();
      auto cached = cache.find(cache_key);
      if (cached != cache.end()) {
        out = cached->second;
        DXMT12SM50DestroyBitcode(bitcode_handle);
        return true;
      }
    }
  }

  const auto materialize_begin = dxmt::clock::now();
  WMT::Reference<WMT::Error> metal_error;
  auto lib_data = WMT::MakeDispatchData(bitcode_data_native, bitcode.Size);
  PipelineMetalShader compiled = {};
  compiled.library = device->GetMTLDevice().newLibrary(lib_data, metal_error);
  DXMT12SM50DestroyBitcode(bitcode_handle);
  if (metal_error || !compiled.library) {
    WARN("D3D12PipelineState: failed to create Metal library for ",
         ShaderStageName(stage), ": ",
         metal_error ? metal_error.description().getUTF8String()
                     : "unknown error");
    return false;
  }

  compiled.function = compiled.library.newFunction(function_name);
  if (!compiled.function) {
    WARN("D3D12PipelineState: Metal function not found: ", function_name);
    return false;
  }

  if (can_hash_bitcode) {
    std::lock_guard lock(PipelineMetalFunctionCacheMutex());
    auto &cache = PipelineMetalFunctionCache();
    auto [it, inserted] = cache.emplace(cache_key, compiled);
    if (inserted)
      out = std::move(compiled);
    else
      out = it->second;
  } else {
    out = std::move(compiled);
  }
  dxmt::perf::recordPsoMaterializeTime(
      dxmt::perf::currentFrameStatistics(),
      dxmt::clock::now() - materialize_begin);

  return true;
}

bool
PersistentAirCacheEnabled() {
  // Default ON; DXMT_SHADER_CACHE=0 must disable every persistent shader cache
  // user, including this D3D12 AIR-bitcode layer.
  static const bool on =
      env::getEnvVar("DXMT_SHADER_CACHE") != "0" &&
      env::getEnvVar("DXMT_D3D12_PERSISTENT_SHADER_CACHE") != "0";
  return on;
}

bool HashShaderCompilationArguments(
    Sha1HashState &hash, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args) {
  for (auto *node = args; node; node =
           static_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(node->next)) {
    HashValue(hash, uint32_t(node->type));
    switch (node->type) {
    case SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT: {
      const auto *value =
          reinterpret_cast<SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA *>(node);
      HashValue(hash, value->num_output_slots);
      HashValue(hash, value->num_elements);
      HashBytes(hash, value->strides, sizeof(value->strides));
      for (uint32_t i = 0; i < value->num_elements; ++i) {
        HashValue(hash, value->elements[i].reg_id);
        HashValue(hash, value->elements[i].component);
        HashValue(hash, value->elements[i].output_slot);
        HashValue(hash, value->elements[i].offset);
      }
      break;
    }
    case SM50_SHADER_COMMON: {
      const auto *value = reinterpret_cast<SM50_SHADER_COMMON_DATA *>(node);
      HashValue(hash, uint32_t(value->metal_version));
      HashValue(hash, uint32_t(value->flags));
      break;
    }
    case SM50_SHADER_DIAG_FORCE_FULLSCREEN_POSITION: {
      const auto *value = reinterpret_cast<
          SM50_SHADER_DIAG_FORCE_FULLSCREEN_POSITION_DATA *>(node);
      HashValue(hash, uint8_t(value->enabled));
      break;
    }
    case SM50_SHADER_BINDLESS_MIRROR: {
      const auto *value =
          reinterpret_cast<SM50_SHADER_BINDLESS_MIRROR_DATA *>(node);
      HashValue(hash, uint8_t(value->enabled));
      break;
    }
    case SM50_SHADER_DXMT12_NATIVE_DESCRIPTOR_ABI: {
      const auto *value = reinterpret_cast<
          DXMT12_MTL4_NATIVE_DESCRIPTOR_ABI_DATA *>(node);
      HashValue(hash, uint32_t(value->version));
      HashValue(hash, uint8_t(value->enabled));
      break;
    }
    case SM50_SHADER_PSO_PIXEL_SHADER: {
      const auto *value =
          reinterpret_cast<SM50_SHADER_PSO_PIXEL_SHADER_DATA *>(node);
      HashValue(hash, value->sample_mask);
      HashValue(hash, uint8_t(value->dual_source_blending));
      if (value->dual_source_blending) {
        // AIR produced before dual-source output indices were propagated is
        // incompatible, so invalidate only affected persistent cache entries.
        constexpr uint8_t kDualSourceOutputAbiVersion = 1;
        HashValue(hash, kDualSourceOutputAbiVersion);
      }
      HashValue(hash, uint8_t(value->disable_depth_output));
      HashValue(hash, value->unorm_output_reg_mask);
      HashValue(hash, value->demote_msaa_srv_mask_lo);
      HashValue(hash, value->demote_msaa_srv_mask_hi);
      HashValue(hash, uint8_t(value->tessellation_primitive_id));
      break;
    }
    case SM50_SHADER_IA_INPUT_LAYOUT: {
      const auto *value =
          reinterpret_cast<SM50_SHADER_IA_INPUT_LAYOUT_DATA *>(node);
      HashValue(hash, uint32_t(value->index_buffer_format));
      HashValue(hash, value->slot_mask);
      HashValue(hash, value->num_elements);
      for (uint32_t i = 0; i < value->num_elements; ++i) {
        const auto &element = value->elements[i];
        HashValue(hash, element.reg);
        HashValue(hash, element.slot);
        HashValue(hash, element.aligned_byte_offset);
        HashValue(hash, element.format);
        HashValue(hash, uint32_t(element.step_function));
        HashValue(hash, uint32_t(element.step_rate));
      }
      break;
    }
    case SM50_SHADER_GS_PASS_THROUGH: {
      const auto *value =
          reinterpret_cast<SM50_SHADER_GS_PASS_THROUGH_DATA *>(node);
      HashValue(hash, value->DataEncoded);
      HashValue(hash, uint8_t(value->RasterizationDisabled));
      break;
    }
    case SM50_SHADER_PSO_GEOMETRY_SHADER: {
      const auto *value =
          reinterpret_cast<SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *>(node);
      HashValue(hash, uint8_t(value->strip_topology));
      break;
    }
    case SM50_SHADER_PSO_TESSELLATOR: {
      const auto *value =
          reinterpret_cast<SM50_SHADER_PSO_TESSELLATOR_DATA *>(node);
      HashValue(hash, value->max_potential_tess_factor);
      break;
    }
    default:
      return false;
    }
  }
  return true;
}

std::optional<Sha1Digest> BuildPersistentAirCacheKey(
    IMTLD3D12Device *device, PipelineShaderStage stage,
    std::string_view compile_kind,
    std::initializer_list<const PipelineDxilShader *> shaders,
    SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
    DXMT12_MTL4_SHADER_ABI_VERSION shader_abi_version) {
  Sha1HashState hash;
  HashString(hash, "dxmt-d3d12-persistent-air-cache-v8-legacy-ab");
  // Fold the DXMT build version into the key so the cache auto-invalidates
  // across clean commits and local dirty builds whenever airconv codegen could
  // have changed.
  HashString(hash, DXMT_VERSION);
  HashString(hash, compile_kind);
  HashValue(hash, uint32_t(stage));
  HashValue(hash, uint32_t(GetMetalVersion(device)));
  HashValue(hash, uint32_t(GetShaderFlags()));
  HashValue(hash, uint32_t(shader_abi_version));
  HashValue(hash, uint32_t(shaders.size()));
  for (const auto *shader : shaders) {
    if (!shader)
      return std::nullopt;
    HashValue(hash, uint32_t(shader->stage));
    HashValue(hash, uint32_t(shader->kind()));
    HashVector(hash, shader->bytecode());
  }
  if (!HashShaderCompilationArguments(hash, args))
    return std::nullopt;
  return hash.final();
}

void LogPersistentAirCacheMarker(const char *result,
                                 PipelineShaderStage stage,
                                 const Sha1Digest &key) {
  const auto marker_path = env::getEnvVar("DXMT_PERSISTENT_AIR_CACHE_MARKER");
  if (marker_path.empty())
    return;
  FILE *marker = fopen(marker_path.c_str(), "a");
  if (!marker)
    return;
  fprintf(marker, "%s stage=%s key=%s\n", result, ShaderStageName(stage),
          key.string().c_str());
  fclose(marker);
}

bool
CompileMetalFunction(IMTLD3D12Device *device, PipelineDxilShader &shader,
                     const char *function_name,
                     SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
                     PipelineMetalShader &out,
                     DXMT12_MTL4_SHADER_ABI_VERSION shader_abi_version,
                     bool read_persistent_cache = true) {
  const char *air_function_name = "shader_main";

  // PERF DIAG (DXMT_DIAG_STALL): persistent-cache hit/miss + airconv transpile
  // time. Distinguishes "airconv transpile is the stall (cache fixes it)" from
  // "newRenderPipelineState is the stall (Metal-side, needs binary archive)".
  static const bool stall_diag = env::getEnvVar("DXMT_DIAG_STALL") == "1";
  static std::atomic<uint64_t> g_cache_hits{0}, g_cache_misses{0},
      g_transpile_us{0};

  // Persistent AIR cache lookup: a hit rebuilds the MTLLibrary from cached
  // bitcode and skips the airconv transpile (the ~99% record-thread stall).
  const auto persistent_key_result = BuildPersistentAirCacheKey(
      device, shader.stage, "single", {&shader}, args, shader_abi_version);
  const bool persistent =
      PersistentAirCacheEnabled() && persistent_key_result.has_value();
  Sha1Digest persistent_key = {};
  ShaderCache *scache = nullptr;
  if (persistent) {
    persistent_key = *persistent_key_result;
    scache = &ShaderCache::getInstance(device->GetDXMTDevice().metalVersion());
  }
  if (persistent && read_persistent_cache) {
    if (auto reader = scache->getReader()) {
      const auto lookup_begin = dxmt::clock::now();
      auto lib_data = reader->get(persistent_key);
      dxmt::perf::recordPsoCacheLookupTime(
          dxmt::perf::currentFrameStatistics(),
          dxmt::clock::now() - lookup_begin);
      if (lib_data) {
        const auto materialize_begin = dxmt::clock::now();
        WMT::Reference<WMT::Error> metal_error;
        PipelineMetalShader compiled = {};
        compiled.library = device->GetMTLDevice().newLibrary(lib_data, metal_error);
        if (compiled.library) {
          compiled.function = compiled.library.newFunction(air_function_name);
          if (compiled.function) {
            compiled.persistent_cache_hit = true;
            compiled.persistent_cache_key = persistent_key.string();
            out = std::move(compiled);
            LogPersistentAirCacheMarker("hit", shader.stage, persistent_key);
            if (stall_diag) {
              auto h = g_cache_hits.fetch_add(1, std::memory_order_relaxed) + 1;
              if ((h & 255) == 0)
                INFO("D3D12 persistent-cache stats: hits=", h,
                     " misses=", g_cache_misses.load(std::memory_order_relaxed),
                     " transpileUs=", g_transpile_us.load(std::memory_order_relaxed));
            }
            dxmt::perf::recordPsoMaterializeTime(
                dxmt::perf::currentFrameStatistics(),
                dxmt::clock::now() - materialize_begin);
            return true; // cache hit: airconv transpile skipped
          }
        }
        // Fall through to recompile if the cached bitcode failed to rebuild.
      }
    }
  }
  if (persistent) {
    LogPersistentAirCacheMarker("miss", shader.stage, persistent_key);
    if (stall_diag)
      g_cache_misses.fetch_add(1, std::memory_order_relaxed);
  }

  sm50_bitcode_t bitcode_handle = nullptr;
  sm50_error_t error = nullptr;
  const auto t0 = stall_diag ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
  const int compile_failed =
      shader.kind() == PipelineShaderBytecodeKind::Dxil
          ? DXMT12DXILCompile(shader.shaderHandle(), args, air_function_name,
                              &bitcode_handle, &error)
          : DXMT12SM50Compile(shader.shaderHandle(), args, air_function_name,
                              &bitcode_handle, &error);
  if (stall_diag)
    g_transpile_us.fetch_add(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0)
            .count(),
        std::memory_order_relaxed);
  if (compile_failed) {
    auto error_message = DXMT12SM50GetErrorMessageString(error);
    WARN("D3D12PipelineState: airconv compile diagnostic stage=",
         ShaderStageName(shader.stage),
         shader.kind() == PipelineShaderBytecodeKind::Dxil ? " kind=DXIL"
                                                         : " kind=DXBC",
         " ret=", compile_failed, " error=", AirconvHandleValue(error),
         " message_len=", error_message.size(), " function=", air_function_name,
         " requestedFunction=", function_name);
    WARN("D3D12PipelineState: failed to compile ", ShaderStageName(shader.stage),
         shader.kind() == PipelineShaderBytecodeKind::Dxil ? " DXIL shader: "
                                                         : " DXBC shader: ",
         error_message);
    D3D12DumpFailedPipelineShader(shader, function_name, error_message,
                                  compile_failed,
                                  AirconvHandleValue(error));
    DXMT12SM50FreeError(error);
    return false;
  }

  // Cache miss: persist the freshly transpiled AIR bitcode for future runs.
  // GetCompiledBitcode is non-destructive (returns pointers into the handle;
  // the handle is freed later by CreateCachedMetalFunction), so reading the
  // bytes here and again inside CreateCachedMetalFunction is safe.
  if (persistent && scache) {
    SM50_COMPILED_BITCODE bitcode = {};
    DXMT12SM50GetCompiledBitcode(bitcode_handle, &bitcode);
    const auto bitcode_data_native = AirconvHandleValue(bitcode.Data);
    if (bitcode_data_native && bitcode.Size) {
      if (auto writer = scache->getWriter()) {
        writer->set(persistent_key,
                    WMT::MakeDispatchData(bitcode_data_native, bitcode.Size));
      }
    }
  }

  return CreateCachedMetalFunction(device, shader.stage, air_function_name,
                                   bitcode_handle, out, shader_abi_version);
}

bool
CreateMetalFunctionFromBitcode(IMTLD3D12Device *device,
                               PipelineShaderStage stage,
                               const char *function_name,
                               sm50_bitcode_t bitcode_handle,
                               PipelineMetalShader &out,
                               DXMT12_MTL4_SHADER_ABI_VERSION shader_abi_version) {
  return CreateCachedMetalFunction(device, stage, function_name, bitcode_handle,
                                   out, shader_abi_version);
}

template <typename Compile>
bool CompileCombinedMetalFunction(
    IMTLD3D12Device *device, PipelineShaderStage stage,
    std::string_view compile_kind,
    std::initializer_list<const PipelineDxilShader *> shaders,
    const char *function_name, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
    PipelineMetalShader &out, const char *failure_description,
    DXMT12_MTL4_SHADER_ABI_VERSION shader_abi_version, Compile &&compile) {
  const auto persistent_key_result = BuildPersistentAirCacheKey(
      device, stage, compile_kind, shaders, args,
      shader_abi_version);
  const bool persistent =
      PersistentAirCacheEnabled() && persistent_key_result.has_value();
  Sha1Digest persistent_key = {};
  ShaderCache *scache = nullptr;
  std::string cached_function_name;
  const char *air_function_name = function_name;
  if (persistent) {
    persistent_key = *persistent_key_result;
    scache = &ShaderCache::getInstance(device->GetDXMTDevice().metalVersion());
    const std::string function_prefix(compile_kind);
    cached_function_name =
        BuildFunctionName(function_prefix.c_str(), persistent_key.string());
    air_function_name = cached_function_name.c_str();
  }

  if (persistent) {
    if (auto reader = scache->getReader()) {
      const auto lookup_begin = dxmt::clock::now();
      auto lib_data = reader->get(persistent_key);
      dxmt::perf::recordPsoCacheLookupTime(
          dxmt::perf::currentFrameStatistics(),
          dxmt::clock::now() - lookup_begin);
      if (lib_data) {
        const auto materialize_begin = dxmt::clock::now();
        WMT::Reference<WMT::Error> metal_error;
        PipelineMetalShader compiled = {};
        compiled.library =
            device->GetMTLDevice().newLibrary(lib_data, metal_error);
        if (compiled.library) {
          compiled.function = compiled.library.newFunction(air_function_name);
          if (compiled.function) {
            compiled.persistent_cache_hit = true;
            compiled.persistent_cache_key = persistent_key.string();
            out = std::move(compiled);
            LogPersistentAirCacheMarker("hit", stage, persistent_key);
            dxmt::perf::recordPsoMaterializeTime(
                dxmt::perf::currentFrameStatistics(),
                dxmt::clock::now() - materialize_begin);
            return true;
          }
        }
      }
    }
    LogPersistentAirCacheMarker("miss", stage, persistent_key);
  }

  sm50_bitcode_t bitcode_handle = nullptr;
  sm50_error_t error = nullptr;
  const int compile_failed =
      compile(air_function_name, &bitcode_handle, &error);
  if (compile_failed) {
    WARN("D3D12PipelineState: ", failure_description, ": ",
         DXMT12SM50GetErrorMessageString(error));
    DXMT12SM50FreeError(error);
    return false;
  }

  if (persistent && scache) {
    SM50_COMPILED_BITCODE bitcode = {};
    DXMT12SM50GetCompiledBitcode(bitcode_handle, &bitcode);
    const auto bitcode_data_native = AirconvHandleValue(bitcode.Data);
    if (bitcode_data_native && bitcode.Size) {
      if (auto writer = scache->getWriter()) {
        writer->set(persistent_key,
                    WMT::MakeDispatchData(bitcode_data_native, bitcode.Size));
      }
    }
  }

  return CreateMetalFunctionFromBitcode(device, stage, air_function_name,
                                        bitcode_handle, out,
                                        shader_abi_version);
}

bool
CompileGeometryPipelineVertexFunction(
    IMTLD3D12Device *device, PipelineDxilShader &vs, PipelineDxilShader &gs,
    const char *function_name, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
    PipelineMetalShader &out,
    DXMT12_MTL4_SHADER_ABI_VERSION shader_abi_version) {
  if (vs.kind() != PipelineShaderBytecodeKind::Dxbc ||
      gs.kind() != PipelineShaderBytecodeKind::Dxbc) {
    WARN("D3D12PipelineState: DXIL geometry shader mesh lowering is unsupported");
    return false;
  }

  return CompileCombinedMetalFunction(
      device, PipelineShaderStage::Vertex, "geometry-vertex", {&vs, &gs},
      function_name, args, out,
      "failed to compile DXBC geometry vertex shader",
      shader_abi_version,
      [&](const char *name, sm50_bitcode_t *bitcode, sm50_error_t *error) {
        return DXMT12SM50CompileGeometryPipelineVertex(
            vs.shaderHandle(), gs.shaderHandle(), args, name, bitcode, error);
      });
}

bool
CompileGeometryPipelineGeometryFunction(
    IMTLD3D12Device *device, PipelineDxilShader &vs, PipelineDxilShader &gs,
    const char *function_name, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
    PipelineMetalShader &out,
    DXMT12_MTL4_SHADER_ABI_VERSION shader_abi_version) {
  if (vs.kind() != PipelineShaderBytecodeKind::Dxbc ||
      gs.kind() != PipelineShaderBytecodeKind::Dxbc) {
    WARN("D3D12PipelineState: DXIL geometry shader mesh lowering is unsupported");
    return false;
  }

  return CompileCombinedMetalFunction(
      device, PipelineShaderStage::Geometry, "geometry-mesh", {&vs, &gs},
      function_name, args, out, "failed to compile DXBC geometry shader",
      shader_abi_version,
      [&](const char *name, sm50_bitcode_t *bitcode, sm50_error_t *error) {
        return DXMT12SM50CompileGeometryPipelineGeometry(
            vs.shaderHandle(), gs.shaderHandle(), args, name, bitcode, error);
      });
}

bool
CompileTessellationPipelineHullFunction(
    IMTLD3D12Device *device, PipelineDxilShader &vs, PipelineDxilShader &hs,
    const char *function_name, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
    PipelineMetalShader &out,
    DXMT12_MTL4_SHADER_ABI_VERSION shader_abi_version) {
  if (vs.kind() != PipelineShaderBytecodeKind::Dxbc ||
      hs.kind() != PipelineShaderBytecodeKind::Dxbc) {
    // TODO(d3d12): add DXIL tessellation lowering once airconv exposes it.
    WARN("D3D12PipelineState: DXIL tessellation hull lowering is unsupported");
    return false;
  }

  return CompileCombinedMetalFunction(
      device, PipelineShaderStage::Hull, "tessellation-hull", {&vs, &hs},
      function_name, args, out,
      "failed to compile DXBC tessellation hull shader",
      shader_abi_version,
      [&](const char *name, sm50_bitcode_t *bitcode, sm50_error_t *error) {
        return DXMT12SM50CompileTessellationPipelineHull(
            vs.shaderHandle(), hs.shaderHandle(), args, name, bitcode, error);
      });
}

bool
CompileTessellationPipelineDomainFunction(
    IMTLD3D12Device *device, PipelineDxilShader &hs, PipelineDxilShader &ds,
    const char *function_name, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
    PipelineMetalShader &out,
    DXMT12_MTL4_SHADER_ABI_VERSION shader_abi_version) {
  if (hs.kind() != PipelineShaderBytecodeKind::Dxbc ||
      ds.kind() != PipelineShaderBytecodeKind::Dxbc) {
    // TODO(d3d12): add DXIL tessellation lowering once airconv exposes it.
    WARN("D3D12PipelineState: DXIL tessellation domain lowering is unsupported");
    return false;
  }

  return CompileCombinedMetalFunction(
      device, PipelineShaderStage::Domain, "tessellation-domain", {&hs, &ds},
      function_name, args, out,
      "failed to compile DXBC tessellation domain shader",
      shader_abi_version,
      [&](const char *name, sm50_bitcode_t *bitcode, sm50_error_t *error) {
        return DXMT12SM50CompileTessellationPipelineDomain(
            hs.shaderHandle(), ds.shaderHandle(), args, name, bitcode, error);
      });
}

WMTPrimitiveTopologyClass
GetTopologyClass(D3D12_PRIMITIVE_TOPOLOGY_TYPE type) {
  switch (type) {
  case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT:
    return WMTPrimitiveTopologyClassPoint;
  case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:
    return WMTPrimitiveTopologyClassLine;
  case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE:
    return WMTPrimitiveTopologyClassTriangle;
  default:
    return WMTPrimitiveTopologyClassUnspecified;
  }
}

bool
BuildInputElements(IMTLD3D12Device *device,
                   const PipelineGraphicsState &state,
                   const PipelineDxilShader *vs,
                   std::vector<SM50_IA_INPUT_ELEMENT> &elements,
                   uint32_t &slot_mask) {
  elements.clear();
  slot_mask = 0;
  std::array<uint32_t, 32> append_offsets = {};
  std::vector<SM50_IA_INPUT_ELEMENT> layout_elements;
  layout_elements.reserve(state.input_elements.size());

  for (uint32_t i = 0; i < state.input_elements.size(); i++) {
    const auto &input = state.input_elements[i];
    if (input.InputSlot >= append_offsets.size()) {
      WARN("D3D12PipelineState: unsupported input layout slot ",
           uint32_t(input.InputSlot));
      return false;
    }

    MTL_DXGI_FORMAT_DESC format = {};
    if (FAILED(MTLQueryDXGIFormat(device->GetMTLDevice(), input.Format,
                                  format)) ||
        format.AttributeFormat == WMTAttributeFormatInvalid) {
      WARN("D3D12PipelineState: unsupported input layout format ",
           uint32_t(input.Format));
      return false;
    }
    if (!format.BytesPerTexel) {
      WARN("D3D12PipelineState: unsupported non-ordinary input layout format ",
           uint32_t(input.Format));
      return false;
    }

    const auto aligned_byte_offset =
        input.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT
            ? align(append_offsets[input.InputSlot],
                    std::min(4u, format.BytesPerTexel))
            : input.AlignedByteOffset;
    append_offsets[input.InputSlot] =
        aligned_byte_offset + format.BytesPerTexel;

    layout_elements.push_back({
        .reg = i,
        .slot = input.InputSlot,
        .aligned_byte_offset = aligned_byte_offset,
        .format = uint32_t(format.AttributeFormat),
        .step_function = input.InputSlotClass ==
                         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,
        .step_rate = input.InstanceDataStepRate,
    });
    if (D3D12PipelineDiagShouldLog()) {
      INFO("D3D12 diagnostic: pipeline input element",
           " semantic=", input.SemanticName ? input.SemanticName : "",
           " semanticIndex=", uint32_t(input.SemanticIndex),
           " format=", uint32_t(input.Format),
           " slot=", uint32_t(input.InputSlot),
           " sourceOffset=", uint32_t(input.AlignedByteOffset),
           " loweredOffset=", uint32_t(aligned_byte_offset),
           " attrFormat=", uint32_t(format.AttributeFormat),
           " bytes=", uint32_t(format.BytesPerTexel),
           " slotClass=", uint32_t(input.InputSlotClass),
           " stepRate=", uint32_t(input.InstanceDataStepRate),
           " reg=", i);
    }
    if (input.InputSlot < 32)
      slot_mask |= 1u << input.InputSlot;
  }

  if (!vs) {
    elements = std::move(layout_elements);
    return true;
  }

  const auto *translation = vs->translation();
  if (!translation) {
    elements = std::move(layout_elements);
    return true;
  }

  auto equal_semantic = [](std::string_view lhs, std::string_view rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                      [](char a, char b) {
                        return std::tolower(static_cast<unsigned char>(a)) ==
                               std::tolower(static_cast<unsigned char>(b));
                      });
  };

  for (const auto &sig : translation->signatures) {
    if (sig.kind != dxil::DxilTranslationSignatureKind::Input ||
        IsSystemGeneratedValue(sig))
      continue;

    auto match = std::find_if(
        state.input_elements.begin(), state.input_elements.end(),
        [&](const D3D12_INPUT_ELEMENT_DESC &input) {
          const auto index =
              uint32_t(&input - state.input_elements.data());
          const auto semantic =
              index < state.input_element_semantic_names.size()
                  ? std::string_view(state.input_element_semantic_names[index])
                  : std::string_view();
          if (!sig.semantic_name.empty())
            return input.SemanticIndex == sig.semantic_index &&
                   equal_semantic(semantic, sig.semantic_name);
          return equal_semantic(
              str::format(semantic, input.SemanticIndex), sig.semantic_key);
        });
    if (match == state.input_elements.end())
      continue;

    const auto layout_index = size_t(match - state.input_elements.begin());
    auto element = layout_elements[layout_index];
    element.reg = sig.start_row;
    elements.push_back(element);
  }

  if (elements.empty())
    elements = std::move(layout_elements);

  return true;
}

template <typename T, size_t N>
const T &
Lookup(const T (&table)[N], uint32_t index, const T &fallback) {
  return index < N ? table[index] : fallback;
}

bool
IsDualSourceBlendFactor(D3D12_BLEND factor) {
  return factor >= D3D12_BLEND_SRC1_COLOR &&
         factor <= D3D12_BLEND_INV_SRC1_ALPHA;
}

bool
IsDualSourceBlend(const D3D12_RENDER_TARGET_BLEND_DESC &desc) {
  return desc.BlendEnable &&
         (IsDualSourceBlendFactor(desc.SrcBlend) ||
          IsDualSourceBlendFactor(desc.DestBlend) ||
          IsDualSourceBlendFactor(desc.SrcBlendAlpha) ||
          IsDualSourceBlendFactor(desc.DestBlendAlpha));
}

bool
UsesDualSourceBlending(const D3D12_BLEND_DESC &desc,
                       uint32_t render_target_count) {
  const auto count = desc.IndependentBlendEnable
                         ? std::min(render_target_count, 8u)
                         : std::min(render_target_count, 1u);
  for (UINT i = 0; i < count; i++) {
    if (IsDualSourceBlend(desc.RenderTarget[i]))
      return true;
  }
  return false;
}

bool
CanLowerD3D12DualSourceBlending(const PipelineGraphicsState &state,
                                uint32_t pixel_shader_render_target_mask) {
  if (!UsesDualSourceBlending(state.desc.BlendState,
                              state.desc.NumRenderTargets))
    return false;

  // D3D dual-source blending consumes o0 and o1 for the single render target
  // at slot 0. Metal represents those values as color(0), index(0) and
  // color(0), index(1), and rejects Source1 factors when the second output is
  // absent. D3D leaves missing pixel-shader outputs undefined, so omit the
  // incompatible blend state instead of submitting an invalid Metal pipeline.
  return state.desc.NumRenderTargets == 1 &&
         IsDualSourceBlend(state.desc.BlendState.RenderTarget[0]) &&
         (pixel_shader_render_target_mask & 0x3u) == 0x3u;
}

bool
ValidateGraphicsRenderFormats(IMTLD3D12Device *device,
                              const PipelineGraphicsState &state,
                              std::array<WMTPixelFormat, 8> &rtv_formats,
                              WMTPixelFormat &depth_format,
                              WMTPixelFormat &stencil_format) {
  rtv_formats.fill(WMTPixelFormatInvalid);
  depth_format = WMTPixelFormatInvalid;
  stencil_format = WMTPixelFormatInvalid;

  for (UINT i = 0; i < state.desc.NumRenderTargets; i++) {
    const auto format = state.desc.RTVFormats[i];
    if (format == DXGI_FORMAT_UNKNOWN)
      continue;
    const auto pixel_format =
        GetRenderTargetPixelFormat(device->GetMTLDevice(), format);
    if (pixel_format == WMTPixelFormatInvalid ||
        DepthStencilPlanarFlags(pixel_format)) {
      WARN("D3D12PipelineState: unsupported RTV format slot=", i,
           " format=", uint32_t(format));
      return false;
    }
    rtv_formats[i] = pixel_format;
  }

  if (state.desc.DSVFormat != DXGI_FORMAT_UNKNOWN) {
    MTL_DXGI_FORMAT_DESC format = {};
    if (FAILED(MTLQueryDXGIFormat(device->GetMTLDevice(),
                                  state.desc.DSVFormat, format)) ||
        format.PixelFormat == WMTPixelFormatInvalid ||
        !DepthStencilPlanarFlags(format.PixelFormat)) {
      WARN("D3D12PipelineState: unsupported DSV format format=",
           uint32_t(state.desc.DSVFormat));
      return false;
    }
    if (DepthStencilPlanarFlags(format.PixelFormat) & 1)
      depth_format = format.PixelFormat;
    if (DepthStencilPlanarFlags(format.PixelFormat) & 2)
      stencil_format = format.PixelFormat;
  }

  return true;
}

template <typename PipelineInfo>
void
ApplyBlendState(PipelineInfo &info,
                const D3D12_BLEND_DESC &blend_desc,
                uint32_t render_target_count,
                uint32_t pixel_shader_render_target_mask,
                bool metal_dual_source_blending,
                const FormatCapabilityInspector &format_capabilities) {
  for (UINT rt = 0; rt < render_target_count && rt < 8; rt++) {
    const auto &src =
        blend_desc.RenderTarget[blend_desc.IndependentBlendEnable ? rt : 0];
    auto &dst = info.colors[rt];
    dst.write_mask =
        Lookup(kColorWriteMaskMap, uint32_t(src.RenderTargetWriteMask),
               kColorWriteMaskMap[15]);
    if (!src.BlendEnable || dst.pixel_format == WMTPixelFormatInvalid ||
        !(pixel_shader_render_target_mask & (1u << rt)))
      continue;
    if (IsDualSourceBlend(src) && !metal_dual_source_blending)
      continue;

    const auto format_capability =
        format_capabilities.textureCapabilities.find(dst.pixel_format);
    if (format_capability == format_capabilities.textureCapabilities.end() ||
        !any_bit_set(format_capability->second & FormatCapability::Blend)) {
      WARN("D3D12PipelineState: ignoring blending on non-blendable RTV slot=",
           rt, " format=", dst.pixel_format);
      continue;
    }

    dst.blending_enabled = true;
    dst.rgb_blend_operation =
        Lookup(kBlendOpMap, uint32_t(src.BlendOp), WMTBlendOperationAdd);
    dst.alpha_blend_operation =
        Lookup(kBlendOpMap, uint32_t(src.BlendOpAlpha), WMTBlendOperationAdd);
    dst.src_rgb_blend_factor =
        Lookup(kBlendFactorMap, uint32_t(src.SrcBlend), WMTBlendFactorOne);
    dst.dst_rgb_blend_factor =
        Lookup(kBlendFactorMap, uint32_t(src.DestBlend), WMTBlendFactorZero);
    dst.src_alpha_blend_factor =
        Lookup(kBlendAlphaFactorMap, uint32_t(src.SrcBlendAlpha),
               WMTBlendFactorOne);
    dst.dst_alpha_blend_factor =
        Lookup(kBlendAlphaFactorMap, uint32_t(src.DestBlendAlpha),
               WMTBlendFactorZero);
  }

  info.alpha_to_coverage_enabled = blend_desc.AlphaToCoverageEnable;
  if (!blend_desc.IndependentBlendEnable &&
      blend_desc.RenderTarget[0].LogicOpEnable) {
    info.logic_operation_enabled = true;
    info.logic_operation =
        Lookup(kLogicOpMap, uint32_t(blend_desc.RenderTarget[0].LogicOp),
               WMTLogicOperationCopy);
  }
}

void
BuildRasterizerCommand(const D3D12_RASTERIZER_DESC &desc,
                       wmtcmd_render_setrasterizerstate &cmd) {
  cmd = {};
  cmd.type = WMTRenderCommandSetRasterizerState;
  cmd.fill_mode = desc.FillMode == D3D12_FILL_MODE_WIREFRAME
                      ? WMTTriangleFillModeLines
                      : WMTTriangleFillModeFill;
  switch (desc.CullMode) {
  case D3D12_CULL_MODE_FRONT:
    cmd.cull_mode = WMTCullModeFront;
    break;
  case D3D12_CULL_MODE_BACK:
    cmd.cull_mode = WMTCullModeBack;
    break;
  default:
    cmd.cull_mode = WMTCullModeNone;
    break;
  }
  cmd.depth_clip_mode =
      desc.DepthClipEnable ? WMTDepthClipModeClip : WMTDepthClipModeClamp;
  cmd.winding = desc.FrontCounterClockwise ? WMTWindingCounterClockwise
                                           : WMTWindingClockwise;
  cmd.depth_bias = float(desc.DepthBias);
  cmd.scole_scale = desc.SlopeScaledDepthBias;
  cmd.depth_bias_clamp = desc.DepthBiasClamp;
}

void
FillStencilInfo(WMTStencilInfo &dst,
                const D3D12_DEPTH_STENCILOP_DESC &src,
                UINT8 read_mask, UINT8 write_mask) {
  dst.enabled = true;
  dst.depth_stencil_pass_op =
      Lookup(kStencilOperationMap, uint32_t(src.StencilPassOp),
             WMTStencilOperationKeep);
  dst.stencil_fail_op =
      Lookup(kStencilOperationMap, uint32_t(src.StencilFailOp),
             WMTStencilOperationKeep);
  dst.depth_fail_op =
      Lookup(kStencilOperationMap, uint32_t(src.StencilDepthFailOp),
             WMTStencilOperationKeep);
  dst.stencil_compare_function =
      Lookup(kCompareFunctionMap, uint32_t(src.StencilFunc),
             WMTCompareFunctionAlways);
  dst.read_mask = read_mask;
  dst.write_mask = write_mask;
}

WMT::Reference<WMT::DepthStencilState>
CreateDepthStencilState(IMTLD3D12Device *device,
                        const D3D12_DEPTH_STENCIL_DESC &desc) {
  WMTDepthStencilInfo info = {};
  info.depth_compare_function = WMTCompareFunctionAlways;
  info.depth_write_enabled = false;
  info.front_stencil.enabled = false;
  info.back_stencil.enabled = false;

  if (desc.DepthEnable) {
    info.depth_compare_function =
        Lookup(kCompareFunctionMap, uint32_t(desc.DepthFunc),
               WMTCompareFunctionAlways);
    info.depth_write_enabled =
        desc.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
  }

  if (desc.StencilEnable) {
    FillStencilInfo(info.front_stencil, desc.FrontFace,
                    desc.StencilReadMask, desc.StencilWriteMask);
    FillStencilInfo(info.back_stencil, desc.BackFace,
                    desc.StencilReadMask, desc.StencilWriteMask);
  }

  return device->GetMTLDevice().newDepthStencilState(info);
}

static bool
NativeRootParameterVisibleToShader(const RootSignatureParameter &parameter,
                                   PipelineShaderStage stage) {
  if (parameter.visibility == D3D12_SHADER_VISIBILITY_ALL)
    return true;
  switch (stage) {
  case PipelineShaderStage::Vertex:
    return parameter.visibility == D3D12_SHADER_VISIBILITY_VERTEX;
  case PipelineShaderStage::Pixel:
    return parameter.visibility == D3D12_SHADER_VISIBILITY_PIXEL;
  case PipelineShaderStage::Geometry:
    return parameter.visibility == D3D12_SHADER_VISIBILITY_GEOMETRY;
  case PipelineShaderStage::Hull:
    return parameter.visibility == D3D12_SHADER_VISIBILITY_HULL;
  case PipelineShaderStage::Domain:
    return parameter.visibility == D3D12_SHADER_VISIBILITY_DOMAIN;
  case PipelineShaderStage::Compute:
    return false;
  }
  return false;
}

static std::optional<D3D12_DESCRIPTOR_RANGE_TYPE>
NativeDescriptorRangeType(SM50BindingType type) {
  switch (type) {
  case SM50BindingType::ConstantBuffer:
    return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
  case SM50BindingType::Sampler:
    return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
  case SM50BindingType::SRV:
    return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  case SM50BindingType::UAV:
    return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  }
  return std::nullopt;
}

static bool
NativeShaderArgumentHasSingleTableRange(
    const RootSignature &root, PipelineShaderStage stage,
    const DXMT12_MTL4_SHADER_ARGUMENT &argument) {
  const auto wanted_type = NativeDescriptorRangeType(argument.Type);
  if (!wanted_type)
    return false;

  const uint32_t lower = argument.RegisterCount
                             ? argument.RegisterLowerBound
                             : argument.SM50BindingSlot;
  const uint32_t count = argument.RegisterCount ? argument.RegisterCount : 1;
  const uint32_t space = argument.RegisterCount ? argument.RegisterSpace : 0;
  if (!count || count == UINT_MAX || lower > UINT_MAX - count)
    return false;

  uint32_t matches = 0;
  for (const auto &parameter : root.GetParameters()) {
    if (!NativeRootParameterVisibleToShader(parameter, stage))
      continue;
    if (parameter.parameter_type !=
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      const bool constants =
          parameter.parameter_type ==
          D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      const auto parameter_type =
          constants || parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV
              ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
              : parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_SRV
                    ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                    : parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_UAV
                          ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                          : D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      const uint32_t parameter_register =
          constants ? parameter.constants.ShaderRegister
                    : parameter.descriptor.ShaderRegister;
      const uint32_t parameter_space =
          constants ? parameter.constants.RegisterSpace
                    : parameter.descriptor.RegisterSpace;
      if (count == 1 && parameter_type == *wanted_type &&
          parameter_register == lower && parameter_space == space)
        matches++;
      continue;
    }
    for (const auto &range : parameter.ranges) {
      if (range.range_type != *wanted_type ||
          range.register_space != space || !range.descriptor_count ||
          range.descriptor_count == UINT_MAX ||
          lower < range.base_shader_register)
        continue;
      const uint32_t local = lower - range.base_shader_register;
      if (local > range.descriptor_count ||
          count > range.descriptor_count - local)
        continue;
      matches++;
    }
  }
  return matches == 1;
}

static NativeShaderAbiEligibilityReason
GetNativeShaderAbiEligibilityImpl(
    const std::vector<PipelineDxilShader> &shaders,
    const RootSignature *root_signature) {
  if (!root_signature || !root_signature->GetStaticSamplers().empty())
    return NativeShaderAbiEligibilityReason::UnsupportedRootSignature;

  const bool compute_pipeline =
      std::any_of(shaders.begin(), shaders.end(), [](const auto &shader) {
        return shader.stage == PipelineShaderStage::Compute;
      });

  for (const auto &shader : shaders) {
    if (shader.kind() == PipelineShaderBytecodeKind::Dxil)
      return NativeShaderAbiEligibilityReason::ShaderAbiMismatch;
    if (shader.stage == PipelineShaderStage::Geometry)
      return NativeShaderAbiEligibilityReason::UnsupportedGeometryPipeline;
    if (shader.stage == PipelineShaderStage::Hull ||
        shader.stage == PipelineShaderStage::Domain)
      return NativeShaderAbiEligibilityReason::UnsupportedTessellationPipeline;
  }

  for (const auto &parameter : root_signature->GetParameters()) {
    if (parameter.parameter_type !=
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      // The native compute root-argument ABI still observes live descriptor
      // state across queued submissions. Keep it on the bindless path until
      // dispatch consumes the same frozen root backing used by graphics.
      if (compute_pipeline)
        return NativeShaderAbiEligibilityReason::UnsupportedRootDescriptor;
      if (parameter.parameter_type ==
              D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS &&
          !parameter.constants.Num32BitValues)
        return NativeShaderAbiEligibilityReason::UnsupportedRootDescriptor;
      if (parameter.parameter_type !=
              D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS &&
          parameter.parameter_type != D3D12_ROOT_PARAMETER_TYPE_CBV &&
          parameter.parameter_type != D3D12_ROOT_PARAMETER_TYPE_SRV &&
          parameter.parameter_type != D3D12_ROOT_PARAMETER_TYPE_UAV)
        return NativeShaderAbiEligibilityReason::UnsupportedRootDescriptor;
      continue;
    }
    if (parameter.ranges.empty())
      return NativeShaderAbiEligibilityReason::UnsupportedDescriptorRange;

    std::optional<D3D12_DESCRIPTOR_HEAP_TYPE> heap_type;
    uint32_t running_offset = 0;
    for (const auto &range : parameter.ranges) {
      if (!range.descriptor_count || range.descriptor_count == UINT_MAX)
        return NativeShaderAbiEligibilityReason::UnsupportedDescriptorRange;
      const auto current_heap_type =
          range.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
              ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
              : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      if (heap_type && *heap_type != current_heap_type)
        return NativeShaderAbiEligibilityReason::UnsupportedDescriptorRange;
      heap_type = current_heap_type;
      const uint32_t offset =
          range.offset_in_descriptors_from_table_start == UINT_MAX
              ? running_offset
              : range.offset_in_descriptors_from_table_start;
      if (offset > UINT_MAX - range.descriptor_count)
        return NativeShaderAbiEligibilityReason::UnsupportedDescriptorRange;
      running_offset = offset + range.descriptor_count;
    }
  }

  for (const auto &shader : shaders) {
    const auto *cbuffers = shader.constantBufferInfo();
    for (uint32_t i = 0; cbuffers &&
                         i < shader.reflection().NumConstantBuffers;
         i++) {
      if (!NativeShaderArgumentHasSingleTableRange(*root_signature,
                                                   shader.stage,
                                                   cbuffers[i]))
        return NativeShaderAbiEligibilityReason::UnsupportedDescriptorRange;
    }
    const auto *arguments = shader.resourceArgumentInfo();
    for (uint32_t i = 0; arguments && i < shader.reflection().NumArguments;
         i++) {
      // Comparison sampling, implicit-LOD bias and LOD calculation require
      // typed texture/sampler handles. Reconstructing those handles from the
      // native descriptor table's raw qwords loses those operations on Metal,
      // while the typed bindless mirror preserves them.
      if (arguments[i].Type == SM50BindingType::SRV &&
          (arguments[i].Flags &
           (MTL_SM50_SHADER_ARGUMENT_TEXTURE_DEPTH |
            MTL_SM50_SHADER_ARGUMENT_TEXTURE_REQUIRES_TYPED_SAMPLER)))
        return NativeShaderAbiEligibilityReason::ShaderAbiMismatch;
      if (!NativeShaderArgumentHasSingleTableRange(*root_signature,
                                                   shader.stage,
                                                   arguments[i]))
        return NativeShaderAbiEligibilityReason::UnsupportedDescriptorRange;
    }
  }

  return NativeShaderAbiEligibilityReason::None;
}

DXMT12_MTL4_SHADER_ABI_VERSION
PsoShaderAbiVersion(const std::vector<PipelineDxilShader> &shaders,
                    const RootSignature *root_signature) {
  if (GetNativeShaderAbiEligibilityImpl(shaders, root_signature) ==
      NativeShaderAbiEligibilityReason::None)
    return DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE;
  return DXMT12_MTL4_SHADER_ABI_BINDLESS_MIRROR;
}

bool
ShaderAbiUsesBindlessMirror(
    DXMT12_MTL4_SHADER_ABI_VERSION shader_abi_version) {
  return shader_abi_version == DXMT12_MTL4_SHADER_ABI_BINDLESS_MIRROR;
}

bool
CreateMetalGraphicsPipeline(IMTLD3D12Device *device,
                            std::vector<PipelineDxilShader> &shaders,
                            const PipelineGraphicsState &state,
                            const RootSignature *root_signature,
                            std::string_view shader_cache_key,
                            PipelineMetalGraphicsState &out,
                            uint64_t demote_msaa_srv_mask_lo = 0,
                            uint64_t demote_msaa_srv_mask_hi = 0) {
  auto *vs = const_cast<PipelineDxilShader *>(
      FindShader(shaders, PipelineShaderStage::Vertex));
  auto *ps = const_cast<PipelineDxilShader *>(
      FindShader(shaders, PipelineShaderStage::Pixel));
  auto *gs = const_cast<PipelineDxilShader *>(
      FindShader(shaders, PipelineShaderStage::Geometry));
  auto *hs = const_cast<PipelineDxilShader *>(
      FindShader(shaders, PipelineShaderStage::Hull));
  auto *ds = const_cast<PipelineDxilShader *>(
      FindShader(shaders, PipelineShaderStage::Domain));
  if (!vs)
    return false;

  SM50_SHADER_COMMON_DATA common = {};
  common.type = SM50_SHADER_COMMON;
  common.metal_version = GetMetalVersion(device);
  common.flags = GetShaderFlags();
  common.next = nullptr;

  // Table-only PSOs use the direct heap ABI. Root payload, static sampler,
  // geometry and tessellation cases use the bindless mirror ABI. DXIL and DXBC
  // use the same selection and neither path emits packed descriptor tables.
  const auto shader_abi_version =
      PsoShaderAbiVersion(shaders, root_signature);
  const bool pso_bindless = ShaderAbiUsesBindlessMirror(shader_abi_version);
  const bool pso_native =
      shader_abi_version == DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE;
  SM50_SHADER_BINDLESS_MIRROR_DATA bindless_node = {};
  bindless_node.type = SM50_SHADER_BINDLESS_MIRROR;
  bindless_node.enabled = pso_bindless;
  bindless_node.next = &common;
  DXMT12_MTL4_NATIVE_DESCRIPTOR_ABI_DATA native_abi_node = {};
  native_abi_node.type = SM50_SHADER_DXMT12_NATIVE_DESCRIPTOR_ABI;
  native_abi_node.version = shader_abi_version;
  native_abi_node.enabled =
      shader_abi_version == DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE;
  native_abi_node.next = pso_bindless
                             ? reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&bindless_node)
                             : reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&common);
  auto *base_shader_args =
      reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(
          &native_abi_node);

  std::vector<SM50_IA_INPUT_ELEMENT> input_elements;
  uint32_t slot_mask = 0;
  if (!BuildInputElements(device, state, vs, input_elements, slot_mask))
    return false;

  std::array<WMTPixelFormat, 8> rtv_formats = {};
  WMTPixelFormat depth_format = WMTPixelFormatInvalid;
  WMTPixelFormat stencil_format = WMTPixelFormatInvalid;
  if (!ValidateGraphicsRenderFormats(device, state, rtv_formats,
                                     depth_format, stencil_format))
    return false;
  FormatCapabilityInspector format_capabilities;
  format_capabilities.Inspect(device->GetMTLDevice());

  SM50_SHADER_IA_INPUT_LAYOUT_DATA ia_layout = {};
  ia_layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
  ia_layout.next = base_shader_args;
  ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
  ia_layout.slot_mask = slot_mask;
  ia_layout.num_elements = uint32_t(input_elements.size());
  ia_layout.elements = input_elements.empty() ? nullptr : input_elements.data();

  SM50_SHADER_DIAG_FORCE_FULLSCREEN_POSITION_DATA diag_force_fullscreen = {};
  diag_force_fullscreen.type = SM50_SHADER_DIAG_FORCE_FULLSCREEN_POSITION;
  diag_force_fullscreen.next = &ia_layout;
  diag_force_fullscreen.enabled = true;
  auto *vs_args =
      reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&ia_layout);
  if (DiagForceFullscreenPositionForShader(*vs)) {
    vs_args = reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(
        &diag_force_fullscreen);
    WARN("D3D12 diagnostic: forcing fullscreen VS position for shader ",
         PipelineShaderDigest(*vs), " pso=", shader_cache_key);
  }

  const auto vs_name = BuildFunctionName("vs", shader_cache_key);
  for (const auto &shader : shaders)
    DebugLogDxilShaderInfo(shader_cache_key, shader);
  D3D12DumpPipelineShaders("graphics", shader_cache_key, shaders, &state,
                           nullptr);

  uint32_t unorm_output_reg_mask = 0;
  for (UINT i = 0; i < state.desc.NumRenderTargets; i++) {
    if (IsUnorm8RenderTargetFormat(rtv_formats[i]))
      unorm_output_reg_mask |= 1u << i;
  }
  const uint32_t pixel_shader_render_target_mask =
      ps ? ps->reflection().PSValidRenderTargets : 0;
  const bool metal_dual_source_blending = CanLowerD3D12DualSourceBlending(
      state, pixel_shader_render_target_mask);
  if (ps) {
    SM50_SHADER_PSO_PIXEL_SHADER_DATA ps_args = {};
    ps_args.type = SM50_SHADER_PSO_PIXEL_SHADER;
    ps_args.next = base_shader_args;
    ps_args.sample_mask = state.desc.SampleMask;
    ps_args.dual_source_blending = metal_dual_source_blending;
    ps_args.disable_depth_output = state.desc.DSVFormat == DXGI_FORMAT_UNKNOWN;
    ps_args.unorm_output_reg_mask = unorm_output_reg_mask;
    ps_args.demote_msaa_srv_mask_lo = demote_msaa_srv_mask_lo;
    ps_args.demote_msaa_srv_mask_hi = demote_msaa_srv_mask_hi;
    ps_args.tessellation_primitive_id = hs && ds;

    const auto ps_name = BuildFunctionName("ps", shader_cache_key);
    if (!CompileMetalFunction(device, *ps, ps_name.c_str(),
                              reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&ps_args),
                              out.pixel, shader_abi_version))
      return false;
  }

  out.pixel_shader_demote_msaa_srv_mask_lo = demote_msaa_srv_mask_lo;
  out.pixel_shader_demote_msaa_srv_mask_hi = demote_msaa_srv_mask_hi;

  if (hs || ds || state.desc.PrimitiveTopologyType ==
                    D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) {
    if (!hs || !ds) {
      // TODO(d3d12): patch topology without a complete HS/DS pair cannot be lowered.
      WARN("D3D12PipelineState: incomplete tessellation shader pair hasHS=",
           bool(hs), " hasDS=", bool(ds));
      return false;
    }
    if (gs && gs->reflection().GeometryShader.GSPassThrough == ~0u) {
      // TODO(d3d12): lower full GS-after-DS pipelines through mesh shaders.
      WARN("D3D12PipelineState: tessellation with full geometry shader is unsupported");
      return false;
    }
    if (hs->reflection().Tessellator.OutputPrimitive ==
            MTL_TESSELLATOR_OUTPUT_LINE ||
        hs->reflection().Tessellator.OutputPrimitive ==
            MTL_TESSELLATOR_OUTPUT_POINT) {
      // TODO(d3d12): implement line/point tessellation primitive generation.
      WARN("D3D12PipelineState: unsupported tessellation output primitive ",
           uint32_t(hs->reflection().Tessellator.OutputPrimitive));
      return false;
    }

    auto max_potential_factor =
        ds->reflection().PostTessellator.MaxPotentialTessFactor;
    if (!device->GetMTLDevice().supportsFamily(WMTGPUFamilyApple9))
      max_potential_factor = std::min(8u, max_potential_factor);
    if (float(max_potential_factor) < hs->reflection().Tessellator.MaxFactor) {
      WARN("D3D12PipelineState: tessellation maxtessfactor(",
           hs->reflection().Tessellator.MaxFactor,
           ") exceeds mesh pipeline limit; clamping to ",
           max_potential_factor);
    }

    SM50_SHADER_PSO_TESSELLATOR_DATA pso_tess = {};
    pso_tess.type = SM50_SHADER_PSO_TESSELLATOR;
    pso_tess.next = base_shader_args;
    pso_tess.max_potential_tess_factor = max_potential_factor;

    SM50_SHADER_GS_PASS_THROUGH_DATA gs_passthrough = {};
    gs_passthrough.type = SM50_SHADER_GS_PASS_THROUGH;
    gs_passthrough.next = &pso_tess;
    gs_passthrough.DataEncoded =
        gs ? gs->reflection().GeometryShader.GSPassThrough : 0;
    gs_passthrough.RasterizationDisabled = false;

    const auto ds_name = BuildFunctionName("ds", shader_cache_key);
    if (!CompileTessellationPipelineDomainFunction(
            device, *hs, *ds, ds_name.c_str(),
            reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(
                &gs_passthrough),
            out.tessellation_domain, shader_abi_version))
      return false;

    auto create_tessellation_variant =
        [&](uint32_t variant, SM50_INDEX_BUFFER_FORMAT index_format,
            const char *prefix, WMT::Reference<WMT::RenderPipelineState> &pso) {
      ia_layout.index_buffer_format = index_format;
      ia_layout.next = &pso_tess;
      const auto hs_name = BuildFunctionName(prefix, shader_cache_key);
      if (!CompileTessellationPipelineHullFunction(
              device, *vs, *hs, hs_name.c_str(),
              reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(
                  &ia_layout),
              out.tessellation_vertex_hull[variant], shader_abi_version))
        return false;

      WMTMeshRenderPipelineInfo info = {};
      WMT::InitializeMeshRenderPipelineInfo(info);
      SetMetalPsoDebugLabel(info, prefix, shader_cache_key);
      info.object_function = out.tessellation_vertex_hull[variant].function;
      info.mesh_function = out.tessellation_domain.function;
      info.fragment_function = out.pixel.function;
      info.rasterization_enabled = true;
      info.raster_sample_count = state.desc.SampleDesc.Count;
      if (!device->GetMTLDevice().supportsFamily(WMTGPUFamilyApple7))
        info.payload_memory_length = 16384;
      info.immutable_object_buffers =
          BufferBindingBit(16) | BufferBindingBit(21) |
          BindlessImmutableBufferMask(*vs, 23, 24, 25, 26) |
          BindlessImmutableBufferMask(*hs);
      info.immutable_mesh_buffers = BindlessImmutableBufferMask(*ds);
      info.immutable_fragment_buffers =
          ps ? BindlessImmutableBufferMask(*ps) : 0;
      info.mesh_tgsize_is_multiple_of_sgwidth = true;
      info.object_tgsize_is_multiple_of_sgwidth = true;

      for (UINT i = 0; i < state.desc.NumRenderTargets; i++) {
        if (state.desc.RTVFormats[i] != DXGI_FORMAT_UNKNOWN)
          info.colors[i].pixel_format = rtv_formats[i];
      }
      ApplyBlendState(info, state.desc.BlendState, state.desc.NumRenderTargets,
                      pixel_shader_render_target_mask,
                      metal_dual_source_blending,
                      format_capabilities);

      if (state.desc.DSVFormat != DXGI_FORMAT_UNKNOWN) {
        info.depth_pixel_format = depth_format;
        info.stencil_pixel_format = stencil_format;
      }

      std::array<obj_handle_t, 1> lookup_archives = {};
      const bool archive_attached =
          AttachPSOBinaryArchive(device, info, lookup_archives);
      WMT::Reference<WMT::Error> error;
      uint64_t compile_wait_us = 0;
      auto create_pipeline = [&] {
        pso = device->GetMTLDevice().newRenderPipelineStateAndGetStats(
            info, error, &compile_wait_us);
      };
      if (archive_attached) {
        std::lock_guard lock(device->GetPSOBinaryArchiveMutex());
        create_pipeline();
      } else {
        create_pipeline();
      }
      dxmt::perf::recordPsoCompileWaitTime(
          dxmt::perf::currentFrameStatistics(),
          std::chrono::microseconds(compile_wait_us));
      if (error || !pso) {
        WARN("D3D12PipelineState: failed to create Metal tessellation PSO: ",
             error ? error.description().getUTF8String() : "unknown error");
        return false;
      }
      if (archive_attached)
        device->NotePSOBinaryArchivePipelineCreated();
      return true;
    };

    if (!create_tessellation_variant(0, SM50_INDEX_BUFFER_FORMAT_NONE, "vshs",
                                     out.pso) ||
        !create_tessellation_variant(1, SM50_INDEX_BUFFER_FORMAT_UINT16,
                                     "vshs_i16", out.tessellation_pso_u16) ||
        !create_tessellation_variant(2, SM50_INDEX_BUFFER_FORMAT_UINT32,
                                     "vshs_i32", out.tessellation_pso_u32))
      return false;

    out.use_tessellation = true;
    out.tess_num_output_control_point_element =
        hs->reflection().NumOutputElement;
    out.tess_threads_per_patch = hs->reflection().ThreadsPerPatch;
    BuildRasterizerCommand(state.desc.RasterizerState, out.rasterizer);
    out.depth_stencil =
        CreateDepthStencilState(device, state.desc.DepthStencilState);
    return true;
  }

  if (gs) {
    auto create_geometry_variant =
        [&](bool strip_topology, PipelineMetalShader &object_shader,
            PipelineMetalShader &mesh_shader,
            WMT::Reference<WMT::RenderPipelineState> &pso) {
      SM50_SHADER_PSO_GEOMETRY_SHADER_DATA geometry_args = {};
      geometry_args.type = SM50_SHADER_PSO_GEOMETRY_SHADER;
      geometry_args.next = &ia_layout;
      geometry_args.strip_topology = strip_topology;

      const auto vsgs_name = BuildFunctionName(
          strip_topology ? "vsgs_strip" : "vsgs", shader_cache_key);
      if (!CompileGeometryPipelineVertexFunction(
              device, *vs, *gs, vsgs_name.c_str(),
              reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(
                  &geometry_args),
              object_shader, shader_abi_version))
        return false;

      geometry_args.next = base_shader_args;
      const auto gs_name = BuildFunctionName(
          strip_topology ? "gs_strip" : "gs", shader_cache_key);
      if (!CompileGeometryPipelineGeometryFunction(
              device, *vs, *gs, gs_name.c_str(),
              reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(
                  &geometry_args),
              mesh_shader, shader_abi_version))
        return false;

      WMTMeshRenderPipelineInfo info = {};
      WMT::InitializeMeshRenderPipelineInfo(info);
      SetMetalPsoDebugLabel(
          info, strip_topology ? "mesh-gs-strip" : "mesh-gs",
          shader_cache_key);
      info.object_function = object_shader.function;
      info.mesh_function = mesh_shader.function;
      info.fragment_function = out.pixel.function;
      info.payload_memory_length = 16256;
      info.rasterization_enabled = true;
      info.raster_sample_count = state.desc.SampleDesc.Count;
      info.immutable_object_buffers =
          BufferBindingBit(16) | BufferBindingBit(21) |
          BindlessImmutableBufferMask(*vs);
      info.immutable_mesh_buffers = BindlessImmutableBufferMask(*gs);
      info.immutable_fragment_buffers =
          ps ? BindlessImmutableBufferMask(*ps) : 0;

      for (UINT i = 0; i < state.desc.NumRenderTargets; i++) {
        if (state.desc.RTVFormats[i] != DXGI_FORMAT_UNKNOWN)
          info.colors[i].pixel_format = rtv_formats[i];
      }
      ApplyBlendState(info, state.desc.BlendState, state.desc.NumRenderTargets,
                      pixel_shader_render_target_mask,
                      metal_dual_source_blending,
                      format_capabilities);

      if (state.desc.DSVFormat != DXGI_FORMAT_UNKNOWN) {
        info.depth_pixel_format = depth_format;
        info.stencil_pixel_format = stencil_format;
      }

      std::array<obj_handle_t, 1> lookup_archives = {};
      const bool archive_attached =
          AttachPSOBinaryArchive(device, info, lookup_archives);
      WMT::Reference<WMT::Error> error;
      uint64_t compile_wait_us = 0;
      auto create_pipeline = [&] {
        pso = device->GetMTLDevice().newRenderPipelineStateAndGetStats(
            info, error, &compile_wait_us);
      };
      if (archive_attached) {
        std::lock_guard lock(device->GetPSOBinaryArchiveMutex());
        create_pipeline();
      } else {
        create_pipeline();
      }
      dxmt::perf::recordPsoCompileWaitTime(
          dxmt::perf::currentFrameStatistics(),
          std::chrono::microseconds(compile_wait_us));
      if (error || !pso) {
        WARN("D3D12PipelineState: failed to create Metal geometry PSO: ",
             error ? error.description().getUTF8String() : "unknown error");
        return false;
      }
      if (archive_attached)
        device->NotePSOBinaryArchivePipelineCreated();
      return true;
    };

    if (!create_geometry_variant(false, out.geometry_vertex, out.geometry,
                                 out.pso) ||
        !create_geometry_variant(true, out.geometry_strip_vertex,
                                 out.geometry_strip, out.strip_pso))
      return false;

    out.use_geometry = true;
    BuildRasterizerCommand(state.desc.RasterizerState, out.rasterizer);
    out.depth_stencil =
        CreateDepthStencilState(device, state.desc.DepthStencilState);
    return true;
  }

  if (!CompileMetalFunction(device, *vs, vs_name.c_str(), vs_args,
                            out.vertex, shader_abi_version))
    return false;

  WMTRenderPipelineInfo info = {};
  WMT::InitializeRenderPipelineInfo(info);
  SetMetalPsoDebugLabel(info, "graphics", shader_cache_key);
  info.vertex_function = out.vertex.function;
  info.fragment_function = out.pixel.function;
  info.rasterization_enabled = true;
  info.raster_sample_count = state.desc.SampleDesc.Count;
  info.input_primitive_topology =
      GetTopologyClass(state.desc.PrimitiveTopologyType);
  info.immutable_vertex_buffers = BufferBindingBit(16);
  info.immutable_fragment_buffers = 0;
  if (pso_bindless) {
    info.immutable_vertex_buffers |= BindlessImmutableBufferMask(*vs);
    if (ps)
      info.immutable_fragment_buffers |= BindlessImmutableBufferMask(*ps);
  } else if (pso_native) {
    info.immutable_vertex_buffers |= NativeDescriptorImmutableBufferMask(*vs);
    if (ps)
      info.immutable_fragment_buffers |=
          NativeDescriptorImmutableBufferMask(*ps);
  } else {
    info.immutable_vertex_buffers |=
        BufferBindingBit(29) | BufferBindingBit(30);
    info.immutable_fragment_buffers |=
        BufferBindingBit(29) | BufferBindingBit(30);
  }

  for (UINT i = 0; i < state.desc.NumRenderTargets; i++) {
    if (state.desc.RTVFormats[i] != DXGI_FORMAT_UNKNOWN)
      info.colors[i].pixel_format = rtv_formats[i];
  }
  ApplyBlendState(info, state.desc.BlendState, state.desc.NumRenderTargets,
                  pixel_shader_render_target_mask,
                  metal_dual_source_blending,
                  format_capabilities);

  if (state.desc.DSVFormat != DXGI_FORMAT_UNKNOWN) {
    info.depth_pixel_format = depth_format;
    info.stencil_pixel_format = stencil_format;
  }

  std::array<obj_handle_t, 1> lookup_archives = {};
  const bool archive_attached =
      AttachPSOBinaryArchive(device, info, lookup_archives);
  WMT::Reference<WMT::Error> error;
  uint64_t compile_wait_us = 0;
  auto create_pipeline = [&] {
    out.pso = device->GetMTLDevice().newRenderPipelineStateAndGetStats(
        info, error, &compile_wait_us);
  };
  if (archive_attached) {
    std::lock_guard lock(device->GetPSOBinaryArchiveMutex());
    create_pipeline();
  } else {
    create_pipeline();
  }
  dxmt::perf::recordPsoCompileWaitTime(
      dxmt::perf::currentFrameStatistics(),
      std::chrono::microseconds(compile_wait_us));
  if (error || !out.pso) {
    WARN("D3D12PipelineState: failed to create Metal graphics PSO: ",
         error ? error.description().getUTF8String() : "unknown error");
    return false;
  }
  if (archive_attached)
    device->NotePSOBinaryArchivePipelineCreated();
  BuildRasterizerCommand(state.desc.RasterizerState, out.rasterizer);
  out.depth_stencil =
      CreateDepthStencilState(device, state.desc.DepthStencilState);

  return true;
}

bool
CreateMetalComputePipeline(IMTLD3D12Device *device,
                           std::vector<PipelineDxilShader> &shaders,
                           const RootSignature *root_signature,
                           std::string_view shader_cache_key,
                           PipelineMetalComputeState &out) {
  auto *cs = const_cast<PipelineDxilShader *>(
      FindShader(shaders, PipelineShaderStage::Compute));
  if (!cs)
    return false;

  SM50_SHADER_COMMON_DATA common = {};
  common.type = SM50_SHADER_COMMON;
  common.metal_version = GetMetalVersion(device);
  common.flags = GetShaderFlags();
  common.next = nullptr;

  // See CreateMetalGraphicsPipeline for the shared native eligibility policy.
  const auto shader_abi_version =
      PsoShaderAbiVersion(shaders, root_signature);
  const bool pso_bindless = ShaderAbiUsesBindlessMirror(shader_abi_version);
  const bool pso_native =
      shader_abi_version == DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE;
  SM50_SHADER_BINDLESS_MIRROR_DATA bindless_node = {};
  bindless_node.type = SM50_SHADER_BINDLESS_MIRROR;
  bindless_node.enabled = pso_bindless;
  bindless_node.next = &common;
  DXMT12_MTL4_NATIVE_DESCRIPTOR_ABI_DATA native_abi_node = {};
  native_abi_node.type = SM50_SHADER_DXMT12_NATIVE_DESCRIPTOR_ABI;
  native_abi_node.version = shader_abi_version;
  native_abi_node.enabled =
      shader_abi_version == DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE;
  native_abi_node.next = pso_bindless
                             ? reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&bindless_node)
                             : reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&common);

  const auto cs_name = BuildFunctionName("cs", shader_cache_key);
  D3D12DumpPipelineShaders("compute", shader_cache_key, shaders, nullptr,
                           nullptr);
  auto *compile_args =
      reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(
          &native_abi_node);
  auto compile_compute = [&](bool read_persistent_cache) {
    return CompileMetalFunction(device, *cs, cs_name.c_str(), compile_args,
                                out.compute, shader_abi_version,
                                read_persistent_cache);
  };
  if (!compile_compute(/*read_persistent_cache=*/true))
    return false;

  WMTComputePipelineInfo info = {};
  WMT::InitializeComputePipelineInfo(info);
  info.compute_function = out.compute.function;
  info.immutable_buffers =
      pso_bindless
          ? BindlessImmutableBufferMask(*cs)
          : pso_native
                ? NativeDescriptorImmutableBufferMask(*cs)
                : (BufferBindingBit(29) | BufferBindingBit(30));
  const auto tgx = std::max<uint32_t>(1, cs->reflection().ThreadgroupSize[0]);
  const auto tgy = std::max<uint32_t>(1, cs->reflection().ThreadgroupSize[1]);
  const auto tgz = std::max<uint32_t>(1, cs->reflection().ThreadgroupSize[2]);
  info.tgsize_is_multiple_of_sgwidth = ((tgx * tgy * tgz) % 32) == 0;

  std::array<obj_handle_t, 1> lookup_archives = {};
  const bool archive_attached =
      AttachPSOBinaryArchive(device, info, lookup_archives);

  auto create_pso = [&](std::string &error_desc) {
    if (ShouldInjectPipelineFault(
            "DXMT_TEST_FAIL_METAL_COMPUTE_PIPELINE_AT",
            g_test_metal_compute_pipeline_occurrence)) {
      RecordInjectedPipelineFault("DXMT_TEST_FAIL_METAL_COMPUTE_PIPELINE_AT");
      error_desc = "injected Metal compute pipeline creation failure";
      return false;
    }
    WMT::Reference<WMT::Error> error;
    uint64_t compile_wait_us = 0;
    auto create_pipeline = [&] {
      out.pso = device->GetMTLDevice().newComputePipelineStateAndGetStats(
          info, error, &compile_wait_us);
    };
    if (archive_attached) {
      std::lock_guard lock(device->GetPSOBinaryArchiveMutex());
      create_pipeline();
    } else {
      create_pipeline();
    }
    dxmt::perf::recordPsoCompileWaitTime(
        dxmt::perf::currentFrameStatistics(),
        std::chrono::microseconds(compile_wait_us));
    if (error || !out.pso) {
      error_desc = error ? error.description().getUTF8String()
                         : "unknown error";
      return false;
    }
    if (archive_attached)
      device->NotePSOBinaryArchivePipelineCreated();
    error_desc.clear();
    return true;
  };
  auto log_failure = [&](std::string_view error_desc) {
    WARN("D3D12PipelineState: failed to create Metal compute PSO: ",
         error_desc, " shaderKey=", shader_cache_key, " function=", cs_name,
         " bindless=", pso_bindless ? 1 : 0,
         " persistentCacheHit=", out.compute.persistent_cache_hit ? 1 : 0,
         " persistentCacheKey=", out.compute.persistent_cache_key,
         " immutableBuffers=0x", std::hex, info.immutable_buffers, std::dec,
         " threadgroup=", tgx, "x", tgy, "x", tgz,
         " tgMultipleOfSgWidth=", info.tgsize_is_multiple_of_sgwidth ? 1 : 0);
  };

  std::string error_desc;
  if (!create_pso(error_desc)) {
    const bool was_persistent_cache_hit = out.compute.persistent_cache_hit;
    const auto persistent_cache_key = out.compute.persistent_cache_key;
    log_failure(error_desc);

    if (!was_persistent_cache_hit)
      return false;

    WARN("D3D12PipelineState: retrying compute PSO with fresh AIR after "
         "persistent cache materialization failure shaderKey=",
         shader_cache_key, " persistentCacheKey=", persistent_cache_key);
    out.compute = {};
    out.pso = {};
    if (!compile_compute(/*read_persistent_cache=*/false))
      return false;
    info.compute_function = out.compute.function;
    if (!create_pso(error_desc)) {
      log_failure(error_desc);
      return false;
    }
    INFO("D3D12PipelineState: recovered compute PSO by refreshing persistent "
         "AIR cache shaderKey=",
         shader_cache_key, " persistentCacheKey=", persistent_cache_key);
  }

  out.threadgroup_size = {tgx, tgy, tgz};
  RegisterComputePipelineDiagInfo(static_cast<obj_handle_t>(out.pso),
                                  std::string(shader_cache_key));
  return true;
}

void
FixGraphicsStatePointers(PipelineGraphicsState &state,
                         const std::vector<PipelineDxilShader> &shaders) {
  state.desc.VS = ShaderBytecodeView(shaders, PipelineShaderStage::Vertex);
  state.desc.PS = ShaderBytecodeView(shaders, PipelineShaderStage::Pixel);
  state.desc.DS = ShaderBytecodeView(shaders, PipelineShaderStage::Domain);
  state.desc.HS = ShaderBytecodeView(shaders, PipelineShaderStage::Hull);
  state.desc.GS = ShaderBytecodeView(shaders, PipelineShaderStage::Geometry);

  for (size_t i = 0; i < state.input_elements.size(); i++) {
    state.input_elements[i].SemanticName =
        state.input_element_semantic_names[i].empty()
            ? nullptr
            : state.input_element_semantic_names[i].c_str();
  }
  state.desc.InputLayout.NumElements =
      UINT(state.input_elements.size());
  state.desc.InputLayout.pInputElementDescs =
      state.input_elements.empty() ? nullptr : state.input_elements.data();

  for (size_t i = 0; i < state.stream_output_entries.size(); i++) {
    state.stream_output_entries[i].SemanticName =
        state.stream_output_semantic_names[i].empty()
            ? nullptr
            : state.stream_output_semantic_names[i].c_str();
  }
  state.desc.StreamOutput.NumEntries =
      UINT(state.stream_output_entries.size());
  state.desc.StreamOutput.pSODeclaration =
      state.stream_output_entries.empty()
          ? nullptr
          : state.stream_output_entries.data();
  state.desc.StreamOutput.NumStrides =
      UINT(state.stream_output_strides.size());
  state.desc.StreamOutput.pBufferStrides =
      state.stream_output_strides.empty()
          ? nullptr
          : state.stream_output_strides.data();

  state.desc.CachedPSO.pCachedBlob =
      state.cached_pso.empty() ? nullptr : state.cached_pso.data();
  state.desc.CachedPSO.CachedBlobSizeInBytes = state.cached_pso.size();
}

void
FixComputeStatePointers(PipelineComputeState &state,
                        const std::vector<PipelineDxilShader> &shaders) {
  state.desc.CS = ShaderBytecodeView(shaders, PipelineShaderStage::Compute);
  state.desc.CachedPSO.pCachedBlob =
      state.cached_pso.empty() ? nullptr : state.cached_pso.data();
  state.desc.CachedPSO.CachedBlobSizeInBytes = state.cached_pso.size();
}

HRESULT
CloneGraphicsState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc,
                   PipelineGraphicsState &state) {
  if (desc.NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
    return WARN_E_INVALIDARG(__func__);
  const bool has_depth_stencil = desc.DSVFormat != DXGI_FORMAT_UNKNOWN;
  const bool has_attachments =
      desc.NumRenderTargets != 0 || has_depth_stencil;
  if (desc.SampleDesc.Count == 0 && has_attachments)
    return WARN_E_INVALIDARG(__func__);
  if (desc.NodeMask > 1) {
    WARN("D3D12PipelineState: multi-node graphics PSOs are unsupported");
    return WARN_E_INVALIDARG(__func__);
  }
  if ((desc.StreamOutput.NumEntries &&
       !desc.StreamOutput.pSODeclaration) ||
      (desc.StreamOutput.NumStrides &&
       !desc.StreamOutput.pBufferStrides)) {
    WARN("D3D12PipelineState: stream output descriptor has a null array");
    return WARN_E_INVALIDARG(__func__);
  }
  if (desc.StreamOutput.NumEntries || desc.StreamOutput.NumStrides) {
    if (!(GetRootSignatureFlags(desc.pRootSignature) &
          D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT)) {
      WARN("D3D12PipelineState: stream output requires root signature support");
      return WARN_E_INVALIDARG(__func__);
    }
    // DX12 stream-output lowering is not implemented. Reject the PSO at its
    // creation boundary instead of accepting it and silently discarding
    // SOSetTargets during replay.
    WARN("D3D12PipelineState: stream output is unsupported");
    return E_NOTIMPL;
  }
  for (UINT i = desc.NumRenderTargets; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
    if (desc.RTVFormats[i] != DXGI_FORMAT_UNKNOWN)
      return WARN_E_INVALIDARG(__func__);
  }
  for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
    const auto &rt = desc.BlendState.RenderTarget[i];
    if (rt.BlendEnable && rt.LogicOpEnable)
      return WARN_E_INVALIDARG(__func__);
    if (rt.LogicOpEnable && desc.BlendState.IndependentBlendEnable)
      return WARN_E_INVALIDARG(__func__);
#ifdef DXMT_NO_PRIVATE_API
    if (rt.LogicOpEnable) {
      WARN("D3D12PipelineState: output-merger logic operations are unsupported");
      return E_NOTIMPL;
    }
#endif
  }
  if (desc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED) {
    WARN("D3D12PipelineState: unsupported primitive topology type ",
         uint32_t(desc.PrimitiveTopologyType));
    return WARN_E_INVALIDARG(__func__);
  }
  if (desc.SampleDesc.Quality != 0) {
    WARN("D3D12PipelineState: MSAA quality levels are unsupported");
    return WARN_E_INVALIDARG(__func__);
  }
  if ((desc.HS.BytecodeLength || desc.HS.pShaderBytecode ||
       desc.DS.BytecodeLength || desc.DS.pShaderBytecode) &&
      (!HasBytecode(desc.HS) || !HasBytecode(desc.DS))) {
    // TODO(d3d12): diagnose partial tessellation PSOs more precisely.
    WARN("D3D12PipelineState: incomplete tessellation shader pair hasHS=",
         HasBytecode(desc.HS), " hasDS=", HasBytecode(desc.DS));
    return WARN_E_INVALIDARG(__func__);
  }
  if (!HasBytecode(desc.VS))
    return WARN_E_INVALIDARG(__func__);
  if (desc.InputLayout.NumElements && !desc.InputLayout.pInputElementDescs)
    return WARN_E_INVALIDARG(__func__);

  state = {};
  state.desc = desc;
  if (state.desc.SampleDesc.Count == 0) {
    if (D3D12AttachmentlessPsoSampleCountWarnShouldLog())
      WARN("D3D12PipelineState: normalizing attachment-less PSO sample count "
           "from 0 to 1");
    state.desc.SampleDesc.Count = 1;
  }

  state.input_elements.reserve(desc.InputLayout.NumElements);
  state.input_element_semantic_names.reserve(desc.InputLayout.NumElements);
  for (UINT i = 0; i < desc.InputLayout.NumElements; i++) {
    const auto &element = desc.InputLayout.pInputElementDescs[i];
    if (!element.SemanticName)
      return WARN_E_INVALIDARG(__func__);
    state.input_element_semantic_names.emplace_back(element.SemanticName);
    state.input_elements.push_back(element);
  }

  state.stream_output_entries.reserve(desc.StreamOutput.NumEntries);
  state.stream_output_semantic_names.reserve(desc.StreamOutput.NumEntries);
  for (UINT i = 0; i < desc.StreamOutput.NumEntries; i++) {
    const auto &entry = desc.StreamOutput.pSODeclaration[i];
    state.stream_output_semantic_names.emplace_back(
        entry.SemanticName ? entry.SemanticName : "");
    state.stream_output_entries.push_back(entry);
  }

  if (desc.StreamOutput.NumStrides) {
    state.stream_output_strides.assign(
        desc.StreamOutput.pBufferStrides,
        desc.StreamOutput.pBufferStrides + desc.StreamOutput.NumStrides);
  }

  if (!CopyCachedBlob(desc.CachedPSO, state.cached_pso))
    return WARN_E_INVALIDARG(__func__);

  return S_OK;
}

HRESULT
CloneComputeState(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc,
                  PipelineComputeState &state) {
  if (!HasBytecode(desc.CS))
    return WARN_E_INVALIDARG(__func__);

  state = {};
  state.desc = desc;
  if (!CopyCachedBlob(desc.CachedPSO, state.cached_pso))
    return WARN_E_INVALIDARG(__func__);
  return S_OK;
}

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
size_t
AlignStreamOffset(size_t value) {
  const size_t alignment = sizeof(void *);
  return (value + alignment - 1) & ~(alignment - 1);
}

size_t
AlignStreamOffset(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

size_t
PipelineStreamPayloadSize(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) {
  switch (type) {
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
    return sizeof(ID3D12RootSignature *);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    return sizeof(D3D12_SHADER_BYTECODE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
    return sizeof(D3D12_STREAM_OUTPUT_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
    return sizeof(D3D12_BLEND_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
    return sizeof(UINT);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
    return sizeof(D3D12_RASTERIZER_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
    return sizeof(D3D12_DEPTH_STENCIL_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
    return sizeof(D3D12_DEPTH_STENCIL_DESC1);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
    return sizeof(D3D12_INPUT_LAYOUT_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
    return sizeof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
    return sizeof(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
    return sizeof(D3D12_RT_FORMAT_ARRAY);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
    return sizeof(DXGI_FORMAT);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
    return sizeof(DXGI_SAMPLE_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
    return sizeof(UINT);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
    return sizeof(D3D12_CACHED_PIPELINE_STATE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
    return sizeof(D3D12_PIPELINE_STATE_FLAGS);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
    return sizeof(D3D12_VIEW_INSTANCING_DESC);
  default:
    return 0;
  }
}

size_t
PipelineStreamPayloadAlignment(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) {
  switch (type) {
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
    return alignof(ID3D12RootSignature *);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    return alignof(D3D12_SHADER_BYTECODE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
    return alignof(D3D12_STREAM_OUTPUT_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
    return alignof(D3D12_BLEND_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
    return alignof(UINT);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
    return alignof(D3D12_RASTERIZER_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
    return alignof(D3D12_DEPTH_STENCIL_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
    return alignof(D3D12_DEPTH_STENCIL_DESC1);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
    return alignof(D3D12_INPUT_LAYOUT_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
    return alignof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
    return alignof(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
    return alignof(D3D12_RT_FORMAT_ARRAY);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
    return alignof(DXGI_FORMAT);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
    return alignof(DXGI_SAMPLE_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
    return alignof(UINT);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
    return alignof(D3D12_CACHED_PIPELINE_STATE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
    return alignof(D3D12_PIPELINE_STATE_FLAGS);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
    return alignof(D3D12_VIEW_INSTANCING_DESC);
  default:
    return 0;
  }
}

HRESULT
FailPipelineStreamParse(const char *reason,
                        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type,
                        size_t offset, HRESULT hr = E_INVALIDARG) {
  if (D3D12PipelineDiagShouldLog())
    INFO("D3D12 diagnostic: pipeline stream parse failed",
         " reason=", reason,
         " type=", uint32_t(type),
         " offset=", offset,
         " hr=0x", std::hex, uint32_t(hr), std::dec);
  return hr;
}

HRESULT
ParsePipelineStateStream(const D3D12_PIPELINE_STATE_STREAM_DESC &stream,
                         D3D12_GRAPHICS_PIPELINE_STATE_DESC &graphics,
                         D3D12_COMPUTE_PIPELINE_STATE_DESC &compute,
                         bool &has_compute_shader) {
  if (!stream.pPipelineStateSubobjectStream || !stream.SizeInBytes)
    return FailPipelineStreamParse("empty stream",
                                   D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MAX_VALID,
                                   0);

  graphics = {};
  graphics.SampleMask = UINT_MAX;
  graphics.SampleDesc.Count = 1;
  graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  compute = {};
  has_compute_shader = false;
  bool seen_subobjects[D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING + 1] = {};

  const auto *bytes =
      static_cast<const uint8_t *>(stream.pPipelineStateSubobjectStream);
  size_t offset = 0;
  while (offset < stream.SizeInBytes) {
    if (stream.SizeInBytes - offset < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE))
      return FailPipelineStreamParse("truncated type",
                                     D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MAX_VALID,
                                     offset);

    auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *>(
        bytes + offset);
    const size_t type_offset = offset;
    offset += sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE);

    const size_t payload_size = PipelineStreamPayloadSize(type);
    const size_t payload_alignment = PipelineStreamPayloadAlignment(type);
    if (!payload_size || !payload_alignment)
      return FailPipelineStreamParse("unknown type", type, type_offset);
    offset = AlignStreamOffset(offset, payload_alignment);
    if (offset > stream.SizeInBytes ||
        stream.SizeInBytes - offset < payload_size)
      return FailPipelineStreamParse("truncated payload", type, type_offset);
    const auto type_index = static_cast<uint32_t>(type);
    if (type_index >= std::size(seen_subobjects) || seen_subobjects[type_index])
      return FailPipelineStreamParse("duplicate subobject", type, type_offset);
    seen_subobjects[type_index] = true;

    const void *payload = bytes + offset;
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
      graphics.pRootSignature =
          *static_cast<ID3D12RootSignature *const *>(payload);
      compute.pRootSignature = graphics.pRootSignature;
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
      graphics.VS = *static_cast<const D3D12_SHADER_BYTECODE *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
      graphics.PS = *static_cast<const D3D12_SHADER_BYTECODE *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
      graphics.DS = *static_cast<const D3D12_SHADER_BYTECODE *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
      graphics.HS = *static_cast<const D3D12_SHADER_BYTECODE *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
      graphics.GS = *static_cast<const D3D12_SHADER_BYTECODE *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
      compute.CS = *static_cast<const D3D12_SHADER_BYTECODE *>(payload);
      has_compute_shader |= HasBytecode(compute.CS);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
      graphics.StreamOutput =
          *static_cast<const D3D12_STREAM_OUTPUT_DESC *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
      graphics.BlendState = *static_cast<const D3D12_BLEND_DESC *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
      graphics.SampleMask = *static_cast<const UINT *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
      graphics.RasterizerState =
          *static_cast<const D3D12_RASTERIZER_DESC *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
      graphics.DepthStencilState =
          *static_cast<const D3D12_DEPTH_STENCIL_DESC *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
      const auto &depth_stencil =
          *static_cast<const D3D12_DEPTH_STENCIL_DESC1 *>(payload);
      graphics.DepthStencilState.DepthEnable = depth_stencil.DepthEnable;
      graphics.DepthStencilState.DepthWriteMask =
          depth_stencil.DepthWriteMask;
      graphics.DepthStencilState.DepthFunc = depth_stencil.DepthFunc;
      graphics.DepthStencilState.StencilEnable = depth_stencil.StencilEnable;
      graphics.DepthStencilState.StencilReadMask =
          depth_stencil.StencilReadMask;
      graphics.DepthStencilState.StencilWriteMask =
          depth_stencil.StencilWriteMask;
      graphics.DepthStencilState.FrontFace = depth_stencil.FrontFace;
      graphics.DepthStencilState.BackFace = depth_stencil.BackFace;
      if (depth_stencil.DepthBoundsTestEnable) {
        // TODO(d3d12): lower depth bounds once dynamic min/max depth-bounds
        // state is represented in the Metal command stream.
        WARN("D3D12PipelineState: depth bounds in PSO stream is unsupported");
        return E_NOTIMPL;
      }
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
      graphics.InputLayout =
          *static_cast<const D3D12_INPUT_LAYOUT_DESC *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
      graphics.IBStripCutValue =
          *static_cast<const D3D12_INDEX_BUFFER_STRIP_CUT_VALUE *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
      graphics.PrimitiveTopologyType =
          *static_cast<const D3D12_PRIMITIVE_TOPOLOGY_TYPE *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
      const auto &formats = *static_cast<const D3D12_RT_FORMAT_ARRAY *>(payload);
      if (formats.NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
        return WARN_E_INVALIDARG(__func__);
      graphics.NumRenderTargets = formats.NumRenderTargets;
      for (UINT i = 0; i < formats.NumRenderTargets; i++)
      graphics.RTVFormats[i] = formats.RTFormats[i];
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
      graphics.DSVFormat = *static_cast<const DXGI_FORMAT *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
      graphics.SampleDesc = *static_cast<const DXGI_SAMPLE_DESC *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
      graphics.NodeMask = *static_cast<const UINT *>(payload);
      compute.NodeMask = graphics.NodeMask;
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
      graphics.CachedPSO =
          *static_cast<const D3D12_CACHED_PIPELINE_STATE *>(payload);
      compute.CachedPSO = graphics.CachedPSO;
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
      graphics.Flags = *static_cast<const D3D12_PIPELINE_STATE_FLAGS *>(payload);
      compute.Flags = graphics.Flags;
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
      const auto &view_instancing =
          *static_cast<const D3D12_VIEW_INSTANCING_DESC *>(payload);
      if (view_instancing.ViewInstanceCount)
        return FailPipelineStreamParse("view instancing unsupported", type,
                                       type_offset, E_NOTIMPL);
      break;
    }
    default:
      return FailPipelineStreamParse("unknown type", type, type_offset);
    }

    offset = AlignStreamOffset(offset + payload_size);
  }

  if (has_compute_shader &&
      (HasBytecode(graphics.VS) || HasBytecode(graphics.DS) ||
       HasBytecode(graphics.HS) || HasBytecode(graphics.GS) ||
       HasBytecode(graphics.PS)))
    return FailPipelineStreamParse("compute stream has incompatible shader",
                                   D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS,
                                   stream.SizeInBytes);

  return S_OK;
}
#endif

#ifdef __ID3D12PipelineLibrary_INTERFACE_DEFINED__
#ifdef __ID3D12PipelineLibrary1_INTERFACE_DEFINED__
using PipelineLibraryBase = ID3D12PipelineLibrary1;
#else
using PipelineLibraryBase = ID3D12PipelineLibrary;
#endif

using PipelineLibraryKey = std::string;

PipelineLibraryKey
PipelineLibraryNameKey(const WCHAR *name) {
  return name ? str::fromws(name) : PipelineLibraryKey();
}

class PipelineLibraryImpl final
    : public ComObjectWithInitialRef<PipelineLibraryBase> {
public:
  PipelineLibraryImpl(IMTLD3D12Device *device) : device_(device) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;

    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12PipelineLibrary)) {
      *object = ref(static_cast<ID3D12PipelineLibrary *>(
          static_cast<PipelineLibraryBase *>(this)));
      return S_OK;
    }
#ifdef __ID3D12PipelineLibrary1_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12PipelineLibrary1)) {
      *object = ref(static_cast<ID3D12PipelineLibrary1 *>(this));
      return S_OK;
    }
#endif

    if (logQueryInterfaceError(__uuidof(ID3D12PipelineLibrary), riid))
      WARN("D3D12PipelineLibrary: unknown interface query ",
           str::format(riid));

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                           void *data) override {
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                           const void *data) override {
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID guid, const IUnknown *data) override {
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  HRESULT STDMETHODCALLTYPE StorePipeline(const WCHAR *name,
                                          ID3D12PipelineState *pipeline) override {
    if (!name || !pipeline)
      return WARN_E_INVALIDARG(__func__);

    auto *state = dynamic_cast<PipelineState *>(pipeline);
    if (!state)
      return WARN_E_INVALIDARG(__func__);

    std::lock_guard lock(mutex_);
    pipelines_[PipelineLibraryNameKey(name)] = {
        .type = state->GetType(),
        .pipeline = pipeline,
    };
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  LoadGraphicsPipeline(const WCHAR *name,
                       const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                       REFIID riid, void **pipeline_state) override {
    InitReturnPtr(pipeline_state);
    if (!pipeline_state)
      return E_POINTER;
    if (!name)
      return WARN_E_INVALIDARG(__func__);

    auto key = PipelineLibraryNameKey(name);
    {
      std::lock_guard lock(mutex_);
      auto entry = pipelines_.find(key);
      if (entry != pipelines_.end()) {
        if (entry->second.type != PipelineStateType::Graphics)
          return WARN_E_INVALIDARG(__func__);
        return entry->second.pipeline->QueryInterface(riid, pipeline_state);
      }
    }

    HRESULT status = S_OK;
    auto pipeline = CreateGraphicsPipelineState(device_.ptr(), desc, &status);
    if (!pipeline)
      return status;

    std::lock_guard lock(mutex_);
    pipelines_[std::move(key)] = {
        .type = PipelineStateType::Graphics,
        .pipeline = pipeline,
    };
    return pipeline->QueryInterface(riid, pipeline_state);
  }

  HRESULT STDMETHODCALLTYPE
  LoadComputePipeline(const WCHAR *name,
                      const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                      REFIID riid, void **pipeline_state) override {
    InitReturnPtr(pipeline_state);
    if (!pipeline_state)
      return E_POINTER;
    if (!name)
      return WARN_E_INVALIDARG(__func__);

    auto key = PipelineLibraryNameKey(name);
    {
      std::lock_guard lock(mutex_);
      auto entry = pipelines_.find(key);
      if (entry != pipelines_.end()) {
        if (entry->second.type != PipelineStateType::Compute)
          return WARN_E_INVALIDARG(__func__);
        return entry->second.pipeline->QueryInterface(riid, pipeline_state);
      }
    }

    HRESULT status = S_OK;
    auto pipeline = CreateComputePipelineState(device_.ptr(), desc, &status);
    if (!pipeline)
      return status;

    std::lock_guard lock(mutex_);
    pipelines_[std::move(key)] = {
        .type = PipelineStateType::Compute,
        .pipeline = pipeline,
    };
    return pipeline->QueryInterface(riid, pipeline_state);
  }

  SIZE_T STDMETHODCALLTYPE GetSerializedSize() override {
    return 0;
  }

  HRESULT STDMETHODCALLTYPE Serialize(void *data,
                                      SIZE_T data_size_in_bytes) override {
    return data || data_size_in_bytes == 0 ? S_OK : E_INVALIDARG;
  }

#ifdef __ID3D12PipelineLibrary1_INTERFACE_DEFINED__
  HRESULT STDMETHODCALLTYPE LoadPipeline(
      const WCHAR *name, const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
      REFIID riid, void **pipeline_state) override {
    InitReturnPtr(pipeline_state);
    if (!pipeline_state)
      return E_POINTER;
    if (!name)
      return WARN_E_INVALIDARG(__func__);

    auto key = PipelineLibraryNameKey(name);
    {
      std::lock_guard lock(mutex_);
      auto entry = pipelines_.find(key);
      if (entry != pipelines_.end())
        return entry->second.pipeline->QueryInterface(riid, pipeline_state);
    }

    HRESULT status = S_OK;
    auto pipeline = CreatePipelineStateFromStream(device_.ptr(), desc, &status);
    if (!pipeline)
      return status;

    auto *state = dynamic_cast<PipelineState *>(pipeline.ptr());
    std::lock_guard lock(mutex_);
    pipelines_[std::move(key)] = {
        .type = state ? state->GetType() : PipelineStateType::Graphics,
        .pipeline = pipeline,
    };
    return pipeline->QueryInterface(riid, pipeline_state);
  }
#endif

private:
  struct Entry {
    PipelineStateType type = PipelineStateType::Graphics;
    Com<ID3D12PipelineState> pipeline;
  };

  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  std::string name_;
  std::mutex mutex_;
  std::unordered_map<PipelineLibraryKey, Entry> pipelines_;
};
#endif

class PipelineStateImpl final : public ComObjectWithInitialRef<ID3D12PipelineState>,
                                public PipelineState {
public:
  PipelineStateImpl(IMTLD3D12Device *device, PipelineStateType type,
                    ID3D12RootSignature *root_signature,
                    std::vector<PipelineDxilShader> &&shaders,
                    std::vector<PipelineSignatureLink> &&signature_links,
                    std::string &&shader_cache_key,
                    PipelineGraphicsState &&graphics_state,
                    PipelineComputeState &&compute_state)
      : cache_identity_(g_pipeline_state_cache_identity.fetch_add(
            1, std::memory_order_relaxed)),
        device_(device), type_(type),
        public_root_signature_(root_signature),
        root_signature_(GetDXMTRootSignature(root_signature)),
        shaders_(std::move(shaders)),
        signature_links_(std::move(signature_links)),
        graphics_state_(std::move(graphics_state)),
        compute_state_(std::move(compute_state)),
        shader_cache_key_(std::move(shader_cache_key)) {
    FixGraphicsStatePointers(graphics_state_, shaders_);
    FixComputeStatePointers(compute_state_, shaders_);
    cached_shader_blob_ =
        BuildCachedShaderBlob(type_, graphics_state_, compute_state_,
                              shader_cache_key_);
    // Same ABI selection over the same shaders the compile path uses, so the
    // runtime draw-path gate matches the AIR that was actually compiled.
    shader_abi_version_ =
        PsoShaderAbiVersion(shaders_, root_signature_.ptr());
    uses_bindless_mirror_ =
        ShaderAbiUsesBindlessMirror(shader_abi_version_);
  }

  ~PipelineStateImpl() {
    DestroyPipelineShaders(shaders_);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;

    *object = nullptr;

    if (riid == IID_DXMTPipelineStateDowncast) {
      // Internal non-RTTI downcast (no AddRef); see d3d12_pipeline.hpp.
      *object = static_cast<PipelineState *>(this);
      return S_OK;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12PipelineState)) {
      *object = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12PipelineState), riid))
      WARN("D3D12PipelineState: unknown interface query ", str::format(riid));

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                           void *data) override {
    if (guid == dxmt::d3d12::test::kPipelineNativeArtifactIdentityGuid) {
      using dxmt::d3d12::test::PipelineNativeArtifactIdentity;
      if (!data_size)
        return E_INVALIDARG;
      const UINT required = sizeof(PipelineNativeArtifactIdentity);
      if (!data) {
        *data_size = required;
        return S_OK;
      }
      if (*data_size < required) {
        *data_size = required;
        return DXGI_ERROR_MORE_DATA;
      }
      PipelineNativeArtifactIdentity identity = {};
      identity.artifact =
          reinterpret_cast<std::uintptr_t>(metal_artifact_.get());
      std::memcpy(data, &identity, required);
      *data_size = required;
      return S_OK;
    }
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                           const void *data) override {
    if (guid == dxmt::d3d12::test::kPipelineNativeArtifactIdentityGuid)
      return E_INVALIDARG;
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,
                                                   const IUnknown *data) override {
    if (guid == dxmt::d3d12::test::kPipelineNativeArtifactIdentityGuid)
      return E_INVALIDARG;
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob **blob) override {
    InitReturnPtr(blob);
    if (!blob)
      return E_POINTER;

    *blob = CreateBlob(cached_shader_blob_).takeOwnership();
    return S_OK;
  }

  IMTLD3D12Device *GetParentDevice() const override {
    return device_.ptr();
  }

  uint64_t GetCacheIdentity() const override { return cache_identity_; }

  PipelineStateType GetType() const override {
    return type_;
  }

  ID3D12RootSignature *GetRootSignature() const override {
    return public_root_signature_.ptr();
  }

  const std::vector<PipelineDxilShader> &GetDxilShaders() const override {
    return shaders_;
  }

  const std::vector<PipelineSignatureLink> &GetSignatureLinks() const override {
    return signature_links_;
  }

  const PipelineGraphicsState *GetGraphicsState() const override {
    return type_ == PipelineStateType::Graphics ? &graphics_state_ : nullptr;
  }

  const PipelineComputeState *GetComputeState() const override {
    return type_ == PipelineStateType::Compute ? &compute_state_ : nullptr;
  }

  const std::string &GetShaderCacheKey() const override {
    return shader_cache_key_;
  }

  bool MaterializeBaseArtifact() {
    auto *cache = device_->GetPipelineNativeArtifactCache();
    if (!cache)
      return false;

    if (type_ == PipelineStateType::Graphics) {
      metal_artifact_ = cache->Acquire(
          shader_cache_key_, dxmt::perf::PsoArtifactKind::Graphics, [&] {
            auto created = std::make_shared<PipelineNativeArtifact>(
                PipelineStateType::Graphics);
            if (!CreateMetalGraphicsPipeline(
                    device_.ptr(), shaders_, graphics_state_,
                    root_signature_.ptr(), shader_cache_key_,
                    created->graphics))
              return std::shared_ptr<PipelineNativeArtifact>();
            return created;
          });
      return metal_artifact_ &&
             metal_artifact_->type == PipelineStateType::Graphics &&
             metal_artifact_->graphics.pso;
    }

    if (type_ == PipelineStateType::Compute) {
      metal_artifact_ = cache->Acquire(
          shader_cache_key_, dxmt::perf::PsoArtifactKind::Compute, [&] {
            auto created = std::make_shared<PipelineNativeArtifact>(
                PipelineStateType::Compute);
            if (!CreateMetalComputePipeline(
                    device_.ptr(), shaders_, root_signature_.ptr(),
                    shader_cache_key_, created->compute))
              return std::shared_ptr<PipelineNativeArtifact>();
            return created;
          });
      return metal_artifact_ &&
             metal_artifact_->type == PipelineStateType::Compute &&
             metal_artifact_->compute.pso;
    }

    return false;
  }

  const PipelineMetalGraphicsState *GetMetalGraphicsState() override {
    return GetMetalGraphicsState(0, 0);
  }

  const PipelineMetalGraphicsState *
  GetMetalGraphicsState(uint64_t demote_msaa_srv_mask_lo,
                        uint64_t demote_msaa_srv_mask_hi) override {
    if (type_ != PipelineStateType::Graphics)
      return nullptr;

    if (demote_msaa_srv_mask_lo || demote_msaa_srv_mask_hi) {
      std::lock_guard lock(metal_mutex_);
      const auto variant_key =
          std::make_pair(demote_msaa_srv_mask_lo, demote_msaa_srv_mask_hi);
      auto it = metal_graphics_variants_.find(variant_key);
      if (it == metal_graphics_variants_.end()) {
        const auto variant_shader_cache_key =
            BuildGraphicsVariantShaderCacheKey(
                shader_cache_key_, demote_msaa_srv_mask_lo,
                demote_msaa_srv_mask_hi);
        auto *cache = device_->GetPipelineNativeArtifactCache();
        if (!cache)
          return nullptr;
        auto artifact = cache->Acquire(
            variant_shader_cache_key,
            dxmt::perf::PsoArtifactKind::GraphicsVariant, [&] {
              auto created =
                  std::make_shared<PipelineNativeArtifact>(
                      PipelineStateType::Graphics);
              if (!CreateMetalGraphicsPipeline(
                      device_.ptr(), shaders_, graphics_state_,
                      root_signature_.ptr(), variant_shader_cache_key,
                      created->graphics, demote_msaa_srv_mask_lo,
                      demote_msaa_srv_mask_hi))
                return std::shared_ptr<PipelineNativeArtifact>();
              return created;
            });
        if (!artifact || artifact->type != PipelineStateType::Graphics)
          return nullptr;
        it = metal_graphics_variants_
                 .emplace(variant_key, std::move(artifact))
                 .first;
      }
      return it->second->graphics.pso ? &it->second->graphics : nullptr;
    }

    return metal_artifact_ &&
                   metal_artifact_->type == PipelineStateType::Graphics &&
                   metal_artifact_->graphics.pso
               ? &metal_artifact_->graphics
               : nullptr;
  }

  const PipelineMetalComputeState *GetMetalComputeState() override {
    if (type_ != PipelineStateType::Compute)
      return nullptr;

    return metal_artifact_ &&
                   metal_artifact_->type == PipelineStateType::Compute &&
                   metal_artifact_->compute.pso
               ? &metal_artifact_->compute
               : nullptr;
  }

  bool UsesBindlessMirror() const override { return uses_bindless_mirror_; }

  DXMT12_MTL4_SHADER_ABI_VERSION GetShaderAbiVersion() const override {
    return shader_abi_version_;
  }

private:
  const uint64_t cache_identity_;
  Com<IMTLD3D12Device> device_;
  PipelineStateType type_;
  Com<ID3D12RootSignature> public_root_signature_;
  Com<RootSignature, false> root_signature_;
  std::vector<PipelineDxilShader> shaders_;
  std::vector<PipelineSignatureLink> signature_links_;
  PipelineGraphicsState graphics_state_;
  PipelineComputeState compute_state_;
  std::string shader_cache_key_;
  std::vector<uint8_t> cached_shader_blob_;
  std::mutex metal_mutex_;
  bool uses_bindless_mirror_ = false;
  DXMT12_MTL4_SHADER_ABI_VERSION shader_abi_version_ =
      DXMT12_MTL4_SHADER_ABI_BINDLESS_MIRROR;
  std::shared_ptr<PipelineNativeArtifact> metal_artifact_;
  std::map<std::pair<uint64_t, uint64_t>,
           std::shared_ptr<PipelineNativeArtifact>>
      metal_graphics_variants_;
  ComPrivateData private_data_;
  std::string name_;
};

Com<ID3D12PipelineState>
CreatePipelineStateObject(IMTLD3D12Device *device, PipelineStateType type,
                          ID3D12RootSignature *root_signature,
                          std::vector<PipelineDxilShader> &&shaders,
                          PipelineGraphicsState &&graphics_state,
                          PipelineComputeState &&compute_state) {
  std::vector<PipelineSignatureLink> signature_links;
  if (!BuildSignatureLinks(shaders, signature_links))
    return nullptr;
  auto resolved_root_signature =
      ResolveRootSignature(device, root_signature, shaders);
  auto shader_cache_key =
      BuildShaderCacheKey(type, shaders, graphics_state, compute_state,
                          resolved_root_signature.ptr());
  DebugLogSignatureLinks(shader_cache_key, signature_links);
  auto *impl = new PipelineStateImpl(
      device, type, resolved_root_signature.ptr(), std::move(shaders),
      std::move(signature_links), std::move(shader_cache_key),
      std::move(graphics_state), std::move(compute_state));
  auto pso = Com<ID3D12PipelineState>::transfer(impl);
  if (!impl->MaterializeBaseArtifact())
    return nullptr;
  return pso;
}

void
StoreStatus(HRESULT *status, HRESULT value) {
  if (status)
    *status = value;
}

void
LogGraphicsPipelineDesc(const char *where,
                        const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc) {
  if (!D3D12PipelineDiagShouldLog())
    return;

  Logger::logFileOnly(
      LogLevel::Info,
      str::format("D3D12 diagnostic: graphics pipeline create",
                  " where=", where,
                  " hasVS=", HasBytecode(desc.VS),
                  " hasPS=", HasBytecode(desc.PS),
                  " hasGS=", HasBytecode(desc.GS),
                  " hasHS=", HasBytecode(desc.HS),
                  " hasDS=", HasBytecode(desc.DS),
                  " topologyType=", uint32_t(desc.PrimitiveTopologyType),
                  " numRT=", uint32_t(desc.NumRenderTargets),
                  " sampleCount=", uint32_t(desc.SampleDesc.Count),
                  " sampleQuality=", uint32_t(desc.SampleDesc.Quality),
                  " streamOutputEntries=", uint32_t(desc.StreamOutput.NumEntries),
                  " streamOutputStrides=", uint32_t(desc.StreamOutput.NumStrides),
                  " cachedPsoBytes=",
                  uint64_t(desc.CachedPSO.CachedBlobSizeInBytes)));
}

std::string
PipelineShaderBytecodeDigest(const D3D12_SHADER_BYTECODE &bytecode) {
  if (!HasBytecode(bytecode))
    return {};
  return Sha1HashState::compute(
             static_cast<const uint8_t *>(bytecode.pShaderBytecode),
             bytecode.BytecodeLength)
      .string();
}

void
LogUnsupportedSampleCount(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc) {
  static std::atomic<uint32_t> count = 0;
  const auto occurrence = count.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool sampled = occurrence <= 4 ||
                       (occurrence & (occurrence - 1)) == 0;
  if (!sampled)
    return;

  if (!D3D12PipelineDiagEnabled()) {
    WARN("D3D12PipelineState: unsupported Metal sample count ",
         desc.SampleDesc.Count, " occurrence=", occurrence);
    return;
  }

  void *frames[12] = {};
#if defined(_WIN32)
  const auto frame_count =
      RtlCaptureStackBackTrace(0, std::size(frames), frames, nullptr);
#else
  const auto frame_count = backtrace(frames, std::size(frames));
#endif
  std::ostringstream stack;
  stack << std::hex;
  for (uint32_t i = 0; i < uint32_t(frame_count); i++) {
    if (i)
      stack << ',';
    stack << reinterpret_cast<uintptr_t>(frames[i]);
  }

  std::ostringstream formats;
  for (UINT i = 0; i < desc.NumRenderTargets && i < 8; i++) {
    if (i)
      formats << ',';
    formats << uint32_t(desc.RTVFormats[i]);
  }

  WARN("D3D12 diagnostic: unsupported MSAA PSO",
       " occurrence=", occurrence,
       " thread=", dxmt::this_thread::get_id(),
       " sampleCount=", uint32_t(desc.SampleDesc.Count),
       " sampleQuality=", uint32_t(desc.SampleDesc.Quality),
       " topologyType=", uint32_t(desc.PrimitiveTopologyType),
       " numRT=", uint32_t(desc.NumRenderTargets),
       " rtvFormats=", formats.str(),
       " dsvFormat=", uint32_t(desc.DSVFormat),
       " rootSignature=", reinterpret_cast<uintptr_t>(desc.pRootSignature),
       " vs=", PipelineShaderBytecodeDigest(desc.VS),
       " ps=", PipelineShaderBytecodeDigest(desc.PS),
       " gs=", PipelineShaderBytecodeDigest(desc.GS),
       " hs=", PipelineShaderBytecodeDigest(desc.HS),
       " ds=", PipelineShaderBytecodeDigest(desc.DS),
       " stack=", stack.str(),
       " hr=", E_INVALIDARG);
}

uint64_t
ElapsedUs(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

} // namespace

NativeShaderAbiEligibilityReason
GetNativeShaderAbiEligibility(
    const std::vector<PipelineDxilShader> &shaders,
    const RootSignature *root_signature) {
  return GetNativeShaderAbiEligibilityImpl(shaders, root_signature);
}

Com<ID3D12PipelineState>
CreateGraphicsPipelineState(IMTLD3D12Device *device,
                            const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                            HRESULT *status) {
  if (!device || !desc) {
    StoreStatus(status, E_INVALIDARG);
    return nullptr;
  }
  const auto create_start = std::chrono::steady_clock::now();
  LogGraphicsPipelineDesc("CreateGraphicsPipelineState", *desc);

  PipelineGraphicsState graphics_state = {};
  HRESULT hr = CloneGraphicsState(*desc, graphics_state);
  if (FAILED(hr)) {
    dxmt::perf::recordGraphicsPipelineCreate(ElapsedUs(create_start), false);
    StoreStatus(status, hr);
    return nullptr;
  }
  if (!device->GetMTLDevice().supportsTextureSampleCount(
          graphics_state.desc.SampleDesc.Count)) {
    LogUnsupportedSampleCount(graphics_state.desc);
    dxmt::perf::recordGraphicsPipelineCreate(ElapsedUs(create_start), false);
    StoreStatus(status, E_INVALIDARG);
    return nullptr;
  }

  std::vector<PipelineDxilShader> shaders;
  shaders.reserve(5);

  if (HasBytecode(desc->VS) && HasBytecode(desc->PS) &&
      !HasBytecode(desc->GS) && !HasBytecode(desc->HS) &&
      !HasBytecode(desc->DS) &&
      !ValidateDirectVertexPixelLinkage(desc->VS, desc->PS)) {
    dxmt::perf::recordGraphicsPipelineCreate(ElapsedUs(create_start), false);
    StoreStatus(status, E_INVALIDARG);
    return nullptr;
  }

  hr = AppendPipelineShader(PipelineShaderStage::Vertex, device, desc->VS, shaders);
  if (FAILED(hr)) {
    DestroyPipelineShaders(shaders);
    dxmt::perf::recordGraphicsPipelineCreate(ElapsedUs(create_start), false);
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendPipelineShader(PipelineShaderStage::Pixel, device, desc->PS, shaders);
  if (FAILED(hr)) {
    DestroyPipelineShaders(shaders);
    dxmt::perf::recordGraphicsPipelineCreate(ElapsedUs(create_start), false);
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendPipelineShader(PipelineShaderStage::Geometry, device, desc->GS, shaders);
  if (FAILED(hr)) {
    DestroyPipelineShaders(shaders);
    dxmt::perf::recordGraphicsPipelineCreate(ElapsedUs(create_start), false);
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendPipelineShader(PipelineShaderStage::Hull, device, desc->HS, shaders);
  if (FAILED(hr)) {
    DestroyPipelineShaders(shaders);
    dxmt::perf::recordGraphicsPipelineCreate(ElapsedUs(create_start), false);
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendPipelineShader(PipelineShaderStage::Domain, device, desc->DS, shaders);
  if (FAILED(hr)) {
    DestroyPipelineShaders(shaders);
    dxmt::perf::recordGraphicsPipelineCreate(ElapsedUs(create_start), false);
    StoreStatus(status, hr);
    return nullptr;
  }
  if (shaders.empty()) {
    DestroyPipelineShaders(shaders);
    dxmt::perf::recordGraphicsPipelineCreate(ElapsedUs(create_start), false);
    StoreStatus(status, E_INVALIDARG);
    return nullptr;
  }

  PipelineComputeState compute_state = {};
  auto pso = CreatePipelineStateObject(
      device, PipelineStateType::Graphics, desc->pRootSignature,
      std::move(shaders), std::move(graphics_state),
      std::move(compute_state));
  dxmt::perf::recordGraphicsPipelineCreate(ElapsedUs(create_start), bool(pso));
  StoreStatus(status, pso ? S_OK : E_INVALIDARG);
  return pso;
}

Com<ID3D12PipelineState>
CreateComputePipelineState(IMTLD3D12Device *device,
                           const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                           HRESULT *status) {
  if (!device || !desc) {
    StoreStatus(status, E_INVALIDARG);
    return nullptr;
  }
  const auto create_start = std::chrono::steady_clock::now();

  PipelineComputeState compute_state = {};
  HRESULT hr = CloneComputeState(*desc, compute_state);
  if (FAILED(hr)) {
    dxmt::perf::recordComputePipelineCreate(ElapsedUs(create_start), false);
    StoreStatus(status, hr);
    return nullptr;
  }

  std::vector<PipelineDxilShader> shaders;
  shaders.reserve(1);
  hr = AppendPipelineShader(PipelineShaderStage::Compute, device, desc->CS, shaders);
  if (FAILED(hr)) {
    DestroyPipelineShaders(shaders);
    dxmt::perf::recordComputePipelineCreate(ElapsedUs(create_start), false);
    StoreStatus(status, hr);
    return nullptr;
  }
  if (shaders.empty()) {
    DestroyPipelineShaders(shaders);
    dxmt::perf::recordComputePipelineCreate(ElapsedUs(create_start), false);
    StoreStatus(status, E_INVALIDARG);
    return nullptr;
  }

  PipelineGraphicsState graphics_state = {};
  auto pso = CreatePipelineStateObject(
      device, PipelineStateType::Compute, desc->pRootSignature,
      std::move(shaders), std::move(graphics_state),
      std::move(compute_state));
  dxmt::perf::recordComputePipelineCreate(ElapsedUs(create_start), bool(pso));
  StoreStatus(status, pso ? S_OK : E_INVALIDARG);
  return pso;
}

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
Com<ID3D12PipelineState>
CreatePipelineStateFromStream(IMTLD3D12Device *device,
                              const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                              HRESULT *status) {
  if (!device || !desc) {
    StoreStatus(status, E_INVALIDARG);
    return nullptr;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics = {};
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute = {};
  bool has_compute_shader = false;
  HRESULT hr = ParsePipelineStateStream(*desc, graphics, compute,
                                        has_compute_shader);
  if (FAILED(hr)) {
    StoreStatus(status, hr);
    return nullptr;
  }

  if (has_compute_shader)
    return CreateComputePipelineState(device, &compute, status);
  return CreateGraphicsPipelineState(device, &graphics, status);
}
#endif

#ifdef __ID3D12PipelineLibrary_INTERFACE_DEFINED__
Com<ID3D12PipelineLibrary>
CreatePipelineLibrary(IMTLD3D12Device *device) {
  return Com<ID3D12PipelineLibrary>::transfer(
      new PipelineLibraryImpl(device));
}
#endif

} // namespace dxmt::d3d12
