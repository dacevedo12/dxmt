#include "d3d12_replay_dispatch_encode.hpp"

#include "d3d12_compiled_binding_encode.hpp"
#include "d3d12_compiled_direct_access.hpp"
#include "d3d12_compiled_direct_binding_encode.hpp"
#include "d3d12_replay_binding_encode.hpp"

#include <cstring>

namespace dxmt::d3d12 {

void EncodeReplayDispatchPacket(const SubmissionBindingContext &ctx,
                                ArgumentEncodingContext &enc,
                                ReplayDispatchPacket &packet,
                                uint64_t &argbuf_offset) {
  const bool compiled = packet.compiled_binding_payload.has_value();
  const bool perf_enabled = compiled && dxmt::perf::enabled();
  const auto encode_begin =
      perf_enabled ? clock::now() : clock::time_point{};

  auto *compute_encoder =
      static_cast<ComputeEncoderData *>(enc.currentEncoder());
  auto &encoder_cache = compute_encoder->binding_state_cache;
  if (!encoder_cache.pipeline_valid ||
      encoder_cache.pso_handle != packet.metal_pso.handle ||
      std::memcmp(&encoder_cache.threadgroup_size, &packet.threadgroup_size,
                  sizeof(packet.threadgroup_size)) != 0) {
    auto &set_pso = enc.encodeComputeCommand<wmtcmd_compute_setpso>();
    set_pso.type = WMTComputeCommandSetPSO;
    set_pso.pso = packet.metal_pso;
    set_pso.threadgroup_size = packet.threadgroup_size;
    encoder_cache.pso_handle = packet.metal_pso.handle;
    encoder_cache.threadgroup_size = packet.threadgroup_size;
    encoder_cache.pipeline_valid = true;
  }

  if (compiled) {
    auto &payload = *packet.compiled_binding_payload;
    const auto resolved_delta =
        ResolveComputeEncoderBindingDelta(payload, encoder_cache);
    const bool binding_hit = !resolved_delta.dirty_fields;
    /*
     * Keep resource dependency publication independent from the binding
     * program cache. Identical dispatch bindings can still be separated by
     * UAV writes or other encoders and therefore represent new ordered
     * accesses on every dispatch.
     */
    RetainFrozenNativeStoreForGpu(enc, payload.frozen_native);
    PublishCompiledDirectAccessListForEncode(
        enc, payload.direct_access, payload.test_telemetry.get());
    if (!binding_hit) {
      if (payload.test_telemetry) {
        auto &counter = resolved_delta.full_bind
                            ? payload.test_telemetry
                                  ->encoder_full_binding_programs
                            : payload.test_telemetry
                                  ->encoder_delta_binding_programs;
        counter.fetch_add(1, std::memory_order_relaxed);
      }
      EncodeCompiledComputeBindings(
          ctx.device, ctx.queue, enc, payload, *packet.pipeline,
          &resolved_delta, payload.test_telemetry.get());
      encoder_cache.binding_program = payload.binding_identity.program;
      encoder_cache.resource_heap = payload.binding_identity.resource_heap;
      encoder_cache.sampler_heap = payload.binding_identity.sampler_heap;
      encoder_cache.root_tables = payload.binding_identity.root_tables;
      encoder_cache.root_constants = payload.binding_identity.root_constants;
      encoder_cache.root_descriptors =
          payload.binding_identity.root_descriptors;
      encoder_cache.descriptor_content_revision =
          payload.descriptor_content_revision;
      encoder_cache.binding_valid = true;
    } else if (payload.test_telemetry) {
      payload.test_telemetry->encoder_binding_program_hits.fetch_add(
          1, std::memory_order_relaxed);
    }
  } else {
    encoder_cache.binding_program = nullptr;
    encoder_cache.binding_valid = false;
    // ReplayDispatch() is the only producer of a non-compiled dispatch
    // packet and it always emplaces replay_state; the compiled producers
    // always emplace compiled_binding_payload instead. Encoding compute
    // bindings without a replay state is impossible, so treat it like the
    // other missing-binding-state cases here and skip the dispatch.
    if (!packet.replay_state) {
      WARN("D3D12CommandQueue: dispatch skipped because replay binding state is missing");
      return;
    }
    const uint64_t argbuf_base = argbuf_offset;
    EncodeComputeBindings(ctx, enc, *packet.replay_state, *packet.pipeline,
                          argbuf_offset);
    if (argbuf_offset - argbuf_base > packet.argument_buffer_size) {
      WARN("D3D12CommandQueue: compute argument buffer estimate was too small estimated=",
           packet.argument_buffer_size,
           " actual=", argbuf_offset - argbuf_base);
    }
  }

  if (enc.argumentBufferOverflowed())
    return;

  if (packet.indirect_argument_buffer) {
    auto [argument_allocation, argument_sub_offset] =
        enc.access<PipelineStage::Compute>(
            packet.indirect_argument_buffer,
            packet.indirect_argument_offset,
            packet.indirect_argument_size, ResourceAccess::Read);
    auto &dispatch =
        enc.encodeComputeCommand<wmtcmd_compute_dispatch_indirect>();
    dispatch.type = WMTComputeCommandDispatchIndirect;
    dispatch.indirect_args_buffer = argument_allocation->buffer();
    dispatch.indirect_args_offset =
        argument_sub_offset + packet.indirect_argument_offset;
  } else {
    auto &dispatch = enc.encodeComputeCommand<wmtcmd_compute_dispatch>();
    dispatch.type = WMTComputeCommandDispatch;
    dispatch.size = {packet.dispatch.x, packet.dispatch.y,
                     packet.dispatch.z};
  }

  if (perf_enabled) {
    dxmt::perf::recordCompiledDispatchEncodeTime(
        &enc.currentFrameStatistics(), clock::now() - encode_begin);
  }
}

} // namespace dxmt::d3d12
