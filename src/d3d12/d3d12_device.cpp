#include "d3d12_device.hpp"
#include "d3d12_dxgi_backend.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "d3d12_command_allocator.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_command_queue.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_agility.hpp"
#include "d3d12_fence.hpp"
#include "d3d12_heap.hpp"
#include "d3d12_indirect_topology.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_query.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_root_signature.hpp"
#include "d3d12_sampler.hpp"
#include "d3d12_texture_swizzle.hpp"
#include "d3d12_texture_view.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_checked_math.hpp"
#include "dxmt_d3d12_test_path.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "dxmt_format.hpp"
#include "dxmt_perf_stats.hpp"
#include "dxmt_shader_cache.hpp"
#include "dxmt_texture.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_noexcept.hpp"
#include "util_string.hpp"
#include "util_win32_compat.h"
#include <atomic>
#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <winbase.h>

namespace dxmt::d3d12 {

namespace {

constexpr D3D_FEATURE_LEVEL kSupportedFeatureLevel = D3D_FEATURE_LEVEL_12_0;
constexpr uint32_t kRepeatedDescriptorWarningLimit = 8;

// Test-only fault injection used by the D3D12 descriptor conformance suite.
// This deliberately leaves the legacy descriptor GPU VA intact while making
// only the native resource sidecar lookup unavailable.
const bool kTestForceNativeCbvResourceLookupMiss = [] {
  const auto value =
      env::getEnvVar("DXMT_TEST_FORCE_NATIVE_CBV_RESOURCE_LOOKUP_MISS");
  return value == "1" || value == "true" || value == "yes";
}();

const bool kTestForceNativeCbvStaleResourceTableEntry = [] {
  const auto value =
      env::getEnvVar("DXMT_TEST_FORCE_NATIVE_CBV_STALE_RESOURCE_TABLE_ENTRY");
  return value == "1" || value == "true" || value == "yes";
}();

std::atomic<uint64_t> g_test_resource_creation_occurrence = 0;
std::atomic<uint64_t> g_test_descriptor_heap_creation_occurrence = 0;
std::atomic<uint64_t> g_test_compute_pipeline_creation_occurrence = 0;
std::atomic<uint64_t> g_test_command_queue_creation_occurrence = 0;
std::atomic<uint64_t> g_test_graphics_pipeline_creation_occurrence = 0;
std::atomic<uint64_t> g_test_heap_creation_occurrence = 0;
std::atomic<uint64_t> g_test_root_signature_creation_occurrence = 0;
std::atomic<uint64_t> g_test_fence_creation_occurrence = 0;
std::atomic<uint64_t> g_test_query_heap_creation_occurrence = 0;
std::atomic<uint64_t> g_test_descriptor_table_allocation_occurrence = 0;
std::atomic<uint64_t> g_test_residency_insertion_occurrence = 0;
std::atomic<uint64_t> g_test_pso_archive_write_occurrence = 0;
std::atomic<uint64_t> g_test_metal_buffer_creation_occurrence = 0;
std::atomic<uint64_t> g_test_metal_texture_creation_occurrence = 0;
std::atomic<uint64_t> g_test_metal_heap_creation_occurrence = 0;
std::atomic<uint64_t> g_test_metal_graphics_pipeline_occurrence = 0;

class FencePrivateReference final {
public:
  explicit FencePrivateReference(Fence *fence) noexcept
      : fence_(fence) {
    fence_->AddRefPrivate();
  }

  FencePrivateReference(const FencePrivateReference &) = delete;
  FencePrivateReference &operator=(const FencePrivateReference &) = delete;

  FencePrivateReference(FencePrivateReference &&other) noexcept
      : fence_(other.fence_) {
    other.fence_ = nullptr;
  }

  FencePrivateReference &operator=(FencePrivateReference &&other) noexcept {
    if (this == &other)
      return *this;
    Reset();
    fence_ = other.fence_;
    other.fence_ = nullptr;
    return *this;
  }

  ~FencePrivateReference() noexcept {
    Reset();
  }

  Fence &Get() const noexcept {
    return *fence_;
  }

private:
  void Reset() noexcept {
    if (!fence_)
      return;
    auto *fence = fence_;
    fence_ = nullptr;
    fence->ReleasePrivate();
  }

  Fence *fence_;
};

static bool
AreFenceValuesSatisfied(const std::vector<FencePrivateReference> &fences,
                        const std::vector<UINT64> &values,
                        D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags) {
  const bool wait_any = flags & D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY;
  for (size_t index = 0; index < fences.size(); index++) {
    const bool satisfied = fences[index].Get().GetCompletedValue() >= values[index];
    if (wait_any && satisfied)
      return true;
    if (!wait_any && !satisfied)
      return false;
  }
  return !wait_any;
}

class MultipleFenceWaitState final
    : public FenceWaitTarget,
      public std::enable_shared_from_this<MultipleFenceWaitState> {
public:
  MultipleFenceWaitState(HANDLE event, bool wait_any,
                         std::vector<FencePrivateReference> fences) noexcept
      : event_(event), wait_any_(wait_any), fences_(std::move(fences)) {}

  MultipleFenceWaitState(const MultipleFenceWaitState &) = delete;
  MultipleFenceWaitState &operator=(const MultipleFenceWaitState &) = delete;
  ~MultipleFenceWaitState() noexcept override = default;

  void Arm(uint32_t pending_count) {
    remaining_.store(pending_count, std::memory_order_relaxed);
    keep_alive_ = shared_from_this();
  }

  Fence &FenceAt(size_t index) const noexcept {
    return fences_[index].Get();
  }

  void CompleteFenceWait() noexcept override {
    if (wait_any_) {
      SignalOnce();
      return;
    }

    if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1)
      SignalOnce();
  }

private:
  void SignalOnce() noexcept {
    bool expected = false;
    if (!signaled_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire))
      return;

    SetEvent(event_);
    keep_alive_.reset();
  }

  HANDLE event_;
  const bool wait_any_;
  std::vector<FencePrivateReference> fences_;
  std::atomic_bool signaled_ = false;
  std::atomic_uint32_t remaining_ = 0;
  std::shared_ptr<MultipleFenceWaitState> keep_alive_;
};

static bool
ShouldInjectCreationFailure(const char *environment_name,
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

struct FaultInjectionConfiguration {
  uint64_t target = 0;
  bool always = false;
};

static FaultInjectionConfiguration
LoadFaultInjectionConfiguration(const char *environment_name) {
  const auto value = env::getEnvVar(environment_name);
  if (value == "always" || value == "all")
    return {.always = true};
  if (value.empty())
    return {};
  char *end = nullptr;
  const auto target = std::strtoull(value.c_str(), &end, 0);
  return end != value.c_str() && !*end && target
             ? FaultInjectionConfiguration{.target = target}
             : FaultInjectionConfiguration{};
}

static bool
ShouldInjectConfiguredFailure(const FaultInjectionConfiguration &configuration,
                              std::atomic<uint64_t> &occurrence) {
  if (!configuration.always && !configuration.target)
    return false;
  const auto current =
      occurrence.fetch_add(1, std::memory_order_relaxed) + 1;
  return configuration.always || current == configuration.target;
}

static void
RecordInjectedFault(const char *name) {
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
ShouldLogRepeatedDescriptorWarning(std::atomic<uint32_t> &counter) {
  auto previous = counter.fetch_add(1, std::memory_order_relaxed);
  return previous < kRepeatedDescriptorWarningLimit;
}

static void
BeginDeviceCall(const char *method) {
  std::string opname = "ID3D12Device::";
  opname += method;
  dxmt::apitrace::record_call(opname.c_str());
}

static bool
D3D12DeviceDiagEnabled() {
  auto enabled = env::getEnvVar("DXMT_DIAG_D3D12_DEVICE");
  if (enabled.empty())
    enabled = env::getEnvVar("DXMT_DIAG_COMMAND_QUEUE");
  if (enabled.empty())
    enabled = env::getEnvVar("DXMT_DIAG_ROOT_CAUSE_DENSE");
  return enabled == "1" || enabled == "true" || enabled == "yes" ||
         enabled == "trace";
}

static bool
D3D12RootCauseDenseDiagEnabled() {
  const auto enabled = env::getEnvVar("DXMT_DIAG_ROOT_CAUSE_DENSE");
  return enabled == "1" || enabled == "true" || enabled == "yes" ||
         enabled == "trace";
}

static UINT64
Align(UINT64 value, UINT64 alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static void
LogPSOBinaryArchiveMarker(const char *format, ...) {
  auto marker_path = env::getEnvVar("DXMT_PSO_ARCHIVE_MARKER");
  if (marker_path.empty() && !D3D12DeviceDiagEnabled())
    return;

  char message[1024];

  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  if (D3D12DeviceDiagEnabled())
    fprintf(stderr, "%s\n", message);
  if (marker_path.empty())
    return;

  FILE *marker = fopen(marker_path.c_str(), "a");
  if (!marker)
    return;
  fprintf(marker, "%s\n", message);
  fclose(marker);
}

static uint64_t
GetPSOBinaryArchiveSerializeEvery() {
  auto value = env::getEnvVar("DXMT_PSO_ARCHIVE_SERIALIZE_EVERY");
  if (value.empty())
    return 256;
  return strtoull(value.c_str(), nullptr, 10);
}

static std::string
EnsureTrailingSlash(std::string path) {
  if (!path.empty() && !path.ends_with('/') && !path.ends_with('\\'))
    path += "/";
  return path;
}

static std::string
GetDarwinUserCacheDirectory() {
  auto cache_dir = env::getEnvVar("DARWIN_USER_CACHE_DIR");
  if (!cache_dir.empty() && cache_dir.starts_with("/"))
    return EnsureTrailingSlash(cache_dir);

  auto tmp_dir = env::getEnvVar("TMPDIR");
  if (tmp_dir.empty() || !tmp_dir.starts_with("/"))
    return {};

  tmp_dir = EnsureTrailingSlash(tmp_dir);
  if (tmp_dir.ends_with("/T/"))
    return tmp_dir.substr(0, tmp_dir.size() - 3) + "/C/";
  if (tmp_dir.ends_with("/T"))
    return tmp_dir.substr(0, tmp_dir.size() - 2) + "/C/";

  return {};
}

static bool
FileExists(const std::string &path) {
  FILE *file = fopen(path.c_str(), "rb");
  if (!file)
    return false;
  fclose(file);
  return true;
}

static std::string
ResolvePSOBinaryArchiveDirectory() {
  auto cache_dir = EnsureTrailingSlash(dxmt::GetDXMTShaderCacheDirectory());
  if (cache_dir.empty())
    return {};

  if (cache_dir.starts_with("/"))
    return cache_dir;

  if (cache_dir.size() >= 2 && cache_dir[1] == ':') {
    auto unix_dir = env::getUnixPath(cache_dir);
    return unix_dir.empty() ? std::string() : EnsureTrailingSlash(unix_dir);
  }

  auto darwin_cache_dir = GetDarwinUserCacheDirectory();
  if (!darwin_cache_dir.empty())
    return EnsureTrailingSlash(darwin_cache_dir + cache_dir);

  auto unix_dir = env::getUnixPath(cache_dir);
  if (!unix_dir.empty() && unix_dir.starts_with("/"))
    return EnsureTrailingSlash(unix_dir);

  return {};
}

static bool
IsCommittedAccessibleVirtualMemory(const MEMORY_BASIC_INFORMATION &info) {
  if (info.State != MEM_COMMIT)
    return false;
  if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))
    return false;
  return true;
}

static bool
GetVirtualAllocationInfo(const void *address,
                         MEMORY_BASIC_INFORMATION &first_region,
                         SIZE_T &allocation_size,
                         const char *&reason) {
  allocation_size = 0;
  reason = nullptr;

  if (!VirtualQuery(address, &first_region, sizeof(first_region)) ||
      !first_region.AllocationBase || !first_region.RegionSize) {
    reason = "query";
    return false;
  }

  if (address != first_region.AllocationBase) {
    reason = "address-not-allocation-base";
    return false;
  }

  auto *base = static_cast<const char *>(first_region.AllocationBase);
  auto *cursor = base;
  auto *end = base;

  for (;;) {
    MEMORY_BASIC_INFORMATION region = {};
    if (!VirtualQuery(cursor, &region, sizeof(region)))
      break;
    if (region.AllocationBase != first_region.AllocationBase)
      break;
    if (static_cast<const char *>(region.BaseAddress) != cursor) {
      reason = "non-contiguous-region";
      return false;
    }
    if (!IsCommittedAccessibleVirtualMemory(region)) {
      reason = "non-committed-or-inaccessible-region";
      return false;
    }

    auto *next = cursor + region.RegionSize;
    if (next <= cursor) {
      reason = "region-overflow";
      return false;
    }
    end = next;
    cursor = next;
  }

  allocation_size = static_cast<SIZE_T>(end - base);
  if (!allocation_size) {
    reason = "zero-allocation-size";
    return false;
  }

  return true;
}

static UINT
SubresourceMipSlice(UINT sub_resource, UINT mip_levels) {
  return mip_levels ? sub_resource % mip_levels : 0;
}

static UINT
GetD3D12FormatPlaneCount(DXGI_FORMAT format) {
  const auto &traits = GetDXGIFormatTraits(format);
  return traits.planeCount ? traits.planeCount : 1;
}

static UINT
GetDescriptorResourcePlaneCount(const Resource &resource) {
  const auto &desc = resource.GetResourceDesc();
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return 1;
  return GetD3D12FormatPlaneCount(desc.Format);
}

static UINT
NormalizeDescriptorViewCount(UINT requested, UINT first, UINT total) {
  if (first >= total)
    return 1;
  const UINT remaining = total - first;
  if (requested == UINT_MAX || requested == 0)
    return remaining;
  return std::min(requested, remaining);
}

static UINT
GetDescriptorMipDepth(const Resource &resource, UINT mip_slice) {
  const auto &desc = resource.GetResourceDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return 1;
  return static_cast<UINT>(
      std::max<UINT64>(1, desc.DepthOrArraySize >> mip_slice));
}

static DXGI_FORMAT
GetD3D12FootprintFormat(DXGI_FORMAT format, UINT plane) {
  const auto &traits = GetDXGIFormatTraits(format);
  if (plane < traits.planeCount && traits.planes[plane].footprintFormat)
    return static_cast<DXGI_FORMAT>(traits.planes[plane].footprintFormat);
  return format;
}

static UINT
GetD3D12FormatPlaneElementSize(DXGI_FORMAT format, UINT plane,
                               const DXGIFormatPlaneFootprintLayout &layout) {
  const auto &traits = GetDXGIFormatTraits(format);
  if (plane < traits.planeCount && traits.planes[plane].elementSize)
    return traits.planes[plane].elementSize;
  return layout.elementSize;
}

static UINT
GetD3D12FormatBlockWidth(const DXGIFormatPlaneFootprintLayout &layout) {
  return layout.blockWidth;
}

static UINT
GetD3D12FormatBlockWidth(const MTL_DXGI_FORMAT_DESC &format_desc) {
  return (format_desc.Flag & MTL_DXGI_FORMAT_BC) ? 4 : 1;
}

static UINT
GetD3D12FormatBlockHeight(const DXGIFormatPlaneFootprintLayout &layout) {
  return layout.blockHeight;
}

static UINT
GetD3D12FormatBlockHeight(const MTL_DXGI_FORMAT_DESC &format_desc) {
  return (format_desc.Flag & MTL_DXGI_FORMAT_BC) ? 4 : 1;
}

static void
GetD3D12FormatSubsampleLog2(DXGI_FORMAT format, UINT plane, UINT &x, UINT &y) {
  const auto &traits = GetDXGIFormatTraits(format);
  if (plane < traits.planeCount) {
    x = traits.planes[plane].subsampleXLog2;
    y = traits.planes[plane].subsampleYLog2;
  } else {
    x = 0;
    y = 0;
  }
}

static UINT64
MipSize(UINT64 value, UINT mip_slice) {
  return std::max<UINT64>(1, value >> mip_slice);
}

static bool
IsAbstractedCpuVisibleHeap(const D3D12_HEAP_PROPERTIES &properties) {
  return properties.Type == D3D12_HEAP_TYPE_UPLOAD ||
         properties.Type == D3D12_HEAP_TYPE_READBACK;
}

static bool
HasInvalidCpuVisibleBufferFlags(const D3D12_RESOURCE_DESC &desc,
                                const D3D12_HEAP_PROPERTIES &heap_properties) {
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
    return false;
  if (!IsAbstractedCpuVisibleHeap(heap_properties))
    return false;
  return desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                       D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
}

static bool
IsValidCrossAdapterResourceDesc(const D3D12_HEAP_PROPERTIES &,
                                D3D12_HEAP_FLAGS heap_flags,
                                const D3D12_RESOURCE_DESC &desc) {
  const bool cross_adapter =
      desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
  const bool cross_adapter_heap =
      heap_flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;

  // D3D12 row-major textures are exclusively cross-adapter resources. DXMT
  // does not yet implement shared cross-adapter texture backing, so accepting
  // one would silently create an ordinary tiled Metal texture instead.
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
      desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
    return false;

  if (!cross_adapter && !cross_adapter_heap)
    return true;

  if (!cross_adapter || !cross_adapter_heap ||
      !(heap_flags & D3D12_HEAP_FLAG_SHARED))
    return false;

  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return true;

  // The only legal cross-adapter texture layout is row-major, which is failed
  // closed above until the shared linear backing path is implemented.
  return false;
}

static bool
IsValidCopyableFootprintDesc(const D3D12_RESOURCE_DESC &desc) {
  if (desc.Width == 0 || desc.Height == 0 || desc.DepthOrArraySize == 0)
    return false;
  if (desc.SampleDesc.Count == 0)
    return false;
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return desc.Format == DXGI_FORMAT_UNKNOWN;
  return desc.Format != DXGI_FORMAT_UNKNOWN;
}

static bool
IsBcFormat(WMT::Device device, DXGI_FORMAT format) {
  MTL_DXGI_FORMAT_DESC format_desc = {};
  return SUCCEEDED(MTLQueryDXGIFormat(device, format, format_desc)) &&
         (format_desc.Flag & MTL_DXGI_FORMAT_BC);
}

static bool
GetSmallResource4KTileShape(WMT::Device device,
                            const D3D12_RESOURCE_DESC &desc,
                            D3D12_TILE_SHAPE &tile_shape) {
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
      desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return false;

  const auto &traits = GetDXGIFormatTraits(desc.Format);
  if (traits.flags & (DXGI_FORMAT_TRAIT_MULTIPLANE |
                      DXGI_FORMAT_TRAIT_VIDEO |
                      DXGI_FORMAT_TRAIT_DEPTH_STENCIL))
    return false;

  MTL_DXGI_FORMAT_DESC format_desc = {};
  if (FAILED(MTLQueryDXGIFormat(device, desc.Format, format_desc)))
    return false;

  tile_shape = {};
  const bool is_bc = format_desc.Flag & MTL_DXGI_FORMAT_BC;
  const UINT unit_bytes =
      is_bc ? format_desc.BlockSize : format_desc.BytesPerTexel;
  const UINT bpu = unit_bytes * 8;
  if (!bpu || bpu > 128)
    return false;

  if (is_bc) {
    if (unit_bytes != 8 && unit_bytes != 16)
      return false;

    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
      tile_shape.WidthInTexels = 16 * GetD3D12FormatBlockWidth(format_desc);
      tile_shape.HeightInTexels = 16 * GetD3D12FormatBlockHeight(format_desc);
      tile_shape.DepthInTexels = 1;
      if (unit_bytes == 8)
        tile_shape.WidthInTexels *= 2;
    } else {
      tile_shape.WidthInTexels = 8 * GetD3D12FormatBlockWidth(format_desc);
      tile_shape.HeightInTexels = 8 * GetD3D12FormatBlockHeight(format_desc);
      tile_shape.DepthInTexels = unit_bytes == 8 ? 8 : 4;
    }

    return true;
  }

  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
    tile_shape.DepthInTexels = 1;
    if (bpu <= 8) {
      tile_shape.WidthInTexels = 64;
      tile_shape.HeightInTexels = 64;
    } else if (bpu <= 16) {
      tile_shape.WidthInTexels = 64;
      tile_shape.HeightInTexels = 32;
    } else if (bpu <= 32) {
      tile_shape.WidthInTexels = 32;
      tile_shape.HeightInTexels = 32;
    } else if (bpu <= 64) {
      tile_shape.WidthInTexels = 32;
      tile_shape.HeightInTexels = 16;
    } else {
      tile_shape.WidthInTexels = 16;
      tile_shape.HeightInTexels = 16;
    }
  } else {
    if (bpu <= 8) {
      tile_shape.WidthInTexels = 16;
      tile_shape.HeightInTexels = 16;
      tile_shape.DepthInTexels = 16;
    } else if (bpu <= 16) {
      tile_shape.WidthInTexels = 16;
      tile_shape.HeightInTexels = 16;
      tile_shape.DepthInTexels = 8;
    } else if (bpu <= 32) {
      tile_shape.WidthInTexels = 16;
      tile_shape.HeightInTexels = 8;
      tile_shape.DepthInTexels = 8;
    } else if (bpu <= 64) {
      tile_shape.WidthInTexels = 8;
      tile_shape.HeightInTexels = 8;
      tile_shape.DepthInTexels = 8;
    } else {
      tile_shape.WidthInTexels = 8;
      tile_shape.HeightInTexels = 8;
      tile_shape.DepthInTexels = 4;
    }
  }

  return true;
}

static bool
IsIndirectOperationArgument(D3D12_INDIRECT_ARGUMENT_TYPE type) {
  return type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW ||
         type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ||
         type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
}

static bool
HasFormatCapability(FormatCapability caps, FormatCapability cap) {
  return (static_cast<int>(caps) & static_cast<int>(cap)) != 0;
}

static DXGI_FORMAT
ResolveDepthTypelessFormatForD3D12Caps(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R16_TYPELESS:
    return DXGI_FORMAT_D16_UNORM;
  case DXGI_FORMAT_R32_TYPELESS:
    return DXGI_FORMAT_D32_FLOAT;
  default:
    return format;
  }
}

static bool
IsEmulatedThreeComponentBufferFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R32G32B32_TYPELESS:
  case DXGI_FORMAT_R32G32B32_FLOAT:
  case DXGI_FORMAT_R32G32B32_UINT:
  case DXGI_FORMAT_R32G32B32_SINT:
    return true;
  default:
    return false;
  }
}

static FormatCapability
GetD3D12FormatCapability(WMT::Device device,
                         const MTL_DXGI_FORMAT_DESC &format) {
  FormatCapabilityInspector inspector;
  inspector.Inspect(device);
  auto entry = inspector.textureCapabilities.find(format.PixelFormat);
  return entry == inspector.textureCapabilities.end()
             ? FormatCapability::None
             : entry->second;
}

static bool IsD3D12TypedUavFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
  case DXGI_FORMAT_R32G32B32A32_UINT:
  case DXGI_FORMAT_R32G32B32A32_SINT:
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_UNORM:
  case DXGI_FORMAT_R16G16B16A16_UINT:
  case DXGI_FORMAT_R16G16B16A16_SNORM:
  case DXGI_FORMAT_R16G16B16A16_SINT:
  case DXGI_FORMAT_R32G32_FLOAT:
  case DXGI_FORMAT_R32G32_UINT:
  case DXGI_FORMAT_R32G32_SINT:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10A2_UINT:
  case DXGI_FORMAT_R11G11B10_FLOAT:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UINT:
  case DXGI_FORMAT_R8G8B8A8_SNORM:
  case DXGI_FORMAT_R8G8B8A8_SINT:
  case DXGI_FORMAT_R16G16_FLOAT:
  case DXGI_FORMAT_R16G16_UNORM:
  case DXGI_FORMAT_R16G16_UINT:
  case DXGI_FORMAT_R16G16_SNORM:
  case DXGI_FORMAT_R16G16_SINT:
  case DXGI_FORMAT_R32_FLOAT:
  case DXGI_FORMAT_R32_UINT:
  case DXGI_FORMAT_R32_SINT:
  case DXGI_FORMAT_R8G8_UNORM:
  case DXGI_FORMAT_R8G8_UINT:
  case DXGI_FORMAT_R8G8_SNORM:
  case DXGI_FORMAT_R8G8_SINT:
  case DXGI_FORMAT_R16_FLOAT:
  case DXGI_FORMAT_R16_UNORM:
  case DXGI_FORMAT_R16_UINT:
  case DXGI_FORMAT_R16_SNORM:
  case DXGI_FORMAT_R16_SINT:
  case DXGI_FORMAT_R8_UNORM:
  case DXGI_FORMAT_R8_UINT:
  case DXGI_FORMAT_R8_SNORM:
  case DXGI_FORMAT_R8_SINT:
  case DXGI_FORMAT_A8_UNORM:
  case DXGI_FORMAT_B5G6R5_UNORM:
  case DXGI_FORMAT_B5G5R5A1_UNORM:
  case DXGI_FORMAT_B4G4R4A4_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
    return true;
  default:
    return false;
  }
}

static bool IsD3D12CoreTypedUavLoadFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R32_FLOAT || format == DXGI_FORMAT_R32_UINT ||
         format == DXGI_FORMAT_R32_SINT;
}

static D3D12_FORMAT_SUPPORT1
GetD3D12FormatSupport1(FormatCapability caps,
                       const MTL_DXGI_FORMAT_DESC &format) {
  D3D12_FORMAT_SUPPORT1 support = D3D12_FORMAT_SUPPORT1_NONE;
  if (HasFormatCapability(caps, FormatCapability::TextureBufferRead) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferWrite) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferReadWrite))
    support |= D3D12_FORMAT_SUPPORT1_BUFFER;

  support |= D3D12_FORMAT_SUPPORT1_TEXTURE1D |
             D3D12_FORMAT_SUPPORT1_TEXTURE2D |
             D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
             D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT;

  if (!(format.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER |
                       MTL_DXGI_FORMAT_STENCIL_PLANER))) {
    support |= D3D12_FORMAT_SUPPORT1_TEXTURE3D |
               D3D12_FORMAT_SUPPORT1_TEXTURECUBE;
  }
  if (HasFormatCapability(caps, FormatCapability::Filter))
    support |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
               D3D12_FORMAT_SUPPORT1_SHADER_GATHER;
  if (HasFormatCapability(caps, FormatCapability::Color))
    support |= D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
  if (HasFormatCapability(caps, FormatCapability::Blend))
    support |= D3D12_FORMAT_SUPPORT1_BLENDABLE;
  if (HasFormatCapability(caps, FormatCapability::DepthStencil))
    support |= D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL;
  if (HasFormatCapability(caps, FormatCapability::MSAA))
    support |= D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET |
               D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD;
  // ResolveSubresource currently lowers through a color render pass. Do not
  // advertise depth/stencil resolve until the command path implements the
  // corresponding depth/stencil attachment semantics.
  if (HasFormatCapability(caps, FormatCapability::Resolve) &&
      !(format.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER |
                       MTL_DXGI_FORMAT_STENCIL_PLANER)))
    support |= D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE;
  if (HasFormatCapability(caps, FormatCapability::Write))
    support |= D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
  if (format.Flag & MTL_DXGI_FORMAT_BACKBUFFER)
    support |= D3D12_FORMAT_SUPPORT1_DISPLAY |
               D3D12_FORMAT_SUPPORT1_BACK_BUFFER_CAST;

  return support;
}

static D3D12_FORMAT_SUPPORT2
GetD3D12FormatSupport2(FormatCapability caps) {
  D3D12_FORMAT_SUPPORT2 support = D3D12_FORMAT_SUPPORT2_NONE;
#ifndef DXMT_NO_PRIVATE_API
  if (HasFormatCapability(caps, FormatCapability::Blend))
    support |= D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP;
#endif
  if (HasFormatCapability(caps, FormatCapability::TextureBufferRead) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferReadWrite))
    support |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
  if (HasFormatCapability(caps, FormatCapability::Write) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferWrite) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferReadWrite))
    support |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
  if (HasFormatCapability(caps, FormatCapability::Atomic))
    support |= D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;
  return support;
}

static bool SupportsD3D12AdditionalTypedUavLoads(WMT::Device device) {
  constexpr DXGI_FORMAT formats[] = {
      DXGI_FORMAT_R32G32B32A32_FLOAT,
      DXGI_FORMAT_R32G32B32A32_UINT,
      DXGI_FORMAT_R32G32B32A32_SINT,
      DXGI_FORMAT_R16G16B16A16_FLOAT,
      DXGI_FORMAT_R16G16B16A16_UINT,
      DXGI_FORMAT_R16G16B16A16_SINT,
      DXGI_FORMAT_R8G8B8A8_UNORM,
      DXGI_FORMAT_R8G8B8A8_UINT,
      DXGI_FORMAT_R8G8B8A8_SINT,
      DXGI_FORMAT_R16_FLOAT,
      DXGI_FORMAT_R16_UINT,
      DXGI_FORMAT_R16_SINT,
      DXGI_FORMAT_R8_UNORM,
      DXGI_FORMAT_R8_UINT,
      DXGI_FORMAT_R8_SINT,
  };
  FormatCapabilityInspector inspector;
  inspector.Inspect(device);
  for (const auto dxgi_format : formats) {
    MTL_DXGI_FORMAT_DESC format = {};
    if (FAILED(MTLQueryDXGIFormat(device, dxgi_format, format)))
      return false;
    const auto entry = inspector.textureCapabilities.find(format.PixelFormat);
    if (entry == inspector.textureCapabilities.end() ||
        !(GetD3D12FormatSupport2(entry->second) &
          D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
      return false;
  }
  return true;
}

static bool
SupportsTiledTextureFormat(WMT::Device device, DXGI_FORMAT dxgi_format,
                           const MTL_DXGI_FORMAT_DESC &format) {
  const auto &traits = GetDXGIFormatTraits(dxgi_format);
  if (traits.flags & (DXGI_FORMAT_TRAIT_MULTIPLANE |
                      DXGI_FORMAT_TRAIT_VIDEO |
                      DXGI_FORMAT_TRAIT_DEPTH_STENCIL))
    return false;

  WMTTextureInfo info = {};
  info.pixel_format = format.PixelFormat;
  info.width = 1;
  info.height = 1;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = WMTTextureUsageShaderRead;

  WMTSparseTileSize tile_size = {};
  return device.sparseTileSize(info, tile_size);
}

static UINT8
GetD3D12FormatPlaneCountForFeatureData(DXGI_FORMAT dxgi_format,
                         const MTL_DXGI_FORMAT_DESC &format) {
  const auto &traits = GetDXGIFormatTraits(dxgi_format);
  if (traits.planeCount)
    return traits.planeCount;
  const uint32_t planes = DepthStencilPlanarFlags(format.PixelFormat);
  return planes == 3 ? 2 : 1;
}

static D3D12_FORMAT_SUPPORT1
GetD3D12TraitFormatSupport1(const DXGIFormatTraits &traits) {
  if (traits.classification == DXGIFormatClass::Mask ||
      traits.classification == DXGIFormatClass::Unsupported)
    return D3D12_FORMAT_SUPPORT1_NONE;

  if (traits.flags & DXGI_FORMAT_TRAIT_VIDEO) {
    return D3D12_FORMAT_SUPPORT1_TEXTURE2D |
           D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
           D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
           D3D12_FORMAT_SUPPORT1_SHADER_GATHER |
           D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT;
  }

  return D3D12_FORMAT_SUPPORT1_NONE;
}

static D3D12_FORMAT_SUPPORT2
GetD3D12TraitFormatSupport2(const DXGIFormatTraits &traits) {
  if (traits.classification == DXGIFormatClass::Mask ||
      traits.classification == DXGIFormatClass::Unsupported)
    return D3D12_FORMAT_SUPPORT2_NONE;
  return D3D12_FORMAT_SUPPORT2_NONE;
}

static bool
IsSupportedD3D12SampleCount(UINT sample_count) {
  return sample_count == 1 || sample_count == 2 || sample_count == 4 ||
         sample_count == 8;
}

static bool
IsSupportedCommandListType(D3D12_COMMAND_LIST_TYPE type) {
  return type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
         type == D3D12_COMMAND_LIST_TYPE_BUNDLE ||
         type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
         type == D3D12_COMMAND_LIST_TYPE_COPY;
}

static bool
IsSupportedCommandQueuePriority(UINT priority) {
  return priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL ||
         priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
}

static DescriptorRecordLease
GetDescriptorRecordForWrite(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                            D3D12_DESCRIPTOR_HEAP_TYPE expected_type,
                            const char *context) {
  auto record = d3d12::GetDescriptorRecordRangeFromCpuHandle(
      handle, expected_type, 1, context);
  if (!record)
    WARN("D3D12Device: invalid descriptor handle for ", context);
  return record;
}

static DescriptorHeapMirror::ScopedLock
LockDescriptorRecordForWrite(const DescriptorRecord &record) {
  return record.mirror ? record.mirror->AcquireLock()
                       : DescriptorHeapMirror::ScopedLock{};
}

static void
ResetDescriptorRecord(DescriptorRecord &record) {
  const auto magic = record.magic;
  const auto heap_type = record.heap_type;
  const auto shader_visible = record.shader_visible;
  const auto cpu_handle = record.cpu_handle;
  const auto heap_index = record.heap_index;
  const auto heap_count = record.heap_count;
  const auto slot_version = record.slot_version;
  auto *const mirror = record.mirror;
  record = {};
  record.magic = magic;
  record.heap_type = heap_type;
  record.shader_visible = shader_visible;
  record.cpu_handle = cpu_handle;
  record.heap_index = heap_index;
  record.heap_count = heap_count;
  record.slot_version = slot_version;
  record.mirror = mirror;
}

static Resource *
GetResourceFromD3D12(ID3D12Resource *resource) {
  if (!resource)
    return nullptr;
  Resource *out = nullptr;
  resource->QueryInterface(IID_DXMTResourceDowncast,
                           reinterpret_cast<void **>(&out));
  return out;
}

static bool
ResourceBelongsToDevice(ID3D12Resource *resource, IMTLD3D12Device *device) {
  if (!resource)
    return true;
  auto *state = GetResourceFromD3D12(resource);
  return state && state->GetParentDevice() == device;
}

static bool
DeviceChildBelongsToDevice(ID3D12DeviceChild *object,
                           IMTLD3D12Device *device) {
  if (!object)
    return false;
  IMTLD3D12Device *parent = nullptr;
  const HRESULT hr = object->GetDevice(
      __uuidof(IMTLD3D12Device), reinterpret_cast<void **>(&parent));
  const bool belongs = SUCCEEDED(hr) && parent == device;
  if (parent)
    parent->Release();
  return belongs;
}

static bool
IsSupportedResidencyObject(ID3D12Pageable *object,
                           IMTLD3D12Device *device) {
  if (!DeviceChildBelongsToDevice(object, device))
    return false;
  if (auto *resource = dynamic_cast<Resource *>(object))
    return resource->GetKind() == ResourceKind::Committed;
  return dynamic_cast<DescriptorHeap *>(object) ||
         dynamic_cast<Heap *>(object) || dynamic_cast<QueryHeap *>(object);
}

static bool
ValidateResidencyObjects(UINT object_count,
                         ID3D12Pageable *const *objects,
                         IMTLD3D12Device *device) {
  if (!object_count)
    return true;
  if (!objects)
    return false;
  for (UINT i = 0; i < object_count; ++i) {
    if (!IsSupportedResidencyObject(objects[i], device))
      return false;
  }
  return true;
}

static bool
IsBufferResource(ID3D12Resource *resource) {
  auto *d3d12_resource = dynamic_cast<Resource *>(resource);
  return d3d12_resource &&
         d3d12_resource->GetResourceDesc().Dimension ==
             D3D12_RESOURCE_DIMENSION_BUFFER;
}

static bool
IsTextureResource(ID3D12Resource *resource) {
  auto *d3d12_resource = dynamic_cast<Resource *>(resource);
  if (!d3d12_resource)
    return false;
  const auto dimension = d3d12_resource->GetResourceDesc().Dimension;
  return dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
         dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
         dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
}

static const D3D12_RESOURCE_DESC *
GetResourceDesc(ID3D12Resource *resource) {
  auto *d3d12_resource = dynamic_cast<Resource *>(resource);
  return d3d12_resource ? &d3d12_resource->GetResourceDesc() : nullptr;
}

static UINT
GetTextureMipDepth(const D3D12_RESOURCE_DESC &resource_desc, UINT mip_slice) {
  if (resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return 1;
  return static_cast<UINT>(
      std::max<UINT64>(1, resource_desc.DepthOrArraySize >> mip_slice));
}

static bool
IsSupportedSrvDimension(D3D12_SRV_DIMENSION dimension) {
  switch (dimension) {
  case D3D12_SRV_DIMENSION_BUFFER:
  case D3D12_SRV_DIMENSION_TEXTURE1D:
  case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
  case D3D12_SRV_DIMENSION_TEXTURE2D:
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
  case D3D12_SRV_DIMENSION_TEXTURE2DMS:
  case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
  case D3D12_SRV_DIMENSION_TEXTURE3D:
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    return true;
  default:
    return false;
  }
}

static bool
IsSupportedUavDimension(D3D12_UAV_DIMENSION dimension) {
  switch (dimension) {
  case D3D12_UAV_DIMENSION_BUFFER:
  case D3D12_UAV_DIMENSION_TEXTURE1D:
  case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
  case D3D12_UAV_DIMENSION_TEXTURE2D:
  case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
  case D3D12_UAV_DIMENSION_TEXTURE3D:
    return true;
  default:
    return false;
  }
}

static bool
ValidateShaderResourceView(ID3D12Resource *resource,
                           const D3D12_SHADER_RESOURCE_VIEW_DESC &desc) {
  if (!IsSupportedSrvDimension(desc.ViewDimension)) {
    WARN("D3D12Device: unsupported SRV dimension ",
         uint32_t(desc.ViewDimension));
    return false;
  }
  if (desc.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
    if (resource && !IsBufferResource(resource)) {
      WARN("D3D12Device: buffer SRV created for non-buffer resource");
      return false;
    }
    const auto raw = (desc.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0;
    const auto structured = desc.Buffer.StructureByteStride != 0;
    const auto typed =
        desc.Format != DXGI_FORMAT_UNKNOWN &&
        !(raw && desc.Format == DXGI_FORMAT_R32_TYPELESS);
    if ((raw && structured) || (raw && typed) || (structured && typed)) {
      WARN("D3D12Device: ambiguous buffer SRV typed/raw/structured descriptor");
      return false;
    }
    if (raw && desc.Format != DXGI_FORMAT_R32_TYPELESS &&
        desc.Format != DXGI_FORMAT_UNKNOWN) {
      WARN("D3D12Device: raw buffer SRV should use R32_TYPELESS/UNKNOWN format");
      return false;
    }
    return true;
  }
  if (resource && !IsTextureResource(resource)) {
    WARN("D3D12Device: texture SRV created for non-texture resource");
    return false;
  }
  const auto *resource_desc = GetResourceDesc(resource);
  if (resource_desc) {
    const bool resource_msaa = resource_desc->SampleDesc.Count > 1;
    const bool view_msaa =
        desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMS ||
        desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
    if (resource_msaa != view_msaa) {
      WARN("D3D12Device: SRV multisample dimension does not match resource sample count");
      return false;
    }
  }
  return true;
}

static bool
ValidateUnorderedAccessView(ID3D12Resource *resource,
                            ID3D12Resource *counter_resource,
                            const D3D12_UNORDERED_ACCESS_VIEW_DESC &desc) {
  if (!IsSupportedUavDimension(desc.ViewDimension)) {
    WARN("D3D12Device: unsupported UAV dimension ",
         uint32_t(desc.ViewDimension));
    return false;
  }
  if (desc.ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
    if (resource && !IsBufferResource(resource)) {
      WARN("D3D12Device: buffer UAV created for non-buffer resource");
      return false;
    }
    const auto raw = (desc.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0;
    const auto structured = desc.Buffer.StructureByteStride != 0;
    const auto typed =
        desc.Format != DXGI_FORMAT_UNKNOWN &&
        !(raw && desc.Format == DXGI_FORMAT_R32_TYPELESS);
    if ((raw && structured) || (raw && typed) || (structured && typed)) {
      WARN("D3D12Device: ambiguous buffer UAV typed/raw/structured descriptor");
      return false;
    }
    if (counter_resource) {
      if (!resource || !structured || !IsBufferResource(counter_resource)) {
        WARN("D3D12Device: UAV counters require data and counter buffers with a structured view");
        return false;
      }
      if (desc.Buffer.CounterOffsetInBytes %
          D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT) {
        WARN("D3D12Device: UAV counter offset is not aligned offset=",
             desc.Buffer.CounterOffsetInBytes);
        return false;
      }
      const auto *counter_desc = GetResourceDesc(counter_resource);
      if (!counter_desc ||
          desc.Buffer.CounterOffsetInBytes > counter_desc->Width ||
          counter_desc->Width - desc.Buffer.CounterOffsetInBytes <
              sizeof(UINT)) {
        WARN("D3D12Device: UAV counter offset is outside the counter buffer offset=",
             desc.Buffer.CounterOffsetInBytes,
             " size=", counter_desc ? counter_desc->Width : 0);
        return false;
      }
    } else if (desc.Buffer.CounterOffsetInBytes) {
      WARN("D3D12Device: UAV counter offset requires a counter resource");
      return false;
    }
    return true;
  }
  if (resource && !IsTextureResource(resource)) {
    WARN("D3D12Device: texture UAV created for non-texture resource");
    return false;
  }
  const auto *resource_desc = GetResourceDesc(resource);
  if (resource_desc && resource_desc->SampleDesc.Count > 1) {
    WARN("D3D12Device: UAVs cannot be created for multisampled resources");
    return false;
  }
  if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D) {
    if (resource_desc && resource_desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
      WARN("D3D12Device: 3D texture UAV created for non-3D texture resource");
      return false;
    }
    if (resource_desc && desc.Texture3D.MipSlice >= resource_desc->MipLevels) {
      WARN("D3D12Device: 3D texture UAV mip slice out of range ",
           desc.Texture3D.MipSlice);
      return false;
    }
    if (resource_desc) {
      const UINT mip_depth = GetTextureMipDepth(*resource_desc, desc.Texture3D.MipSlice);
      const UINT first_w = desc.Texture3D.FirstWSlice;
      const UINT w_size = desc.Texture3D.WSize == UINT_MAX
                              ? (first_w < mip_depth ? mip_depth - first_w : 0)
                              : desc.Texture3D.WSize;
      if (first_w >= mip_depth || w_size == 0 || w_size > mip_depth - first_w) {
        WARN("D3D12Device: invalid 3D texture UAV W slice range first=",
             first_w, " size=", w_size, " mip_depth=", mip_depth);
        return false;
      }
    }
  }
  if (counter_resource) {
    WARN("D3D12Device: UAV counter resource is ignored for texture UAVs");
    return false;
  }
  return true;
}

static bool
ValidateRenderTargetView(ID3D12Resource *resource,
                         const D3D12_RENDER_TARGET_VIEW_DESC &desc) {
  const auto *resource_desc = GetResourceDesc(resource);
  if (!resource_desc || !IsTextureResource(resource))
    return true;
  const bool resource_msaa = resource_desc->SampleDesc.Count > 1;
  const bool view_msaa =
      desc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DMS ||
      desc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
  if (resource_msaa != view_msaa) {
    WARN("D3D12Device: RTV multisample dimension does not match resource sample count");
    return false;
  }
  if (desc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE3D) {
    if (resource_desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D ||
        desc.Texture3D.MipSlice >= resource_desc->MipLevels) {
      WARN("D3D12Device: 3D RTV dimension or mip does not match resource");
      return false;
    }
    const UINT mip_depth =
        GetTextureMipDepth(*resource_desc, desc.Texture3D.MipSlice);
    const UINT first_w = desc.Texture3D.FirstWSlice;
    const UINT w_size = desc.Texture3D.WSize == UINT_MAX
                            ? (first_w < mip_depth ? mip_depth - first_w : 0)
                            : desc.Texture3D.WSize;
    if (first_w >= mip_depth || w_size == 0 || w_size > mip_depth - first_w) {
      WARN("D3D12Device: invalid 3D RTV W slice range first=", first_w,
           " size=", w_size, " mip_depth=", mip_depth);
      return false;
    }
  }
  return true;
}

static bool
ValidateDepthStencilView(ID3D12Resource *resource,
                         const D3D12_DEPTH_STENCIL_VIEW_DESC &desc) {
  auto log_invalid = [&](const char *reason) {
    const auto *resource_desc = GetResourceDesc(resource);
    WARN("D3D12Device: invalid DSV descriptor reason=", reason,
         " resource=", reinterpret_cast<uintptr_t>(resource),
         " resource_dimension=", resource_desc ? uint32_t(resource_desc->Dimension) : 0,
         " resource_size=", resource_desc ? uint64_t(resource_desc->Width) : 0,
         "x", resource_desc ? uint32_t(resource_desc->Height) : 0,
         "x", resource_desc ? uint32_t(resource_desc->DepthOrArraySize) : 0,
         " resource_mips=", resource_desc ? uint32_t(resource_desc->MipLevels) : 0,
         " resource_format=", resource_desc ? uint32_t(resource_desc->Format) : 0,
         " resource_samples=", resource_desc ? uint32_t(resource_desc->SampleDesc.Count) : 0,
         " view_format=", uint32_t(desc.Format),
         " view_dimension=", uint32_t(desc.ViewDimension),
         " flags=", uint32_t(desc.Flags),
         " tex1d_mip=", uint32_t(desc.Texture1D.MipSlice),
         " tex1d_array_mip=", uint32_t(desc.Texture1DArray.MipSlice),
         " tex1d_array_first=", uint32_t(desc.Texture1DArray.FirstArraySlice),
         " tex1d_array_size=", uint32_t(desc.Texture1DArray.ArraySize),
         " tex2d_mip=", uint32_t(desc.Texture2D.MipSlice),
         " tex2d_array_mip=", uint32_t(desc.Texture2DArray.MipSlice),
         " tex2d_array_first=", uint32_t(desc.Texture2DArray.FirstArraySlice),
         " tex2d_array_size=", uint32_t(desc.Texture2DArray.ArraySize),
         " tex2dms_array_first=", uint32_t(desc.Texture2DMSArray.FirstArraySlice),
         " tex2dms_array_size=", uint32_t(desc.Texture2DMSArray.ArraySize));
  };

  switch (desc.ViewDimension) {
  case D3D12_DSV_DIMENSION_TEXTURE1D:
  case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
  case D3D12_DSV_DIMENSION_TEXTURE2D:
  case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
  case D3D12_DSV_DIMENSION_TEXTURE2DMS:
  case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
    break;
  default:
    log_invalid("unsupported dimension");
    return false;
  }

  const auto *resource_desc = GetResourceDesc(resource);
  if (!resource_desc)
    return true;
  if (!IsTextureResource(resource)) {
    log_invalid("non-texture resource");
    return false;
  }

  const bool is_1d_view =
      desc.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE1D ||
      desc.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
  const bool is_2d_view =
      desc.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D ||
      desc.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY ||
      desc.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMS ||
      desc.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
  if ((is_1d_view && resource_desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D) ||
      (is_2d_view && resource_desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)) {
    log_invalid("view dimension does not match resource dimension");
    return false;
  }

  const bool resource_msaa = resource_desc->SampleDesc.Count > 1;
  const bool view_msaa =
      desc.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMS ||
      desc.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
  if (resource_msaa != view_msaa) {
    log_invalid("multisample dimension does not match resource sample count");
    return false;
  }

  auto validate_mip = [&](UINT mip) {
    if (mip >= resource_desc->MipLevels) {
      log_invalid("mip slice out of range");
      return false;
    }
    return true;
  };
  auto validate_array = [&](UINT first, UINT count) {
    if (count == 0 || first >= resource_desc->DepthOrArraySize ||
        count > resource_desc->DepthOrArraySize - first) {
      log_invalid("array slice range out of range");
      return false;
    }
    return true;
  };

  switch (desc.ViewDimension) {
  case D3D12_DSV_DIMENSION_TEXTURE1D:
    return validate_mip(desc.Texture1D.MipSlice);
  case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
    return validate_mip(desc.Texture1DArray.MipSlice) &&
           validate_array(desc.Texture1DArray.FirstArraySlice,
                          desc.Texture1DArray.ArraySize);
  case D3D12_DSV_DIMENSION_TEXTURE2D:
    return validate_mip(desc.Texture2D.MipSlice);
  case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
    return validate_mip(desc.Texture2DArray.MipSlice) &&
           validate_array(desc.Texture2DArray.FirstArraySlice,
                          desc.Texture2DArray.ArraySize);
  case D3D12_DSV_DIMENSION_TEXTURE2DMS:
    return true;
  case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
    return validate_array(desc.Texture2DMSArray.FirstArraySlice,
                          desc.Texture2DMSArray.ArraySize);
  default:
    return false;
  }
}

static void
CopyDescriptorRecord(DescriptorRecord &dst, const DescriptorRecord &src) {
  const auto magic = dst.magic;
  const auto heap_type = dst.heap_type;
  const auto shader_visible = dst.shader_visible;
  const auto cpu_handle = dst.cpu_handle;
  const auto heap_index = dst.heap_index;
  const auto heap_count = dst.heap_count;
  auto *const mirror = dst.mirror;
  dst = src;
  dst.magic = magic;
  dst.heap_type = heap_type;
  dst.shader_visible = shader_visible;
  dst.cpu_handle = cpu_handle;
  dst.heap_index = heap_index;
  dst.heap_count = heap_count;
  dst.mirror = mirror;
}

static bool
DescriptorWriteAffectsShaderBinding(const DescriptorRecord &record) {
  return record.shader_visible &&
         (record.heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
          record.heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
}

class DescriptorContentRevisionCommitGuard {
public:
  explicit DescriptorContentRevisionCommitGuard(bool armed) : armed_(armed) {}
  DescriptorContentRevisionCommitGuard(
      const DescriptorContentRevisionCommitGuard &) = delete;
  DescriptorContentRevisionCommitGuard &operator=(
      const DescriptorContentRevisionCommitGuard &) = delete;
  ~DescriptorContentRevisionCommitGuard() {
    if (armed_)
      BumpDescriptorContentRevision();
  }

private:
  bool armed_ = false;
};

static void
BeginDescriptorSlotWrite(
    const DescriptorHeapMirror::ScopedLock &mirror_lock,
    DescriptorRecord &record,
    DescriptorRecordType write_type = DescriptorRecordType::Empty) {
  if (DescriptorWriteAffectsShaderBinding(record)) {
    record.slot_version =
        record.mirror->BeginSlotWrite(mirror_lock, record.heap_index);
    const auto type =
        write_type == DescriptorRecordType::Empty ? record.type : write_type;
    switch (type) {
    case DescriptorRecordType::ConstantBufferView:
      dxmt::perf::recordDescriptorContentWrite(1);
      break;
    case DescriptorRecordType::ShaderResourceView:
      dxmt::perf::recordDescriptorContentWrite(2);
      break;
    case DescriptorRecordType::UnorderedAccessView:
      dxmt::perf::recordDescriptorContentWrite(3);
      break;
    case DescriptorRecordType::Sampler:
      dxmt::perf::recordDescriptorContentWrite(4);
      break;
    default:
      dxmt::perf::recordDescriptorContentWrite(0);
      break;
    }
  }
}

static void
RecordDescriptorContentCopyPerf() {
  dxmt::perf::recordDescriptorContentWrite(5);
}

// Bindless mirror generation marker for descriptor writes. Phase 2 materializes
// texture, sampler, buffer texture-view, and null descriptors immediately.
// record.mirror is null unless the heap is a shader-visible CBV/SRV/UAV or
// SAMPLER heap, so non-shader heaps do nothing here.
static void
MaterializeSamplerMirrorForWrite(WMT::Device device,
                                 const DescriptorHeapMirror::ScopedLock &mirror_lock,
                                 DescriptorRecord &record) {
  if (!record.mirror || !record.mirror->isSamplerHeap())
    return;
  if (record.type != DescriptorRecordType::Sampler || !record.has_desc) {
    record.materialized_sampler = nullptr;
    return;
  }

  auto sampler = CreateD3D12Sampler(device, record.desc.sampler);
  if (!sampler)
    return;
  record.materialized_sampler = std::move(sampler);
  record.mirror->FillSamplerSlot(mirror_lock, record.heap_index,
                                 record.materialized_sampler.ptr(), 0);
}

struct BufferDescriptorMaterialization {
  UINT64 view_offset = 0;
  uint64_t byte_size = 0;
  uint32_t stride = 0;
  uint32_t format = DXGI_FORMAT_UNKNOWN;
  uint32_t flags = 0;
  bool typed = false;
};

struct BufferTextureViewMaterialization {
  WMTTextureBufferViewDescriptor descriptor = {};
  uint64_t offset = 0;
  uint64_t bytes_per_row = 0;
  uint32_t element_count = 0;
  uint32_t first_element = 0;
};

static DXGI_FORMAT
BufferTextureViewFormat(const BufferDescriptorMaterialization &view) {
  if (view.typed)
    return static_cast<DXGI_FORMAT>(view.format);
  if (view.flags & BufferDescriptorRecordFlagRaw)
    return DXGI_FORMAT_R32_UINT;
  if (view.flags & BufferDescriptorRecordFlagStructured) {
    switch (view.stride) {
    case 4:
      return DXGI_FORMAT_R32_UINT;
    case 8:
      return DXGI_FORMAT_R32G32_UINT;
    case 16:
      return DXGI_FORMAT_R32G32B32A32_UINT;
    default:
      return DXGI_FORMAT_UNKNOWN;
    }
  }
  return DXGI_FORMAT_R32_UINT;
}

static bool
BuildBufferTextureViewMaterialization(
    WMT::Device device, Resource &resource,
    const BufferDescriptorMaterialization &view,
    DescriptorRecordType descriptor_type,
    BufferTextureViewMaterialization &out) {
  out = {};
  auto *allocation = resource.GetBufferAllocation();
  if (!allocation || !allocation->buffer())
    return false;

  const auto dxgi_format = BufferTextureViewFormat(view);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN)
    return false;

  MTL_DXGI_FORMAT_DESC format = {};
  if (FAILED(MTLQueryDXGIFormat(device, dxgi_format, format)) ||
      format.PixelFormat == WMTPixelFormatInvalid || !format.BytesPerTexel)
    return false;

  const uint64_t resource_offset =
      resource.GetHeapOffset() + allocation->currentSuballocationOffset();
  if (resource_offset < resource.GetHeapOffset() ||
      view.view_offset > UINT64_MAX - resource_offset)
    return false;
  const uint64_t backing_offset = resource_offset + view.view_offset;
  if (backing_offset > allocation->length())
    return false;

  const uint64_t byte_size =
      std::min<uint64_t>(view.byte_size, allocation->length() - backing_offset);
  if (!byte_size || backing_offset % format.BytesPerTexel ||
      byte_size % format.BytesPerTexel)
    return false;

  const uint64_t alignment = std::max<uint64_t>(
      format.BytesPerTexel,
      device.minimumLinearTextureAlignmentForPixelFormat(format.PixelFormat));
  const uint64_t aligned_offset = backing_offset - backing_offset % alignment;
  const uint64_t bias_bytes = backing_offset - aligned_offset;
  const uint64_t view_byte_size = bias_bytes + byte_size;
  if (bias_bytes % format.BytesPerTexel || aligned_offset > UINT32_MAX ||
      view_byte_size > UINT32_MAX)
    return false;

  const uint64_t element_stride = view.stride ? view.stride : format.BytesPerTexel;
  if (!element_stride || byte_size % element_stride ||
      byte_size / element_stride > UINT32_MAX ||
      bias_bytes / format.BytesPerTexel > UINT32_MAX)
    return false;

  auto &texture = out.descriptor.texture;
  texture.type = WMTTextureTypeTextureBuffer;
  texture.width = view_byte_size / format.BytesPerTexel;
  texture.height = 1;
  texture.depth = 1;
  texture.array_length = 1;
  texture.mipmap_level_count = 1;
  texture.sample_count = 1;
  texture.pixel_format = format.PixelFormat;
  texture.options = allocation->resourceOptions();
  texture.usage = descriptor_type == DescriptorRecordType::UnorderedAccessView
                      ? static_cast<WMTTextureUsage>(
                            WMTTextureUsageShaderRead |
                            WMTTextureUsageShaderWrite)
                      : WMTTextureUsageShaderRead;
  if (descriptor_type == DescriptorRecordType::UnorderedAccessView &&
      (format.PixelFormat == WMTPixelFormatR32Uint ||
       format.PixelFormat == WMTPixelFormatR32Sint ||
       (format.PixelFormat == WMTPixelFormatRG32Uint &&
        device.supportsFamily(WMTGPUFamilyApple8))))
    texture.usage = static_cast<WMTTextureUsage>(texture.usage |
                                                 WMTTextureUsageShaderAtomic);

  out.offset = aligned_offset;
  out.bytes_per_row = view_byte_size;
  out.element_count = static_cast<uint32_t>(byte_size / element_stride);
  out.first_element =
      static_cast<uint32_t>(bias_bytes / format.BytesPerTexel);
  return true;
}

static bool
GetBufferSrvMaterialization(WMT::Device device, Resource &resource,
                            const DescriptorRecord &record,
                            BufferDescriptorMaterialization &out) {
  out = {};
  out.byte_size = resource.GetResourceDesc().Width;
  out.flags = BufferDescriptorRecordFlagSRV;

  if (record.has_desc) {
    const auto &srv = record.desc.srv;
    if (srv.ViewDimension != D3D12_SRV_DIMENSION_BUFFER)
      return false;

    const UINT64 first_element = srv.Buffer.FirstElement;
    if (srv.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) {
      out.view_offset += first_element * sizeof(uint32_t);
      out.byte_size = UINT64(srv.Buffer.NumElements) * sizeof(uint32_t);
      out.stride = sizeof(uint32_t);
      out.flags |= BufferDescriptorRecordFlagRaw;
    } else if (srv.Format != DXGI_FORMAT_UNKNOWN) {
      MTL_DXGI_FORMAT_DESC format = {};
      if (FAILED(MTLQueryDXGIFormat(device, srv.Format, format)) ||
          !format.BytesPerTexel)
        return false;
      out.view_offset += first_element * format.BytesPerTexel;
      out.byte_size = UINT64(srv.Buffer.NumElements) * format.BytesPerTexel;
      out.stride = format.BytesPerTexel;
      out.format = srv.Format;
      out.typed = true;
      out.flags |= BufferDescriptorRecordFlagTyped;
    } else if (srv.Buffer.StructureByteStride) {
      out.view_offset += first_element * srv.Buffer.StructureByteStride;
      out.byte_size = UINT64(srv.Buffer.NumElements) *
                      srv.Buffer.StructureByteStride;
      out.stride = srv.Buffer.StructureByteStride;
      out.flags |= BufferDescriptorRecordFlagStructured;
    } else {
      out.view_offset += first_element;
      out.byte_size = srv.Buffer.NumElements;
    }
  }

  return true;
}

static bool
GetBufferUavMaterialization(WMT::Device device, Resource &resource,
                            const DescriptorRecord &record,
                            BufferDescriptorMaterialization &out) {
  out = {};
  out.byte_size = resource.GetResourceDesc().Width;
  out.flags = BufferDescriptorRecordFlagUAV;

  if (record.has_desc) {
    const auto &uav = record.desc.uav;
    if (uav.ViewDimension != D3D12_UAV_DIMENSION_BUFFER)
      return false;

    const UINT64 first_element = uav.Buffer.FirstElement;
    if (uav.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) {
      out.view_offset += first_element * sizeof(uint32_t);
      out.byte_size = UINT64(uav.Buffer.NumElements) * sizeof(uint32_t);
      out.stride = sizeof(uint32_t);
      out.flags |= BufferDescriptorRecordFlagRaw;
    } else if (uav.Format != DXGI_FORMAT_UNKNOWN) {
      MTL_DXGI_FORMAT_DESC format = {};
      if (FAILED(MTLQueryDXGIFormat(device, uav.Format, format)) ||
          !format.BytesPerTexel)
        return false;
      out.view_offset += first_element * format.BytesPerTexel;
      out.byte_size = UINT64(uav.Buffer.NumElements) * format.BytesPerTexel;
      out.stride = format.BytesPerTexel;
      out.format = uav.Format;
      out.typed = true;
      out.flags |= BufferDescriptorRecordFlagTyped;
    } else if (uav.Buffer.StructureByteStride) {
      out.view_offset += first_element * uav.Buffer.StructureByteStride;
      out.byte_size = UINT64(uav.Buffer.NumElements) *
                      uav.Buffer.StructureByteStride;
      out.stride = uav.Buffer.StructureByteStride;
      out.flags |= BufferDescriptorRecordFlagStructured;
    } else {
      out.view_offset += first_element;
      out.byte_size = uav.Buffer.NumElements;
    }
  }

  return true;
}

static bool
ValidateDescriptorTextureViewRange(const char *context,
                                   TextureViewDescriptor &view,
                                   const Resource &resource) {
  const auto *texture = resource.GetTexture();
  if (!texture)
    return false;

  if (view.firstMiplevel >= texture->miplevelCount() ||
      view.miplevelCount == 0 ||
      view.miplevelCount > texture->miplevelCount() - view.firstMiplevel) {
    WARN("D3D12Device: ", context,
         " mip range exceeds texture levels first=", view.firstMiplevel,
         " count=", view.miplevelCount,
         " levels=", texture->miplevelCount());
    return false;
  }

  if (resource.GetResourceDesc().Dimension ==
      D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
    view.firstArraySlice = 0;
    view.arraySize = 1;
    return true;
  }

  if (view.firstArraySlice >= texture->arrayLength() ||
      view.arraySize == 0 ||
      view.arraySize > texture->arrayLength() - view.firstArraySlice) {
    WARN("D3D12Device: ", context,
         " array range exceeds texture array first=", view.firstArraySlice,
         " count=", view.arraySize, " array_length=", texture->arrayLength());
    return false;
  }
  return true;
}

struct DescriptorTextureViewBinding {
  Rc<Texture> texture;
  TextureViewKey view;
  uint32_t array_length = 0;

  explicit operator bool() const {
    return texture && uint64_t(view);
  }
};

static DescriptorTextureViewBinding
CreateDescriptorShaderResourceTextureView(WMT::Device device,
                                          Resource &resource,
                                          const DescriptorRecord &record) {
  auto *texture = resource.GetTexture();
  if (!texture)
    return {};

  TextureViewDescriptor view = {};
  view.format = texture->pixelFormat();
  view.type = texture->textureType();
  view.firstMiplevel = 0;
  view.miplevelCount = texture->miplevelCount();
  view.firstArraySlice = 0;
  view.arraySize = texture->arrayLength();
  view.intendedUsage = WMTTextureUsageShaderRead;
  view.swizzle =
      ShaderResourceViewSwizzle(view.format,
                                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);

  Rc<Texture> view_texture(texture);
  if (record.has_desc) {
    const auto &srv = record.desc.srv;
    UINT plane = 0;
    switch (srv.ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE2D:
      plane = srv.Texture2D.PlaneSlice;
      break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
      plane = srv.Texture2DArray.PlaneSlice;
      break;
    default:
      break;
    }
    if (GetDescriptorResourcePlaneCount(resource) > 1 &&
        srv.Format != DXGI_FORMAT_UNKNOWN &&
        !IsDXGIFormatPlaneCompatible(resource.GetResourceDesc().Format,
                                     srv.Format, plane)) {
      WARN("D3D12Device: unsupported SRV plane format resource_format=",
           uint32_t(resource.GetResourceDesc().Format),
           " view_format=", uint32_t(srv.Format),
           " plane=", uint32_t(plane));
      return {};
    }

    auto *plane_texture = resource.GetTexture(plane);
    if (!plane_texture)
      return {};
    view_texture = Rc<Texture>(plane_texture);
    view.format = ResolveTextureViewFormat(device, resource, srv.Format, plane,
                                           "D3D12Device");
    if (view.format == WMTPixelFormatInvalid)
      return {};
    view.swizzle =
        ShaderResourceViewSwizzle(view.format, srv.Shader4ComponentMapping);

    switch (srv.ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D:
      view.type = WMTTextureType2D;
      view.firstMiplevel = srv.Texture1D.MostDetailedMip;
      view.miplevelCount = NormalizeDescriptorViewCount(
          srv.Texture1D.MipLevels, view.firstMiplevel,
          texture->miplevelCount());
      view.arraySize = 1;
      break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
      view.type = WMTTextureType2DArray;
      view.firstMiplevel = srv.Texture1DArray.MostDetailedMip;
      view.miplevelCount = NormalizeDescriptorViewCount(
          srv.Texture1DArray.MipLevels, view.firstMiplevel,
          texture->miplevelCount());
      view.firstArraySlice = srv.Texture1DArray.FirstArraySlice;
      view.arraySize = NormalizeDescriptorViewCount(
          srv.Texture1DArray.ArraySize, view.firstArraySlice,
          texture->arrayLength());
      break;
    case D3D12_SRV_DIMENSION_TEXTURE2D:
      view.type = WMTTextureType2D;
      view.firstMiplevel = srv.Texture2D.MostDetailedMip;
      view.miplevelCount = NormalizeDescriptorViewCount(
          srv.Texture2D.MipLevels, view.firstMiplevel,
          texture->miplevelCount());
      view.firstArraySlice = 0;
      view.arraySize = 1;
      break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
      view.type = WMTTextureType2DArray;
      view.firstMiplevel = srv.Texture2DArray.MostDetailedMip;
      view.miplevelCount = NormalizeDescriptorViewCount(
          srv.Texture2DArray.MipLevels, view.firstMiplevel,
          texture->miplevelCount());
      view.firstArraySlice = srv.Texture2DArray.FirstArraySlice;
      view.arraySize = NormalizeDescriptorViewCount(
          srv.Texture2DArray.ArraySize, view.firstArraySlice,
          texture->arrayLength());
      break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS:
      view.type = WMTTextureType2DMultisample;
      view.miplevelCount = 1;
      view.arraySize = 1;
      break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
      view.type = WMTTextureType2DMultisampleArray;
      view.miplevelCount = 1;
      view.firstArraySlice = srv.Texture2DMSArray.FirstArraySlice;
      view.arraySize = NormalizeDescriptorViewCount(
          srv.Texture2DMSArray.ArraySize, view.firstArraySlice,
          texture->arrayLength());
      break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
      view.type = WMTTextureType3D;
      view.firstMiplevel = srv.Texture3D.MostDetailedMip;
      view.miplevelCount = NormalizeDescriptorViewCount(
          srv.Texture3D.MipLevels, view.firstMiplevel,
          texture->miplevelCount());
      view.arraySize = 1;
      break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
      view.type = WMTTextureTypeCube;
      view.firstMiplevel = srv.TextureCube.MostDetailedMip;
      view.miplevelCount = NormalizeDescriptorViewCount(
          srv.TextureCube.MipLevels, view.firstMiplevel,
          texture->miplevelCount());
      view.arraySize = std::min<UINT>(6, texture->arrayLength());
      break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
      view.type = WMTTextureTypeCubeArray;
      view.firstMiplevel = srv.TextureCubeArray.MostDetailedMip;
      view.miplevelCount = NormalizeDescriptorViewCount(
          srv.TextureCubeArray.MipLevels, view.firstMiplevel,
          texture->miplevelCount());
      view.firstArraySlice = srv.TextureCubeArray.First2DArrayFace;
      view.arraySize = NormalizeDescriptorViewCount(
          srv.TextureCubeArray.NumCubes * 6, view.firstArraySlice,
          texture->arrayLength());
      break;
    default:
      WARN("D3D12Device: unsupported SRV texture dimension ",
           uint32_t(srv.ViewDimension));
      return {};
    }
  }

  if (!ValidateDescriptorTextureViewRange("SRV texture view", view, resource))
    return {};
  auto key = view_texture->createView(view);
  return {std::move(view_texture), key, view.arraySize};
}

static DescriptorTextureViewBinding
CreateDescriptorUnorderedAccessTextureView(WMT::Device device,
                                           Resource &resource,
                                           const DescriptorRecord &record) {
  auto *texture = resource.GetTexture();
  if (!texture)
    return {};

  TextureViewDescriptor view = {};
  view.format = texture->pixelFormat();
  view.type = texture->textureType();
  view.firstMiplevel = 0;
  view.miplevelCount = 1;
  view.firstArraySlice = 0;
  view.arraySize = texture->arrayLength();
  view.intendedUsage =
      WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite;

  Rc<Texture> view_texture(texture);
  if (record.has_desc) {
    const auto &uav = record.desc.uav;
    UINT plane = 0;
    switch (uav.ViewDimension) {
    case D3D12_UAV_DIMENSION_TEXTURE2D:
      plane = uav.Texture2D.PlaneSlice;
      break;
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
      plane = uav.Texture2DArray.PlaneSlice;
      break;
    default:
      break;
    }
    if (GetDescriptorResourcePlaneCount(resource) > 1 &&
        uav.Format != DXGI_FORMAT_UNKNOWN &&
        !IsDXGIFormatPlaneCompatible(resource.GetResourceDesc().Format,
                                     uav.Format, plane)) {
      WARN("D3D12Device: unsupported UAV plane format resource_format=",
           uint32_t(resource.GetResourceDesc().Format),
           " view_format=", uint32_t(uav.Format),
           " plane=", uint32_t(plane));
      return {};
    }

    auto *plane_texture = resource.GetTexture(plane);
    if (!plane_texture)
      return {};
    view_texture = Rc<Texture>(plane_texture);
    view.format = ResolveTextureViewFormat(device, resource, uav.Format, plane,
                                           "D3D12Device");
    if (view.format == WMTPixelFormatInvalid)
      return {};

    switch (uav.ViewDimension) {
    case D3D12_UAV_DIMENSION_TEXTURE1D:
      view.type = WMTTextureType2D;
      view.firstMiplevel = uav.Texture1D.MipSlice;
      view.arraySize = 1;
      break;
    case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
      view.type = WMTTextureType2DArray;
      view.firstMiplevel = uav.Texture1DArray.MipSlice;
      view.firstArraySlice = uav.Texture1DArray.FirstArraySlice;
      view.arraySize = NormalizeDescriptorViewCount(
          uav.Texture1DArray.ArraySize, view.firstArraySlice,
          texture->arrayLength());
      break;
    case D3D12_UAV_DIMENSION_TEXTURE2D:
      view.type = WMTTextureType2D;
      view.firstMiplevel = uav.Texture2D.MipSlice;
      view.arraySize = 1;
      break;
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
      view.type = WMTTextureType2DArray;
      view.firstMiplevel = uav.Texture2DArray.MipSlice;
      view.firstArraySlice = uav.Texture2DArray.FirstArraySlice;
      view.arraySize = NormalizeDescriptorViewCount(
          uav.Texture2DArray.ArraySize, view.firstArraySlice,
          texture->arrayLength());
      break;
    case D3D12_UAV_DIMENSION_TEXTURE3D:
      view.type = WMTTextureType3D;
      view.firstMiplevel = uav.Texture3D.MipSlice;
      view.arraySize = 1;
      if (view.firstMiplevel >= texture->miplevelCount()) {
        WARN("D3D12Device: invalid 3D texture UAV mip slice ",
             view.firstMiplevel);
        return {};
      }
      {
        const UINT mip_depth =
            GetDescriptorMipDepth(resource, view.firstMiplevel);
        const UINT first_w = uav.Texture3D.FirstWSlice;
        const UINT w_size = uav.Texture3D.WSize == UINT_MAX
                                ? (first_w < mip_depth ? mip_depth - first_w
                                                       : 0)
                                : uav.Texture3D.WSize;
        if (first_w >= mip_depth || w_size == 0 ||
            w_size > mip_depth - first_w) {
          WARN("D3D12Device: invalid 3D texture UAV W slice range first=",
               first_w, " size=", w_size, " mip_depth=", mip_depth);
          return {};
        }
        if (first_w != 0 || w_size != mip_depth) {
          WARN("D3D12Device: unsupported 3D texture UAV W slice subrange first=",
               first_w, " size=", w_size, " mip_depth=", mip_depth);
          return {};
        }
      }
      break;
    default:
      WARN("D3D12Device: unsupported UAV texture dimension ",
           uint32_t(uav.ViewDimension));
      return {};
    }
  }

  if (!ValidateDescriptorTextureViewRange("UAV texture view", view, resource))
    return {};
  auto key = view_texture->createView(view);
  return {std::move(view_texture), key, view.arraySize};
}

static float
GetShaderResourceTextureMinLod(const DescriptorRecord &record) {
  if (!record.has_desc)
    return 0.0f;
  const auto &srv = record.desc.srv;
  switch (srv.ViewDimension) {
  case D3D12_SRV_DIMENSION_TEXTURE1D:
    return srv.Texture1D.ResourceMinLODClamp;
  case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
    return srv.Texture1DArray.ResourceMinLODClamp;
  case D3D12_SRV_DIMENSION_TEXTURE2D:
    return srv.Texture2D.ResourceMinLODClamp;
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
    return srv.Texture2DArray.ResourceMinLODClamp;
  case D3D12_SRV_DIMENSION_TEXTURE3D:
    return srv.Texture3D.ResourceMinLODClamp;
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
    return srv.TextureCube.ResourceMinLODClamp;
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    return srv.TextureCubeArray.ResourceMinLODClamp;
  default:
    return 0.0f;
  }
}

static uint64_t
DescriptorBufferResourceIdentity(Resource *resource) {
  return resource ? resource->GetDescriptorIdentity() : 0;
}

static WMT::Resource
DescriptorBufferResidencyAllocation(Resource *resource) {
  if (!resource || !resource->GetBufferAllocation())
    return {};
  return WMT::Resource{resource->GetBufferAllocation()->buffer().handle};
}

static bool
GetBufferResourceTableBinding(Resource *resource, WMT::Resource &allocation,
                              uint64_t &gpu_address, uint64_t &byte_size) {
  allocation = {};
  gpu_address = 0;
  byte_size = 0;

  if (!resource || !resource->GetBufferAllocation())
    return false;

  auto *buffer_allocation = resource->GetBufferAllocation();
  allocation = WMT::Resource{buffer_allocation->buffer().handle};
  if (!allocation)
    return false;

  gpu_address =
      buffer_allocation->gpuAddress() +
      buffer_allocation->currentSuballocationOffset();
  byte_size = resource->GetResourceDesc().Width;
  return byte_size != 0;
}

static bool
RegisterBufferDescriptorResource(DescriptorHeapMirror &mirror, UINT slot,
                                 const DescriptorHeapMirror::ScopedLock &mirror_lock,
                                 bool counter_resource,
                                 Resource *resource,
                                 uint32_t &resource_index) {
  WMT::Resource allocation;
  uint64_t gpu_address = 0;
  uint64_t byte_size = 0;
  if (!GetBufferResourceTableBinding(resource, allocation, gpu_address,
                                     byte_size)) {
    resource_index = kNullDescriptorResourceIndex;
    return false;
  }

  const auto before_generation =
      mirror.backendResourceTableGeneration(mirror_lock);
  resource_index = mirror.RegisterBufferResource(
      mirror_lock, slot, counter_resource,
      DescriptorBufferResourceIdentity(resource), allocation, gpu_address,
      byte_size);
  if (mirror.backendResourceTableGeneration(mirror_lock) != before_generation)
    dxmt::perf::recordNativeDescriptorResourceTableEntry();
  return resource_index != kNullDescriptorResourceIndex;
}

static bool
WriteNativeBufferDescriptorRecord(DescriptorHeapMirror &mirror, UINT slot,
                                  const DescriptorHeapMirror::ScopedLock &mirror_lock,
                                  Resource *resource,
                                  const BufferDescriptorMaterialization &view,
                                  DescriptorRecordType type) {
  BufferDescriptorRecord native = {};
  native.flags = BufferDescriptorRecordFlagValid | view.flags;
  native.byte_size = view.byte_size;
  native.stride = view.stride;
  native.format = view.format;

  if (type == DescriptorRecordType::ConstantBufferView)
    native.flags |= BufferDescriptorRecordFlagCBV;

  if (!RegisterBufferDescriptorResource(mirror, slot, mirror_lock, false, resource,
                                        native.resource_index)) {
    mirror.WriteBufferDescriptorRecord(mirror_lock, slot, {});
    dxmt::perf::recordNativeDescriptorBufferRecordMissingResource();
    return false;
  }

  native.byte_offset = resource->GetHeapOffset() + view.view_offset;
  mirror.WriteBufferDescriptorRecord(mirror_lock, slot, native);

  switch (type) {
  case DescriptorRecordType::ConstantBufferView:
    dxmt::perf::recordNativeDescriptorBufferRecord(1);
    break;
  case DescriptorRecordType::ShaderResourceView:
    dxmt::perf::recordNativeDescriptorBufferRecord(2);
    break;
  case DescriptorRecordType::UnorderedAccessView:
    dxmt::perf::recordNativeDescriptorBufferRecord(3);
    break;
  default:
    break;
  }
  return true;
}

static bool
WriteNativeUavCounterRecord(DescriptorHeapMirror &mirror, UINT slot,
                            const DescriptorHeapMirror::ScopedLock &mirror_lock,
                            const DescriptorRecord &record) {
  if (record.type != DescriptorRecordType::UnorderedAccessView ||
      !record.counter_resource.ptr())
    return false;

  auto current = mirror.bufferDescriptorRecord(mirror_lock, slot);
  if (!current || !(current->flags & BufferDescriptorRecordFlagValid)) {
    dxmt::perf::recordNativeDescriptorBufferRecordMissingResource();
    return false;
  }

  auto native = *current;
  auto *counter = GetResourceFromD3D12(record.counter_resource.ptr());
  if (!RegisterBufferDescriptorResource(mirror, slot, mirror_lock, true, counter,
                                        native.counter_resource_index)) {
    dxmt::perf::recordNativeDescriptorBufferRecordMissingResource();
    return false;
  }

  native.flags |= BufferDescriptorRecordFlagCounter;
  native.counter_offset =
      counter->GetHeapOffset() +
      (record.has_desc ? record.desc.uav.Buffer.CounterOffsetInBytes : 0);
  mirror.WriteBufferDescriptorRecord(mirror_lock, slot, native);
  dxmt::perf::recordNativeDescriptorBufferRecordCounter();
  return true;
}

static bool
MaterializeBufferTextureViewForWrite(
    WMT::Device device, DescriptorHeapMirror &mirror, UINT slot,
    const DescriptorHeapMirror::ScopedLock &mirror_lock,
    Resource &resource, BufferDescriptorMaterialization &view,
    DescriptorRecordType descriptor_type) {
  BufferTextureViewMaterialization texture_view = {};
  if (!BuildBufferTextureViewMaterialization(
          device, resource, view, descriptor_type, texture_view))
    return false;

  const auto texture_view_id = mirror.SetTexturePoolBufferSlot(
      mirror_lock, slot, resource.GetBufferAllocation()->buffer(),
      texture_view.descriptor, texture_view.offset,
      texture_view.bytes_per_row);
  if (!texture_view_id)
    return false;

  view.flags |= BufferDescriptorRecordFlagTextureView;
  mirror.WriteBufferTextureTableEntry(
      mirror_lock, slot, resource.GetGpuVirtualAddress() + view.view_offset,
      view.byte_size, texture_view_id, texture_view.element_count,
      texture_view.first_element, view.flags);
  return true;
}

static WMT::Resource
DescriptorTextureResidencyAllocation(Resource *resource) {
  if (!resource || !resource->GetTextureAllocation())
    return {};
  return WMT::Resource{resource->GetTextureAllocation()->texture().handle};
}

static void
SetDescriptorResidencyAllocation(WMT::Reference<WMT::Resource> &dst,
                                 WMT::Resource allocation) {
  if (allocation)
    dst = allocation;
  else
    dst = nullptr;
}

static DescriptorResidencyTarget
GetDescriptorResidencyTarget(const DescriptorRecord &record) {
  DescriptorResidencyTarget target = {};
  switch (record.type) {
  case DescriptorRecordType::ConstantBufferView: {
    if (!record.has_desc || !record.desc.cbv.BufferLocation)
      return target;
    UINT64 offset = 0;
    auto *resource = LookupBufferResourceByGpuVirtualAddress(
        record.desc.cbv.BufferLocation, &offset);
    (void)offset;
    if (resource && resource->HasLifetimeResidency())
      return target;
    SetDescriptorResidencyAllocation(
        target.allocation, DescriptorBufferResidencyAllocation(resource));
    return target;
  }
  case DescriptorRecordType::ShaderResourceView:
  case DescriptorRecordType::UnorderedAccessView: {
    auto *resource = GetResourceFromD3D12(record.resource.ptr());
    if (!resource)
      return target;
    if (resource->HasLifetimeResidency()) {
      // D3DMetal keeps ordinary D3D12 allocations in a device-wide residency
      // set for the resource lifetime. Avoid rebuilding equivalent residency
      // references for every descriptor slot update.
    } else if (resource->GetBuffer()) {
      SetDescriptorResidencyAllocation(
          target.allocation, DescriptorBufferResidencyAllocation(resource));
    } else if (resource->GetTexture()) {
      SetDescriptorResidencyAllocation(
          target.allocation, DescriptorTextureResidencyAllocation(resource));
    }

    if (record.type == DescriptorRecordType::UnorderedAccessView) {
      auto *counter = GetResourceFromD3D12(record.counter_resource.ptr());
      if (!counter || !counter->HasLifetimeResidency())
        SetDescriptorResidencyAllocation(
            target.secondary_allocation,
            DescriptorBufferResidencyAllocation(counter));
    }
    return target;
  }
  case DescriptorRecordType::Sampler:
    target.sampler = record.materialized_sampler;
    return target;
  default:
    return target;
  }
}

static void
ClearDescriptorResidencyTarget(
    IMTLD3D12Device *device,
    const DescriptorHeapMirror::ScopedLock &mirror_lock,
    DescriptorRecord &record) {
  auto &queue = device->GetDXMTDevice().queue();
  auto transition = record.mirror->ReplaceResidencyTarget(
      mirror_lock, record.heap_index, {});
  if (transition.previous.sampler)
    queue.RetainGpuOwner(std::move(transition.previous.sampler));
}

static void
ApplyDescriptorResidencyTarget(IMTLD3D12Device *device,
                               const DescriptorHeapMirror::ScopedLock &mirror_lock,
                               DescriptorRecord &record) {
  if (!device || !record.mirror)
    return;

  static const auto fault = LoadFaultInjectionConfiguration(
      "DXMT_TEST_FAIL_RESIDENCY_INSERTION_AT");
  if (ShouldInjectConfiguredFailure(fault,
                                    g_test_residency_insertion_occurrence)) {
    RecordInjectedFault("DXMT_TEST_FAIL_RESIDENCY_INSERTION_AT");
    WARN("D3D12Device: injected descriptor residency-set insertion failure");
    record.mirror->WriteNullTableEntry(mirror_lock, record.heap_index);
    ClearDescriptorResidencyTarget(device, mirror_lock, record);
    return;
  }

  auto target = GetDescriptorResidencyTarget(record);
  auto &queue = device->GetDXMTDevice().queue();
  // Descriptor slots retain their payload objects, but residency belongs to
  // the allocation owner. Updating a descriptor never mutates a residency set.
  auto transition = record.mirror->ReplaceResidencyTarget(
      mirror_lock, record.heap_index, std::move(target));
  if (transition.previous.sampler)
    queue.RetainGpuOwner(std::move(transition.previous.sampler));
}

static void
MaterializeDescriptorTableForWrite(IMTLD3D12Device *device,
                                   const DescriptorHeapMirror::ScopedLock &mirror_lock,
                                   DescriptorRecord &record) {
  const auto mtl_device = device->GetMTLDevice();
  const bool perf_enabled = dxmt::perf::enabled();
  const auto update_begin =
      perf_enabled ? dxmt::clock::now() : dxmt::clock::time_point{};
  auto record_update_time = [&] {
    if (perf_enabled) {
      dxmt::perf::recordArgumentTableUpdateTime(
          dxmt::perf::currentFrameStatistics(),
          dxmt::clock::now() - update_begin);
    }
  };
  auto *mirror = record.mirror;
  if (!mirror)
    return;
  static const auto fault = LoadFaultInjectionConfiguration(
      "DXMT_TEST_FAIL_DESCRIPTOR_TABLE_ALLOCATION_AT");
  if (ShouldInjectConfiguredFailure(
          fault, g_test_descriptor_table_allocation_occurrence)) {
    RecordInjectedFault("DXMT_TEST_FAIL_DESCRIPTOR_TABLE_ALLOCATION_AT");
    WARN("D3D12Device: injected descriptor-table allocation failure");
    mirror->WriteNullTableEntry(mirror_lock, record.heap_index);
    ClearDescriptorResidencyTarget(device, mirror_lock, record);
    record_update_time();
    return;
  }
  auto finish = [&] {
    ApplyDescriptorResidencyTarget(device, mirror_lock, record);
    record_update_time();
  };

  const UINT slot = record.heap_index;
  switch (record.type) {
  case DescriptorRecordType::ConstantBufferView: {
    if (!record.has_desc ||
        (!record.desc.cbv.BufferLocation && !record.desc.cbv.SizeInBytes)) {
      mirror->WriteNullTableEntry(mirror_lock, slot);
      finish();
      return;
    }
    mirror->WriteBufferTableEntry(mirror_lock, slot,
                                  record.desc.cbv.BufferLocation,
                                  record.desc.cbv.SizeInBytes, false);
    UINT64 offset = 0;
    auto *resource = kTestForceNativeCbvResourceLookupMiss
                         ? nullptr
                         : LookupBufferResourceByGpuVirtualAddress(
                               record.desc.cbv.BufferLocation, &offset);
    BufferDescriptorMaterialization native = {};
    native.view_offset = offset;
    native.byte_size = record.desc.cbv.SizeInBytes;
    if (WriteNativeBufferDescriptorRecord(
            *mirror, slot, mirror_lock, resource, native,
            DescriptorRecordType::ConstantBufferView) &&
        kTestForceNativeCbvStaleResourceTableEntry) {
      const auto record = mirror->bufferDescriptorRecord(mirror_lock, slot);
      if (record)
        mirror->InvalidateBufferResourceTableEntryForTesting(
            record->resource_index);
    }
    if (D3D12RootCauseDenseDiagEnabled()) {
      const auto native_record =
          mirror->bufferDescriptorRecord(mirror_lock, slot);
      const auto backend = native_record
                               ? mirror->backendResourceRecord(
                                     mirror_lock, native_record->resource_index)
                               : std::nullopt;
      const auto diag_flags = native_record
                                  ? DiagnoseNativeBufferDescriptor(
                                        *native_record, backend)
                                  : NativeDescriptorDiagnosticMissingResource;
      WARN_FILE_ONLY(
          "D3D12 root-cause: materialize CBV",
          " slot=", slot,
          " d3dGpuVa=", record.desc.cbv.BufferLocation,
          " d3dSize=", record.desc.cbv.SizeInBytes,
          " lookupResource=", reinterpret_cast<uintptr_t>(resource),
          " lookupOffset=", offset,
          " resourceIndex=", native_record ? native_record->resource_index : 0,
          " nativeOffset=", native_record ? native_record->byte_offset : 0,
          " nativeSize=", native_record ? native_record->byte_size : 0,
          " resourceGpuAddress=", backend ? backend->gpu_address : 0,
          " resourceSize=", backend ? backend->byte_size : 0,
          " resourceGeneration=", backend ? backend->generation : 0,
          " tableGeneration=",
          mirror->backendResourceTableGeneration(mirror_lock),
          " diagFlags=0x", std::hex, diag_flags, std::dec);
    }
    finish();
    return;
  }
  case DescriptorRecordType::ShaderResourceView: {
    auto *resource = GetResourceFromD3D12(record.resource.ptr());
    if (!resource) {
      mirror->WriteNullTableEntry(mirror_lock, slot);
      finish();
      return;
    }
    if (resource->GetBuffer()) {
      BufferDescriptorMaterialization native = {};
      if (!GetBufferSrvMaterialization(mtl_device, *resource, record,
                                       native)) {
        mirror->WriteNullTableEntry(mirror_lock, slot);
        finish();
        return;
      }
      // Raw and structured descriptors use native buffer records in both
      // bindless ABIs. Only typed buffers require a Metal texture-buffer
      // view; creating one for every structured write needlessly mutates the
      // texture-view pool and invalidates its descriptor mirror.
      if (!native.typed || !MaterializeBufferTextureViewForWrite(
              mtl_device, *mirror, slot, mirror_lock, *resource, native,
              DescriptorRecordType::ShaderResourceView)) {
        mirror->WriteBufferTableEntry(
            mirror_lock, slot,
            resource->GetGpuVirtualAddress() + native.view_offset,
            native.byte_size, native.typed);
        if (native.typed)
          mirror->ClearTextureSlot(mirror_lock, slot);
      }
      WriteNativeBufferDescriptorRecord(
          *mirror, slot, mirror_lock, resource, native,
          DescriptorRecordType::ShaderResourceView);
      finish();
      return;
    }
    if (resource->IsReservedTexture() &&
        !resource->EnsureTextureAllocation("DescriptorTableTextureSRV")) {
      mirror->WriteNullTableEntry(mirror_lock, slot);
      finish();
      return;
    }
    auto binding =
        CreateDescriptorShaderResourceTextureView(mtl_device, *resource, record);
    auto *allocation =
        binding.texture ? binding.texture->current() : nullptr;
    const uint64_t gpu_resource_id =
        binding && allocation
            ? mirror->SetTexturePoolSlot(mirror_lock, slot,
                                         binding.texture.ptr(), binding.view,
                                         allocation)
            : 0;
    if (!gpu_resource_id) {
      mirror->WriteNullTableEntry(mirror_lock, slot);
      finish();
      return;
    }
    if (D3D12RootCauseDenseDiagEnabled()) {
      WARN_FILE_ONLY(
          "D3D12 root-cause: materialize texture SRV",
          " slot=", slot,
          " resource=", reinterpret_cast<uintptr_t>(resource),
          " resourceDimension=", uint32_t(resource->GetResourceDesc().Dimension),
          " resourceFormat=", uint32_t(resource->GetResourceDesc().Format),
          " srvDimension=", record.has_desc ? uint32_t(record.desc.srv.ViewDimension) : 0,
          " srvFormat=", record.has_desc ? uint32_t(record.desc.srv.Format) : 0,
          " metalResourceType=", uint32_t(binding.texture->textureType()),
          " metalViewType=", uint32_t(binding.texture->textureType(binding.view)),
          " metalView=", uint64_t(binding.view),
          " gpuResourceId=", gpu_resource_id,
          " arrayLength=", binding.texture->arrayLength(binding.view),
          " sampleCount=", binding.texture->sampleCount(),
          " slotGeneration=", record.slot_version.sequence,
          " tableGeneration=",
          mirror->backendResourceTableGeneration(mirror_lock));
    }
    const uint32_t array_length =
        binding.texture->arrayLength(binding.view);
    const float min_lod = GetShaderResourceTextureMinLod(record);
    mirror->WriteTextureTableEntry(mirror_lock, slot, gpu_resource_id,
                                   array_length, min_lod);
    mirror->FillTextureSlot(mirror_lock, slot, gpu_resource_id, array_length,
                            min_lod);
    finish();
    return;
  }
  case DescriptorRecordType::UnorderedAccessView: {
    auto *resource = GetResourceFromD3D12(record.resource.ptr());
    if (!resource) {
      mirror->WriteNullTableEntry(mirror_lock, slot);
      finish();
      return;
    }
    if (resource->GetBuffer()) {
      BufferDescriptorMaterialization native = {};
      if (!GetBufferUavMaterialization(mtl_device, *resource, record,
                                       native)) {
        mirror->WriteNullTableEntry(mirror_lock, slot);
        finish();
        return;
      }
      if (!native.typed || !MaterializeBufferTextureViewForWrite(
              mtl_device, *mirror, slot, mirror_lock, *resource, native,
              DescriptorRecordType::UnorderedAccessView)) {
        mirror->WriteBufferTableEntry(
            mirror_lock, slot,
            resource->GetGpuVirtualAddress() + native.view_offset,
            native.byte_size, native.typed);
        if (native.typed)
          mirror->ClearTextureSlot(mirror_lock, slot);
      }
      if (WriteNativeBufferDescriptorRecord(
              *mirror, slot, mirror_lock, resource, native,
              DescriptorRecordType::UnorderedAccessView))
        WriteNativeUavCounterRecord(*mirror, slot, mirror_lock, record);
      finish();
      return;
    }
    if (resource->IsReservedTexture() &&
        !resource->EnsureTextureAllocation("DescriptorTableTextureUAV")) {
      mirror->WriteNullTableEntry(mirror_lock, slot);
      finish();
      return;
    }
    auto binding =
        CreateDescriptorUnorderedAccessTextureView(mtl_device, *resource, record);
    auto *allocation =
        binding.texture ? binding.texture->current() : nullptr;
    if (!binding || !allocation) {
      mirror->WriteNullTableEntry(mirror_lock, slot);
      finish();
      return;
    }
    auto &view = binding.texture->view(binding.view, allocation);
    if (!view.texture || !view.gpuResourceID) {
      mirror->WriteNullTableEntry(mirror_lock, slot);
      finish();
      return;
    }
    const uint32_t array_length =
        binding.texture->arrayLength(binding.view);
    mirror->WriteTextureTableEntry(mirror_lock, slot, view.gpuResourceID,
                                   array_length);
    mirror->FillTextureSlot(mirror_lock, slot, view.gpuResourceID,
                            array_length);
    finish();
    return;
  }
  case DescriptorRecordType::Sampler: {
    if (!mirror->isSamplerHeap() || !record.has_desc) {
      mirror->WriteNullTableEntry(mirror_lock, slot);
      finish();
      return;
    }
    if (!record.materialized_sampler)
      record.materialized_sampler = CreateD3D12Sampler(mtl_device,
                                                       record.desc.sampler);
    mirror->WriteSamplerTableEntry(mirror_lock, slot,
                                   record.materialized_sampler.ptr());
    finish();
    return;
  }
  default:
    mirror->WriteNullTableEntry(mirror_lock, slot);
    finish();
    return;
  }
}

#ifdef __ID3D12Device9_INTERFACE_DEFINED__
using DeviceComBase = ID3D12Device9;
#elif defined(__ID3D12Device8_INTERFACE_DEFINED__)
using DeviceComBase = ID3D12Device8;
#elif defined(__ID3D12Device7_INTERFACE_DEFINED__)
using DeviceComBase = ID3D12Device7;
#elif defined(__ID3D12Device6_INTERFACE_DEFINED__)
using DeviceComBase = ID3D12Device6;
#elif defined(__ID3D12Device2_INTERFACE_DEFINED__)
using DeviceComBase = ID3D12Device2;
#else
using DeviceComBase = ID3D12Device1;
#endif

#ifdef __ID3D12DeviceRemovedExtendedData2_INTERFACE_DEFINED__
class DredDataImpl final : public ID3D12DeviceRemovedExtendedData2 {
public:
  explicit DredDataImpl(IMTLD3D12Device *device) : device_(device) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    InitReturnPtr(object);
    if (!object)
      return E_POINTER;

    if (riid == __uuidof(ID3D12DeviceRemovedExtendedData)) {
      *object = static_cast<ID3D12DeviceRemovedExtendedData *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == __uuidof(ID3D12DeviceRemovedExtendedData1)) {
      *object = static_cast<ID3D12DeviceRemovedExtendedData1 *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == __uuidof(ID3D12DeviceRemovedExtendedData2)) {
      *object = static_cast<ID3D12DeviceRemovedExtendedData2 *>(this);
      AddRef();
      return S_OK;
    }
    return device_->QueryInterface(riid, object);
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return device_->AddRef(); }

  ULONG STDMETHODCALLTYPE Release() override { return device_->Release(); }

  HRESULT STDMETHODCALLTYPE GetAutoBreadcrumbsOutput(
      D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT *output) override {
    if (!output)
      return E_INVALIDARG;
    *output = {};
    return ReportStatus();
  }

  HRESULT STDMETHODCALLTYPE GetPageFaultAllocationOutput(
      D3D12_DRED_PAGE_FAULT_OUTPUT *output) override {
    if (!output)
      return E_INVALIDARG;
    *output = {};
    return ReportStatus();
  }

  HRESULT STDMETHODCALLTYPE GetAutoBreadcrumbsOutput1(
      D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 *output) override {
    if (!output)
      return E_INVALIDARG;
    *output = {};
    return ReportStatus();
  }

  HRESULT STDMETHODCALLTYPE GetPageFaultAllocationOutput1(
      D3D12_DRED_PAGE_FAULT_OUTPUT1 *output) override {
    if (!output)
      return E_INVALIDARG;
    *output = {};
    return ReportStatus();
  }

  HRESULT STDMETHODCALLTYPE GetPageFaultAllocationOutput2(
      D3D12_DRED_PAGE_FAULT_OUTPUT2 *output) override {
    if (!output)
      return E_INVALIDARG;
    *output = {};
    return ReportStatus();
  }

  D3D12_DRED_DEVICE_STATE STDMETHODCALLTYPE GetDeviceState() override {
    return device_->GetDXMTDevice().queue().HasDeviceError()
               ? D3D12_DRED_DEVICE_STATE_FAULT
               : D3D12_DRED_DEVICE_STATE_UNKNOWN;
  }

private:
  HRESULT ReportStatus() const {
    return device_->GetDXMTDevice().queue().HasDeviceError()
               ? DXGI_ERROR_UNSUPPORTED
               : DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
  }

  IMTLD3D12Device *device_;
};
#endif

class SetEventQueueSubmission final : public DxmtQueueSubmissionTarget {
public:
  SetEventQueueSubmission(WMT::Reference<WMT::SharedEvent> signal,
                          HANDLE event, uint64_t value) noexcept
      : signal_(std::move(signal)), event_(event), value_(value) {}

  [[nodiscard]] bool
  Submit(IMTLD3D12Device &device,
         uint64_t &submitted_sequence) noexcept override {
    return dxmt::invokeNoexcept("D3D12 EnqueueSetEvent submission", [&]() {
      auto &queue = device.GetDXMTDevice().queue();
      submitted_sequence = queue.CurrentSeqId();
      MTLSharedEvent_setWin32EventAtValue(
          signal_.handle, queue.GetSharedEventListener(), event_, value_);
      queue.CurrentChunk()->emitcc(
          [signal = std::move(signal_), value = value_](
              ArgumentEncodingContext &enc) mutable {
            enc.signalEvent(std::move(signal), value);
          });
      queue.CommitCurrentChunk();
    });
  }

private:
  WMT::Reference<WMT::SharedEvent> signal_;
  HANDLE event_ = nullptr;
  uint64_t value_ = 0;
};

class DeviceImpl final : public ComObjectWithInitialRef<IMTLD3D12Device,
                                                        DeviceComBase,
                                                        ID3D12DeviceConfiguration1> {
public:
  DeviceImpl(std::unique_ptr<dxmt::Device> &&device, IMTLDXGIAdapter *adapter)
      : adapter_(adapter), device_(std::move(device)),
        pipeline_native_artifact_cache_(CreatePipelineNativeArtifactCache())
#ifdef __ID3D12DeviceRemovedExtendedData2_INTERFACE_DEFINED__
        , dred_(static_cast<IMTLD3D12Device *>(this))
#endif
  {
    const auto archive_setting = env::getEnvVar("DXMT_PSO_BINARY_ARCHIVE");
    pso_binary_archive_enabled_ =
        env::getEnvVar("DXMT_SHADER_CACHE") != "0" &&
        archive_setting != "0";
    if (pso_binary_archive_enabled_) {
      pso_binary_archive_serialize_every_ =
          GetPSOBinaryArchiveSerializeEvery();
      auto archive_base_dir = ResolvePSOBinaryArchiveDirectory();
      pso_binary_archive_unix_dir_ =
          archive_base_dir.empty()
              ? std::string()
              : EnsureTrailingSlash(archive_base_dir + "com.apple.metal4");
      pso_binary_archive_unix_path_ =
          pso_binary_archive_unix_dir_.empty()
              ? std::string()
              : pso_binary_archive_unix_dir_ + "dxmt_pso.binaryarchive";
      pso_binary_archive_unavailable_path_ =
          pso_binary_archive_unix_dir_.empty()
              ? std::string()
              : pso_binary_archive_unix_dir_ + "dxmt_pso_archive_unavailable.txt";
      try {
        if (!pso_binary_archive_unix_dir_.empty())
          std::filesystem::create_directories(pso_binary_archive_unix_dir_);
      } catch (const std::exception &e) {
        WARN("D3D12Device: failed to create DXMT PSO binary archive directory ",
             pso_binary_archive_unix_dir_, ": ", e.what());
      } catch (...) {
        WARN("D3D12Device: failed to create DXMT PSO binary archive directory ",
             pso_binary_archive_unix_dir_);
      }
    }

    enqueue_set_event_signal_ = device_->device().newSharedEvent();
    multiple_fence_wait_signal_ = device_->device().newSharedEvent();
    DXGI_ADAPTER_DESC adapter_desc = {};
    if (SUCCEEDED(adapter_->GetDesc(&adapter_desc)))
      adapter_luid_ = adapter_desc.AdapterLuid;

    if (adapter_->GetLocalD3DKMT()) {
      D3DKMT_CREATEDEVICE create = {};
      create.hAdapter = adapter_->GetLocalD3DKMT();
      if (D3DKMTCreateDevice(&create))
        WARN("D3D12Device: failed to create D3DKMT device");
      else
        local_kmt_ = create.hDevice;
    }
  }

  ~DeviceImpl() {
    if (pso_binary_archive_enabled_) {
      LogPSOBinaryArchiveMarker(
          "DXMT_PSO_ARCHIVE: dtor reached enabled=%d have_archive=%d",
          pso_binary_archive_enabled_ ? 1 : 0, pso_binary_archive_ ? 1 : 0);
      if (pso_binary_archive_)
        SerializePSOBinaryArchive("dtor", 0);
    }

    if (local_kmt_) {
      D3DKMT_DESTROYDEVICE destroy = {};
      destroy.hDevice = local_kmt_;
      D3DKMTDestroyDevice(&destroy);
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12Device)) {
      *ppvObject = ref(AsD3D12Device());
      return S_OK;
    }

    if (riid == __uuidof(ID3D12Device1)) {
      *ppvObject = ref(static_cast<ID3D12Device1 *>(
          static_cast<DeviceComBase *>(this)));
      return S_OK;
    }

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12Device2)) {
      *ppvObject = ref(static_cast<ID3D12Device2 *>(this));
      return S_OK;
    }
#endif

#ifdef __ID3D12Device3_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12Device3)) {
      *ppvObject = ref(static_cast<ID3D12Device3 *>(this));
      return S_OK;
    }
#endif

#ifdef __ID3D12Device4_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12Device4)) {
      *ppvObject = ref(static_cast<ID3D12Device4 *>(this));
      return S_OK;
    }
#endif

#ifdef __ID3D12Device5_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12Device5)) {
      *ppvObject = ref(static_cast<ID3D12Device5 *>(this));
      return S_OK;
    }
#endif

#ifdef __ID3D12Device6_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12Device6)) {
      *ppvObject = ref(static_cast<ID3D12Device6 *>(this));
      return S_OK;
    }
#endif

#ifdef __ID3D12Device7_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12Device7)) {
      *ppvObject = ref(static_cast<ID3D12Device7 *>(this));
      return S_OK;
    }
#endif

#ifdef __ID3D12Device8_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12Device8)) {
      *ppvObject = ref(static_cast<ID3D12Device8 *>(this));
      return S_OK;
    }
#endif

#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12Device9)) {
      *ppvObject = ref(static_cast<ID3D12Device9 *>(this));
      return S_OK;
    }
#endif

    if (riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDevice) ||
        riid == __uuidof(IDXGIDevice1) || riid == __uuidof(IDXGIDevice2) ||
        riid == __uuidof(IDXGIDevice3) || riid == __uuidof(IMTLDXGIDevice) ||
        riid == __uuidof(IMTLD3D12Device)) {
      *ppvObject = ref(static_cast<IMTLD3D12Device *>(this));
      return S_OK;
    }

    if (riid == __uuidof(ID3D12DeviceConfiguration) ||
        riid == __uuidof(ID3D12DeviceConfiguration1)) {
      *ppvObject = ref(static_cast<ID3D12DeviceConfiguration1 *>(this));
      return S_OK;
    }

#ifdef __ID3D12DeviceRemovedExtendedData2_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12DeviceRemovedExtendedData) ||
        riid == __uuidof(ID3D12DeviceRemovedExtendedData1) ||
        riid == __uuidof(ID3D12DeviceRemovedExtendedData2))
      return dred_.QueryInterface(riid, ppvObject);
#endif

    if (logQueryInterfaceError(__uuidof(ID3D12Device), riid))
      WARN("D3D12Device: unknown interface query ", str::format(riid));

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size, void *data) override {
    if (guid == dxmt::d3d12::test::kPipelineNativeArtifactCacheStatsGuid) {
      using dxmt::d3d12::test::PipelineNativeArtifactCacheStats;
      if (!data_size)
        return E_INVALIDARG;
      const UINT required = sizeof(PipelineNativeArtifactCacheStats);
      if (!data) {
        *data_size = required;
        return S_OK;
      }
      if (*data_size < required) {
        *data_size = required;
        return DXGI_ERROR_MORE_DATA;
      }
      const auto internal = dxmt::d3d12::GetPipelineNativeArtifactCacheStats(
          pipeline_native_artifact_cache_.get());
      PipelineNativeArtifactCacheStats stats = {};
      stats.hits = internal.hits;
      stats.misses = internal.misses;
      stats.waits = internal.waits;
      stats.compiles = internal.compiles;
      stats.compile_failures = internal.compile_failures;
      std::memcpy(data, &stats, required);
      *data_size = required;
      return S_OK;
    }
    if (guid == dxmt::d3d12::test::kPersistentResidencyStatsGuid) {
      using dxmt::d3d12::test::PersistentResidencyStats;
      if (!data_size)
        return E_INVALIDARG;
      const UINT required = sizeof(PersistentResidencyStats);
      if (!data) {
        *data_size = required;
        return S_OK;
      }
      if (*data_size < required) {
        *data_size = required;
        return DXGI_ERROR_MORE_DATA;
      }
      const auto [entry_count, total_ref_count, pending_removal_count,
                  cached_allocation_count, commit_count] =
          device_->queue().PersistentResidencyStatsForTesting();
      PersistentResidencyStats stats = {};
      stats.entry_count = entry_count;
      stats.total_ref_count = total_ref_count;
      stats.pending_removal_count = pending_removal_count;
      stats.cached_allocation_count = cached_allocation_count;
      stats.commit_count = commit_count;
      std::memcpy(data, &stats, required);
      *data_size = required;
      return S_OK;
    }
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size, const void *data) override {
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *data) override {
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override {
    return adapter_->QueryInterface(riid, ppParent);
  }

  HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter **pAdapter) override {
    if (!pAdapter)
      return WARN_E_INVALIDARG(__func__);

    return adapter_->QueryInterface(IID_PPV_ARGS(pAdapter));
  }

  HRESULT STDMETHODCALLTYPE
  CreateSurface(const DXGI_SURFACE_DESC *desc, UINT surface_count, DXGI_USAGE usage,
                const DXGI_SHARED_RESOURCE *shared_resource, IDXGISurface **surface) override {
    InitReturnPtr(surface);
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE
  QueryResourceResidency(IUnknown *const *resources, DXGI_RESIDENCY *residency, UINT resource_count) override {
    if (!resources || !residency)
      return WARN_E_INVALIDARG(__func__);

    for (UINT i = 0; i < resource_count; i++)
      residency[i] = DXGI_RESIDENCY_FULLY_RESIDENT;

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT priority) override {
    if (priority < -7 || priority > 7)
      return WARN_E_INVALIDARG(__func__);

    gpu_thread_priority_ = priority;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *priority) override {
    if (!priority)
      return E_POINTER;

    *priority = gpu_thread_priority_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) override {
    constexpr UINT kDefaultMaximumFrameLatency = 3;
    constexpr UINT kMaximumFrameLatency = 16;
    if (max_latency > kMaximumFrameLatency)
      return DXGI_ERROR_INVALID_CALL;

    maximum_frame_latency_ = max_latency ? max_latency
                                         : kDefaultMaximumFrameLatency;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) override {
    if (!max_latency)
      return DXGI_ERROR_INVALID_CALL;

    *max_latency = maximum_frame_latency_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  OfferResources(UINT resource_count, IDXGIResource *const *resources,
                 DXGI_OFFER_RESOURCE_PRIORITY priority) override {
    if (priority < DXGI_OFFER_RESOURCE_PRIORITY_LOW ||
        priority > DXGI_OFFER_RESOURCE_PRIORITY_HIGH ||
        (resource_count && !resources))
      return WARN_E_INVALIDARG(__func__);
    for (UINT i = 0; i < resource_count; i++) {
      if (!resources[i])
        return WARN_E_INVALIDARG(__func__);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ReclaimResources(UINT resource_count, IDXGIResource *const *resources,
                                             WINBOOL *discarded) override {
    if (resource_count && !resources)
      return WARN_E_INVALIDARG(__func__);
    for (UINT i = 0; i < resource_count; i++) {
      if (!resources[i])
        return WARN_E_INVALIDARG(__func__);
    }
    if (discarded) {
      for (UINT i = 0; i < resource_count; i++)
        discarded[i] = FALSE;
    }

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE event) override {
    if (!event)
      return WARN_E_INVALIDARG(__func__);
    auto signal = enqueue_set_event_signal_;
    auto value = ++enqueue_set_event_value_;
    uint64_t submitted_sequence = 0;
    const bool submitted = SubmitDxmtQueueWork(
        static_cast<IMTLD3D12Device *>(this),
        std::make_unique<SetEventQueueSubmission>(
            std::move(signal), event, value),
        submitted_sequence);
    return submitted ? S_OK : E_FAIL;
  }

  void STDMETHODCALLTYPE Trim() override {}

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() override {
    return GetD3D12AdapterDevice(adapter_.ptr());
  }

  WMT::BinaryArchive *STDMETHODCALLTYPE GetPSOBinaryArchive() override {
    return GetOrCreatePSOBinaryArchive();
  }

  std::mutex &STDMETHODCALLTYPE GetPSOBinaryArchiveMutex() override {
    return pso_binary_archive_mutex_;
  }

  void STDMETHODCALLTYPE NotePSOBinaryArchivePipelineCreated() override {
    if (!pso_binary_archive_enabled_ || !pso_binary_archive_serialize_every_)
      return;

    auto count = pso_binary_archive_successful_creates_.fetch_add(
                     1, std::memory_order_relaxed) +
                 1;
    if (count != 1 && count % pso_binary_archive_serialize_every_)
      return;

    bool ok = SerializePSOBinaryArchive("periodic", count);
    LogPSOBinaryArchiveMarker(
        "DXMT_PSO_ARCHIVE: periodic serialize after %llu creates ok=%d",
        static_cast<unsigned long long>(count), ok ? 1 : 0);
  }

  PipelineNativeArtifactCache *STDMETHODCALLTYPE
  GetPipelineNativeArtifactCache() override {
    return pipeline_native_artifact_cache_.get();
  }

  DxgiBackendKind STDMETHODCALLTYPE GetBackendKind() override {
    return DxgiBackendKind::Metal4;
  }

  uint64_t STDMETHODCALLTYPE GetMetalDeviceHandle() override {
    return adapter_->GetMetalDeviceHandle();
  }

  dxmt::Device &GetDXMTDevice() override {
    return *device_;
  }

  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() override {
    return local_kmt_;
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChain(
      IDXGIFactory1 *factory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *desc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
      IDXGISwapChain1 **swap_chain) override {
    InitReturnPtr(swap_chain);
    return DXGI_ERROR_UNSUPPORTED;
  }

  UINT STDMETHODCALLTYPE GetNodeCount() override {
    return 1;
  }

  HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *desc,
                                               REFIID riid, void **command_queue) override {
    InitReturnPtr(command_queue);
    if (!command_queue)
      return E_POINTER;
    if (ShouldInjectCreationFailure(
            "DXMT_TEST_FAIL_COMMAND_QUEUE_CREATION_AT",
            g_test_command_queue_creation_occurrence))
      return E_OUTOFMEMORY;
    auto hr = d3d12::CreateCommandQueue(
        static_cast<IMTLD3D12Device *>(this), desc, riid, command_queue);
    if (dxmt::apitrace::d3d_enabled()) {
      dxmt::apitrace::record_create_command_queue(
          this, desc, command_queue ? *command_queue : nullptr,
          static_cast<int32_t>(hr));
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type,
                                                   REFIID riid, void **command_allocator) override {
    if (D3D12DeviceDiagEnabled()) {
      WARN_FILE_ONLY("D3D12 diagnostic: CreateCommandAllocator enter"
           " type=", type,
           " riid=", str::format(riid));
    }
    InitReturnPtr(command_allocator);
    if (!command_allocator)
      return E_POINTER;
    if (!IsSupportedCommandListType(type)) {
      WARN("D3D12Device: unsupported command allocator type ", type);
      return WARN_E_INVALIDARG(__func__);
    }

    auto allocator = d3d12::CreateCommandAllocator(static_cast<IMTLD3D12Device *>(this), type);
    HRESULT hr = allocator->QueryInterface(riid, command_allocator);
    if (dxmt::apitrace::d3d_enabled()) {
      dxmt::apitrace::record_create_command_allocator(
          this, type, command_allocator ? *command_allocator : nullptr,
          static_cast<int32_t>(hr));
    }
    if (D3D12DeviceDiagEnabled()) {
      WARN_FILE_ONLY("D3D12 diagnostic: CreateCommandAllocator result"
           " hr=", hr,
           " allocator=", command_allocator && *command_allocator
                             ? reinterpret_cast<uintptr_t>(*command_allocator)
                             : 0);
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                              REFIID riid, void **pipeline_state) override {
    dxmt::perf::ScopedFrameTimer perf_timer(
        dxmt::perf::FrameTimeBucket::CreatePipeline);
    InitReturnPtr(pipeline_state);
    if (!pipeline_state)
      return E_POINTER;
    if (ShouldInjectCreationFailure(
            "DXMT_TEST_FAIL_GRAPHICS_PIPELINE_CREATION_AT",
            g_test_graphics_pipeline_creation_occurrence))
      return E_OUTOFMEMORY;
    if (ShouldInjectCreationFailure(
            "DXMT_TEST_FAIL_METAL_GRAPHICS_PIPELINE_AT",
            g_test_metal_graphics_pipeline_occurrence)) {
      RecordInjectedFault("DXMT_TEST_FAIL_METAL_GRAPHICS_PIPELINE_AT");
      return E_OUTOFMEMORY;
    }
    HRESULT status = S_OK;
    auto state = d3d12::CreateGraphicsPipelineState(
        static_cast<IMTLD3D12Device *>(this), desc, &status);
    if (!state)
      return status;
    auto hr = state->QueryInterface(riid, pipeline_state);
    if (dxmt::apitrace::d3d_enabled()) {
      dxmt::apitrace::record_create_graphics_pipeline_state(
          this, desc, pipeline_state ? *pipeline_state : nullptr,
          static_cast<int32_t>(hr));
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                             REFIID riid, void **pipeline_state) override {
    dxmt::perf::ScopedFrameTimer perf_timer(
        dxmt::perf::FrameTimeBucket::CreatePipeline);
    InitReturnPtr(pipeline_state);
    if (ShouldInjectCreationFailure(
            "DXMT_TEST_FAIL_COMPUTE_PIPELINE_CREATION_AT",
            g_test_compute_pipeline_creation_occurrence))
      return E_OUTOFMEMORY;
    HRESULT status = S_OK;
    auto state = d3d12::CreateComputePipelineState(
        static_cast<IMTLD3D12Device *>(this), desc, &status);
    if (!state)
      return status;
    auto hr = state->QueryInterface(riid, pipeline_state);
    if (dxmt::apitrace::d3d_enabled()) {
      dxmt::apitrace::record_create_compute_pipeline_state(
          this, desc, pipeline_state ? *pipeline_state : nullptr,
          static_cast<int32_t>(hr));
    }
    return hr;
  }

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
  HRESULT STDMETHODCALLTYPE
  CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                      REFIID riid, void **pipeline_state) override {
    dxmt::perf::ScopedFrameTimer perf_timer(
        dxmt::perf::FrameTimeBucket::CreatePipeline);
    BeginDeviceCall("CreatePipelineState");
    InitReturnPtr(pipeline_state);
    if (!pipeline_state)
      return E_POINTER;

    HRESULT status = S_OK;
    auto state = d3d12::CreatePipelineStateFromStream(
        static_cast<IMTLD3D12Device *>(this), desc, &status);
    if (!state)
      return status;
    auto hr = state->QueryInterface(riid, pipeline_state);
    if (dxmt::apitrace::d3d_enabled()) {
      dxmt::apitrace::record_create_pipeline_state(
          this,
          desc ? desc->pPipelineStateSubobjectStream : nullptr,
          desc ? desc->SizeInBytes : 0,
          pipeline_state ? *pipeline_state : nullptr,
          static_cast<int32_t>(hr));
    }
    return hr;
  }
#endif

  HRESULT STDMETHODCALLTYPE CreateCommandList(UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
                                              ID3D12CommandAllocator *command_allocator,
                                              ID3D12PipelineState *initial_pipeline_state,
                                              REFIID riid, void **command_list) override {
    if (D3D12DeviceDiagEnabled()) {
      WARN_FILE_ONLY("D3D12 diagnostic: CreateCommandList enter"
           " nodeMask=", node_mask,
           " type=", type,
           " allocator=", reinterpret_cast<uintptr_t>(command_allocator),
           " initialPSO=", reinterpret_cast<uintptr_t>(initial_pipeline_state),
           " riid=", str::format(riid));
    }
    InitReturnPtr(command_list);

    if (node_mask > 1 || !command_allocator)
      return WARN_E_INVALIDARG(__func__);
    if (!IsSupportedCommandListType(type)) {
      WARN("D3D12Device: unsupported command list type ", type);
      return WARN_E_INVALIDARG(__func__);
    }

    auto allocator_state = dynamic_cast<d3d12::CommandAllocator *>(command_allocator);
    if (!allocator_state ||
        allocator_state->GetParentDevice() !=
            static_cast<IMTLD3D12Device *>(this) ||
        allocator_state->GetCommandListType() != type)
      return WARN_E_INVALIDARG(__func__);

    HRESULT status = S_OK;
    auto list = d3d12::CreateGraphicsCommandList(
        static_cast<IMTLD3D12Device *>(this), node_mask, type,
        command_allocator, initial_pipeline_state, &status);
    if (!list)
      return status;
    HRESULT hr = list->QueryInterface(riid, command_list);
    if (dxmt::apitrace::d3d_enabled()) {
      dxmt::apitrace::record_create_command_list(
          this, node_mask, type, command_allocator, initial_pipeline_state,
          command_list ? *command_list : nullptr, static_cast<int32_t>(hr));
    }
    if (D3D12DeviceDiagEnabled()) {
      WARN_FILE_ONLY("D3D12 diagnostic: CreateCommandList result"
           " createStatus=", status,
           " hr=", hr,
           " list=", command_list && *command_list
                      ? reinterpret_cast<uintptr_t>(*command_list)
                      : 0);
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE feature, void *feature_data,
                                                UINT feature_data_size) override {
    if (!feature_data)
      return WARN_E_INVALIDARG(__func__);

    switch (feature) {
    case D3D12_FEATURE_FEATURE_LEVELS: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS *>(feature_data);
      if (!data->pFeatureLevelsRequested || !data->NumFeatureLevels)
        return WARN_E_INVALIDARG(__func__);

      data->MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL(0);

      for (UINT i = 0; i < data->NumFeatureLevels; i++) {
        const auto requested = data->pFeatureLevelsRequested[i];
        if (requested <= kSupportedFeatureLevel)
          data->MaxSupportedFeatureLevel = std::max(data->MaxSupportedFeatureLevel, requested);
      }

      return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_ARCHITECTURE))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE *>(feature_data);
      if (data->NodeIndex != 0)
        return WARN_E_INVALIDARG(__func__);

      const bool unified_memory = GetMTLDevice().hasUnifiedMemory();
      data->TileBasedRenderer = TRUE;
      data->UMA = unified_memory;
      data->CacheCoherentUMA = unified_memory;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
      data->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_1;
#ifndef DXMT_NO_PRIVATE_API
      data->OutputMergerLogicOp = TRUE;
#endif
      data->TypedUAVLoadAdditionalFormats =
          SupportsD3D12AdditionalTypedUavLoads(device_->device());
      data->TiledResourcesTier = GetDXMTDevice().device().supportsPlacementSparse()
                                     ? D3D12_TILED_RESOURCES_TIER_1
                                     : D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
      data->CrossNodeSharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
      data->MaxGPUVirtualAddressBitsPerResource = 40;
      return S_OK;
    }
    case D3D12_FEATURE_FORMAT_SUPPORT: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT *>(feature_data);
      data->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
      data->Support2 = D3D12_FORMAT_SUPPORT2_NONE;

      if (data->Format == DXGI_FORMAT_UNKNOWN) {
        data->Support1 = D3D12_FORMAT_SUPPORT1_BUFFER;
        return S_OK;
      }

      const auto &traits = GetDXGIFormatTraits(data->Format);
      if (traits.classification == DXGIFormatClass::Mask) {
        return S_OK;
      }
      if (traits.flags & DXGI_FORMAT_TRAIT_VIDEO) {
        data->Support1 = GetD3D12TraitFormatSupport1(traits);
        data->Support2 = GetD3D12TraitFormatSupport2(traits);
        return S_OK;
      }

      MTL_DXGI_FORMAT_DESC format = {};
      if (FAILED(MTLQueryDXGIFormat(device_->device(), data->Format, format))) {
        WARN("D3D12Device: CheckFeatureSupport(FORMAT_SUPPORT) unsupported format ",
             data->Format);
        return S_OK;
      }

      const auto caps = GetD3D12FormatCapability(device_->device(), format);
      data->Support1 = GetD3D12FormatSupport1(caps, format);
      data->Support2 = GetD3D12FormatSupport2(caps);
      if (!IsD3D12TypedUavFormat(data->Format)) {
        data->Support1 = static_cast<D3D12_FORMAT_SUPPORT1>(
            data->Support1 &
            ~D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW);
        data->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
      }
      if (!IsD3D12CoreTypedUavLoadFormat(data->Format) &&
          !SupportsD3D12AdditionalTypedUavLoads(device_->device())) {
        data->Support2 = static_cast<D3D12_FORMAT_SUPPORT2>(
            data->Support2 & ~D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD);
      }
      const UINT kTypedAtomicOperations =
          D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
          D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
          D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
          D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
          D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
          D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;
      if (data->Format != DXGI_FORMAT_R32_UINT &&
          data->Format != DXGI_FORMAT_R32_SINT) {
        data->Support2 = static_cast<D3D12_FORMAT_SUPPORT2>(
            static_cast<UINT>(data->Support2) & ~kTypedAtomicOperations);
      }
      // RGB32 buffer views are represented by three scalar R32 elements in
      // memory, while the Metal texture-buffer view exposes only one channel.
      // Keep Buffer/ShaderLoad for the typed formats, but do not advertise
      // typed UAV load/store or atomics until three-component lowering exists.
      // Feature-level 12 also disallows Buffer/ShaderLoad for RGB32_TYPELESS
      // and tiled resources for every 96-bpp RGB32 format.
      const bool emulated_rgb32 =
          IsEmulatedThreeComponentBufferFormat(data->Format);
      if (emulated_rgb32) {
        data->Support1 = static_cast<D3D12_FORMAT_SUPPORT1>(
            data->Support1 &
            ~D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW);
        if (data->Format == DXGI_FORMAT_R32G32B32_TYPELESS) {
          const UINT kAllowedTypelessSupport1 =
              D3D12_FORMAT_SUPPORT1_TEXTURE1D |
              D3D12_FORMAT_SUPPORT1_TEXTURE2D |
              D3D12_FORMAT_SUPPORT1_TEXTURE3D |
              D3D12_FORMAT_SUPPORT1_TEXTURECUBE |
              D3D12_FORMAT_SUPPORT1_MIP |
              D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT;
          data->Support1 = static_cast<D3D12_FORMAT_SUPPORT1>(
              static_cast<UINT>(data->Support1) & kAllowedTypelessSupport1);
        }
        data->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
      }
      if (GetDXMTDevice().device().supportsPlacementSparse() &&
          !emulated_rgb32 &&
          SupportsTiledTextureFormat(device_->device(), data->Format, format))
        data->Support2 |= D3D12_FORMAT_SUPPORT2_TILED;
      return S_OK;
    }
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *>(feature_data);
      data->NumQualityLevels = 0;
      if (!data->SampleCount)
        return E_FAIL;
      if (data->Flags & ~D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE)
        return WARN_E_INVALIDARG(__func__);

      if (data->SampleCount == 1) {
        data->NumQualityLevels = 1;
        return S_OK;
      }

      // Reserved MSAA textures are not implemented. Keep tiled-resource
      // queries separate from ordinary texture support so applications do not
      // select a configuration that CreateReservedResource must reject.
      if (data->Flags &
          D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE)
        return S_OK;

      MTL_DXGI_FORMAT_DESC format = {};
      const auto query_format =
          ResolveDepthTypelessFormatForD3D12Caps(data->Format);
      if (FAILED(MTLQueryDXGIFormat(device_->device(), query_format, format))) {
        WARN("D3D12Device: CheckFeatureSupport(MSAA) unsupported format ",
             data->Format);
        return S_OK;
      }

      const auto caps = GetD3D12FormatCapability(device_->device(), format);
      if (IsSupportedD3D12SampleCount(data->SampleCount) &&
          device_->device().supportsTextureSampleCount(data->SampleCount) &&
          (data->SampleCount == 1 || HasFormatCapability(caps, FormatCapability::MSAA)))
        data->NumQualityLevels = 1;
      return S_OK;
    }
    case D3D12_FEATURE_FORMAT_INFO: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_FORMAT_INFO))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_FORMAT_INFO *>(feature_data);
      data->PlaneCount = 0;

      if (data->Format == DXGI_FORMAT_UNKNOWN) {
        data->PlaneCount = 1;
        return S_OK;
      }
      const auto &traits = GetDXGIFormatTraits(data->Format);
      if (traits.classification == DXGIFormatClass::Mask)
        return WARN_E_INVALIDARG(__func__);
      if (traits.flags & DXGI_FORMAT_TRAIT_VIDEO) {
        data->PlaneCount = traits.planeCount;
        return S_OK;
      }

      MTL_DXGI_FORMAT_DESC format = {};
      if (FAILED(MTLQueryDXGIFormat(device_->device(), data->Format, format))) {
        WARN("D3D12Device: CheckFeatureSupport(FORMAT_INFO) unsupported format ",
             data->Format);
        return S_OK;
      }

      data->PlaneCount = GetD3D12FormatPlaneCountForFeatureData(data->Format, format);
      return S_OK;
    }
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT *>(feature_data);
      data->MaxGPUVirtualAddressBitsPerResource = 40;
      data->MaxGPUVirtualAddressBitsPerProcess = 40;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_MODEL: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_SHADER_MODEL))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_SHADER_MODEL *>(feature_data);
      if (data->HighestShaderModel >= D3D_SHADER_MODEL_6_0)
        data->HighestShaderModel = D3D_SHADER_MODEL_6_0;
      else
        data->HighestShaderModel = D3D_SHADER_MODEL_5_1;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS1: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS1 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->ExpandedComputeResourceStates = TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT: {
      if (feature_data_size !=
          sizeof(D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_SUPPORT))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<
          D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_SUPPORT *>(feature_data);
      if (data->NodeIndex != 0)
        return WARN_E_INVALIDARG(__func__);
      data->Support = D3D12_PROTECTED_RESOURCE_SESSION_SUPPORT_FLAG_NONE;
      return S_OK;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_ROOT_SIGNATURE *>(feature_data);
      if (data->HighestVersion == D3D_ROOT_SIGNATURE_VERSION_1_0)
        return S_OK;
      if (data->HighestVersion == D3D_ROOT_SIGNATURE_VERSION_1_1) {
        data->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        return S_OK;
      }
      return WARN_E_INVALIDARG(__func__);
    }
    case D3D12_FEATURE_ARCHITECTURE1: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE1 *>(feature_data);
      if (data->NodeIndex != 0)
        return WARN_E_INVALIDARG(__func__);

      const bool unified_memory = GetMTLDevice().hasUnifiedMemory();
      data->TileBasedRenderer = TRUE;
      data->UMA = unified_memory;
      data->CacheCoherentUMA = unified_memory;
      data->IsolatedMMU = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS2: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS2))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS2 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->ProgrammableSamplePositionsTier =
          D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_CACHE: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_SHADER_CACHE))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_SHADER_CACHE *>(feature_data);
      data->SupportFlags = D3D12_SHADER_CACHE_SUPPORT_NONE;
      return S_OK;
    }
    case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY *>(feature_data);
      data->PriorityForTypeIsSupported =
          IsSupportedCommandListType(data->CommandListType) &&
          IsSupportedCommandQueuePriority(data->Priority);
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS3: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS3))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS3 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->CastingFullyTypedFormatSupported = TRUE;
      data->CopyQueueTimestampQueriesSupported = TRUE;
      data->WriteBufferImmediateSupportFlags =
          D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT |
          D3D12_COMMAND_LIST_SUPPORT_FLAG_BUNDLE |
          D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE |
          D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY;
      data->ViewInstancingTier = D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_EXISTING_HEAPS: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_EXISTING_HEAPS))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_EXISTING_HEAPS *>(feature_data);
      data->Supported = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS4: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS4))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS4 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->SharedResourceCompatibilityTier =
          D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_SERIALIZATION: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_SERIALIZATION))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_SERIALIZATION *>(feature_data);
      if (data->NodeIndex != 0)
        return WARN_E_INVALIDARG(__func__);
      data->HeapSerializationTier = D3D12_HEAP_SERIALIZATION_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_CROSS_NODE: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_CROSS_NODE))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_CROSS_NODE *>(feature_data);
      data->SharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
      data->AtomicShaderInstructions = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS5: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->RenderPassesTier = D3D12_RENDER_PASS_TIER_0;
      data->RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_DISPLAYABLE: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_DISPLAYABLE))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_DISPLAYABLE *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->SharedResourceCompatibilityTier =
          D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPE_COUNT: {
      if (feature_data_size != sizeof(
                                   D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPE_COUNT))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<
          D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPE_COUNT *>(feature_data);
      if (data->NodeIndex != 0)
        return WARN_E_INVALIDARG(__func__);
      data->Count = 0;
      return S_OK;
    }
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES: {
      if (feature_data_size != sizeof(
                                   D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPES))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<
          D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPES *>(feature_data);
      if (data->NodeIndex != 0 || (data->Count && !data->pTypes))
        return WARN_E_INVALIDARG(__func__);
      data->Count = 0;
      return S_OK;
    }
#ifdef __ID3D12GraphicsCommandList4_INTERFACE_DEFINED__
    case D3D12_FEATURE_QUERY_META_COMMAND: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_QUERY_META_COMMAND))
        return WARN_E_INVALIDARG(__func__);
      const auto *data =
          static_cast<D3D12_FEATURE_DATA_QUERY_META_COMMAND *>(feature_data);
      if (data->NodeMask > 1)
        return WARN_E_INVALIDARG(__func__);
      return DXGI_ERROR_NOT_FOUND;
    }
    case D3D12_FEATURE_D3D12_OPTIONS6: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS6))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS6 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->VariableShadingRateTier =
          D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS7: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS7 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->MeshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
      data->SamplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS8: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS8))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS8 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS9: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS9))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS9 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->WaveMMATier = D3D12_WAVE_MMA_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS10: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS10))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS10 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS11: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS11))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS11 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS12: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS12))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS12 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->MSPrimitivesPipelineStatisticIncludesCulledPrimitives =
          D3D12_TRI_STATE_UNKNOWN;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS13: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS13))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS13 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS14: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS14))
        return WARN_E_INVALIDARG(__func__);
      auto *data =
          static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS14 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS15: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS15))
        return WARN_E_INVALIDARG(__func__);
      auto *data =
          static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS15 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS16: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS16))
        return WARN_E_INVALIDARG(__func__);
      auto *data =
          static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS16 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS17: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS17))
        return WARN_E_INVALIDARG(__func__);
      auto *data =
          static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS17 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS18: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS18))
        return WARN_E_INVALIDARG(__func__);
      auto *data =
          static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS18 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
#endif
    default:
      WARN("D3D12Device: CheckFeatureSupport unsupported feature ", feature);
      return DXGI_ERROR_UNSUPPORTED;
    }
  }

  HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *desc,
                                                 REFIID riid, void **descriptor_heap) override {
    InitReturnPtr(descriptor_heap);
    if (!desc || desc->NumDescriptors == 0)
      return WARN_E_INVALIDARG(__func__);
    if (desc->NodeMask > 1 ||
        (static_cast<UINT>(desc->Flags) &
         ~static_cast<UINT>(D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)))
      return WARN_E_INVALIDARG(__func__);

    switch (desc->Type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
      if ((desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
           desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) &&
          (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
        return WARN_E_INVALIDARG(__func__);
      break;
    default:
      return WARN_E_INVALIDARG(__func__);
    }
    if (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
        (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) &&
        desc->NumDescriptors > 2048)
      return WARN_E_INVALIDARG(__func__);
    if (!descriptor_heap)
      return S_FALSE;
    if (ShouldInjectCreationFailure(
            "DXMT_TEST_FAIL_DESCRIPTOR_HEAP_CREATION_AT",
            g_test_descriptor_heap_creation_occurrence))
      return E_OUTOFMEMORY;

    auto heap = d3d12::CreateDescriptorHeap(
        static_cast<IMTLD3D12Device *>(this), desc);
    auto hr = heap->QueryInterface(riid, descriptor_heap);
    if (dxmt::apitrace::d3d_enabled()) {
      uint64_t cpu_start = 0;
      uint64_t gpu_start = 0;
      if (SUCCEEDED(hr) && descriptor_heap && *descriptor_heap) {
        auto *heap_object = static_cast<ID3D12DescriptorHeap *>(*descriptor_heap);
        cpu_start = heap_object->GetCPUDescriptorHandleForHeapStart().ptr;
        if (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
          gpu_start = heap_object->GetGPUDescriptorHandleForHeapStart().ptr;
        }
      }
      dxmt::apitrace::record_create_descriptor_heap(
          this, desc, descriptor_heap ? *descriptor_heap : nullptr,
          desc ? GetDescriptorHandleIncrementSize(desc->Type) : 0,
          cpu_start, gpu_start,
          static_cast<int32_t>(hr));
    }
    return hr;
  }

  UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
    switch (descriptor_heap_type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
      return sizeof(DescriptorRecord);
    default:
      return 0;
    }
  }

  HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT node_mask, const void *bytecode,
                                                SIZE_T bytecode_length, REFIID riid,
                                                void **root_signature) override {
    InitReturnPtr(root_signature);
    if (!root_signature)
      return E_POINTER;
    if (ShouldInjectCreationFailure(
            "DXMT_TEST_FAIL_ROOT_SIGNATURE_CREATION_AT",
            g_test_root_signature_creation_occurrence))
      return E_OUTOFMEMORY;
    if (node_mask > 1 || (!bytecode && bytecode_length))
      return WARN_E_INVALIDARG(__func__);

    auto object = d3d12::CreateRootSignatureFromBlob(
        static_cast<IMTLD3D12Device *>(this),
        std::span<const std::byte>(static_cast<const std::byte *>(bytecode),
                                   bytecode_length));
    if (!object)
      return WARN_E_INVALIDARG(__func__);

    auto hr = object->QueryInterface(riid, root_signature);
    if (dxmt::apitrace::d3d_enabled()) {
      dxmt::apitrace::record_create_root_signature(
          this, node_mask, bytecode, bytecode_length,
          root_signature ? *root_signature : nullptr, static_cast<int32_t>(hr));
    }
    return hr;
  }

  void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        "CreateConstantBufferView");
    if (!record)
      return;
    auto descriptor_lock = LockDescriptorRecordForWrite(*record);
    DescriptorContentRevisionCommitGuard revision_commit(
        DescriptorWriteAffectsShaderBinding(*record));
    ResetDescriptorRecord(*record);
    BeginDescriptorSlotWrite(
        descriptor_lock, *record, DescriptorRecordType::ConstantBufferView);
    record->type = DescriptorRecordType::ConstantBufferView;
    if (desc) {
      const bool null_cbv = !desc->BufferLocation && !desc->SizeInBytes;
      if (!null_cbv &&
          (desc->BufferLocation &
           (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1)))
        WARN("D3D12Device: CBV BufferLocation is not 256-byte aligned");
      if (!null_cbv &&
          (desc->SizeInBytes &
           (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1)))
        WARN("D3D12Device: CBV SizeInBytes is not 256-byte aligned");
      UINT64 offset = 0;
      auto *resource =
          LookupBufferResourceByGpuVirtualAddress(desc->BufferLocation, &offset);
      if (!null_cbv && resource &&
          resource->GetParentDevice() != static_cast<IMTLD3D12Device *>(this)) {
        ResetDescriptorRecord(*record);
        MaterializeDescriptorTableForWrite(this, descriptor_lock, *record);
        return;
      }
      if (!null_cbv && (!resource || !resource->GetBuffer())) {
        static std::atomic<uint32_t> unresolved_cbv_warnings = 0;
        if (ShouldLogRepeatedDescriptorWarning(unresolved_cbv_warnings))
          WARN("D3D12Device: CBV BufferLocation does not resolve to a buffer resource");
      } else if (!null_cbv && desc->SizeInBytes >
                 resource->GetResourceDesc().Width - offset) {
        WARN("D3D12Device: CBV range exceeds buffer resource size");
      }
      record->desc.cbv = *desc;
      record->has_desc = true;
    }
    MaterializeDescriptorTableForWrite(this, descriptor_lock, *record);
    if (dxmt::apitrace::d3d_enabled()) {
      UINT64 offset = 0;
      auto *resource = desc ? LookupBufferResourceByGpuVirtualAddress(
                                  desc->BufferLocation, &offset)
                            : nullptr;
      if (desc && desc->BufferLocation && desc->SizeInBytes && resource &&
          resource->GetBufferAllocation() &&
          resource->GetBufferAllocation()->mappedMemory(0)) {
        const UINT64 width = resource->GetResourceDesc().Width;
        if (offset < width) {
          const UINT64 end =
              std::min<UINT64>(width, offset + desc->SizeInBytes);
          if (end > offset) {
            const auto *mapped = static_cast<const char *>(
                resource->GetBufferAllocation()->mappedMemory(0));
            dxmt::apitrace::record_resource_snapshot(
                resource->GetD3D12Resource(), 0, offset, end,
                mapped + resource->GetHeapOffset() + offset,
                static_cast<size_t>(end - offset));
          }
        }
      }
      dxmt::apitrace::record_create_constant_buffer_view(
          this, desc, descriptor,
          resource ? resource->GetD3D12Resource() : nullptr, offset,
          resource ? resource->GetResourceDesc().Width : 0);
    }
  }

  void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource *resource,
                                                  const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        "CreateShaderResourceView");
    if (!record)
      return;
    auto descriptor_lock = LockDescriptorRecordForWrite(*record);
    DescriptorContentRevisionCommitGuard revision_commit(
        DescriptorWriteAffectsShaderBinding(*record));
    ResetDescriptorRecord(*record);
    BeginDescriptorSlotWrite(
        descriptor_lock, *record, DescriptorRecordType::ShaderResourceView);
    if (!ResourceBelongsToDevice(resource,
                                 static_cast<IMTLD3D12Device *>(this))) {
      ResetDescriptorRecord(*record);
      MaterializeDescriptorTableForWrite(this, descriptor_lock, *record);
      return;
    }
    record->type = DescriptorRecordType::ShaderResourceView;
    record->resource = resource;
    if (desc) {
      if (!ValidateShaderResourceView(resource, *desc)) {
        ResetDescriptorRecord(*record);
        MaterializeDescriptorTableForWrite(this, descriptor_lock, *record);
        return;
      }
      record->desc.srv = *desc;
      record->has_desc = true;
    }
    MaterializeDescriptorTableForWrite(this, descriptor_lock, *record);
    if (dxmt::apitrace::d3d_enabled()) {
      dxmt::apitrace::record_create_shader_resource_view(
          this, resource, desc, descriptor);
    }
  }

  void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource *resource,
                                                   ID3D12Resource *counter_resource,
                                                   const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        "CreateUnorderedAccessView");
    if (!record)
      return;
    auto descriptor_lock = LockDescriptorRecordForWrite(*record);
    DescriptorContentRevisionCommitGuard revision_commit(
        DescriptorWriteAffectsShaderBinding(*record));
    ResetDescriptorRecord(*record);
    BeginDescriptorSlotWrite(
        descriptor_lock, *record, DescriptorRecordType::UnorderedAccessView);
    if (!ResourceBelongsToDevice(resource,
                                 static_cast<IMTLD3D12Device *>(this)) ||
        !ResourceBelongsToDevice(counter_resource,
                                 static_cast<IMTLD3D12Device *>(this))) {
      ResetDescriptorRecord(*record);
      MaterializeDescriptorTableForWrite(this, descriptor_lock, *record);
      return;
    }
    record->type = DescriptorRecordType::UnorderedAccessView;
    record->resource = resource;
    record->counter_resource = counter_resource;
    if (counter_resource && !desc) {
      ResetDescriptorRecord(*record);
      MaterializeDescriptorTableForWrite(this, descriptor_lock, *record);
      return;
    }
    if (desc) {
      if (!ValidateUnorderedAccessView(resource, counter_resource, *desc)) {
        ResetDescriptorRecord(*record);
        MaterializeDescriptorTableForWrite(this, descriptor_lock, *record);
        return;
      }
      record->desc.uav = *desc;
      record->has_desc = true;
    }
    MaterializeDescriptorTableForWrite(this, descriptor_lock, *record);
    if (dxmt::apitrace::d3d_enabled())
      dxmt::apitrace::record_create_unordered_access_view(
          this, resource, counter_resource, desc, descriptor);
  }

  void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource *resource,
                                                const D3D12_RENDER_TARGET_VIEW_DESC *desc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        "CreateRenderTargetView");
    if (!record)
      return;
    auto descriptor_lock = LockDescriptorRecordForWrite(*record);
    ResetDescriptorRecord(*record);
    if (!ResourceBelongsToDevice(resource,
                                 static_cast<IMTLD3D12Device *>(this)))
      return;
    record->type = DescriptorRecordType::RenderTargetView;
    record->resource = resource;
    if (desc) {
      if (!ValidateRenderTargetView(resource, *desc)) {
        ResetDescriptorRecord(*record);
        return;
      }
      record->desc.rtv = *desc;
      record->has_desc = true;
    }
    if (dxmt::apitrace::d3d_enabled())
      dxmt::apitrace::record_create_render_target_view(
          this, resource, desc, descriptor);
  }

  void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource *resource,
                                                const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
        "CreateDepthStencilView");
    if (!record)
      return;
    ResetDescriptorRecord(*record);
    if (!ResourceBelongsToDevice(resource,
                                 static_cast<IMTLD3D12Device *>(this)))
      return;
    record->type = DescriptorRecordType::DepthStencilView;
    record->resource = resource;
    if (desc) {
      if (!ValidateDepthStencilView(resource, *desc)) {
        WARN("D3D12Device: rejecting invalid DSV descriptor"
             " cpuHandle=", uint64_t(descriptor.ptr),
             " heapIndex=", record->heap_index,
             " heapCount=", record->heap_count);
        ResetDescriptorRecord(*record);
        return;
      }
      record->desc.dsv = *desc;
      record->has_desc = true;
    }
    if (dxmt::apitrace::d3d_enabled())
      dxmt::apitrace::record_create_depth_stencil_view(
          this, resource, desc, descriptor);
  }

  void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC *desc,
                                       D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, "CreateSampler");
    if (!record)
      return;
    auto descriptor_lock = LockDescriptorRecordForWrite(*record);
    DescriptorContentRevisionCommitGuard revision_commit(
        DescriptorWriteAffectsShaderBinding(*record));
    ResetDescriptorRecord(*record);
    record->type = DescriptorRecordType::Sampler;
    if (desc) {
      record->desc.sampler = *desc;
      record->has_desc = true;
    }
    BeginDescriptorSlotWrite(descriptor_lock, *record,
                             DescriptorRecordType::Sampler);
    MaterializeSamplerMirrorForWrite(GetMTLDevice(), descriptor_lock, *record);
    MaterializeDescriptorTableForWrite(this, descriptor_lock, *record);
    if (dxmt::apitrace::d3d_enabled())
      dxmt::apitrace::record_create_sampler(this, desc, descriptor);
  }

  void STDMETHODCALLTYPE CopyDescriptors(UINT dst_descriptor_range_count,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets,
                                         const UINT *dst_descriptor_range_sizes,
                                         UINT src_descriptor_range_count,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets,
                                         const UINT *src_descriptor_range_sizes,
                                         D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
    if (!dst_descriptor_range_count)
      return;
    if ((dst_descriptor_range_count && !dst_descriptor_range_offsets) ||
        (src_descriptor_range_count && !src_descriptor_range_offsets)) {
      WARN("D3D12Device: CopyDescriptors called with null range offsets");
      return;
    }

    std::vector<DescriptorRecord *> destinations;
    std::vector<DescriptorRecordLease> destination_leases;
    UINT64 dst_total = 0;
    for (UINT dst_range = 0; dst_range < dst_descriptor_range_count; dst_range++) {
      const UINT dst_count = dst_descriptor_range_sizes
                                 ? dst_descriptor_range_sizes[dst_range]
                                 : 1;
      if (!dst_count)
        continue;
      auto dst_lease = d3d12::GetDescriptorRecordRangeFromCpuHandle(
          dst_descriptor_range_offsets[dst_range], descriptor_heap_type,
          dst_count, "CopyDescriptors destination");
      if (!dst_lease || !dst_lease.owner() ||
          dst_lease.owner()->GetParentDevice() !=
              static_cast<IMTLD3D12Device *>(this))
        return;
      auto *dst = dst_lease.get();
      if (dst_total > UINT_MAX - dst_count) {
        WARN("D3D12Device: CopyDescriptors destination descriptor count overflow");
        return;
      }
      dst_total += dst_count;
      destinations.reserve(static_cast<size_t>(dst_total));
      for (UINT i = 0; i < dst_count; i++)
        destinations.push_back(dst + i);
      destination_leases.push_back(std::move(dst_lease));
    }
    if (destinations.empty())
      return;

    std::vector<DescriptorRecord> copied;
    copied.reserve(destinations.size());
    for (UINT src_range = 0; src_range < src_descriptor_range_count; src_range++) {
      const UINT src_count = src_descriptor_range_sizes
                                 ? src_descriptor_range_sizes[src_range]
                                 : 1;
      if (!src_count)
        continue;
      auto src_lease = d3d12::GetDescriptorRecordRangeFromCpuHandle(
          src_descriptor_range_offsets[src_range], descriptor_heap_type,
          src_count, "CopyDescriptors source");
      if (!src_lease || !src_lease.owner() ||
          src_lease.owner()->GetParentDevice() !=
              static_cast<IMTLD3D12Device *>(this))
        return;
      auto *src = src_lease.get();
      if (src[0].shader_visible) {
        WARN("D3D12Device: CopyDescriptors source heap must not be "
             "shader-visible");
        return;
      }
      for (UINT i = 0; i < src_count && copied.size() < destinations.size(); i++) {
        auto source_lock = LockDescriptorRecordForWrite(src[i]);
        copied.push_back(src[i]);
      }
      if (copied.size() == destinations.size())
        break;
    }
    if (copied.size() != destinations.size()) {
      WARN("D3D12Device: CopyDescriptors source descriptor count is smaller "
           "than destination descriptor count");
      return;
    }

    const bool affects_shader_binding = std::any_of(
        destinations.begin(), destinations.end(), [](const auto *record) {
          return record && DescriptorWriteAffectsShaderBinding(*record);
        });
    if (affects_shader_binding)
      RecordDescriptorContentCopyPerf();
    // Publish each destination record and its stale marker under the same
    // heap lock. Queue-side snapshot readers can therefore observe either the
    // old descriptor or the complete new descriptor, never an in-between
    // record paired with the wrong generation.
    for (size_t i = 0; i < destinations.size(); i++) {
      auto destination_lock =
          LockDescriptorRecordForWrite(*destinations[i]);
      DescriptorContentRevisionCommitGuard revision_commit(
          DescriptorWriteAffectsShaderBinding(*destinations[i]));
      CopyDescriptorRecord(*destinations[i], copied[i]);
      if (DescriptorWriteAffectsShaderBinding(*destinations[i]))
        BeginDescriptorSlotWrite(destination_lock, *destinations[i]);
      MaterializeSamplerMirrorForWrite(GetMTLDevice(), destination_lock,
                                       *destinations[i]);
      MaterializeDescriptorTableForWrite(this, destination_lock,
                                         *destinations[i]);
    }

    if (dxmt::apitrace::d3d_enabled())
      dxmt::apitrace::record_copy_descriptors(
          this,
          dst_descriptor_range_count,
          dst_descriptor_range_offsets,
          dst_descriptor_range_sizes,
          src_descriptor_range_count,
          src_descriptor_range_offsets,
          src_descriptor_range_sizes,
          descriptor_heap_type,
          GetDescriptorHandleIncrementSize(descriptor_heap_type));
  }

  void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT descriptor_count,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset,
                                               D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
    if (!descriptor_count)
      return;
    auto dst_lease = d3d12::GetDescriptorRecordRangeFromCpuHandle(
        dst_descriptor_range_offset, descriptor_heap_type, descriptor_count,
        "CopyDescriptorsSimple destination");
    auto src_lease = d3d12::GetDescriptorRecordRangeFromCpuHandle(
        src_descriptor_range_offset, descriptor_heap_type, descriptor_count,
        "CopyDescriptorsSimple source");
    if (!dst_lease || !src_lease || !dst_lease.owner() ||
        !src_lease.owner() ||
        dst_lease.owner()->GetParentDevice() !=
            static_cast<IMTLD3D12Device *>(this) ||
        src_lease.owner()->GetParentDevice() !=
            static_cast<IMTLD3D12Device *>(this))
      return;
    auto *dst = dst_lease.get();
    auto *src = src_lease.get();
    if (src[0].shader_visible) {
      WARN("D3D12Device: CopyDescriptorsSimple source heap must not be "
           "shader-visible");
      return;
    }
    std::vector<DescriptorRecord> copied;
    copied.reserve(descriptor_count);
    for (UINT i = 0; i < descriptor_count; i++) {
      auto source_lock = LockDescriptorRecordForWrite(src[i]);
      copied.push_back(src[i]);
    }
    const bool affects_shader_binding = DescriptorWriteAffectsShaderBinding(dst[0]);
    if (affects_shader_binding)
      RecordDescriptorContentCopyPerf();
    for (UINT i = 0; i < descriptor_count; i++) {
      auto destination_lock = LockDescriptorRecordForWrite(dst[i]);
      DescriptorContentRevisionCommitGuard revision_commit(
          DescriptorWriteAffectsShaderBinding(dst[i]));
      CopyDescriptorRecord(dst[i], copied[i]);
      if (DescriptorWriteAffectsShaderBinding(dst[i]))
        BeginDescriptorSlotWrite(destination_lock, dst[i]);
      MaterializeSamplerMirrorForWrite(GetMTLDevice(), destination_lock,
                                       dst[i]);
      MaterializeDescriptorTableForWrite(this, destination_lock, dst[i]);
    }

    if (dxmt::apitrace::d3d_enabled())
      dxmt::apitrace::record_copy_descriptors_simple(
          this,
          descriptor_count,
          dst_descriptor_range_offset,
          src_descriptor_range_offset,
          descriptor_heap_type,
          GetDescriptorHandleIncrementSize(descriptor_heap_type));
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo(D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
                            UINT resource_desc_count, const D3D12_RESOURCE_DESC *resource_descs) override {
    *__ret = GetResourceAllocationInfoImpl(visible_mask, resource_desc_count, resource_descs);
    return __ret;
  }
#else
  D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE
  GetResourceAllocationInfo(UINT visible_mask, UINT resource_desc_count,
                            const D3D12_RESOURCE_DESC *resource_descs) override {
    return GetResourceAllocationInfoImpl(visible_mask, resource_desc_count, resource_descs);
  }
#endif

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE
  GetCustomHeapProperties(D3D12_HEAP_PROPERTIES *__ret, UINT node_mask,
                          D3D12_HEAP_TYPE heap_type) override {
    *__ret = GetCustomHeapPropertiesImpl(node_mask, heap_type);
    return __ret;
  }
#else
  D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE
  GetCustomHeapProperties(UINT node_mask, D3D12_HEAP_TYPE heap_type) override {
    return GetCustomHeapPropertiesImpl(node_mask, heap_type);
  }
#endif

  HRESULT STDMETHODCALLTYPE
  CreateCommittedResource(const D3D12_HEAP_PROPERTIES *heap_properties,
                          D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc,
                          D3D12_RESOURCE_STATES initial_state,
                          const D3D12_CLEAR_VALUE *optimized_clear_value,
                          REFIID riid, void **resource) override {
    dxmt::perf::ScopedFrameTimer perf_timer(
        dxmt::perf::FrameTimeBucket::CreateResource);
    BeginDeviceCall("CreateCommittedResource");
    InitReturnPtr(resource);
    if (desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        optimized_clear_value)
      return WARN_E_INVALIDARG(__func__);
    if (!IsValidCommittedResourceDesc(heap_properties, heap_flags, desc,
                                      initial_state))
      return WARN_E_INVALIDARG(__func__);
    if (!resource)
      return S_FALSE;
    if (ShouldInjectCreationFailure(
            "DXMT_TEST_FAIL_RESOURCE_CREATION_AT",
            g_test_resource_creation_occurrence))
      return E_OUTOFMEMORY;
    const char *metal_fault = desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                                  ? "DXMT_TEST_FAIL_METAL_BUFFER_CREATION_AT"
                                  : "DXMT_TEST_FAIL_METAL_TEXTURE_CREATION_AT";
    auto &metal_occurrence = desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                                 ? g_test_metal_buffer_creation_occurrence
                                 : g_test_metal_texture_creation_occurrence;
    if (ShouldInjectCreationFailure(metal_fault, metal_occurrence)) {
      RecordInjectedFault(metal_fault);
      return E_OUTOFMEMORY;
    }

    auto resource_object = d3d12::CreateResource(
        static_cast<IMTLD3D12Device *>(this), heap_properties, heap_flags,
        desc, initial_state, 0, optimized_clear_value);
    auto hr = resource_object->QueryInterface(riid, resource);
    if (dxmt::apitrace::d3d_enabled()) {
      auto *created = resource && *resource ? dynamic_cast<d3d12::Resource *>(
                                               static_cast<ID3D12Resource *>(*resource))
                                             : nullptr;
      dxmt::apitrace::record_create_committed_resource(
          this, heap_properties, heap_flags, desc, initial_state,
          optimized_clear_value, resource ? *resource : nullptr,
          created ? created->GetGpuVirtualAddress() : 0, static_cast<int32_t>(hr));
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC *desc, REFIID riid,
                                       void **heap) override {
    dxmt::perf::ScopedFrameTimer perf_timer(
        dxmt::perf::FrameTimeBucket::CreateHeap);
    BeginDeviceCall("CreateHeap");
    InitReturnPtr(heap);
    if (!IsValidHeapDesc(desc))
      return WARN_E_INVALIDARG(__func__);
    if (!heap)
      return S_FALSE;
    if (ShouldInjectCreationFailure("DXMT_TEST_FAIL_HEAP_CREATION_AT",
                                    g_test_heap_creation_occurrence))
      return E_OUTOFMEMORY;
    if (ShouldInjectCreationFailure(
            "DXMT_TEST_FAIL_METAL_HEAP_CREATION_AT",
            g_test_metal_heap_creation_occurrence)) {
      RecordInjectedFault("DXMT_TEST_FAIL_METAL_HEAP_CREATION_AT");
      return E_OUTOFMEMORY;
    }

    auto heap_object = d3d12::CreateHeap(static_cast<IMTLD3D12Device *>(this),
                                         desc);
    auto hr = heap_object->QueryInterface(riid, heap);
    if (dxmt::apitrace::d3d_enabled()) {
      dxmt::apitrace::record_create_heap(
          this, desc, heap ? *heap : nullptr, static_cast<int32_t>(hr));
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap *heap, UINT64 heap_offset,
                                                 const D3D12_RESOURCE_DESC *desc,
                                                 D3D12_RESOURCE_STATES initial_state,
                                                 const D3D12_CLEAR_VALUE *optimized_clear_value,
                                                 REFIID riid, void **resource) override {
    dxmt::perf::ScopedFrameTimer perf_timer(
        dxmt::perf::FrameTimeBucket::CreateResource);
    BeginDeviceCall("CreatePlacedResource");
    InitReturnPtr(resource);
    if (!heap || !desc)
      return WARN_E_INVALIDARG(__func__);
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        optimized_clear_value)
      return WARN_E_INVALIDARG(__func__);

    auto *heap_object = dynamic_cast<d3d12::Heap *>(heap);
    if (!heap_object ||
        heap_object->GetParentDevice() !=
            static_cast<IMTLD3D12Device *>(this))
      return WARN_E_INVALIDARG(__func__);

    if (const char *reason =
            GetInvalidPlacedResourceDescReason(*heap_object, heap_offset, desc,
                                               initial_state)) {
      const auto &heap_desc = heap_object->GetHeapDesc();
      WARN("D3D12Device: CreatePlacedResource invalid"
           " reason=", reason,
           " heapOffset=", heap_offset,
           " heapSize=", heap_desc.SizeInBytes,
           " heapType=", heap_desc.Properties.Type,
           " cpuPage=", heap_desc.Properties.CPUPageProperty,
           " memoryPool=", heap_desc.Properties.MemoryPoolPreference,
           " heapFlags=", heap_desc.Flags,
           " dimension=", desc->Dimension,
           " width=", desc->Width,
           " height=", desc->Height,
           " depthOrArray=", desc->DepthOrArraySize,
           " mipLevels=", desc->MipLevels,
           " format=", desc->Format,
           " layout=", desc->Layout,
           " resourceFlags=", desc->Flags,
           " initialState=", initial_state);
      return WARN_E_INVALIDARG(__func__);
    }

    const auto &heap_desc = heap_object->GetHeapDesc();
    const auto &format_traits = GetDXGIFormatTraits(desc->Format);
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        (format_traits.flags & DXGI_FORMAT_TRAIT_VIDEO) &&
        format_traits.planeCount > 1) {
      WARN("D3D12Device: CreatePlacedResource does not support split-plane "
           "texture placement"
           " format=", desc->Format,
           " planes=", format_traits.planeCount);
      return E_NOTIMPL;
    }

    auto placement_heap = heap_object->GetPlacementHeap();
    auto placement_heap_residency =
        heap_object->GetPlacementHeapResidency();
    if (!placement_heap &&
        desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
      WARN("D3D12Device: CreatePlacedResource could not create placement heap"
           " heapSize=", heap_desc.SizeInBytes,
           " heapType=", heap_desc.Properties.Type,
           " heapFlags=", heap_desc.Flags,
           " dimension=", desc->Dimension);
      return E_NOTIMPL;
    }

    // Diagnostic only (no behavior change): sample placed allocations so the
    // last CreatePlacedResource before an IOGPUMetalHeap storage-mode assert
    // is visible in wine/d3d12 logs. Rate-limited; hot path is a counter bump.
    {
      const auto heap_type = d3d12::GetHeapType(heap_desc.Properties);
      // Metal placement heap storage: DEFAULT→Private, else Shared.
      const bool heap_metal_private = heap_type == D3D12_HEAP_TYPE_DEFAULT;
      // Resource allocation flags currently follow the same heap type mapping.
      const bool resource_metal_private = heap_type == D3D12_HEAP_TYPE_DEFAULT;
      static std::atomic<uint32_t> placed_diag = 0;
      const uint32_t n =
          placed_diag.fetch_add(1, std::memory_order_relaxed) + 1;
      // Prefer early samples + any non-DEFAULT heap (Shared Metal heap) +
      // textures (common assert path).
      const bool interesting =
          !heap_metal_private ||
          desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
          heap_desc.Properties.Type == D3D12_HEAP_TYPE_CUSTOM;
      if (n <= 32 || (interesting && (n % 32) == 0) || (n % 256) == 0) {
        WARN("D3D12Device: CreatePlacedResource diag"
             " heapOffset=", heap_offset,
             " heapSize=", heap_desc.SizeInBytes,
             " heapType=", heap_desc.Properties.Type,
             " resolvedHeapType=", heap_type,
             " cpuPage=", heap_desc.Properties.CPUPageProperty,
             " memoryPool=", heap_desc.Properties.MemoryPoolPreference,
             " heapFlags=", heap_desc.Flags,
             " placementHeap=", placement_heap ? placement_heap.handle : 0,
             " heapMetalPrivate=", heap_metal_private,
             " resourceMetalPrivate=", resource_metal_private,
             " usesMetalPlacementHeap=", placement_heap ? 1 : 0,
             " dimension=", desc->Dimension,
             " width=", desc->Width,
             " height=", desc->Height,
             " depthOrArray=", desc->DepthOrArraySize,
             " mips=", desc->MipLevels,
             " format=", desc->Format,
             " layout=", desc->Layout,
             " sampleCount=", desc->SampleDesc.Count,
             " resourceFlags=", desc->Flags,
             " initialState=", initial_state,
             " count=", n);
      }
    }

    if (!resource)
      return S_FALSE;
    dxmt::Buffer *placed_buffer =
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && !placement_heap
            ? heap_object->GetBuffer()
            : nullptr;
    dxmt::BufferAllocation *placed_buffer_allocation =
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && !placement_heap
            ? heap_object->GetAllocation()
            : nullptr;
    auto resource_object = d3d12::CreateResource(
        static_cast<IMTLD3D12Device *>(this), &heap_desc.Properties,
        heap_desc.Flags, desc, initial_state, heap_offset,
        optimized_clear_value, d3d12::ResourceKind::Placed,
        placed_buffer, placed_buffer_allocation, placement_heap,
        std::move(placement_heap_residency));
    auto *resource_impl = dynamic_cast<d3d12::Resource *>(resource_object.ptr());
    const bool allocation_valid =
        resource_impl &&
        (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
             ? resource_impl->GetBufferAllocation() != nullptr
             : resource_impl->GetTextureAllocation() != nullptr);
    if (!allocation_valid) {
      WARN("D3D12Device: CreatePlacedResource backing allocation failed"
           " heapOffset=", heap_offset,
           " heapSize=", heap_desc.SizeInBytes,
           " heapType=", heap_desc.Properties.Type,
           " resolvedHeapType=", d3d12::GetHeapType(heap_desc.Properties),
           " placementHeap=", placement_heap ? placement_heap.handle : 0,
           " dimension=", desc->Dimension,
           " width=", desc->Width,
           " height=", desc->Height,
           " format=", desc->Format,
           " layout=", desc->Layout,
           " resourceFlags=", desc->Flags);
      return E_OUTOFMEMORY;
    }
    auto hr = resource_object->QueryInterface(riid, resource);
    if (dxmt::apitrace::d3d_enabled()) {
      auto *created = resource && *resource ? dynamic_cast<d3d12::Resource *>(
                                               static_cast<ID3D12Resource *>(*resource))
                                             : nullptr;
      dxmt::apitrace::record_create_placed_resource(
          this, heap, heap_offset, desc, initial_state, optimized_clear_value,
          resource ? *resource : nullptr,
          created ? created->GetGpuVirtualAddress() : 0, static_cast<int32_t>(hr));
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC *desc,
                                                   D3D12_RESOURCE_STATES initial_state,
                                                   const D3D12_CLEAR_VALUE *optimized_clear_value,
                                                   REFIID riid, void **resource) override {
    dxmt::perf::ScopedFrameTimer perf_timer(
        dxmt::perf::FrameTimeBucket::CreateReservedResource);
    BeginDeviceCall("CreateReservedResource");
    InitReturnPtr(resource);
    const bool is_buffer =
        desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    if (is_buffer && optimized_clear_value)
      return WARN_E_INVALIDARG(__func__);
    if (!IsValidReservedResourceDesc(desc, initial_state))
      return WARN_E_INVALIDARG(__func__);
    if (!GetDXMTDevice().device().supportsPlacementSparse()) {
      if (dxmt::apitrace::d3d_enabled()) {
        dxmt::apitrace::record_create_reserved_resource(
            this, desc, initial_state, optimized_clear_value, nullptr, 0,
            static_cast<int32_t>(E_NOTIMPL));
      }
      return E_NOTIMPL;
    }
    if (!resource)
      return S_FALSE;

    const auto heap_properties = GetDefaultResourceHeapProperties();
    auto resource_object = d3d12::CreateResource(
        static_cast<IMTLD3D12Device *>(this), &heap_properties,
        D3D12_HEAP_FLAG_NONE, desc, initial_state, 0,
        optimized_clear_value,
        is_buffer ? d3d12::ResourceKind::ReservedBuffer
                  : d3d12::ResourceKind::ReservedTexture);
    auto *reserved = resource_object.ptr()
                         ? dynamic_cast<d3d12::Resource *>(resource_object.ptr())
                         : nullptr;
    const bool resource_ready =
        reserved && reserved->GetTiling() &&
        (is_buffer ? reserved->GetBufferAllocation() != nullptr
                   : reserved->GetTexture() != nullptr);
    if (!resource_ready) {
      const HRESULT failure =
          is_buffer && reserved && reserved->GetTiling()
              ? E_OUTOFMEMORY
              : E_NOTIMPL;
      WARN("D3D12Device: CreateReservedResource backend unavailable"
           " dimension=", desc->Dimension,
           " format=", desc->Format,
           " width=", desc->Width,
           " height=", desc->Height,
           " depthOrArray=", desc->DepthOrArraySize,
           " mipLevels=", desc->MipLevels,
           " sampleCount=", desc->SampleDesc.Count,
           " layout=", desc->Layout,
           " flags=", desc->Flags);
      if (dxmt::apitrace::d3d_enabled()) {
        dxmt::apitrace::record_create_reserved_resource(
            this, desc, initial_state, optimized_clear_value, nullptr, 0,
            static_cast<int32_t>(failure));
      }
      return failure;
    }
    auto hr = resource_object->QueryInterface(riid, resource);
    if (dxmt::apitrace::d3d_enabled()) {
      auto *created = resource && *resource ? dynamic_cast<d3d12::Resource *>(
                                               static_cast<ID3D12Resource *>(*resource))
                                             : nullptr;
      dxmt::apitrace::record_create_reserved_resource(
          this, desc, initial_state, optimized_clear_value,
          resource ? *resource : nullptr,
          created ? created->GetGpuVirtualAddress() : 0, static_cast<int32_t>(hr));
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild *object,
                                               const SECURITY_ATTRIBUTES *attributes,
                                               DWORD access, const WCHAR *name,
                                               HANDLE *handle) override {
    if (handle)
      *handle = nullptr;
    // TODO(d3d12): export shareable NT handles once resource/process ownership
    // is represented outside the local COM object lifetime.
    WARN("D3D12Device: shared handles are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE handle, REFIID riid, void **object) override {
    InitReturnPtr(object);
    WARN("D3D12Device: shared handles are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(const WCHAR *name, DWORD access,
                                                   HANDLE *handle) override {
    if (handle)
      *handle = nullptr;
    WARN("D3D12Device: named shared handles are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE MakeResident(UINT object_count, ID3D12Pageable *const *objects) override {
    return ValidateResidencyObjects(object_count, objects,
                                    static_cast<IMTLD3D12Device *>(this))
               ? S_OK
               : WARN_E_INVALIDARG(__func__);
  }

  HRESULT STDMETHODCALLTYPE Evict(UINT object_count, ID3D12Pageable *const *objects) override {
    return ValidateResidencyObjects(object_count, objects,
                                    static_cast<IMTLD3D12Device *>(this))
               ? S_OK
               : WARN_E_INVALIDARG(__func__);
  }

#ifdef __ID3D12Device3_INTERFACE_DEFINED__
  HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(const void *address,
                                                        REFIID riid,
                                                        void **heap) override {
    InitReturnPtr(heap);
    if (!heap)
      return E_POINTER;
    if (!address)
      return WARN_E_INVALIDARG(__func__);

    MEMORY_BASIC_INFORMATION memory_info = {};
    SIZE_T allocation_size = 0;
    const char *allocation_reason = nullptr;
    if (!GetVirtualAllocationInfo(address, memory_info, allocation_size,
                                  allocation_reason)) {
      WARN("D3D12Device: OpenExistingHeapFromAddress failed to query allocation"
           " address=", address,
           " reason=", allocation_reason ? allocation_reason : "unknown",
           " allocationBase=", memory_info.AllocationBase,
           " regionSize=", memory_info.RegionSize,
           " protect=", memory_info.Protect,
           " state=", memory_info.State,
           " type=", memory_info.Type);
      return E_INVALIDARG;
    }

    D3D12_HEAP_DESC desc = {};
    desc.SizeInBytes = allocation_size;
    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    desc.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    desc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    desc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    desc.Properties.CreationNodeMask = 1;
    desc.Properties.VisibleNodeMask = 1;
    desc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;

    static std::atomic<uint32_t> log_count = 0;
    if (ShouldLogRepeatedDescriptorWarning(log_count)) {
      WARN("D3D12Device: OpenExistingHeapFromAddress using external CPU backing"
           " address=", address,
           " allocationBase=", memory_info.AllocationBase,
           " regionSize=", memory_info.RegionSize,
           " allocationSize=", allocation_size,
           " protect=", memory_info.Protect,
           " state=", memory_info.State,
           " type=", memory_info.Type);
    }

    auto heap_object =
        d3d12::CreateExternalCpuHeap(static_cast<IMTLD3D12Device *>(this),
                                     &desc, memory_info.AllocationBase);
    return heap_object->QueryInterface(riid, heap);
  }

  HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(HANDLE file_mapping,
                                                            REFIID riid,
                                                            void **heap) override {
    InitReturnPtr(heap);
    if (!heap)
      return E_POINTER;
    if (!file_mapping)
      return WARN_E_INVALIDARG(__func__);
    WARN("D3D12Device: existing heaps from file mappings are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE EnqueueMakeResident(D3D12_RESIDENCY_FLAGS flags,
                                                UINT object_count,
                                                ID3D12Pageable *const *objects,
                                                ID3D12Fence *fence,
                                                UINT64 fence_value) override {
    if (flags & ~D3D12_RESIDENCY_FLAG_DENY_OVERBUDGET)
      return WARN_E_INVALIDARG(__func__);
    if (!DeviceChildBelongsToDevice(
            fence, static_cast<IMTLD3D12Device *>(this)))
      return WARN_E_INVALIDARG(__func__);
    HRESULT hr = MakeResident(object_count, objects);
    if (FAILED(hr))
      return hr;
    return fence->Signal(fence_value);
  }
#endif

  HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(const void *blob,
                                                  SIZE_T blob_size,
                                                  REFIID iid,
                                                  void **lib) override {
    InitReturnPtr(lib);
    (void)blob;
    (void)blob_size;
    (void)iid;
    // CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE) reports NONE. D3D12
    // requires CreatePipelineLibrary to fail with DXGI_ERROR_UNSUPPORTED when
    // D3D12_SHADER_CACHE_SUPPORT_LIBRARY is not advertised; returning an empty
    // library would falsely promise persistence while discarding its blob.
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(
      ID3D12Fence *const *fences, const UINT64 *values, UINT fence_count,
      D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags, HANDLE event) override {
    if (!fence_count || !fences || !values)
      return WARN_E_INVALIDARG(__func__);
    if (static_cast<UINT>(flags) &
        ~static_cast<UINT>(D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY))
      return WARN_E_INVALIDARG(__func__);

    for (UINT i = 0; i < fence_count; i++) {
      if (!fences[i] ||
          !DeviceChildBelongsToDevice(
              fences[i], static_cast<IMTLD3D12Device *>(this)) ||
          !dynamic_cast<Fence *>(fences[i]))
        return WARN_E_INVALIDARG(__func__);
    }

    std::vector<FencePrivateReference> wait_fences;
    wait_fences.reserve(fence_count);
    std::vector<UINT64> wait_values(values, values + fence_count);
    for (UINT i = 0; i < fence_count; i++) {
      auto *fence = dynamic_cast<Fence *>(fences[i]);
      wait_fences.emplace_back(fence);
    }

    if (AreFenceValuesSatisfied(wait_fences, wait_values, flags)) {
      if (event) {
        auto signal = device_->device().newSharedEvent();
        MTLSharedEvent_setWin32EventAtValue(
            signal.handle, device_->queue().GetSharedEventListener(), event, 1);
        signal.signalValue(1);
      }
      return S_OK;
    }

    if (!event) {
      while (!AreFenceValuesSatisfied(wait_fences, wait_values, flags))
        dxmt::this_thread::yield();
      return S_OK;
    }

    const bool wait_any = flags & D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY;
    std::vector<size_t> pending_indices;
    pending_indices.reserve(wait_fences.size());
    for (size_t i = 0; i < wait_fences.size(); i++) {
      if (wait_fences[i].Get().GetCompletedValue() >= wait_values[i]) {
        if (wait_any) {
          SetEvent(event);
          return S_OK;
        }
        continue;
      }
      pending_indices.push_back(i);
    }

    if (pending_indices.empty()) {
      SetEvent(event);
      return S_OK;
    }

    auto state = std::make_shared<MultipleFenceWaitState>(
        event, wait_any, std::move(wait_fences));
    state->Arm(static_cast<uint32_t>(pending_indices.size()));

    for (size_t index : pending_indices) {
      auto &fence = state->FenceAt(index);
      const auto value = wait_values[index];
      if (fence.GetCompletedValue() >= value) {
        state->CompleteFenceWait();
        continue;
      }

      fence.RegisterQueueWait(value, std::weak_ptr<FenceWaitTarget>(state));
    }

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  SetResidencyPriority(UINT object_count, ID3D12Pageable *const *objects,
                       const D3D12_RESIDENCY_PRIORITY *priorities) override {
    if (object_count && !priorities)
      return WARN_E_INVALIDARG(__func__);
    return ValidateResidencyObjects(object_count, objects,
                                    static_cast<IMTLD3D12Device *>(this))
               ? S_OK
               : WARN_E_INVALIDARG(__func__);
  }

  HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initial_value, D3D12_FENCE_FLAGS flags,
                                        REFIID riid, void **fence) override {
    InitReturnPtr(fence);
    if (!fence)
      return E_POINTER;
    if (ShouldInjectCreationFailure("DXMT_TEST_FAIL_FENCE_CREATION_AT",
                                    g_test_fence_creation_occurrence))
      return E_OUTOFMEMORY;
    if (flags & ~D3D12_FENCE_FLAG_SHARED) {
      WARN("D3D12Device::CreateFence: unsupported fence flags ", flags);
      return E_NOTIMPL;
    }

    auto fence_object = d3d12::CreateFence(static_cast<IMTLD3D12Device *>(this), initial_value, flags);
    auto hr = fence_object->QueryInterface(riid, fence);
    if (dxmt::apitrace::d3d_enabled()) {
      dxmt::apitrace::record_create_fence(
          this, initial_value, flags, fence ? *fence : nullptr,
          static_cast<int32_t>(hr));
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override {
    return device_->queue().HasDeviceError() ? DXGI_ERROR_DEVICE_REMOVED : S_OK;
  }

  void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC *desc,
                                               UINT first_sub_resource,
                                               UINT sub_resource_count, UINT64 base_offset,
                                               D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
                                               UINT *row_count, UINT64 *row_size,
                                               UINT64 *total_bytes) override {
    GetCopyableFootprintsImpl(desc, first_sub_resource, sub_resource_count,
                              base_offset, layouts, row_count, row_size,
                              total_bytes);
  }

    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *desc,
                                              REFIID riid, void **heap) override {
      InitReturnPtr(heap);
      if (!desc || desc->Count == 0 || desc->NodeMask > 1)
        return WARN_E_INVALIDARG(__func__);

      switch (desc->Type) {
      case D3D12_QUERY_HEAP_TYPE_OCCLUSION:
      case D3D12_QUERY_HEAP_TYPE_TIMESTAMP:
        break;
      case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS:
      case D3D12_QUERY_HEAP_TYPE_SO_STATISTICS:
        WARN("D3D12Device::CreateQueryHeap: pipeline and stream-output "
             "statistics are unsupported");
        return E_NOTIMPL;
      default:
        return WARN_E_INVALIDARG(__func__);
      }
      if (!heap)
        return S_FALSE;
      if (ShouldInjectCreationFailure(
              "DXMT_TEST_FAIL_QUERY_HEAP_CREATION_AT",
              g_test_query_heap_creation_occurrence))
        return E_OUTOFMEMORY;

      auto query_heap = d3d12::CreateQueryHeap(
          static_cast<IMTLD3D12Device *>(this), desc);
      auto hr = query_heap->QueryInterface(riid, heap);
      dxmt::apitrace::record_create_query_heap(
          this, desc, heap ? *heap : nullptr, static_cast<int32_t>(hr));
      return hr;
    }

  HRESULT STDMETHODCALLTYPE SetStablePowerState(WINBOOL enable) override {
    if (enable)
      WARN("D3D12Device: stable power state is unsupported");
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC *desc,
                                                   ID3D12RootSignature *root_signature,
                                                   REFIID riid, void **command_signature) override {
    InitReturnPtr(command_signature);
    if (!command_signature)
      return E_POINTER;
    if (!desc || !desc->NumArgumentDescs || !desc->pArgumentDescs ||
        !desc->ByteStride || desc->NodeMask > 1)
      return WARN_E_INVALIDARG(__func__);

    // 64-bit: each argument size is already capped at UINT_MAX, but summing
    // enough of them in 32 bits wraps to a small value and the ByteStride check
    // below then waves through a stride far too short for the arguments it
    // claims to describe.
    UINT64 min_stride = 0;
    UINT operation_count = 0;
    for (UINT i = 0; i < desc->NumArgumentDescs; i++) {
      const auto &argument = desc->pArgumentDescs[i];
      const auto argument_size = IndirectArgumentByteSize(argument);
      if (!argument_size) {
        WARN("D3D12Device::CreateCommandSignature: unsupported indirect "
             "argument type ",
             argument.Type);
        return E_NOTIMPL;
      }
      min_stride += argument_size;
      if (IsIndirectOperationArgument(argument.Type)) {
        if (++operation_count > 1 || i + 1 != desc->NumArgumentDescs) {
          WARN("D3D12Device::CreateCommandSignature: unsupported indirect "
               "operation layout");
          return WARN_E_INVALIDARG(__func__);
        }
      }
      switch (argument.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
        if (!root_signature)
          return WARN_E_INVALIDARG(__func__);
        break;
      default:
        break;
      }
    }
    if (!operation_count) {
      WARN("D3D12Device::CreateCommandSignature: missing indirect operation");
      return WARN_E_INVALIDARG(__func__);
    }
    if (min_stride > UINT_MAX || desc->ByteStride < min_stride)
      return WARN_E_INVALIDARG(__func__);
    if (desc->NumArgumentDescs != 1) {
      // State-changing signatures require GPU-side argument preprocessing.
      // The CPU expansion path cannot read legal DEFAULT-heap arguments, so
      // reject these signatures instead of accepting and later dropping work.
      WARN("D3D12Device::CreateCommandSignature: state-changing indirect "
           "signatures are unsupported");
      return E_NOTIMPL;
    }

    auto signature = d3d12::CreateCommandSignature(
        static_cast<IMTLD3D12Device *>(this), desc, root_signature);
    HRESULT hr = signature->QueryInterface(riid, command_signature);
    if (dxmt::apitrace::d3d_enabled()) {
      dxmt::apitrace::record_create_command_signature(
          this, desc, root_signature,
          SUCCEEDED(hr) && command_signature ? *command_signature : nullptr,
          hr);
    }
    return hr;
  }

  void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource *resource, UINT *total_tile_count,
                                           D3D12_PACKED_MIP_INFO *packed_mip_info,
                                           D3D12_TILE_SHAPE *standard_tile_shape,
                                           UINT *sub_resource_tiling_count,
                                           UINT first_sub_resource_tiling,
                                           D3D12_SUBRESOURCE_TILING *sub_resource_tilings) override {
    auto *resource_object = dynamic_cast<d3d12::Resource *>(resource);
    const auto *tiling = resource_object ? resource_object->GetTiling() : nullptr;
    if (!tiling) {
      if (resource) {
        WARN("D3D12Device: TODO GetResourceTiling unsupported for non-reserved or unsupported resource"
             " resource=", resource);
      }
      if (total_tile_count)
        *total_tile_count = 0;
      if (sub_resource_tiling_count)
        *sub_resource_tiling_count = 0;
      return;
    }

    UINT recorded_total_tile_count = 0;
    D3D12_PACKED_MIP_INFO recorded_packed_mip_info = {};
    D3D12_TILE_SHAPE recorded_tile_shape = {};
    std::vector<D3D12_SUBRESOURCE_TILING> recorded_subresource_tilings;

    if (total_tile_count)
      *total_tile_count = tiling->total_tile_count;
    recorded_total_tile_count = tiling->total_tile_count;
    if (packed_mip_info)
      *packed_mip_info = tiling->packed_mip_info;
    recorded_packed_mip_info = tiling->packed_mip_info;
    if (standard_tile_shape)
      *standard_tile_shape = tiling->tile_shape;
    recorded_tile_shape = tiling->tile_shape;
    if (!sub_resource_tiling_count) {
      dxmt::apitrace::record_get_resource_tiling(
          this, resource, recorded_total_tile_count,
          packed_mip_info ? &recorded_packed_mip_info : nullptr,
          standard_tile_shape ? &recorded_tile_shape : nullptr,
          0, first_sub_resource_tiling, nullptr);
      return;
    }

    const UINT available =
        first_sub_resource_tiling < tiling->subresources.size()
            ? static_cast<UINT>(tiling->subresources.size() -
                                first_sub_resource_tiling)
            : 0;
    const UINT count = std::min(*sub_resource_tiling_count, available);
    *sub_resource_tiling_count = count;
    recorded_subresource_tilings.resize(count);
    if (!sub_resource_tilings && count)
      sub_resource_tilings = recorded_subresource_tilings.data();
    if (!sub_resource_tilings) {
      dxmt::apitrace::record_get_resource_tiling(
          this, resource, recorded_total_tile_count,
          packed_mip_info ? &recorded_packed_mip_info : nullptr,
          standard_tile_shape ? &recorded_tile_shape : nullptr,
          count, first_sub_resource_tiling, nullptr);
      return;
    }
    for (UINT i = 0; i < count; ++i) {
      const auto &src = tiling->subresources[first_sub_resource_tiling + i];
      D3D12_SUBRESOURCE_TILING &dst = sub_resource_tilings[i];
      if (src.start_tile_index == D3D12_PACKED_TILE) {
        dst = {};
        dst.StartTileIndexInOverallResource = D3D12_PACKED_TILE;
      } else {
        dst.WidthInTiles = src.width_in_tiles;
        dst.HeightInTiles = src.height_in_tiles;
        dst.DepthInTiles = src.depth_in_tiles;
        dst.StartTileIndexInOverallResource = src.start_tile_index;
      }
      recorded_subresource_tilings[i] = dst;
    }
    dxmt::apitrace::record_get_resource_tiling(
        this, resource, recorded_total_tile_count,
        packed_mip_info ? &recorded_packed_mip_info : nullptr,
        standard_tile_shape ? &recorded_tile_shape : nullptr,
        count, first_sub_resource_tiling, recorded_subresource_tilings.data());
  }

#ifdef __ID3D12Device4_INTERFACE_DEFINED__
  HRESULT STDMETHODCALLTYPE CreateCommandList1(UINT node_mask,
                                               D3D12_COMMAND_LIST_TYPE type,
                                               D3D12_COMMAND_LIST_FLAGS flags,
                                               REFIID riid,
                                               void **command_list) override {
    if (D3D12DeviceDiagEnabled()) {
      WARN_FILE_ONLY("D3D12 diagnostic: CreateCommandList1 enter"
           " nodeMask=", node_mask,
           " type=", type,
           " flags=", flags,
           " riid=", str::format(riid));
    }
    InitReturnPtr(command_list);
    if (!command_list)
      return E_POINTER;
    if (flags)
      return WARN_E_INVALIDARG(__func__);
    if (node_mask > 1)
      return WARN_E_INVALIDARG(__func__);
    if (!IsSupportedCommandListType(type)) {
      WARN("D3D12Device: unsupported command list type ", type);
      return WARN_E_INVALIDARG(__func__);
    }

    HRESULT status = S_OK;
    auto list = d3d12::CreateGraphicsCommandList(
        static_cast<IMTLD3D12Device *>(this), node_mask, type, nullptr,
        nullptr, &status);
    if (!list)
      return status;
    if (auto *graphics_list = dynamic_cast<d3d12::GraphicsCommandList *>(list.ptr()))
      graphics_list->SetApitraceLifecycleRecordingEnabled(false);
    status = list->Close();
    if (auto *graphics_list = dynamic_cast<d3d12::GraphicsCommandList *>(list.ptr()))
      graphics_list->SetApitraceLifecycleRecordingEnabled(true);
    if (FAILED(status))
      return status;
    HRESULT hr = list->QueryInterface(riid, command_list);
    if (dxmt::apitrace::d3d_enabled()) {
      dxmt::apitrace::record_create_command_list1(
          this, node_mask, type, flags, command_list ? *command_list : nullptr,
          static_cast<int32_t>(hr));
    }
    if (D3D12DeviceDiagEnabled()) {
      WARN_FILE_ONLY("D3D12 diagnostic: CreateCommandList1 result"
           " closeStatus=", status,
           " hr=", hr,
           " list=", command_list && *command_list
                      ? reinterpret_cast<uintptr_t>(*command_list)
                      : 0);
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  CreateProtectedResourceSession(const D3D12_PROTECTED_RESOURCE_SESSION_DESC *desc,
                                 REFIID riid, void **session) override {
    InitReturnPtr(session);
    if (!session)
      return E_POINTER;
    if (!desc || desc->NodeMask > 1 ||
        desc->Flags != D3D12_PROTECTED_RESOURCE_SESSION_FLAG_NONE)
      return WARN_E_INVALIDARG(__func__);
    WARN("D3D12Device: protected resource sessions are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommittedResource1(const D3D12_HEAP_PROPERTIES *heap_properties,
                           D3D12_HEAP_FLAGS heap_flags,
                           const D3D12_RESOURCE_DESC *desc,
                           D3D12_RESOURCE_STATES initial_state,
                           const D3D12_CLEAR_VALUE *optimized_clear_value,
                           ID3D12ProtectedResourceSession *protected_session,
                           REFIID riid, void **resource) override {
    if (protected_session) {
      InitReturnPtr(resource);
      WARN("D3D12Device: protected committed resources are unsupported");
      return E_NOTIMPL;
    }
    return CreateCommittedResource(heap_properties, heap_flags, desc,
                                   initial_state, optimized_clear_value, riid,
                                   resource);
  }

  HRESULT STDMETHODCALLTYPE CreateHeap1(const D3D12_HEAP_DESC *desc,
                                        ID3D12ProtectedResourceSession *protected_session,
                                        REFIID riid, void **heap) override {
    if (protected_session) {
      InitReturnPtr(heap);
      WARN("D3D12Device: protected heaps are unsupported");
      return E_NOTIMPL;
    }
    return CreateHeap(desc, riid, heap);
  }

  HRESULT STDMETHODCALLTYPE
  CreateReservedResource1(const D3D12_RESOURCE_DESC *desc,
                          D3D12_RESOURCE_STATES initial_state,
                          const D3D12_CLEAR_VALUE *optimized_clear_value,
                          ID3D12ProtectedResourceSession *protected_session,
                          REFIID riid, void **resource) override {
    if (protected_session) {
      InitReturnPtr(resource);
      WARN("D3D12Device: protected reserved resources are unsupported");
      return E_NOTIMPL;
    }
    return CreateReservedResource(desc, initial_state, optimized_clear_value,
                                  riid, resource);
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo1(D3D12_RESOURCE_ALLOCATION_INFO *__ret,
                             UINT visible_mask, UINT resource_desc_count,
                             const D3D12_RESOURCE_DESC *resource_descs,
                             D3D12_RESOURCE_ALLOCATION_INFO1 *resource_info) override {
    *__ret = GetResourceAllocationInfo1Impl(visible_mask, resource_desc_count,
                                            resource_descs, resource_info);
    return __ret;
  }
#else
  D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE
  GetResourceAllocationInfo1(UINT visible_mask, UINT resource_desc_count,
                             const D3D12_RESOURCE_DESC *resource_descs,
                             D3D12_RESOURCE_ALLOCATION_INFO1 *resource_info) override {
    return GetResourceAllocationInfo1Impl(visible_mask, resource_desc_count,
                                          resource_descs, resource_info);
  }
#endif
#endif

#ifdef __ID3D12Device5_INTERFACE_DEFINED__
  HRESULT STDMETHODCALLTYPE CreateLifetimeTracker(ID3D12LifetimeOwner *owner,
                                                  REFIID riid,
                                                  void **tracker) override {
    InitReturnPtr(tracker);
    if (!tracker)
      return E_POINTER;
    if (!owner)
      return WARN_E_INVALIDARG(__func__);
    WARN("D3D12Device: lifetime trackers are unsupported");
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE RemoveDevice() override {
    device_->queue().MarkDeviceError();
  }

  HRESULT STDMETHODCALLTYPE EnumerateMetaCommands(UINT *meta_command_count,
                                                  D3D12_META_COMMAND_DESC *descs) override {
    if (!meta_command_count)
      return WARN_E_INVALIDARG(__func__);
    *meta_command_count = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  EnumerateMetaCommandParameters(REFGUID command_id,
                                 D3D12_META_COMMAND_PARAMETER_STAGE stage,
                                 UINT *total_structure_size,
                                 UINT *parameter_count,
                                 D3D12_META_COMMAND_PARAMETER_DESC *parameter_descs) override {
    if (!total_structure_size || !parameter_count)
      return WARN_E_INVALIDARG(__func__);
    if (*parameter_count && !parameter_descs)
      return WARN_E_INVALIDARG(__func__);
    *total_structure_size = 0;
    *parameter_count = 0;
    return DXGI_ERROR_NOT_FOUND;
  }

  HRESULT STDMETHODCALLTYPE CreateMetaCommand(REFGUID command_id, UINT node_mask,
                                              const void *creation_parameters,
                                              SIZE_T creation_parameters_size,
                                              REFIID riid, void **meta_command) override {
    InitReturnPtr(meta_command);
    if (!meta_command)
      return E_POINTER;
    if (node_mask > 1)
      return WARN_E_INVALIDARG(__func__);
    WARN("D3D12Device: meta commands are unsupported");
    return DXGI_ERROR_NOT_FOUND;
  }

  HRESULT STDMETHODCALLTYPE CreateStateObject(const D3D12_STATE_OBJECT_DESC *desc,
                                              REFIID riid,
                                              void **state_object) override {
    InitReturnPtr(state_object);
    if (!state_object)
      return E_POINTER;
    if (!desc)
      return WARN_E_INVALIDARG(__func__);
    WARN("D3D12Device: state objects are unsupported");
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE GetRaytracingAccelerationStructurePrebuildInfo(
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *desc,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info) override {
    if (info)
      std::memset(info, 0, sizeof(*info));
    WARN("D3D12Device: raytracing acceleration structures are unsupported");
  }

  D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE
  CheckDriverMatchingIdentifier(
      D3D12_SERIALIZED_DATA_TYPE serialized_data_type,
      const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier) override {
    return D3D12_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE;
  }
#endif

#ifdef __ID3D12Device6_INTERFACE_DEFINED__
  HRESULT STDMETHODCALLTYPE
  SetBackgroundProcessingMode(D3D12_BACKGROUND_PROCESSING_MODE mode,
                              D3D12_MEASUREMENTS_ACTION action, HANDLE event,
                              WINBOOL *further_measurements_desired) override {
    if (mode < D3D12_BACKGROUND_PROCESSING_MODE_ALLOWED ||
        mode > D3D12_BACKGROUND_PROCESSING_MODE_DISABLE_PROFILING_BY_SYSTEM ||
        action < D3D12_MEASUREMENTS_ACTION_KEEP_ALL ||
        action > D3D12_MEASUREMENTS_ACTION_DISCARD_PREVIOUS)
      return WARN_E_INVALIDARG(__func__);
    if (further_measurements_desired)
      *further_measurements_desired = FALSE;
    if (event)
      SetEvent(event);
    return S_OK;
  }
#endif

#ifdef __ID3D12Device7_INTERFACE_DEFINED__
  HRESULT STDMETHODCALLTYPE AddToStateObject(
      const D3D12_STATE_OBJECT_DESC *addition,
      ID3D12StateObject *state_object_to_grow_from, REFIID riid,
      void **new_state_object) override {
    InitReturnPtr(new_state_object);
    if (!new_state_object)
      return E_POINTER;
    if (!addition || !state_object_to_grow_from)
      return WARN_E_INVALIDARG(__func__);
    WARN("D3D12Device: state objects are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  CreateProtectedResourceSession1(
      const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *desc, REFIID riid,
      void **session) override {
    InitReturnPtr(session);
    if (!session)
      return E_POINTER;
    if (!desc || desc->NodeMask > 1 ||
        desc->Flags != D3D12_PROTECTED_RESOURCE_SESSION_FLAG_NONE)
      return WARN_E_INVALIDARG(__func__);
    WARN("D3D12Device: protected resource sessions are unsupported");
    return E_NOTIMPL;
  }
#endif

#ifdef __ID3D12Device8_INTERFACE_DEFINED__
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo2(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
      UINT resource_descs_count, const D3D12_RESOURCE_DESC1 *resource_descs,
      D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) override {
    *__ret = GetResourceAllocationInfo2Impl(visible_mask, resource_descs_count,
                                            resource_descs,
                                            resource_allocation_info1);
    return __ret;
  }
#else
  D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE
  GetResourceAllocationInfo2(
      UINT visible_mask, UINT resource_descs_count,
      const D3D12_RESOURCE_DESC1 *resource_descs,
      D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) override {
    return GetResourceAllocationInfo2Impl(visible_mask, resource_descs_count,
                                          resource_descs,
                                          resource_allocation_info1);
  }
#endif

  HRESULT STDMETHODCALLTYPE CreateCommittedResource2(
      const D3D12_HEAP_PROPERTIES *heap_properties,
      D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1 *desc,
      D3D12_RESOURCE_STATES initial_resource_state,
      const D3D12_CLEAR_VALUE *optimized_clear_value,
      ID3D12ProtectedResourceSession *protected_session,
      REFIID riid_resource, void **resource) override {
    const auto downgraded_desc = ToResourceDesc(desc);
    return CreateCommittedResource1(heap_properties, heap_flags,
                                    desc ? &downgraded_desc : nullptr,
                                    initial_resource_state,
                                    optimized_clear_value,
                                    protected_session, riid_resource,
                                    resource);
  }

  HRESULT STDMETHODCALLTYPE CreatePlacedResource1(
      ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC1 *desc,
      D3D12_RESOURCE_STATES initial_state,
      const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid,
      void **resource) override {
    const auto downgraded_desc = ToResourceDesc(desc);
    return CreatePlacedResource(heap, heap_offset,
                                desc ? &downgraded_desc : nullptr,
                                initial_state, optimized_clear_value, riid,
                                resource);
  }

  void STDMETHODCALLTYPE CreateSamplerFeedbackUnorderedAccessView(
      ID3D12Resource *targeted_resource, ID3D12Resource *feedback_resource,
      D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor) override {
    WARN("D3D12Device: sampler feedback UAVs are unsupported");
    if (auto record = GetDescriptorRecordForWrite(
            dst_descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            "sampler feedback UAV")) {
      auto descriptor_lock = LockDescriptorRecordForWrite(*record);
      DescriptorContentRevisionCommitGuard revision_commit(
          DescriptorWriteAffectsShaderBinding(*record));
      ResetDescriptorRecord(*record);
      BeginDescriptorSlotWrite(
          descriptor_lock, *record, DescriptorRecordType::UnorderedAccessView);
      MaterializeDescriptorTableForWrite(this, descriptor_lock, *record);
      dxmt::perf::recordDescriptorContentWrite(6);
    }
  }

  void STDMETHODCALLTYPE GetCopyableFootprints1(
      const D3D12_RESOURCE_DESC1 *resource_desc, UINT first_subresource,
      UINT subresources_count, UINT64 base_offset,
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows_count,
      UINT64 *row_size_in_bytes, UINT64 *total_bytes) override {
    const auto downgraded_desc = ToResourceDesc(resource_desc);
    GetCopyableFootprintsImpl(resource_desc ? &downgraded_desc : nullptr,
                              first_subresource,
                              subresources_count, base_offset, layouts,
                              rows_count, row_size_in_bytes, total_bytes);
  }
#endif

#ifdef __ID3D12Device9_INTERFACE_DEFINED__
  HRESULT STDMETHODCALLTYPE CreateShaderCacheSession(
      const D3D12_SHADER_CACHE_SESSION_DESC *desc, REFIID riid,
      void **session) override {
    InitReturnPtr(session);
    if (!session)
      return E_POINTER;
    if (!desc)
      return WARN_E_INVALIDARG(__func__);
    if (desc->Mode != D3D12_SHADER_CACHE_MODE_MEMORY &&
        desc->Mode != D3D12_SHADER_CACHE_MODE_DISK)
      return WARN_E_INVALIDARG(__func__);
    // Cast to UINT before OR: mingw DEFINE_ENUM_FLAG_OPERATORS operator| is
    // not constexpr, so enum|enum cannot initialize a constexpr UINT.
    constexpr UINT known_flags =
        static_cast<UINT>(D3D12_SHADER_CACHE_FLAG_DRIVER_VERSIONED) |
        static_cast<UINT>(D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR);
    if (static_cast<UINT>(desc->Flags) & ~known_flags)
      return WARN_E_INVALIDARG(__func__);
    WARN("D3D12Device: shader cache sessions are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAGS kinds,
                     D3D12_SHADER_CACHE_CONTROL_FLAGS control) override {
    constexpr UINT known_kinds =
        static_cast<UINT>(
            D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_D3D_CACHE_FOR_DRIVER) |
        static_cast<UINT>(
            D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_D3D_CONVERSIONS) |
        static_cast<UINT>(
            D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_DRIVER_MANAGED) |
        static_cast<UINT>(D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED);
    constexpr UINT known_control =
        static_cast<UINT>(D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE) |
        static_cast<UINT>(D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE) |
        static_cast<UINT>(D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR);
    const UINT kind_bits = static_cast<UINT>(kinds);
    const UINT control_bits = static_cast<UINT>(control);
    if ((kind_bits & ~known_kinds) || !control_bits ||
        (control_bits & ~known_control) ||
        (control_bits & D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE &&
         control_bits & D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE))
      return WARN_E_INVALIDARG(__func__);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreateCommandQueue1(
      const D3D12_COMMAND_QUEUE_DESC *desc, REFIID creator_id, REFIID riid,
      void **command_queue) override {
    return CreateCommandQueue(desc, riid, command_queue);
  }
#endif

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  LUID *STDMETHODCALLTYPE GetAdapterLuid(LUID *__ret) override {
    *__ret = adapter_luid_;
    return __ret;
  }
#else
  LUID STDMETHODCALLTYPE GetAdapterLuid() override {
    return adapter_luid_;
  }
#endif

private:
  ID3D12Device *AsD3D12Device() {
    return static_cast<ID3D12Device *>(static_cast<DeviceComBase *>(this));
  }

  WMT::BinaryArchive *GetOrCreatePSOBinaryArchive() {
    if (!pso_binary_archive_enabled_)
      return nullptr;

    LogPSOBinaryArchiveMarker("DXMT_PSO_ARCHIVE: enter");

    std::lock_guard lock(pso_binary_archive_mutex_);
    if (pso_binary_archive_) {
      LogPSOBinaryArchiveMarker("DXMT_PSO_ARCHIVE: return ok=1");
      return static_cast<WMT::BinaryArchive *>(&pso_binary_archive_);
    }

    if (pso_binary_archive_unavailable_) {
      LogPSOBinaryArchiveMarker("DXMT_PSO_ARCHIVE: return ok=0");
      return nullptr;
    }

    WMT::Reference<WMT::Error> error;
    const char *archive_path = nullptr;
    try {
      if (!pso_binary_archive_unix_dir_.empty())
        std::filesystem::create_directories(pso_binary_archive_unix_dir_);
      if (FileExists(pso_binary_archive_unix_path_))
        archive_path = pso_binary_archive_unix_path_.c_str();
    } catch (const std::exception &e) {
      WARN("D3D12Device: failed to check DXMT PSO binary archive at ",
           pso_binary_archive_unix_path_, ": ", e.what());
    } catch (...) {
      WARN("D3D12Device: failed to check DXMT PSO binary archive at ",
           pso_binary_archive_unix_path_);
    }

    if (pso_binary_archive_unix_path_.empty()) {
      pso_binary_archive_unavailable_ = true;
      LogPSOBinaryArchiveMarker(
          "DXMT_PSO_ARCHIVE: create cold=1 ok=0 err=empty unix path");
      LogPSOBinaryArchiveMarker("DXMT_PSO_ARCHIVE: return ok=0");
      return nullptr;
    }

    pso_binary_archive_ = GetMTLDevice().newBinaryArchive(archive_path, error);
    const std::string archive_error =
        error ? error.description().getUTF8String() : std::string{};
    LogPSOBinaryArchiveMarker(
        "DXMT_PSO_ARCHIVE: create cold=%d ok=%d err=%s",
        archive_path ? 0 : 1, pso_binary_archive_ ? 1 : 0,
        archive_error.c_str());
    if (error || !pso_binary_archive_) {
      WARN("D3D12Device: failed to ",
           archive_path ? "load" : "create",
           " DXMT PSO binary archive at ", pso_binary_archive_unix_path_, ": ",
           error ? error.description().getUTF8String() : "unknown error");
      WritePSOBinaryArchiveUnavailableMarker(
          archive_path ? "load" : "create",
          error ? error.description().getUTF8String() : "newBinaryArchive returned null");
      pso_binary_archive_ = {};
      pso_binary_archive_unavailable_ = true;
      LogPSOBinaryArchiveMarker("DXMT_PSO_ARCHIVE: return ok=0");
      return nullptr;
    }

    LogPSOBinaryArchiveMarker("DXMT_PSO_ARCHIVE: return ok=1");
    return static_cast<WMT::BinaryArchive *>(&pso_binary_archive_);
  }

  bool SerializePSOBinaryArchive(const char *reason, uint64_t count) {
    if (!pso_binary_archive_enabled_)
      return false;

    if (ShouldInjectCreationFailure("DXMT_TEST_FAIL_PSO_ARCHIVE_WRITE_AT",
                                    g_test_pso_archive_write_occurrence)) {
      RecordInjectedFault("DXMT_TEST_FAIL_PSO_ARCHIVE_WRITE_AT");
      WARN("D3D12Device: injected PSO binary archive write failure");
      LogPSOBinaryArchiveMarker(
          "DXMT_PSO_ARCHIVE: serialize reason=%s count=%llu ok=0 err=injected",
          reason, static_cast<unsigned long long>(count));
      return false;
    }

    std::lock_guard lock(pso_binary_archive_mutex_);
    if (!pso_binary_archive_ || pso_binary_archive_unix_path_.empty())
      return false;

    try {
      if (!pso_binary_archive_unix_dir_.empty())
        std::filesystem::create_directories(pso_binary_archive_unix_dir_);
      WMT::Reference<WMT::Error> error;
      pso_binary_archive_.serialize(pso_binary_archive_unix_path_.c_str(),
                                    error);
      if (error) {
        WARN("D3D12Device: failed to serialize DXMT PSO binary archive to ",
             pso_binary_archive_unix_path_, ": ",
             error.description().getUTF8String());
        LogPSOBinaryArchiveMarker(
            "DXMT_PSO_ARCHIVE: serialize reason=%s count=%llu ok=0 path=%s err=%s",
            reason, static_cast<unsigned long long>(count),
            pso_binary_archive_unix_path_.c_str(),
            error.description().getUTF8String().c_str());
        return false;
      }

      INFO("D3D12Device: DXMT PSO binary archive serialized to ",
           pso_binary_archive_unix_path_);
      LogPSOBinaryArchiveMarker(
          "DXMT_PSO_ARCHIVE: serialize reason=%s count=%llu ok=1 path=%s err=",
          reason, static_cast<unsigned long long>(count),
          pso_binary_archive_unix_path_.c_str());
      return true;
    } catch (...) {
      WARN("D3D12Device: failed to serialize DXMT PSO binary archive to ",
           pso_binary_archive_unix_path_);
      LogPSOBinaryArchiveMarker(
          "DXMT_PSO_ARCHIVE: serialize reason=%s count=%llu ok=0 path=%s err=exception",
          reason, static_cast<unsigned long long>(count),
          pso_binary_archive_unix_path_.c_str());
      return false;
    }
  }

  void WritePSOBinaryArchiveUnavailableMarker(const char *operation,
                                             const std::string &error) {
    if (pso_binary_archive_unavailable_path_.empty())
      return;

    try {
      std::filesystem::create_directories(pso_binary_archive_unix_dir_);
    } catch (...) {
    }

    FILE *marker = fopen(pso_binary_archive_unavailable_path_.c_str(), "w");
    if (!marker)
      return;

    fprintf(marker, "DXMT_PSO_BINARY_ARCHIVE unavailable\n");
    fprintf(marker, "operation=%s\n", operation ? operation : "");
    fprintf(marker, "archive_path=%s\n", pso_binary_archive_unix_path_.c_str());
    fprintf(marker, "error=%s\n", error.c_str());
    fclose(marker);
  }

  bool IsResourceDescSupportedByDevice(
      const D3D12_RESOURCE_DESC &desc) const {
    if (!d3d12::IsSupportedResourceDesc(desc))
      return false;
    return desc.SampleDesc.Count == 1 ||
           device_->device().supportsTextureSampleCount(desc.SampleDesc.Count);
  }

  bool GetResourceSizeAndAlignment(const D3D12_RESOURCE_DESC &desc,
                                   UINT64 &size,
                                   UINT64 &alignment) const {
    if (!IsResourceDescSupportedByDevice(desc) ||
        (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
         IsBcFormat(device_->device(), desc.Format)))
      return false;

    alignment = desc.Alignment;
    if (!alignment) {
      alignment = desc.SampleDesc.Count > 1
                      ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                      : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    }

    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      size = desc.Width;
      return size != 0;
    }

    WMTSizeAndAlign metal_size_and_align = {};
    if (!d3d12::GetTextureHeapSizeAndAlign(
            device_->device(), desc, metal_size_and_align))
      return false;

    size = metal_size_and_align.size;
    alignment = std::max(alignment, metal_size_and_align.alignment);
    return size != 0 && alignment != 0;
  }

  D3D12_RESOURCE_ALLOCATION_INFO
  GetResourceAllocationInfoImpl(UINT visible_mask, UINT resource_desc_count,
                                const D3D12_RESOURCE_DESC *resource_descs) const {
    D3D12_RESOURCE_ALLOCATION_INFO info = {};
    info.Alignment = 1;

    if (!resource_desc_count)
      return info;
    if (visible_mask > 1 || !resource_descs) {
      info.SizeInBytes = UINT64_MAX;
      return info;
    }

    UINT64 offset = 0;
    for (UINT i = 0; i < resource_desc_count; i++) {
      const auto &desc = resource_descs[i];
      UINT64 size = 0;
      UINT64 alignment = 0;
      if (!GetResourceSizeAndAlignment(desc, size, alignment)) {
        info.SizeInBytes = UINT64_MAX;
        return info;
      }

      info.Alignment = std::max(info.Alignment, alignment);
      UINT64 resource_offset = 0;
      UINT64 aligned_size = 0;
      UINT64 next_offset = 0;
      if (!CheckedAlign(offset, alignment, resource_offset) ||
          !CheckedAlign(size, alignment, aligned_size) ||
          !CheckedAdd(resource_offset, aligned_size, next_offset)) {
        info.SizeInBytes = UINT64_MAX;
        return info;
      }
      offset = next_offset;
    }

    info.SizeInBytes = offset;
    return info;
  }

#ifdef __ID3D12Device8_INTERFACE_DEFINED__
  static D3D12_RESOURCE_DESC
  ToResourceDesc(const D3D12_RESOURCE_DESC1 *desc) {
    D3D12_RESOURCE_DESC downgraded = {};
    if (!desc)
      return downgraded;
    downgraded.Dimension = desc->Dimension;
    downgraded.Alignment = desc->Alignment;
    downgraded.Width = desc->Width;
    downgraded.Height = desc->Height;
    downgraded.DepthOrArraySize = desc->DepthOrArraySize;
    downgraded.MipLevels = desc->MipLevels;
    downgraded.Format = desc->Format;
    downgraded.SampleDesc = desc->SampleDesc;
    downgraded.Layout = desc->Layout;
    downgraded.Flags = desc->Flags;
    return downgraded;
  }

  D3D12_RESOURCE_ALLOCATION_INFO
  GetResourceAllocationInfo2Impl(
      UINT visible_mask, UINT resource_desc_count,
      const D3D12_RESOURCE_DESC1 *resource_descs,
      D3D12_RESOURCE_ALLOCATION_INFO1 *resource_info) const {
    if (!resource_desc_count || !resource_descs)
      return GetResourceAllocationInfo1Impl(visible_mask, resource_desc_count,
                                            nullptr, resource_info);

    std::vector<D3D12_RESOURCE_DESC> downgraded_descs;
    downgraded_descs.reserve(resource_desc_count);
    for (UINT i = 0; i < resource_desc_count; ++i)
      downgraded_descs.push_back(ToResourceDesc(&resource_descs[i]));

    return GetResourceAllocationInfo1Impl(
        visible_mask, resource_desc_count,
        downgraded_descs.empty() ? nullptr : downgraded_descs.data(),
        resource_info);
  }
#endif

#ifdef __ID3D12Device4_INTERFACE_DEFINED__
  D3D12_RESOURCE_ALLOCATION_INFO
  GetResourceAllocationInfo1Impl(UINT visible_mask, UINT resource_desc_count,
                                 const D3D12_RESOURCE_DESC *resource_descs,
                                 D3D12_RESOURCE_ALLOCATION_INFO1 *resource_info) const {
    D3D12_RESOURCE_ALLOCATION_INFO info = {};
    info.Alignment = 1;

    auto invalidate_resource_info = [&](UINT first, UINT64 alignment) {
      if (!resource_info)
        return;
      for (UINT i = first; i < resource_desc_count; ++i) {
        resource_info[i].Offset = UINT64_MAX;
        resource_info[i].Alignment = 0;
        resource_info[i].SizeInBytes = UINT64_MAX;
      }
      resource_info[first].Alignment = alignment;
    };

    if (!resource_desc_count)
      return info;
    if (visible_mask > 1 || !resource_descs) {
      info.SizeInBytes = UINT64_MAX;
      invalidate_resource_info(0, 0);
      return info;
    }

    UINT64 offset = 0;
    for (UINT i = 0; i < resource_desc_count; i++) {
      const auto &desc = resource_descs[i];
      UINT64 size = 0;
      UINT64 alignment = 0;
      if (!GetResourceSizeAndAlignment(desc, size, alignment)) {
        info.SizeInBytes = UINT64_MAX;
        invalidate_resource_info(i, alignment);
        return info;
      }

      info.Alignment = std::max(info.Alignment, alignment);
      UINT64 resource_offset = 0;
      UINT64 aligned_size = 0;
      UINT64 next_offset = 0;
      if (!CheckedAlign(offset, alignment, resource_offset) ||
          !CheckedAlign(size, alignment, aligned_size) ||
          !CheckedAdd(resource_offset, aligned_size, next_offset)) {
        info.SizeInBytes = UINT64_MAX;
        invalidate_resource_info(i, alignment);
        return info;
      }
      if (resource_info) {
        resource_info[i].Offset = resource_offset;
        resource_info[i].Alignment = alignment;
        resource_info[i].SizeInBytes = aligned_size;
      }

      offset = next_offset;
    }

    info.SizeInBytes = offset;
    return info;
  }
#endif

  D3D12_HEAP_PROPERTIES
  GetCustomHeapPropertiesImpl(UINT node_mask, D3D12_HEAP_TYPE heap_type) {
    D3D12_HEAP_PROPERTIES properties = {};
    const bool unified_memory = GetMTLDevice().hasUnifiedMemory();
    properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    properties.CreationNodeMask = node_mask ? node_mask : 1;
    properties.VisibleNodeMask = node_mask ? node_mask : 1;
    switch (heap_type) {
    case D3D12_HEAP_TYPE_DEFAULT:
      properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
      properties.MemoryPoolPreference =
          unified_memory ? D3D12_MEMORY_POOL_L0 : D3D12_MEMORY_POOL_L1;
      break;
    case D3D12_HEAP_TYPE_UPLOAD:
      properties.CPUPageProperty =
          unified_memory ? D3D12_CPU_PAGE_PROPERTY_WRITE_BACK
                         : D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
      properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
      break;
    case D3D12_HEAP_TYPE_READBACK:
      properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
      properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
      break;
    default:
      break;
    }
    return properties;
  }

  const char *GetInvalidHeapDescReason(const D3D12_HEAP_DESC *desc) const {
    if (!desc)
      return "null-desc";
    if (desc->SizeInBytes == 0)
      return "zero-size";
    if (desc->Alignment > D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)
      return "alignment-too-large";
    if (desc->Alignment &&
        desc->Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
        desc->Alignment != D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)
      return "unsupported-alignment";
    const UINT64 heap_alignment =
        desc->Alignment ? desc->Alignment
                        : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    if (desc->SizeInBytes > UINT64_MAX - (heap_alignment - 1))
      return "size-alignment-overflow";
    if (desc->Properties.CreationNodeMask > 1)
      return "creation-node-mask";
    if (desc->Properties.VisibleNodeMask > 1)
      return "visible-node-mask";
    if (desc->Properties.CreationNodeMask && desc->Properties.VisibleNodeMask &&
        (desc->Properties.CreationNodeMask & desc->Properties.VisibleNodeMask) == 0)
      return "disjoint-node-masks";
    if (desc->Properties.Type == D3D12_HEAP_TYPE_CUSTOM) {
      switch (desc->Properties.CPUPageProperty) {
      case D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE:
      case D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE:
      case D3D12_CPU_PAGE_PROPERTY_WRITE_BACK:
        break;
      default:
        return "custom-cpu-page-property";
      }
      switch (desc->Properties.MemoryPoolPreference) {
      case D3D12_MEMORY_POOL_UNKNOWN:
      case D3D12_MEMORY_POOL_L0:
      case D3D12_MEMORY_POOL_L1:
        break;
      default:
        return "custom-memory-pool";
      }
      if (desc->Flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY)
        return "custom-allow-display";
      if ((desc->Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) &&
          desc->Properties.CPUPageProperty !=
              D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE)
        return "custom-cross-adapter-cpu-visible";
      return nullptr;
    }
    if (desc->Properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN)
      return "typed-cpu-page-property";
    if (desc->Properties.MemoryPoolPreference != D3D12_MEMORY_POOL_UNKNOWN)
      return "typed-memory-pool";
    if (desc->Flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY)
      return "allow-display";
    if (desc->Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)
      return "shared-cross-adapter";
    switch (desc->Properties.Type) {
    case D3D12_HEAP_TYPE_DEFAULT:
    case D3D12_HEAP_TYPE_UPLOAD:
    case D3D12_HEAP_TYPE_READBACK:
      return nullptr;
    default:
      return "heap-type";
    }
  }

  bool IsValidHeapDesc(const D3D12_HEAP_DESC *desc) const {
    if (GetInvalidHeapDescReason(desc))
      return false;
    return true;
  }

  bool IsValidHeapProperties(const D3D12_HEAP_PROPERTIES *properties) const {
    if (!properties)
      return false;

    D3D12_HEAP_DESC heap_desc = {};
    heap_desc.SizeInBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_desc.Properties = *properties;
    return IsValidHeapDesc(&heap_desc);
  }

  bool IsValidInitialState(const D3D12_HEAP_PROPERTIES &properties,
                           D3D12_RESOURCE_STATES initial_state) const {
    if (properties.Type == D3D12_HEAP_TYPE_UPLOAD) {
      return initial_state == D3D12_RESOURCE_STATE_GENERIC_READ;
    }
    if (properties.Type == D3D12_HEAP_TYPE_READBACK) {
      return initial_state == D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if ((initial_state & D3D12_RESOURCE_STATE_RENDER_TARGET) &&
        initial_state != D3D12_RESOURCE_STATE_RENDER_TARGET)
      return false;
    if ((initial_state & D3D12_RESOURCE_STATE_DEPTH_WRITE) &&
        initial_state != D3D12_RESOURCE_STATE_DEPTH_WRITE)
      return false;
    return true;
  }

  bool IsValidResourceStateForDesc(const D3D12_RESOURCE_DESC &desc,
                                   D3D12_RESOURCE_STATES initial_state) const {
    if ((initial_state & D3D12_RESOURCE_STATE_RENDER_TARGET) &&
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
      return false;
    if ((initial_state & D3D12_RESOURCE_STATE_DEPTH_WRITE) &&
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
      return false;
    return true;
  }

  bool IsValidResourceAlignment(const D3D12_RESOURCE_DESC &desc) const {
    switch (desc.Alignment) {
    case 0:
    case D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT:
      return true;
    case D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT:
      return desc.SampleDesc.Count > 1;
    case D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT: {
      if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
          desc.SampleDesc.Count > 1 ||
          (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
        return false;
      return IsSmallResourceAlignmentEligible(desc);
    }
    default:
      return false;
    }
  }

  bool GetSmallResourceTileCount(const D3D12_RESOURCE_DESC &desc,
                                 UINT64 *tile_count) const {
    D3D12_TILE_SHAPE tile_shape = {};
    if (!GetSmallResource4KTileShape(device_->device(), desc, tile_shape))
      return false;

    if (!tile_shape.WidthInTexels || !tile_shape.HeightInTexels ||
        !tile_shape.DepthInTexels)
      return false;
    if (!desc.Width || !desc.Height || !desc.DepthOrArraySize)
      return false;

    const UINT64 width_tiles =
        (desc.Width + tile_shape.WidthInTexels - 1) /
        tile_shape.WidthInTexels;
    const UINT64 height_tiles =
        (UINT64(desc.Height) + tile_shape.HeightInTexels - 1) /
        tile_shape.HeightInTexels;
    const UINT64 depth =
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
            ? desc.DepthOrArraySize
            : 1;
    const UINT64 depth_tiles =
        (depth + tile_shape.DepthInTexels - 1) / tile_shape.DepthInTexels;

    if (!width_tiles || !height_tiles || !depth_tiles)
      return false;
    if (width_tiles > ~UINT64(0) / height_tiles)
      return false;
    const UINT64 xy_tiles = width_tiles * height_tiles;
    if (xy_tiles > ~UINT64(0) / depth_tiles)
      return false;

    if (tile_count)
      *tile_count = xy_tiles * depth_tiles;
    return true;
  }

  bool IsSmallResourceAlignmentEligible(const D3D12_RESOURCE_DESC &desc) const {
    UINT64 tile_count = 0;
    return GetSmallResourceTileCount(desc, &tile_count) &&
           tile_count <= D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT /
                             D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
  }

  bool IsValidCommittedResourceDesc(
      const D3D12_HEAP_PROPERTIES *heap_properties,
      D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc,
      D3D12_RESOURCE_STATES initial_state) const {
    if (!IsValidHeapProperties(heap_properties) || !desc)
      return false;
    if (!IsResourceDescSupportedByDevice(*desc))
      return false;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
        IsBcFormat(device_->device(), desc->Format))
      return false;
    if (!IsValidInitialState(*heap_properties, initial_state))
      return false;
    if (!IsValidResourceStateForDesc(*desc, initial_state))
      return false;
    if (!IsValidResourceAlignment(*desc))
      return false;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))
      return false;

    if (!IsValidCrossAdapterResourceDesc(*heap_properties, heap_flags, *desc))
      return false;
    if (HasInvalidCpuVisibleBufferFlags(*desc, *heap_properties))
      return false;
    if (IsAbstractedCpuVisibleHeap(*heap_properties)) {
      // UPLOAD and READBACK are abstracted buffer-only heap types. CPU-visible
      // custom heaps have separate UMA-dependent rules and are intentionally
      // not covered by this check.
      if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        return false;
      if (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                         D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))
        return false;
    }

    return true;
  }

  bool IsValidReservedResourceDesc(const D3D12_RESOURCE_DESC *desc,
                                   D3D12_RESOURCE_STATES initial_state) const {
    if (!desc)
      return false;
    if (!IsResourceDescSupportedByDevice(*desc))
      return false;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
        IsBcFormat(device_->device(), desc->Format))
      return false;

    const auto heap_properties = GetDefaultResourceHeapProperties();
    if (!IsValidInitialState(heap_properties, initial_state))
      return false;
    if (!IsValidResourceStateForDesc(*desc, initial_state))
      return false;
    if (!IsValidResourceAlignment(*desc))
      return false;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))
      return false;
    if (!IsValidCrossAdapterResourceDesc(heap_properties, D3D12_HEAP_FLAG_NONE,
                                        *desc))
      return false;

    return true;
  }

  D3D12_HEAP_PROPERTIES GetDefaultResourceHeapProperties() const {
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
  }

  bool IsValidPlacedResourceDesc(const d3d12::Heap &heap, UINT64 heap_offset,
                                 const D3D12_RESOURCE_DESC *desc,
                                 D3D12_RESOURCE_STATES initial_state) const {
    return !GetInvalidPlacedResourceDescReason(heap, heap_offset, desc,
                                               initial_state);
  }

  const char *GetInvalidPlacedResourceDescReason(
      const d3d12::Heap &heap, UINT64 heap_offset,
      const D3D12_RESOURCE_DESC *desc,
      D3D12_RESOURCE_STATES initial_state) const {
    if (!desc)
      return "null-desc";
    if (!IsResourceDescSupportedByDevice(*desc))
      return "unsupported-desc";
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
        IsBcFormat(device_->device(), desc->Format))
      return "bc-texture1d";
    if (!IsValidInitialState(heap.GetHeapDesc().Properties, initial_state))
      return "initial-state";
    if (!IsValidResourceStateForDesc(*desc, initial_state))
      return "state-for-desc";
    if (!IsValidResourceAlignment(*desc))
      return "resource-alignment";
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))
      return "buffer-simultaneous-access";

    const auto allocation_info = GetResourceAllocationInfoImpl(1, 1, desc);
    if (allocation_info.SizeInBytes == 0 ||
        allocation_info.SizeInBytes == UINT64_MAX)
      return "invalid-allocation-size";
    const UINT64 placement_alignment = allocation_info.Alignment;
    if (heap_offset % placement_alignment)
      return "heap-offset-alignment";
    const auto &heap_desc = heap.GetHeapDesc();
    const UINT64 heap_size = heap_desc.SizeInBytes;
    if (heap_offset > heap_size ||
        allocation_info.SizeInBytes > heap_size - heap_offset)
      return "heap-range";

    if (!IsValidCrossAdapterResourceDesc(heap.GetHeapDesc().Properties,
                                        heap.GetHeapDesc().Flags, *desc))
      return "cross-adapter-resource";
    if (HasInvalidCpuVisibleBufferFlags(*desc, heap.GetHeapDesc().Properties))
      return "cpu-visible-buffer-flags";
    if (IsAbstractedCpuVisibleHeap(heap.GetHeapDesc().Properties) &&
        desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
      return "abstracted-cpu-visible-texture";

    const auto heap_flags = heap.GetHeapDesc().Flags;
    if ((heap_flags & D3D12_HEAP_FLAG_DENY_BUFFERS) &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
      return "heap-deny-buffer";
    if ((heap_flags & D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES) &&
        (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
      return "heap-deny-rt-ds";
    if ((heap_flags & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES) &&
        desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        !(desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
      return "heap-deny-non-rt-ds";

    return nullptr;
  }

  void GetCopyableFootprintsImpl(
      const D3D12_RESOURCE_DESC *desc, UINT first_sub_resource,
      UINT sub_resource_count, UINT64 base_offset,
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *row_count,
      UINT64 *row_size, UINT64 *total_bytes,
      bool strict_resource_desc = true) const {
    UINT64 offset = 0;
    UINT64 total = 0;

    auto set_invalid = [&]() {
      for (UINT i = 0; i < sub_resource_count; i++) {
        if (layouts) {
          layouts[i].Offset = UINT64_MAX;
          layouts[i].Footprint.Format = static_cast<DXGI_FORMAT>(~0u);
          layouts[i].Footprint.Width = ~0u;
          layouts[i].Footprint.Height = ~0u;
          layouts[i].Footprint.Depth = ~0u;
          layouts[i].Footprint.RowPitch = ~0u;
        }
        if (row_count)
          row_count[i] = ~0u;
        if (row_size)
          row_size[i] = UINT64_MAX;
      }
      if (total_bytes)
        *total_bytes = UINT64_MAX;
    };

    if (!desc || sub_resource_count == 0) {
      if (total_bytes)
        *total_bytes = 0;
      return;
    }

    if (!(strict_resource_desc ? IsResourceDescSupportedByDevice(*desc)
                               : IsValidCopyableFootprintDesc(*desc))) {
      set_invalid();
      return;
    }

    DXGIFormatPlaneFootprintLayout default_format = {};
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        !GetDXGIFormatPlaneFootprintLayout(desc->Format, 0, default_format)) {
      WARN("D3D12Device: TODO GetCopyableFootprints unsupported format"
           " format=", uint32_t(desc->Format),
           " dimension=", uint32_t(desc->Dimension),
           " width=", uint64_t(desc->Width),
           " height=", uint32_t(desc->Height),
           " depthOrArraySize=", uint32_t(desc->DepthOrArraySize),
           " mipLevels=", uint32_t(desc->MipLevels),
           " sampleCount=", uint32_t(desc->SampleDesc.Count),
           " layout=", uint32_t(desc->Layout),
           " flags=", uint32_t(desc->Flags));
      set_invalid();
      return;
    }

    const UINT mip_levels = desc->MipLevels ? desc->MipLevels : 1;
    const UINT array_size =
        desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
            ? 1
            : desc->DepthOrArraySize;
    const UINT plane_count =
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
            ? 1
            : GetD3D12FormatPlaneCount(desc->Format);
    const UINT subresources_per_plane = mip_levels * array_size;
    const UINT max_subresources = subresources_per_plane * plane_count;
    if (first_sub_resource >= max_subresources ||
        sub_resource_count > max_subresources - first_sub_resource) {
      set_invalid();
      return;
    }
    for (UINT i = 0; i < sub_resource_count; i++) {
      const UINT subresource = first_sub_resource + i;
      const UINT plane =
          subresources_per_plane ? subresource / subresources_per_plane : 0;
      const UINT plane_subresource =
          subresources_per_plane ? subresource % subresources_per_plane
                                 : subresource;
      D3D12_SUBRESOURCE_FOOTPRINT footprint = {};
      UINT rows = 1;
      UINT64 unpadded_row_size = desc->Width;
      UINT64 subresource_size = desc->Width;

      footprint.Format = GetD3D12FootprintFormat(desc->Format, plane);

      if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        footprint.Width = static_cast<UINT>(desc->Width);
        footprint.Height = 1;
        footprint.Depth = 1;
        footprint.RowPitch =
            static_cast<UINT>(Align(desc->Width, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
        // subresource_size already holds desc->Width from its initializer; a
        // buffer subresource occupies exactly Width bytes (the 256-byte pitch
        // alignment applies to the placed footprint, not to the byte count).
      } else {
        if (plane >= plane_count) {
          WARN("D3D12Device: GetCopyableFootprints subresource plane out of range subresource=",
               subresource, " plane=", plane, " plane_count=", plane_count);
          break;
        }

        const UINT mip_slice = SubresourceMipSlice(plane_subresource, mip_levels);
        UINT subsample_x_log2 = 0;
        UINT subsample_y_log2 = 0;
        GetD3D12FormatSubsampleLog2(desc->Format, plane, subsample_x_log2,
                                    subsample_y_log2);
        const UINT64 width =
            Align(MipSize(desc->Width, mip_slice + subsample_x_log2),
                  GetD3D12FormatBlockWidth(default_format));
        const UINT height = static_cast<UINT>(
            Align(MipSize(desc->Height, mip_slice + subsample_y_log2),
                  GetD3D12FormatBlockHeight(default_format)));
        const UINT depth = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                               ? static_cast<UINT>(MipSize(desc->DepthOrArraySize, mip_slice))
                               : 1;
        DXGIFormatPlaneFootprintLayout plane_format = {};
        if (!GetDXGIFormatPlaneFootprintLayout(desc->Format, plane,
                                               plane_format)) {
          WARN("D3D12Device: TODO GetCopyableFootprints unsupported plane"
               " format=", uint32_t(desc->Format),
               " plane=", uint32_t(plane),
               " subresource=", uint32_t(subresource));
          set_invalid();
          return;
        }

        const UINT block_width = GetD3D12FormatBlockWidth(plane_format);
        const UINT block_height = GetD3D12FormatBlockHeight(plane_format);
        const UINT element_size =
            GetD3D12FormatPlaneElementSize(desc->Format, plane, plane_format);
        const UINT num_planes = std::max(1u, plane_count);

        footprint.Width = static_cast<UINT>(width);
        footprint.Height = height;
        footprint.Depth = depth;

        rows = std::max(1u, height / block_height);
        unpadded_row_size = (width / block_width) * element_size;

        UINT64 row_alignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        if (num_planes == 2 || desc->Format == DXGI_FORMAT_420_OPAQUE)
          row_alignment *= 2;
        footprint.RowPitch = static_cast<UINT>(
            Align(unpadded_row_size, row_alignment));
        const UINT64 row_span =
            rows > 1 ? (rows - 1) * UINT64(footprint.RowPitch) + unpadded_row_size
                     : unpadded_row_size;
        const UINT64 slice_pitch =
            Align(row_span, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * num_planes);
        subresource_size =
            depth > 1 ? (depth - 1) * slice_pitch + row_span : row_span;
      }

      if ((layouts && base_offset > UINT64_MAX - offset) ||
          subresource_size > UINT64_MAX - offset) {
        set_invalid();
        return;
      }
      if (layouts) {
        layouts[i].Offset = base_offset + offset;
        layouts[i].Footprint = footprint;
      }
      if (row_count)
        row_count[i] = rows;
      if (row_size)
        row_size[i] = unpadded_row_size;

      total = offset + subresource_size;
      if (i + 1 < sub_resource_count) {
        if (total >
            UINT64_MAX - (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1)) {
          set_invalid();
          return;
        }
        offset = Align(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      }
    }

    if (total_bytes)
      *total_bytes = total;
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_DEVICE_CONFIGURATION_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DEVICE_CONFIGURATION_DESC *__ret) override {
    *__ret = GetDeviceConfigurationDesc();
    return __ret;
  }
#else
  D3D12_DEVICE_CONFIGURATION_DESC STDMETHODCALLTYPE GetDesc() override {
    return GetDeviceConfigurationDesc();
  }
#endif

  HRESULT STDMETHODCALLTYPE GetEnabledExperimentalFeatures(GUID *guids,
                                                           UINT guid_count) override {
    if (guid_count && !guids)
      return WARN_E_INVALIDARG(__func__);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SerializeVersionedRootSignature(
      const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob,
      ID3DBlob **error_blob) override {
    return d3d12::SerializeVersionedRootSignature(desc, blob, error_blob);
  }

  HRESULT STDMETHODCALLTYPE CreateVersionedRootSignatureDeserializer(
      const void *blob, SIZE_T size, REFIID riid, void **deserializer) override {
    if (!blob && size)
      return WARN_E_INVALIDARG(__func__);
    return d3d12::CreateRootSignatureDeserializer(
        std::span<const std::byte>(static_cast<const std::byte *>(blob), size),
        riid, deserializer);
  }

  HRESULT STDMETHODCALLTYPE
  CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary(
      const void *library_blob, SIZE_T size, LPCWSTR subobject_name,
      REFIID riid, void **deserializer) override {
    if (!library_blob && size)
      return WARN_E_INVALIDARG(__func__);
    return d3d12::CreateRootSignatureDeserializerFromSubobjectInLibrary(
        std::span<const std::byte>(
            static_cast<const std::byte *>(library_blob), size),
        subobject_name, riid, deserializer);
  }

  static D3D12_DEVICE_CONFIGURATION_DESC GetDeviceConfigurationDesc() {
    D3D12_DEVICE_CONFIGURATION_DESC desc = {};
    desc.Flags = D3D12_DEVICE_FLAG_NONE;
    desc.SDKVersion = kAgilitySdkVersion;
    return desc;
  }

  Com<IMTLDXGIAdapter> adapter_;
  std::unique_ptr<dxmt::Device> device_;
  std::shared_ptr<PipelineNativeArtifactCache>
      pipeline_native_artifact_cache_;
#ifdef __ID3D12DeviceRemovedExtendedData2_INTERFACE_DEFINED__
  DredDataImpl dred_;
#endif
  ComPrivateData private_data_;
  WMT::Reference<WMT::SharedEvent> enqueue_set_event_signal_;
  WMT::Reference<WMT::SharedEvent> multiple_fence_wait_signal_;
  D3DKMT_HANDLE local_kmt_ = 0;
  LUID adapter_luid_ = {};
  UINT maximum_frame_latency_ = 3;
  INT gpu_thread_priority_ = 0;
  uint64_t enqueue_set_event_value_ = 0;
  uint64_t multiple_fence_wait_value_ = 0;
  WMT::Reference<WMT::BinaryArchive> pso_binary_archive_;
  std::mutex pso_binary_archive_mutex_;
  std::atomic<uint64_t> pso_binary_archive_successful_creates_ = 0;
  uint64_t pso_binary_archive_serialize_every_ = 200;
  bool pso_binary_archive_enabled_ = false;
  bool pso_binary_archive_unavailable_ = false;
  std::string pso_binary_archive_unix_dir_;
  std::string pso_binary_archive_unix_path_;
  std::string pso_binary_archive_unavailable_path_;
  std::string name_;
};

} // namespace

Com<IMTLD3D12Device>
CreateD3D12Device(std::unique_ptr<dxmt::Device> &&device, IMTLDXGIAdapter *adapter) {
  return Com<IMTLD3D12Device>::transfer(new DeviceImpl(std::move(device), adapter));
}

} // namespace dxmt::d3d12
