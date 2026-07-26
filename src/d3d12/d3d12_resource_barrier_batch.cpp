#include "d3d12_resource_barrier_batch.hpp"

#include <algorithm>
#include <functional>

namespace dxmt::d3d12 {

namespace {

constexpr size_t kHashMixConstant = 0x9e3779b97f4a7c15ull;

void MixBarrierKeyHash(size_t &hash, size_t value) {
  hash ^= (value + kHashMixConstant + (hash << 6) + (hash >> 2));
}

} // namespace

size_t
ResourceAccessBarrierKeyHash::operator()(
    const ResourceAccessBarrierKey &key) const {
  size_t hash = std::hash<uintptr_t>{}(key.object);
  MixBarrierKeyHash(hash, std::hash<uintptr_t>{}(key.d3d_resource));
  MixBarrierKeyHash(hash, size_t(key.level));
  MixBarrierKeyHash(hash, size_t(key.slice));
  MixBarrierKeyHash(hash, size_t(key.texture));
  return hash;
}

ResourceAccessBarrierKey
MakeResourceAccessBarrierKey(const ResourceAccessBarrierEntry &entry) {
  ResourceAccessBarrierKey key = {};
  key.d3d_resource = reinterpret_cast<uintptr_t>(entry.d3d_resource.ptr());
  if (entry.buffer) {
    key.object = reinterpret_cast<uintptr_t>(entry.buffer.ptr());
    return key;
  }
  key.object = reinterpret_cast<uintptr_t>(entry.texture.ptr());
  key.level = entry.level;
  key.slice = entry.slice;
  key.texture = true;
  return key;
}

bool ResourceAccessBarrierEntriesMatch(
    const ResourceAccessBarrierEntry &lhs,
    const ResourceAccessBarrierEntry &rhs) {
  if (lhs.buffer || rhs.buffer)
    return lhs.d3d_resource.ptr() == rhs.d3d_resource.ptr() &&
           lhs.buffer.ptr() == rhs.buffer.ptr();
  return lhs.d3d_resource.ptr() == rhs.d3d_resource.ptr() &&
         lhs.texture.ptr() == rhs.texture.ptr() && lhs.level == rhs.level &&
         lhs.slice == rhs.slice;
}

void MergeResourceAccessBarrierEntry(ResourceAccessBarrierEntry &dst,
                                     ResourceAccessBarrierEntry &&src) {
  dst.buffer_length = std::max(dst.buffer_length, src.buffer_length);
  dst.access |= src.access;
  dst.requires_cross_submit_wait =
      dst.requires_cross_submit_wait || src.requires_cross_submit_wait;
}

void RebuildResourceAccessBarrierIndex(ResourceAccessBarrierBatch &batch) {
  batch.entry_index.clear();
  batch.entry_index.reserve(batch.entries.size());
  batch.resource_entry_keys.clear();
  for (size_t i = 0; i < batch.entries.size(); i++) {
    auto key = MakeResourceAccessBarrierKey(batch.entries[i]);
    batch.entry_index.emplace(key, i);
    if (auto *resource = batch.entries[i].d3d_resource.ptr())
      batch.resource_entry_keys[resource].push_back(key);
  }
}

} // namespace dxmt::d3d12
