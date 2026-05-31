#include "d3d12_command_list.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace dxmt::d3d12 {
namespace {

std::atomic<uint32_t> g_apitrace_record_diag_log_count = 0;
thread_local uint64_t g_current_command_record_d3d_sequence = 0;

#ifdef __ID3D12GraphicsCommandList4_INTERFACE_DEFINED__
using ApitraceRenderPassResolveSubresources =
    std::vector<std::vector<dxmt::apitrace::RenderPassResolveSubresourceDesc>>;

static dxmt::apitrace::RenderPassClearValue
ToApitraceRenderPassClearValue(const D3D12_CLEAR_VALUE &value) {
  dxmt::apitrace::RenderPassClearValue result = {};
  result.format = static_cast<uint32_t>(value.Format);
  for (uint32_t index = 0; index < 4; ++index)
    result.color[index] = value.Color[index];
  result.depth = value.DepthStencil.Depth;
  result.stencil = value.DepthStencil.Stencil;
  return result;
}

static dxmt::apitrace::RenderPassBeginningAccessDesc
ToApitraceRenderPassBeginningAccess(
    const D3D12_RENDER_PASS_BEGINNING_ACCESS &access) {
  dxmt::apitrace::RenderPassBeginningAccessDesc result = {};
  result.type = static_cast<uint32_t>(access.Type);
  if (access.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
    result.clear = ToApitraceRenderPassClearValue(access.Clear.ClearValue);
  return result;
}

static dxmt::apitrace::RenderPassEndingAccessDesc
ToApitraceRenderPassEndingAccess(
    const D3D12_RENDER_PASS_ENDING_ACCESS &access,
    std::vector<dxmt::apitrace::RenderPassResolveSubresourceDesc> &subresources) {
  dxmt::apitrace::RenderPassEndingAccessDesc result = {};
  result.type = static_cast<uint32_t>(access.Type);
  if (access.Type != D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE)
    return result;

  result.src_resource = access.Resolve.pSrcResource;
  result.dst_resource = access.Resolve.pDstResource;
  result.subresource_count = access.Resolve.SubresourceCount;
  result.format = static_cast<uint32_t>(access.Resolve.Format);
  result.resolve_mode = static_cast<uint32_t>(access.Resolve.ResolveMode);
  result.preserve_resolve_source = access.Resolve.PreserveResolveSource;
  subresources.reserve(access.Resolve.SubresourceCount);
  for (UINT index = 0;
       access.Resolve.pSubresourceParameters &&
       index < access.Resolve.SubresourceCount;
       ++index) {
    const auto &src = access.Resolve.pSubresourceParameters[index];
    dxmt::apitrace::RenderPassResolveSubresourceDesc dst = {};
    dst.src_subresource = src.SrcSubresource;
    dst.dst_subresource = src.DstSubresource;
    dst.dst_x = src.DstX;
    dst.dst_y = src.DstY;
    dst.has_src_rect = true;
    dst.src_left = src.SrcRect.left;
    dst.src_top = src.SrcRect.top;
    dst.src_right = src.SrcRect.right;
    dst.src_bottom = src.SrcRect.bottom;
    subresources.push_back(dst);
  }
  result.subresource_count = static_cast<uint32_t>(subresources.size());
  result.subresources = subresources.data();
  return result;
}

static dxmt::apitrace::RenderPassRenderTargetDesc
ToApitraceRenderPassRenderTarget(
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC &render_target,
    std::vector<dxmt::apitrace::RenderPassResolveSubresourceDesc> &subresources) {
  dxmt::apitrace::RenderPassRenderTargetDesc result = {};
  result.cpu_descriptor = render_target.cpuDescriptor.ptr;
  result.beginning_access =
      ToApitraceRenderPassBeginningAccess(render_target.BeginningAccess);
  result.ending_access =
      ToApitraceRenderPassEndingAccess(render_target.EndingAccess, subresources);
  return result;
}

static dxmt::apitrace::RenderPassDepthStencilDesc
ToApitraceRenderPassDepthStencil(
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC &depth_stencil,
    std::array<std::vector<dxmt::apitrace::RenderPassResolveSubresourceDesc>, 2> &subresources) {
  dxmt::apitrace::RenderPassDepthStencilDesc result = {};
  result.cpu_descriptor = depth_stencil.cpuDescriptor.ptr;
  result.depth_beginning_access =
      ToApitraceRenderPassBeginningAccess(depth_stencil.DepthBeginningAccess);
  result.stencil_beginning_access =
      ToApitraceRenderPassBeginningAccess(depth_stencil.StencilBeginningAccess);
  result.depth_ending_access =
      ToApitraceRenderPassEndingAccess(depth_stencil.DepthEndingAccess, subresources[0]);
  result.stencil_ending_access =
      ToApitraceRenderPassEndingAccess(depth_stencil.StencilEndingAccess, subresources[1]);
  return result;
}

static bool
IsRenderPassPreserveOrNoAccess(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE type) {
  return type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE ||
         type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
}

static bool
IsRenderPassPreserveOrNoAccess(D3D12_RENDER_PASS_ENDING_ACCESS_TYPE type) {
  return type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE ||
         type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
}

static UINT
RenderPassMipLevel(const D3D12_RESOURCE_DESC &desc, UINT subresource) {
  const UINT mip_levels = desc.MipLevels ? desc.MipLevels : 1;
  return mip_levels ? subresource % mip_levels : 0;
}

static bool
IsFullSubresourceRect(ID3D12Resource *resource, UINT subresource,
                      const D3D12_RECT &rect) {
  if (!resource)
    return false;

  auto *d3d12_resource = dynamic_cast<Resource *>(resource);
  if (!d3d12_resource)
    return false;

  const auto &desc = d3d12_resource->GetResourceDesc();
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return false;

  const UINT mip = RenderPassMipLevel(desc, subresource);
  const LONG width = static_cast<LONG>(std::max<UINT64>(1, desc.Width >> mip));
  const LONG height =
      static_cast<LONG>(std::max<UINT64>(1, desc.Height >> mip));
  return rect.left == 0 && rect.top == 0 && rect.right == width &&
         rect.bottom == height;
}
#endif

StoredTextureCopyLocation
StoreTextureCopyLocation(const D3D12_TEXTURE_COPY_LOCATION &location) {
  StoredTextureCopyLocation stored = {};
  stored.resource = location.pResource;
  stored.type = location.Type;
  if (location.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
    stored.placed_footprint = location.PlacedFootprint;
  else
    stored.subresource_index = location.SubresourceIndex;
  return stored;
}

StoredResourceBarrier
StoreResourceBarrier(const D3D12_RESOURCE_BARRIER &barrier) {
  StoredResourceBarrier stored = {};
  stored.barrier = barrier;
  switch (barrier.Type) {
  case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
    stored.resource = barrier.Transition.pResource;
    break;
  case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
    stored.resource_before = barrier.Aliasing.pResourceBefore;
    stored.resource_after = barrier.Aliasing.pResourceAfter;
    break;
  case D3D12_RESOURCE_BARRIER_TYPE_UAV:
    stored.resource = barrier.UAV.pResource;
    break;
  }
  return stored;
}

static bool
IsWriteResourceState(D3D12_RESOURCE_STATES state) {
  static constexpr UINT WriteStateBits =
      UINT(D3D12_RESOURCE_STATE_RENDER_TARGET) |
      UINT(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
      UINT(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
      UINT(D3D12_RESOURCE_STATE_COPY_DEST) |
      UINT(D3D12_RESOURCE_STATE_RESOLVE_DEST) |
      UINT(D3D12_RESOURCE_STATE_STREAM_OUT);
  return (static_cast<UINT>(state) & WriteStateBits) != 0;
}

static bool
IsValidTransitionState(D3D12_RESOURCE_STATES state) {
  if (state == D3D12_RESOURCE_STATE_COMMON)
    return true;

  if (!IsWriteResourceState(state))
    return true;

  const auto bits = static_cast<UINT>(state);
  return (bits & (bits - 1)) == 0;
}

static bool
IsValidResourceBarrier(const D3D12_RESOURCE_BARRIER &barrier) {
  switch (barrier.Type) {
  case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
    return barrier.Transition.pResource &&
           IsValidTransitionState(barrier.Transition.StateBefore) &&
           IsValidTransitionState(barrier.Transition.StateAfter);
  case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
  case D3D12_RESOURCE_BARRIER_TYPE_UAV:
    return true;
  default:
    return false;
  }
}

#ifdef __ID3D12GraphicsCommandList6_INTERFACE_DEFINED__
using GraphicsCommandListComBase = ID3D12GraphicsCommandList6;
#elif defined(__ID3D12GraphicsCommandList5_INTERFACE_DEFINED__)
using GraphicsCommandListComBase = ID3D12GraphicsCommandList5;
#elif defined(__ID3D12GraphicsCommandList4_INTERFACE_DEFINED__)
using GraphicsCommandListComBase = ID3D12GraphicsCommandList4;
#elif defined(__ID3D12GraphicsCommandList2_INTERFACE_DEFINED__)
using GraphicsCommandListComBase = ID3D12GraphicsCommandList2;
#elif defined(__ID3D12GraphicsCommandList1_INTERFACE_DEFINED__)
using GraphicsCommandListComBase = ID3D12GraphicsCommandList1;
#else
using GraphicsCommandListComBase = ID3D12GraphicsCommandList;
#endif

class GraphicsCommandListImpl final
    : public ComObjectWithInitialRef<GraphicsCommandListComBase>,
      public GraphicsCommandList,
      public IMTLD3D12GraphicsCommandListExt {
public:
  ULONG STDMETHODCALLTYPE AddRef() override {
    return ComObjectWithInitialRef<GraphicsCommandListComBase>::AddRef();
  }

  ULONG STDMETHODCALLTYPE Release() override {
    return ComObjectWithInitialRef<GraphicsCommandListComBase>::Release();
  }

  ~GraphicsCommandListImpl() override {
    if (allocator_ && !closed_)
      allocator_->EndCommandListRecording(this);
  }

  GraphicsCommandListImpl(IMTLD3D12Device *device, UINT node_mask,
                          D3D12_COMMAND_LIST_TYPE type,
                          ID3D12CommandAllocator *command_allocator,
                          ID3D12PipelineState *initial_pipeline_state,
                          HRESULT *status)
      : device_(device), node_mask_(node_mask), type_(type),
        initial_pipeline_state_(initial_pipeline_state) {
    if (status)
      *status = S_OK;
    if (auto allocator_state = dynamic_cast<CommandAllocatorObject *>(command_allocator)) {
      if (allocator_state->GetCommandListType() == type_) {
        if (!allocator_state->BeginCommandListRecording(this)) {
          if (status)
            *status = E_INVALIDARG;
          return;
        }
        allocator_ = allocator_state;
      }
    }
    if (!IsPipelineStateCompatible(initial_pipeline_state_.ptr()))
      initial_pipeline_state_ = nullptr;
    current_pipeline_state_ = initial_pipeline_state_;
    if (current_pipeline_state_)
      AddRecord(PipelineStateRecord{current_pipeline_state_});
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12CommandList) ||
        riid == __uuidof(ID3D12GraphicsCommandList)) {
      *ppvObject = ref(AsGraphicsCommandList());
      return S_OK;
    }

#ifdef __ID3D12GraphicsCommandList1_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12GraphicsCommandList1)) {
      *ppvObject = ref(static_cast<ID3D12GraphicsCommandList1 *>(
          static_cast<GraphicsCommandListComBase *>(this)));
      return S_OK;
    }
#endif

#ifdef __ID3D12GraphicsCommandList2_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12GraphicsCommandList2)) {
      *ppvObject = ref(static_cast<ID3D12GraphicsCommandList2 *>(
          static_cast<GraphicsCommandListComBase *>(this)));
      return S_OK;
    }
#endif

#ifdef __ID3D12GraphicsCommandList4_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12GraphicsCommandList3)) {
      *ppvObject = ref(static_cast<ID3D12GraphicsCommandList3 *>(
          static_cast<GraphicsCommandListComBase *>(this)));
      return S_OK;
    }
    if (riid == __uuidof(ID3D12GraphicsCommandList4)) {
      *ppvObject = ref(static_cast<ID3D12GraphicsCommandList4 *>(this));
      return S_OK;
    }
#endif

#ifdef __ID3D12GraphicsCommandList5_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12GraphicsCommandList5)) {
      *ppvObject = ref(static_cast<ID3D12GraphicsCommandList5 *>(
          static_cast<GraphicsCommandListComBase *>(this)));
      return S_OK;
    }
#endif

#ifdef __ID3D12GraphicsCommandList6_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12GraphicsCommandList6)) {
      *ppvObject = ref(static_cast<ID3D12GraphicsCommandList6 *>(this));
      return S_OK;
    }
#endif

    if (riid == __uuidof(IMTLD3D12GraphicsCommandListExt)) {
      *ppvObject = ref(static_cast<IMTLD3D12GraphicsCommandListExt *>(this));
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12GraphicsCommandList), riid))
      WARN("D3D12GraphicsCommandList: unknown interface query ", str::format(riid));

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size, void *data) override {
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

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return type_; }

  HRESULT STDMETHODCALLTYPE Close() override {
    if (closed_)
      return E_FAIL;
    closed_ = true;
    if (allocator_)
      allocator_->EndCommandListRecording(this);
    HRESULT hr = recording_error_ ? recording_error_ : S_OK;
    if (apitrace_lifecycle_recording_enabled_ &&
        dxmt::apitrace::d3d_enabled()) {
      dxmt::apitrace::record_close_command_list(this, hr);
    }
    if (recording_error_)
      return recording_error_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *allocator,
                                  ID3D12PipelineState *initial_state) override {
    if (!allocator)
      return WARN_E_INVALIDARG(__func__);

    auto allocator_state = dynamic_cast<CommandAllocatorObject *>(allocator);
    if (!allocator_state || allocator_state->GetCommandListType() != type_)
      return WARN_E_INVALIDARG(__func__);
    if (!IsPipelineStateCompatible(initial_state))
      return WARN_E_INVALIDARG(__func__);

    if (allocator_.ptr() == allocator_state) {
      allocator_->EndCommandListRecording(this);
      if (!allocator_state->BeginCommandListRecording(this))
        return WARN_E_INVALIDARG(__func__);
    } else {
      if (!allocator_state->BeginCommandListRecording(this))
        return WARN_E_INVALIDARG(__func__);
      if (allocator_)
        allocator_->EndCommandListRecording(this);
    }
    allocator_ = allocator_state;
    initial_pipeline_state_ = initial_state;
    current_pipeline_state_ = initial_pipeline_state_;
    compute_root_signature_ = nullptr;
    graphics_root_signature_ = nullptr;
    records_.clear();
    closed_ = false;
    submitted_ = false;
    recording_error_ = S_OK;
    if (current_pipeline_state_) {
      if (apitrace_lifecycle_recording_enabled_) {
        g_current_command_record_d3d_sequence =
            dxmt::apitrace::record_reset_command_list(
                this, allocator, initial_state, S_OK);
      }
      AddRecord(PipelineStateRecord{current_pipeline_state_});
    } else if (apitrace_lifecycle_recording_enabled_) {
      dxmt::apitrace::record_reset_command_list(
          this, allocator, initial_state, S_OK);
    }
    return S_OK;
  }

  bool IsClosed() const override { return closed_; }

  D3D12_COMMAND_LIST_TYPE GetCommandListType() const override { return type_; }

  const std::vector<CommandRecord> &GetCommandRecords() const override {
    return records_;
  }

  void SetApitraceLifecycleRecordingEnabled(bool enabled) override {
    apitrace_lifecycle_recording_enabled_ = enabled;
  }

  HRESULT MarkSubmittedToQueue(
      D3D12_COMMAND_LIST_TYPE queue_type,
      std::vector<SubmittedCommandAllocatorUse> &allocator_uses) override {
    if (!closed_ || type_ != queue_type)
      return WARN_E_INVALIDARG(__func__);

    submitted_ = true;
    if (allocator_) {
      allocator_uses.push_back(
          SubmittedCommandAllocatorUse{allocator_, allocator_->MarkCommandListSubmitted()});
    }
    return S_OK;
  }

  void STDMETHODCALLTYPE
  TemporalUpscale(const MTL_TEMPORAL_UPSCALE_D3D12_DESC *desc) override {
    if (!desc || !desc->Color || !desc->Depth || !desc->MotionVector ||
        !desc->Output)
      return;

    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_temporal_upscale(
            this, desc->InputContentWidth, desc->InputContentHeight,
            desc->AutoExposure, desc->InReset, desc->DepthReversed,
            desc->MotionVectorInDisplayRes, desc->Color, desc->Depth,
            desc->MotionVector, desc->Output, desc->MotionVectorScaleX,
            desc->MotionVectorScaleY, desc->PreExposure, desc->ExposureTexture,
            desc->JitterOffsetX, desc->JitterOffsetY);
    TemporalUpscaleRecord record = {};
    record.input_content_width = desc->InputContentWidth;
    record.input_content_height = desc->InputContentHeight;
    const D3D12_RESOURCE_DESC motion_desc = desc->MotionVector->GetDesc();
    record.motion_vector_width = static_cast<UINT>(motion_desc.Width);
    record.motion_vector_height = motion_desc.Height;
    record.auto_exposure = desc->AutoExposure;
    record.in_reset = desc->InReset;
    record.depth_reversed = desc->DepthReversed;
    record.motion_vector_in_display_res = desc->MotionVectorInDisplayRes;
    record.color = desc->Color;
    record.depth = desc->Depth;
    record.motion_vector = desc->MotionVector;
    record.output = desc->Output;
    record.motion_vector_scale_x = desc->MotionVectorScaleX;
    record.motion_vector_scale_y = desc->MotionVectorScaleY;
    record.pre_exposure = desc->PreExposure;
    record.exposure_texture = desc->ExposureTexture;
    record.jitter_offset_x = desc->JitterOffsetX;
    record.jitter_offset_y = desc->JitterOffsetY;
    AddRecord(std::move(record));
  }

  HRESULT STDMETHODCALLTYPE CheckFeatureSupport(MTL_D3D12_FEATURE feature,
                                                void *feature_support_data,
                                                UINT feature_support_data_size) override {
    if (!feature_support_data)
      return E_INVALIDARG;

    switch (feature) {
    case MTL_D3D12_FEATURE_METALFX_TEMPORAL_SCALER:
      if (feature_support_data_size != sizeof(BOOL))
        return E_INVALIDARG;
      *reinterpret_cast<BOOL *>(feature_support_data) =
          device_->GetMTLDevice().supportsFXTemporalScaler();
      return S_OK;
    }

    return E_INVALIDARG;
  }

  void STDMETHODCALLTYPE ClearState(ID3D12PipelineState *pipeline_state) override {
    if (!IsPipelineStateCompatible(pipeline_state))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_clear_state(this, pipeline_state);
    current_pipeline_state_ = pipeline_state;
    AddRecord(PipelineStateRecord{current_pipeline_state_});
  }
  void STDMETHODCALLTYPE DrawInstanced(UINT vertex_count_per_instance, UINT instance_count,
                                       UINT start_vertex_location, UINT start_instance_location) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_draw_instanced(
            this, vertex_count_per_instance, instance_count,
            start_vertex_location, start_instance_location);
    AddRecord(DrawInstancedRecord{
        vertex_count_per_instance, instance_count, start_vertex_location,
        start_instance_location});
  }
  void STDMETHODCALLTYPE DrawIndexedInstanced(UINT index_count_per_instance, UINT instance_count,
                                              UINT start_vertex_location, INT base_vertex_location,
                                              UINT start_instance_location) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_draw_indexed_instanced(
            this, index_count_per_instance, instance_count,
            start_vertex_location, base_vertex_location, start_instance_location);
    AddRecord(DrawIndexedInstancedRecord{
        index_count_per_instance, instance_count, start_vertex_location,
        base_vertex_location, start_instance_location});
  }
  void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_dispatch(this, x, y, z);
    AddRecord(DispatchRecord{x, y, z});
  }
  void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource *dst_buffer, UINT64 dst_offset,
                                          ID3D12Resource *src_buffer, UINT64 src_offset,
                                          UINT64 byte_count) override {
    RecordCopyBufferRegion("CopyBufferRegion", dst_buffer, dst_offset, src_buffer,
                           src_offset, byte_count);
  }
  void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, UINT dst_x,
                                           UINT dst_y, UINT dst_z,
                                           const D3D12_TEXTURE_COPY_LOCATION *src,
                                           const D3D12_BOX *src_box) override {
    if (!dst || !src || !dst->pResource || !src->pResource)
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_copy_texture_region(
            this, dst, dst_x, dst_y, dst_z, src, src_box);
    CopyTextureRegionRecord record = {};
    record.dst = StoreTextureCopyLocation(*dst);
    record.dst_x = dst_x;
    record.dst_y = dst_y;
    record.dst_z = dst_z;
    record.src = StoreTextureCopyLocation(*src);
    if (src_box)
      record.src_box = *src_box;
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE CopyResource(ID3D12Resource *dst_resource,
                                      ID3D12Resource *src_resource) override {
    if (!dst_resource || !src_resource)
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_copy_resource(this, dst_resource, src_resource);
    AddRecord(CopyResourceRecord{dst_resource, src_resource});
  }
  void STDMETHODCALLTYPE CopyTiles(ID3D12Resource *tiled_resource,
                                   const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate,
                                   const D3D12_TILE_REGION_SIZE *tile_region_size,
                                   ID3D12Resource *buffer, UINT64 buffer_offset,
                                   D3D12_TILE_COPY_FLAGS flags) override {
    // TODO(d3d12): implement tiled-resource tile copies once reserved resources
    // and tile mappings are represented by the D3D12 resource layer.
    WARN("D3D12GraphicsCommandList: CopyTiles is unsupported");
  }
  void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource *dst_resource, UINT dst_sub_resource,
                                            ID3D12Resource *src_resource, UINT src_sub_resource,
                                            DXGI_FORMAT format) override {
    if (!dst_resource || !src_resource)
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_resolve_subresource(
            this, dst_resource, dst_sub_resource, src_resource, src_sub_resource,
            static_cast<uint32_t>(format));
    ResolveSubresourceRecord record = {};
    record.dst = dst_resource;
    record.dst_subresource = dst_sub_resource;
    record.src = src_resource;
    record.src_subresource = src_sub_resource;
    record.format = format;
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitive_topology) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_ia_set_primitive_topology(
            this, static_cast<uint32_t>(primitive_topology));
    AddRecord(PrimitiveTopologyRecord{primitive_topology});
  }
  void STDMETHODCALLTYPE RSSetViewports(UINT viewport_count, const D3D12_VIEWPORT *viewports) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_rs_set_viewports(this, viewport_count, viewports);
    ViewportRecord record = {};
    if (viewports && viewport_count)
      record.viewports.assign(viewports, viewports + viewport_count);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE RSSetScissorRects(UINT rect_count, const D3D12_RECT *rects) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_rs_set_scissor_rects(this, rect_count, rects);
    ScissorRecord record = {};
    if (rects && rect_count)
      record.rects.assign(rects, rects + rect_count);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT blend_factor[4]) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_om_set_blend_factor(this, blend_factor);
    BlendFactorRecord record = {};
    if (blend_factor)
      std::copy(blend_factor, blend_factor + 4, record.blend_factor.begin());
    AddRecord(record);
  }
  void STDMETHODCALLTYPE OMSetStencilRef(UINT stencil_ref) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_om_set_stencil_ref(this, stencil_ref);
    AddRecord(StencilRefRecord{stencil_ref});
  }
  void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState *pipeline_state) override {
    if (!IsPipelineStateCompatible(pipeline_state))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_pipeline_state(this, pipeline_state);
    current_pipeline_state_ = pipeline_state;
    AddRecord(PipelineStateRecord{current_pipeline_state_});
  }
  void STDMETHODCALLTYPE ResourceBarrier(UINT barrier_count, const D3D12_RESOURCE_BARRIER *barriers) override {
    if (!barriers || !barrier_count)
      return;

    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_resource_barrier(this, barrier_count, barriers);

    ResourceBarrierRecord record = {};
    record.barriers.reserve(barrier_count);
    for (UINT i = 0; i < barrier_count; i++) {
      if (!IsValidResourceBarrier(barriers[i]))
        recording_error_ = E_INVALIDARG;
      record.barriers.push_back(StoreResourceBarrier(barriers[i]));
    }
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *command_list) override {
    auto *bundle = dynamic_cast<GraphicsCommandList *>(command_list);
    if (!bundle || bundle->GetCommandListType() != D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      WARN("D3D12GraphicsCommandList: ExecuteBundle called with non-bundle command list");
      return;
    }
    if (!bundle->IsClosed()) {
      WARN("D3D12GraphicsCommandList: ExecuteBundle called with an open bundle");
      return;
    }
    const auto &bundle_records = bundle->GetCommandRecords();
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_execute_bundle(this, command_list);
    records_.insert(records_.end(), bundle_records.begin(), bundle_records.end());
  }
  void STDMETHODCALLTYPE SetDescriptorHeaps(UINT heap_count, ID3D12DescriptorHeap *const *heaps) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_descriptor_heaps(
            this, heap_count, reinterpret_cast<const void *const *>(heaps));
    DescriptorHeapsRecord record = {};
    if (heaps && heap_count) {
      record.heaps.reserve(heap_count);
      for (UINT i = 0; i < heap_count; i++)
        record.heaps.push_back(heaps[i]);
    }
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature *root_signature) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_signature(this, true, root_signature);
    compute_root_signature_ = root_signature;
    AddRecord(RootSignatureRecord{true, root_signature});
  }

  void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *root_signature) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_signature(this, false, root_signature);
    graphics_root_signature_ = root_signature;
    AddRecord(RootSignatureRecord{false, root_signature});
  }
  void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT root_parameter_index,
                                                       D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor_table(
            this, true, root_parameter_index, base_descriptor);
    AddRecord(RootDescriptorTableRecord{
        true, root_parameter_index, base_descriptor});
  }
  void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT root_parameter_index,
                                                        D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor_table(
            this, false, root_parameter_index, base_descriptor);
    AddRecord(RootDescriptorTableRecord{
        false, root_parameter_index, base_descriptor});
  }
  void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT root_parameter_index, UINT data,
                                                     UINT dst_offset) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_32bit_constants(
            this, true, root_parameter_index, 1, &data, dst_offset);
    RootConstantsRecord record = {};
    record.compute = true;
    record.root_parameter_index = root_parameter_index;
    record.dst_offset = dst_offset;
    record.values.push_back(data);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT root_parameter_index, UINT data,
                                                      UINT dst_offset) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_32bit_constants(
            this, false, root_parameter_index, 1, &data, dst_offset);
    RootConstantsRecord record = {};
    record.compute = false;
    record.root_parameter_index = root_parameter_index;
    record.dst_offset = dst_offset;
    record.values.push_back(data);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT root_parameter_index, UINT constant_count,
                                                      const void *data, UINT dst_offset) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_32bit_constants(
            this, true, root_parameter_index, constant_count,
            static_cast<const UINT *>(data), dst_offset);
    RootConstantsRecord record = {};
    record.compute = true;
    record.root_parameter_index = root_parameter_index;
    record.dst_offset = dst_offset;
    if (data && constant_count) {
      const auto *values = static_cast<const UINT *>(data);
      record.values.assign(values, values + constant_count);
    }
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT root_parameter_index, UINT constant_count,
                                                       const void *data, UINT dst_offset) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_32bit_constants(
            this, false, root_parameter_index, constant_count,
            static_cast<const UINT *>(data), dst_offset);
    RootConstantsRecord record = {};
    record.compute = false;
    record.root_parameter_index = root_parameter_index;
    record.dst_offset = dst_offset;
    if (data && constant_count) {
      const auto *values = static_cast<const UINT *>(data);
      record.values.assign(values, values + constant_count);
    }
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT root_parameter_index,
                                                          D3D12_GPU_VIRTUAL_ADDRESS address) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor(
            this, true, D3D12_ROOT_PARAMETER_TYPE_CBV, root_parameter_index, address);
    AddRecord(RootDescriptorRecord{
        true, D3D12_ROOT_PARAMETER_TYPE_CBV, root_parameter_index, address});
  }
  void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT root_parameter_index,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor(
            this, false, D3D12_ROOT_PARAMETER_TYPE_CBV, root_parameter_index, address);
    AddRecord(RootDescriptorRecord{
        false, D3D12_ROOT_PARAMETER_TYPE_CBV, root_parameter_index, address});
  }
  void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT root_parameter_index,
                                                          D3D12_GPU_VIRTUAL_ADDRESS address) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor(
            this, true, D3D12_ROOT_PARAMETER_TYPE_SRV, root_parameter_index, address);
    AddRecord(RootDescriptorRecord{
        true, D3D12_ROOT_PARAMETER_TYPE_SRV, root_parameter_index, address});
  }
  void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT root_parameter_index,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor(
            this, false, D3D12_ROOT_PARAMETER_TYPE_SRV, root_parameter_index, address);
    AddRecord(RootDescriptorRecord{
        false, D3D12_ROOT_PARAMETER_TYPE_SRV, root_parameter_index, address});
  }
  void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT root_parameter_index,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor(
            this, true, D3D12_ROOT_PARAMETER_TYPE_UAV, root_parameter_index, address);
    AddRecord(RootDescriptorRecord{
        true, D3D12_ROOT_PARAMETER_TYPE_UAV, root_parameter_index, address});
  }
  void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT root_parameter_index,
                                                            D3D12_GPU_VIRTUAL_ADDRESS address) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor(
            this, false, D3D12_ROOT_PARAMETER_TYPE_UAV, root_parameter_index, address);
    AddRecord(RootDescriptorRecord{
        false, D3D12_ROOT_PARAMETER_TYPE_UAV, root_parameter_index, address});
  }
  void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *view) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_ia_set_index_buffer(this, view);
    IndexBufferRecord record = {};
    if (view)
      record.view = *view;
    AddRecord(record);
  }
  void STDMETHODCALLTYPE IASetVertexBuffers(UINT start_slot, UINT view_count,
                                            const D3D12_VERTEX_BUFFER_VIEW *views) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_ia_set_vertex_buffers(
            this, start_slot, view_count, views);
    VertexBuffersRecord record = {};
    record.start_slot = start_slot;
    if (views && view_count)
      record.views.assign(views, views + view_count);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE SOSetTargets(UINT start_slot, UINT view_count,
                                      const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views) override {
    if (view_count && views) {
      // TODO(d3d12): lower stream-output buffer bindings into Metal transform
      // feedback or an emulated write path.
      WARN("D3D12GraphicsCommandList: stream output targets are unsupported");
    }
  }
  void STDMETHODCALLTYPE OMSetRenderTargets(UINT render_target_descriptor_count,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
                                            WINBOOL single_descriptor_handle,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_om_set_render_targets(
            this, render_target_descriptor_count, render_target_descriptors,
            single_descriptor_handle, depth_stencil_descriptor);
    RenderTargetsRecord record = {};
    if (render_target_descriptors && render_target_descriptor_count) {
      record.render_targets.reserve(render_target_descriptor_count);
      auto *base = GetDescriptorRecordFromCpuHandle(render_target_descriptors[0]);
      for (UINT i = 0; i < render_target_descriptor_count; i++) {
        auto *descriptor = single_descriptor_handle
                               ? base + i
                               : GetDescriptorRecordFromCpuHandle(render_target_descriptors[i]);
        if (descriptor)
          record.render_targets.push_back(*descriptor);
      }
    }
    if (depth_stencil_descriptor) {
      if (auto *descriptor = GetDescriptorRecordFromCpuHandle(*depth_stencil_descriptor))
        record.depth_stencil = *descriptor;
    }
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags,
                                               FLOAT depth, UINT8 stencil, UINT rect_count,
                                               const D3D12_RECT *rects) override {
    auto *descriptor = GetDescriptorRecordFromCpuHandle(dsv);
    if (!descriptor)
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_clear_depth_stencil_view(
            this, dsv, flags, depth, stencil, rect_count, rects);
    ClearDepthStencilRecord record = {};
    record.descriptor = *descriptor;
    record.flags = flags;
    record.depth = depth;
    record.stencil = stencil;
    if (rects && rect_count)
      record.rects.assign(rects, rects + rect_count);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4],
                                               UINT rect_count, const D3D12_RECT *rects) override {
    auto *descriptor = GetDescriptorRecordFromCpuHandle(rtv);
    if (!descriptor)
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_clear_render_target_view(
            this, rtv, color, rect_count, rects);
    ClearRenderTargetRecord record = {};
    record.descriptor = *descriptor;
    if (color)
      std::copy(color, color + 4, record.color.begin());
    if (rects && rect_count)
      record.rects.assign(rects, rects + rect_count);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
                                                      D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                                      ID3D12Resource *resource, const UINT values[4],
                                                      UINT rect_count, const D3D12_RECT *rects) override {
    if (!resource || !values)
      return;
    auto *descriptor =
        GetDescriptorRecordFromGpuHandle(gpu_handle,
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!descriptor)
      descriptor =
          GetDescriptorRecordFromCpuHandle(cpu_handle,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!descriptor || descriptor->type != DescriptorRecordType::UnorderedAccessView)
      return;

    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_clear_unordered_access_view_uint(
            this, gpu_handle, cpu_handle, resource, values, rect_count, rects);
    ClearUnorderedAccessRecord record = {};
    record.descriptor = *descriptor;
    record.resource = resource;
    record.integer = true;
    std::copy(values, values + 4, record.uint_values.begin());
    if (rects && rect_count)
      record.rects.assign(rects, rects + rect_count);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
                                                       D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                                       ID3D12Resource *resource, const float values[4],
                                                       UINT rect_count, const D3D12_RECT *rects) override {
    if (!resource || !values)
      return;
    auto *descriptor =
        GetDescriptorRecordFromGpuHandle(gpu_handle,
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!descriptor)
      descriptor =
          GetDescriptorRecordFromCpuHandle(cpu_handle,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!descriptor || descriptor->type != DescriptorRecordType::UnorderedAccessView)
      return;

    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_clear_unordered_access_view_float(
            this, gpu_handle, cpu_handle, resource, values, rect_count, rects);
    ClearUnorderedAccessRecord record = {};
    record.descriptor = *descriptor;
    record.resource = resource;
    std::copy(values, values + 4, record.float_values.begin());
    if (rects && rect_count)
      record.rects.assign(rects, rects + rect_count);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *resource, const D3D12_DISCARD_REGION *region) override {
    if (!resource)
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_discard_resource(
            this, resource, region ? region->FirstSubresource : 0,
            region ? region->NumSubresources : 0,
            region ? region->NumRects : 0,
            region ? region->pRects : nullptr);
    DiscardResourceRecord record = {};
    record.resource = resource;
    if (region) {
      record.first_subresource = region->FirstSubresource;
      record.subresource_count = region->NumSubresources;
      if (region->pRects && region->NumRects)
        record.rects.assign(region->pRects, region->pRects + region->NumRects);
    }
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) override {
    if (!heap)
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_begin_query(
            this, heap, static_cast<uint32_t>(type), index);
    AddRecord(BeginQueryRecord{heap, type, index});
  }
  void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) override {
    if (!heap)
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_end_query(
            this, heap, static_cast<uint32_t>(type), index);
    AddRecord(EndQueryRecord{heap, type, index});
  }
  void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type,
                                          UINT start_index, UINT query_count,
                                          ID3D12Resource *dst_buffer,
                                          UINT64 aligned_dst_buffer_offset) override {
    if (!heap || !dst_buffer || !query_count)
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_resolve_query_data(
            this, heap, static_cast<uint32_t>(type), start_index, query_count,
            dst_buffer, aligned_dst_buffer_offset);
    AddRecord(ResolveQueryDataRecord{
        heap, type, start_index, query_count, dst_buffer,
        aligned_dst_buffer_offset});
  }
  void STDMETHODCALLTYPE SetPredication(ID3D12Resource *buffer, UINT64 aligned_buffer_offset,
                                        D3D12_PREDICATION_OP operation) override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_predication(
            this, buffer, aligned_buffer_offset, static_cast<uint32_t>(operation));
    AddRecord(PredicationRecord{buffer, aligned_buffer_offset, operation});
  }
  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) override {}
  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) override {}
  void STDMETHODCALLTYPE EndEvent() override {}
  void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature *command_signature,
                                         UINT max_command_count, ID3D12Resource *arg_buffer,
                                         UINT64 arg_buffer_offset, ID3D12Resource *count_buffer,
                                         UINT64 count_buffer_offset) override {
    if (!command_signature || !arg_buffer || !max_command_count)
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_execute_indirect(
            this, command_signature, max_command_count, arg_buffer,
            arg_buffer_offset, count_buffer, count_buffer_offset);
    AddRecord(ExecuteIndirectRecord{
        command_signature, max_command_count, arg_buffer, arg_buffer_offset,
        count_buffer, count_buffer_offset});
  }

#ifdef __ID3D12GraphicsCommandList1_INTERFACE_DEFINED__
  void STDMETHODCALLTYPE AtomicCopyBufferUINT(
      ID3D12Resource *dst_buffer, UINT64 dst_offset,
      ID3D12Resource *src_buffer, UINT64 src_offset,
      UINT dependent_resource_count,
      ID3D12Resource *const *dependent_resources,
      const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges) override {
    RecordCopyBufferRegion("AtomicCopyBufferUINT", dst_buffer, dst_offset,
                           src_buffer, src_offset, sizeof(UINT));
  }

  void STDMETHODCALLTYPE AtomicCopyBufferUINT64(
      ID3D12Resource *dst_buffer, UINT64 dst_offset,
      ID3D12Resource *src_buffer, UINT64 src_offset,
      UINT dependent_resource_count,
      ID3D12Resource *const *dependent_resources,
      const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges) override {
    RecordCopyBufferRegion("AtomicCopyBufferUINT64", dst_buffer, dst_offset,
                           src_buffer, src_offset, sizeof(UINT64));
  }

  void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT min, FLOAT max) override {
    depth_bounds_min_ = min;
    depth_bounds_max_ = max;
    WARN("D3D12GraphicsCommandList: depth bounds state recorded but not "
         "applied by Metal");
  }

  void STDMETHODCALLTYPE SetSamplePositions(
      UINT sample_count, UINT pixel_count,
      D3D12_SAMPLE_POSITION *sample_positions) override {
    const bool reset = sample_count == 0 && pixel_count == 0 && !sample_positions;
    if (!reset) {
      WARN("D3D12GraphicsCommandList: programmable sample positions are unsupported");
      return;
    }
  }

  void STDMETHODCALLTYPE ResolveSubresourceRegion(
      ID3D12Resource *dst_resource, UINT dst_sub_resource_idx, UINT dst_x,
      UINT dst_y, ID3D12Resource *src_resource, UINT src_sub_resource_idx,
      D3D12_RECT *src_rect, DXGI_FORMAT format, D3D12_RESOLVE_MODE mode) override {
    if (mode == D3D12_RESOLVE_MODE_DECOMPRESS) {
      WARN("D3D12GraphicsCommandList: ResolveSubresourceRegion decompress mode is unsupported");
      return;
    }
    if (!dst_resource || !src_resource)
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_resolve_subresource_region(
            this, dst_resource, dst_sub_resource_idx, dst_x, dst_y,
            src_resource, src_sub_resource_idx, src_rect,
            static_cast<uint32_t>(format), static_cast<uint32_t>(mode));
    ResolveSubresourceRecord record = {};
    record.dst = dst_resource;
    record.dst_subresource = dst_sub_resource_idx;
    record.dst_x = dst_x;
    record.dst_y = dst_y;
    record.src = src_resource;
    record.src_subresource = src_sub_resource_idx;
    if (src_rect)
      record.src_rect = *src_rect;
    record.format = format;
    record.mode = mode;
    AddRecord(std::move(record));
  }

  void STDMETHODCALLTYPE SetViewInstanceMask(UINT mask) override {
    view_instance_mask_ = mask;
  }
#endif

#ifdef __ID3D12GraphicsCommandList2_INTERFACE_DEFINED__
  void STDMETHODCALLTYPE WriteBufferImmediate(
      UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
      const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes) override {
    if (!count || !parameters)
      return;

    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_write_buffer_immediate(
            this, count, parameters, modes);
    WriteBufferImmediateRecord record = {};
    record.parameters.assign(parameters, parameters + count);
    if (modes)
      record.modes.assign(modes, modes + count);
    AddRecord(std::move(record));
  }
#endif

#ifdef __ID3D12GraphicsCommandList3_INTERFACE_DEFINED__
  void STDMETHODCALLTYPE SetProtectedResourceSession(
      ID3D12ProtectedResourceSession *protected_resource_session) override {
    if (protected_resource_session)
      WARN("D3D12GraphicsCommandList: protected resource sessions are "
           "unsupported");
  }
#endif

#ifdef __ID3D12GraphicsCommandList4_INTERFACE_DEFINED__
  void STDMETHODCALLTYPE BeginRenderPass(
      UINT render_targets_count,
      const D3D12_RENDER_PASS_RENDER_TARGET_DESC *render_targets,
      const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth_stencil,
      D3D12_RENDER_PASS_FLAGS flags) override {
    pending_render_pass_resolves_.clear();
    if (flags & ~(D3D12_RENDER_PASS_FLAG_NONE |
                  D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES)) {
      // TODO(d3d12): model suspended/resumed render passes instead of
      // flattening them into ordinary target binds.
      WARN("D3D12GraphicsCommandList: suspended/resumed render passes are "
           "unsupported");
      return;
    }

    std::vector<dxmt::apitrace::RenderPassRenderTargetDesc> apitrace_render_targets;
    ApitraceRenderPassResolveSubresources apitrace_render_target_resolves;
    if (render_targets && render_targets_count) {
      apitrace_render_targets.reserve(render_targets_count);
      apitrace_render_target_resolves.resize(render_targets_count);
      for (UINT i = 0; i < render_targets_count; i++) {
        apitrace_render_targets.push_back(ToApitraceRenderPassRenderTarget(
            render_targets[i], apitrace_render_target_resolves[i]));
      }
    }
    std::optional<dxmt::apitrace::RenderPassDepthStencilDesc> apitrace_depth_stencil;
    std::array<std::vector<dxmt::apitrace::RenderPassResolveSubresourceDesc>, 2>
        apitrace_depth_stencil_resolves;
    if (depth_stencil) {
      apitrace_depth_stencil = ToApitraceRenderPassDepthStencil(
          *depth_stencil, apitrace_depth_stencil_resolves);
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_begin_render_pass(
            this, static_cast<uint32_t>(apitrace_render_targets.size()),
            apitrace_render_targets.data(),
            apitrace_depth_stencil ? &*apitrace_depth_stencil : nullptr,
            static_cast<uint32_t>(flags));
    RenderTargetsRecord record = {};
    if (render_targets && render_targets_count) {
      record.render_targets.reserve(render_targets_count);
      for (UINT i = 0; i < render_targets_count; i++) {
        if (auto *descriptor =
                GetDescriptorRecordFromCpuHandle(render_targets[i].cpuDescriptor))
          record.render_targets.push_back(*descriptor);
      }
    }
    if (depth_stencil) {
      if (auto *descriptor =
              GetDescriptorRecordFromCpuHandle(depth_stencil->cpuDescriptor))
        record.depth_stencil = *descriptor;
    }
    AddRecord(std::move(record));

    if (render_targets && render_targets_count) {
      for (UINT i = 0; i < render_targets_count; i++)
        AddRenderPassRenderTargetAccess(render_targets[i]);
    }

    if (depth_stencil)
      AddRenderPassDepthStencilAccess(*depth_stencil);
  }

  void STDMETHODCALLTYPE EndRenderPass() override {
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_end_render_pass(this);
    for (const auto &resolve : pending_render_pass_resolves_) {
      ResolveSubresourceRecord record = {};
      record.dst = resolve.dst;
      record.dst_subresource = resolve.dst_subresource;
      record.src = resolve.src;
      record.src_subresource = resolve.src_subresource;
      record.format = resolve.format;
      record.mode = resolve.mode;
      AddRecord(std::move(record));
    }
    pending_render_pass_resolves_.clear();
  }

  void STDMETHODCALLTYPE InitializeMetaCommand(
      ID3D12MetaCommand *meta_command,
      const void *initialization_parameters_data,
      SIZE_T initialization_parameters_data_size_in_bytes) override {
    WARN("D3D12GraphicsCommandList: meta commands are unsupported");
  }

  void STDMETHODCALLTYPE ExecuteMetaCommand(
      ID3D12MetaCommand *meta_command, const void *execution_parameters_data,
      SIZE_T execution_parameters_data_size_in_bytes) override {
    WARN("D3D12GraphicsCommandList: meta commands are unsupported");
  }

  void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc,
      UINT postbuild_info_descs_count,
      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *postbuild_info_descs) override {
    WARN("D3D12GraphicsCommandList: raytracing acceleration structures are "
         "unsupported");
  }

  void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(
      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc,
      UINT src_acceleration_structures_count,
      const D3D12_GPU_VIRTUAL_ADDRESS *src_acceleration_structure_data) override {
    WARN("D3D12GraphicsCommandList: raytracing acceleration structures are "
         "unsupported");
  }

  void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(
      D3D12_GPU_VIRTUAL_ADDRESS dst_acceleration_structure_data,
      D3D12_GPU_VIRTUAL_ADDRESS src_acceleration_structure_data,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) override {
    WARN("D3D12GraphicsCommandList: raytracing acceleration structures are "
         "unsupported");
  }

  void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject *state_object) override {
    if (state_object)
      WARN("D3D12GraphicsCommandList: state objects are unsupported");
  }

  void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC *desc) override {
    WARN("D3D12GraphicsCommandList: ray dispatch is unsupported");
  }
#endif

#ifdef __ID3D12GraphicsCommandList5_INTERFACE_DEFINED__
  void STDMETHODCALLTYPE RSSetShadingRate(
      D3D12_SHADING_RATE base_shading_rate,
      const D3D12_SHADING_RATE_COMBINER *combiners) override {
    if (base_shading_rate != D3D12_SHADING_RATE_1X1 || combiners)
      WARN("D3D12GraphicsCommandList: variable rate shading is unsupported");
  }

  void STDMETHODCALLTYPE RSSetShadingRateImage(
      ID3D12Resource *shading_rate_image) override {
    if (shading_rate_image)
      WARN("D3D12GraphicsCommandList: shading rate images are unsupported");
  }
#endif

#ifdef __ID3D12GraphicsCommandList6_INTERFACE_DEFINED__
  void STDMETHODCALLTYPE DispatchMesh(
      UINT thread_group_count_x,
      UINT thread_group_count_y,
      UINT thread_group_count_z) override {
    WARN("D3D12GraphicsCommandList: mesh shaders are unsupported");
  }
#endif

private:
  ID3D12GraphicsCommandList *AsGraphicsCommandList() {
    return static_cast<ID3D12GraphicsCommandList *>(
        static_cast<GraphicsCommandListComBase *>(this));
  }

  bool IsPipelineStateCompatible(ID3D12PipelineState *pipeline_state) const {
    if (!pipeline_state)
      return true;

    const auto *state = dynamic_cast<PipelineState *>(pipeline_state);
    if (!state)
      return false;

    switch (type_) {
    case D3D12_COMMAND_LIST_TYPE_DIRECT:
      return true;
    case D3D12_COMMAND_LIST_TYPE_BUNDLE:
      return state->GetType() == PipelineStateType::Graphics;
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:
      return state->GetType() == PipelineStateType::Compute;
    default:
      return false;
    }
  }

  void RecordCopyBufferRegion(const char *method, ID3D12Resource *dst_buffer,
                              UINT64 dst_offset, ID3D12Resource *src_buffer,
                              UINT64 src_offset, UINT64 byte_count) {
    if (!dst_buffer || !src_buffer || byte_count == 0)
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_copy_buffer_region(
            method, this, dst_buffer, dst_offset, src_buffer, src_offset,
            byte_count);
    AddRecord(CopyBufferRegionRecord{
        dst_buffer, dst_offset, src_buffer, src_offset, byte_count});
  }

  template <typename T>
  void AddRecord(T &&payload) {
    if (closed_)
      return;
    if (g_current_command_record_d3d_sequence != 0 &&
        std::getenv("DXMT_APITRACE_DIAG") &&
        g_apitrace_record_diag_log_count.fetch_add(1, std::memory_order_relaxed) < 128) {
      WARN("DXMT apitrace d3d:",
           " event=record_command",
           " seq=", g_current_command_record_d3d_sequence);
    }
    records_.push_back(CommandRecord{
        g_current_command_record_d3d_sequence, std::forward<T>(payload)});
  }

#ifdef __ID3D12GraphicsCommandList4_INTERFACE_DEFINED__
  void AddRenderPassRenderTargetAccess(
      const D3D12_RENDER_PASS_RENDER_TARGET_DESC &render_target) {
    auto *descriptor = GetDescriptorRecordFromCpuHandle(render_target.cpuDescriptor);
    if (!descriptor)
      return;

    switch (render_target.BeginningAccess.Type) {
    case D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR: {
      ClearRenderTargetRecord clear = {};
      clear.descriptor = *descriptor;
      std::copy(std::begin(render_target.BeginningAccess.Clear.ClearValue.Color),
                std::end(render_target.BeginningAccess.Clear.ClearValue.Color),
                clear.color.begin());
      AddRecord(std::move(clear));
      break;
    }
    case D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD:
      AddRenderPassDiscard(*descriptor);
      break;
    default:
      if (!IsRenderPassPreserveOrNoAccess(render_target.BeginningAccess.Type))
        WARN("D3D12GraphicsCommandList: unsupported render pass RTV beginning "
             "access type ",
             render_target.BeginningAccess.Type);
      break;
    }

    AddRenderPassEndingAccess(render_target.EndingAccess);
  }

  void AddRenderPassDepthStencilAccess(
      const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC &depth_stencil) {
    auto *descriptor = GetDescriptorRecordFromCpuHandle(depth_stencil.cpuDescriptor);
    if (!descriptor)
      return;

    ClearDepthStencilRecord clear = {};
    clear.descriptor = *descriptor;
    clear.flags = D3D12_CLEAR_FLAGS(0);
    if (depth_stencil.DepthBeginningAccess.Type ==
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
      clear.flags =
          static_cast<D3D12_CLEAR_FLAGS>(clear.flags | D3D12_CLEAR_FLAG_DEPTH);
      clear.depth =
          depth_stencil.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth;
    } else if (depth_stencil.DepthBeginningAccess.Type ==
               D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD) {
      AddRenderPassDiscard(*descriptor);
    } else if (!IsRenderPassPreserveOrNoAccess(
                   depth_stencil.DepthBeginningAccess.Type)) {
      WARN("D3D12GraphicsCommandList: unsupported render pass depth beginning "
           "access type ",
           depth_stencil.DepthBeginningAccess.Type);
    }

    if (depth_stencil.StencilBeginningAccess.Type ==
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
      clear.flags =
          static_cast<D3D12_CLEAR_FLAGS>(clear.flags | D3D12_CLEAR_FLAG_STENCIL);
      clear.stencil =
          depth_stencil.StencilBeginningAccess.Clear.ClearValue.DepthStencil.Stencil;
    } else if (depth_stencil.StencilBeginningAccess.Type ==
               D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD) {
      AddRenderPassDiscard(*descriptor);
    } else if (!IsRenderPassPreserveOrNoAccess(
                   depth_stencil.StencilBeginningAccess.Type)) {
      WARN("D3D12GraphicsCommandList: unsupported render pass stencil "
           "beginning access type ",
           depth_stencil.StencilBeginningAccess.Type);
    }

    if (clear.flags != D3D12_CLEAR_FLAGS(0))
      AddRecord(std::move(clear));

    AddRenderPassEndingAccess(depth_stencil.DepthEndingAccess);
    AddRenderPassEndingAccess(depth_stencil.StencilEndingAccess);
  }

  void AddRenderPassDiscard(const DescriptorRecord &descriptor) {
    if (!descriptor.resource)
      return;
    DiscardResourceRecord discard = {};
    discard.resource = descriptor.resource.ptr();
    AddRecord(std::move(discard));
  }

  void AddRenderPassEndingAccess(
      const D3D12_RENDER_PASS_ENDING_ACCESS &ending_access) {
    switch (ending_access.Type) {
    case D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE:
      AddRenderPassResolve(ending_access.Resolve);
      break;
    case D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD:
      // D3D12 render-pass discard only says prior contents do not need to be
      // preserved after the pass. The current backend has no end-store action,
      // so this remains a conservative no-op.
      break;
    default:
      if (!IsRenderPassPreserveOrNoAccess(ending_access.Type))
        WARN("D3D12GraphicsCommandList: unsupported render pass ending access "
             "type ",
             ending_access.Type);
      break;
    }
  }

  void AddRenderPassResolve(
      const D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_PARAMETERS &resolve) {
    if (!resolve.pSrcResource || !resolve.pDstResource ||
        !resolve.SubresourceCount || !resolve.pSubresourceParameters)
      return;

    if (resolve.ResolveMode == D3D12_RESOLVE_MODE_DECOMPRESS ||
        !resolve.PreserveResolveSource) {
      WARN("D3D12GraphicsCommandList: render pass decompress resolve or source discard is unsupported");
      return;
    }

    for (UINT i = 0; i < resolve.SubresourceCount; i++) {
      const auto &subresource = resolve.pSubresourceParameters[i];
      if (subresource.DstX || subresource.DstY ||
          !IsFullSubresourceRect(resolve.pSrcResource,
                                 subresource.SrcSubresource,
                                 subresource.SrcRect)) {
        // TODO(d3d12): lower partial render-pass resolves once
        // ResolveSubresourceRegion has real region support.
        WARN("D3D12GraphicsCommandList: partial render pass resolves are "
             "unsupported");
        continue;
      }
      pending_render_pass_resolves_.push_back(PendingRenderPassResolve{
          resolve.pSrcResource, resolve.pDstResource,
          subresource.SrcSubresource, subresource.DstSubresource,
          resolve.Format, resolve.ResolveMode});
    }
  }
#endif

  Com<IMTLD3D12Device> device_;
  UINT node_mask_;
  D3D12_COMMAND_LIST_TYPE type_;
  Com<CommandAllocatorObject, false> allocator_;
  Com<ID3D12PipelineState> initial_pipeline_state_;
  Com<ID3D12PipelineState> current_pipeline_state_;
  Com<ID3D12RootSignature> compute_root_signature_;
  Com<ID3D12RootSignature> graphics_root_signature_;
  ComPrivateData private_data_;
  std::vector<CommandRecord> records_;
  FLOAT depth_bounds_min_ = 0.0f;
  FLOAT depth_bounds_max_ = 1.0f;
  UINT view_instance_mask_ = 0xffffffffu;
  std::vector<PendingRenderPassResolve> pending_render_pass_resolves_;
  bool closed_ = false;
  bool submitted_ = false;
  bool apitrace_lifecycle_recording_enabled_ = true;
  HRESULT recording_error_ = S_OK;
  std::string name_;
};

class CommandSignatureImpl final
    : public ComObjectWithInitialRef<ID3D12CommandSignature>,
      public CommandSignature {
public:
  CommandSignatureImpl(IMTLD3D12Device *device,
                       const D3D12_COMMAND_SIGNATURE_DESC &desc,
                       ID3D12RootSignature *root_signature)
      : device_(device), root_signature_(root_signature), desc_(desc) {
    arguments_.assign(desc.pArgumentDescs,
                      desc.pArgumentDescs + desc.NumArgumentDescs);
    desc_.pArgumentDescs = arguments_.empty() ? nullptr : arguments_.data();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12CommandSignature)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12CommandSignature), riid))
      WARN("D3D12CommandSignature: unknown interface query ",
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

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,
                                                   const IUnknown *data) override {
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  const D3D12_COMMAND_SIGNATURE_DESC &GetDesc() const override {
    return desc_;
  }

  const std::vector<D3D12_INDIRECT_ARGUMENT_DESC> &GetArguments() const override {
    return arguments_;
  }

  ID3D12RootSignature *GetRootSignature() const override {
    return root_signature_.ptr();
  }

private:
  Com<IMTLD3D12Device> device_;
  Com<ID3D12RootSignature> root_signature_;
  ComPrivateData private_data_;
  D3D12_COMMAND_SIGNATURE_DESC desc_ = {};
  std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments_;
  std::string name_;
};

} // namespace

Com<ID3D12GraphicsCommandList>
CreateGraphicsCommandList(IMTLD3D12Device *device, UINT node_mask,
                          D3D12_COMMAND_LIST_TYPE type,
                          ID3D12CommandAllocator *command_allocator,
                          ID3D12PipelineState *initial_pipeline_state,
                          HRESULT *status) {
  auto *list = new GraphicsCommandListImpl(
      device, node_mask, type, command_allocator, initial_pipeline_state, status);
  if (status && FAILED(*status)) {
    list->ReleasePrivate();
    return nullptr;
  }
  return Com<ID3D12GraphicsCommandList>::transfer(list);
}

Com<ID3D12CommandSignature>
CreateCommandSignature(IMTLD3D12Device *device,
                       const D3D12_COMMAND_SIGNATURE_DESC *desc,
                       ID3D12RootSignature *root_signature) {
  return Com<ID3D12CommandSignature>::transfer(
      new CommandSignatureImpl(device, *desc, root_signature));
}

} // namespace dxmt::d3d12
