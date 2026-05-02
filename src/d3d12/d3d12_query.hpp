#pragma once

#include "d3d12_device.hpp"
#include "com/com_pointer.hpp"
#include "dxmt_occlusion_query.hpp"
#include <d3d12.h>
#include <vector>

namespace dxmt::d3d12 {

class QueryHeap;

class QueryHeap {
public:
  virtual ~QueryHeap() = default;

  virtual const D3D12_QUERY_HEAP_DESC &GetDesc() const = 0;
  virtual Rc<VisibilityResultQuery> BeginVisibility(D3D12_QUERY_TYPE type,
                                                    UINT index) = 0;
  virtual Rc<VisibilityResultQuery> EndVisibility(D3D12_QUERY_TYPE type,
                                                  UINT index) = 0;
  virtual Rc<TimestampQuery> EndTimestamp(D3D12_QUERY_TYPE type,
                                          UINT index) = 0;
  virtual bool BeginStatistics(D3D12_QUERY_TYPE type, UINT index) = 0;
  virtual bool EndStatistics(D3D12_QUERY_TYPE type, UINT index) = 0;
  virtual bool Resolve(D3D12_QUERY_TYPE type, UINT start_index,
                       UINT query_count, std::vector<uint8_t> &data) const = 0;
};

Com<ID3D12QueryHeap>
CreateQueryHeap(IMTLD3D12Device *device, const D3D12_QUERY_HEAP_DESC *desc);

} // namespace dxmt::d3d12
