#include "d3d12_command_list.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "log/log.hpp"
#include "util_string.hpp"

namespace dxmt::d3d12 {
namespace {

class GraphicsCommandListImpl final : public ComObjectWithInitialRef<ID3D12GraphicsCommandList>,
                                      public GraphicsCommandList {
public:
  GraphicsCommandListImpl(IMTLD3D12Device *device, UINT node_mask,
                          D3D12_COMMAND_LIST_TYPE type,
                          ID3D12CommandAllocator *command_allocator,
                          ID3D12PipelineState *initial_pipeline_state)
      : device_(device), node_mask_(node_mask), type_(type), allocator_(command_allocator),
        initial_pipeline_state_(initial_pipeline_state) {
    if (allocator_) {
      auto allocator_state = dynamic_cast<CommandAllocator *>(allocator_.ptr());
      if (!allocator_state || allocator_state->GetCommandListType() != type_)
        allocator_ = nullptr;
    }
    if (!IsPipelineStateCompatible(initial_pipeline_state_.ptr()))
      initial_pipeline_state_ = nullptr;
    current_pipeline_state_ = initial_pipeline_state_;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12CommandList) ||
        riid == __uuidof(ID3D12GraphicsCommandList)) {
      *ppvObject = ref(this);
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
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return type_; }

  HRESULT STDMETHODCALLTYPE Close() override {
    if (closed_)
      return E_FAIL;
    closed_ = true;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *allocator,
                                  ID3D12PipelineState *initial_state) override {
    if (!allocator)
      return E_INVALIDARG;

    auto allocator_state = dynamic_cast<CommandAllocator *>(allocator);
    if (!allocator_state || allocator_state->GetCommandListType() != type_)
      return E_INVALIDARG;
    if (!IsPipelineStateCompatible(initial_state))
      return E_INVALIDARG;

    allocator_ = allocator;
    initial_pipeline_state_ = initial_state;
    current_pipeline_state_ = initial_pipeline_state_;
    compute_root_signature_ = nullptr;
    graphics_root_signature_ = nullptr;
    closed_ = false;
    submitted_ = false;
    return S_OK;
  }

  bool IsClosed() const override { return closed_; }

  D3D12_COMMAND_LIST_TYPE GetCommandListType() const override { return type_; }

  HRESULT MarkSubmittedToQueue(D3D12_COMMAND_LIST_TYPE queue_type) override {
    if (!closed_ || type_ != queue_type)
      return E_INVALIDARG;

    submitted_ = true;
    return S_OK;
  }

  void STDMETHODCALLTYPE ClearState(ID3D12PipelineState *pipeline_state) override {
    if (!IsPipelineStateCompatible(pipeline_state))
      return;
    current_pipeline_state_ = pipeline_state;
  }
  void STDMETHODCALLTYPE DrawInstanced(UINT vertex_count_per_instance, UINT instance_count,
                                       UINT start_vertex_location, UINT start_instance_location) override {}
  void STDMETHODCALLTYPE DrawIndexedInstanced(UINT index_count_per_instance, UINT instance_count,
                                              UINT start_vertex_location, INT base_vertex_location,
                                              UINT start_instance_location) override {}
  void STDMETHODCALLTYPE Dispatch(UINT x, UINT u, UINT z) override {}
  void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource *dst_buffer, UINT64 dst_offset,
                                          ID3D12Resource *src_buffer, UINT64 src_offset,
                                          UINT64 byte_count) override {}
  void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, UINT dst_x,
                                           UINT dst_y, UINT dst_z,
                                           const D3D12_TEXTURE_COPY_LOCATION *src,
                                           const D3D12_BOX *src_box) override {}
  void STDMETHODCALLTYPE CopyResource(ID3D12Resource *dst_resource,
                                      ID3D12Resource *src_resource) override {}
  void STDMETHODCALLTYPE CopyTiles(ID3D12Resource *tiled_resource,
                                   const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate,
                                   const D3D12_TILE_REGION_SIZE *tile_region_size,
                                   ID3D12Resource *buffer, UINT64 buffer_offset,
                                   D3D12_TILE_COPY_FLAGS flags) override {}
  void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource *dst_resource, UINT dst_sub_resource,
                                            ID3D12Resource *src_resource, UINT src_sub_resource,
                                            DXGI_FORMAT format) override {}
  void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitive_topology) override {}
  void STDMETHODCALLTYPE RSSetViewports(UINT viewport_count, const D3D12_VIEWPORT *viewports) override {}
  void STDMETHODCALLTYPE RSSetScissorRects(UINT rect_count, const D3D12_RECT *rects) override {}
  void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT blend_factor[4]) override {}
  void STDMETHODCALLTYPE OMSetStencilRef(UINT stencil_ref) override {}
  void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState *pipeline_state) override {
    if (!IsPipelineStateCompatible(pipeline_state))
      return;
    current_pipeline_state_ = pipeline_state;
  }
  void STDMETHODCALLTYPE ResourceBarrier(UINT barrier_count, const D3D12_RESOURCE_BARRIER *barriers) override {}
  void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *command_list) override {}
  void STDMETHODCALLTYPE SetDescriptorHeaps(UINT heap_count, ID3D12DescriptorHeap *const *heaps) override {}
  void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature *root_signature) override {
    compute_root_signature_ = root_signature;
  }

  void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *root_signature) override {
    graphics_root_signature_ = root_signature;
  }
  void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT root_parameter_index,
                                                       D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) override {}
  void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT root_parameter_index,
                                                        D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) override {}
  void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT root_parameter_index, UINT data,
                                                     UINT dst_offset) override {}
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT root_parameter_index, UINT data,
                                                      UINT dst_offset) override {}
  void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT root_parameter_index, UINT constant_count,
                                                      const void *data, UINT dst_offset) override {}
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT root_parameter_index, UINT constant_count,
                                                       const void *data, UINT dst_offset) override {}
  void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT root_parameter_index,
                                                          D3D12_GPU_VIRTUAL_ADDRESS address) override {}
  void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT root_parameter_index,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address) override {}
  void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT root_parameter_index,
                                                          D3D12_GPU_VIRTUAL_ADDRESS address) override {}
  void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT root_parameter_index,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address) override {}
  void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT root_parameter_index,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address) override {}
  void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT root_parameter_index,
                                                            D3D12_GPU_VIRTUAL_ADDRESS address) override {}
  void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *view) override {}
  void STDMETHODCALLTYPE IASetVertexBuffers(UINT start_slot, UINT view_count,
                                            const D3D12_VERTEX_BUFFER_VIEW *views) override {}
  void STDMETHODCALLTYPE SOSetTargets(UINT start_slot, UINT view_count,
                                      const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views) override {}
  void STDMETHODCALLTYPE OMSetRenderTargets(UINT render_target_descriptor_count,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
                                            WINBOOL single_descriptor_handle,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor) override {}
  void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags,
                                               FLOAT depth, UINT8 stencil, UINT rect_count,
                                               const D3D12_RECT *rects) override {}
  void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4],
                                               UINT rect_count, const D3D12_RECT *rects) override {}
  void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
                                                      D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                                      ID3D12Resource *resource, const UINT values[4],
                                                      UINT rect_count, const D3D12_RECT *rects) override {}
  void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
                                                       D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                                       ID3D12Resource *resource, const float values[4],
                                                       UINT rect_count, const D3D12_RECT *rects) override {}
  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *resource, const D3D12_DISCARD_REGION *region) override {}
  void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) override {}
  void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) override {}
  void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type,
                                          UINT start_index, UINT query_count,
                                          ID3D12Resource *dst_buffer,
                                          UINT64 aligned_dst_buffer_offset) override {}
  void STDMETHODCALLTYPE SetPredication(ID3D12Resource *buffer, UINT64 aligned_buffer_offset,
                                        D3D12_PREDICATION_OP operation) override {}
  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) override {}
  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) override {}
  void STDMETHODCALLTYPE EndEvent() override {}
  void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature *command_signature,
                                         UINT max_command_count, ID3D12Resource *arg_buffer,
                                         UINT64 arg_buffer_offset, ID3D12Resource *count_buffer,
                                         UINT64 count_buffer_offset) override {}

private:
  bool IsPipelineStateCompatible(ID3D12PipelineState *pipeline_state) const {
    if (!pipeline_state)
      return true;

    const auto *state = dynamic_cast<PipelineState *>(pipeline_state);
    if (!state)
      return false;

    switch (type_) {
    case D3D12_COMMAND_LIST_TYPE_DIRECT:
    case D3D12_COMMAND_LIST_TYPE_BUNDLE:
      return true;
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:
      return state->GetType() == PipelineStateType::Compute;
    default:
      return false;
    }
  }

  Com<IMTLD3D12Device> device_;
  UINT node_mask_;
  D3D12_COMMAND_LIST_TYPE type_;
  Com<ID3D12CommandAllocator> allocator_;
  Com<ID3D12PipelineState> initial_pipeline_state_;
  Com<ID3D12PipelineState> current_pipeline_state_;
  Com<ID3D12RootSignature> compute_root_signature_;
  Com<ID3D12RootSignature> graphics_root_signature_;
  ComPrivateData private_data_;
  bool closed_ = false;
  bool submitted_ = false;
  std::string name_;
};

} // namespace

Com<ID3D12GraphicsCommandList>
CreateGraphicsCommandList(IMTLD3D12Device *device, UINT node_mask,
                          D3D12_COMMAND_LIST_TYPE type,
                          ID3D12CommandAllocator *command_allocator,
                          ID3D12PipelineState *initial_pipeline_state) {
  return Com<ID3D12GraphicsCommandList>::transfer(new GraphicsCommandListImpl(
      device, node_mask, type, command_allocator, initial_pipeline_state));
}

} // namespace dxmt::d3d12
