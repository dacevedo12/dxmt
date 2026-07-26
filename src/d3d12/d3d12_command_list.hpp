#pragma once

#include "d3d12_command_allocator.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_interfaces.hpp"
#include "d3d12_pipeline.hpp"
#include "com/com_pointer.hpp"
#include "dxmt_d3d12_test_path.hpp"
#include "dxmt_statistics.hpp"
#include <d3d12.h>
#include <array>
#include <atomic>
#include <bit>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace dxmt::d3d12 {

struct StoredTextureCopyLocation {
  Com<ID3D12Resource> resource;
  D3D12_TEXTURE_COPY_TYPE type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed_footprint = {};
  UINT subresource_index = 0;
};

struct StoredResourceBarrier {
  D3D12_RESOURCE_BARRIER barrier = {};
  Com<ID3D12Resource> resource;
  Com<ID3D12Resource> resource_before;
  Com<ID3D12Resource> resource_after;
};

struct DrawInstancedRecord {
  UINT vertex_count_per_instance = 0;
  UINT instance_count = 0;
  UINT start_vertex_location = 0;
  UINT start_instance_location = 0;
};

struct DrawIndexedInstancedRecord {
  UINT index_count_per_instance = 0;
  UINT instance_count = 0;
  UINT start_index_location = 0;
  INT base_vertex_location = 0;
  UINT start_instance_location = 0;
};

struct DispatchRecord {
  UINT x = 0;
  UINT y = 0;
  UINT z = 0;
};

struct PipelineStateRecord {
  Com<ID3D12PipelineState> pipeline_state;
};

struct ClearStateRecord {
  Com<ID3D12PipelineState> pipeline_state;
};

struct CopyBufferRegionRecord {
  Com<ID3D12Resource> dst;
  UINT64 dst_offset = 0;
  Com<ID3D12Resource> src;
  UINT64 src_offset = 0;
  UINT64 byte_count = 0;
};

struct CopyTextureRegionRecord {
  StoredTextureCopyLocation dst;
  UINT dst_x = 0;
  UINT dst_y = 0;
  UINT dst_z = 0;
  StoredTextureCopyLocation src;
  std::optional<D3D12_BOX> src_box;
};

struct CopyResourceRecord {
  Com<ID3D12Resource> dst;
  Com<ID3D12Resource> src;
};

struct CopyTilesRecord {
  Com<ID3D12Resource> tiled_resource;
  D3D12_TILED_RESOURCE_COORDINATE start = {};
  D3D12_TILE_REGION_SIZE size = {};
  Com<ID3D12Resource> buffer;
  UINT64 buffer_offset = 0;
  D3D12_TILE_COPY_FLAGS flags = D3D12_TILE_COPY_FLAG_NONE;
};

struct ResolveSubresourceRecord {
  Com<ID3D12Resource> dst;
  UINT dst_subresource = 0;
  UINT dst_x = 0;
  UINT dst_y = 0;
  Com<ID3D12Resource> src;
  UINT src_subresource = 0;
  std::optional<D3D12_RECT> src_rect;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  D3D12_RESOLVE_MODE mode = D3D12_RESOLVE_MODE_AVERAGE;
};

struct PendingRenderPassResolve {
  Com<ID3D12Resource> src;
  Com<ID3D12Resource> dst;
  UINT src_subresource = 0;
  UINT dst_subresource = 0;
  UINT dst_x = 0;
  UINT dst_y = 0;
  std::optional<D3D12_RECT> src_rect;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  D3D12_RESOLVE_MODE mode = D3D12_RESOLVE_MODE_AVERAGE;
};

struct WriteBufferImmediateOperation {
  Com<ID3D12Resource> resource;
  UINT64 offset = 0;
  UINT value = 0;
  D3D12_WRITEBUFFERIMMEDIATE_MODE mode =
      D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
};

struct WriteBufferImmediateRecord {
  std::vector<WriteBufferImmediateOperation> operations;
};

struct ResourceBarrierRecord {
  std::vector<StoredResourceBarrier> barriers;
};

struct ClearRenderTargetRecord {
  DescriptorRecord descriptor;
  std::array<FLOAT, 4> color = {};
  std::vector<D3D12_RECT> rects;
};

struct ClearDepthStencilRecord {
  DescriptorRecord descriptor;
  D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
  FLOAT depth = 1.0f;
  UINT8 stencil = 0;
  std::vector<D3D12_RECT> rects;
};

struct ClearUnorderedAccessRecord {
  DescriptorRecord descriptor;
  Com<ID3D12Resource> resource;
  std::array<UINT, 4> uint_values = {};
  std::array<FLOAT, 4> float_values = {};
  bool integer = false;
  std::vector<D3D12_RECT> rects;
};

struct DiscardResourceRecord {
  Com<ID3D12Resource> resource;
  std::vector<D3D12_RECT> rects;
  UINT first_subresource = 0;
  UINT subresource_count = 0;
};

struct ViewportRecord {
  std::vector<D3D12_VIEWPORT> viewports;
};

struct ScissorRecord {
  std::vector<D3D12_RECT> rects;
};

struct BlendFactorRecord {
  std::array<FLOAT, 4> blend_factor = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct StencilRefRecord {
  UINT stencil_ref = 0;
};

struct PrimitiveTopologyRecord {
  D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
};

struct RenderTargetsRecord {
  std::vector<DescriptorRecord> render_targets;
  std::optional<DescriptorRecord> depth_stencil;
};

struct VertexBuffersRecord {
  UINT start_slot = 0;
  UINT view_count = 0;
  std::vector<D3D12_VERTEX_BUFFER_VIEW> views;
};

struct IndexBufferRecord {
  std::optional<D3D12_INDEX_BUFFER_VIEW> view;
};

struct RootSignatureRecord {
  bool compute = false;
  Com<ID3D12RootSignature> root_signature;
};

struct DescriptorHeapsRecord {
  std::vector<Com<ID3D12DescriptorHeap>> heaps;
};

struct RootDescriptorTableRecord {
  bool compute = false;
  UINT root_parameter_index = 0;
  D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor = {};
};

struct RootConstantsRecord {
  bool compute = false;
  UINT root_parameter_index = 0;
  UINT dst_offset = 0;
  std::vector<UINT> values;
};

struct RootDescriptorRecord {
  bool compute = false;
  D3D12_ROOT_PARAMETER_TYPE parameter_type = D3D12_ROOT_PARAMETER_TYPE_CBV;
  UINT root_parameter_index = 0;
  D3D12_GPU_VIRTUAL_ADDRESS address = 0;
};

struct BeginQueryRecord {
  Com<ID3D12QueryHeap> heap;
  D3D12_QUERY_TYPE type = D3D12_QUERY_TYPE_OCCLUSION;
  UINT index = 0;
};

struct EndQueryRecord {
  Com<ID3D12QueryHeap> heap;
  D3D12_QUERY_TYPE type = D3D12_QUERY_TYPE_OCCLUSION;
  UINT index = 0;
};

struct ResolveQueryDataRecord {
  uintptr_t command_list_identity = 0;
  Com<ID3D12QueryHeap> heap;
  D3D12_QUERY_TYPE type = D3D12_QUERY_TYPE_OCCLUSION;
  UINT start_index = 0;
  UINT query_count = 0;
  Com<ID3D12Resource> dst_buffer;
  UINT64 dst_buffer_offset = 0;
};

struct PredicationRecord {
  Com<ID3D12Resource> buffer;
  UINT64 buffer_offset = 0;
  D3D12_PREDICATION_OP operation = D3D12_PREDICATION_OP_EQUAL_ZERO;
};

struct ExecuteIndirectRecord {
  Com<ID3D12CommandSignature> command_signature;
  UINT max_command_count = 0;
  Com<ID3D12Resource> arg_buffer;
  UINT64 arg_buffer_offset = 0;
  Com<ID3D12Resource> count_buffer;
  UINT64 count_buffer_offset = 0;
};

struct TemporalUpscaleRecord {
  UINT input_content_width = 0;
  UINT input_content_height = 0;
  UINT motion_vector_width = 0;
  UINT motion_vector_height = 0;
  BOOL auto_exposure = FALSE;
  BOOL in_reset = FALSE;
  BOOL depth_reversed = FALSE;
  BOOL motion_vector_in_display_res = FALSE;
  Com<ID3D12Resource> color;
  Com<ID3D12Resource> depth;
  Com<ID3D12Resource> motion_vector;
  Com<ID3D12Resource> output;
  FLOAT motion_vector_scale_x = 1.0f;
  FLOAT motion_vector_scale_y = 1.0f;
  FLOAT pre_exposure = 0.0f;
  Com<ID3D12Resource> exposure_texture;
  FLOAT jitter_offset_x = 0.0f;
  FLOAT jitter_offset_y = 0.0f;
};

using CommandRecordPayload = std::variant<
    DrawInstancedRecord, DrawIndexedInstancedRecord, DispatchRecord,
    PipelineStateRecord, ClearStateRecord, CopyBufferRegionRecord,
    CopyTextureRegionRecord, CopyResourceRecord, CopyTilesRecord,
    ResolveSubresourceRecord,
    ResourceBarrierRecord, ClearRenderTargetRecord, ClearDepthStencilRecord,
    ClearUnorderedAccessRecord, DiscardResourceRecord, ViewportRecord,
    ScissorRecord, BlendFactorRecord, StencilRefRecord, PrimitiveTopologyRecord,
    RenderTargetsRecord, VertexBuffersRecord, IndexBufferRecord,
    RootSignatureRecord, DescriptorHeapsRecord, RootDescriptorTableRecord,
    RootConstantsRecord, RootDescriptorRecord, BeginQueryRecord,
    EndQueryRecord, ResolveQueryDataRecord, PredicationRecord,
    WriteBufferImmediateRecord,
    ExecuteIndirectRecord, TemporalUpscaleRecord>;

enum class CommandRecordCompileKind : std::uint8_t {
  Other,
  Graphics,
  Compute,
  Barrier,
};

struct CommandRecord {
  std::uint64_t d3d_sequence = 0;
  CommandRecordCompileKind compile_kind = CommandRecordCompileKind::Other;
  CommandRecordPayload payload;
};

enum class CompiledCommandSegmentKind {
  Graphics,
  Compute,
  Indirect,
  Barrier,
  Transfer,
  Control,
  ElidedState,
  Typed,
  Fallback,
};

enum class CompiledCommandFallbackReason {
  None,
  ConservativeCompiler,
  LegacyPipelineState,
  NonBindlessPipelineState,
  GeometryPipeline,
  TessellationPipeline,
  MissingPipelineState,
  MissingRootSignature,
  ExecuteIndirect,
  UnsupportedBarrier,
  CopyOrResolve,
  ClearOrDiscard,
  QueryOrPredication,
  SnapshotDependent,
  TemporalUpscale,
  NativeUnsupportedRootSignature,
  NativeUnsupportedDescriptorRange,
  NativeDescriptorNullBase,
  NativeDescriptorMixedHeap,
  NativeDescriptorNoRange,
  NativeDescriptorInvalidHandle,
  NativeDescriptorHeapTail,
  NativeDescriptorAmbiguousRange,
  NativeDescriptorBackendGeneration,
  NativeUnsupportedRootDescriptor,
  NativeUnsupportedGeometryPipeline,
  NativeUnsupportedTessellationPipeline,
  NativeUnsupportedExecuteIndirect,
  NativeUnsupportedDynamicResource,
  NativeMissingDescriptorBackend,
  NativeShaderAbiMismatch,
  NativeResidencyUnsupported,
  InjectedNativePacketAllocationFailure,
  InjectedNativeSegmentFinalizationFailure,
  InjectedNativePipelineCompilationFailure,
  UnsupportedRootSignature,
  UnsupportedDescriptorTable,
  UnsupportedRootDescriptor,
  UnsupportedRootConstants,
  UnsupportedVertexIndexState,
  UnsupportedRenderTargetState,
  UnsupportedArgumentTable,
  UnsupportedCommand,
};

template <typename T>
class CompiledImmutableVector {
public:
  using Storage = std::vector<T>;
  using value_type = T;
  using size_type = typename Storage::size_type;
  using iterator = typename Storage::iterator;
  using const_iterator = typename Storage::const_iterator;

  CompiledImmutableVector() = default;
  CompiledImmutableVector(const Storage &values)
      : storage_(values.empty() ? nullptr : Acquire(values.size())) {
    if (storage_)
      storage_->values.assign(values.begin(), values.end());
  }
  CompiledImmutableVector(Storage &&values)
      : storage_(values.empty() ? nullptr : Acquire(0)) {
    if (storage_)
      storage_->values = std::move(values);
  }

  CompiledImmutableVector(const CompiledImmutableVector &other)
      : storage_(other.storage_) {
    Retain(storage_);
  }

  CompiledImmutableVector(CompiledImmutableVector &&other) noexcept
      : storage_(std::exchange(other.storage_, nullptr)) {}

  ~CompiledImmutableVector() { Release(storage_); }

  CompiledImmutableVector &operator=(const CompiledImmutableVector &other) {
    if (this == &other)
      return *this;
    Retain(other.storage_);
    Release(storage_);
    storage_ = other.storage_;
    return *this;
  }

  CompiledImmutableVector &operator=(
      CompiledImmutableVector &&other) noexcept {
    if (this == &other)
      return *this;
    Release(storage_);
    storage_ = std::exchange(other.storage_, nullptr);
    return *this;
  }

  CompiledImmutableVector &operator=(const Storage &values) {
    Release(storage_);
    if (values.empty()) {
      storage_ = nullptr;
    } else {
      storage_ = Acquire(values.size());
      storage_->values.assign(values.begin(), values.end());
    }
    return *this;
  }

  CompiledImmutableVector &operator=(Storage &&values) {
    Release(storage_);
    if (values.empty()) {
      storage_ = nullptr;
    } else {
      storage_ = Acquire(0);
      storage_->values = std::move(values);
    }
    return *this;
  }

  const Storage &view() const {
    static const Storage empty;
    return storage_ ? storage_->values : empty;
  }

  Storage copy() const {
    return view();
  }

  Storage &mutableView() {
    if (!storage_) {
      storage_ = Acquire(0);
    } else if (storage_->references.load(std::memory_order_acquire) != 1) {
      auto *replacement = Acquire(storage_->values.size());
      replacement->values.assign(storage_->values.begin(),
                                 storage_->values.end());
      Release(storage_);
      storage_ = replacement;
    }
    return storage_->values;
  }

  operator const Storage &() const { return view(); }
  operator Storage &() { return mutableView(); }

  bool empty() const { return !storage_ || storage_->values.empty(); }
  size_type size() const { return storage_ ? storage_->values.size() : 0; }
  size_type capacity() const {
    return storage_ ? storage_->values.capacity() : 0;
  }
  const T *data() const {
    return storage_ ? storage_->values.data() : nullptr;
  }
  const void *identity() const { return storage_; }
  bool sharesStorageWith(const CompiledImmutableVector &other) const {
    return storage_ == other.storage_;
  }
  std::span<const T> span() const { return {data(), size()}; }

  const T &operator[](size_type index) const { return view()[index]; }
  T &operator[](size_type index) { return mutableView()[index]; }
  const T &back() const { return view().back(); }
  T &back() { return mutableView().back(); }

  const_iterator begin() const { return view().begin(); }
  const_iterator end() const { return view().end(); }
  iterator begin() { return mutableView().begin(); }
  iterator end() { return mutableView().end(); }

  void clear() {
    if (empty())
      return;
    Release(storage_);
    storage_ = nullptr;
  }
  void reserve(size_type count) {
    if (!storage_) {
      storage_ = Acquire(count);
      return;
    }
    auto &values = mutableView();
    if (values.capacity() < count) {
      RecordAllocationEvent();
      values.reserve(count);
    }
  }
  void push_back(const T &value) {
    auto &values = mutableView();
    if (values.size() == values.capacity())
      RecordAllocationEvent();
    values.push_back(value);
  }
  void push_back(T &&value) {
    auto &values = mutableView();
    if (values.size() == values.capacity())
      RecordAllocationEvent();
    values.push_back(std::move(value));
  }

  iterator insert(iterator position, const T &value) {
    auto &values = mutableView();
    if (values.size() == values.capacity())
      RecordAllocationEvent();
    return values.insert(position, value);
  }

  template <typename InputIt>
  iterator insert(iterator position, InputIt first, InputIt last) {
    auto &values = mutableView();
    const auto additional = static_cast<size_type>(std::distance(first, last));
    if (values.capacity() - values.size() < additional)
      RecordAllocationEvent();
    return values.insert(position, first, last);
  }

  void resize(size_type count, const T &value = T()) {
    auto &values = mutableView();
    if (values.capacity() < count)
      RecordAllocationEvent();
    values.resize(count, value);
  }

  template <typename... Args>
  T &emplace_back(Args &&...args) {
    auto &values = mutableView();
    if (values.size() == values.capacity())
      RecordAllocationEvent();
    return values.emplace_back(std::forward<Args>(args)...);
  }

  static std::uint64_t ThreadAllocationEventCount() {
    return thread_allocation_events_;
  }

private:
  struct Block;

  struct Pool {
    std::array<Block *, sizeof(size_type) * 8 + 1> available = {};
    std::mutex returned_mutex;
    Block *returned = nullptr;
  };

  struct Block {
    std::atomic<std::uint32_t> references = 1;
    Storage values;
    Pool *owner = nullptr;
    Block *next = nullptr;
  };

  static Pool &GetPool() {
    // A pool is owned by the recording thread. Blocks may be released by a
    // queue worker after GPU completion, so the pool intentionally has process
    // lifetime and accepts returned blocks through a small synchronized list.
    // Only its owner thread consumes `available`; Close never takes a global
    // compiler lock.
    if (!thread_pool_)
      thread_pool_ = new Pool();
    return *thread_pool_;
  }

  static void RecordAllocationEvent() { thread_allocation_events_++; }

  static size_type CapacityBucket(size_type capacity) {
    return capacity <= 1 ? 0 : std::bit_width(capacity - 1);
  }

  static Block *Acquire(size_type required_capacity) {
    auto &pool = GetPool();
    Block *returned = nullptr;
    {
      std::lock_guard lock(pool.returned_mutex);
      returned = std::exchange(pool.returned, nullptr);
    }
    if (returned) {
      while (returned) {
        auto *next = returned->next;
        const auto bucket = CapacityBucket(returned->values.capacity());
        returned->next = pool.available[bucket];
        pool.available[bucket] = returned;
        returned = next;
      }
    }

    Block *block = nullptr;
    const auto required_bucket = CapacityBucket(required_capacity);
    for (size_type bucket = required_bucket;
         bucket < pool.available.size(); ++bucket) {
      if (!pool.available[bucket])
        continue;
      block = pool.available[bucket];
      pool.available[bucket] = block->next;
      break;
    }
    if (!block) {
      for (size_type bucket = required_bucket; bucket-- > 0;) {
        if (!pool.available[bucket])
          continue;
        block = pool.available[bucket];
        pool.available[bucket] = block->next;
        break;
      }
    }
    if (!block) {
      block = new Block();
      block->owner = &pool;
      RecordAllocationEvent();
    }
    block->next = nullptr;
    block->references.store(1, std::memory_order_relaxed);
    if (block->values.capacity() < required_capacity) {
      RecordAllocationEvent();
      block->values.reserve(required_capacity);
    }
    return block;
  }

  static void Retain(Block *block) {
    if (block)
      block->references.fetch_add(1, std::memory_order_relaxed);
  }

  static void Release(Block *block) {
    if (!block ||
        block->references.fetch_sub(1, std::memory_order_acq_rel) != 1)
      return;
    block->values.clear();
    if (thread_pool_ == block->owner) {
      const auto bucket = CapacityBucket(block->values.capacity());
      block->next = block->owner->available[bucket];
      block->owner->available[bucket] = block;
      return;
    }
    std::lock_guard lock(block->owner->returned_mutex);
    block->next = block->owner->returned;
    block->owner->returned = block;
  }

  inline static thread_local std::uint64_t thread_allocation_events_ = 0;
  inline static thread_local Pool *thread_pool_ = nullptr;
  Block *storage_ = nullptr;
};

enum CompiledCommandStateDomain : std::uint32_t {
  CompiledCommandStateDomainPipeline = 1u << 0,
  CompiledCommandStateDomainRootSignature = 1u << 1,
  CompiledCommandStateDomainDescriptorHeaps = 1u << 2,
  CompiledCommandStateDomainRootTables = 1u << 3,
  CompiledCommandStateDomainRootConstants = 1u << 4,
  CompiledCommandStateDomainRootDescriptors = 1u << 5,
  CompiledCommandStateDomainInputAssembler = 1u << 6,
  CompiledCommandStateDomainRenderTargets = 1u << 7,
  CompiledCommandStateDomainViewports = 1u << 8,
  CompiledCommandStateDomainScissors = 1u << 9,
  CompiledCommandStateDomainBlendFactor = 1u << 10,
  CompiledCommandStateDomainStencilRef = 1u << 11,
  CompiledCommandStateDomainTopology = 1u << 12,
};

struct CompiledCommandStateDelta {
  std::uint32_t dirty_domains = 0;
  std::uint64_t root_table_dirty_mask = 0;
  std::uint64_t root_constant_dirty_mask = 0;
  std::uint64_t root_descriptor_dirty_mask = 0;
};

struct CompiledCommandDescriptorHeaps {
  Com<ID3D12DescriptorHeap> cbv_srv_uav;
  Com<ID3D12DescriptorHeap> sampler;
  CompiledImmutableVector<Com<ID3D12DescriptorHeap>> all;
};

struct CompiledCommandPipelineMetadata {
  PipelineState *pipeline = nullptr;
  const PipelineMetalGraphicsState *metal_graphics = nullptr;
  const PipelineMetalComputeState *metal_compute = nullptr;
  // Binding identity follows the shader-visible layout rather than the PSO
  // object. Raster/depth/blend-only PSO changes must not invalidate root
  // arguments that the next pipeline consumes through the same ABI layout.
  std::uint64_t binding_layout_fingerprint = 0;
  std::uint64_t vertex_layout_fingerprint = 0;
  PipelineStateType type = PipelineStateType::Graphics;
  DXMT12_MTL4_SHADER_ABI_VERSION shader_abi_version =
      DXMT12_MTL4_SHADER_ABI_BINDLESS_MIRROR;
  NativeShaderAbiEligibilityReason native_eligibility_reason =
      NativeShaderAbiEligibilityReason::UnsupportedRootSignature;
  dxmt::CompiledFallbackReason perf_fallback_reason =
      dxmt::CompiledFallbackReason::Unknown;
  bool has_pipeline_state = false;
  bool has_dxmt_pipeline = false;
  bool type_matches = false;
  bool has_root_signature = false;
  bool uses_bindless_mirror = false;
  bool uses_bindless_mirror_abi = false;
  bool uses_native_descriptor_table_abi = false;
  bool uses_geometry = false;
  bool uses_tessellation = false;
  bool ordinary_native = false;
  bool ordinary_bindless = false;
  bool ordinary_compiled = false;
  bool metal_pso_ready = false;
};

struct CompiledCommandPipelineBinding {
  Com<ID3D12PipelineState> pipeline_state;
  Com<ID3D12RootSignature> root_signature;
  RootSignature *root_signature_impl = nullptr;
  CompiledCommandPipelineMetadata metadata;
  bool pipeline_state_pending = true;
  bool root_signature_pending = true;
  bool bindless_candidate = false;
};

struct CompiledCommandRootDescriptorTable {
  UINT root_parameter_index = 0;
  D3D12_DESCRIPTOR_HEAP_TYPE heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
  D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor = {};
  UINT heap_index = 0;
  UINT descriptor_count = 0;
  UINT heap_count = 0;
  UINT table_offset = 0;
  UINT table_entry_stride = sizeof(uint64_t) * 3;
  UINT root_table_base_descriptor_index = 0;
  uint64_t descriptor_table_gpu_address = 0;
  uint64_t descriptor_table_entry_gpu_address = 0;
  uint64_t buffer_descriptor_record_gpu_address = 0;
  uint64_t buffer_resource_table_gpu_address = 0;
  uint64_t buffer_resource_table_generation = 0;
  // Keeps the mirror-owned descriptor backend buffers alive while compiled
  // packets are in flight.
  Com<ID3D12DescriptorHeap> owning_heap;
  DescriptorHeapMirror *mirror = nullptr;
  bool resolved = false;
  bool descriptor_table_backend_ready = false;
  bool native_descriptor_record_storage_ready = false;
  bool native_buffer_resource_table_ready = false;
  bool native_root_table_base_ready = false;
};

struct CompiledCommandRootConstants {
  UINT root_parameter_index = 0;
  UINT dst_offset = 0;
  CompiledImmutableVector<UINT> values;
};

struct CompiledCommandRootDescriptor {
  D3D12_ROOT_PARAMETER_TYPE parameter_type = D3D12_ROOT_PARAMETER_TYPE_CBV;
  UINT root_parameter_index = 0;
  D3D12_GPU_VIRTUAL_ADDRESS address = 0;
};

struct CompiledCommandVertexBuffer {
  UINT slot = 0;
  D3D12_VERTEX_BUFFER_VIEW view = {};
};

struct CompiledCommandInputAssemblerState {
  CompiledImmutableVector<CompiledCommandVertexBuffer> vertex_buffers;
  std::optional<D3D12_INDEX_BUFFER_VIEW> index_buffer;
  std::uint64_t vertex_buffer_dirty_mask = 0;
  bool index_buffer_dirty = false;
};

struct CompiledCommandRenderState {
  CompiledImmutableVector<DescriptorRecord> render_targets;
  std::optional<DescriptorRecord> depth_stencil;
  CompiledImmutableVector<D3D12_VIEWPORT> viewports;
  CompiledImmutableVector<D3D12_RECT> scissors;
  std::array<FLOAT, 4> blend_factor = {1.0f, 1.0f, 1.0f, 1.0f};
  UINT stencil_ref = 0;
  D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
};

struct CompiledDynamicRenderStateRecipe {
  CompiledImmutableVector<WMTViewport> viewports;
  CompiledImmutableVector<WMTScissorRect> scissors;
  std::array<FLOAT, 4> blend_factor = {1.0f, 1.0f, 1.0f, 1.0f};
  uint8_t stencil_ref = 0;
  bool valid = false;
};

struct CompiledVertexBindingRecipe {
  struct Binding {
    uint32_t slot = 0;
    uint64_t offset = 0;
    uint32_t stride = 0;
    Rc<Buffer> buffer;
    Rc<BufferAllocation> allocation;
  };
  CompiledImmutableVector<Binding> bindings;
  uint32_t slot_mask = 0;
  uint32_t cleared_slot_mask = 0;
};

enum CompiledBindingDirtyField : std::uint32_t {
  CompiledBindingDirtyPipelineLayout = 1u << 0,
  CompiledBindingDirtyResourceHeap = 1u << 1,
  CompiledBindingDirtySamplerHeap = 1u << 2,
  CompiledBindingDirtyRootTables = 1u << 3,
  CompiledBindingDirtyRootConstants = 1u << 4,
  CompiledBindingDirtyRootDescriptors = 1u << 5,
  CompiledBindingDirtyVertexBuffers = 1u << 6,
};

struct CompiledBindingProgram;

struct CompiledBindingDelta {
  std::shared_ptr<const CompiledBindingProgram> base_program;
  const void *base_resource_heap_identity = nullptr;
  const void *base_sampler_heap_identity = nullptr;
  const void *base_root_tables_identity = nullptr;
  const void *base_root_constants_identity = nullptr;
  const void *base_root_descriptors_identity = nullptr;
  const void *base_vertex_bindings_identity = nullptr;
  std::uint32_t dirty_fields = 0;
  std::uint64_t root_table_dirty_mask = 0;
  std::uint64_t root_constant_dirty_mask = 0;
  std::uint64_t root_descriptor_dirty_mask = 0;
  std::uint32_t vertex_buffer_dirty_mask = 0;
  bool full_bind = false;
};

// Immutable Close-time binding layout. Mutable descriptor heaps, root
// arguments and vertex bindings deliberately do not participate in this
// object's identity. The encoder keeps this layout installed while applying
// field/slot deltas from packet binding state.
struct CompiledBindingProgram {
  CompiledCommandPipelineBinding pipeline;
  bool compute = false;
};

struct CompiledNativeStageBinding {
  uint64_t cbuffer_root_base_offset = 0;
  uint64_t resource_root_base_offset = 0;
  uint32_t cbuffer_root_base_count = 0;
  uint32_t resource_root_base_count = 0;
  std::uint64_t cbuffer_root_table_mask = 0;
  std::uint64_t resource_root_table_mask = 0;
  std::uint64_t cbuffer_root_constant_mask = 0;
  std::uint64_t resource_root_constant_mask = 0;
  std::uint64_t cbuffer_root_descriptor_mask = 0;
  std::uint64_t resource_root_descriptor_mask = 0;
  // CPU copies are retained for bounded diagnostics. The GPU consumes the
  // packed native_root_base_buffer; keeping the source words lets a hang log
  // identify the exact descriptor-table bases without a synchronizing GPU
  // readback that would perturb command scheduling.
  std::vector<uint32_t> cbuffer_root_bases;
  std::vector<uint32_t> resource_root_bases;
  bool ready = false;
};

struct CompiledGraphicsPacket {
  UINT record_index = 0;
  std::uint64_t d3d_sequence = 0;
  CompiledCommandPipelineBinding pipeline;
  CompiledCommandDescriptorHeaps descriptor_heaps;
  CompiledImmutableVector<CompiledCommandRootDescriptorTable> root_tables;
  CompiledImmutableVector<CompiledCommandRootConstants> root_constants;
  CompiledImmutableVector<CompiledCommandRootDescriptor> root_descriptors;
  CompiledCommandInputAssemblerState input_assembler;
  CompiledCommandRenderState render_state;
  std::shared_ptr<const CompiledDynamicRenderStateRecipe>
      dynamic_render_state_recipe;
  std::shared_ptr<const CompiledVertexBindingRecipe> vertex_binding_recipe;
  std::shared_ptr<const CompiledBindingProgram> binding_program;
  CompiledBindingDelta binding_delta;
  CompiledImmutableVector<Rc<BufferAllocation>> direct_buffer_allocations;
  CompiledCommandStateDelta state_delta;
  CompiledCommandFallbackReason vertex_binding_recipe_reason =
      CompiledCommandFallbackReason::None;
  bool root_tables_close_materialized = false;
  CompiledCommandFallbackReason compatibility_reason =
      CompiledCommandFallbackReason::None;
  std::optional<DrawInstancedRecord> draw;
  std::optional<DrawIndexedInstancedRecord> draw_indexed;
};

struct CompiledComputePacket {
  UINT record_index = 0;
  std::uint64_t d3d_sequence = 0;
  CompiledCommandPipelineBinding pipeline;
  CompiledCommandDescriptorHeaps descriptor_heaps;
  CompiledImmutableVector<CompiledCommandRootDescriptorTable> root_tables;
  CompiledImmutableVector<CompiledCommandRootConstants> root_constants;
  CompiledImmutableVector<CompiledCommandRootDescriptor> root_descriptors;
  std::shared_ptr<const CompiledBindingProgram> binding_program;
  CompiledBindingDelta binding_delta;
  CompiledImmutableVector<Rc<BufferAllocation>> direct_buffer_allocations;
  CompiledCommandStateDelta state_delta;
  bool root_tables_close_materialized = false;
  CompiledCommandFallbackReason compatibility_reason =
      CompiledCommandFallbackReason::None;
  DispatchRecord dispatch;
};

// ExecuteIndirect remains dynamically decoded by the GPU/argument-buffer
// path, but its command-list state is compiled at Close just like an ordinary
// draw or dispatch. This keeps the dynamic D3D12 semantics without retaining
// or scanning the original command record at submission.
struct CompiledIndirectPacket {
  UINT record_index = 0;
  std::uint64_t d3d_sequence = 0;
  ExecuteIndirectRecord execute;
  // Index of the immutable graphics/compute state packet in the generation's
  // normal packet store. Indirect work must participate in submission-time
  // descriptor freezing and Close-time binding-program compilation just like
  // an ordinary draw/dispatch; keeping a private packet here bypassed both.
  UINT state_packet_index = UINT_MAX;
  bool compute = false;
  std::optional<CompiledGraphicsPacket> graphics_state;
  std::optional<CompiledComputePacket> compute_state;
};

enum class CompiledTransferOpcode : std::uint8_t {
  CopyBufferRegion,
  CopyTextureRegion,
  CopyResource,
  CopyTiles,
  ResolveSubresource,
  ClearRenderTarget,
  ClearDepthStencil,
  ClearUnorderedAccess,
  DiscardResource,
  WriteBufferImmediate,
  TemporalUpscale,
};

struct CompiledTransferPacket {
  CompiledTransferOpcode opcode = CompiledTransferOpcode::CopyBufferRegion;
  UINT payload_index = 0;
  UINT record_index = 0;
  std::uint64_t d3d_sequence = 0;
};

struct CompiledTransferStorage {
  CompiledImmutableVector<CopyBufferRegionRecord> copy_buffer_regions;
  CompiledImmutableVector<CopyTextureRegionRecord> copy_texture_regions;
  CompiledImmutableVector<CopyResourceRecord> copy_resources;
  CompiledImmutableVector<CopyTilesRecord> copy_tiles;
  CompiledImmutableVector<ResolveSubresourceRecord> resolves;
  CompiledImmutableVector<ClearRenderTargetRecord> clear_render_targets;
  CompiledImmutableVector<ClearDepthStencilRecord> clear_depth_stencils;
  CompiledImmutableVector<ClearUnorderedAccessRecord> clear_unordered_access;
  CompiledImmutableVector<DiscardResourceRecord> discards;
  CompiledImmutableVector<WriteBufferImmediateRecord> write_buffer_immediate;
  CompiledImmutableVector<TemporalUpscaleRecord> temporal_upscales;
};

enum class CompiledControlOpcode : std::uint8_t {
  ClearState,
  BeginQuery,
  EndQuery,
  ResolveQueryData,
  Predication,
};

struct CompiledControlPacket {
  CompiledControlOpcode opcode = CompiledControlOpcode::ClearState;
  UINT payload_index = 0;
  UINT record_index = 0;
  std::uint64_t d3d_sequence = 0;
};

struct CompiledControlStorage {
  CompiledImmutableVector<ClearStateRecord> clear_states;
  CompiledImmutableVector<BeginQueryRecord> begin_queries;
  CompiledImmutableVector<EndQueryRecord> end_queries;
  CompiledImmutableVector<ResolveQueryDataRecord> resolve_queries;
  CompiledImmutableVector<PredicationRecord> predications;
};

struct CompiledCommandSegment {
  CompiledCommandSegmentKind kind = CompiledCommandSegmentKind::Fallback;
  UINT first_record_index = 0;
  UINT record_count = 0;
  UINT first_graphics_packet = 0;
  UINT graphics_packet_count = 0;
  UINT first_compute_packet = 0;
  UINT compute_packet_count = 0;
  UINT first_indirect_packet = 0;
  UINT indirect_packet_count = 0;
  UINT first_barrier = 0;
  UINT barrier_count = 0;
  UINT first_transfer_packet = 0;
  UINT transfer_packet_count = 0;
  UINT first_control_packet = 0;
  UINT control_packet_count = 0;
  CompiledCommandFallbackReason fallback_reason =
      CompiledCommandFallbackReason::None;
  dxmt::CompiledFallbackReason perf_fallback_reason =
      dxmt::CompiledFallbackReason::Unknown;
};

// Close-time execution graph. Segments preserve the exact source-stream
// partition for diagnostics and compatibility fallback; encoder nodes are the
// smaller runtime program consumed by ExecuteCommandLists. State setters that
// have already been folded into packets never become execution nodes.
enum class CompiledEncoderKind : std::uint8_t {
  None,
  Graphics,
  Compute,
};

struct CompiledEncoderNode {
  CompiledCommandSegment work;
  UINT predecessor_node = UINT_MAX;
  UINT encoder_index = UINT_MAX;
  UINT first_source_record_index = 0;
  UINT source_record_count = 0;
  UINT elided_state_record_count = 0;
  CompiledEncoderKind encoder_kind = CompiledEncoderKind::None;
  bool begins_encoder = false;
  bool ends_encoder = false;
};

struct CompiledEncoderGraph {
  CompiledImmutableVector<CompiledEncoderNode> nodes;
  UINT elided_state_record_count = 0;
  UINT encoder_count = 0;
  UINT inlined_barrier_node_count = 0;
};

struct CompiledCommandBarrierRange {
  UINT record_index = 0;
  UINT first_barrier = 0;
  UINT barrier_count = 0;
  std::uint64_t epoch = 0;
};

struct CompiledCommandResourceStateDelta {
  Com<ID3D12Resource> resource;
  UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  D3D12_RESOURCE_STATES import_state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES export_state = D3D12_RESOURCE_STATE_COMMON;
  std::uint64_t first_epoch = 0;
  std::uint64_t last_epoch = 0;
};

struct CompiledCommandAccessSummary {
  CompiledImmutableVector<CompiledCommandBarrierRange> barrier_ranges;
  CompiledImmutableVector<StoredResourceBarrier> barriers;
  CompiledImmutableVector<CompiledCommandResourceStateDelta>
      resource_state_deltas;
  std::uint64_t final_barrier_epoch = 0;
};

struct CompiledCommandTestTelemetry {
  std::atomic<UINT> replayed_graphics_packets = 0;
  std::atomic<UINT> replayed_compute_packets = 0;
  std::atomic<UINT> replayed_fallback_ranges = 0;
  std::atomic<UINT> replayed_fallback_records = 0;
  std::atomic<UINT> replayed_compiled_packet_fallbacks = 0;
  std::atomic<UINT> replayed_empty_native_segments = 0;
  std::atomic<UINT> replayed_empty_fallback_segments = 0;
  std::atomic<UINT> submitted_graphics_packets = 0;
  std::atomic<UINT> submitted_compute_packets = 0;
  std::atomic<UINT> submission_prepare_failures = 0;
  std::atomic<UINT> submitted_descriptor_snapshots = 0;
  std::atomic<UINT> submitted_descriptor_entries = 0;
  std::atomic<UINT> submitted_unique_descriptor_snapshots = 0;
  std::atomic<UINT> submitted_unique_descriptor_records = 0;
  std::atomic<UINT> submitted_descriptor_record_reuses = 0;
  std::atomic<UINT> generic_descriptor_span_lookups = 0;
  std::atomic<UINT> generic_descriptor_span_unique = 0;
  std::atomic<UINT> generic_descriptor_span_reuses = 0;
  std::atomic<UINT> frozen_native_direct_packets = 0;
  std::atomic<UINT> frozen_range_lookups = 0;
  std::atomic<UINT> frozen_range_unique = 0;
  std::atomic<UINT> frozen_range_reuses = 0;
  std::atomic<UINT> frozen_root_word_lookups = 0;
  std::atomic<UINT> frozen_root_word_reuses = 0;
  std::atomic<UINT> submission_graphics_encoder_boundary_merges = 0;
  std::atomic<UINT> submission_compute_encoder_boundary_merges = 0;
  std::atomic<UINT> submission_encoder_boundary_flushes = 0;
  std::atomic<UINT> replayed_indirect_nodes = 0;
  std::atomic<UINT> replayed_indirect_dependency_nodes = 0;
  std::atomic<UINT> replayed_indirect_fallbacks = 0;
  std::atomic<UINT> encoder_full_binding_programs = 0;
  std::atomic<UINT> encoder_delta_binding_programs = 0;
  std::atomic<UINT> encoder_binding_program_hits = 0;
  std::atomic<UINT> encoder_native_binding_ops = 0;
  std::atomic<UINT> encoder_native_binding_ops_skipped = 0;
  std::atomic<UINT> encoder_resource_plan_publications = 0;
  std::atomic<UINT> encoder_resource_plan_reuses = 0;
  std::atomic<UINT> submitted_generation_shares = 0;
  std::atomic<UINT> submitted_generation_deep_copies = 0;
  std::atomic<UINT> encoder_attachment_materializations = 0;
  std::atomic<UINT> submitted_root_table_fast_patches = 0;
  std::atomic<UINT> submitted_root_table_full_materializations = 0;
  std::atomic<UINT> submitted_native_binding_plan_reuses = 0;
  std::atomic<UINT> submitted_native_binding_recipe_reuses = 0;
};

struct CompiledCommandList {
  std::uint64_t generation = 0;
  UINT record_count = 0;
  // Retained only for explicit compatibility/fault fallback. Native plans do
  // not own or scan the original typed command stream.
  std::shared_ptr<const std::vector<CommandRecord>> fallback_records;
  CompiledImmutableVector<CompiledCommandSegment> segments;
  CompiledEncoderGraph encoder_graph;
  CompiledImmutableVector<CompiledGraphicsPacket> graphics_packets;
  CompiledImmutableVector<CompiledComputePacket> compute_packets;
  CompiledImmutableVector<CompiledIndirectPacket> indirect_packets;
  CompiledImmutableVector<CompiledTransferPacket> transfer_packets;
  CompiledTransferStorage transfer_storage;
  CompiledImmutableVector<CompiledControlPacket> control_packets;
  CompiledControlStorage control_storage;
  // Close-time control-flow reduction. Execute consumes this immutable mask
  // instead of rescanning the typed stream for superseded state setters on
  // every submission of the same command-list generation.
  CompiledImmutableVector<std::uint8_t> superseded_state_record_mask;
  CompiledCommandAccessSummary access_summary;
  UINT unexpected_container_growths = 0;
  UINT storage_allocation_events = 0;
  UINT node_storage_allocation_events = 0;
  UINT state_storage_allocation_events = 0;
  UINT access_storage_allocation_events = 0;
  UINT immutable_state_reuses = 0;
  UINT close_materialized_root_table_sets = 0;
  UINT close_dynamic_render_state_recipes = 0;
  UINT close_vertex_binding_recipes = 0;
  UINT close_direct_access_plans = 0;
  UINT close_binding_programs = 0;
  UINT close_binding_program_reuses = 0;
  UINT close_full_binding_programs = 0;
  UINT close_delta_binding_programs = 0;
  dxmt::d3d12::test::ExecutionPathMode test_path_mode =
      dxmt::d3d12::test::ExecutionPathMode::Auto;
  UINT test_work_record_count = 0;
  UINT test_compiled_work_record_count = 0;
  bool test_native_requirement_satisfied = true;
  std::shared_ptr<CompiledCommandTestTelemetry> test_telemetry;
};

struct SubmittedCompiledGraphicsPacket {
  std::shared_ptr<const std::vector<CompiledCommandRootDescriptorTable>>
      root_tables;
  CompiledCommandFallbackReason prepare_reason =
      CompiledCommandFallbackReason::None;
};

struct SubmittedCompiledComputePacket {
  std::shared_ptr<const std::vector<CompiledCommandRootDescriptorTable>>
      root_tables;
  CompiledCommandFallbackReason prepare_reason =
      CompiledCommandFallbackReason::None;
};

// Per-Execute overlay. The Close generation remains immutable and shared;
// only descriptor-table materialization and backend generation state that
// must be frozen at Execute lives here.
struct SubmittedCompiledCommandListPlan {
  std::shared_ptr<const CompiledCommandList> generation;
  std::vector<SubmittedCompiledGraphicsPacket> graphics_packets;
  std::vector<SubmittedCompiledComputePacket> compute_packets;
};

const char *CompiledCommandSegmentKindName(CompiledCommandSegmentKind kind);
const char *CompiledCommandFallbackReasonName(
    CompiledCommandFallbackReason reason);
dxmt::CompiledFallbackReason
CompiledCommandFallbackReasonToPerf(CompiledCommandFallbackReason reason);

std::shared_ptr<SubmittedCompiledCommandListPlan>
PrepareSubmittedCompiledCommandList(
    std::shared_ptr<const CompiledCommandList> compiled);

class GraphicsCommandList {
public:
  virtual ~GraphicsCommandList() = default;

  virtual IMTLD3D12Device *GetParentDevice() const = 0;
  virtual bool IsClosed() const = 0;
  virtual D3D12_COMMAND_LIST_TYPE GetCommandListType() const = 0;
  virtual std::shared_ptr<const std::vector<CommandRecord>>
  GetCommandRecordGeneration() const = 0;
  virtual std::shared_ptr<const CompiledCommandList> GetCompiledCommands()
      const = 0;
  virtual void SetApitraceLifecycleRecordingEnabled(bool enabled) = 0;
  virtual HRESULT MarkSubmittedToQueue(
      D3D12_COMMAND_LIST_TYPE queue_type,
      std::vector<SubmittedCommandAllocatorUse> &allocator_uses) = 0;
};

class CommandSignature {
public:
  virtual ~CommandSignature() = default;
  virtual IMTLD3D12Device *GetParentDevice() const = 0;
  virtual const D3D12_COMMAND_SIGNATURE_DESC &GetDesc() const = 0;
  virtual const std::vector<D3D12_INDIRECT_ARGUMENT_DESC> &GetArguments() const = 0;
  virtual ID3D12RootSignature *GetRootSignature() const = 0;
};

Com<ID3D12GraphicsCommandList>
CreateGraphicsCommandList(IMTLD3D12Device *device, UINT node_mask,
                          D3D12_COMMAND_LIST_TYPE type,
                          ID3D12CommandAllocator *command_allocator,
                          ID3D12PipelineState *initial_pipeline_state,
                          HRESULT *status = nullptr);

Com<ID3D12CommandSignature>
CreateCommandSignature(IMTLD3D12Device *device,
                       const D3D12_COMMAND_SIGNATURE_DESC *desc,
                       ID3D12RootSignature *root_signature);

} // namespace dxmt::d3d12
