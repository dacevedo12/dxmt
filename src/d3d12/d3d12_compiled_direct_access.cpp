#include "d3d12_compiled_direct_access.hpp"

#include "dxmt_command_queue.hpp"

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

namespace dxmt::d3d12 {

namespace {

// The frozen store owns, beyond its retained resources, the descriptor table,
// buffer record, buffer resource table and root base buffers plus the root base
// residency handle.
constexpr size_t kFrozenNativeStoreFixedOwnerCount = 5;

void
PublishCompiledDirectAllocation(ArgumentEncodingContext &enc,
                                dxmt::CommandQueue &queue,
                                const Rc<BufferAllocation> &allocation) {
  if (!allocation)
    return;
  allocation->ensureLifetimeResidency(
      queue, WMT::Object{allocation->buffer().handle});
  if (!enc.retainAllocation(allocation.ptr()))
    return;
}

template <PipelineStage Stage>
void
PublishCompiledDirectAccess(
    ArgumentEncodingContext &enc,
    const CompiledDirectAccessList::EncoderAccess &access) {
  switch (access.kind) {
  case CompiledDirectAccessList::Kind::BufferRange:
    if (access.buffer && access.length)
      enc.access<Stage>(access.buffer, access.offset, access.length,
                        access.flags);
    break;
  case CompiledDirectAccessList::Kind::BufferView:
    if (access.buffer && access.view_id)
      enc.access<Stage>(access.buffer, access.view_id, access.flags);
    break;
  case CompiledDirectAccessList::Kind::TextureView:
    if (access.texture && access.view_id)
      enc.access<Stage>(access.texture, access.view_id, access.flags);
    break;
  }
}

void
PublishCompiledDirectAccessStage(
    ArgumentEncodingContext &enc, const CompiledDirectAccessList &list,
    PipelineStage stage, CompiledCommandTestTelemetry *test_telemetry) {
  for (const auto &access : list.encoder_accesses) {
    if (access.stage != stage)
      continue;
    /*
     * Resource hazards are command-ordered state, not encoder binding
     * state. Re-publishing a packet whose immutable bindings are unchanged
     * is required to observe intervening UAV writes and stage transitions.
     *
     * Render stages are published in pipeline execution order below.
     * Descriptor enumeration order is unrelated to execution order and
     * publishing Pixel before Vertex can manufacture a false
     * Fragment->PreRaster dependency inside one draw.
     */
    const EncoderResourcePlanKey key = {
        reinterpret_cast<uintptr_t>(
            access.buffer ? static_cast<const void *>(access.buffer.ptr())
                          : static_cast<const void *>(access.texture.ptr())),
        access.offset,
        access.length,
        access.view_id,
        static_cast<uint8_t>(access.stage),
        static_cast<uint8_t>(access.kind)};
    auto &published =
        access.stage == PipelineStage::Compute
            ? static_cast<ComputeEncoderData *>(enc.currentEncoder())
                  ->resource_plan_accesses
            : enc.currentRenderEncoder()->resource_plan_accesses;
    auto [published_it, inserted] = published.try_emplace(key, access.flags);
    if (!inserted) {
      published_it->second |= access.flags;
      if (test_telemetry)
        test_telemetry->encoder_resource_plan_reuses.fetch_add(
            1, std::memory_order_relaxed);
    }
    if (test_telemetry)
      test_telemetry->encoder_resource_plan_publications.fetch_add(
          1, std::memory_order_relaxed);
    switch (access.stage) {
    case PipelineStage::Compute:
      PublishCompiledDirectAccess<PipelineStage::Compute>(enc, access);
      break;
    case PipelineStage::Pixel:
      PublishCompiledDirectAccess<PipelineStage::Pixel>(enc, access);
      break;
    case PipelineStage::Geometry:
      PublishCompiledDirectAccess<PipelineStage::Geometry>(enc, access);
      break;
    case PipelineStage::Hull:
      PublishCompiledDirectAccess<PipelineStage::Hull>(enc, access);
      break;
    case PipelineStage::Domain:
      PublishCompiledDirectAccess<PipelineStage::Domain>(enc, access);
      break;
    case PipelineStage::Vertex:
    default:
      PublishCompiledDirectAccess<PipelineStage::Vertex>(enc, access);
      break;
    }
  }
}

} // namespace

void
AddCompiledDirectEncoderAccess(
    CompiledDirectAccessList &list,
    CompiledDirectAccessList::EncoderAccess access) {
  for (auto &existing : list.encoder_accesses) {
    if (existing.stage != access.stage || existing.kind != access.kind ||
        existing.buffer.ptr() != access.buffer.ptr() ||
        existing.texture.ptr() != access.texture.ptr() ||
        existing.offset != access.offset || existing.length != access.length ||
        existing.view_id != access.view_id)
      continue;
    existing.flags |= access.flags;
    return;
  }
  list.encoder_accesses.push_back(std::move(access));
}

bool
CompiledDirectAccessListRequiresReverseBoundary(
    ArgumentEncodingContext &enc, const CompiledDirectAccessList &list) {
  for (const auto &access : list.encoder_accesses) {
    if (access.stage == PipelineStage::Compute ||
        access.stage == PipelineStage::Pixel)
      continue;
    switch (access.kind) {
    case CompiledDirectAccessList::Kind::BufferRange:
    case CompiledDirectAccessList::Kind::BufferView:
      if (access.buffer &&
          enc.requiresReverseRenderPassBoundary(access.buffer, access.flags))
        return true;
      break;
    case CompiledDirectAccessList::Kind::TextureView:
      if (access.texture && access.view_id &&
          enc.requiresReverseRenderPassBoundary(access.texture, access.view_id,
                                                access.flags))
        return true;
      break;
    }
  }
  return false;
}

void
PublishCompiledDirectAccessListForEncode(
    ArgumentEncodingContext &enc, const CompiledDirectAccessList &list,
    CompiledCommandTestTelemetry *test_telemetry) {
  auto &queue = enc.queue();
  for (const auto &allocation : list.static_buffer_allocations)
    PublishCompiledDirectAllocation(enc, queue, allocation);
  for (const auto &allocation : list.buffer_allocations)
    PublishCompiledDirectAllocation(enc, queue, allocation);

  PublishCompiledDirectAccessStage(enc, list, PipelineStage::Vertex,
                                   test_telemetry);
  PublishCompiledDirectAccessStage(enc, list, PipelineStage::Hull,
                                   test_telemetry);
  PublishCompiledDirectAccessStage(enc, list, PipelineStage::Domain,
                                   test_telemetry);
  PublishCompiledDirectAccessStage(enc, list, PipelineStage::Geometry,
                                   test_telemetry);
  PublishCompiledDirectAccessStage(enc, list, PipelineStage::Pixel,
                                   test_telemetry);
  PublishCompiledDirectAccessStage(enc, list, PipelineStage::Compute,
                                   test_telemetry);
}

void
RetainFrozenNativeStoreForGpu(
    ArgumentEncodingContext &enc,
    const std::shared_ptr<SubmittedFrozenNativeDescriptorStore> &store) {
  if (!store)
    return;
  const auto sequence = enc.currentSeqId();
  auto retained = store->retained_sequence.load(std::memory_order_acquire);
  while (retained != sequence) {
    if (store->retained_sequence.compare_exchange_weak(
            retained, sequence, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      std::vector<GpuRetainedOwner> owners;
      owners.reserve(store->retained_resources.size() +
                     kFrozenNativeStoreFixedOwnerCount);
      for (const auto &resource : store->retained_resources)
        owners.emplace_back(resource);
      if (store->descriptor_table_buffer)
        owners.emplace_back(store->descriptor_table_buffer);
      if (store->buffer_record_buffer)
        owners.emplace_back(store->buffer_record_buffer);
      if (store->buffer_resource_table_buffer)
        owners.emplace_back(store->buffer_resource_table_buffer);
      if (store->root_base_buffer)
        owners.emplace_back(store->root_base_buffer);
      if (store->root_base_residency)
        owners.emplace_back(store->root_base_residency);
      enc.queue().RetainGpuOwners(sequence, std::move(owners));
      return;
    }
  }
}

} // namespace dxmt::d3d12
