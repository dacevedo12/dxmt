#pragma once

#include "d3d12_command_allocator.hpp"
#include "d3d12_pipeline.hpp"
#include <d3d12.h>

namespace dxmt::d3d12 {

class GraphicsCommandList {
public:
  virtual ~GraphicsCommandList() = default;

  virtual bool IsClosed() const = 0;
  virtual D3D12_COMMAND_LIST_TYPE GetCommandListType() const = 0;
  virtual HRESULT MarkSubmittedToQueue(D3D12_COMMAND_LIST_TYPE queue_type) = 0;
};

Com<ID3D12GraphicsCommandList>
CreateGraphicsCommandList(IMTLD3D12Device *device, UINT node_mask,
                          D3D12_COMMAND_LIST_TYPE type,
                          ID3D12CommandAllocator *command_allocator,
                          ID3D12PipelineState *initial_pipeline_state);

} // namespace dxmt::d3d12
