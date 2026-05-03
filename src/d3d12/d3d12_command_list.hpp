#pragma once

#include "d3d12_command_allocator.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_pipeline.hpp"
#include "com/com_pointer.hpp"
#include <d3d12.h>
#include <array>
#include <optional>
#include <unordered_map>
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

struct ResolveSubresourceRecord {
  Com<ID3D12Resource> dst;
  UINT dst_subresource = 0;
  Com<ID3D12Resource> src;
  UINT src_subresource = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct PendingRenderPassResolve {
  Com<ID3D12Resource> src;
  Com<ID3D12Resource> dst;
  UINT src_subresource = 0;
  UINT dst_subresource = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct WriteBufferImmediateRecord {
  std::vector<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER> parameters;
  std::vector<D3D12_WRITEBUFFERIMMEDIATE_MODE> modes;
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

using CommandRecordPayload = std::variant<
    DrawInstancedRecord, DrawIndexedInstancedRecord, DispatchRecord,
    PipelineStateRecord, CopyBufferRegionRecord, CopyTextureRegionRecord,
    CopyResourceRecord, ResolveSubresourceRecord, ResourceBarrierRecord,
    ClearRenderTargetRecord, ClearDepthStencilRecord,
    ClearUnorderedAccessRecord, DiscardResourceRecord, ViewportRecord,
    ScissorRecord, BlendFactorRecord, StencilRefRecord, PrimitiveTopologyRecord,
    RenderTargetsRecord, VertexBuffersRecord, IndexBufferRecord,
    RootSignatureRecord, DescriptorHeapsRecord, RootDescriptorTableRecord,
    RootConstantsRecord, RootDescriptorRecord, BeginQueryRecord,
    EndQueryRecord, ResolveQueryDataRecord, PredicationRecord,
    WriteBufferImmediateRecord,
    ExecuteIndirectRecord>;

struct CommandRecord {
  CommandRecordPayload payload;
};

class GraphicsCommandList {
public:
  virtual ~GraphicsCommandList() = default;

  virtual bool IsClosed() const = 0;
  virtual D3D12_COMMAND_LIST_TYPE GetCommandListType() const = 0;
  virtual const std::vector<CommandRecord> &GetCommandRecords() const = 0;
  virtual HRESULT MarkSubmittedToQueue(D3D12_COMMAND_LIST_TYPE queue_type) = 0;
};

class CommandSignature {
public:
  virtual ~CommandSignature() = default;
  virtual const D3D12_COMMAND_SIGNATURE_DESC &GetDesc() const = 0;
  virtual const std::vector<D3D12_INDIRECT_ARGUMENT_DESC> &GetArguments() const = 0;
  virtual ID3D12RootSignature *GetRootSignature() const = 0;
};

Com<ID3D12GraphicsCommandList>
CreateGraphicsCommandList(IMTLD3D12Device *device, UINT node_mask,
                          D3D12_COMMAND_LIST_TYPE type,
                          ID3D12CommandAllocator *command_allocator,
                          ID3D12PipelineState *initial_pipeline_state);

Com<ID3D12CommandSignature>
CreateCommandSignature(IMTLD3D12Device *device,
                       const D3D12_COMMAND_SIGNATURE_DESC *desc,
                       ID3D12RootSignature *root_signature);

} // namespace dxmt::d3d12
