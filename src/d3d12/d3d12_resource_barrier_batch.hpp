#pragma once

#include "d3d12_resource.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dxmt::d3d12 {

struct ResourceAccessBarrierEntry {
  Com<ID3D12Resource> d3d_resource;
  Rc<Buffer> buffer;
  Rc<Texture> texture;
  UINT64 buffer_length = 0;
  UINT level = 0;
  UINT slice = 0;
  int access = 0;
  bool requires_cross_submit_wait = false;
};

struct ResourceAccessBarrierKey {
  uintptr_t object = 0;
  uintptr_t d3d_resource = 0;
  UINT level = 0;
  UINT slice = 0;
  bool texture = false;

  bool operator==(const ResourceAccessBarrierKey &other) const {
    return object == other.object && d3d_resource == other.d3d_resource &&
           level == other.level && slice == other.slice &&
           texture == other.texture;
  }
};

struct ResourceAccessBarrierKeyHash {
  size_t operator()(const ResourceAccessBarrierKey &key) const;
};

struct ResourceAccessBarrierBatch {
  std::vector<ResourceAccessBarrierEntry> entries;
  std::unordered_map<ResourceAccessBarrierKey, size_t,
                     ResourceAccessBarrierKeyHash>
      entry_index;
  // Stable keys keep resource lookups valid while entries are swap-removed.
  std::unordered_map<ID3D12Resource *, std::vector<ResourceAccessBarrierKey>>
      resource_entry_keys;
  bool needs_separator = false;
};

inline constexpr size_t kResourceAccessBarrierLinearEntryLimit = 32;

[[nodiscard]] ResourceAccessBarrierKey
MakeResourceAccessBarrierKey(const ResourceAccessBarrierEntry &entry);

[[nodiscard]] bool
ResourceAccessBarrierEntriesMatch(const ResourceAccessBarrierEntry &lhs,
                                  const ResourceAccessBarrierEntry &rhs);

void MergeResourceAccessBarrierEntry(ResourceAccessBarrierEntry &dst,
                                     ResourceAccessBarrierEntry &&src);

void RebuildResourceAccessBarrierIndex(ResourceAccessBarrierBatch &batch);

} // namespace dxmt::d3d12
