#include "d3d12_replay_blit_hazard.hpp"

namespace dxmt::d3d12 {

bool
ReplayBlitBatchHasHazard(const ReplayBlitBatch &batch,
                         std::initializer_list<ID3D12Resource *> reads,
                         std::initializer_list<ID3D12Resource *> writes) {
  for (auto *resource : reads) {
    if (resource && batch.writes.count(resource))
      return true;
  }
  for (auto *resource : writes) {
    if (!resource)
      continue;
    if (batch.reads.count(resource) || batch.writes.count(resource))
      return true;
  }
  return false;
}

void
ReplayBlitBatchTrackAccess(ReplayBlitBatch &batch,
                           std::initializer_list<ID3D12Resource *> reads,
                           std::initializer_list<ID3D12Resource *> writes) {
  for (auto *resource : reads) {
    if (resource)
      batch.reads.insert(resource);
  }
  for (auto *resource : writes) {
    if (resource)
      batch.writes.insert(resource);
  }
}

} // namespace dxmt::d3d12
