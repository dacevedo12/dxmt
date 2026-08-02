#include "dxmt_context.hpp"
#include "airconv_dx12_metal4.h"
#include "Metal.hpp"
#include "dxmt_apitrace.hpp"
#include "dxmt_bindless_buffer_table.hpp"
#include "dxmt_argument_buffer.hpp"
#include "dxmt_descriptor_mirror.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_deptrack.hpp"
#include "dxmt_format.hpp"
#include "dxmt_occlusion_query.hpp"
#include "dxmt_presenter.hpp"
#include "dxmt_pipeline_diag.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "wsi_platform.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace dxmt {

// Forward declaration: defined later in this TU (after encodeShaderResources). The
// bindless-mirror buffer-table packers (packBindlessCBuffers / packBindlessBufferTable)
// appear earlier in the file and reuse it for the same null-binding diagnostics.
template <PipelineStage stage, PipelineKind kind>
static void DebugLogNullShaderBinding(
    const char *binding_type, const char *expected, const std::string &shader_hash,
    const MTL_SM50_SHADER_ARGUMENT &arg, bool has_buffer_binding, bool has_texture_binding,
    bool has_counter_binding, uint64_t encoder_id, const char *action = "zero",
    uint32_t resolved_slot = UINT32_MAX, uint32_t range_local = 0,
    bool snapshot_supplied = false, bool snapshot_captured = false
);

template <PipelineStage stage, PipelineKind kind>
static void DebugLogSnapshotBindingFallback(
    const char *binding_type, const std::string &shader_hash,
    const MTL_SM50_SHADER_ARGUMENT &arg, uint32_t resolved_slot,
    uint32_t range_local, bool has_buffer_binding, bool has_texture_binding,
    bool has_counter_binding, uint64_t encoder_id);

template <PipelineStage stage, PipelineKind kind>
static void DebugLogConstantBufferBinding(
    const std::string &shader_hash, const MTL_SM50_SHADER_ARGUMENT &arg,
    const ConstantBufferBinding &binding, uint64_t encoded_address,
    uint64_t valid_length, uint64_t encoder_id, bool dummy
);

static bool DebugShouldLogBinding(const std::string &shader_hash);
static bool DebugEnabledEnv(const char *name);

static bool
BindlessMirrorVerifyEnabled() {
  return false;
}

static bool
BindlessMirrorVerifyShouldLog() {
  static std::atomic<uint32_t> count = 0;
  return BindlessMirrorVerifyEnabled() &&
         count.fetch_add(1, std::memory_order_relaxed) < 50;
}

static const char *
BindlessVerifyStageName(PipelineStage stage) {
  switch (stage) {
  case PipelineStage::Vertex:
    return "Vertex";
  case PipelineStage::Pixel:
    return "Pixel";
  case PipelineStage::Geometry:
    return "Geometry";
  case PipelineStage::Hull:
    return "Hull";
  case PipelineStage::Domain:
    return "Domain";
  case PipelineStage::Compute:
    return "Compute";
  }
  return "Unknown";
}

static bool
ResolveRenderPassColorAttachment(
    const char *message, unsigned slot, const TextureViewRef &attachment, WMT::Texture &resolved_texture
) {
  if (!attachment)
    return true;

  auto allocation = attachment->allocation;
  auto texture = allocation ? allocation->descriptor : nullptr;
  auto actual_texture = attachment.texture();
  if (!actual_texture) {
    WARN(message, " slot=", slot, " view=", uint64_t(attachment->key), " reason=missing Metal texture");
    return false;
  }

  auto actual_usage = actual_texture.usage();
  if (actual_usage & WMTTextureUsageRenderTarget) {
    resolved_texture = actual_texture;
    return true;
  }

  if (!texture) {
    WARN(
        message,
        " slot=", slot,
        " actual_usage=", uint32_t(actual_usage),
        " view=", uint64_t(attachment->key),
        " reason=missing texture descriptor"
    );
    return false;
  }

  auto usage = texture->usage();
  WARN(
      message,
      " slot=", slot,
      " descriptor_usage=", uint32_t(usage),
      " actual_usage=", uint32_t(actual_usage),
      " texture=", reinterpret_cast<const void *>(texture),
      " view=", uint64_t(attachment->key),
      " format=", uint32_t(texture->pixelFormat()),
      " type=", uint32_t(texture->textureType()),
      " size=", texture->width(), "x", texture->height(), "x", texture->depth(),
      " array_size=", texture->arrayLength(),
      " mip_levels=", texture->miplevelCount(),
      " sample_count=", texture->sampleCount()
  );
  return false;
}

static bool
ResolveRenderPassBufferColorAttachment(
    const char *message, unsigned slot, WMT::Texture attachment, WMT::Texture &resolved_texture
) {
  if (!attachment) {
    WARN(message, " slot=", slot, " reason=missing Metal buffer texture");
    return false;
  }

  auto actual_usage = attachment.usage();
  if (actual_usage & WMTTextureUsageRenderTarget) {
    resolved_texture = attachment;
    return true;
  }

  WARN(
      message,
      " slot=", slot,
      " actual_usage=", uint32_t(actual_usage),
      " format=", uint32_t(attachment.pixelFormat()),
      " size=", attachment.width(), "x", attachment.height(), "x", attachment.depth(),
      " array_size=", attachment.arrayLength(),
      " mip_levels=", attachment.mipmapLevelCount()
  );
  return false;
}

ArgumentEncodingContext::ArgumentEncodingContext(CommandQueue &queue, WMT::Device device, InternalCommandLibrary &lib) :
    emulated_cmd(device, lib, *this),
    clear_rt_cmd(device, lib, *this),
    resolve_texture_cmd(device, lib, *this),
    stretch_blit_cmd(device, lib, *this),
    blit_depth_stencil_cmd(device, lib, *this),
    clear_res_cmd(device, lib, *this),
    mv_scale_cmd(device, lib, *this),
    tile_barrier_cmd(device, lib, *this),
    timestamp_state_(device),
    device_(device),
    queue_(queue) {
  dummy_sampler_info_.support_argument_buffers = true;
  dummy_sampler_info_.border_color = WMTSamplerBorderColorTransparentBlack;
  dummy_sampler_info_.compare_function = WMTCompareFunctionNever;
  dummy_sampler_info_.normalized_coords = true;
  dummy_sampler_info_.r_address_mode = WMTSamplerAddressModeClampToEdge;
  dummy_sampler_info_.s_address_mode = WMTSamplerAddressModeClampToEdge;
  dummy_sampler_info_.t_address_mode = WMTSamplerAddressModeClampToEdge;
  dummy_sampler_info_.min_filter = WMTSamplerMinMagFilterNearest;
  dummy_sampler_info_.mag_filter = WMTSamplerMinMagFilterNearest;
  dummy_sampler_info_.mip_filter = WMTSamplerMipFilterNotMipmapped;
  dummy_sampler_info_.lod_min_clamp = 0.0f;
  dummy_sampler_info_.lod_max_clamp = FLT_MAX;
  dummy_sampler_info_.max_anisotroy = 1;
  dummy_sampler_info_.lod_average = false;
  dummy_sampler_ = device.newSamplerState(dummy_sampler_info_);
  dummy_cbuffer_host_ = wsi::aligned_malloc(65536, DXMT_PAGE_SIZE);
  dummy_cbuffer_info_.length = 65536;
  dummy_cbuffer_info_.memory.set(dummy_cbuffer_host_);
  dummy_cbuffer_info_.options = WMTResourceOptionCPUCacheModeWriteCombined | WMTResourceStorageModeShared |
                                WMTResourceHazardTrackingModeUntracked;
  std::memset(dummy_cbuffer_info_.memory.get(), 0, 65536);
  dummy_cbuffer_ = device.newBuffer(dummy_cbuffer_info_);
  dummy_cbuffer_residency_registration_ =
      queue_.RegisterLifetimeResidency(
          ResidencyOwnership::Lifetime(dummy_cbuffer_));
  cpu_buffer_chunks_.emplace_back();
  barrier_event_ = device_.newEvent();
};

ArgumentEncodingContext::~ArgumentEncodingContext() {
  for (auto &binding : dummy_srv_textures_)
    binding.residency_registration.reset();
  dummy_cbuffer_residency_registration_.reset();
  wsi::aligned_free(dummy_cbuffer_host_);
};

WMT::Fence
ArgumentEncodingContext::fenceForEncoder(EncoderId id) {
  const auto mapping = fence_bindings_.find(id);
  if (!mapping)
    return {};

  const uint32_t slot = *mapping;
  auto &bank = fence_pool_banks_[fence_pool_bank_index_];
  if (slot >= bank.size())
    bank.resize(slot + 1);
  auto &fence = bank[slot];
  if (fence)
    return fence;

  fence = device_.newFence();
  assert(fence);

  const auto marker = env::getEnvVar("DXMT_TEST_FENCE_CREATION_MARKER");
  if (!marker.empty()) {
    if (FILE *file = fopen(marker.c_str(), "a")) {
      fprintf(file, "%u %u\n", fence_pool_bank_index_, slot);
      fclose(file);
    }
  }
  return fence;
}

FencePoolPreparationResult
ArgumentEncodingContext::prepareFencePool(
    EncoderData **encoders, unsigned encoder_count,
    CommandBufferDiagnosticInfo *diagnostic_info) {
  // ---------------------------------------------------------------------------
  // GPTK barrier/fence model (IDA ResolveBarriers / FlushEncoders / UpdateFence /
  // DoExecute) mapped onto DXMT deptrack EncoderIds for this command buffer:
  //
  // 1) Bind only local producer ids with a later consumer. D3DMetal Metal 4
  //    builds encoder submission order first, then reuses a bounded set of
  //    stage-domain fences; interval colouring is the equivalent for DXMT's
  //    EncoderId dependency representation.
  //
  // 2) Wait only on local producer fences (ids updated in this CB).
  //    Residual EncoderIds from earlier CBs are NOT Metal wait sources —
  //    DoExecute timeline wait(prev)/signal(next) covers cross-submit order.
  // ---------------------------------------------------------------------------
  struct FenceInterval {
    EncoderId id;
    uint32_t first;
    uint32_t last;
  };
  std::vector<FenceInterval> intervals;
  std::unordered_map<EncoderId, size_t> interval_by_id;
  std::unordered_map<EncoderId, std::vector<uint32_t>> producer_indices_by_id;
  std::unordered_set<EncoderId> local_update_ids;
  std::unordered_set<EncoderId> consumed_updates;
  FenceDependencyOrderTracker dependency_order;
  FencePoolPreparationResult result = {};

  auto record_producer = [&](const FenceSet &updates, uint32_t index) {
    updates.forEach([&](EncoderId id) {
      producer_indices_by_id[id].push_back(index);
      local_update_ids.insert(id);
      if (interval_by_id.contains(id))
        return;
      interval_by_id.emplace(id, intervals.size());
      intervals.push_back({id, index, index});
    });
  };
  auto extend_waits = [&](const FenceSet &waits, uint32_t index) {
    waits.forEach([&](EncoderId id) {
      const auto interval = interval_by_id.find(id);
      if (interval == interval_by_id.end())
        return;
      intervals[interval->second].last =
          std::max(intervals[interval->second].last, index);
      consumed_updates.insert(id);
    });
  };
  auto collect_updates = [&](const EncoderData *encoder) {
    FenceSet updates = encoder->fence_update;
    if (encoder->type == EncoderType::Render)
      updates.merge(
          static_cast<const RenderEncoderData *>(encoder)->fence_update_vertex);
    return updates;
  };
  auto collect_waits = [&](const EncoderData *encoder) {
    FenceSet waits = encoder->fence_wait;
    if (encoder->type == EncoderType::Render)
      waits.merge(
          static_cast<const RenderEncoderData *>(encoder)->fence_wait_vertex);
    return waits;
  };

  for (unsigned i = 0; i < encoder_count; i++) {
    const auto *encoder = encoders[i];
    if (encoder->type == EncoderType::Null)
      continue;
    const FenceSet updates = collect_updates(encoder);
    record_producer(updates, i);
    dependency_order.recordUpdates(updates);
  }
  if (has_pending_fence_only_blit_) {
    record_producer(pending_fence_only_blit_update_, encoder_count);
    dependency_order.recordUpdates(pending_fence_only_blit_update_);
  }

  for (unsigned i = 0; i < encoder_count; i++) {
    const auto *encoder = encoders[i];
    if (encoder->type == EncoderType::Null)
      continue;
    const FenceSet waits = collect_waits(encoder);
    const FenceSet updates = collect_updates(encoder);
    if (encoder->type == EncoderType::Blit) {
      waits.forEach([&](EncoderId id) {
        result.external_blit_wait_count += !local_update_ids.contains(id);
      });
    }
    if (encoder->requires_cross_submit_wait) {
      waits.forEach([&](EncoderId id) {
        result.external_explicit_barrier_wait_count +=
            !local_update_ids.contains(id);
      });
    }
    extend_waits(waits, i);
    dependency_order.analyzeEncoder(waits, updates);
  }
  if (has_pending_fence_only_blit_) {
    pending_fence_only_blit_wait_.forEach([&](EncoderId id) {
      result.external_blit_wait_count += !local_update_ids.contains(id);
    });
    extend_waits(pending_fence_only_blit_wait_, encoder_count);
    dependency_order.analyzeEncoder(pending_fence_only_blit_wait_,
                                    pending_fence_only_blit_update_);
  }

  std::sort(intervals.begin(), intervals.end(), [](const auto &lhs,
                                                   const auto &rhs) {
    return lhs.first != rhs.first ? lhs.first < rhs.first : lhs.id < rhs.id;
  });
  std::erase_if(intervals, [&](const auto &interval) {
    return !consumed_updates.contains(interval.id);
  });

  auto &bank = fence_pool_banks_[fence_pool_bank_index_];
  fence_bindings_.reset(static_cast<uint32_t>(bank.size()));
  uint32_t used_slot_count = 0;
  for (const auto &interval : intervals) {
    /*
     * D3DMetal's Metal 4 UpdateFence calls
     * GetFenceForCurrentCommandBuffer once per stage-domain update (twice for
     * render: PreRaster and Fragment), moves each returned fence from the free
     * pool to the current-CB used list, and only returns that whole list after
     * completion. Reusing one MTLFence for disjoint index intervals is not
     * equivalent: stage-overlapped render/compute encoders may observe a later
     * update on the same fence and form a GPU-side cycle.
     */
    const auto slot =
        fence_bindings_.bindUnique(interval.id, interval.last);
    used_slot_count = std::max(used_slot_count, slot + 1);
  }
  const uint32_t local_fence_id_count =
      static_cast<uint32_t>(intervals.size());

  if (diagnostic_info &&
      (DebugEnabledEnv("DXMT_DIAG_GPU_HANG_DENSE") ||
       DebugEnabledEnv("DXMT_DIAG_ROOT_CAUSE_DENSE") ||
       DebugEnabledEnv("DXMT_DIAG_ERROR_SNAPSHOT") ||
       DebugEnabledEnv("DXMT_VALIDATION") ||
       DebugEnabledEnv("DXMT_DIAG_VALIDATION"))) {
    struct ResourcePlanAggregate {
      const EncoderResourcePlanKey *key = nullptr;
      uint32_t access = 0;
      uint32_t count = 0;
      uint64_t hash = 1469598103934665603ull;
    };
    std::vector<std::unordered_map<uintptr_t, ResourcePlanAggregate>>
        resource_plans_by_encoder(encoder_count);
    for (unsigned i = 0; i < encoder_count; i++) {
      const auto *encoder = encoders[i];
      const std::unordered_map<EncoderResourcePlanKey, int,
                               EncoderResourcePlanKeyHash> *plan = nullptr;
      if (encoder->type == EncoderType::Render) {
        plan = &static_cast<const RenderEncoderData *>(encoder)
                    ->resource_plan_accesses;
      } else if (encoder->type == EncoderType::Compute) {
        plan = &static_cast<const ComputeEncoderData *>(encoder)
                    ->resource_plan_accesses;
      }
      if (!plan)
        continue;
      auto &aggregates = resource_plans_by_encoder[i];
      aggregates.reserve(plan->size());
      for (const auto &[key, access] : *plan) {
        auto &aggregate = aggregates[key.object];
        if (!aggregate.key)
          aggregate.key = &key;
        aggregate.access |= static_cast<uint32_t>(access);
        aggregate.count++;
        aggregate.hash ^= EncoderResourcePlanKeyHash{}(key) +
                          0x9e3779b97f4a7c15ull +
                          (aggregate.hash << 6) + (aggregate.hash >> 2);
      }
    }

    auto record_edge = [&](EncoderId dependency_id, EncoderId consumer_id,
                           uint32_t consumer_index, uint32_t flags,
                           bool want_local) {
      const auto producers = producer_indices_by_id.find(dependency_id);
      const bool local = producers != producer_indices_by_id.end();
      if (local != want_local)
        return;
      uint32_t producer_index = UINT32_MAX;
      if (local) {
        const auto &indices = producers->second;
        const auto next =
            std::lower_bound(indices.begin(), indices.end(), consumer_index);
        producer_index =
            next != indices.begin() ? *std::prev(next) : indices.front();
      }
      if (producer_index < encoder_count &&
          encoders[producer_index]->type == EncoderType::Render &&
          consumer_index < encoder_count &&
          encoders[consumer_index]->type == EncoderType::Render) {
        const auto *producer_render =
            static_cast<const RenderEncoderData *>(
                encoders[producer_index]);
        const bool producer_vertex =
            producer_render->fence_update_vertex.test(dependency_id);
        const bool producer_fragment =
            producer_render->fence_update.test(dependency_id);
        const bool consumer_vertex =
            flags & CommandBufferFenceEdgePreRaster;
        const bool consumer_fragment =
            flags & CommandBufferFenceEdgeFragment;
        if (producer_vertex)
          flags |= CommandBufferFenceEdgeProducerPreRaster;
        if (producer_fragment)
          flags |= CommandBufferFenceEdgeProducerFragment;
        diagnostic_info->render_valid_cross_stage_count +=
            producer_vertex && consumer_fragment;
        diagnostic_info->render_same_stage_wait_count +=
            (producer_vertex && consumer_vertex) ||
            (producer_fragment && consumer_fragment);
        diagnostic_info->render_reverse_stage_wait_count +=
            producer_fragment && consumer_vertex;
      }
      const auto edge_index = diagnostic_info->fence_edge_count++;
      if (edge_index >= kCommandBufferFenceEdgeCapacity) {
        diagnostic_info->fence_edge_count =
            kCommandBufferFenceEdgeCapacity;
        diagnostic_info->fence_edge_overflow_count++;
        return;
      }
      auto &edge = diagnostic_info->fence_edges[edge_index];
      edge.producer_id = dependency_id;
      edge.consumer_id = consumer_id;
      edge.producer_index = producer_index;
      edge.consumer_index = consumer_index;
      edge.slot = fence_bindings_.find(dependency_id).value_or(UINT32_MAX);
      edge.flags = flags;
      if (!local || producer_index >= encoder_count ||
          consumer_index >= encoder_count)
        return;

      const auto &producer_plan =
          resource_plans_by_encoder[producer_index];
      const auto &consumer_plan =
          resource_plans_by_encoder[consumer_index];
      const auto *smaller = &producer_plan;
      const auto *larger = &consumer_plan;
      if (smaller->size() > larger->size())
        std::swap(smaller, larger);
      const ResourcePlanAggregate *producer_resource = nullptr;
      const ResourcePlanAggregate *consumer_resource = nullptr;
      for (const auto &[object, aggregate] : *smaller) {
        const auto other = larger->find(object);
        if (other == larger->end())
          continue;
        const auto producer = producer_plan.find(object);
        const auto consumer = consumer_plan.find(object);
        if (producer == producer_plan.end() ||
            consumer == consumer_plan.end())
          continue;
        edge.resource_match_count++;
        edge.resource_match_hash ^=
            producer->second.hash + 0x9e3779b97f4a7c15ull +
            (edge.resource_match_hash << 6) +
            (edge.resource_match_hash >> 2);
        edge.resource_match_hash ^=
            consumer->second.hash + 0x9e3779b97f4a7c15ull +
            (edge.resource_match_hash << 6) +
            (edge.resource_match_hash >> 2);
        if (!producer_resource) {
          producer_resource = &producer->second;
          consumer_resource = &consumer->second;
        }
      }
      if (!producer_resource || !consumer_resource ||
          !producer_resource->key || !consumer_resource->key)
        return;

      const auto &producer_key = *producer_resource->key;
      const auto &consumer_key = *consumer_resource->key;
      edge.resource_object = producer_key.object;
      edge.producer_offset = producer_key.offset;
      edge.producer_length = producer_key.length;
      edge.producer_view_id = producer_key.view_id;
      edge.consumer_offset = consumer_key.offset;
      edge.consumer_length = consumer_key.length;
      edge.consumer_view_id = consumer_key.view_id;
      edge.producer_access = producer_resource->access;
      edge.consumer_access = consumer_resource->access;
      edge.producer_stage_kind =
          static_cast<uint32_t>(producer_key.stage) |
          (static_cast<uint32_t>(producer_key.kind) << 8);
      edge.consumer_stage_kind =
          static_cast<uint32_t>(consumer_key.stage) |
          (static_cast<uint32_t>(consumer_key.kind) << 8);

      if (producer_key.kind <= 1) {
        auto *buffer = reinterpret_cast<Buffer *>(producer_key.object);
        auto *allocation = buffer ? buffer->current() : nullptr;
        edge.resource_identity = producer_key.object;
        edge.allocation_object =
            reinterpret_cast<uintptr_t>(allocation);
        if (allocation) {
          edge.metal_resource_handle = allocation->buffer().handle;
          edge.gpu_address_or_resource_id = allocation->gpuAddress();
        }
      } else if (producer_key.kind == 2) {
        auto *texture = reinterpret_cast<Texture *>(producer_key.object);
        auto *allocation = texture ? texture->current() : nullptr;
        edge.resource_identity =
            texture ? texture->diagnosticIdentity() : 0;
        edge.allocation_object =
            reinterpret_cast<uintptr_t>(allocation);
        if (allocation) {
          edge.metal_resource_handle = allocation->texture().handle;
          edge.gpu_address_or_resource_id = allocation->gpuResourceID;
        }
      }
    };
    auto record_edges = [&](bool want_local) {
      for (unsigned i = 0; i < encoder_count; i++) {
        const auto *encoder = encoders[i];
        if (encoder->type == EncoderType::Null)
          continue;
        if (encoder->type == EncoderType::Render) {
          const auto *render =
              static_cast<const RenderEncoderData *>(encoder);
          render->fence_wait.forEach(
              render->fence_wait_vertex,
              [&](EncoderId id) {
                record_edge(id, render->encoder_id_vertex, i,
                            CommandBufferFenceEdgePreRaster, want_local);
              },
              [&](EncoderId id) {
                record_edge(id, encoder->id, i,
                            CommandBufferFenceEdgeFragment, want_local);
              });
        } else {
          uint32_t flags = CommandBufferFenceEdgeOther;
          if (encoder->type == EncoderType::Compute)
            flags = CommandBufferFenceEdgeCompute;
          else if (encoder->type == EncoderType::Blit)
            flags = CommandBufferFenceEdgeBlit;
          encoder->fence_wait.forEach([&](EncoderId id) {
            record_edge(id, encoder->id, i, flags, want_local);
          });
        }
      }
      if (has_pending_fence_only_blit_) {
        pending_fence_only_blit_wait_.forEach([&](EncoderId id) {
          record_edge(id, 0, encoder_count,
                      CommandBufferFenceEdgeBlit |
                          CommandBufferFenceEdgePending,
                      want_local);
        });
      }
    };
    // Local graph first, then external residuals (diagnostic only — encode
    // strips external waits via retain_bound below).
    record_edges(true);
    record_edges(false);
  }

  // Waits and updates retain only consumed local producers. External residual
  // EncoderIds drop — covered by DoExecute timeline, not per-id MTLFence waits.
  // Same-encoder self-waits (wait id also updated by this encoder) would
  // deadlock on MTLFence (wait before update); GPTK resolves those as
  // intrapass memoryBarrier / implicit stage order, not waitForFence.
  auto retain_bound = [&](FenceSet &fences) {
    FenceSet retained;
    fences.forEach([&](EncoderId id) {
      if (fence_bindings_.find(id))
        retained.set(id);
    });
    fences = std::move(retained);
  };
  auto strip_self_waits = [&](FenceSet &waits, const FenceSet &updates) {
    FenceSet retained;
    waits.forEach([&](EncoderId id) {
      if (!updates.test(id))
        retained.set(id);
    });
    waits = std::move(retained);
  };
  for (unsigned i = 0; i < encoder_count; i++) {
    auto *encoder = encoders[i];
    if (encoder->type == EncoderType::Null)
      continue;
    retain_bound(encoder->fence_wait);
    retain_bound(encoder->fence_update);
    if (encoder->type == EncoderType::Render) {
      auto *render = static_cast<RenderEncoderData *>(encoder);
      retain_bound(render->fence_wait_vertex);
      retain_bound(render->fence_update_vertex);
      // Render updates cover both stage ids for self-wait stripping.
      FenceSet all_updates = render->fence_update;
      all_updates.merge(render->fence_update_vertex);
      strip_self_waits(render->fence_wait_vertex, all_updates);
      strip_self_waits(encoder->fence_wait, all_updates);
    } else {
      strip_self_waits(encoder->fence_wait, encoder->fence_update);
    }
  }
  if (has_pending_fence_only_blit_) {
    retain_bound(pending_fence_only_blit_wait_);
    retain_bound(pending_fence_only_blit_update_);
    strip_self_waits(pending_fence_only_blit_wait_,
                     pending_fence_only_blit_update_);
  }

  const auto &order = dependency_order.analysis();
  result.external_wait_count = order.external_waits;
  if (diagnostic_info) {
    diagnostic_info->prior_local_fence_wait_count = order.prior_local_waits;
    diagnostic_info->future_local_fence_wait_count = order.future_local_waits;
    diagnostic_info->same_encoder_fence_wait_count = order.same_encoder_waits;
    diagnostic_info->external_fence_wait_count = order.external_waits;
    diagnostic_info->repeated_fence_update_count = order.repeated_updates;
    diagnostic_info->local_fence_id_count = local_fence_id_count;
    diagnostic_info->bound_fence_slot_count = used_slot_count;
  }
  return result;
}

template void ArgumentEncodingContext::encodeVertexBuffers<PipelineKind::Ordinary>(uint32_t slot_mask, uint64_t argument_buffer_offset);
template void ArgumentEncodingContext::encodeVertexBuffers<PipelineKind::Tessellation>(uint32_t slot_mask, uint64_t argument_buffer_offset);
template void ArgumentEncodingContext::encodeVertexBuffers<PipelineKind::Geometry>(uint32_t slot_mask, uint64_t argument_buffer_offset);

template <PipelineKind kind>
void
ArgumentEncodingContext::encodeVertexBuffers(uint32_t slot_mask, uint64_t offset) {
  struct VERTEX_BUFFER_ENTRY {
    uint64_t buffer_handle;
    uint32_t stride;
    uint32_t length;
  };
  uint32_t max_slot = 32 - __builtin_clz(slot_mask);
  const auto table_size =
      uint64_t(__builtin_popcount(slot_mask)) * sizeof(VERTEX_BUFFER_ENTRY);

  VERTEX_BUFFER_ENTRY *entries =
      getMappedArgumentBuffer<VERTEX_BUFFER_ENTRY>(offset, table_size);
  if (!entries)
    return;

  for (unsigned slot = 0, index = 0; slot < max_slot; slot++) {
    if (!(slot_mask & (1 << slot)))
      continue;
    auto &state = vbuf_[slot];
    auto &buffer = state.buffer;
    if (!buffer.ptr()) {
      entries[index].buffer_handle = 0;
      entries[index].stride = 0;
      entries[index++].length = 0;
      continue;
    }
    auto valid_length = buffer->length() > state.offset ? buffer->length() - state.offset : 0;
    auto [buffer_alloc, buffer_offset] =
        access<PipelineStage::Vertex>(buffer, state.offset, valid_length, ResourceAccess::Read);
    entries[index].buffer_handle = buffer_alloc->gpuAddress() + buffer_offset + state.offset;
    entries[index].stride = state.stride;
    entries[index++].length = valid_length;
    // FIXME: did we intended to use the whole buffer?
    makeResident<PipelineStage::Vertex, kind>(buffer.ptr());
  };
  {
    const WMTRenderStages stages =
        (kind == PipelineKind::Geometry || kind == PipelineKind::Tessellation)
            ? WMTRenderStageObject
            : WMTRenderStageVertex;
    const auto binding_offset = deduplicateRenderArgumentTableSlice(stages, 16, offset, table_size);
    const auto final_offset = getFinalArgumentBufferOffset(binding_offset);
    if (!shouldEmitRenderArgumentBufferOffset(stages, 16, final_offset, binding_offset != offset))
      return;
    auto &cmd = encodeRenderCommand<wmtcmd_render_setargumentbufferoffset>();
    cmd.offset = final_offset;
    cmd.index = 16;
    cmd.type = WMTRenderCommandSetArgumentBufferOffset;
    cmd.stages = stages;
  }
}

template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const std::string &shader_hash
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const std::string &shader_hash
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const std::string &shader_hash
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const std::string &shader_hash
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Hull, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const std::string &shader_hash
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Domain, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const std::string &shader_hash
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Compute, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const std::string &shader_hash
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const std::string &shader_hash
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Geometry, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const std::string &shader_hash
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const std::string &shader_hash
);

template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Hull, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Domain, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Compute, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Geometry, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
);

template <PipelineStage stage, PipelineKind kind>
void
ArgumentEncodingContext::encodeConstantBuffers(
    const MTL_SHADER_REFLECTION *reflection,
    const MTL_SM50_SHADER_ARGUMENT *constant_buffers, uint64_t offset,
    const std::string &shader_hash) {
  small_vector<uint64_t, 16> encoded_table(reflection->NumConstantBuffers, 0);
  const auto encoder_id = currentEncoderId();

  for (unsigned i = 0; i < reflection->NumConstantBuffers; i++) {
    auto &arg = constant_buffers[i];
    auto slot = 14 * unsigned(stage) + arg.SM50BindingSlot;
    switch (arg.Type) {
    case SM50BindingType::ConstantBuffer: {
      auto &cbuf = cbuf_[slot];
      if (cbuf.direct_buffer) {
        encoded_table[arg.StructurePtrOffset] =
            cbuf.direct_gpu_address + cbuf.offset;
        DebugLogConstantBufferBinding<stage, kind>(
            shader_hash, arg, cbuf, encoded_table[arg.StructurePtrOffset],
            cbuf.direct_length > cbuf.offset
                ? cbuf.direct_length - cbuf.offset
                : 0,
            encoder_id, false);
        makeResident<stage, kind>(cbuf.direct_buffer,
                                  GetResidencyMask<kind>(stage, true, false));
        continue;
      }
      if (!cbuf.buffer.ptr()) {
        encoded_table[arg.StructurePtrOffset] = dummy_cbuffer_info_.gpu_address;
        DebugLogConstantBufferBinding<stage, kind>(
            shader_hash, arg, cbuf, dummy_cbuffer_info_.gpu_address, 0,
            encoder_id, true);
        makeResident<stage, kind>(dummy_cbuffer_, GetResidencyMask<kind>(stage, true, false));
        continue;
      }
      auto argbuf = cbuf.buffer;
      auto valid_length = argbuf->length() > cbuf.offset ? argbuf->length() - cbuf.offset : 0;
      auto [argbuf_alloc, argbuf_offset] = access<stage>(argbuf, cbuf.offset, valid_length, ResourceAccess::Read);
      encoded_table[arg.StructurePtrOffset] = argbuf_alloc->gpuAddress() + argbuf_offset + cbuf.offset;
      DebugLogConstantBufferBinding<stage, kind>(
          shader_hash, arg, cbuf, encoded_table[arg.StructurePtrOffset],
          valid_length, encoder_id, false);
      makeResident<stage, kind>(argbuf.ptr());
      break;
    }
    default:
      DXMT_UNREACHABLE
    }
  }

  /* kConstantBufferTableBinding = 29 */
  const auto table_size = uint64_t(reflection->NumConstantBuffers) << 3;
  if constexpr (stage == PipelineStage::Compute) {
    const auto binding_offset =
        deduplicateComputeArgumentTableSliceBytes(29, offset, table_size, encoded_table.data());
    if (binding_offset == offset) {
      auto *mapped =
          getMappedArgumentBuffer<uint64_t, true>(offset, table_size);
      if (!mapped)
        return;
      std::memcpy(mapped, encoded_table.data(), table_size);
    }
    const auto final_offset = getFinalArgumentBufferOffset<true>(binding_offset);
    if (!shouldEmitComputeArgumentBufferOffset(29, final_offset, binding_offset != offset))
      return;
    auto &cmd = encodeComputeCommand<wmtcmd_compute_setargumentbufferoffset>();
    cmd.type = WMTComputeCommandSetArgumentBufferOffset;
    cmd.offset = final_offset;
    cmd.index = 29;
  } else {
    uint8_t index = 29;
    WMTRenderStages stages = WMTRenderStageVertex;
    if constexpr (stage == PipelineStage::Vertex) {
      if constexpr (kind == PipelineKind::Geometry)
        stages = WMTRenderStageObject;
      else if constexpr (kind == PipelineKind::Tessellation) {
        stages = WMTRenderStageObject;
        index = 27;
      } else
        stages = WMTRenderStageVertex;
    } else if constexpr (stage == PipelineStage::Pixel) {
      stages = WMTRenderStageFragment;
    } else if constexpr (stage == PipelineStage::Hull) {
      stages = WMTRenderStageObject;
    } else if constexpr (stage == PipelineStage::Domain) {
      stages = WMTRenderStageMesh;
    } else if constexpr (stage == PipelineStage::Geometry) {
      stages = WMTRenderStageMesh;
    } else {
      assert(0 && "Not implemented or unreachable");
    }
    const auto binding_offset =
        deduplicateRenderArgumentTableSliceBytes(stages, index, offset, table_size, encoded_table.data());
    if (binding_offset == offset) {
      auto *mapped = getMappedArgumentBuffer<uint64_t>(offset, table_size);
      if (!mapped)
        return;
      std::memcpy(mapped, encoded_table.data(), table_size);
    }
    const auto final_offset = getFinalArgumentBufferOffset(binding_offset);
    if (!shouldEmitRenderArgumentBufferOffset(stages, index, final_offset, binding_offset != offset))
      return;
    auto &cmd = encodeRenderCommand<wmtcmd_render_setargumentbufferoffset>();
    cmd.type = WMTRenderCommandSetArgumentBufferOffset;
    cmd.index = index;
    cmd.stages = stages;
    cmd.offset = final_offset;
  }
};

template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Vertex, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Pixel, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Vertex, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Pixel, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Hull, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Domain, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Compute, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Vertex, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Geometry, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Pixel, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset, const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings, uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
);

// ---- bindless-mirror (Stage-1 sub-step ③) per-draw BUFFER-table pack ----
//
// Hybrid ABI (fh4-debug/BINDLESS-ABI.md §4.5): in bindless-mirror mode, textures and
// samplers are read from the persistent slot-30 mirror (filled on the encode thread by
// FillBindlessMirrorSlot, ②/③A), so they are NOT written into a per-draw argument buffer.
// BUFFER descriptors (CBV + SRV/UAV buffer + UAV counter) DO churn per draw (their
// gpuAddress = alloc base + currentSuballocationOffset()), so they are packed FRESH each
// draw into the slot-27 `buf_table` by a STATIC compact index that airconv bakes at
// compile time. This routine packs exactly those buffer qwords.
//
// It walks the SAME reflection and resolves each buffer's address with the EXACT same
// access<stage>() expressions as encodeShaderResources (so the bytes are identical), but:
//   - writes buffers to buf_table[compact_base*..] (slot 27) instead of the big argbuf;
//   - for textures/samplers it STILL calls access<>()/makeResident (residency + barrier
//     tracking are preserved exactly as legacy), it just does not emit any buffer bytes
//     for them — the slot-30 mirror already carries their handles;
//   - emits NO slot-30 setArgumentBufferOffset (the mirror is bound whole at slot 30 by
//     the caller); the caller binds buf_table (27) + root_offsets (28) + mirror (30).
//
// The compact-index walk MUST match airconv (ForEachBufferTableField / BINDLESS-ABI.md).
// `cb_compact` maps a CBV's StructurePtrOffset (== its index in constant_buffers) to its
// buf_table qword base; `res_compact` maps a resource arg's StructurePtrOffset to its
// buf_table qword base. They are precomputed by the caller via the shared walk so both
// sides agree. `buf_table` is the CPU-mapped slot-27 ring slice; `buf_table_qwords` is
// its capacity (for bounds safety only).
// Pack the CBV portion of buf_table (the first fields by the compact walk). Resolves
// each CBV's base gpuAddress with the SAME expression as encodeConstantBuffers
// (dxmt_context.cpp), writing qw0 = address, qw1 = 0 (CBV meta is unused, ABI §4.5).
// `cb_compact[i]` is CBV i's qword base. Constant buffers are bound via cbuf_ (no
// per-draw snapshot bindings exist for the bindless draw path).
template <PipelineStage stage, PipelineKind kind>
void
ArgumentEncodingContext::packBindlessCBuffers(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t *buf_table, uint32_t buf_table_qwords, const uint32_t *cb_compact,
    const ConstantBufferBindingSnapshot *bindings
) {
  for (unsigned i = 0; i < reflection->NumConstantBuffers; i++) {
    auto &arg = constant_buffers[i];
    const uint32_t qw_base = cb_compact[i];
    if (qw_base + 1 >= buf_table_qwords)
      continue;
    auto slot = 14 * unsigned(stage) + arg.SM50BindingSlot;
    const auto count = BufferTableRangeQwordCount(arg.RegisterCount) /
                       kBufferTableQwordsPerDescriptor;
    const auto write_cbv = [&](uint32_t local, const ConstantBufferBinding &cbuf) {
      const uint32_t dst = qw_base + local * kBufferTableQwordsPerDescriptor;
      if (dst + 1 >= buf_table_qwords)
        return;
      // Root CBV (SetGraphicsRootConstantBufferView -> bindConstantBufferDirect)
      // clears cbuf.buffer and stores the address in direct_gpu_address. Mirror the
      // legacy encodeConstantBuffers direct_buffer branch; without it this CBV would
      // fall through to the dummy below and read garbage constants.
      if (cbuf.direct_buffer) {
        buf_table[dst] = cbuf.direct_gpu_address + cbuf.offset;
        buf_table[dst + 1] = 0;
        makeResident<stage, kind>(cbuf.direct_buffer,
                                  GetResidencyMask<kind>(stage, true, false));
        return;
      }
      if (!cbuf.buffer.ptr()) {
        buf_table[dst] = dummy_cbuffer_info_.gpu_address;
        buf_table[dst + 1] = 0;
        makeResident<stage, kind>(dummy_cbuffer_,
                                  GetResidencyMask<kind>(stage, true, false));
        return;
      }
      auto argbuf = cbuf.buffer;
      auto valid_length =
          argbuf->length() > cbuf.offset ? argbuf->length() - cbuf.offset : 0;
      auto [argbuf_alloc, argbuf_offset] =
          access<stage>(argbuf, cbuf.offset, valid_length, ResourceAccess::Read);
      buf_table[dst] = argbuf_alloc->gpuAddress() + argbuf_offset + cbuf.offset;
      buf_table[dst + 1] = 0;
      makeResident<stage, kind>(argbuf.ptr());
    };

    if (bindings) {
      for (uint32_t local = 0; local < count; local++) {
        const auto local_slot = slot + local;
        if (local_slot >= 14 * (unsigned(stage) + 1))
          break;
        const auto &snapshot = bindings[local_slot];
        // Root constants are restored into cbuf_ immediately before this
        // packer runs. They intentionally do not have a descriptor snapshot,
        // so treating that live read as a snapshot fallback produces a hot
        // path false positive without identifying stale state.
        write_cbv(local,
                  snapshot.captured ? snapshot.binding : cbuf_[local_slot]);
      }
      continue;
    }

    for (uint32_t local = 0; local < count; local++) {
      const auto local_slot = slot + local;
      if (local_slot >= 14 * (unsigned(stage) + 1))
        break;
      write_cbv(local, cbuf_[local_slot]);
    }
  }
}

template <PipelineStage stage, PipelineKind kind>
void
ArgumentEncodingContext::packBindlessBufferTable(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments,
    const std::string &shader_hash, uint64_t *buf_table, uint32_t buf_table_qwords,
    const uint32_t *res_compact, const BindlessBufferTableSnapshot *bindings,
    uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
) {
  auto BindingCount = reflection->NumArguments;
  auto &UAVBindingSet = stage == PipelineStage::Compute ? cs_uav_ : om_uav_;
  auto encoder_id = currentEncoderId();

  auto write_slot = [&](uint32_t qw_base, uint64_t qw0, uint64_t qw1) {
    if (qw_base + 1 < buf_table_qwords) {
      buf_table[qw_base] = qw0;
      buf_table[qw_base + 1] = qw1;
    }
  };

  for (unsigned i = 0; i < BindingCount; i++) {
    auto arg = arguments[i];
    if constexpr (stage == PipelineStage::Pixel) {
      if (arg.Type == SM50BindingType::SRV && arg.SM50BindingSlot < 128) {
        const bool demote_msaa =
            arg.SM50BindingSlot < 64
                ? (demote_msaa_srv_mask_lo &
                   (uint64_t(1) << arg.SM50BindingSlot))
                : (demote_msaa_srv_mask_hi &
                   (uint64_t(1) << (arg.SM50BindingSlot - 64)));
        if (demote_msaa) {
          arg.Flags = MTL_SM50_SHADER_ARGUMENT_FLAG(
              arg.Flags & ~MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLED);
        }
      }
    }
    const uint32_t compact = res_compact[i]; // qword base for this arg's buffer field
    switch (arg.Type) {
    case SM50BindingType::Sampler:
      // Sampler lives in the slot-30 mirror; nothing to pack. (No access<>() — the legacy
      // sampler branch performs no access<>()/makeResident either.)
      break;
    case SM50BindingType::SRV: {
      const auto slot = kSRVBindings * unsigned(stage) + arg.SM50BindingSlot;
      const auto count = BufferTableRangeQwordCount(arg.RegisterCount) /
                         kBufferTableQwordsPerDescriptor;
      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER) {
        for (uint32_t local = 0; local < count; local++) {
          const auto local_slot = slot + local;
          if (local_slot >= kSRVBindings * (unsigned(stage) + 1))
            break;
          const auto &snapshot =
              bindings && bindings->resources
                  ? bindings->resources[local_slot]
                  : ShaderResourceBindingSnapshot{};
          if (bindings && bindings->resources && !snapshot.srv_captured) {
            const auto &live = resview_[local_slot];
            DebugLogSnapshotBindingFallback<stage, kind>(
                "SRV", shader_hash, arg, local_slot, local,
                bool(live.buffer.ptr()), bool(live.texture.ptr()), false,
                encoder_id);
          }
          const auto &srv = snapshot.srv_captured ? snapshot.srv
                                                  : resview_[local_slot];
          const uint32_t dst = compact + local * kBufferTableQwordsPerDescriptor;
          if (srv.buffer.ptr()) {
            auto [srv_alloc, offset] = access<stage>(
                srv.buffer, srv.slice.byteOffset, srv.slice.byteLength,
                ResourceAccess::Read);
            write_slot(dst, srv_alloc->gpuAddress() + offset + srv.slice.byteOffset,
                       srv.slice.byteLength);
            makeResident<stage, kind>(srv.buffer.ptr());
          } else {
            DebugLogNullShaderBinding<stage, kind>(
                "SRV", "buffer", shader_hash, arg, bool(srv.buffer.ptr()),
                bool(srv.texture.ptr()), false, encoder_id, "zero",
                local_slot, local, bool(bindings), snapshot.srv_captured);
            write_slot(dst, 0, 0);
          }
        }
      } else if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE) {
        // Texture / tbuffer ranges live in the mirrors, but every descriptor the
        // shader can index still needs the matching access and residency state.
        for (uint32_t local = 0; local < count; local++) {
          const auto local_slot = slot + local;
          if (local_slot >= kSRVBindings * (unsigned(stage) + 1))
            break;
          const auto &snapshot =
              bindings && bindings->resources
                  ? bindings->resources[local_slot]
                  : ShaderResourceBindingSnapshot{};
          if (bindings && bindings->resources && !snapshot.srv_captured) {
            const auto &live = resview_[local_slot];
            DebugLogSnapshotBindingFallback<stage, kind>(
                "SRV_TEXTURE", shader_hash, arg, local_slot, local,
                bool(live.buffer.ptr()), bool(live.texture.ptr()), false,
                encoder_id);
          }
          const auto &srv = snapshot.srv_captured ? snapshot.srv
                                                  : resview_[local_slot];
          if (srv.buffer.ptr()) {
            assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET);
            auto allocation = srv.buffer->current();
            auto *view_ptr = srv.buffer->tryView(srv.viewId, allocation);
            if (view_ptr && view_ptr->texture) {
              access<stage>(srv.buffer, srv.viewId, ResourceAccess::Read);
              makeResident<stage, kind>(srv.buffer.ptr(), srv.viewId);
            }
          } else if (srv.texture.ptr()) {
            assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP);
            auto viewIdChecked = srv.texture->checkViewUseArray(
                srv.viewId, arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY);
            access<stage>(srv.texture, viewIdChecked, ResourceAccess::Read);
            makeResident<stage, kind>(srv.texture.ptr(), viewIdChecked);
          } else {
            auto &dummy_texture = dummySRVTexture(arg);
            if (dummy_texture.texture) {
              DXMT_RESOURCE_RESIDENCY requested =
                  GetResidencyMask<kind>(stage, true, false);
              if (CheckResourceResidency(dummy_texture.residency,
                                         currentEncoderId(), requested))
                makeResident<stage, kind>(dummy_texture.texture, requested);
            }
          }
        }
      }
      break;
    }
    case SM50BindingType::UAV: {
      const auto count = BufferTableRangeQwordCount(arg.RegisterCount) /
                         kBufferTableQwordsPerDescriptor;
      bool read = (arg.Flags >> 10) & 1, write = (arg.Flags >> 10) & 2;
      if (!read && !write) {
        read = true;
        write = true;
      }
      int access_flags = (read ? ResourceAccess::Read : 0) |
                             (write ? ResourceAccess::Write : 0) |
                             ResourceAccess::UAV;
      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER) {
        for (uint32_t local = 0; local < count; local++) {
          const auto local_slot = arg.SM50BindingSlot + local;
          if (local_slot >= kUAVBindings)
            break;
          const auto &snapshot =
              bindings && bindings->resources
                  ? bindings->resources[local_slot]
                  : ShaderResourceBindingSnapshot{};
          if (bindings && bindings->resources && !snapshot.uav_captured) {
            const auto &live = UAVBindingSet[local_slot];
            DebugLogSnapshotBindingFallback<stage, kind>(
                "UAV", shader_hash, arg, local_slot, local,
                bool(live.buffer.ptr()), bool(live.texture.ptr()),
                bool(live.counter.ptr()), encoder_id);
          }
          const auto &uav = snapshot.uav_captured
                                ? snapshot.uav
                                : UAVBindingSet[local_slot];
          const uint32_t dst = compact + local * kBufferTableQwordsPerDescriptor;
          if (uav.buffer.ptr()) {
            auto [uav_alloc, offset] = access<stage>(
                uav.buffer, uav.slice.byteOffset, uav.slice.byteLength,
                access_flags);
            write_slot(dst, uav_alloc->gpuAddress() + offset + uav.slice.byteOffset,
                       uav.slice.byteLength);
            makeResident<stage, kind>(uav.buffer.ptr(), read, write);
          } else {
            DebugLogNullShaderBinding<stage, kind>(
                "UAV", "buffer", shader_hash, arg, bool(uav.buffer.ptr()),
                bool(uav.texture.ptr()), bool(uav.counter.ptr()), encoder_id,
                "zero", local_slot, local, bool(bindings),
                snapshot.uav_captured);
            write_slot(dst, 0, 0);
          }
        }
      } else if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE) {
        for (uint32_t local = 0; local < count; local++) {
          const auto local_slot = arg.SM50BindingSlot + local;
          if (local_slot >= kUAVBindings)
            break;
          const auto &snapshot =
              bindings && bindings->resources
                  ? bindings->resources[local_slot]
                  : ShaderResourceBindingSnapshot{};
          if (bindings && bindings->resources && !snapshot.uav_captured) {
            const auto &live = UAVBindingSet[local_slot];
            DebugLogSnapshotBindingFallback<stage, kind>(
                "UAV_TEXTURE", shader_hash, arg, local_slot, local,
                bool(live.buffer.ptr()), bool(live.texture.ptr()),
                bool(live.counter.ptr()), encoder_id);
          }
          const auto &uav = snapshot.uav_captured
                                ? snapshot.uav
                                : UAVBindingSet[local_slot];
          if (uav.buffer.ptr()) {
            assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET);
            auto allocation = uav.buffer->current();
            auto *view_ptr = uav.buffer->tryView(uav.viewId, allocation);
            if (view_ptr && view_ptr->texture) {
              access<stage>(uav.buffer, uav.viewId, access_flags);
              makeResident<stage, kind>(uav.buffer.ptr(), uav.viewId, read,
                                        write);
            }
          } else if (uav.texture.ptr()) {
            assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP);
            auto viewIdChecked = uav.texture->checkViewUseArray(
                uav.viewId, arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY);
            access<stage>(uav.texture, viewIdChecked, access_flags);
            makeResident<stage, kind>(uav.texture.ptr(), viewIdChecked, read,
                                      write);
          }
        }
      }
      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER) {
        for (uint32_t local = 0; local < count; local++) {
          const auto local_slot = arg.SM50BindingSlot + local;
          if (local_slot >= kUAVBindings)
            break;
          const auto &snapshot =
              bindings && bindings->resources
                  ? bindings->resources[local_slot]
                  : ShaderResourceBindingSnapshot{};
          const auto &uav = snapshot.uav_captured
                                ? snapshot.uav
                                : UAVBindingSet[local_slot];
          const uint32_t dst =
              compact + local * kBufferTableQwordsPerDescriptor + 2;
          if (uav.counter) {
            auto [counter_alloc, offset] =
                access<stage>(uav.counter, 0, 4, ResourceAccess::All);
            if (dst < buf_table_qwords)
              buf_table[dst] = counter_alloc->gpuAddress() + offset;
            makeResident<stage, kind>(uav.counter.ptr(), true, true);
          } else {
            DebugLogNullShaderBinding<stage, kind>(
                "UAV_COUNTER", "counter", shader_hash, arg,
                bool(uav.buffer.ptr()), bool(uav.texture.ptr()),
                bool(uav.counter.ptr()), encoder_id, "zero", local_slot,
                local, bool(bindings), snapshot.uav_captured);
            if (dst < buf_table_qwords)
              buf_table[dst] = 0;
          }
        }
      }
      break;
    }
    case SM50BindingType::ConstantBuffer:
      DXMT_UNREACHABLE
    }
  }
}

template <PipelineStage stage, PipelineKind kind>
void
ArgumentEncodingContext::verifyBindlessBufferTable(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    const MTL_SM50_SHADER_ARGUMENT *arguments, const uint64_t *buf_table,
    uint32_t buf_table_qwords, const uint32_t *cb_compact, const uint32_t *res_compact,
    uint64_t verify_draw_id, uint64_t verify_draw_serial) {
  if (!BindlessMirrorVerifyEnabled() || !buf_table || !buf_table_qwords)
    return;

  auto log_mismatch = [&](const MTL_SM50_SHADER_ARGUMENT &arg, uint32_t qw_base,
                          const char *binding, const char *field,
                          uint64_t expected, uint64_t actual) {
    if (expected == actual || !BindlessMirrorVerifyShouldLog())
      return;
    ERR("DXMT bindless-mirror VERIFY mismatch (buf_table)"
        " draw=", verify_draw_id,
        " drawSerial=", verify_draw_serial,
        " stage=", BindlessVerifyStageName(stage),
        " arg=", arg.StructurePtrOffset,
        " qword=", qw_base,
        " binding=", binding,
        " field=", field,
        " expected=0x", std::hex, expected,
        " actual=0x", actual, std::dec);
  };

  for (unsigned i = 0; i < reflection->NumConstantBuffers; i++) {
    const auto &arg = constant_buffers[i];
    const uint32_t qw_base = cb_compact[i];
    if (qw_base + 1 >= buf_table_qwords)
      continue;

    uint64_t expected_address = dummy_cbuffer_info_.gpu_address;
    auto slot = 14 * unsigned(stage) + arg.SM50BindingSlot;
    auto &cbuf = cbuf_[slot];
    if (cbuf.direct_buffer) {
      expected_address = cbuf.direct_gpu_address + cbuf.offset;
    } else if (cbuf.buffer.ptr()) {
      auto argbuf = cbuf.buffer;
      auto valid_length = argbuf->length() > cbuf.offset ? argbuf->length() - cbuf.offset : 0;
      auto [argbuf_alloc, argbuf_offset] = access<stage>(argbuf, cbuf.offset, valid_length, ResourceAccess::Read);
      expected_address = argbuf_alloc->gpuAddress() + argbuf_offset + cbuf.offset;
    }
    log_mismatch(arg, qw_base, "CBV", "gpuAddr", expected_address, buf_table[qw_base]);
    log_mismatch(arg, qw_base + 1, "CBV", "meta", 0, buf_table[qw_base + 1]);
  }

  auto &UAVBindingSet = stage == PipelineStage::Compute ? cs_uav_ : om_uav_;
  for (unsigned i = 0; i < reflection->NumArguments; i++) {
    const auto &arg = arguments[i];
    const uint32_t compact = res_compact[i];
    if (compact == kNotABufferTableField || compact + 1 >= buf_table_qwords)
      continue;

    if (arg.Type == SM50BindingType::SRV &&
        (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER)) {
      const auto slot = kSRVBindings * unsigned(stage) + arg.SM50BindingSlot;
      const auto count = BufferTableRangeQwordCount(arg.RegisterCount) /
                         kBufferTableQwordsPerDescriptor;
      for (uint32_t local = 0; local < count; local++) {
        const auto local_slot = slot + local;
        if (local_slot >= kSRVBindings * (unsigned(stage) + 1))
          break;
        const auto qw_base = compact + local * kBufferTableQwordsPerDescriptor;
        if (qw_base + 1 >= buf_table_qwords)
          break;
        auto &srv = resview_[local_slot];
        uint64_t expected_address = 0;
        uint64_t expected_meta = 0;
        if (srv.buffer.ptr()) {
          auto [srv_alloc, offset] = access<stage>(
              srv.buffer, srv.slice.byteOffset, srv.slice.byteLength,
              ResourceAccess::Read);
          expected_address =
              srv_alloc->gpuAddress() + offset + srv.slice.byteOffset;
          expected_meta = srv.slice.byteLength;
        }
        log_mismatch(arg, qw_base, "SRV", "gpuAddr", expected_address,
                     buf_table[qw_base]);
        log_mismatch(arg, qw_base + 1, "SRV", "meta", expected_meta,
                     buf_table[qw_base + 1]);
      }
    } else if (arg.Type == SM50BindingType::UAV &&
               (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER)) {
      bool read = (arg.Flags >> 10) & 1, write = (arg.Flags >> 10) & 2;
      if (!read && !write) {
        read = true;
        write = true;
      }
      int access_flags = (read ? ResourceAccess::Read : 0) |
                         (write ? ResourceAccess::Write : 0) |
                         ResourceAccess::UAV;
      const auto count = BufferTableRangeQwordCount(arg.RegisterCount) /
                         kBufferTableQwordsPerDescriptor;
      for (uint32_t local = 0; local < count; local++) {
        const auto local_slot = arg.SM50BindingSlot + local;
        if (local_slot >= kUAVBindings)
          break;
        const auto qw_base = compact + local * kBufferTableQwordsPerDescriptor;
        if (qw_base + 1 >= buf_table_qwords)
          break;
        auto &uav = UAVBindingSet[local_slot];
        uint64_t expected_address = 0;
        uint64_t expected_meta = 0;
        if (uav.buffer.ptr()) {
          auto [uav_alloc, offset] = access<stage>(
              uav.buffer, uav.slice.byteOffset, uav.slice.byteLength,
              access_flags);
          expected_address =
              uav_alloc->gpuAddress() + offset + uav.slice.byteOffset;
          expected_meta = uav.slice.byteLength;
        }
        log_mismatch(arg, qw_base, "UAV", "gpuAddr", expected_address,
                     buf_table[qw_base]);
        log_mismatch(arg, qw_base + 1, "UAV", "meta", expected_meta,
                     buf_table[qw_base + 1]);

        if (!(arg.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER))
          continue;
        const auto counter_qw = qw_base + 2;
        if (counter_qw >= buf_table_qwords)
          continue;
        uint64_t expected_counter = 0;
        if (uav.counter) {
          auto [counter_alloc, offset] = access<stage>(uav.counter, 0, 4, ResourceAccess::All);
          expected_counter = counter_alloc->gpuAddress() + offset;
        }
        log_mismatch(arg, counter_qw, "UAV", "counter", expected_counter,
                     buf_table[counter_qw]);
      }
    }
  }
}

template void ArgumentEncodingContext::packBindlessCBuffers<PipelineStage::Vertex, PipelineKind::Ordinary>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, uint64_t *, uint32_t, const uint32_t *, const ConstantBufferBindingSnapshot *);
template void ArgumentEncodingContext::packBindlessCBuffers<PipelineStage::Pixel, PipelineKind::Ordinary>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, uint64_t *, uint32_t, const uint32_t *, const ConstantBufferBindingSnapshot *);
template void ArgumentEncodingContext::packBindlessCBuffers<PipelineStage::Vertex, PipelineKind::Tessellation>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, uint64_t *, uint32_t, const uint32_t *, const ConstantBufferBindingSnapshot *);
template void ArgumentEncodingContext::packBindlessCBuffers<PipelineStage::Pixel, PipelineKind::Tessellation>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, uint64_t *, uint32_t, const uint32_t *, const ConstantBufferBindingSnapshot *);
template void ArgumentEncodingContext::packBindlessCBuffers<PipelineStage::Hull, PipelineKind::Tessellation>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, uint64_t *, uint32_t, const uint32_t *, const ConstantBufferBindingSnapshot *);
template void ArgumentEncodingContext::packBindlessCBuffers<PipelineStage::Domain, PipelineKind::Tessellation>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, uint64_t *, uint32_t, const uint32_t *, const ConstantBufferBindingSnapshot *);
template void ArgumentEncodingContext::packBindlessCBuffers<PipelineStage::Compute, PipelineKind::Ordinary>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, uint64_t *, uint32_t, const uint32_t *, const ConstantBufferBindingSnapshot *);
template void ArgumentEncodingContext::packBindlessCBuffers<PipelineStage::Vertex, PipelineKind::Geometry>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, uint64_t *, uint32_t, const uint32_t *, const ConstantBufferBindingSnapshot *);
template void ArgumentEncodingContext::packBindlessCBuffers<PipelineStage::Geometry, PipelineKind::Geometry>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, uint64_t *, uint32_t, const uint32_t *, const ConstantBufferBindingSnapshot *);
template void ArgumentEncodingContext::packBindlessCBuffers<PipelineStage::Pixel, PipelineKind::Geometry>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, uint64_t *, uint32_t, const uint32_t *, const ConstantBufferBindingSnapshot *);
template void ArgumentEncodingContext::packBindlessBufferTable<PipelineStage::Vertex, PipelineKind::Ordinary>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t *, uint32_t, const uint32_t *, const BindlessBufferTableSnapshot *, uint64_t, uint64_t);
template void ArgumentEncodingContext::packBindlessBufferTable<PipelineStage::Pixel, PipelineKind::Ordinary>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t *, uint32_t, const uint32_t *, const BindlessBufferTableSnapshot *, uint64_t, uint64_t);
template void ArgumentEncodingContext::packBindlessBufferTable<PipelineStage::Vertex, PipelineKind::Tessellation>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t *, uint32_t, const uint32_t *, const BindlessBufferTableSnapshot *, uint64_t, uint64_t);
template void ArgumentEncodingContext::packBindlessBufferTable<PipelineStage::Pixel, PipelineKind::Tessellation>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t *, uint32_t, const uint32_t *, const BindlessBufferTableSnapshot *, uint64_t, uint64_t);
template void ArgumentEncodingContext::packBindlessBufferTable<PipelineStage::Hull, PipelineKind::Tessellation>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t *, uint32_t, const uint32_t *, const BindlessBufferTableSnapshot *, uint64_t, uint64_t);
template void ArgumentEncodingContext::packBindlessBufferTable<PipelineStage::Domain, PipelineKind::Tessellation>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t *, uint32_t, const uint32_t *, const BindlessBufferTableSnapshot *, uint64_t, uint64_t);
template void ArgumentEncodingContext::packBindlessBufferTable<PipelineStage::Compute, PipelineKind::Ordinary>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t *, uint32_t, const uint32_t *, const BindlessBufferTableSnapshot *, uint64_t, uint64_t);
template void ArgumentEncodingContext::packBindlessBufferTable<PipelineStage::Vertex, PipelineKind::Geometry>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t *, uint32_t, const uint32_t *, const BindlessBufferTableSnapshot *, uint64_t, uint64_t);
template void ArgumentEncodingContext::packBindlessBufferTable<PipelineStage::Geometry, PipelineKind::Geometry>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t *, uint32_t, const uint32_t *, const BindlessBufferTableSnapshot *, uint64_t, uint64_t);
template void ArgumentEncodingContext::packBindlessBufferTable<PipelineStage::Pixel, PipelineKind::Geometry>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t *, uint32_t, const uint32_t *, const BindlessBufferTableSnapshot *, uint64_t, uint64_t);

// ---- bindless-mirror (Stage-1 sub-step ③.3) live per-draw wiring ----
//
// packBindlessStage: allocate + fill ONE stage's slot-27 buf_table for the current draw.
// The compact bases come from BuildBufferTableCompactBases — the SAME walk airconv bakes its
// compile-time compact_base from (dxmt_bindless_buffer_table.hpp / BINDLESS-ABI.md §4.5) — so
// the runtime fill and the shader's indexing agree without any communication. Runs inside the
// encode path so the packers' access<>()/makeResident calls bind to encoder_current.
template <PipelineStage stage, PipelineKind kind>
AllocatedArgumentBufferSlice
ArgumentEncodingContext::packBindlessStage(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    const MTL_SM50_SHADER_ARGUMENT *arguments, const std::string &shader_hash,
    uint64_t verify_draw_id, uint64_t verify_draw_serial,
    const BindlessBufferTableSnapshot *bindings,
    uint64_t demote_msaa_srv_mask_lo,
    uint64_t demote_msaa_srv_mask_hi,
    const char *diagnostic_path
) {
  const uint32_t num_cbuffers = reflection->NumConstantBuffers;
  const uint32_t num_arguments = reflection->NumArguments;

  // Compute compact bases + total qword count via the shared walk. cb_bases[i] = CBV i's base;
  // res_bases[i] = resource arg i's base (kNotABufferTableField for tex/sampler args).
  std::vector<uint32_t> cb_bases(num_cbuffers ? num_cbuffers : 1);
  std::vector<uint32_t> res_bases(num_arguments ? num_arguments : 1);
  const uint32_t qword_count = BuildBufferTableCompactBases(
      constant_buffers, num_cbuffers, arguments, num_arguments, cb_bases.data(), res_bases.data()
  );

  const auto log_binding = [&](const char *result) {
    if (!DebugShouldLogBinding(shader_hash))
      return;
    INFO("DXMT diagnostic: bindless shader binding",
         " stage=", BindlessVerifyStageName(stage),
         " kind=", uint32_t(kind),
         " shader=", shader_hash,
         " encoder=", currentEncoderId(),
         " path=", diagnostic_path ? diagnostic_path : "unknown",
         " qwords=", qword_count,
         " result=", result);
  };

  if (!qword_count) {
    // No buffer fields in this stage; tex/sampler still need residency tracked. Run the
    // packers anyway (with a null/zero-size buf_table) so the access<>()/makeResident for
    // textures/samplers happens exactly as legacy; the write_slot bounds-guard (qw_base+1 <
    // buf_table_qwords == 0) skips every write. The caller still binds the mirrors.
    if (num_arguments && arguments)
      packBindlessBufferTable<stage, kind>(reflection, arguments, shader_hash, nullptr, 0,
                                           res_bases.data(), bindings,
                                           demote_msaa_srv_mask_lo,
                                           demote_msaa_srv_mask_hi);
    log_binding("empty");
    return {};
  }

  auto bt = queue_.AllocateArgumentBuffer(currentSeqId(), uint64_t(qword_count) * sizeof(uint64_t));
  if (!bt.valid()) {
    log_binding("allocation-failed");
    return {};
  }
  auto *buf_table = bt.map<uint64_t>(0, qword_count);
  if (!buf_table || !bt.fill_zero()) {
    log_binding("allocation-failed");
    return {};
  }

  if (num_cbuffers && constant_buffers)
    packBindlessCBuffers<stage, kind>(reflection, constant_buffers, buf_table, qword_count,
                                      cb_bases.data(), bindings ? bindings->constant_buffers : nullptr);
  if (num_arguments && arguments)
    packBindlessBufferTable<stage, kind>(reflection, arguments, shader_hash, buf_table, qword_count,
                                         res_bases.data(), bindings,
                                         demote_msaa_srv_mask_lo,
                                         demote_msaa_srv_mask_hi);

  verifyBindlessBufferTable<stage, kind>(
      reflection, constant_buffers, arguments, buf_table, qword_count,
      cb_bases.data(), res_bases.data(), verify_draw_id, verify_draw_serial);

  bt.flush_if_needed();
  log_binding("bound");
  return bt;
}

// bindBindlessTables: emit this draw's deferred table binds. The standard layout is 27..30;
// tessellation VS uses 23..26 to coexist with HS in one object function. Null buffers are
// skipped (the per-pass argbuf or a
// prior bind stays at that slot; the shader for this PSO does not read a skipped slot). Sets the
// mixed-PSO guard flag after binding mirrors.
//
// RESIDENCY (③.4): NO useResource is emitted here for the four bound buffers, and none is
// needed. All four are bound DIRECTLY to the encoder via setArgumentBuffer, so they are
// auto-resident in DXMT's model (DXMT has NO MTLResidencySet / requestResidency / useHeap —
// grep src/dxmt src/winemetal src/d3d12; the only residency API is per-encoder useResource for
// INDIRECTLY-accessed resources). Proof: the legacy per-pass argbuf is bound directly at slots
// 16/29/30 (FlushRenderEncoderArgumentBuffer / appendRenderArgumentBufferBindings) and is NEVER
// passed to useResource — directly-bound argument buffers do not need it. buf_table/root_offsets
// come from the SAME argbuf ring (argbuf_allocator, dxmt_command_queue.cpp:114) as that legacy
// argbuf; the two mirrors are separate persistent device.newBuffer allocations
// (d3d12_descriptor_mirror.cpp:19) with the SAME storage/hazard options (Shared|Untracked) and
// the same heap-less newBuffer path the ring blocks use (dxmt_ring_bump_allocator.hpp:92/146) —
// so they are resident by virtue of the direct bind, exactly like the ring.
//
// The resources these four point to ARE accessed indirectly and DO need residency, but it is
// already established elsewhere (do NOT re-emit it here): the ③.1 packers
// (packBindlessCBuffers / packBindlessBufferTable) call access<>()/makeResident on every CBV /
// SRV/UAV BUFFER they pack into buf_table, and on every mirrored TEXTURE this draw indexes (the
// shader only reaches textures via reflection args, which those packers walk). FillBindlessMirror
// Slot (d3d12_command_queue.cpp:10261) only writes the handle bytes once-per-change; the per-draw
// residency for those textures is the packer's makeResident. (Stage-2 may replace the per-resource
// makeResident with a single useResource over the whole mirror — explicitly deferred, not done
// here.)
template <PipelineStage stage, PipelineKind kind>
static constexpr WMTRenderStages
BindlessRenderStages() {
  if constexpr (stage == PipelineStage::Vertex) {
    if constexpr (kind == PipelineKind::Ordinary)
      return WMTRenderStageVertex;
    else
      return WMTRenderStageObject;
  } else if constexpr (stage == PipelineStage::Pixel) {
    return WMTRenderStageFragment;
  } else if constexpr (stage == PipelineStage::Hull) {
    return WMTRenderStageObject;
  } else if constexpr (stage == PipelineStage::Geometry ||
                       stage == PipelineStage::Domain) {
    return WMTRenderStageMesh;
  } else {
    static_assert(stage != PipelineStage::Compute,
                  "compute bindless tables use a compute encoder");
    return {};
  }
}

template <PipelineStage stage, PipelineKind kind>
void
ArgumentEncodingContext::bindBindlessTables(
    const AllocatedArgumentBufferSlice &buf_table,
    const AllocatedArgumentBufferSlice &root_offsets,
    const AllocatedArgumentBufferSlice &tex_mirror,
    const AllocatedArgumentBufferSlice &sampler_mirror
) {
  if constexpr (stage == PipelineStage::Compute) {
    invalidateNativeArgumentBuffers(true);
    auto bind = [&](WMT::Buffer buffer, uint64_t offset, uint8_t index) {
      if (!buffer)
        return;
      retainResourceForCurrentCommandBuffer(buffer);
      auto &cmd = encodeComputeCommand<wmtcmd_compute_setargumentbuffer>();
      cmd.type = WMTComputeCommandSetArgumentBuffer;
      cmd.buffer = buffer;
      cmd.offset = offset;
      cmd.index = index;
    };
    bind(buf_table.gpu_buffer, buf_table.offset, 27);
    bind(root_offsets.gpu_buffer, root_offsets.offset, 28);
    bind(sampler_mirror.gpu_buffer, sampler_mirror.offset, 29);
    bind(tex_mirror.gpu_buffer, tex_mirror.offset, 30);
    for (auto &state : static_cast<ComputeEncoderData *>(encoder_current)->argument_buffer_offsets) {
      if (state.valid && state.index >= 27 && state.index <= 30)
        state.valid = false;
    }
  } else {
    constexpr WMTRenderStages stages = BindlessRenderStages<stage, kind>();
    constexpr uint8_t first_bind_index =
        stage == PipelineStage::Vertex && kind == PipelineKind::Tessellation
            ? 23
            : 27;
    invalidateNativeArgumentBuffers(false, stages);
    auto bind = [&](WMT::Buffer buffer, uint64_t offset, uint8_t index) {
      if (!buffer)
        return;
      retainResourceForCurrentCommandBuffer(buffer);
      auto &cmd = encodeRenderCommand<wmtcmd_render_setargumentbuffer>();
      cmd.type = WMTRenderCommandSetArgumentBuffer;
      cmd.buffer = buffer;
      cmd.offset = offset;
      cmd.index = index;
      cmd.stages = stages;
    };
    bind(buf_table.gpu_buffer, buf_table.offset, first_bind_index);
    bind(root_offsets.gpu_buffer, root_offsets.offset, first_bind_index + 1);
    bind(sampler_mirror.gpu_buffer, sampler_mirror.offset,
         first_bind_index + 2);
    bind(tex_mirror.gpu_buffer, tex_mirror.offset, first_bind_index + 3);
    for (auto &state : currentRenderEncoder()->argument_buffer_offsets) {
      if (state.valid && (state.stages & stages) &&
          state.index >= first_bind_index &&
          state.index <= first_bind_index + 3)
        state.valid = false;
    }
  }
}

template <PipelineStage stage>
void
ArgumentEncodingContext::bindNativeRootTableBases(
    const AllocatedArgumentBufferSlice &root_bases, uint32_t bind_index) {
  if (!root_bases.gpu_buffer)
    return;

  if constexpr (stage == PipelineStage::Compute) {
    bindNativeArgumentBuffer(root_bases.gpu_buffer, root_bases.offset,
                             bind_index, true);
  } else {
    constexpr WMTRenderStages stages =
        stage == PipelineStage::Pixel ? WMTRenderStageFragment
                                      : WMTRenderStageVertex;
    bindNativeArgumentBuffer(root_bases.gpu_buffer, root_bases.offset,
                             bind_index, false, stages);
  }
}

void
ArgumentEncodingContext::bindNativeArgumentBuffer(
    WMT::Buffer buffer, uint64_t offset, uint32_t bind_index, bool compute,
    WMTRenderStages render_stages) {
  if (!buffer || bind_index >= 31)
    return;
  retainResourceForCurrentCommandBuffer(buffer);

  if (compute) {
    auto *data = static_cast<ComputeEncoderData *>(encoder_current);
    auto &cached = data->native_argument_buffers[bind_index];
    if (cached.valid && cached.buffer_handle == buffer.handle &&
        cached.offset == offset)
      return;
    cached = {buffer.handle, offset, true};

    auto &cmd = encodeComputeCommand<wmtcmd_compute_setargumentbuffer>();
    cmd.type = WMTComputeCommandSetArgumentBuffer;
    cmd.buffer = buffer;
    cmd.offset = offset;
    cmd.index = bind_index;
    auto &use = encodeComputeCommand<wmtcmd_compute_useresource>();
    use.type = WMTComputeCommandUseResource;
    use.resource = buffer;
    use.usage = WMTResourceUsageRead;
    for (auto &state : data->argument_buffer_offsets) {
      if (state.valid && state.index == bind_index)
        state.valid = false;
    }
    return;
  }

  auto *data = currentRenderEncoder();
  WMTRenderStages changed_stages = {};
  auto update = [&](WMTRenderStages stage,
                    std::array<NativeArgumentBufferBindingState, 31> &cache) {
    if (!(render_stages & stage))
      return;
    auto &cached = cache[bind_index];
    if (cached.valid && cached.buffer_handle == buffer.handle &&
        cached.offset == offset)
      return;
    cached = {buffer.handle, offset, true};
    changed_stages = static_cast<WMTRenderStages>(changed_stages | stage);
  };
  update(WMTRenderStageVertex, data->native_argument_buffers_vertex);
  update(WMTRenderStageFragment, data->native_argument_buffers_fragment);
  if (!changed_stages)
    return;

  auto &cmd = encodeRenderCommand<wmtcmd_render_setargumentbuffer>();
  cmd.type = WMTRenderCommandSetArgumentBuffer;
  cmd.buffer = buffer;
  cmd.offset = offset;
  cmd.index = bind_index;
  cmd.stages = changed_stages;
  auto &use = encodeRenderCommand<wmtcmd_render_useresource>();
  use.type = WMTRenderCommandUseResource;
  use.resource = buffer;
  use.usage = WMTResourceUsageRead;
  use.stages = changed_stages;
  for (auto &state : data->argument_buffer_offsets) {
    if (state.valid && (state.stages & changed_stages) &&
        state.index == bind_index)
      state.valid = false;
  }
}

void
ArgumentEncodingContext::bindNativeNullConstantBuffer(
    bool compute, WMTRenderStages render_stages) {
  bindNativeArgumentBuffer(
      dummy_cbuffer_, 0, DXMT12_MTL4_NATIVE_NULL_CBUFFER_BIND_INDEX,
      compute, render_stages);
}

void
ArgumentEncodingContext::bindNativeNullBuffer(
    bool compute, WMTRenderStages render_stages) {
  bindNativeArgumentBuffer(
      dummy_cbuffer_, 0, DXMT12_MTL4_NATIVE_NULL_BUFFER_BIND_INDEX,
      compute, render_stages);
}

void
ArgumentEncodingContext::diagnoseNativeShaderBinding(
    PipelineStage stage, const std::string &shader_hash, const char *path,
    const AllocatedArgumentBufferSlice &cbuffer_root_bases,
    const AllocatedArgumentBufferSlice &resource_root_bases) {
  if (!DebugShouldLogBinding(shader_hash))
    return;

  INFO("DXMT diagnostic: native shader binding",
       " stage=", BindlessVerifyStageName(stage),
       " shader=", shader_hash,
       " encoder=", currentEncoderId(),
       " path=", path ? path : "unknown",
       " cbufferGpuAddress=0x", std::hex,
       cbuffer_root_bases.gpu_address,
       " cbufferOffset=0x", cbuffer_root_bases.offset,
       " cbufferLength=0x", cbuffer_root_bases.length,
       " resourceGpuAddress=0x", resource_root_bases.gpu_address,
       " resourceOffset=0x", resource_root_bases.offset,
       " resourceLength=0x", resource_root_bases.length, std::dec);
}

void
ArgumentEncodingContext::invalidateNativeArgumentBuffers(
    bool compute, WMTRenderStages render_stages) {
  if (compute) {
    for (auto &state :
         static_cast<ComputeEncoderData *>(encoder_current)
             ->native_argument_buffers)
      state.valid = false;
    return;
  }

  auto *data = currentRenderEncoder();
  if (render_stages & WMTRenderStageVertex) {
    for (auto &state : data->native_argument_buffers_vertex)
      state.valid = false;
  }
  if (render_stages & WMTRenderStageFragment) {
    for (auto &state : data->native_argument_buffers_fragment)
      state.valid = false;
  }
}

template AllocatedArgumentBufferSlice ArgumentEncodingContext::packBindlessStage<PipelineStage::Vertex, PipelineKind::Ordinary>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t, uint64_t, const BindlessBufferTableSnapshot *, uint64_t, uint64_t, const char *);
template AllocatedArgumentBufferSlice ArgumentEncodingContext::packBindlessStage<PipelineStage::Pixel, PipelineKind::Ordinary>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t, uint64_t, const BindlessBufferTableSnapshot *, uint64_t, uint64_t, const char *);
template AllocatedArgumentBufferSlice ArgumentEncodingContext::packBindlessStage<PipelineStage::Compute, PipelineKind::Ordinary>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t, uint64_t, const BindlessBufferTableSnapshot *, uint64_t, uint64_t, const char *);
template AllocatedArgumentBufferSlice ArgumentEncodingContext::packBindlessStage<PipelineStage::Vertex, PipelineKind::Geometry>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t, uint64_t, const BindlessBufferTableSnapshot *, uint64_t, uint64_t, const char *);
template AllocatedArgumentBufferSlice ArgumentEncodingContext::packBindlessStage<PipelineStage::Geometry, PipelineKind::Geometry>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t, uint64_t, const BindlessBufferTableSnapshot *, uint64_t, uint64_t, const char *);
template AllocatedArgumentBufferSlice ArgumentEncodingContext::packBindlessStage<PipelineStage::Pixel, PipelineKind::Geometry>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t, uint64_t, const BindlessBufferTableSnapshot *, uint64_t, uint64_t, const char *);
template AllocatedArgumentBufferSlice ArgumentEncodingContext::packBindlessStage<PipelineStage::Vertex, PipelineKind::Tessellation>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t, uint64_t, const BindlessBufferTableSnapshot *, uint64_t, uint64_t, const char *);
template AllocatedArgumentBufferSlice ArgumentEncodingContext::packBindlessStage<PipelineStage::Hull, PipelineKind::Tessellation>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t, uint64_t, const BindlessBufferTableSnapshot *, uint64_t, uint64_t, const char *);
template AllocatedArgumentBufferSlice ArgumentEncodingContext::packBindlessStage<PipelineStage::Domain, PipelineKind::Tessellation>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t, uint64_t, const BindlessBufferTableSnapshot *, uint64_t, uint64_t, const char *);
template AllocatedArgumentBufferSlice ArgumentEncodingContext::packBindlessStage<PipelineStage::Pixel, PipelineKind::Tessellation>(const MTL_SHADER_REFLECTION *, const MTL_SM50_SHADER_ARGUMENT *, const MTL_SM50_SHADER_ARGUMENT *, const std::string &, uint64_t, uint64_t, const BindlessBufferTableSnapshot *, uint64_t, uint64_t, const char *);
template void ArgumentEncodingContext::bindBindlessTables<PipelineStage::Vertex, PipelineKind::Ordinary>(const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &);
template void ArgumentEncodingContext::bindBindlessTables<PipelineStage::Pixel, PipelineKind::Ordinary>(const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &);
template void ArgumentEncodingContext::bindBindlessTables<PipelineStage::Compute, PipelineKind::Ordinary>(const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &);
template void ArgumentEncodingContext::bindBindlessTables<PipelineStage::Vertex, PipelineKind::Geometry>(const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &);
template void ArgumentEncodingContext::bindBindlessTables<PipelineStage::Geometry, PipelineKind::Geometry>(const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &);
template void ArgumentEncodingContext::bindBindlessTables<PipelineStage::Pixel, PipelineKind::Geometry>(const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &);
template void ArgumentEncodingContext::bindBindlessTables<PipelineStage::Vertex, PipelineKind::Tessellation>(const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &);
template void ArgumentEncodingContext::bindBindlessTables<PipelineStage::Hull, PipelineKind::Tessellation>(const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &);
template void ArgumentEncodingContext::bindBindlessTables<PipelineStage::Domain, PipelineKind::Tessellation>(const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &);
template void ArgumentEncodingContext::bindBindlessTables<PipelineStage::Pixel, PipelineKind::Tessellation>(const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &, const AllocatedArgumentBufferSlice &);
template void ArgumentEncodingContext::bindNativeRootTableBases<PipelineStage::Vertex>(const AllocatedArgumentBufferSlice &, uint32_t);
template void ArgumentEncodingContext::bindNativeRootTableBases<PipelineStage::Pixel>(const AllocatedArgumentBufferSlice &, uint32_t);
template void ArgumentEncodingContext::bindNativeRootTableBases<PipelineStage::Compute>(const AllocatedArgumentBufferSlice &, uint32_t);
template void ArgumentEncodingContext::bindNativeRootTableBases<PipelineStage::Geometry>(const AllocatedArgumentBufferSlice &, uint32_t);
template void ArgumentEncodingContext::bindNativeRootTableBases<PipelineStage::Hull>(const AllocatedArgumentBufferSlice &, uint32_t);
template void ArgumentEncodingContext::bindNativeRootTableBases<PipelineStage::Domain>(const AllocatedArgumentBufferSlice &, uint32_t);

inline uint64_t
TextureMetadata(uint32_t array_length, float min_lod) {
  return ((uint64_t)array_length << 32) | (uint64_t)std::bit_cast<uint32_t>(min_lod);
}

static constexpr uint32_t kDummyTextureKindCount = 7;
static constexpr uint32_t kDummyTextureFormatCount = 4;

enum class DummyTextureKind : uint32_t {
  Texture2D = 0,
  Texture2DArray = 1,
  Texture2DMultisampled = 2,
  Texture2DMultisampledArray = 3,
  Texture3D = 4,
  TextureCube = 5,
  TextureCubeArray = 6,
};

enum class DummyTextureFormat : uint32_t {
  Float = 0,
  Uint = 1,
  Sint = 2,
  Depth = 3,
};

static DummyTextureKind
DummyTextureKindForArgument(const MTL_SM50_SHADER_ARGUMENT &arg) {
  bool is_array = arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY;
  if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_CUBE)
    return is_array ? DummyTextureKind::TextureCubeArray : DummyTextureKind::TextureCube;
  if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_3D)
    return DummyTextureKind::Texture3D;
  if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLED)
    return is_array ? DummyTextureKind::Texture2DMultisampledArray : DummyTextureKind::Texture2DMultisampled;
  return is_array ? DummyTextureKind::Texture2DArray : DummyTextureKind::Texture2D;
}

static DummyTextureFormat
DummyTextureFormatForArgument(const MTL_SM50_SHADER_ARGUMENT &arg) {
  if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_DEPTH)
    return DummyTextureFormat::Depth;
  if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_SINT)
    return DummyTextureFormat::Sint;
  if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_UINT)
    return DummyTextureFormat::Uint;
  return DummyTextureFormat::Float;
}

static uint32_t
DummyTextureIndex(DummyTextureKind kind, DummyTextureFormat format) {
  return uint32_t(format) * kDummyTextureKindCount + uint32_t(kind);
}

static WMTPixelFormat
DummyTexturePixelFormat(DummyTextureFormat format) {
  switch (format) {
  case DummyTextureFormat::Uint:
    return WMTPixelFormatRGBA32Uint;
  case DummyTextureFormat::Sint:
    return WMTPixelFormatRGBA32Sint;
  case DummyTextureFormat::Depth:
    return WMTPixelFormatDepth32Float;
  case DummyTextureFormat::Float:
  default:
    return WMTPixelFormatRGBA32Float;
  }
}

static WMTTextureType
DummyTextureType(DummyTextureKind kind) {
  switch (kind) {
  case DummyTextureKind::Texture2DArray:
    return WMTTextureType2DArray;
  case DummyTextureKind::Texture2DMultisampled:
    return WMTTextureType2DMultisample;
  case DummyTextureKind::Texture2DMultisampledArray:
    return WMTTextureType2DMultisampleArray;
  case DummyTextureKind::Texture3D:
    return WMTTextureType3D;
  case DummyTextureKind::TextureCube:
    return WMTTextureTypeCube;
  case DummyTextureKind::TextureCubeArray:
    return WMTTextureTypeCubeArray;
  case DummyTextureKind::Texture2D:
  default:
    return WMTTextureType2D;
  }
}

static uint32_t
DummyTextureArrayLength(DummyTextureKind kind) {
  switch (kind) {
  case DummyTextureKind::Texture2DArray:
  case DummyTextureKind::Texture2DMultisampledArray:
  case DummyTextureKind::TextureCubeArray:
    return 1;
  default:
    return 1;
  }
}

static bool
DebugEnabledEnv(const char *name) {
  auto value = env::getEnvVar(name);
  return value == "1" || value == "true" || value == "yes" || value == "trace";
}

static bool
DebugHangDenseEnabled() {
  static const bool enabled =
      DebugEnabledEnv("DXMT_DIAG_GPU_HANG_DENSE") ||
      DebugEnabledEnv("DXMT_DIAG_ROOT_CAUSE_DENSE") ||
      DebugEnabledEnv("DXMT_VALIDATION") ||
      DebugEnabledEnv("DXMT_DIAG_VALIDATION");
  return enabled;
}

static bool
DebugShouldLogArgumentTableSliceCache() {
  static const bool enabled = DebugEnabledEnv("DXMT_DIAG_ARGUMENT_TABLE_CACHE");
  return enabled;
}

static uint64_t
HashArgumentTableSlice(const uint8_t *bytes, uint64_t size) {
  constexpr uint64_t kFnvOffset = 14695981039346656037ull;
  constexpr uint64_t kFnvPrime = 1099511628211ull;
  uint64_t hash = kFnvOffset;
  for (uint64_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= kFnvPrime;
  }
  return hash;
}

static ArgumentTableSliceCache *
RenderArgumentTableSliceCache(RenderEncoderData *data, WMTRenderStages stages) {
  switch (stages) {
  case WMTRenderStageVertex:
    return &data->argument_table_cache_vertex;
  case WMTRenderStageFragment:
    return &data->argument_table_cache_fragment;
  case WMTRenderStageObject:
    return &data->argument_table_cache_object;
  case WMTRenderStageMesh:
    return &data->argument_table_cache_mesh;
  default:
    return nullptr;
  }
}

static uint64_t
DeduplicateArgumentTableSlice(
    ArgumentTableSliceCache &cache, uint8_t index, uint64_t offset, uint64_t size,
    const uint8_t *base, const uint8_t *bytes
) {
  if (!size || index >= cache.entries.size())
    return offset;

  cache.lookups++;
  const auto hash = HashArgumentTableSlice(bytes, size);
  auto &entry = cache.entries[index];
  if (entry.valid && entry.size == size && entry.hash == hash &&
      !std::memcmp(base + entry.offset, bytes, size)) {
    cache.hits++;
    cache.bytes_avoided += size;
    cache.hits_by_index[index]++;
    return entry.offset;
  }

  cache.misses++;
  entry.valid = true;
  entry.hash = hash;
  entry.offset = offset;
  entry.size = static_cast<uint32_t>(size);
  return offset;
}

static void
DebugLogArgumentTableSliceCache(
    const char *kind, const char *stage, uint64_t frame_id, uint64_t seq_id,
    uint64_t encoder_id, const ArgumentTableSliceCache &cache
) {
  if (!DebugShouldLogArgumentTableSliceCache() || !cache.lookups)
    return;

  std::stringstream hit_distribution;
  bool first = true;
  for (unsigned i = 0; i < cache.hits_by_index.size(); i++) {
    if (!cache.hits_by_index[i])
      continue;
    if (!first)
      hit_distribution << ",";
    hit_distribution << i << ":" << cache.hits_by_index[i];
    first = false;
  }

  INFO(
      "DXMT diagnostic: argument table slice cache",
      " kind=", kind,
      " stage=", stage,
      " frame=", frame_id,
      " seq=", seq_id,
      " encoder=", encoder_id,
      " lookups=", cache.lookups,
      " hits=", cache.hits,
      " misses=", cache.misses,
      " bytesAvoided=", cache.bytes_avoided,
      " hitByIndex=", first ? "-" : hit_distribution.str()
  );
}

uint64_t
ArgumentEncodingContext::deduplicateRenderArgumentTableSlice(
    WMTRenderStages stages, uint8_t index, uint64_t offset, uint64_t size
) {
  auto *bytes = getMappedArgumentBuffer<uint8_t>(offset, size);
  if (!bytes)
    return offset;
  return deduplicateRenderArgumentTableSliceBytes(stages, index, offset, size, bytes);
}

uint64_t
ArgumentEncodingContext::deduplicateRenderArgumentTableSliceBytes(
    WMTRenderStages stages, uint8_t index, uint64_t offset, uint64_t size, const void *bytes
) {
  auto *data = currentRenderEncoder();
  auto *cache = RenderArgumentTableSliceCache(data, stages);
  if (!cache)
    return offset;
  auto *base = getMappedArgumentBuffer<uint8_t>(0, 0);
  if (!base)
    return offset;
  return DeduplicateArgumentTableSlice(*cache, index, offset, size, base, static_cast<const uint8_t *>(bytes));
}

uint64_t
ArgumentEncodingContext::deduplicateComputeArgumentTableSlice(
    uint8_t index, uint64_t offset, uint64_t size
) {
  auto *bytes = getMappedArgumentBuffer<uint8_t, true>(offset, size);
  if (!bytes)
    return offset;
  return deduplicateComputeArgumentTableSliceBytes(index, offset, size, bytes);
}

uint64_t
ArgumentEncodingContext::deduplicateComputeArgumentTableSliceBytes(
    uint8_t index, uint64_t offset, uint64_t size, const void *bytes
) {
  auto *data = static_cast<ComputeEncoderData *>(currentEncoder());
  auto *base = getMappedArgumentBuffer<uint8_t, true>(0, 0);
  if (!base)
    return offset;
  return DeduplicateArgumentTableSlice(
      data->argument_table_cache, index, offset, size, base, static_cast<const uint8_t *>(bytes));
}

bool
ArgumentEncodingContext::shouldEmitRenderArgumentBufferOffset(
    WMTRenderStages stages, uint8_t index, uint64_t offset, bool content_cache_hit
) {
  auto &states = currentRenderEncoder()->argument_buffer_offsets;
  RenderArgumentBufferOffsetState *free_state = nullptr;
  for (auto &state : states) {
    if (!state.valid) {
      if (!free_state)
        free_state = &state;
      continue;
    }
    if (state.stages != stages || state.index != index)
      continue;
    if (content_cache_hit && state.offset == offset)
      return false;
    state.offset = offset;
    return true;
  }
  auto *state = free_state ? free_state : &states.back();
  state->stages = stages;
  state->index = index;
  state->offset = offset;
  state->valid = true;
  return true;
}

bool
ArgumentEncodingContext::shouldEmitComputeArgumentBufferOffset(
    uint8_t index, uint64_t offset, bool content_cache_hit
) {
  auto &states = static_cast<ComputeEncoderData *>(currentEncoder())->argument_buffer_offsets;
  ComputeArgumentBufferOffsetState *free_state = nullptr;
  for (auto &state : states) {
    if (!state.valid) {
      if (!free_state)
        free_state = &state;
      continue;
    }
    if (state.index != index)
      continue;
    if (content_cache_hit && state.offset == offset)
      return false;
    state.offset = offset;
    return true;
  }
  auto *state = free_state ? free_state : &states.back();
  state->index = index;
  state->offset = offset;
  state->valid = true;
  return true;
}

static void
NormalizeRenderPassInfo(WMTRenderPassInfo &info) {
  if (!info.default_raster_sample_count)
    info.default_raster_sample_count = 1;
  if (!info.render_target_array_length)
    info.render_target_array_length = 1;
  if (info.depth.texture && info.depth.depth_plane >= WMT::Texture{info.depth.texture}.depth()) {
    WARN("RenderPass guard: clamping invalid depth attachment depthPlane=",
         info.depth.depth_plane);
    info.depth.depth_plane = 0;
  }
  if (info.stencil.texture && info.stencil.depth_plane >= WMT::Texture{info.stencil.texture}.depth()) {
    WARN("RenderPass guard: clamping invalid stencil attachment depthPlane=",
         info.stencil.depth_plane);
    info.stencil.depth_plane = 0;
  }
}

static double
DebugMillis(clock::duration duration) {
  return duration.count() / 1000000.0;
}

static bool
DebugShaderHashSelected(const std::string &shader_hash) {
  auto filters = env::getEnvVar("DXMT_DIAG_SHADER_HASHES");
  if (filters.empty() || filters == "all")
    return true;

  for (auto filter : str::split(filters, ",; ")) {
    if (filter == "all")
      return true;
    if (shader_hash == filter)
      return true;
    if (shader_hash.starts_with(filter))
      return true;
  }

  return false;
}

static bool
DebugBindingDiagnosticsEnabled() {
  static const bool enabled = DebugEnabledEnv("DXMT_DIAG_BINDINGS");
  return enabled;
}

static bool
DebugShouldLogBinding(const std::string &shader_hash) {
  if (!DebugBindingDiagnosticsEnabled() ||
      (!shader_hash.empty() && !DebugShaderHashSelected(shader_hash)))
    return false;

  static std::atomic<uint32_t> count = 0;
  const auto occurrence = count.fetch_add(1, std::memory_order_relaxed) + 1;
  return occurrence <= 32 || (occurrence & (occurrence - 1)) == 0;
}

static bool
DebugShouldLogBindingAnomaly(const std::string &shader_hash,
                             std::atomic<uint32_t> &counter,
                             uint32_t &occurrence) {
  if (!DebugBindingDiagnosticsEnabled() ||
      (!shader_hash.empty() && !DebugShaderHashSelected(shader_hash)))
    return false;
  occurrence = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  return occurrence <= 128 || (occurrence & (occurrence - 1)) == 0;
}

static bool
DebugShouldLogRenderPasses() {
  static const bool enabled = DebugEnabledEnv("DXMT_DIAG_RENDER_PASS") ||
                              DebugEnabledEnv("DXMT_DIAG_DRAW_STATE");
  return enabled;
}

static bool
DebugShouldLogFlushPerf() {
  static const bool enabled =
      DebugEnabledEnv("DXMT_DIAG_DXMT_QUEUE_VERBOSE") ||
      DebugEnabledEnv("DXMT_DIAG_FLUSH_PERF");
  return enabled;
}

static bool
FenceOnlyBlitOptimizationEnabled() {
  static const bool enabled = []() {
    auto value = env::getEnvVar("DXMT_FENCE_ONLY_BLIT_OPT");
    return value != "0" && value != "false" && value != "no" &&
           value != "off";
  }();
  return enabled;
}

static bool
DebugFlushPerfShouldLog() {
  if (!DebugShouldLogFlushPerf())
    return false;

  static std::atomic<uint32_t> count = 0;
  const auto index = count.fetch_add(1, std::memory_order_relaxed);
  return index < 2048 || (index % 256) == 0;
}

static bool
DebugPresentReadbackEnabled() {
  static const bool enabled = DebugEnabledEnv("DXMT_DIAG_PRESENT_READBACK");
  return enabled;
}

static bool
DebugRenderReadbackEnabled() {
  static const bool enabled = DebugEnabledEnv("DXMT_DIAG_RENDER_READBACK");
  return enabled;
}

static bool
DebugPresentReadbackGridEnabled() {
  static const bool enabled =
      DebugEnabledEnv("DXMT_DIAG_PRESENT_READBACK_GRID");
  return enabled;
}

static uint32_t
DebugPresentReadbackGridSize() {
  static const uint32_t size = []() {
    auto value = env::getEnvVar("DXMT_DIAG_PRESENT_READBACK_GRID_SIZE");
    if (value.empty())
      return 3u;
    char *end = nullptr;
    auto parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str())
      return 3u;
    auto clamped = std::clamp<unsigned long>(parsed, 1, 33);
    return static_cast<uint32_t>(clamped | 1u);
  }();
  return size;
}

static uint32_t
DebugShouldSamplePresentReadback(uint32_t &present_index) {
  static std::atomic<uint32_t> present_count = 0;
  if (!DebugPresentReadbackEnabled())
    return false;
  present_index = present_count.fetch_add(1, std::memory_order_relaxed);
  return true;
}

static bool
DebugShouldSampleRenderReadback() {
  return DebugRenderReadbackEnabled();
}

static bool
DebugSupportsTextureReadback(WMT::Texture texture) {
  if (!texture)
    return false;
  auto format = texture.pixelFormat();
  if (!MTLGetTexelSize(format))
    return false;
  if (IsBlockCompressionFormat(format))
    return false;
  return true;
}

static void
DebugEncodeTexturePointReadback(QueryReadbacks &readbacks, WMT::CommandBuffer cmdbuf,
                                WMT::Device device, CommandQueue &queue,
                                WMT::Texture texture,
                                const char *label, uint64_t frame_id,
                                uint64_t seq_id, uint64_t encoder_id,
                                uint32_t index, uint32_t point_x,
                                uint32_t point_y, uint16_t level, uint16_t slice,
                                uint32_t width, uint32_t height) {
  if (!DebugSupportsTextureReadback(texture)) {
    INFO("DXMT diagnostic: texture readback skipped",
         " label=", label,
         " frame=", frame_id,
         " seq=", seq_id,
         " encoder=", encoder_id,
         " index=", index,
         " texture=", uint64_t(texture),
         " format=", texture ? uint32_t(texture.pixelFormat()) : 0,
         " size=", texture ? texture.width() : 0, "x", texture ? texture.height() : 0,
         " reason=unsupported_texture");
    return;
  }

  if (!width || !height) {
    INFO("DXMT diagnostic: texture readback skipped",
         " label=", label,
         " frame=", frame_id,
         " seq=", seq_id,
         " encoder=", encoder_id,
         " index=", index,
         " texture=", uint64_t(texture),
         " format=", uint32_t(texture.pixelFormat()),
         " size=", width, "x", height,
         " reason=empty_texture");
    return;
  }

  const auto texel_size = MTLGetTexelSize(texture.pixelFormat());
  const auto row_pitch = std::max<uint32_t>(256, texel_size);
  constexpr uint32_t readback_size = 256;
  WMTBufferInfo buffer_info = {};
  buffer_info.length = readback_size;
  buffer_info.options = WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked;
  buffer_info.memory.set(nullptr);
#ifdef __i386__
  buffer_info.memory.set(wsi::aligned_malloc(readback_size, DXMT_PAGE_SIZE));
#endif
  auto buffer = device.newBuffer(buffer_info);
  auto *mapped = static_cast<uint8_t *>(buffer_info.memory.get_accessible_or_null());
  if (!buffer || !mapped) {
    INFO("DXMT diagnostic: texture readback skipped",
         " label=", label,
         " frame=", frame_id,
         " seq=", seq_id,
         " encoder=", encoder_id,
         " index=", index,
         " texture=", uint64_t(texture),
         " format=", uint32_t(texture.pixelFormat()),
         " size=", width, "x", height,
         " reason=buffer_allocation_failed");
#ifdef __i386__
    wsi::aligned_free(buffer_info.memory.get_accessible_or_null());
#endif
    return;
  }
  queue.RetainReplayTemporaryResidencyUntilGpuComplete(
      ResidencyOwnership::ReplayTemporary(buffer), seq_id);

  const auto x = std::min(point_x, width - 1);
  const auto y = std::min(point_y, height - 1);

  auto encoder = cmdbuf.blitCommandEncoder();
  wmtcmd_blit_copy_from_texture_to_buffer copy = {};
  copy.type = WMTBlitCommandCopyFromTextureToBuffer;
  copy.next.set(nullptr);
  copy.src = texture;
  copy.slice = slice;
  copy.level = level;
  copy.origin = {x, y, 0};
  copy.size = {1, 1, 1};
  copy.dst = buffer;
  copy.offset = 0;
  copy.bytes_per_row = row_pitch;
  copy.bytes_per_image = readback_size;
  encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
  encoder.endEncoding();

  const auto format = texture.pixelFormat();
  TexturePointDiagnosticReadback readback(buffer, mapped);
  readback.label = label;
  readback.frame_id = frame_id;
  readback.seq_id = seq_id;
  readback.texture_id = uint64_t(texture);
  readback.format = uint32_t(format);
  readback.width = width;
  readback.height = height;
  readback.x = x;
  readback.y = y;
  readback.texel_size = texel_size;
  readback.encoder_id = encoder_id;
  readback.index = index;
  readback.level = level;
  readback.slice = slice;
  readbacks.diagnostics.emplace_back(std::move(readback));
}

void
ExecuteDiagnosticReadback(
    TexturePointDiagnosticReadback &readback) noexcept {
  dxmt::invokeNoexcept("texture diagnostic readback", [&readback]() {
    uint8_t bytes[16] = {};
    const auto copy_size =
        std::min<uint32_t>(readback.texel_size, sizeof(bytes));
    std::memcpy(bytes, readback.mapped(), copy_size);
    const uint32_t u32 =
        uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
        (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
    INFO("DXMT diagnostic: texture readback",
         " label=", readback.label,
         " frame=", readback.frame_id,
         " seq=", readback.seq_id,
         " encoder=", readback.encoder_id,
         " index=", readback.index,
         " texture=", readback.texture_id,
         " format=", readback.format,
         " size=", readback.width, "x", readback.height,
         " level=", uint32_t(readback.level),
         " slice=", uint32_t(readback.slice),
         " xy=", readback.x, ",", readback.y,
         " texelSize=", readback.texel_size,
         " bytes=", uint32_t(bytes[0]), ",", uint32_t(bytes[1]), ",",
         uint32_t(bytes[2]), ",", uint32_t(bytes[3]),
         " u32=", u32);
  });
}

static void
DebugEncodeTextureCenterReadback(QueryReadbacks &readbacks, WMT::CommandBuffer cmdbuf,
                                 WMT::Device device, CommandQueue &queue,
                                 WMT::Texture texture,
                                 const char *label, uint64_t frame_id,
                                 uint64_t seq_id, uint32_t present_index) {
  const auto width = texture ? texture.width() : 0;
  const auto height = texture ? texture.height() : 0;
  DebugEncodeTexturePointReadback(readbacks, cmdbuf, device, queue, texture, label, frame_id,
                                  seq_id, present_index, present_index,
                                  width / 2, height / 2, 0, 0, width, height);
}

static void
DebugEncodePresentReadbacks(QueryReadbacks &readbacks, WMT::CommandBuffer cmdbuf,
                            WMT::Device device, CommandQueue &queue,
                            WMT::Texture texture,
                            const char *label, uint64_t frame_id,
                            uint64_t seq_id, uint32_t present_index) {
  if (!DebugPresentReadbackGridEnabled()) {
    DebugEncodeTextureCenterReadback(readbacks, cmdbuf, device, queue, texture, label,
                                     frame_id, seq_id, present_index);
    return;
  }

  const auto width = texture ? texture.width() : 0;
  const auto height = texture ? texture.height() : 0;
  const auto grid_size = DebugPresentReadbackGridSize();
  for (uint32_t y = 0; y < grid_size; y++) {
    for (uint32_t x = 0; x < grid_size; x++) {
      const auto index = y * grid_size + x;
      const auto point_x = width ? ((uint64_t(x) * width) + (width / 2)) / grid_size : 0;
      const auto point_y = height ? ((uint64_t(y) * height) + (height / 2)) / grid_size : 0;
    DebugEncodeTexturePointReadback(
        readbacks, cmdbuf, device, queue, texture, label, frame_id, seq_id,
          present_index, index, point_x, point_y, 0, 0, width, height);
    }
  }
}

static void
DebugEncodeRenderAttachmentReadbacks(QueryReadbacks &readbacks, WMT::CommandBuffer cmdbuf,
                                     WMT::Device device, CommandQueue &queue,
                                     uint64_t frame_id,
                                     uint64_t seq_id, const RenderEncoderData *data,
                                     bool sample, const char *label) {
  if (!sample)
    return;

  if (data->default_raster_sample_count > 1) {
    INFO("DXMT diagnostic: render readback skipped",
         " frame=", frame_id,
         " seq=", seq_id,
         " encoder=", data->id,
         " samples=", uint32_t(data->default_raster_sample_count),
         " reason=multisample");
    return;
  }

  for (unsigned i = 0; i < std::size(data->colors); i++) {
    auto &color = data->colors[i];
    if (!color.attachment && !color.buffer_texture)
      continue;

    WMT::Texture texture;
    uint16_t level = color.level;
    uint16_t slice = color.slice;
    uint32_t width = data->render_target_width;
    uint32_t height = data->render_target_height;
    if (color.attachment) {
      texture = color.attachment.texture();
    } else {
      texture = color.buffer_texture;
      level = 0;
      slice = 0;
      width = texture ? texture.width() : 0;
      height = texture ? texture.height() : 0;
    }

    DebugEncodeTexturePointReadback(readbacks, cmdbuf, device, queue, texture,
                                    label, frame_id, seq_id,
                                    data->id, i, width / 2, height / 2, level,
                                    slice, width, height);
  }
}

static const char *
DebugPipelineStageName(PipelineStage stage) {
  switch (stage) {
  case PipelineStage::Vertex:
    return "VS";
  case PipelineStage::Pixel:
    return "PS";
  case PipelineStage::Geometry:
    return "GS";
  case PipelineStage::Hull:
    return "HS";
  case PipelineStage::Domain:
    return "DS";
  case PipelineStage::Compute:
    return "CS";
  }
  return "?";
}

static void
DebugAccumulateRenderCommands(FrameStatistics &statistics,
                              const wmtcmd_render_nop *cmd_head) {
  auto command = reinterpret_cast<const wmtcmd_base *>(cmd_head->next.ptr);
  while (command) {
    statistics.render_command_count++;
    switch (static_cast<WMTRenderCommandType>(command->type)) {
    case WMTRenderCommandSetPSO:
      statistics.render_pso_bind_count++;
      break;
    case WMTRenderCommandDraw:
      statistics.render_draw_count++;
      break;
    case WMTRenderCommandDrawIndexed:
      statistics.render_indexed_draw_count++;
      break;
    case WMTRenderCommandDrawIndirect:
    case WMTRenderCommandDrawIndexedIndirect:
      statistics.render_indirect_draw_count++;
      break;
    case WMTRenderCommandDrawMeshThreadgroups:
    case WMTRenderCommandDrawMeshThreadgroupsIndirect:
    case WMTRenderCommandDXMTGeometryDraw:
    case WMTRenderCommandDXMTGeometryDrawIndexed:
    case WMTRenderCommandDXMTGeometryDrawIndirect:
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect:
    case WMTRenderCommandDXMTTessellationMeshDraw:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed:
    case WMTRenderCommandDXMTTessellationMeshDrawIndirect:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect:
      statistics.render_mesh_draw_count++;
      break;
    case WMTRenderCommandDispatchThreadsPerTile:
      statistics.render_tile_dispatch_count++;
      break;
    default:
      break;
    }
    command = static_cast<const wmtcmd_base *>(command->next.ptr);
  }
}

static void
DebugAccumulateBlitCommands(FrameStatistics &statistics,
                            const wmtcmd_blit_nop *cmd_head) {
  auto command = reinterpret_cast<const wmtcmd_base *>(cmd_head->next.ptr);
  while (command) {
    statistics.blit_command_count++;
    switch (static_cast<WMTBlitCommandType>(command->type)) {
    case WMTBlitCommandCopyFromBufferToBuffer:
      statistics.blit_copy_buffer_to_buffer_count++;
      break;
    case WMTBlitCommandCopyFromBufferToTexture:
      statistics.blit_copy_buffer_to_texture_count++;
      break;
    case WMTBlitCommandCopyFromTextureToBuffer:
      statistics.blit_copy_texture_to_buffer_count++;
      break;
    case WMTBlitCommandCopyFromTextureToTexture:
      statistics.blit_copy_texture_to_texture_count++;
      break;
    case WMTBlitCommandGenerateMipmaps:
      statistics.blit_generate_mipmaps_count++;
      break;
    case WMTBlitCommandWaitForFence:
      statistics.blit_fence_wait_count++;
      break;
    case WMTBlitCommandUpdateFence:
      statistics.blit_fence_update_count++;
      break;
    case WMTBlitCommandFillBuffer:
      statistics.blit_fill_buffer_count++;
      break;
    case WMTBlitCommandResolveCounters:
      statistics.blit_resolve_counters_count++;
      break;
    case WMTBlitCommandResourceStateBarrier:
      break;
#if !DXMT_DX12_METAL4
    case WMTBlitCommandOptimizeContentsForGPUAccess:
      break;
#endif
    case WMTBlitCommandNop:
      break;
    }
    command = static_cast<const wmtcmd_base *>(command->next.ptr);
  }
}

static void
DebugAccumulateBlitPass(FrameStatistics &statistics,
                        const BlitEncoderData *data) {
  const bool has_commands = data->cmd_head.next.ptr != nullptr;
  const auto fence_wait_count = data->fence_wait.count();
  const auto fence_update_count = data->fence_update.count();

  if (has_commands)
    statistics.blit_pass_with_commands_count++;
  else
    statistics.blit_pass_empty_count++;

  if (fence_wait_count)
    statistics.blit_pass_with_fence_wait_count++;
  if (fence_update_count)
    statistics.blit_pass_with_fence_update_count++;

  statistics.blit_fence_wait_entry_count += fence_wait_count;
  statistics.blit_fence_update_entry_count += fence_update_count;
  DebugAccumulateBlitCommands(statistics, &data->cmd_head);
}

static void
DebugAccumulateComputePass(FrameStatistics &statistics,
                           const ComputeEncoderData *data) {
  bool has_dispatch = false;
  auto command = reinterpret_cast<const wmtcmd_base *>(data->cmd_head.next.ptr);
  while (command) {
    statistics.compute_command_count++;
    switch (static_cast<WMTComputeCommandType>(command->type)) {
    case WMTComputeCommandSetPSO:
      statistics.compute_pso_bind_count++;
      break;
    case WMTComputeCommandSetBuffer:
    case WMTComputeCommandSetBufferOffset:
    case WMTComputeCommandSetArgumentBuffer:
    case WMTComputeCommandSetArgumentBufferOffset:
#ifdef WMT_HAS_ARGUMENT_TABLE_COMMANDS
    case WMTComputeCommandSetArgumentTable:
#endif
      statistics.compute_set_buffer_count++;
      break;
    case WMTComputeCommandSetTexture:
      statistics.compute_set_texture_count++;
      break;
    case WMTComputeCommandSetBytes:
      statistics.compute_set_bytes_count++;
      break;
    case WMTComputeCommandUseResource:
      statistics.compute_use_resource_count++;
      break;
    case WMTComputeCommandDispatch:
    case WMTComputeCommandDispatchThreads:
      statistics.compute_dispatch_count++;
      has_dispatch = true;
      break;
    case WMTComputeCommandDispatchIndirect:
      statistics.compute_dispatch_indirect_count++;
      has_dispatch = true;
      break;
    case WMTComputeCommandMemoryBarrier:
      statistics.compute_memory_barrier_count++;
      break;
    case WMTComputeCommandWaitForFence:
      statistics.compute_fence_wait_count++;
      break;
    case WMTComputeCommandUpdateFence:
      statistics.compute_fence_update_count++;
      break;
    case WMTComputeCommandNop:
      break;
    }
    command = static_cast<const wmtcmd_base *>(command->next.ptr);
  }

  if (has_dispatch)
    statistics.compute_pass_with_dispatch_count++;
  else
    statistics.compute_pass_without_dispatch_count++;
}

struct DebugRenderCommandSummary {
  uint32_t command_count = 0;
  uint32_t pso_binds = 0;
  uint32_t draws = 0;
  uint32_t indexed_draws = 0;
  uint32_t indirect_draws = 0;
  uint32_t mesh_draws = 0;
  uint32_t tile_dispatches = 0;
};

struct DebugComputeCommandSummary {
  uint32_t command_count = 0;
  uint32_t pso_binds = 0;
  uint32_t set_buffers = 0;
  uint32_t set_textures = 0;
  uint32_t set_bytes = 0;
  uint32_t use_resources = 0;
  uint32_t dispatches = 0;
  uint32_t indirect_dispatches = 0;
  uint32_t barriers = 0;
  uint32_t wait_fences = 0;
  uint32_t update_fences = 0;
  obj_handle_t first_pso = 0;
  obj_handle_t last_pso = 0;
  WMTSize last_threadgroup_size = {};
  WMTSize first_dispatch_size = {};
  WMTSize last_dispatch_size = {};
  WMTSize max_dispatch_size = {};
  uint64_t dispatch_grid_volume = 0;
  uint64_t max_dispatch_grid_volume = 0;
  uint64_t set_bytes_total = 0;
  uint64_t first_indirect_offset = 0;
  uint64_t last_indirect_offset = 0;
  obj_handle_t first_indirect_buffer = 0;
  obj_handle_t last_indirect_buffer = 0;
};

static std::string
DebugShortShaderKey(const std::string &key) {
  if (key.empty())
    return "-";
  return key.substr(0, std::min<size_t>(key.size(), 16));
}

struct DebugBlitCommandSummary {
  uint32_t command_count = 0;
  uint32_t copy_buffer_to_buffer = 0;
  uint32_t copy_buffer_to_texture = 0;
  uint32_t copy_texture_to_buffer = 0;
  uint32_t copy_texture_to_texture = 0;
  uint32_t generate_mipmaps = 0;
  uint32_t fills = 0;
  uint32_t barriers = 0;
  uint32_t wait_fences = 0;
  uint32_t update_fences = 0;
};

struct DebugAttachmentKey {
  obj_handle_t texture = 0;
  uint64_t view = 0;
  uint16_t level = 0;
  uint16_t slice = 0;
  uint32_t depth_plane = 0;

  bool
  operator==(const DebugAttachmentKey &other) const {
    return texture == other.texture &&
           view == other.view &&
           level == other.level &&
           slice == other.slice &&
           depth_plane == other.depth_plane;
  }
};

struct DebugLastAttachmentUse {
  DebugAttachmentKey key = {};
  enum WMTStoreAction store_action = WMTStoreActionDontCare;
  bool valid = false;
};

static DebugAttachmentKey
DebugMakeColorAttachmentKey(const RenderEncoderColorAttachmentData &color) {
  WMT::Texture texture;
  if (color.attachment)
    texture = color.attachment.texture();
  else
    texture = color.buffer_texture;

  return {
      .texture = static_cast<obj_handle_t>(texture),
      .view = color.attachment ? uint64_t(color.attachment->key) : color.buffer_view_id,
      .level = color.level,
      .slice = color.slice,
      .depth_plane = color.depth_plane,
  };
}

static DebugAttachmentKey
DebugMakeDepthAttachmentKey(const RenderEncoderDepthAttachmentData &depth) {
  return {
      .texture = static_cast<obj_handle_t>(depth.attachment.texture()),
      .view = depth.attachment ? uint64_t(depth.attachment->key) : 0,
      .level = depth.level,
      .slice = depth.slice,
      .depth_plane = depth.depth_plane,
  };
}

static DebugAttachmentKey
DebugMakeStencilAttachmentKey(const RenderEncoderStencilAttachmentData &stencil) {
  return {
      .texture = static_cast<obj_handle_t>(stencil.attachment.texture()),
      .view = stencil.attachment ? uint64_t(stencil.attachment->key) : 0,
      .level = stencil.level,
      .slice = stencil.slice,
      .depth_plane = stencil.depth_plane,
  };
}

static void
DebugAccumulateAttachmentUse(FrameStatistics &statistics,
                             const DebugAttachmentKey &key,
                             enum WMTLoadAction load_action,
                             enum WMTStoreAction store_action,
                             DebugLastAttachmentUse &last_use) {
  if (!key.texture)
    return;

  statistics.render_attachment_count++;
  if (last_use.valid && last_use.key == key) {
    statistics.render_attachment_reuse_count++;
    if (load_action == WMTLoadActionLoad)
      statistics.render_attachment_reload_count++;
    if (load_action == WMTLoadActionLoad &&
        last_use.store_action == WMTStoreActionStore)
      statistics.render_attachment_store_then_load_count++;
  }

  last_use.key = key;
  last_use.store_action = store_action;
  last_use.valid = true;
}

static void
DebugAccumulateRenderPassTileStats(FrameStatistics &statistics,
                                   const RenderEncoderData *data,
                                   std::array<DebugLastAttachmentUse, 10> &last_attachments) {
  for (unsigned i = 0; i < data->render_target_count && i < data->colors.size(); i++) {
    const auto &color = data->colors[i];
    if (!color.attachment && !color.buffer_texture)
      continue;

    statistics.render_color_load_count += color.load_action == WMTLoadActionLoad;
    statistics.render_color_store_count += color.store_action == WMTStoreActionStore;
    statistics.render_color_store_count +=
        color.store_action == WMTStoreActionStoreAndMultisampleResolve;
    DebugAccumulateAttachmentUse(
        statistics, DebugMakeColorAttachmentKey(color), color.load_action,
        color.store_action, last_attachments[i]);
  }

  if (data->depth.attachment) {
    statistics.render_depth_load_count += data->depth.load_action == WMTLoadActionLoad;
    statistics.render_depth_store_count += data->depth.store_action == WMTStoreActionStore;
    DebugAccumulateAttachmentUse(
        statistics, DebugMakeDepthAttachmentKey(data->depth), data->depth.load_action,
        data->depth.store_action, last_attachments[8]);
  }

  if (data->stencil.attachment) {
    statistics.render_stencil_load_count += data->stencil.load_action == WMTLoadActionLoad;
    statistics.render_stencil_store_count += data->stencil.store_action == WMTStoreActionStore;
    DebugAccumulateAttachmentUse(
        statistics, DebugMakeStencilAttachmentKey(data->stencil), data->stencil.load_action,
        data->stencil.store_action, last_attachments[9]);
  }
}

static WMT::String
DebugEncoderLabel(const std::string &label) {
  return WMT::String::string(label.c_str(), WMTUTF8StringEncoding);
}

static std::string
DebugHex(uint64_t value) {
  std::stringstream stream;
  stream << std::hex << value;
  return stream.str();
}

static DebugRenderCommandSummary
DebugSummarizeRenderCommands(const wmtcmd_render_nop *cmd_head) {
  DebugRenderCommandSummary summary;
  auto command = reinterpret_cast<const wmtcmd_base *>(cmd_head->next.ptr);
  while (command) {
    summary.command_count++;
    switch (static_cast<WMTRenderCommandType>(command->type)) {
    case WMTRenderCommandSetPSO:
      summary.pso_binds++;
      break;
    case WMTRenderCommandDraw:
      summary.draws++;
      break;
    case WMTRenderCommandDrawIndexed:
      summary.indexed_draws++;
      break;
    case WMTRenderCommandDrawIndirect:
    case WMTRenderCommandDrawIndexedIndirect:
      summary.indirect_draws++;
      break;
    case WMTRenderCommandDrawMeshThreadgroups:
    case WMTRenderCommandDrawMeshThreadgroupsIndirect:
    case WMTRenderCommandDXMTGeometryDraw:
    case WMTRenderCommandDXMTGeometryDrawIndexed:
    case WMTRenderCommandDXMTGeometryDrawIndirect:
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect:
    case WMTRenderCommandDXMTTessellationMeshDraw:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed:
    case WMTRenderCommandDXMTTessellationMeshDrawIndirect:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect:
      summary.mesh_draws++;
      break;
    case WMTRenderCommandDispatchThreadsPerTile:
      summary.tile_dispatches++;
      break;
    default:
      break;
    }
    command = static_cast<const wmtcmd_base *>(command->next.ptr);
  }
  return summary;
}

static DebugComputeCommandSummary
DebugSummarizeComputeCommands(const wmtcmd_compute_nop *cmd_head) {
  DebugComputeCommandSummary summary;
  auto record_dispatch = [&](const WMTSize &size) {
    summary.dispatches++;
    if (summary.dispatches == 1)
      summary.first_dispatch_size = size;
    summary.last_dispatch_size = size;
    summary.max_dispatch_size.width = std::max(summary.max_dispatch_size.width, size.width);
    summary.max_dispatch_size.height = std::max(summary.max_dispatch_size.height, size.height);
    summary.max_dispatch_size.depth = std::max(summary.max_dispatch_size.depth, size.depth);
    const auto volume = size.width * std::max<uint64_t>(size.height, 1) * std::max<uint64_t>(size.depth, 1);
    summary.dispatch_grid_volume += volume;
    summary.max_dispatch_grid_volume = std::max(summary.max_dispatch_grid_volume, volume);
  };
  auto command = reinterpret_cast<const wmtcmd_base *>(cmd_head->next.ptr);
  while (command) {
    summary.command_count++;
    switch (static_cast<WMTComputeCommandType>(command->type)) {
    case WMTComputeCommandSetPSO: {
      auto set_pso = reinterpret_cast<const wmtcmd_compute_setpso *>(command);
      summary.pso_binds++;
      if (!summary.first_pso)
        summary.first_pso = set_pso->pso;
      summary.last_pso = set_pso->pso;
      summary.last_threadgroup_size = set_pso->threadgroup_size;
      break;
    }
    case WMTComputeCommandSetBuffer:
    case WMTComputeCommandSetBufferOffset:
    case WMTComputeCommandSetArgumentBuffer:
    case WMTComputeCommandSetArgumentBufferOffset:
#ifdef WMT_HAS_ARGUMENT_TABLE_COMMANDS
    case WMTComputeCommandSetArgumentTable:
#endif
      summary.set_buffers++;
      break;
    case WMTComputeCommandSetTexture:
      summary.set_textures++;
      break;
    case WMTComputeCommandSetBytes: {
      auto set_bytes = reinterpret_cast<const wmtcmd_compute_setbytes *>(command);
      summary.set_bytes++;
      summary.set_bytes_total += set_bytes->length;
      break;
    }
    case WMTComputeCommandUseResource:
      summary.use_resources++;
      break;
    case WMTComputeCommandDispatch:
    case WMTComputeCommandDispatchThreads: {
      auto dispatch = reinterpret_cast<const wmtcmd_compute_dispatch *>(command);
      record_dispatch(dispatch->size);
      break;
    }
    case WMTComputeCommandDispatchIndirect: {
      auto dispatch = reinterpret_cast<const wmtcmd_compute_dispatch_indirect *>(command);
      summary.indirect_dispatches++;
      if (summary.indirect_dispatches == 1) {
        summary.first_indirect_buffer = dispatch->indirect_args_buffer;
        summary.first_indirect_offset = dispatch->indirect_args_offset;
      }
      summary.last_indirect_buffer = dispatch->indirect_args_buffer;
      summary.last_indirect_offset = dispatch->indirect_args_offset;
      break;
    }
    case WMTComputeCommandMemoryBarrier:
      summary.barriers++;
      break;
    case WMTComputeCommandWaitForFence:
      summary.wait_fences++;
      break;
    case WMTComputeCommandUpdateFence:
      summary.update_fences++;
      break;
    default:
      break;
    }
    command = static_cast<const wmtcmd_base *>(command->next.ptr);
  }
  return summary;
}

static bool
DebugComputePerfShouldLog(const DebugComputeCommandSummary &summary) {
  if (!DebugShouldLogFlushPerf())
    return false;
  static std::atomic<uint64_t> log_count = 0;
  auto index = log_count.fetch_add(1, std::memory_order_relaxed);
  if (index < 2048 || (index % 256) == 0)
    return true;
  return summary.indirect_dispatches || summary.max_dispatch_grid_volume >= (512ull * 512ull);
}

static DebugBlitCommandSummary
DebugSummarizeBlitCommands(const wmtcmd_blit_nop *cmd_head) {
  DebugBlitCommandSummary summary;
  auto command = reinterpret_cast<const wmtcmd_base *>(cmd_head->next.ptr);
  while (command) {
    summary.command_count++;
    switch (static_cast<WMTBlitCommandType>(command->type)) {
    case WMTBlitCommandCopyFromBufferToBuffer:
      summary.copy_buffer_to_buffer++;
      break;
    case WMTBlitCommandCopyFromBufferToTexture:
      summary.copy_buffer_to_texture++;
      break;
    case WMTBlitCommandCopyFromTextureToBuffer:
      summary.copy_texture_to_buffer++;
      break;
    case WMTBlitCommandCopyFromTextureToTexture:
      summary.copy_texture_to_texture++;
      break;
    case WMTBlitCommandGenerateMipmaps:
      summary.generate_mipmaps++;
      break;
    case WMTBlitCommandFillBuffer:
      summary.fills++;
      break;
    case WMTBlitCommandResolveCounters:
    case WMTBlitCommandResourceStateBarrier:
      summary.barriers++;
      break;
    case WMTBlitCommandWaitForFence:
      summary.wait_fences++;
      break;
    case WMTBlitCommandUpdateFence:
      summary.update_fences++;
      break;
#if !DXMT_DX12_METAL4
    case WMTBlitCommandOptimizeContentsForGPUAccess:
      break;
#endif
    case WMTBlitCommandNop:
      break;
    }
    command = static_cast<const wmtcmd_base *>(command->next.ptr);
  }
  return summary;
}

/* One-shot Invalid Resource forensics: dump every blit command's Metal handles.
 * Enabled under hang-dense / validation only. Never alters encoding. */
static void
DebugLogBlitCommandsForHang(uint64_t frame_id, uint64_t seq_id, uint64_t encoder_id,
                            const DebugBlitCommandSummary &summary,
                            const wmtcmd_blit_nop *cmd_head) {
  if (!DebugHangDenseEnabled() || !cmd_head)
    return;
  WARN("DXMT hang-diag blit encoder:"
       " frame=", frame_id,
       " seq=", seq_id,
       " encoderId=", encoder_id,
       " cmds=", summary.command_count,
       " b2b=", summary.copy_buffer_to_buffer,
       " b2t=", summary.copy_buffer_to_texture,
       " t2t=", summary.copy_texture_to_texture,
       " t2b=", summary.copy_texture_to_buffer,
       " fill=", summary.fills,
       " mip=", summary.generate_mipmaps,
       " waitFence=", summary.wait_fences,
       " updateFence=", summary.update_fences);
  uint32_t index = 0;
  auto command = reinterpret_cast<const wmtcmd_base *>(cmd_head->next.ptr);
  while (command) {
    switch (static_cast<WMTBlitCommandType>(command->type)) {
    case WMTBlitCommandCopyFromBufferToBuffer: {
      const auto *c =
          reinterpret_cast<const wmtcmd_blit_copy_from_buffer_to_buffer *>(
              command);
      WARN("DXMT hang-diag blit cmd:"
           " frame=", frame_id, " seq=", seq_id, " encoderId=", encoder_id,
           " idx=", index, " kind=b2b",
           " src=", uint64_t(c->src), " srcOff=", c->src_offset,
           " dst=", uint64_t(c->dst), " dstOff=", c->dst_offset,
           " len=", c->copy_length,
           " nullSrc=", c->src ? 0u : 1u, " nullDst=", c->dst ? 0u : 1u);
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      const auto *c =
          reinterpret_cast<const wmtcmd_blit_copy_from_buffer_to_texture *>(
              command);
      WARN("DXMT hang-diag blit cmd:"
           " frame=", frame_id, " seq=", seq_id, " encoderId=", encoder_id,
           " idx=", index, " kind=b2t",
           " srcBuf=", uint64_t(c->src), " srcOff=", c->src_offset,
           " rowPitch=", c->bytes_per_row, " imagePitch=", c->bytes_per_image,
           " size=", c->size.width, "x", c->size.height, "x", c->size.depth,
           " dstTex=", uint64_t(c->dst),
           " slice=", c->slice, " level=", c->level,
           " origin=", c->origin.x, ",", c->origin.y, ",", c->origin.z,
           " nullSrc=", c->src ? 0u : 1u, " nullDst=", c->dst ? 0u : 1u);
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer: {
      const auto *c =
          reinterpret_cast<const wmtcmd_blit_copy_from_texture_to_buffer *>(
              command);
      WARN("DXMT hang-diag blit cmd:"
           " frame=", frame_id, " seq=", seq_id, " encoderId=", encoder_id,
           " idx=", index, " kind=t2b",
           " srcTex=", uint64_t(c->src),
           " slice=", c->slice, " level=", c->level,
           " origin=", c->origin.x, ",", c->origin.y, ",", c->origin.z,
           " size=", c->size.width, "x", c->size.height, "x", c->size.depth,
           " dstBuf=", uint64_t(c->dst), " dstOff=", c->offset,
           " rowPitch=", c->bytes_per_row, " imagePitch=", c->bytes_per_image,
           " nullSrc=", c->src ? 0u : 1u, " nullDst=", c->dst ? 0u : 1u);
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      const auto *c =
          reinterpret_cast<const wmtcmd_blit_copy_from_texture_to_texture *>(
              command);
      WARN("DXMT hang-diag blit cmd:"
           " frame=", frame_id, " seq=", seq_id, " encoderId=", encoder_id,
           " idx=", index, " kind=t2t",
           " srcTex=", uint64_t(c->src),
           " srcSlice=", c->src_slice, " srcLevel=", c->src_level,
           " srcOrigin=", c->src_origin.x, ",", c->src_origin.y, ",",
           c->src_origin.z,
           " size=", c->src_size.width, "x", c->src_size.height, "x",
           c->src_size.depth,
           " dstTex=", uint64_t(c->dst),
           " dstSlice=", c->dst_slice, " dstLevel=", c->dst_level,
           " dstOrigin=", c->dst_origin.x, ",", c->dst_origin.y, ",",
           c->dst_origin.z,
           " nullSrc=", c->src ? 0u : 1u, " nullDst=", c->dst ? 0u : 1u);
      break;
    }
    case WMTBlitCommandFillBuffer: {
      const auto *c =
          reinterpret_cast<const wmtcmd_blit_fillbuffer *>(command);
      WARN("DXMT hang-diag blit cmd:"
           " frame=", frame_id, " seq=", seq_id, " encoderId=", encoder_id,
           " idx=", index, " kind=fill",
           " buf=", uint64_t(c->buffer), " off=", c->offset, " len=", c->length,
           " value=", uint32_t(c->value),
           " nullBuf=", c->buffer ? 0u : 1u);
      break;
    }
    case WMTBlitCommandGenerateMipmaps: {
      const auto *c =
          reinterpret_cast<const wmtcmd_blit_generate_mipmaps *>(command);
      WARN("DXMT hang-diag blit cmd:"
           " frame=", frame_id, " seq=", seq_id, " encoderId=", encoder_id,
           " idx=", index, " kind=mip",
           " tex=", uint64_t(c->texture),
           " nullTex=", c->texture ? 0u : 1u);
      break;
    }
    case WMTBlitCommandWaitForFence:
    case WMTBlitCommandUpdateFence: {
      const auto *c =
          reinterpret_cast<const wmtcmd_blit_fence_op *>(command);
      WARN("DXMT hang-diag blit cmd:"
           " frame=", frame_id, " seq=", seq_id, " encoderId=", encoder_id,
           " idx=", index,
           " kind=",
           (static_cast<WMTBlitCommandType>(command->type) ==
            WMTBlitCommandWaitForFence)
               ? "waitFence"
               : "updateFence",
           " fence=", uint64_t(c->fence),
           " nullFence=", c->fence ? 0u : 1u);
      break;
    }
    default:
      WARN("DXMT hang-diag blit cmd:"
           " frame=", frame_id, " seq=", seq_id, " encoderId=", encoder_id,
           " idx=", index, " kind=other type=", uint32_t(command->type));
      break;
    }
    index++;
    command = static_cast<const wmtcmd_base *>(command->next.ptr);
  }
}

static void
DebugLogRenderPassInfo(uint64_t frame_id, uint64_t seq_id, uint64_t encoder_id,
                       const RenderEncoderData *data,
                       const DebugRenderCommandSummary &summary) {
  if (!DebugShouldLogRenderPasses())
    return;

  static std::atomic<uint32_t> log_count = 0;
  const auto occurrence =
      log_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (occurrence > 32 && (occurrence & (occurrence - 1)) != 0)
    return;

  INFO(
      "DXMT diagnostic: render pass",
      " occurrence=", occurrence,
      " frame=", frame_id,
      " seq=", seq_id,
      " encoder=", encoder_id,
      " size=", data->render_target_width, "x", data->render_target_height,
      " rt_count=", uint32_t(data->render_target_count),
      " samples=", uint32_t(data->default_raster_sample_count),
      " array_length=", uint32_t(data->render_target_array_length),
      " commands=", summary.command_count,
      " psoBinds=", summary.pso_binds,
      " draws=", summary.draws,
      " indexedDraws=", summary.indexed_draws,
      " indirectDraws=", summary.indirect_draws,
      " meshDraws=", summary.mesh_draws,
      " tileDispatches=", summary.tile_dispatches
  );

  for (unsigned i = 0; i < std::size(data->colors); i++) {
    auto &color = data->colors[i];
    if (!color.attachment && !color.buffer_texture)
      continue;
    WMT::Texture texture;
    if (color.attachment)
      texture = color.attachment.texture();
    else
      texture = color.buffer_texture;
    auto *allocation = color.attachment ? color.attachment->allocation : nullptr;
    auto *descriptor = allocation ? allocation->descriptor : nullptr;
    INFO(
        "DXMT diagnostic: render color attachment",
        " occurrence=", occurrence,
        " frame=", frame_id,
        " seq=", seq_id,
        " encoder=", encoder_id,
        " slot=", i,
        " texture=", uint64_t(texture),
        " texture_descriptor=", uint64_t(descriptor),
        " resource_identity=", descriptor ? descriptor->diagnosticIdentity() : 0,
        " allocation=", uint64_t(allocation),
        " allocation_texture=", allocation ? uint64_t(allocation->texture()) : 0,
        " allocation_matches_view=",
        allocation && allocation->texture() == texture ? 1 : 0,
        " descriptor_size=", descriptor ? descriptor->width() : 0, "x",
        descriptor ? descriptor->height() : 0, "x",
        descriptor ? descriptor->depth() : 0,
        " descriptor_array=", descriptor ? descriptor->arrayLength() : 0,
        " descriptor_mips=", descriptor ? descriptor->miplevelCount() : 0,
        " descriptor_samples=", descriptor ? descriptor->sampleCount() : 0,
        " descriptor_format=",
        descriptor ? uint32_t(descriptor->pixelFormat()) : 0,
        " descriptor_type=",
        descriptor ? uint32_t(descriptor->textureType()) : 0,
        " descriptor_usage=", descriptor ? uint32_t(descriptor->usage()) : 0,
        " view=", color.attachment ? uint64_t(color.attachment->key) : color.buffer_view_id,
        " load=", uint32_t(color.load_action),
        " store=", uint32_t(color.store_action),
        " clear=", color.clear_color.r, ",", color.clear_color.g, ",", color.clear_color.b, ",", color.clear_color.a,
        " level=", uint32_t(color.level),
        " slice=", uint32_t(color.slice),
        " resolve=", uint64_t(color.resolve_attachment ? color.resolve_attachment.texture() : WMT::Texture{})
    );
  }

  if (data->depth.attachment) {
    const auto &depth = data->depth;
    auto *allocation = depth.attachment->allocation;
    auto *descriptor = allocation ? allocation->descriptor : nullptr;
    INFO("DXMT diagnostic: render depth attachment",
         " occurrence=", occurrence,
         " frame=", frame_id,
         " seq=", seq_id,
         " encoder=", encoder_id,
         " texture=", uint64_t(depth.attachment.texture()),
         " texture_descriptor=", uint64_t(descriptor),
         " resource_identity=",
         descriptor ? descriptor->diagnosticIdentity() : 0,
         " allocation=", uint64_t(allocation),
         " allocation_texture=",
         allocation ? uint64_t(allocation->texture()) : 0,
         " allocation_matches_view=",
         allocation && allocation->texture() == depth.attachment.texture() ? 1
                                                                          : 0,
         " descriptor_size=", descriptor ? descriptor->width() : 0, "x",
         descriptor ? descriptor->height() : 0, "x",
         descriptor ? descriptor->depth() : 0,
         " descriptor_array=", descriptor ? descriptor->arrayLength() : 0,
         " descriptor_mips=", descriptor ? descriptor->miplevelCount() : 0,
         " descriptor_samples=", descriptor ? descriptor->sampleCount() : 0,
         " descriptor_format=",
         descriptor ? uint32_t(descriptor->pixelFormat()) : 0,
         " descriptor_type=",
         descriptor ? uint32_t(descriptor->textureType()) : 0,
         " descriptor_usage=", descriptor ? uint32_t(descriptor->usage()) : 0,
         " view=", uint64_t(depth.attachment->key),
         " load=", uint32_t(depth.load_action),
         " store=", uint32_t(depth.store_action),
         " clear=", depth.clear_depth,
         " level=", uint32_t(depth.level),
         " slice=", uint32_t(depth.slice),
         " depth_plane=", depth.depth_plane);
  }

  if (data->stencil.attachment) {
    const auto &stencil = data->stencil;
    auto *allocation = stencil.attachment->allocation;
    auto *descriptor = allocation ? allocation->descriptor : nullptr;
    INFO("DXMT diagnostic: render stencil attachment",
         " occurrence=", occurrence,
         " frame=", frame_id,
         " seq=", seq_id,
         " encoder=", encoder_id,
         " texture=", uint64_t(stencil.attachment.texture()),
         " texture_descriptor=", uint64_t(descriptor),
         " resource_identity=",
         descriptor ? descriptor->diagnosticIdentity() : 0,
         " allocation=", uint64_t(allocation),
         " allocation_texture=",
         allocation ? uint64_t(allocation->texture()) : 0,
         " allocation_matches_view=",
         allocation && allocation->texture() == stencil.attachment.texture()
             ? 1
             : 0,
         " descriptor_size=", descriptor ? descriptor->width() : 0, "x",
         descriptor ? descriptor->height() : 0, "x",
         descriptor ? descriptor->depth() : 0,
         " descriptor_array=", descriptor ? descriptor->arrayLength() : 0,
         " descriptor_mips=", descriptor ? descriptor->miplevelCount() : 0,
         " descriptor_samples=", descriptor ? descriptor->sampleCount() : 0,
         " descriptor_format=",
         descriptor ? uint32_t(descriptor->pixelFormat()) : 0,
         " descriptor_type=",
         descriptor ? uint32_t(descriptor->textureType()) : 0,
         " descriptor_usage=", descriptor ? uint32_t(descriptor->usage()) : 0,
         " view=", uint64_t(stencil.attachment->key),
         " load=", uint32_t(stencil.load_action),
         " store=", uint32_t(stencil.store_action),
         " clear=", uint32_t(stencil.clear_stencil),
         " level=", uint32_t(stencil.level),
         " slice=", uint32_t(stencil.slice),
         " depth_plane=", stencil.depth_plane);
  }
}

static void
DebugLogClearPassInfo(uint64_t frame_id, uint64_t seq_id, uint64_t encoder_id,
                      const ClearEncoderData *data) {
  if (!DebugShouldLogRenderPasses())
    return;

  static std::atomic<uint32_t> log_count = 0;
  const auto occurrence =
      log_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (occurrence > 16 && (occurrence & (occurrence - 1)) != 0)
    return;

  WMT::Texture texture;
  if (data->attachment)
    texture = data->attachment.texture();
  else
    texture = data->buffer_texture;
  auto *allocation = data->attachment ? data->attachment->allocation : nullptr;
  auto *descriptor = allocation ? allocation->descriptor : nullptr;

  INFO(
      "DXMT diagnostic: clear pass",
      " occurrence=", occurrence,
      " frame=", frame_id,
      " seq=", seq_id,
      " encoder=", encoder_id,
      " size=", data->width, "x", data->height,
      " array_length=", uint32_t(data->array_length),
      " clear_dsv=", uint32_t(data->clear_dsv),
      " level=", uint32_t(data->level),
      " slice=", uint32_t(data->slice),
      " depth_plane=", uint32_t(data->depth_plane),
      " stencil_depth_plane=", uint32_t(data->stencil_depth_plane),
      " texture=", uint64_t(texture),
      " texture_descriptor=", uint64_t(descriptor),
      " resource_identity=", descriptor ? descriptor->diagnosticIdentity() : 0,
      " allocation=", uint64_t(allocation),
      " allocation_texture=", allocation ? uint64_t(allocation->texture()) : 0,
      " allocation_matches_view=",
      allocation && allocation->texture() == texture ? 1 : 0,
      " descriptor_size=", descriptor ? descriptor->width() : 0, "x",
      descriptor ? descriptor->height() : 0, "x",
      descriptor ? descriptor->depth() : 0,
      " descriptor_array=", descriptor ? descriptor->arrayLength() : 0,
      " descriptor_mips=", descriptor ? descriptor->miplevelCount() : 0,
      " descriptor_samples=", descriptor ? descriptor->sampleCount() : 0,
      " descriptor_format=", descriptor ? uint32_t(descriptor->pixelFormat()) : 0,
      " descriptor_type=", descriptor ? uint32_t(descriptor->textureType()) : 0,
      " descriptor_usage=", descriptor ? uint32_t(descriptor->usage()) : 0,
      " view=", data->attachment ? uint64_t(data->attachment->key) : data->buffer_view_id,
      " color=", data->color.r, ",", data->color.g, ",", data->color.b, ",", data->color.a,
      " depth=", data->depth_stencil.first,
      " stencil=", uint32_t(data->depth_stencil.second)
  );
}

static const char *
DebugPipelineKindName(PipelineKind kind) {
  switch (kind) {
  case PipelineKind::Ordinary:
    return "ordinary";
  case PipelineKind::Tessellation:
    return "tessellation";
  case PipelineKind::Geometry:
    return "geometry";
  }
  return "?";
}

template <PipelineStage stage, PipelineKind kind>
static void
DebugLogConstantBufferBinding(
    const std::string &shader_hash, const MTL_SM50_SHADER_ARGUMENT &arg,
    const ConstantBufferBinding &binding, uint64_t encoded_address,
    uint64_t valid_length, uint64_t encoder_id, bool dummy
) {
  if (!DebugShouldLogBinding(shader_hash))
    return;

  static std::atomic<uint32_t> log_count = 0;
  log_count.fetch_add(1, std::memory_order_relaxed);
  INFO(
      "DXMT diagnostic: shader constant buffer binding",
      " stage=", DebugPipelineStageName(stage),
      " kind=", DebugPipelineKindName(kind),
      " shader=", shader_hash,
      " encoder=", encoder_id,
      " slot=", arg.SM50BindingSlot,
      " arg_index=", GetArgumentIndex(arg.Type, arg.SM50BindingSlot),
      " struct_qword=", arg.StructurePtrOffset,
      " register_lower=", arg.RegisterLowerBound,
      " register_count=", arg.RegisterCount,
      " register_space=", arg.RegisterSpace,
      " cbuffer_vec4=", arg.CBufferSizeInVec4,
      " buffer=", uint64_t(binding.buffer.ptr()),
      " offset=", uint32_t(binding.offset),
      " length=", valid_length,
      " encoded=0x", std::hex, encoded_address, std::dec,
      " dummy=", dummy
  );
}

static const char *
DebugTextureTypeName(WMTTextureType type) {
  switch (type) {
  case WMTTextureType1D:
    return "1D";
  case WMTTextureType1DArray:
    return "1DArray";
  case WMTTextureType2D:
    return "2D";
  case WMTTextureType2DArray:
    return "2DArray";
  case WMTTextureType2DMultisample:
    return "2DMS";
  case WMTTextureType2DMultisampleArray:
    return "2DMSArray";
  case WMTTextureTypeCube:
    return "Cube";
  case WMTTextureTypeCubeArray:
    return "CubeArray";
  case WMTTextureType3D:
    return "3D";
  case WMTTextureTypeTextureBuffer:
    return "TextureBuffer";
  default:
    return "Unknown";
  }
}

template <PipelineStage stage, PipelineKind kind>
void
ArgumentEncodingContext::encodeConstantBuffers(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers, uint64_t offset,
    const ConstantBufferBinding *bindings
) {
  small_vector<uint64_t, 16> encoded_table(reflection->NumConstantBuffers, 0);
  auto encoder_id = currentEncoderId();

  for (unsigned i = 0; i < reflection->NumConstantBuffers; i++) {
    auto &arg = constant_buffers[i];
    switch (arg.Type) {
    case SM50BindingType::ConstantBuffer: {
      auto &cbuf = bindings[i];
      if (cbuf.direct_buffer) {
        encoded_table[arg.StructurePtrOffset] =
            cbuf.direct_gpu_address + cbuf.offset;
        DebugLogConstantBufferBinding<stage, kind>(
            "", arg, cbuf, encoded_table[arg.StructurePtrOffset],
            cbuf.direct_length > cbuf.offset ? cbuf.direct_length - cbuf.offset : 0,
            encoder_id, false);
        makeResident<stage, kind>(cbuf.direct_buffer,
                                  GetResidencyMask<kind>(stage, true, false));
        continue;
      }
      if (!cbuf.buffer.ptr()) {
        encoded_table[arg.StructurePtrOffset] = dummy_cbuffer_info_.gpu_address;
        DebugLogConstantBufferBinding<stage, kind>(
            "", arg, cbuf, dummy_cbuffer_info_.gpu_address, 0, encoder_id,
            true);
        makeResident<stage, kind>(dummy_cbuffer_, GetResidencyMask<kind>(stage, true, false));
        continue;
      }
      auto argbuf = cbuf.buffer;
      auto valid_length = argbuf->length() > cbuf.offset ? argbuf->length() - cbuf.offset : 0;
      auto [argbuf_alloc, argbuf_offset] = access<stage>(argbuf, cbuf.offset, valid_length, ResourceAccess::Read);
      encoded_table[arg.StructurePtrOffset] = argbuf_alloc->gpuAddress() + argbuf_offset + cbuf.offset;
      DebugLogConstantBufferBinding<stage, kind>(
          "", arg, cbuf, encoded_table[arg.StructurePtrOffset],
          valid_length, encoder_id, false);
      makeResident<stage, kind>(argbuf.ptr());
      break;
    }
    default:
      DXMT_UNREACHABLE
    }
  }

  /* kConstantBufferTableBinding = 29 */
  const auto table_size = uint64_t(reflection->NumConstantBuffers) << 3;
  if constexpr (stage == PipelineStage::Compute) {
    const auto binding_offset =
        deduplicateComputeArgumentTableSliceBytes(29, offset, table_size, encoded_table.data());
    if (binding_offset == offset) {
      auto *mapped =
          getMappedArgumentBuffer<uint64_t, true>(offset, table_size);
      if (!mapped)
        return;
      std::memcpy(mapped, encoded_table.data(), table_size);
    }
    const auto final_offset = getFinalArgumentBufferOffset<true>(binding_offset);
    if (!shouldEmitComputeArgumentBufferOffset(29, final_offset, binding_offset != offset))
      return;
    auto &cmd = encodeComputeCommand<wmtcmd_compute_setargumentbufferoffset>();
    cmd.type = WMTComputeCommandSetArgumentBufferOffset;
    cmd.offset = final_offset;
    cmd.index = 29;
  } else {
    uint8_t index = 29;
    WMTRenderStages stages = WMTRenderStageVertex;
    if constexpr (stage == PipelineStage::Vertex) {
      if constexpr (kind == PipelineKind::Geometry)
        stages = WMTRenderStageObject;
      else if constexpr (kind == PipelineKind::Tessellation) {
        stages = WMTRenderStageObject;
        index = 27;
      } else
        stages = WMTRenderStageVertex;
    } else if constexpr (stage == PipelineStage::Pixel) {
      stages = WMTRenderStageFragment;
    } else if constexpr (stage == PipelineStage::Hull) {
      stages = WMTRenderStageObject;
    } else if constexpr (stage == PipelineStage::Domain) {
      stages = WMTRenderStageMesh;
    } else if constexpr (stage == PipelineStage::Geometry) {
      stages = WMTRenderStageMesh;
    } else {
      assert(0 && "Not implemented or unreachable");
    }
    const auto binding_offset =
        deduplicateRenderArgumentTableSliceBytes(stages, index, offset, table_size, encoded_table.data());
    if (binding_offset == offset) {
      auto *mapped = getMappedArgumentBuffer<uint64_t>(offset, table_size);
      if (!mapped)
        return;
      std::memcpy(mapped, encoded_table.data(), table_size);
    }
    const auto final_offset = getFinalArgumentBufferOffset(binding_offset);
    if (!shouldEmitRenderArgumentBufferOffset(stages, index, final_offset, binding_offset != offset))
      return;
    auto &cmd = encodeRenderCommand<wmtcmd_render_setargumentbufferoffset>();
    cmd.type = WMTRenderCommandSetArgumentBufferOffset;
    cmd.index = index;
    cmd.stages = stages;
    cmd.offset = final_offset;
  }
}

static bool
DebugTextureTypeIsMultisampled(WMTTextureType type) {
  return type == WMTTextureType2DMultisample || type == WMTTextureType2DMultisampleArray;
}

template <PipelineStage stage, PipelineKind kind>
static void
DebugLogNullShaderBinding(
    const char *binding_type, const char *expected, const std::string &shader_hash,
    const MTL_SM50_SHADER_ARGUMENT &arg, bool has_buffer_binding, bool has_texture_binding,
    bool has_counter_binding, uint64_t encoder_id, const char *action,
    uint32_t resolved_slot, uint32_t range_local, bool snapshot_supplied,
    bool snapshot_captured
) {
  static std::atomic<uint32_t> count = 0;
  uint32_t occurrence = 0;
  if (!DebugShouldLogBindingAnomaly(shader_hash, count, occurrence))
    return;

  WARN(
      "DXMT diagnostic: null shader binding",
      " occurrence=", occurrence,
      " stage=", DebugPipelineStageName(stage),
      " kind=", DebugPipelineKindName(kind),
      " shader=", shader_hash,
      " encoder=", encoder_id,
      " binding=", binding_type,
      " expected=", expected,
      " slot=", arg.SM50BindingSlot,
      " resolved_slot=", resolved_slot,
      " range_local=", range_local,
      " arg_index=", GetArgumentIndex(arg.Type, arg.SM50BindingSlot),
      " struct_qword=", arg.StructurePtrOffset,
      " flags=0x", std::hex, arg.Flags, std::dec,
      " has_buffer=", has_buffer_binding,
      " has_texture=", has_texture_binding,
      " has_counter=", has_counter_binding,
      " snapshot_supplied=", snapshot_supplied,
      " snapshot_captured=", snapshot_captured,
      " action=", action
  );
}

template <PipelineStage stage, PipelineKind kind>
static void
DebugLogSnapshotBindingFallback(
    const char *binding_type, const std::string &shader_hash,
    const MTL_SM50_SHADER_ARGUMENT &arg, uint32_t resolved_slot,
    uint32_t range_local, bool has_buffer_binding, bool has_texture_binding,
    bool has_counter_binding, uint64_t encoder_id) {
  static std::atomic<uint32_t> count = 0;
  uint32_t occurrence = 0;
  if (!DebugShouldLogBindingAnomaly(shader_hash, count, occurrence))
    return;

  WARN("DXMT diagnostic: bindless snapshot binding fallback",
       " occurrence=", occurrence,
       " stage=", DebugPipelineStageName(stage),
       " kind=", DebugPipelineKindName(kind),
       " shader=", shader_hash,
       " encoder=", encoder_id,
       " binding=", binding_type,
       " slot=", arg.SM50BindingSlot,
       " resolved_slot=", resolved_slot,
       " range_local=", range_local,
       " lower=", arg.RegisterLowerBound,
       " count=", arg.RegisterCount,
       " struct_qword=", arg.StructurePtrOffset,
       " flags=0x", std::hex, arg.Flags, std::dec,
       " live_has_buffer=", has_buffer_binding,
       " live_has_texture=", has_texture_binding,
       " live_has_counter=", has_counter_binding);
}

template <PipelineStage stage, PipelineKind kind>
static void
DebugLogTextureBindingMismatch(
    const std::string &shader_hash, const MTL_SM50_SHADER_ARGUMENT &arg, Texture *texture, TextureViewKey view_id,
    uint64_t encoder_id
) {
  if (!DebugShouldLogBinding(shader_hash))
    return;

  auto actual_type = texture->textureType(view_id);
  bool shader_expects_ms = arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLED;
  bool actual_is_ms = DebugTextureTypeIsMultisampled(actual_type);
  if (shader_expects_ms == actual_is_ms)
    return;

  static std::atomic<uint32_t> log_count = 0;
  log_count.fetch_add(1, std::memory_order_relaxed);

  WARN(
      "DXMT diagnostic: texture binding type mismatch",
      " stage=", DebugPipelineStageName(stage),
      " kind=", DebugPipelineKindName(kind),
      " shader=", shader_hash,
      " encoder=", encoder_id,
      " binding=SRV",
      " slot=", arg.SM50BindingSlot,
      " arg_index=", GetArgumentIndex(arg.Type, arg.SM50BindingSlot),
      " struct_qword=", arg.StructurePtrOffset,
      " flags=0x", std::hex, arg.Flags, std::dec,
      " shader_expects_ms=", shader_expects_ms,
      " view_is_ms=", actual_is_ms,
      " view=", uint64_t(view_id),
      " view_type=", DebugTextureTypeName(actual_type), "(", uint32_t(actual_type), ")",
      " resource_type=", DebugTextureTypeName(texture->textureType()), "(", uint32_t(texture->textureType()), ")",
      " sample_count=", texture->sampleCount(),
      " format=", uint32_t(texture->pixelFormat(view_id)),
      " size=", texture->width(view_id), "x", texture->height(view_id),
      " array_size=", texture->arrayLength(view_id),
      " action=keep_original"
  );
}

template <PipelineStage stage, PipelineKind kind>
static void
DebugLogShaderTextureBinding(
    const std::string &shader_hash, const MTL_SM50_SHADER_ARGUMENT &arg, Texture *texture, TextureViewKey view_id,
    uint64_t encoder_id
) {
  if (!DebugShouldLogBinding(shader_hash))
    return;

  auto *allocation = texture->current();

  INFO(
      "DXMT diagnostic: shader texture binding",
      " stage=", DebugPipelineStageName(stage),
      " kind=", DebugPipelineKindName(kind),
      " shader=", shader_hash,
      " encoder=", encoder_id,
      " binding=SRV",
      " slot=", arg.SM50BindingSlot,
      " arg_index=", GetArgumentIndex(arg.Type, arg.SM50BindingSlot),
      " struct_qword=", arg.StructurePtrOffset,
      " flags=0x", std::hex, arg.Flags, std::dec,
      " texture_identity=", texture->diagnosticIdentity(),
      " view=", uint64_t(view_id),
      " view_index=", uint32_t(view_id.index),
      " view_mip=", uint32_t(view_id.mip_start), "..",
      uint32_t(view_id.mip_end),
      " view_array_slice=", uint32_t(view_id.array_start), "..",
      uint32_t(view_id.array_end),
      " view_format=", uint32_t(texture->pixelFormat(view_id)),
      " view_type=", DebugTextureTypeName(texture->textureType(view_id)), "(", uint32_t(texture->textureType(view_id)), ")",
      " view_size=", texture->width(view_id), "x", texture->height(view_id),
      " view_array=", texture->arrayLength(view_id),
      " resource_format=", uint32_t(texture->pixelFormat()),
      " resource_type=", DebugTextureTypeName(texture->textureType()), "(", uint32_t(texture->textureType()), ")",
      " resource_size=", texture->width(), "x", texture->height(), "x", texture->depth(),
      " resource_mips=", texture->miplevelCount(),
      " resource_usage=0x", std::hex, uint32_t(texture->usage()), std::dec,
      " sample_count=", texture->sampleCount(),
      " allocation=", uint64_t(allocation),
      " allocation_gpu_id=", allocation ? allocation->gpuResourceID : 0,
      " metal_texture=", allocation ? uint64_t(allocation->texture()) : 0
  );
}

DummyTextureBinding &
ArgumentEncodingContext::dummySRVTexture(const MTL_SM50_SHADER_ARGUMENT &arg) {
  auto kind = DummyTextureKindForArgument(arg);
  auto format = DummyTextureFormatForArgument(arg);
  auto &binding = dummy_srv_textures_[DummyTextureIndex(kind, format)];
  if (binding.texture)
    return binding;

  WMTTextureInfo info = {};
  info.type = DummyTextureType(kind);
  info.pixel_format = DummyTexturePixelFormat(format);
  info.usage = WMTTextureUsageShaderRead;
  info.options = WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked;
  info.width = 1;
  info.height = 1;
  info.depth = 1;
  info.mipmap_level_count = 1;
  info.sample_count = (kind == DummyTextureKind::Texture2DMultisampled ||
                       kind == DummyTextureKind::Texture2DMultisampledArray)
                          ? 2
                          : 1;
  info.array_length = DummyTextureArrayLength(kind);

  binding.texture = device_.newTexture(info);
  binding.gpu_resource_id = info.gpu_resource_id;
  binding.array_length = info.array_length;
  if (!binding.texture) {
    WARN(
        "DXMT diagnostic: failed to create dummy SRV texture",
        " type=", uint32_t(info.type),
        " format=", uint32_t(info.pixel_format),
        " sample_count=", uint32_t(info.sample_count)
    );
    return binding;
  }
  binding.residency_registration =
      queue_.RegisterLifetimeResidency(
          ResidencyOwnership::Lifetime(binding.texture));

  if (format != DummyTextureFormat::Depth && info.sample_count == 1) {
    std::array<uint32_t, 4> zero = {};
    uint32_t slices = info.type == WMTTextureTypeCube ? 6 : info.type == WMTTextureTypeCubeArray ? 6 * info.array_length
                                                                                                 : info.array_length;
    for (uint32_t slice = 0; slice < slices; slice++) {
      binding.texture.replaceRegion({0, 0, 0}, {1, 1, 1}, 0, slice, zero.data(), sizeof(zero), sizeof(zero));
    }
  }

  return binding;
}

template <PipelineStage stage, PipelineKind kind>
void
ArgumentEncodingContext::encodeShaderResources(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t offset,
    const std::string &shader_hash, const ShaderResourceBindingSnapshot *bindings,
    uint64_t demote_msaa_srv_mask_lo, uint64_t demote_msaa_srv_mask_hi
) {
  auto BindingCount = reflection->NumArguments;
  const auto table_size = uint64_t(reflection->ArgumentTableQwords) << 3;
  if (BindingCount && !table_size) {
    markArgumentBufferOverflow();
    return;
  }
  uint64_t *encoded_buffer =
      getMappedArgumentBuffer<uint64_t, stage == PipelineStage::Compute>(
          offset, table_size);
  if (!encoded_buffer)
    return;

  auto &UAVBindingSet = stage == PipelineStage::Compute ? cs_uav_ : om_uav_;
  auto encoder_id = currentEncoderId();

  for (unsigned i = 0; i < BindingCount; i++) {
    auto arg = arguments[i];
    if constexpr (stage == PipelineStage::Pixel) {
      if (arg.Type == SM50BindingType::SRV && arg.SM50BindingSlot < 128) {
        bool demote_msaa =
            arg.SM50BindingSlot < 64
                ? (demote_msaa_srv_mask_lo & (uint64_t(1) << arg.SM50BindingSlot))
                : (demote_msaa_srv_mask_hi & (uint64_t(1) << (arg.SM50BindingSlot - 64)));
        if (demote_msaa) {
          arg.Flags = MTL_SM50_SHADER_ARGUMENT_FLAG(
              arg.Flags & ~MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLED
          );
        }
      }
    }
    switch (arg.Type) {
    case SM50BindingType::ConstantBuffer: {
      DXMT_UNREACHABLE
    }
    case SM50BindingType::Sampler: {
      auto slot = 16 * unsigned(stage) + arg.SM50BindingSlot;
      auto sampler = bindings ? bindings[i].sampler : sampler_[slot].sampler.ptr();
      if (!sampler) {
        EncodeMirrorSamplerSlotNull(&encoded_buffer[arg.StructurePtrOffset], dummy_sampler_info_.gpu_resource_id);
        break;
      }
      EncodeMirrorSamplerSlot(&encoded_buffer[arg.StructurePtrOffset], *sampler);
      break;
    }
    case SM50BindingType::SRV: {
      auto slot = 128 * unsigned(stage) + arg.SM50BindingSlot;
      auto &srv = bindings ? bindings[i].srv : resview_[slot];

      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER) {
        if (srv.buffer.ptr()) {
          auto [srv_alloc, offset] = access<stage>(srv.buffer, srv.slice.byteOffset, srv.slice.byteLength, ResourceAccess::Read);
          encoded_buffer[arg.StructurePtrOffset] = srv_alloc->gpuAddress() + offset + srv.slice.byteOffset;
          encoded_buffer[arg.StructurePtrOffset + 1] = srv.slice.byteLength;
          makeResident<stage, kind>(srv.buffer.ptr());
        } else {
          DebugLogNullShaderBinding<stage, kind>(
              "SRV", "buffer", shader_hash, arg, bool(srv.buffer.ptr()), bool(srv.texture.ptr()), false, encoder_id
          );
          encoded_buffer[arg.StructurePtrOffset] = 0;
          encoded_buffer[arg.StructurePtrOffset + 1] = 0;
        }
      } else if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE) {
        if (srv.buffer.ptr()) {
          assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET);
          auto allocation = srv.buffer->current();
          auto *view_ptr = srv.buffer->tryView(srv.viewId, allocation);
          if (!view_ptr || !view_ptr->texture) {
            DebugLogNullShaderBinding<stage, kind>(
                "SRV", "texture-buffer", shader_hash, arg,
                bool(srv.buffer.ptr()), bool(srv.texture.ptr()), false,
                encoder_id, "invalid_buffer_view"
            );
            encoded_buffer[arg.StructurePtrOffset] = 0;
            encoded_buffer[arg.StructurePtrOffset + 1] = 0;
            break;
          }
          auto [view, offset] = access<stage>(srv.buffer, srv.viewId, ResourceAccess::Read);
          encoded_buffer[arg.StructurePtrOffset] = view.gpu_resource_id;
          encoded_buffer[arg.StructurePtrOffset + 1] =
              ((uint64_t)srv.slice.elementCount << 32) | (uint64_t)(srv.slice.firstElement + offset);
          makeResident<stage, kind>(srv.buffer.ptr(), srv.viewId);
        } else if (srv.texture.ptr()) {
          assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP);
          auto viewIdChecked = srv.texture->checkViewUseArray(srv.viewId, arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY);
          DebugLogTextureBindingMismatch<stage, kind>(shader_hash, arg, srv.texture.ptr(), viewIdChecked, encoder_id);
          DebugLogShaderTextureBinding<stage, kind>(shader_hash, arg, srv.texture.ptr(), viewIdChecked, encoder_id);
          EncodeMirrorTextureSlot(
              &encoded_buffer[arg.StructurePtrOffset],
              access<stage>(srv.texture, viewIdChecked, ResourceAccess::Read).gpuResourceID,
              srv.texture->arrayLength(viewIdChecked));
          makeResident<stage, kind>(srv.texture.ptr(), viewIdChecked);
        } else {
          auto &dummy_texture = dummySRVTexture(arg);
          DebugLogNullShaderBinding<stage, kind>(
              "SRV", "texture", shader_hash, arg, bool(srv.buffer.ptr()), bool(srv.texture.ptr()), false, encoder_id,
              dummy_texture.texture ? "dummy_texture" : "zero"
          );
          if (dummy_texture.texture) {
            encoded_buffer[arg.StructurePtrOffset] = dummy_texture.gpu_resource_id;
            encoded_buffer[arg.StructurePtrOffset + 1] = TextureMetadata(dummy_texture.array_length, 0);
            DXMT_RESOURCE_RESIDENCY requested = GetResidencyMask<kind>(stage, true, false);
            if (CheckResourceResidency(dummy_texture.residency, currentEncoderId(), requested))
              makeResident<stage, kind>(dummy_texture.texture, requested);
          } else {
            encoded_buffer[arg.StructurePtrOffset] = 0;
            encoded_buffer[arg.StructurePtrOffset + 1] = 0;
          }
        }
      }
      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER) {
        assert(0 && "srv can not have counter associated");
      }
      break;
    }
    case SM50BindingType::UAV: {
      auto &uav = UAVBindingSet[arg.SM50BindingSlot];
      bool read = (arg.Flags >> 10) & 1, write = (arg.Flags >> 10) & 2;
      if (!read && !write) {
        read = true;
        write = true;
      }
      int access_flags = (read ? ResourceAccess::Read : 0) |
                         (write ? ResourceAccess::Write : 0) |
                         ResourceAccess::UAV;

      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER) {
        if (uav.buffer.ptr()) {
          auto [uav_alloc, offset] = access<stage>(uav.buffer, uav.slice.byteOffset, uav.slice.byteLength, access_flags);
          encoded_buffer[arg.StructurePtrOffset] = uav_alloc->gpuAddress() + offset + uav.slice.byteOffset;
          encoded_buffer[arg.StructurePtrOffset + 1] = uav.slice.byteLength;
          makeResident<stage, kind>(uav.buffer.ptr(), read, write);
        } else {
          DebugLogNullShaderBinding<stage, kind>(
              "UAV", "buffer", shader_hash, arg, bool(uav.buffer.ptr()), bool(uav.texture.ptr()), bool(uav.counter.ptr()),
              encoder_id
          );
          encoded_buffer[arg.StructurePtrOffset] = 0;
          encoded_buffer[arg.StructurePtrOffset + 1] = 0;
        }
      } else if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE) {
        if (uav.buffer.ptr()) {
          assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET);
          auto allocation = uav.buffer->current();
          auto *view_ptr = uav.buffer->tryView(uav.viewId, allocation);
          if (!view_ptr || !view_ptr->texture) {
            DebugLogNullShaderBinding<stage, kind>(
                "UAV", "texture-buffer", shader_hash, arg,
                bool(uav.buffer.ptr()), bool(uav.texture.ptr()),
                bool(uav.counter.ptr()), encoder_id, "invalid_buffer_view"
            );
            encoded_buffer[arg.StructurePtrOffset] = 0;
            encoded_buffer[arg.StructurePtrOffset + 1] = 0;
            break;
          }
          auto [view, offset] = access<stage>(uav.buffer, uav.viewId, access_flags);
          encoded_buffer[arg.StructurePtrOffset] = view.gpu_resource_id;
          encoded_buffer[arg.StructurePtrOffset + 1] =
              ((uint64_t)uav.slice.elementCount << 32) | (uint64_t)(uav.slice.firstElement + offset);
          makeResident<stage, kind>(uav.buffer.ptr(), uav.viewId, read, write);
        } else if (uav.texture.ptr()) {
          assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP);
          auto viewIdChecked = uav.texture->checkViewUseArray(uav.viewId, arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY);
          auto &view = access<stage>(uav.texture, viewIdChecked, access_flags);
          EncodeMirrorTextureSlot(
              &encoded_buffer[arg.StructurePtrOffset],
              view.gpuResourceID,
              uav.texture->arrayLength(viewIdChecked));
          makeResident<stage, kind>(uav.texture.ptr(), viewIdChecked, read, write);
        } else {
          DebugLogNullShaderBinding<stage, kind>(
              "UAV", "texture", shader_hash, arg, bool(uav.buffer.ptr()), bool(uav.texture.ptr()), bool(uav.counter.ptr()),
              encoder_id
          );
          encoded_buffer[arg.StructurePtrOffset] = 0;
          encoded_buffer[arg.StructurePtrOffset + 1] = 0;
        }
      }
      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER) {
        if (uav.counter) {
          auto [counter_alloc, offset] = access<stage>(uav.counter, 0, 4, ResourceAccess::All);
          encoded_buffer[arg.StructurePtrOffset + 2] = counter_alloc->gpuAddress() + offset;
          makeResident<stage, kind>(uav.counter.ptr(), true, true);
        } else {
          DebugLogNullShaderBinding<stage, kind>(
              "UAV_COUNTER", "counter", shader_hash, arg, bool(uav.buffer.ptr()), bool(uav.texture.ptr()),
              bool(uav.counter.ptr()), encoder_id
          );
          /*
           * potentially cause gpu pagefault, even providing a dummy buffer doesn't improve since the returned
           * counter value is likely to be used as an index to another read/write operation.
           */
          encoded_buffer[arg.StructurePtrOffset + 2] = 0;
        }
      }
      break;
    }
    }
  }

  if constexpr (stage == PipelineStage::Compute) {
    const auto binding_offset = deduplicateComputeArgumentTableSlice(30, offset, table_size);
    const auto final_offset = getFinalArgumentBufferOffset<true>(binding_offset);
    if (!shouldEmitComputeArgumentBufferOffset(30, final_offset, binding_offset != offset))
      return;
    auto &cmd = encodeComputeCommand<wmtcmd_compute_setargumentbufferoffset>();
    cmd.type = WMTComputeCommandSetArgumentBufferOffset;
    cmd.offset = final_offset;
    cmd.index = 30;
  } else {
    uint8_t index = 30;
    WMTRenderStages stages = WMTRenderStageVertex;
    if constexpr (stage == PipelineStage::Vertex) {
      if constexpr (kind == PipelineKind::Geometry)
        stages = WMTRenderStageObject;
      else if constexpr (kind == PipelineKind::Tessellation) {
        stages = WMTRenderStageObject;
        index = 28;
      } else
        stages = WMTRenderStageVertex;
    } else if constexpr (stage == PipelineStage::Pixel) {
      stages = WMTRenderStageFragment;
    } else if constexpr (stage == PipelineStage::Hull) {
      stages = WMTRenderStageObject;
    } else if constexpr (stage == PipelineStage::Domain) {
      stages = WMTRenderStageMesh;
    } else if constexpr (stage == PipelineStage::Geometry) {
      stages = WMTRenderStageMesh;
    } else {
      assert(0 && "Not implemented or unreachable");
    }
    const auto binding_offset = deduplicateRenderArgumentTableSlice(stages, index, offset, table_size);
    const auto final_offset = getFinalArgumentBufferOffset(binding_offset);
    if (!shouldEmitRenderArgumentBufferOffset(stages, index, final_offset, binding_offset != offset))
      return;
    auto &cmd = encodeRenderCommand<wmtcmd_render_setargumentbufferoffset>();
    cmd.type = WMTRenderCommandSetArgumentBufferOffset;
    cmd.index = index;
    cmd.stages = stages;
    cmd.offset = final_offset;
  }
}

bool
ArgumentEncodingContext::retainAllocation(Allocation* allocation) {
  if (allocation->checkRetained(seq_id_))
    return false;
  queue_.Retain(seq_id_, allocation);
  return true;
}

void
ArgumentEncodingContext::beginCommandBufferResourceLifetime(
    WMT::CommandBuffer command_buffer,
    std::vector<WMT::Reference<WMT::Resource>> &resources,
    std::unordered_set<obj_handle_t> &resource_handles) {
  assert(!current_command_buffer_);
  assert(!current_command_buffer_resources_);
  assert(!current_command_buffer_resource_handles_);
  current_command_buffer_ = command_buffer;
  current_command_buffer_resources_ = &resources;
  current_command_buffer_resource_handles_ = &resource_handles;
}

void
ArgumentEncodingContext::endCommandBufferResourceLifetime() {
  current_command_buffer_ = {};
  current_command_buffer_resources_ = nullptr;
  current_command_buffer_resource_handles_ = nullptr;
}

bool
ArgumentEncodingContext::tryRetainResourceForCurrentCommandBuffer(
    WMT::Resource resource) {
#if DXMT_DX12_METAL4
  if (!resource)
    return false;
  // Fail-closed: no generation ⇒ no pin (callers must not write host mappings
  // of unpinned argument buffers).
  if (!hasActiveCommandBufferGeneration())
    return false;
  if (!current_command_buffer_resource_handles_->insert(resource.handle).second)
    return true; // already pinned for this generation

  // GPTK D3DMCommandQueue::RetainResourceForCurrentCommandBuffer: CPU-side
  // retain list + refcount only. Do NOT call Metal residency set APIs here —
  // lifetime residency is RegisterResource/AddPersistentResidency at create,
  // and mixing membershipLock+containsAllocation on this path deadlocks with
  // RenderThread addAllocation (BMW hang ~frame 1188).
  current_command_buffer_resources_->emplace_back(resource);
  return true;
#else
  (void)resource;
  return true;
#endif
}

void
ArgumentEncodingContext::retainResourceForCurrentCommandBuffer(
    WMT::Resource resource) {
#if DXMT_DX12_METAL4
  // Soft-skip only for non-argument-buffer retain paths (residency probes).
  // AllocateArgumentBuffer uses tryRetain and fails closed on false.
  (void)tryRetainResourceForCurrentCommandBuffer(resource);
#else
  (void)resource;
#endif
}

void
ArgumentEncodingContext::clearColor(
    Rc<Texture> &&texture, uint64_t viewId, unsigned arrayLength,
    WMTClearColor color, bool has_rects, uint32_t depth_plane) {
  assert(!encoder_current);
  auto encoder_info = allocate<ClearEncoderData>();
  encoder_info->type = EncoderType::Clear;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->clear_dsv = 0;
  encoder_info->color = color;
  encoder_info->has_rects = has_rects;
  encoder_info->array_length = arrayLength;
  TextureViewKey view = viewId;
  encoder_info->width = texture->width(view);
  encoder_info->height = texture->height(view);
  encoder_info->sample_count = texture->sampleCount();
  encoder_current = encoder_info;

  encoder_info->attachment = access(texture, viewId, ResourceAccess::Write);
  encoder_info->level = 0;
  encoder_info->slice = 0;
  encoder_info->depth_plane = depth_plane;
  encoder_info->stencil_depth_plane = 0;

  currentFrameStatistics().clear_pass_count++;

  endPass();
}

void
ArgumentEncodingContext::clearColor(
    Rc<Buffer> &&buffer, uint64_t viewId, unsigned width,
    WMTClearColor color, bool has_rects) {
  assert(!encoder_current);
  auto encoder_info = allocate<ClearEncoderData>();
  encoder_info->type = EncoderType::Clear;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->clear_dsv = 0;
  encoder_info->color = color;
  encoder_info->has_rects = has_rects;
  encoder_info->array_length = 0;
  encoder_info->width = width;
  encoder_info->height = 1;
  encoder_info->sample_count = 1;
  encoder_current = encoder_info;

  auto [view, suballocation_offset] = access<PipelineStage::Pixel>(buffer, viewId, ResourceAccess::Write);
  if (suballocation_offset)
    WARN("ClearRenderTargetView: buffer RTV suballocation offset is not supported offset=", suballocation_offset);
  encoder_info->buffer_attachment = std::move(buffer);
  encoder_info->buffer_view_id = viewId;
  encoder_info->buffer_texture = view.texture;

  currentFrameStatistics().clear_pass_count++;

  endPass();
}

void
ArgumentEncodingContext::clearDepthStencil(
    Rc<Texture> &&texture, uint64_t viewId, unsigned arrayLength, unsigned flag,
    float depth, uint8_t stencil, bool has_rects
) {
  assert(!encoder_current);
  auto encoder_info = allocate<ClearEncoderData>();
  encoder_info->type = EncoderType::Clear;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->clear_dsv = flag & DepthStencilPlanarFlags(texture->pixelFormat());
  encoder_info->depth_stencil = {depth, stencil};
  encoder_info->has_rects = has_rects;
  encoder_info->array_length = arrayLength;
  TextureViewKey view = viewId;
  encoder_info->width = texture->width(view);
  encoder_info->height = texture->height(view);
  encoder_info->sample_count = texture->sampleCount();
  encoder_current = encoder_info;

  encoder_info->attachment = access(texture, viewId, ResourceAccess::Write);
  encoder_info->level = 0;
  encoder_info->slice = 0;
  encoder_info->depth_plane = 0;
  encoder_info->stencil_depth_plane = 0;

  currentFrameStatistics().clear_pass_count++;
  
  endPass();
}

void
ArgumentEncodingContext::resolveTexture(
    Rc<Texture> &&src, TextureViewKey src_view, Rc<Texture> &&dst, TextureViewKey dst_view,
    WMT::RenderPipelineState pso, std::optional<WMTScissorRect> src_rect,
    WMTOrigin dst_origin, WMTSize resolve_size
) {
  assert(!encoder_current);
  auto encoder_info = allocate<ResolveEncoderData>();
  encoder_info->type = EncoderType::Resolve;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_current = encoder_info;

  encoder_info->src = access(src, src_view, ResourceAccess::Read);
  encoder_info->dst = access(dst, dst_view, ResourceAccess::Write);
  encoder_info->pso = pso;
  encoder_info->src_rect = src_rect;
  encoder_info->dst_origin = dst_origin;
  encoder_info->resolve_size = resolve_size;

  endPass();
};

void
ArgumentEncodingContext::resolveDepthTexture(
    Rc<Texture> &&src, TextureViewKey src_view, Rc<Texture> &&dst, TextureViewKey dst_view,
    WMT::RenderPipelineState pso, std::optional<WMTScissorRect> src_rect, WMTOrigin dst_origin, WMTSize resolve_size
) {
  assert(!encoder_current);
  auto encoder_info = allocate<ResolveEncoderData>();
  encoder_info->type = EncoderType::Resolve;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_current = encoder_info;

  encoder_info->src = access(src, src_view, ResourceAccess::Read);
  encoder_info->dst = access(dst, dst_view, ResourceAccess::Write);
  encoder_info->pso = pso;
  encoder_info->src_rect = src_rect;
  encoder_info->dst_origin = dst_origin;
  encoder_info->resolve_size = resolve_size;
  encoder_info->is_depth = true;

  endPass();
};

void
ArgumentEncodingContext::present(Rc<Texture> &texture, Rc<Presenter> &presenter,
                                 double after, DXMTPresentMetadata metadata,
                                 uint64_t apitrace_frame_index,
                                 uint32_t sync_interval, uint32_t flags) {
  present(texture, texture->fullView, presenter, after, metadata,
          apitrace_frame_index, sync_interval, flags);
}

void
ArgumentEncodingContext::stretchBlit(
    Rc<Texture> &&src, TextureViewKey src_view, Rc<Texture> &&dst, TextureViewKey dst_view,
    WMT::RenderPipelineState pso, WMT::SamplerState sampler,
    WMTOrigin src_origin, WMTSize src_size,
    WMTOrigin dst_origin, WMTSize dst_size
) {
  assert(!encoder_current);
  // The src texture-view's level dimensions normalize the sample sub-
  // rect into uv-space. dxmt::Texture::width/height(view_key) returns
  // max(info_.width >> view.mip_start, 1); already guards against
  // zero, so a divide-by-zero is impossible.
  uint32_t src_view_w = src ? src->width(src_view) : 1;
  uint32_t src_view_h = src ? src->height(src_view) : 1;

  auto encoder_info = allocate<StretchBlitEncoderData>();
  encoder_info->type = EncoderType::StretchBlit;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_current = encoder_info;

  encoder_info->src = access(src, src_view, ResourceAccess::Read);
  encoder_info->dst = access(dst, dst_view, ResourceAccess::Write);
  encoder_info->pso = pso;
  encoder_info->sampler = sampler;
  encoder_info->src_uv_origin[0] = float(src_origin.x) / float(src_view_w);
  encoder_info->src_uv_origin[1] = float(src_origin.y) / float(src_view_h);
  encoder_info->src_uv_size[0] = float(src_size.width) / float(src_view_w);
  encoder_info->src_uv_size[1] = float(src_size.height) / float(src_view_h);
  encoder_info->dst_origin = dst_origin;
  encoder_info->dst_size = dst_size;

  endPass();
}

void
ArgumentEncodingContext::copyTexture(
    const Rc<Texture> &src, unsigned src_level, unsigned src_slice, WMTOrigin src_origin,
    const Rc<Texture> &dst, unsigned dst_level, unsigned dst_slice, WMTOrigin dst_origin,
    WMTSize size
) {
  assert(!encoder_current);
  // Blit-encoder texture-to-texture copy in its own pass: a 1:1 sample-for-sample
  // copy of one subresource region, matched sample count (single-sample or MSAA
  // alike). The access(src, Read) + access(dst, Write) register the pass in the
  // fence trackers, so a write-after-read against slot 0 (the swapchain rotation)
  // or a read-after-write chain is ordered by the encoder scheduler; this is the
  // same primitive EmitBlitOp_d9 emits for the d3d9 StretchRect Copy path.
  startBlitPass();
  auto src_tex = access<PipelineStage::Compute>(src, src_level, src_slice, ResourceAccess::Read);
  auto dst_tex = access<PipelineStage::Compute>(dst, dst_level, dst_slice, ResourceAccess::Write);
  auto &cmd = encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
  cmd.type = WMTBlitCommandCopyFromTextureToTexture;
  cmd.src = src_tex.handle;
  cmd.src_slice = src_slice;
  cmd.src_level = src_level;
  cmd.src_origin = src_origin;
  cmd.src_size = size;
  cmd.dst = dst_tex.handle;
  cmd.dst_slice = dst_slice;
  cmd.dst_level = dst_level;
  cmd.dst_origin = dst_origin;
  endPass();
}

// Only the Metal3 backend enqueues this (its sole caller is the d3d9 StretchRect
// scale path, whose StretchBlit encoder is itself compiled out under Metal4).
#if !DXMT_DX12_METAL4
void
ArgumentEncodingContext::optimizeTextureForGPUAccess(const Rc<Texture> &texture, unsigned level, unsigned slice) {
  assert(!encoder_current);
  // Write access: the re-tile mutates the texture's layout, so it must register
  // as the subresource's last writer for the following sample to wait on it.
  startBlitPass();
  auto tex = access<PipelineStage::Compute>(texture, level, slice, ResourceAccess::Write);
  auto &cmd = encodeBlitCommand<wmtcmd_blit_optimize_contents>();
  cmd.type = WMTBlitCommandOptimizeContentsForGPUAccess;
  cmd.texture = tex.handle;
  cmd.slice = slice;
  cmd.level = level;
  endPass();
}
#endif

void
ArgumentEncodingContext::present(Rc<Texture> &texture, TextureViewKey view,
                                 Rc<Presenter> &presenter, double after,
                                 DXMTPresentMetadata metadata,
                                 uint64_t apitrace_frame_index,
                                 uint32_t sync_interval, uint32_t flags) {
  assert(!encoder_current);
  auto encoder_info = allocate<PresentData>();
  encoder_info->type = EncoderType::Present;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->presenter = presenter;
  encoder_info->after = after;
  encoder_info->metadata = metadata;
  encoder_info->apitrace_frame_index = apitrace_frame_index;
  encoder_info->sync_interval = sync_interval;
  encoder_info->flags = flags;

  encoder_current = encoder_info;
  TextureViewKey present_view = uint64_t(view) ? view : texture->fullView;
  encoder_info->backbuffer = access(texture, present_view, ResourceAccess::Read).texture;
  endPass();
}

void
ArgumentEncodingContext::upscale(Rc<Texture> &texture, Rc<Texture> &upscaled, Rc<SpatialScaler> &scaler) {
  assert(!encoder_current);
  auto encoder_info = allocate<SpatialUpscaleData>();
  encoder_info->type = EncoderType::SpatialUpscale;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->scaler = scaler;

  encoder_current = encoder_info;
  encoder_info->backbuffer = access(texture, texture->fullView, ResourceAccess::Read).texture;
  encoder_info->upscaled = access(upscaled, upscaled->fullView, ResourceAccess::Write).texture;
  endPass();
}

void
ArgumentEncodingContext::upscaleTemporal(
    Rc<Texture> &input, Rc<Texture> &output, Rc<Texture> &depth, Rc<Texture> &motion_vector, TextureViewKey mvViewId,
    Rc<Texture> &exposure, Rc<TemporalScaler> &scaler, const WMTFXTemporalScalerProps &props
) {
  assert(!encoder_current);
  auto encoder_info = allocate<TemporalUpscaleData>();
  encoder_info->type = EncoderType::TemporalUpscale;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->scaler = scaler;
  encoder_info->props = props;

  encoder_current = encoder_info;
  encoder_info->input = access(input, input->fullView, ResourceAccess::Read).texture;
  encoder_info->depth = access(depth, depth->fullView, ResourceAccess::Read).texture;
  encoder_info->motion_vector = access(motion_vector, mvViewId, ResourceAccess::Read).texture;
  encoder_info->output = access(output, output->fullView, ResourceAccess::Write).texture;
  if (exposure) {
    encoder_info->exposure = access(exposure, exposure->fullView, ResourceAccess::Read).texture;
  }
  endPass();
}

void
ArgumentEncodingContext::signalEvent(uint64_t value) {
  assert(!encoder_current);
  auto encoder_info = allocate<SignalEventData>();
  encoder_info->type = EncoderType::SignalEvent;
  encoder_info->id = ~0ull;
  encoder_info->event = queue_.event;
  encoder_info->value = value;

  encoder_current = encoder_info;
  endPass();
}

void
ArgumentEncodingContext::signalEvent(WMT::Reference<WMT::Event> &&event, uint64_t value) {
  assert(!encoder_current);
  auto encoder_info = allocate<SignalEventData>();
  encoder_info->type = EncoderType::SignalEvent;
  encoder_info->id = ~0ull;
  encoder_info->event = std::move(event);
  encoder_info->value = value;

  encoder_current = encoder_info;
  endPass();
}

void
ArgumentEncodingContext::signalEventByHandle(obj_handle_t event_handle, uint64_t value) {
  assert(!encoder_current);
  auto encoder_info = allocate<SignalEventData>();
  encoder_info->type = EncoderType::SignalEvent;
  encoder_info->id = ~0ull;
  // Reference operator=(Class non_retained); retains, paired with the
  // SignalEventData destructor's release. The retain runs on the encode
  // thread so the calling-thread emit site dodges a wine_unix_call.
  encoder_info->event = WMT::Event{event_handle};
  encoder_info->value = value;

  encoder_current = encoder_info;
  endPass();
}

void
ArgumentEncodingContext::waitEvent(WMT::Reference<WMT::Event> &&event, uint64_t value) {
  assert(!encoder_current);
  auto encoder_info = allocate<WaitForEventData>();
  encoder_info->type = EncoderType::WaitForEvent;
  encoder_info->id = ~0ull;
  encoder_info->event = std::move(event);
  encoder_info->value = value;

  encoder_current = encoder_info;
  endPass();
}

RenderEncoderData *
ArgumentEncodingContext::startRenderPass(
    uint8_t dsv_planar_flags, uint8_t dsv_readonly_flags, uint8_t render_target_count, uint64_t encoder_argbuf_size
) {
  assert(!encoder_current);
  auto encoder_info = allocate<RenderEncoderData>();
  encoder_info->type = EncoderType::Render;
  encoder_info->encoder_id_vertex = nextEncoderId();
  encoder_info->fence_wait_vertex = {};
  encoder_info->fence_update_vertex = {encoder_info->encoder_id_vertex};
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->cmd_head.type = WMTRenderCommandNop;
  encoder_info->cmd_head.next.set(0);
  encoder_info->cmd_tail = (wmtcmd_base *)&encoder_info->cmd_head;
  encoder_info->dsv_planar_flags = dsv_planar_flags;
  encoder_info->dsv_readonly_flags = dsv_readonly_flags;
  encoder_info->render_target_count = render_target_count;
  auto argbuf = queue_.AllocateArgumentBuffer(seq_id_, encoder_argbuf_size);
  // AllocateArgumentBuffer already generation-pins; store owned slice for L2 writes.
  encoder_info->argbuf_slice = argbuf;
  encoder_current = encoder_info;

  currentFrameStatistics().render_pass_count++;

  vro_state_.beginEncoder();

  return encoder_info;
}

EncoderData *
ArgumentEncodingContext::startComputePass(uint64_t encoder_argbuf_size) {
  assert(!encoder_current);
  auto encoder_info = allocate<ComputeEncoderData>();
  encoder_info->type = EncoderType::Compute;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->cmd_head.type = WMTComputeCommandNop;
  encoder_info->cmd_head.next.set(0);
  encoder_info->cmd_tail = (wmtcmd_base *)&encoder_info->cmd_head;
  auto argbuf = queue_.AllocateArgumentBuffer(seq_id_, encoder_argbuf_size);
  encoder_info->argbuf_slice = argbuf;
  encoder_current = encoder_info;

  currentFrameStatistics().compute_pass_count++;

  return encoder_info;
}

EncoderData *
ArgumentEncodingContext::startBlitPass() {
  assert(!encoder_current);
  auto encoder_info = allocate<BlitEncoderData>();
  encoder_info->type = EncoderType::Blit;
  encoder_info->id = nextEncoderId();
  encoder_info->fence_wait = {};
  encoder_info->fence_update = {encoder_info->id};
  encoder_info->cmd_head.type = WMTBlitCommandNop;
  encoder_info->cmd_head.next.set(0);
  encoder_info->cmd_tail = (wmtcmd_base *)&encoder_info->cmd_head;
  encoder_current = encoder_info;

  currentFrameStatistics().blit_pass_count++;

  return encoder_info;
}

void
ArgumentEncodingContext::endPass() {
  assert(encoder_current);
  noteCurrentEncoderD3DSequence();

  if (FenceOnlyBlitOptimizationEnabled()) {
    if (tryDeferFenceOnlyBlitPass(encoder_current)) {
      currentFrameStatistics().blit_pass_deferred_fence_only_count++;
      encoder_current = nullptr;
      return;
    }

    switch (encoder_current->type) {
    case EncoderType::SignalEvent:
    case EncoderType::WaitForEvent:
    case EncoderType::SampleTimestamp:
    case EncoderType::ResolveTimestamp:
      appendPendingFenceOnlyBlitPass();
      break;
    default:
      mergePendingFenceOnlyBlitPassInto(encoder_current);
      break;
    }
  }

  if (encoder_current->id != ~0ull) {
    if (encoder_current->type == EncoderType::Render) {
      vro_state_.endEncoder();
      auto render_encoder = static_cast<RenderEncoderData *>(encoder_current);

      if (render_encoder->depth.attachment && !(render_encoder->dsv_readonly_flags & 1))
        access<PipelineStage::Pixel>(
            render_encoder->depth.attachment->allocation->descriptor, render_encoder->depth.attachment->key,
            ResourceAccess::Write
        );
      if (render_encoder->stencil.attachment && !(render_encoder->dsv_readonly_flags & 2))
        access<PipelineStage::Pixel>(
            render_encoder->stencil.attachment->allocation->descriptor, render_encoder->stencil.attachment->key,
            ResourceAccess::Write
        );

      render_encoder->fence_wait_vertex =
          fence_locality_.collectAndSimplifyWaits(
              render_encoder->fence_wait_vertex,
              render_encoder->encoder_id_vertex,
              false,
              "render_vertex");
      encoder_current->fence_wait =
          fence_locality_.collectAndSimplifyWaits(
              encoder_current->fence_wait,
              encoder_current->id,
              true,
              "render_fragment");
    } else {
      const char *trace_scope = "encoder";
      switch (encoder_current->type) {
      case EncoderType::Compute:
        trace_scope = "compute";
        break;
      case EncoderType::Blit:
        trace_scope = "blit";
        break;
      case EncoderType::Clear:
        trace_scope = "clear";
        break;
      case EncoderType::Resolve:
        trace_scope = "resolve";
        break;
      case EncoderType::Present:
        trace_scope = "present";
        break;
      default:
        break;
      }
      encoder_current->fence_wait =
          fence_locality_.collectAndSimplifyWaits(
              encoder_current->fence_wait,
              encoder_current->id,
              false,
              trace_scope);
    }
  }

  encoder_last->next = encoder_current;
  encoder_last = encoder_current;
  encoder_current = nullptr;
  encoder_count_++;
}

void
ArgumentEncodingContext::noteCurrentEncoderD3DSequence() {
  assert(encoder_current);
  uint64_t sequence = dxmt::apitrace::current_d3d_sequence();
  if (!sequence)
    sequence = seq_id_;
  if (!encoder_current->diagnostic_d3d_sequence_begin)
    encoder_current->diagnostic_d3d_sequence_begin = sequence;
  encoder_current->diagnostic_d3d_sequence_end = sequence;
}

std::pair<WMT::Buffer, size_t>
ArgumentEncodingContext::allocateTempBuffer(size_t size, size_t alignment) {
  auto allocation = queue_.AllocateTempBuffer(seq_id_, size, alignment);
  retainResourceForCurrentCommandBuffer(allocation.first);
  return allocation;
};

AllocatedTempBufferSlice
ArgumentEncodingContext::allocateTempBuffer1(size_t size, size_t alignment) {
  auto allocation = queue_.AllocateTempBuffer1(seq_id_, size, alignment);
  retainResourceForCurrentCommandBuffer(allocation.gpu_buffer);
  return allocation;
};

void
ArgumentEncodingContext::beginVisibilityResultQuery(Rc<VisibilityResultQuery> &&query) {
  query->begin(seq_id_, vro_state_.getNextReadOffset());
  active_visibility_query_count_++;
  pending_queries_.push_back(std::move(query));
}
void
ArgumentEncodingContext::endVisibilityResultQuery(Rc<VisibilityResultQuery> &&query) {
  query->end(seq_id_, vro_state_.getNextReadOffset());
  assert(active_visibility_query_count_);
  active_visibility_query_count_--;
}
void
ArgumentEncodingContext::bumpVisibilityResultOffset() {
  auto render_encoder = currentRenderEncoder();
  render_encoder->use_visibility_result = render_encoder->use_visibility_result || bool(active_visibility_query_count_);

  uint64_t offset;
  if (vro_state_.tryGetNextWriteOffset(active_visibility_query_count_, offset)) {
    auto &cmd = encodeRenderCommand<wmtcmd_render_setvisibilitymode>();
    cmd.type = WMTRenderCommandSetVisibilityMode;
    if (~offset == 0) {
      cmd.mode = WMTVisibilityResultModeDisabled;
      cmd.offset = 0;
    } else {
      cmd.mode = WMTVisibilityResultModeCounting;
      cmd.offset = offset << 3;
    }
  }
}

FrameStatistics&
ArgumentEncodingContext::currentFrameStatistics() {
  return queue_.statistics.at(frame_id_);
}

void
ArgumentEncodingContext::sampleTimestamp(Rc<TimestampQuery> &&query) {
  assert(!encoder_current);
  if (encoder_last && encoder_last->type == EncoderType::SampleTimestamp) {
    auto *last = static_cast<SampleTimestampData *>(encoder_last);
    if (query->sampleIndex() == ~0ull) {
      timestamp_state_.coalesceQuery(query.ptr());
    } else {
      timestamp_state_.addQueryAt(query.ptr(), query->sampleIndex());
    }
    last->queries.push_back(std::move(query));
    return;
  }
  auto encoder_info = allocate<SampleTimestampData>();
  encoder_info->type = EncoderType::SampleTimestamp;
  encoder_info->id = ~0ull;
  encoder_info->readback_index =
      query->sampleIndex() == ~0ull
          ? timestamp_state_.addQuery(query.ptr())
          : timestamp_state_.addQueryAt(query.ptr(), query->sampleIndex());
  encoder_info->queries = {};
  encoder_info->queries.push_back(std::move(query));

  encoder_current = encoder_info;
  endPass();
}

void
ArgumentEncodingContext::resolveTimestamp(uint64_t start_index, uint64_t query_count,
                                          WMT::Reference<WMT::Buffer> dst_buffer,
                                          uint64_t dst_offset, uint64_t dst_length) {
#if DXMT_DX12_METAL4
  resolveTimestamp({}, start_index, query_count, std::move(dst_buffer),
                   dst_offset, dst_length);
#else
  appendResolveTimestampRange(start_index, query_count, std::move(dst_buffer),
                              dst_offset, dst_length);
#endif
}

#if DXMT_DX12_METAL4
void ArgumentEncodingContext::resolveTimestamp(
    WMT::Reference<WMT::CounterHeap> src_heap, uint64_t start_index,
    uint64_t query_count, WMT::Reference<WMT::Buffer> dst_buffer,
    uint64_t dst_offset, uint64_t dst_length) {
  appendResolveTimestampRange(std::move(src_heap), start_index, query_count,
                              std::move(dst_buffer), dst_offset, dst_length);
}
#endif

void ArgumentEncodingContext::appendResolveTimestampRange(
#if DXMT_DX12_METAL4
    WMT::Reference<WMT::CounterHeap> src_heap,
#endif
    uint64_t start_index, uint64_t query_count,
    WMT::Reference<WMT::Buffer> dst_buffer, uint64_t dst_offset,
    uint64_t dst_length) {
  assert(!encoder_current);
  if (!query_count || !dst_buffer || !dst_length)
    return;
  retainResourceForCurrentCommandBuffer(dst_buffer);

  if (encoder_last && encoder_last->type == EncoderType::ResolveTimestamp) {
    auto *last = static_cast<ResolveTimestampData *>(encoder_last);
    last->ranges.push_back({
#if DXMT_DX12_METAL4
        std::move(src_heap),
#endif
        std::move(dst_buffer),
        start_index,
        query_count,
        dst_offset,
        dst_length});
    return;
  }

  auto encoder_info = allocate<ResolveTimestampData>();
  encoder_info->type = EncoderType::ResolveTimestamp;
  encoder_info->id = ~0ull;
  encoder_info->ranges.push_back({
#if DXMT_DX12_METAL4
      std::move(src_heap),
#endif
      std::move(dst_buffer),
      start_index,
      query_count,
      dst_offset,
      dst_length});

  encoder_current = encoder_info;
  endPass();
}

void
ArgumentEncodingContext::resolveComputePassBarrier() {
  assert(encoder_current);
  assert(encoder_current->type == EncoderType::Compute);
  auto &barrier_state = encoder_current->barrier_state;
  if (barrier_state.barrierSet & ~intrapass_barrier_control_bits_) {
    auto &cmd = encodeComputeCommand<wmtcmd_compute_memory_barrier>();
    cmd.type = WMTComputeCommandMemoryBarrier;
    cmd.scope = WMTBarrierScopeBuffers | WMTBarrierScopeTextures;
    barrier_state.barrierSet = 0;
  }
}

bool
ArgumentEncodingContext::trackerRequiresReverseRenderPassBoundary(
    GenericAccessTracker &tracker, int flags) {
  assert(encoder_current);
  assert(encoder_current->type == EncoderType::Render);
  tracker.ensureGeneration(access_tracker_generation_);
  return tracker.wouldRequireFragmentToPreRasterBoundary(
      static_cast<RenderEncoderData *>(encoder_current)->encoder_id_vertex,
      flags);
}

bool
ArgumentEncodingContext::requiresReverseRenderPassBoundary(
    Rc<Buffer> const &buffer, int flags) {
  if (!buffer)
    return false;
  auto *allocation = buffer->current();
  if (!allocation ||
      allocation->flags().test(BufferAllocationFlag::GpuReadonly))
    return false;
  const auto suballocation = allocation->currentSuballocation();
  if (suballocation >= allocation->fenceTrackers.size())
    return false;
  return trackerRequiresReverseRenderPassBoundary(
      allocation->fenceTrackers[suballocation], flags);
}

bool
ArgumentEncodingContext::requiresReverseRenderPassBoundary(
    Rc<Texture> const &texture, unsigned level, unsigned slice, int flags) {
  if (!texture)
    return false;
  auto *allocation = texture->current();
  if (!allocation ||
      allocation->flags().test(TextureAllocationFlag::GpuReadonly))
    return false;
  if (allocation->fenceTrackers.empty())
    return false;
  if (allocation->flags().test(TextureAllocationFlag::ShaderReadonly)) {
    return trackerRequiresReverseRenderPassBoundary(
        allocation->fenceTrackers[0], flags);
  }
  const auto mip_count = allocation->descriptor->miplevelCount();
  const uint64_t tracker_index = uint64_t(slice) * mip_count + level;
  if (level >= mip_count ||
      tracker_index >= allocation->fenceTrackers.size()) {
    return trackerRequiresReverseRenderPassBoundary(
        allocation->fenceTrackers[0], flags);
  }
  return trackerRequiresReverseRenderPassBoundary(
      allocation->fenceTrackers[tracker_index], flags);
}

bool
ArgumentEncodingContext::requiresReverseRenderPassBoundary(
    Rc<Texture> const &texture, uint64_t view_id, int flags) {
  if (!texture || !view_id)
    return false;
  auto *allocation = texture->current();
  if (!allocation ||
      allocation->flags().test(TextureAllocationFlag::GpuReadonly))
    return false;
  if (allocation->fenceTrackers.empty())
    return false;
  if (allocation->flags().test(TextureAllocationFlag::ShaderReadonly)) {
    return trackerRequiresReverseRenderPassBoundary(
        allocation->fenceTrackers[0], flags);
  }
  const TextureViewKey key = view_id;
  const auto mip_count = allocation->descriptor->miplevelCount();
  for (unsigned slice = key.array_start; slice < key.array_end; ++slice) {
    for (unsigned level = key.mip_start; level < key.mip_end; ++level) {
      const uint64_t tracker_index = uint64_t(slice) * mip_count + level;
      if (level >= mip_count ||
          tracker_index >= allocation->fenceTrackers.size()) {
        return trackerRequiresReverseRenderPassBoundary(
            allocation->fenceTrackers[0], flags);
      }
      if (trackerRequiresReverseRenderPassBoundary(
              allocation->fenceTrackers[tracker_index], flags))
        return true;
    }
  }
  return false;
}

void
ArgumentEncodingContext::splitRenderPassForReverseStage() {
  assert(encoder_current);
  assert(encoder_current->type == EncoderType::Render);
  auto *former = static_cast<RenderEncoderData *>(encoder_current);
  const EncoderId fragment_producer = former->id;

  // Snapshot pass topology before endPass moves the encoder into the list.
  const uint8_t dsv_planar = former->dsv_planar_flags;
  const uint8_t dsv_readonly = former->dsv_readonly_flags;
  const uint8_t rt_count = former->render_target_count;
  const uint64_t argbuf_size =
      former->argbuf_slice.length
          ? former->argbuf_slice.length
          : 65536ull;
  const auto colors = former->colors;
  const auto depth = former->depth;
  const auto stencil = former->stencil;
  const uint32_t width = former->render_target_width;
  const uint32_t height = former->render_target_height;
  const uint16_t array_length = former->render_target_array_length;
  const uint8_t sample_count = former->default_raster_sample_count;
  const bool use_geometry = former->use_geometry;
  const bool use_tessellation = former->use_tessellation;
  const bool use_visibility = former->use_visibility_result;
  const auto tile_key = former->tile_barrier_pso_key;

  // Clear reverse bit so endPass/track does not re-enter.
  former->barrier_state.barrierPreRasterAfterFragmentSet = 0;

  endPass(); // UpdateFence(Fragment) on former pass

  auto *reopen = startRenderPass(dsv_planar, dsv_readonly, rt_count, argbuf_size);
  reopen->colors = colors;
  reopen->depth = depth;
  reopen->stencil = stencil;
  // Re-open mid-frame: always load previous contents.
  for (auto &color : reopen->colors) {
    if (color.attachment)
      color.load_action = WMTLoadActionLoad;
  }
  if (reopen->depth.attachment)
    reopen->depth.load_action = WMTLoadActionLoad;
  if (reopen->stencil.attachment)
    reopen->stencil.load_action = WMTLoadActionLoad;
  reopen->render_target_width = width;
  reopen->render_target_height = height;
  reopen->render_target_array_length = array_length;
  reopen->default_raster_sample_count = sample_count;
  reopen->use_geometry = use_geometry;
  reopen->use_tessellation = use_tessellation;
  reopen->use_visibility_result = use_visibility;
  reopen->tile_barrier_pso_key = tile_key;
  reopen->prevent_merge_with_previous = true;
  // Preserve the Fragment producer even for a late fallback split. Preflight
  // callers publish the resource access only after reopening and therefore
  // establish the same edge through deptrack; FenceSet deduplicates it.
  reopen->fence_wait_vertex.set(fragment_producer);
}

void
ArgumentEncodingContext::resolveRenderPassBarrier() {
  assert(encoder_current);
  assert(encoder_current->type == EncoderType::Render);
  auto &barrier_state = encoder_current->barrier_state;
  // GPTK SynchroniseStages / EncodeBarrier (memoryBarrier):
  // Metal barrierAfterEncoderStages:after beforeEncoderStages:before —
  // after = producers that must complete, before = consumers that wait.
  //
  // Metal4 debug rejects reverse-stage barriers with afterEncoderStages that
  // include Fragment ("must be Vertex|Object|Mesh"). Classic Metal3 allows
  // reverse memoryBarrier; Metal4 requires flush-first (split pass + fence).
  if (barrier_state.barrierPreRasterAfterFragmentSet & ~intrapass_barrier_control_bits_) {
#if DXMT_DX12_METAL4
    splitRenderPassForReverseStage();
#else
    auto &cmd = encodeRenderCommand<wmtcmd_render_memory_barrier>();
    cmd.type = WMTRenderCommandMemoryBarrier;
    cmd.scope = WMTBarrierScopeBuffers | WMTBarrierScopeTextures;
    cmd.stages_after = WMTRenderStageFragment;   // producers complete
    cmd.stages_before = WMTRenderStagePreRaster; // consumers wait
    barrier_state.barrierPreRasterAfterFragmentSet = 0;
#endif
  }
  // Individual barriers (encoder still open after optional reverse split)
  if (!encoder_current || encoder_current->type != EncoderType::Render)
    return;
  auto &state = encoder_current->barrier_state;
  if (state.barrierSet & ~intrapass_barrier_control_bits_) {
    tile_barrier_cmd.dispatch();
    state.barrierSet = 0;
  }
  if (state.barrierPreRasterSet & ~intrapass_barrier_control_bits_) {
    auto &cmd = encodeRenderCommand<wmtcmd_render_memory_barrier>();
    cmd.type = WMTRenderCommandMemoryBarrier;
    cmd.scope = WMTBarrierScopeBuffers | WMTBarrierScopeTextures;
    cmd.stages_before = WMTRenderStagePreRaster;
    cmd.stages_after = WMTRenderStagePreRaster;
    state.barrierPreRasterSet = 0;
  }
  if (state.barrierFragmentAfterPreRasterSet & ~intrapass_barrier_control_bits_) {
    auto &cmd = encodeRenderCommand<wmtcmd_render_memory_barrier>();
    cmd.type = WMTRenderCommandMemoryBarrier;
    cmd.scope = WMTBarrierScopeBuffers | WMTBarrierScopeTextures;
    cmd.stages_after = WMTRenderStagePreRaster; // producers complete
    cmd.stages_before = WMTRenderStageFragment; // consumers wait
    state.barrierFragmentAfterPreRasterSet = 0;
  }
}

void
ArgumentEncodingContext::$$setEncodingContext(uint64_t seq_id, uint64_t frame_id) {
  current_buffer_chunk_ = 0;
  cpu_buffer_ = cpu_buffer_chunks_[current_buffer_chunk_].ptr;
  cpu_buffer_offset_ = 0;
  seq_id_ = seq_id;
  frame_id_ = frame_id;
  sparse_access_diagnostic_ = {};
}

void
ArgumentEncodingContext::noteSparseTextureAccess(
    TextureAllocation *allocation, unsigned level, unsigned slice,
    int flags) {
  if (!allocation ||
      !allocation->flags().test(TextureAllocationFlag::PlacementSparse))
    return;
  sparse_access_diagnostic_.sparse_access_count++;
  sparse_access_diagnostic_.sparse_access_flags |= uint32_t(flags);
  sparse_access_diagnostic_.sparse_access_level = level;
  sparse_access_diagnostic_.sparse_access_slice = slice;
  sparse_access_diagnostic_.sparse_access_descriptor =
      uint64_t(allocation->descriptor);
  sparse_access_diagnostic_.sparse_access_resource_identity =
      allocation->descriptor->diagnosticIdentity();
  sparse_access_diagnostic_.sparse_access_texture_handle =
      uint64_t(allocation->texture().handle);
  sparse_access_diagnostic_.sparse_access_gpu_resource_id =
      allocation->gpuResourceID;
  sparse_access_diagnostic_.sparse_access_encoder_id = currentEncoderId();
}

constexpr unsigned kEncoderOptimizerThreshold = 64;

static void
FlushRenderEncoderArgumentBuffer(RenderEncoderData *data) {
  data->argbuf_slice.flush_if_needed();
}

void
ArgumentEncodingContext::appendRenderArgumentBufferBindings(
    RenderEncoderData *data, WMT::Buffer buffer, bool use_geometry,
    bool use_tessellation) {
  auto append_setargumentbuffer = [&](WMTRenderStages stages, uint8_t index) {
    auto cmd = reinterpret_cast<wmtcmd_render_setargumentbuffer *>(
        allocate_cpu_heap(sizeof(wmtcmd_render_setargumentbuffer), 16));
    cmd->type = WMTRenderCommandSetArgumentBuffer;
    cmd->next.set(0);
    cmd->buffer = buffer;
    cmd->offset = 0;
    cmd->index = index;
    cmd->stages = stages;
    data->cmd_tail->next.set(cmd);
    data->cmd_tail = reinterpret_cast<wmtcmd_base *>(cmd);
  };

  append_setargumentbuffer(WMTRenderStageVertex, 16);
  append_setargumentbuffer(WMTRenderStageVertex, 29);
  append_setargumentbuffer(WMTRenderStageVertex, 30);
  append_setargumentbuffer(WMTRenderStageFragment, 29);
  append_setargumentbuffer(WMTRenderStageFragment, 30);

  if (use_geometry || use_tessellation) {
    append_setargumentbuffer(WMTRenderStageObject, 16);
    append_setargumentbuffer(WMTRenderStageObject, 21);
    if (use_tessellation) {
      append_setargumentbuffer(WMTRenderStageObject, 27);
      append_setargumentbuffer(WMTRenderStageObject, 28);
    }
    append_setargumentbuffer(WMTRenderStageObject, 29);
    append_setargumentbuffer(WMTRenderStageObject, 30);
    append_setargumentbuffer(WMTRenderStageMesh, 29);
    append_setargumentbuffer(WMTRenderStageMesh, 30);
  }
}

void
ArgumentEncodingContext::appendComputeArgumentBufferBindings(ComputeEncoderData *data, WMT::Buffer buffer) {
  auto append_setargumentbuffer = [&](uint8_t index) {
    auto cmd = reinterpret_cast<wmtcmd_compute_setargumentbuffer *>(
        allocate_cpu_heap(sizeof(wmtcmd_compute_setargumentbuffer), 16));
    cmd->type = WMTComputeCommandSetArgumentBuffer;
    cmd->next.set(nullptr);
    cmd->buffer = buffer;
    cmd->offset = 0;
    cmd->index = index;
    data->cmd_tail->next.set(cmd);
    data->cmd_tail = reinterpret_cast<wmtcmd_base *>(cmd);
  };

  append_setargumentbuffer(29);
  append_setargumentbuffer(30);
}

QueryReadbacks
ArgumentEncodingContext::flushCommands(
    WMT::CommandBuffer cmdbuf, uint64_t seqId, uint64_t event_seq_id,
    CommandBufferDiagnosticInfo *diagnostic_info) {
  assert(!encoder_current);

  struct FlushPerfStats {
    clock::duration collect{};
    clock::duration relation{};
    clock::duration queries{};
    clock::duration timestamp{};
    clock::duration render{};
    clock::duration compute{};
    clock::duration blit{};
    clock::duration present{};
    clock::duration clear{};
    clock::duration resolve{};
    clock::duration scaler{};
    clock::duration event{};
    clock::duration sample{};
    clock::duration pendingFence{};
    clock::duration signal{};
    clock::duration cleanup{};
    uint32_t inputEncoders = 0;
    uint32_t nullEncoders = 0;
    uint32_t encodedRender = 0;
    uint32_t encodedCompute = 0;
    uint32_t encodedBlit = 0;
    uint32_t encodedPresent = 0;
    uint32_t encodedClear = 0;
    uint32_t encodedResolve = 0;
    uint32_t encodedScaler = 0;
    uint32_t encodedSignalEvent = 0;
    uint32_t encodedWaitEvent = 0;
    uint32_t encodedTimestamp = 0;
    uint32_t skippedRender = 0;
    uint32_t skippedClear = 0;
    uint32_t skippedResolve = 0;
    uint32_t pendingFenceWaits = 0;
    uint32_t pendingFenceUpdates = 0;
    uint32_t fenceWaits = 0;
    uint32_t fenceUpdates = 0;
  } perf;

  const bool log_flush_perf = DebugShouldLogFlushPerf();
  const auto flush_start = clock::now();

  unsigned encoder_count = encoder_count_;
  perf.inputEncoders = encoder_count;
  unsigned encoder_index = 0;
  EncoderData **encoders =
      reinterpret_cast<EncoderData **>(allocate_cpu_heap(sizeof(EncoderData *) * encoder_count, alignof(EncoderData *))
      );
  std::array<DebugLastAttachmentUse, 10> last_attachments = {};
  EncoderType last_non_null_type = EncoderType::Null;

  {
    const auto t0 = clock::now();
    EncoderData *current = encoder_head.next;
    while (current) {
      encoders[encoder_index++] = current;
      current = current->next;
    }
    assert(encoder_index == encoder_count);
    perf.collect += clock::now() - t0;
  }

  {
    const auto t0 = clock::now();
    planEncoderSubmissionOrder(encoders, encoder_count);
    perf.relation += clock::now() - t0;
  }

  if (diagnostic_info &&
      (DebugEnabledEnv("DXMT_DIAG_GPU_HANG_DENSE") ||
       DebugEnabledEnv("DXMT_DIAG_ROOT_CAUSE_DENSE") ||
       DebugEnabledEnv("DXMT_DIAG_ERROR_SNAPSHOT") ||
       DebugEnabledEnv("DXMT_VALIDATION") ||
       DebugEnabledEnv("DXMT_DIAG_VALIDATION"))) {
    constexpr uint64_t kFenceHashOffset = 1469598103934665603ull;
    constexpr uint64_t kFenceHashPrime = 1099511628211ull;
    auto hash_fence = [&](EncoderId id, uint64_t domain, uint64_t &hash,
                          uint32_t &count) {
      hash ^= domain;
      hash *= kFenceHashPrime;
      hash ^= id;
      hash *= kFenceHashPrime;
      count++;
    };
    auto hash_fences = [&](const FenceSet &fences, uint64_t domain,
                           uint64_t &hash, uint32_t &count) {
      fences.forEach([&](EncoderId id) {
        hash_fence(id, domain, hash, count);
      });
    };
    for (unsigned i = 0; i < encoder_count; i++) {
      const auto *encoder = encoders[i];
      if (encoder->type == EncoderType::Null)
        continue;
      const auto diagnostic_index =
          diagnostic_info->encoder_diagnostic_count++;
      if (diagnostic_index >= kCommandBufferEncoderDiagnosticCapacity) {
        diagnostic_info->encoder_diagnostic_count =
            kCommandBufferEncoderDiagnosticCapacity;
        diagnostic_info->encoder_diagnostic_overflow_count++;
        continue;
      }
      auto &entry = diagnostic_info->encoders[diagnostic_index];
      entry.encoder_id = encoder->id;
      entry.encoder_index = i;
      entry.type = static_cast<uint32_t>(encoder->type);
      entry.d3d_sequence_begin =
          encoder->diagnostic_d3d_sequence_begin;
      entry.d3d_sequence_end =
          encoder->diagnostic_d3d_sequence_end;
      entry.wait_hash = kFenceHashOffset;
      entry.update_hash = kFenceHashOffset;
      entry.resource_plan_hash = kFenceHashOffset;
      if (encoder->requires_cross_submit_wait)
        entry.flags |= CommandBufferEncoderRequiresCrossSubmitWait;
      hash_fences(encoder->fence_wait, 1, entry.wait_hash, entry.wait_count);
      hash_fences(encoder->fence_update, 2, entry.update_hash,
                  entry.update_count);
      if (encoder->type == EncoderType::Render) {
        const auto *render = static_cast<const RenderEncoderData *>(encoder);
        entry.vertex_encoder_id = render->encoder_id_vertex;
        entry.pipeline_handle = render->last_pso.handle;
        entry.argument_buffer_handle = render->argbuf_slice.gpu_buffer.handle;
        entry.argument_buffer_offset = render->argbuf_slice.offset;
        entry.argument_buffer_size = render->argbuf_slice.length;
        auto hash_attachment = [&](uint64_t texture, uint64_t view) {
          entry.primary_resource ^= texture + 0x9e3779b97f4a7c15ull +
                                    (entry.primary_resource << 6) +
                                    (entry.primary_resource >> 2);
          entry.primary_resource ^= view + 0x9e3779b97f4a7c15ull +
                                    (entry.primary_resource << 6) +
                                    (entry.primary_resource >> 2);
        };
        for (const auto &color : render->colors) {
          hash_attachment(color.attachment.texture().handle,
                          color.attachment
                              ? static_cast<uint64_t>(color.attachment->key)
                              : color.buffer_view_id);
        }
        hash_attachment(
            render->depth.attachment.texture().handle,
            render->depth.attachment
                ? static_cast<uint64_t>(render->depth.attachment->key)
                : 0);
        hash_attachment(
            render->stencil.attachment.texture().handle,
            render->stencil.attachment
                ? static_cast<uint64_t>(render->stencil.attachment->key)
                : 0);
        entry.resource_plan_count =
            static_cast<uint32_t>(render->resource_plan_accesses.size());
        for (const auto &[key, access] : render->resource_plan_accesses) {
          entry.resource_plan_hash ^=
              EncoderResourcePlanKeyHash{}(key) ^
              (static_cast<uint64_t>(access) * kFenceHashPrime);
        }
        if (!render->fence_wait_vertex.empty() ||
            !render->fence_update_vertex.empty())
          entry.flags |= CommandBufferEncoderHasVertexStageFence;
        entry.wait_hash = kFenceHashOffset;
        entry.wait_count = 0;
        render->fence_wait.forEach(
            render->fence_wait_vertex,
            [&](EncoderId id) {
              hash_fence(id, 3, entry.wait_hash, entry.wait_count);
            },
            [&](EncoderId id) {
              hash_fence(id, 4, entry.wait_hash, entry.wait_count);
            });
        entry.update_hash = kFenceHashOffset;
        entry.update_count = 0;
        render->fence_update.forEach(
            render->fence_update_vertex,
            [&](EncoderId id) {
              hash_fence(id, 5, entry.update_hash, entry.update_count);
            },
            [&](EncoderId id) {
              hash_fence(id, 6, entry.update_hash, entry.update_count);
            });
      } else if (encoder->type == EncoderType::Compute) {
        const auto *compute =
            static_cast<const ComputeEncoderData *>(encoder);
        entry.argument_buffer_handle = compute->argbuf_slice.gpu_buffer.handle;
        entry.argument_buffer_offset = compute->argbuf_slice.offset;
        entry.argument_buffer_size = compute->argbuf_slice.length;
        entry.resource_plan_count =
            static_cast<uint32_t>(compute->resource_plan_accesses.size());
        for (const auto &[key, access] : compute->resource_plan_accesses) {
          entry.resource_plan_hash ^=
              EncoderResourcePlanKeyHash{}(key) ^
              (static_cast<uint64_t>(access) * kFenceHashPrime);
        }
      } else if (encoder->type == EncoderType::Clear) {
        const auto *clear = static_cast<const ClearEncoderData *>(encoder);
        entry.primary_resource = clear->attachment.texture().handle;
        entry.secondary_resource = clear->buffer_texture.handle;
      } else if (encoder->type == EncoderType::Resolve) {
        const auto *resolve =
            static_cast<const ResolveEncoderData *>(encoder);
        entry.pipeline_handle = resolve->pso.handle;
        entry.primary_resource = resolve->src.texture().handle;
        entry.secondary_resource = resolve->dst.texture().handle;
      } else if (encoder->type == EncoderType::Present) {
        entry.primary_resource =
            static_cast<const PresentData *>(encoder)->backbuffer.handle;
      } else if (encoder->type == EncoderType::SpatialUpscale) {
        const auto *scaler =
            static_cast<const SpatialUpscaleData *>(encoder);
        entry.primary_resource = scaler->backbuffer.handle;
        entry.secondary_resource = scaler->upscaled.handle;
      }
    }
  }

  {
    auto &stats = currentFrameStatistics();
    for (unsigned i = 0; i < encoder_count; i++) {
      auto *encoder = encoders[i];
      if (encoder->type == EncoderType::Null)
        continue;

      auto count_fences = [&](const FenceSet &waits,
                              const FenceSet &updates) {
        perf.fenceWaits += waits.count();
        perf.fenceUpdates += updates.count();
      };
      switch (encoder->type) {
      case EncoderType::Render: {
        auto *data = static_cast<RenderEncoderData *>(encoder);
        data->fence_wait.forEach(
            data->fence_wait_vertex,
            [&](auto) { perf.fenceWaits++; },
            [&](auto) { perf.fenceWaits++; });
        data->fence_update.forEach(
            data->fence_update_vertex,
            [&](auto) { perf.fenceUpdates++; },
            [&](auto) { perf.fenceUpdates++; });
        break;
      }
      case EncoderType::Compute: {
        auto *data = static_cast<ComputeEncoderData *>(encoder);
        count_fences(data->fence_wait, data->fence_update);
        break;
      }
      case EncoderType::Blit: {
        auto *data = static_cast<BlitEncoderData *>(encoder);
        count_fences(data->fence_wait, data->fence_update);
        break;
      }
      default:
        count_fences(encoder->fence_wait, encoder->fence_update);
        break;
      }

      if (last_non_null_type != EncoderType::Null &&
          last_non_null_type != encoder->type) {
        if (last_non_null_type == EncoderType::Render &&
            encoder->type == EncoderType::Compute)
          stats.encoder_switch_render_compute_count++;
        else if (last_non_null_type == EncoderType::Compute &&
                 encoder->type == EncoderType::Render)
          stats.encoder_switch_compute_render_count++;
        else
          stats.encoder_switch_to_other_count++;
      }

      last_non_null_type = encoder->type;
    }
  }

  QueryReadbacks readbacks{};

  {
    const auto t0 = clock::now();
  uint64_t visibility_count = vro_state_.reset();
  if (visibility_count) {
    readbacks.visibility = std::make_unique<VisibilityResultReadback>(
        device_, seqId, visibility_count, pending_queries_
    );
    auto visibility_buffer = readbacks.visibility->visibility_result_heap;
    auto visibility_residency =
        queue_.RegisterLifetimeResidency(
            ResidencyOwnership::Lifetime(visibility_buffer));
    readbacks.visibility->setResidencyRetirement(
        std::move(visibility_residency));
  }
  std::erase_if(pending_queries_, [&](auto &query) -> bool {
    if (query->queryEndAt() != seqId)
      return false;
    // A query whose end chunk carried no visibility results gets no readback to
    // resolve it (none was created above), yet it must still resolve or GetData
    // spins forever. Hand it to the finish thread instead of stamping here:
    // an encode-time stamp would be clobbered backward by the later
    // finish-thread issue() of an earlier still-in-flight count>0 chunk. When
    // count>0 the readback (which copied these queries) resolves it at finish.
    if (!visibility_count)
      readbacks.visibility_empty_ends.push_back(query);
    return true;
  });
    perf.queries += clock::now() - t0;
  }

  {
    const auto t0 = clock::now();
  readbacks.timestamp = timestamp_state_.flush(cmdbuf);
    perf.timestamp += clock::now() - t0;
  }

  // Isolate MTLFence objects for this command-buffer generation. Banks contain
  // only the peak number of overlapping dependency intervals, not one fence
  // per encoder.
  fence_pool_bank_index_ =
      static_cast<uint32_t>(event_seq_id % kFencePoolBanks);

  const auto fence_preparation =
      prepareFencePool(encoders, encoder_count, diagnostic_info);
  // ---------------------------------------------------------------------------
  // Cross-submit — GPTK DoExecute only (no residual EncoderId waits)
  //
  // prepareFencePool already dropped non-local wait EncoderIds. Cross-submit
  // order is solely:
  //   EncodeWaitMTL4(prev) → commit → signal(prev+1)
  // EncodeWaitMTL4: if (value <= completed) skip.
  // ---------------------------------------------------------------------------
  bool encoded_cross_submit_wait = false;
  bool skipped_wait_as_complete = false;
  if (event_seq_id > 1) {
    const uint64_t wait_value = event_seq_id - 1;
    const uint64_t completed = queue_.SignaledEventSeqId();
    if (wait_value > completed) {
      cmdbuf.encodeWaitForEvent(queue_.event, wait_value);
      encoded_cross_submit_wait = true;
    } else {
      skipped_wait_as_complete = true;
    }
  }
  if (diagnostic_info) {
    // Residual external deptrack edges are never encoded as MTLFence waits;
    // report them only when the timeline wait was skipped-as-complete (or no
    // previous value). When a Metal timeline wait ran, residual is covered.
    diagnostic_info->skipped_external_fence_wait_count =
        encoded_cross_submit_wait
            ? 0
            : fence_preparation.external_wait_count;
    (void)skipped_wait_as_complete;
    (void)fence_preparation.external_blit_wait_count;
    (void)fence_preparation.external_explicit_barrier_wait_count;
  }

  // Fence ops that remain after prepareFencePool (local waits + always-signal
  // updates) are what reach Metal.
  perf.fenceWaits = 0;
  perf.fenceUpdates = 0;
  for (unsigned i = 0; i < encoder_count; i++) {
    auto *encoder = encoders[i];
    if (encoder->type == EncoderType::Null)
      continue;
    if (encoder->type == EncoderType::Render) {
      auto *render = static_cast<RenderEncoderData *>(encoder);
      render->fence_wait.forEach(
          render->fence_wait_vertex,
          [&](auto) { perf.fenceWaits++; },
          [&](auto) { perf.fenceWaits++; });
      render->fence_update.forEach(
          render->fence_update_vertex,
          [&](auto) { perf.fenceUpdates++; },
          [&](auto) { perf.fenceUpdates++; });
    } else {
      perf.fenceWaits += encoder->fence_wait.count();
      perf.fenceUpdates += encoder->fence_update.count();
    }
  }

  while (encoder_index) {
    auto current = encoders[encoder_count - encoder_index];
    const auto encoder_type = current->type;
    const auto encoder_start = clock::now();
    switch (encoder_type) {
    case EncoderType::Render: {
      auto data = static_cast<RenderEncoderData *>(current);
      DebugAccumulateRenderPassTileStats(currentFrameStatistics(), data, last_attachments);
      WMTRenderPassInfo render_pass_info;
      WMT::InitializeRenderPassInfo(render_pass_info);
      bool valid_render_pass = true;
      {
        for (unsigned i = 0; i < std::size(render_pass_info.colors); i++) {
          auto &color_data = data->colors[i];
          if (!color_data.attachment && !color_data.buffer_texture)
            continue;
          WMT::Texture color_texture;
          bool valid_color_attachment = color_data.attachment
              ? ResolveRenderPassColorAttachment(
                    "RenderPass guard: color attachment missing WMTTextureUsageRenderTarget", i, color_data.attachment,
                    color_texture
                )
              : ResolveRenderPassBufferColorAttachment(
                    "RenderPass guard: buffer color attachment missing WMTTextureUsageRenderTarget", i,
                    color_data.buffer_texture, color_texture
                );
          if (!valid_color_attachment) {
            valid_render_pass = false;
            continue;
          }
          auto &color_info = render_pass_info.colors[i];
          color_info.texture = color_texture;
          color_info.load_action = color_data.load_action;
          color_info.store_action = color_data.store_action;
          color_info.level = color_data.level;
          color_info.slice = color_data.slice;
          color_info.depth_plane = color_data.depth_plane;
          color_info.clear_color = color_data.clear_color;
          color_info.resolve_texture = color_data.resolve_attachment.texture();
          color_info.resolve_level = color_data.resolve_level;
          color_info.resolve_slice = color_data.resolve_slice;
          color_info.resolve_depth_plane = color_data.resolve_depth_plane;
        }
        if (data->depth.attachment) {
          auto &depth_info = render_pass_info.depth;
          auto &depth_data = data->depth;
          depth_info.texture = depth_data.attachment.texture();
          depth_info.load_action = depth_data.load_action;
          depth_info.store_action = depth_data.store_action;
          depth_info.level = depth_data.level;
          depth_info.slice = depth_data.slice;
          depth_info.depth_plane = depth_data.depth_plane;
          depth_info.clear_depth = depth_data.clear_depth;
        }
        if (data->stencil.attachment) {
          auto &stencil_info = render_pass_info.stencil;
          auto &stencil_data = data->stencil;
          stencil_info.texture = stencil_data.attachment.texture();
          stencil_info.load_action = stencil_data.load_action;
          stencil_info.store_action = stencil_data.store_action;
          stencil_info.level = stencil_data.level;
          stencil_info.slice = stencil_data.slice;
          stencil_info.depth_plane = stencil_data.depth_plane;
          stencil_info.clear_stencil = stencil_data.clear_stencil;
        }
        render_pass_info.default_raster_sample_count = data->default_raster_sample_count;
        render_pass_info.render_target_array_length = data->render_target_array_length;
        render_pass_info.render_target_width = data->render_target_width;
        render_pass_info.render_target_height = data->render_target_height;
      }
      if (data->use_visibility_result) {
        assert(readbacks.visibility);
        render_pass_info.visibility_buffer = readbacks.visibility->visibility_result_heap;
      }
      if (!valid_render_pass) {
        WARN("RenderPass guard: skipped unsafe render pass encoder=", data->id);
        data->~RenderEncoderData();
        perf.skippedRender++;
        break;
      }
      NormalizeRenderPassInfo(render_pass_info);
      FlushRenderEncoderArgumentBuffer(data);
      auto gpu_buffer_ = data->argbuf_slice.gpu_buffer;
      const bool sample_render_readback = DebugShouldSampleRenderReadback();
      DebugEncodeRenderAttachmentReadbacks(readbacks, cmdbuf, device_, queue_,
                                           frame_id_,
                                           seqId, data, sample_render_readback,
                                           "render-color-before-pass");
      if (queue_.apitraceEnabled()) {
        dxmt::apitrace::set_current_d3d_sequence(seqId);
      }
      auto encoder = cmdbuf.renderCommandEncoder(render_pass_info);
      data->fence_wait.forEach(
          data->fence_wait_vertex, // if a fence is waited pre-raster, no need to wait again at fragment
          [&](auto id) {
            withFence(id, [&](auto fence) {
              encoder.waitForFence(fence, WMTRenderStageGptkPreRasterFence);
            });
          },
          [&](auto id) { withFence(id, [&](auto fence) { encoder.waitForFence(fence, WMTRenderStageFragment); }); }
      );
      if (data->argbuf_slice.length) {
        encoder.setArgumentBuffer(gpu_buffer_, 0, 16, WMTRenderStageVertex);
        encoder.setArgumentBuffer(gpu_buffer_, 0, 29, WMTRenderStageVertex);
        encoder.setArgumentBuffer(gpu_buffer_, 0, 30, WMTRenderStageVertex);
        encoder.setArgumentBuffer(gpu_buffer_, 0, 29, WMTRenderStageFragment);
        encoder.setArgumentBuffer(gpu_buffer_, 0, 30, WMTRenderStageFragment);
      }
      if ((data->use_geometry || data->use_tessellation) && data->argbuf_slice.length) {
        encoder.setArgumentBuffer(gpu_buffer_, 0, 16, WMTRenderStageObject);
        encoder.setArgumentBuffer(gpu_buffer_, 0, 21, WMTRenderStageObject); // draw arguments
        if (data->use_tessellation) {
          encoder.setArgumentBuffer(gpu_buffer_, 0, 27, WMTRenderStageObject);
          encoder.setArgumentBuffer(gpu_buffer_, 0, 28, WMTRenderStageObject);
        }
        encoder.setArgumentBuffer(gpu_buffer_, 0, 29, WMTRenderStageObject);
        encoder.setArgumentBuffer(gpu_buffer_, 0, 30, WMTRenderStageObject);
        encoder.setArgumentBuffer(gpu_buffer_, 0, 29, WMTRenderStageMesh);
        encoder.setArgumentBuffer(gpu_buffer_, 0, 30, WMTRenderStageMesh);
      }
      // D3DMetal UpdateFence (IDA): render pops TWO distinct MTLFences —
      // update fenceA after Vertex|Mesh (17), fenceB after Fragment (2).
      // DXMT maps fenceA↔encoder_id_vertex, fenceB↔encoder_id.
      auto update_stage_fences = [&](WMT::RenderCommandEncoder enc) {
        data->fence_update_vertex.forEach(
            data->fence_update,
            [&](auto id) {
              withFence(id, [&](auto fence) {
                enc.updateFence(fence, WMTRenderStageFragment);
              });
            },
            [&](auto id) {
              withFence(id, [&](auto fence) {
                enc.updateFence(fence, WMTRenderStageGptkPreRasterFence);
              });
            });
      };
      auto abort_unmapped_marshal_buffer = [&](const char *kind) {
        WARN("RenderPass guard: skipped pass after ", kind,
             " marshal argument-buffer allocation failed encoder=", data->id);
        update_stage_fences(encoder);
        encoder.endEncoding();
        data->~RenderEncoderData();
        perf.skippedRender++;
      };
      if (data->gs_arg_marshal_tasks.size()) {
        auto task_count = data->gs_arg_marshal_tasks.size();
        struct GS_MARSHAL_TASK {
          uint64_t draw_args;
          uint64_t dispatch_args_out;
          uint64_t max_object_threadgroups;
          uint32_t vertex_count_per_warp;
          uint32_t end_of_command;
        };
        size_t task_bytes = 0;
        if (!ArgumentBufferByteSize<GS_MARSHAL_TASK>(task_count,
                                                     task_bytes)) {
          abort_unmapped_marshal_buffer("geometry");
          break;
        }
        auto task_argbuf =
            queue_.AllocateArgumentBuffer(seq_id_, task_bytes);
        auto *tasks_data =
            MappedArgumentBufferSlice<GS_MARSHAL_TASK>(task_argbuf,
                                                       task_count);
        if (!tasks_data) {
          abort_unmapped_marshal_buffer("geometry");
          break;
        }
        for (unsigned i = 0; i<task_count; i++) {
          auto & task = data->gs_arg_marshal_tasks[i];
          tasks_data[i].draw_args = task.draw_arguments_va;
          tasks_data[i].dispatch_args_out = task.dispatch_arguments_va;
          tasks_data[i].max_object_threadgroups = task.max_object_threadgroups;
          tasks_data[i].vertex_count_per_warp = task.vertex_count_per_warp;
          tasks_data[i].end_of_command = 0;
          encoder.useResource(task.draw_arguments, WMTResourceUsageRead, WMTRenderStageVertex);
          encoder.useResource(task.dispatch_arguments_buffer, WMTResourceUsageWrite, WMTRenderStageVertex);
        }
        tasks_data[task_count - 1].end_of_command = 1;
        task_argbuf.flush_if_needed();
        emulated_cmd.MarshalGSDispatchArguments(encoder, task_argbuf.gpu_buffer, task_argbuf.offset);
      }
      if (data->ts_arg_marshal_tasks.size()) {
        auto task_count = data->ts_arg_marshal_tasks.size();
        struct TS_MARSHAL_TASK {
          uint64_t draw_args;
          uint64_t dispatch_args_out;
          uint64_t max_object_threadgroups;
          uint16_t control_point_count;
          uint16_t patch_per_group;
          uint32_t end_of_command;
        };
        size_t task_bytes = 0;
        if (!ArgumentBufferByteSize<TS_MARSHAL_TASK>(task_count,
                                                     task_bytes)) {
          abort_unmapped_marshal_buffer("tessellation");
          break;
        }
        auto task_argbuf =
            queue_.AllocateArgumentBuffer(seq_id_, task_bytes);
        auto *tasks_data =
            MappedArgumentBufferSlice<TS_MARSHAL_TASK>(task_argbuf,
                                                       task_count);
        if (!tasks_data) {
          abort_unmapped_marshal_buffer("tessellation");
          break;
        }
        for (unsigned i = 0; i<task_count; i++) {
          auto & task = data->ts_arg_marshal_tasks[i];
          tasks_data[i].draw_args = task.draw_arguments_va;
          tasks_data[i].dispatch_args_out = task.dispatch_arguments_va;
          tasks_data[i].max_object_threadgroups = task.max_object_threadgroups;
          tasks_data[i].control_point_count = task.control_point_count;
          tasks_data[i].patch_per_group = task.patch_per_group;
          tasks_data[i].end_of_command = 0;
          encoder.useResource(task.draw_arguments, WMTResourceUsageRead, WMTRenderStageVertex);
          encoder.useResource(task.dispatch_arguments_buffer, WMTResourceUsageWrite, WMTRenderStageVertex);
        }
        tasks_data[task_count - 1].end_of_command = 1;
        task_argbuf.flush_if_needed();
        emulated_cmd.MarshalTSDispatchArguments(encoder, task_argbuf.gpu_buffer, task_argbuf.offset);
      }
      if (data->gs_arg_marshal_tasks.size() > 0 || data->ts_arg_marshal_tasks.size() > 0) {
        encoder.memoryBarrier(
            WMTBarrierScopeBuffers, WMTRenderStageVertex,
            WMTRenderStageVertex | WMTRenderStageMesh | WMTRenderStageObject
        );
      }
      auto command_summary = DebugSummarizeRenderCommands(&data->cmd_head);
      DebugAccumulateRenderCommands(currentFrameStatistics(), &data->cmd_head);
      DebugLogRenderPassInfo(frame_id_, seqId, data->id, data, command_summary);
      DebugLogArgumentTableSliceCache(
          "render", "vertex", frame_id_, seqId, data->id,
          data->argument_table_cache_vertex);
      DebugLogArgumentTableSliceCache(
          "render", "fragment", frame_id_, seqId, data->id,
          data->argument_table_cache_fragment);
      DebugLogArgumentTableSliceCache(
          "render", "object", frame_id_, seqId, data->id,
          data->argument_table_cache_object);
      DebugLogArgumentTableSliceCache(
          "render", "mesh", frame_id_, seqId, data->id,
          data->argument_table_cache_mesh);
      encoder.setLabel(DebugEncoderLabel(
          "RenderPass id=" + std::to_string(data->id) +
          " draw=" + std::to_string(command_summary.draws + command_summary.indexed_draws) +
          " mesh=" + std::to_string(command_summary.mesh_draws) +
          " tile=" + std::to_string(command_summary.tile_dispatches)));
      encoder.encodeCommands(&data->cmd_head);
      update_stage_fences(encoder);
      encoder.endEncoding();
      DebugEncodeRenderAttachmentReadbacks(readbacks, cmdbuf, device_, queue_,
                                           frame_id_,
                                           seqId, data, sample_render_readback,
                                           "render-color-after-pass");
      data->~RenderEncoderData();
      perf.encodedRender++;
      break;
    }
    case EncoderType::Compute: {
      auto data = static_cast<ComputeEncoderData *>(current);
      DebugAccumulateComputePass(currentFrameStatistics(), data);
      auto command_summary = DebugSummarizeComputeCommands(&data->cmd_head);
      auto first_pso_diag = LookupComputePipelineDiagInfo(command_summary.first_pso);
      auto last_pso_diag = LookupComputePipelineDiagInfo(command_summary.last_pso);
      DebugLogArgumentTableSliceCache(
          "compute", "compute", frame_id_, seqId, data->id,
          data->argument_table_cache);
      data->argbuf_slice.flush_if_needed();
      if (queue_.apitraceEnabled()) {
        dxmt::apitrace::set_current_d3d_sequence(seqId);
      }
      auto encoder = cmdbuf.computeCommandEncoder(true);
      encoder.setLabel(DebugEncoderLabel(
          "C id=" + std::to_string(data->id) +
          " pso=0x" + DebugHex(command_summary.last_pso) +
          " d=" + std::to_string(command_summary.dispatches) +
          " id=" + std::to_string(command_summary.indirect_dispatches) +
          " max=" + std::to_string(command_summary.max_dispatch_size.width) + "x" +
          std::to_string(command_summary.max_dispatch_size.height) + "x" +
          std::to_string(command_summary.max_dispatch_size.depth) +
          " wf=" + std::to_string(command_summary.wait_fences + data->fence_wait.count()) +
          " uf=" + std::to_string(command_summary.update_fences + data->fence_update.count())));
      if (DebugComputePerfShouldLog(command_summary)) {
        WARN_FILE_ONLY("DXMT compute perf:"
             " frame=", frame_id_,
             " seq=", seqId,
             " encoder=", data->id,
             " commands=", command_summary.command_count,
             " psoBinds=", command_summary.pso_binds,
             " firstPsoId=", first_pso_diag.id,
             " firstPsoKey=", DebugShortShaderKey(first_pso_diag.shader_cache_key),
             " lastPsoId=", last_pso_diag.id,
             " lastPsoKey=", DebugShortShaderKey(last_pso_diag.shader_cache_key),
             " firstPso=0x", std::hex, command_summary.first_pso,
             " lastPso=0x", command_summary.last_pso, std::dec,
             " tg=", command_summary.last_threadgroup_size.width, "x",
             command_summary.last_threadgroup_size.height, "x",
             command_summary.last_threadgroup_size.depth,
             " setBuf=", command_summary.set_buffers,
             " setTex=", command_summary.set_textures,
             " setBytes=", command_summary.set_bytes,
             " setBytesTotal=", command_summary.set_bytes_total,
             " useRes=", command_summary.use_resources,
             " dispatch=", command_summary.dispatches,
             " indirect=", command_summary.indirect_dispatches,
             " firstDispatch=", command_summary.first_dispatch_size.width, "x",
             command_summary.first_dispatch_size.height, "x",
             command_summary.first_dispatch_size.depth,
             " lastDispatch=", command_summary.last_dispatch_size.width, "x",
             command_summary.last_dispatch_size.height, "x",
             command_summary.last_dispatch_size.depth,
             " maxDispatch=", command_summary.max_dispatch_size.width, "x",
             command_summary.max_dispatch_size.height, "x",
             command_summary.max_dispatch_size.depth,
             " dispatchVolume=", command_summary.dispatch_grid_volume,
             " maxDispatchVolume=", command_summary.max_dispatch_grid_volume,
             " firstIndirect=0x", std::hex, command_summary.first_indirect_buffer,
             "+0x", command_summary.first_indirect_offset,
             " lastIndirect=0x", command_summary.last_indirect_buffer,
             "+0x", command_summary.last_indirect_offset, std::dec,
             " barriers=", command_summary.barriers,
             " waitFence=", command_summary.wait_fences + data->fence_wait.count(),
             " updateFence=", command_summary.update_fences + data->fence_update.count());
      }
      data->fence_wait.forEach([&](auto id) { withFence(id, [&](auto fence) { encoder.waitForFence(fence); }); });
      if (data->argbuf_slice.length) {
        struct wmtcmd_compute_setargumentbuffer setcmd;
        setcmd.type = WMTComputeCommandSetArgumentBuffer;
        setcmd.next.set(nullptr);
        setcmd.buffer = data->argbuf_slice.gpu_buffer;
        setcmd.offset = 0;
        setcmd.index = 29;
        encoder.encodeCommands((const wmtcmd_compute_nop *)&setcmd);
        setcmd.index = 30;
        encoder.encodeCommands((const wmtcmd_compute_nop *)&setcmd);
      }
      encoder.encodeCommands(&data->cmd_head);
      data->fence_update.forEach([&](auto id) { withFence(id, [&](auto fence) { encoder.updateFence(fence); }); });
      encoder.endEncoding();
      data->~ComputeEncoderData();
      perf.encodedCompute++;
      break;
    }
    case EncoderType::Blit: {
      auto data = static_cast<BlitEncoderData *>(current);
      if (queue_.apitraceEnabled()) {
        dxmt::apitrace::set_current_d3d_sequence(seqId);
      }
      DebugAccumulateBlitPass(currentFrameStatistics(), data);
      auto command_summary = DebugSummarizeBlitCommands(&data->cmd_head);
      DebugLogBlitCommandsForHang(currentFrameId(), seqId, data->id,
                                  command_summary, &data->cmd_head);
      auto encoder = cmdbuf.blitCommandEncoder();
      encoder.setLabel(DebugEncoderLabel(
          "BlitPass id=" + std::to_string(data->id) +
          " cmd=" + std::to_string(command_summary.command_count) +
          " b2t=" + std::to_string(command_summary.copy_buffer_to_texture) +
          " t2t=" + std::to_string(command_summary.copy_texture_to_texture) +
          " t2b=" + std::to_string(command_summary.copy_texture_to_buffer) +
          " fill=" + std::to_string(command_summary.fills) +
          " mip=" + std::to_string(command_summary.generate_mipmaps) +
          " wf=" + std::to_string(command_summary.wait_fences + data->fence_wait.count()) +
          " uf=" + std::to_string(command_summary.update_fences + data->fence_update.count())));
      data->fence_wait.forEach([&](auto id) { withFence(id, [&](auto fence) { encoder.waitForFence(fence); }); });
      encoder.encodeCommands(&data->cmd_head);
      data->fence_update.forEach([&](auto id) { withFence(id, [&](auto fence) { encoder.updateFence(fence); }); });
      encoder.endEncoding();
      data->~BlitEncoderData();
      perf.encodedBlit++;
      break;
    }
    case EncoderType::Present: {
      auto data = static_cast<PresentData *>(current);
      auto t0 = clock::now();
      currentFrameStatistics().present_pass_count++;
      uint32_t present_index = 0;
      const bool sample_present_readback = DebugShouldSamplePresentReadback(present_index);
      if (sample_present_readback) {
        DebugEncodePresentReadbacks(
            readbacks, cmdbuf, device_, queue_, data->backbuffer,
            "backbuffer-before-present",
            currentFrameId(), seqId, present_index);
      }
      if (queue_.apitraceEnabled()) {
        dxmt::apitrace::set_current_d3d_sequence(seqId);
      }
      auto drawable = data->presenter->encodeCommands(
          cmdbuf, data->backbuffer, data->metadata,
          [&](WMT::RenderCommandEncoder encoder) {
            data->fence_wait.forEach([&](auto id) { withFence(id, [&](auto fence) { encoder.waitForFence(fence, WMTRenderStageFragment); }); });
          },
          [&](WMT::RenderCommandEncoder encoder) {
            data->fence_update.forEach([&](auto id) { withFence(id, [&](auto fence) { encoder.updateFence(fence, WMTRenderStageFragment); }); });
          }
      );
      auto t1 = clock::now();
      auto present_encode_interval = t1 - t0;
      currentFrameStatistics().drawable_blocking_interval += present_encode_interval;
      if (sample_present_readback) {
        DebugEncodePresentReadbacks(
            readbacks, cmdbuf, device_, queue_, drawable.texture(),
            "drawable-after-present",
            currentFrameId(), seqId, present_index);
      }
      if (DebugEnabledEnv("DXMT_DIAG_SWAPCHAIN") || DebugMillis(present_encode_interval) > 250.0) {
        static std::atomic<uint64_t> present_diag_count = 0;
        auto index = present_diag_count.fetch_add(1, std::memory_order_relaxed);
        if (index < 64 || (index % 120) == 0 || DebugMillis(present_encode_interval) > 250.0) {
          INFO("DXMT: Present encode frame=", currentFrameId(), " backbuffer=", data->backbuffer.width(), "x",
               data->backbuffer.height(), " elapsedMs=", DebugMillis(present_encode_interval),
               " presentAfter=", data->after);
        }
      }
      if (data->after > 0)
        cmdbuf.presentDrawableAfterMinimumDuration(drawable, data->after);
      else
        cmdbuf.presentDrawable(drawable);
      if (queue_.apitraceEnabled()) {
        const auto present_frame_index =
            data->apitrace_frame_index != ~0ull ? data->apitrace_frame_index : currentFrameId();
        dxmt::apitrace::on_present_drawable(cmdbuf.handle, drawable.handle, present_frame_index, 0, 0);
      }
      data->~PresentData();
      perf.encodedPresent++;
      break;
    }
    case EncoderType::Clear: {
      auto data = static_cast<ClearEncoderData *>(current);
      DebugLogClearPassInfo(frame_id_, seqId, data->id, data);
      {
        WMTRenderPassInfo info;
        WMT::InitializeRenderPassInfo(info);
        if (data->clear_dsv) {
          if (data->clear_dsv & 1) {
            info.depth.clear_depth = data->depth_stencil.first;
            info.depth.texture = data->attachment.texture();
            info.depth.level = data->level;
            info.depth.slice = data->slice;
            info.depth.depth_plane = data->depth_plane;
            info.depth.load_action = WMTLoadActionClear;
            info.depth.store_action = WMTStoreActionStore;
          }
          if (data->clear_dsv & 2) {
            info.stencil.clear_stencil = data->depth_stencil.second;
            info.stencil.texture = data->attachment.texture();
            info.stencil.level = data->level;
            info.stencil.slice = data->slice;
            info.stencil.depth_plane = data->stencil_depth_plane;
            info.stencil.load_action = WMTLoadActionClear;
            info.stencil.store_action = WMTStoreActionStore;
          }
        } else {
          WMT::Texture color_texture;
          bool valid_color_attachment = data->attachment
              ? ResolveRenderPassColorAttachment(
                    "ClearPass guard: color attachment missing WMTTextureUsageRenderTarget", 0, data->attachment,
                    color_texture
                )
              : ResolveRenderPassBufferColorAttachment(
                    "ClearPass guard: buffer color attachment missing WMTTextureUsageRenderTarget", 0,
                    data->buffer_texture, color_texture
                );
          if (!valid_color_attachment) {
            WARN("ClearPass guard: skipped unsafe clear pass encoder=", data->id);
            data->~ClearEncoderData();
            perf.skippedClear++;
            break;
          }
          info.colors[0].clear_color = data->color;
          info.colors[0].texture = color_texture;
          info.colors[0].level = data->level;
          info.colors[0].slice = data->slice;
          info.colors[0].depth_plane = data->depth_plane;
          info.colors[0].load_action = WMTLoadActionClear;
          info.colors[0].store_action = WMTStoreActionStore;
        }
        info.render_target_width = data->width;
        info.render_target_height = data->height;
        info.render_target_array_length = data->array_length;
        info.default_raster_sample_count = data->sample_count;
        NormalizeRenderPassInfo(info);
        if (queue_.apitraceEnabled()) {
          dxmt::apitrace::set_current_d3d_sequence(seqId);
        }
        auto encoder = cmdbuf.renderCommandEncoder(info);
        encoder.setLabel(WMT::String::string("ClearPass", WMTUTF8StringEncoding));
        data->fence_wait.forEach([&](auto id) { withFence(id, [&](auto fence) { encoder.waitForFence(fence, WMTRenderStageFragment); }); });
        data->fence_update.forEach([&](auto id) { withFence(id, [&](auto fence) { encoder.updateFence(fence, WMTRenderStageFragment); }); });
        encoder.endEncoding();
      }
      data->~ClearEncoderData();
      perf.encodedClear++;
      break;
    }
    case EncoderType::Resolve: {
      auto data = static_cast<ResolveEncoderData *>(current);
      if (data->is_depth) {
        // Point resolve of a multisampled depth surface: a depth-only render pass
        // whose fragment shader reads sample 0 of the source (bound as a fragment
        // depth texture) and writes the destination depth attachment. The source
        // is a multisampled depth-stencil surface, allocated with ShaderRead for
        // exactly this sampled bind (CreateDepthStencilSurface). Only the depth
        // aspect is resolved; a depth-stencil source's stencil is not carried,
        // matching wined3d and DXVK, which resolve depth only.
        WMT::Texture src_texture;
        WMT::Texture dst_texture;
        if (!ResolveRenderPassColorAttachment("DepthResolve guard: source missing", 0, data->src, src_texture) ||
            !ResolveRenderPassColorAttachment("DepthResolve guard: destination missing", 0, data->dst, dst_texture)) {
          WARN("DepthResolve guard: skipped unsafe depth resolve encoder=", data->id);
          data->~ResolveEncoderData();
          perf.skippedResolve++;
          break;
        }
        auto *dst_allocation = data->dst ? data->dst->allocation : nullptr;
        auto *dst_descriptor = dst_allocation ? dst_allocation->descriptor : nullptr;

        WMTRenderPassInfo info;
        WMT::InitializeRenderPassInfo(info);
        info.depth.texture = dst_texture;
        info.depth.load_action = WMTLoadActionDontCare;
        info.depth.store_action = WMTStoreActionStore;
        if (dst_descriptor) {
          info.render_target_width = dst_descriptor->width(data->dst->key);
          info.render_target_height = dst_descriptor->height(data->dst->key);
        }
        info.render_target_array_length = 1;
        info.default_raster_sample_count = 1;
        NormalizeRenderPassInfo(info);

#if !DXMT_DX12_METAL4
        // The depth resolve is a d3d9 (Metal 3) path; the Metal 4 render encoder
        // sets depth-stencil state through the pipeline, not setDepthStencilState,
        // and this branch never runs on the Metal 4 backend, so keep it out of it.
        if (!depth_resolve_dss_) {
          WMTDepthStencilInfo ds_info = {};
          ds_info.depth_compare_function = WMTCompareFunctionAlways;
          ds_info.depth_write_enabled = true;
          depth_resolve_dss_ = device_.newDepthStencilState(ds_info);
        }
#endif

        auto encoder = cmdbuf.renderCommandEncoder(info);
        encoder.setLabel(WMT::String::string("DepthResolvePass", WMTUTF8StringEncoding));
        data->fence_wait.forEach([&](auto id) {
          withFence(id, [&](auto fence) { encoder.waitForFence(fence, WMTRenderStageFragment); });
        });
        struct ResolveMetadata {
          uint32_t src_origin[2];
          uint32_t dst_origin[2];
          uint32_t size[2];
        } metadata = {};
        metadata.src_origin[0] = data->src_rect ? data->src_rect->x : 0;
        metadata.src_origin[1] = data->src_rect ? data->src_rect->y : 0;
        metadata.dst_origin[0] = data->dst_origin.x;
        metadata.dst_origin[1] = data->dst_origin.y;
        metadata.size[0] = data->resolve_size.width ? data->resolve_size.width : info.render_target_width;
        metadata.size[1] = data->resolve_size.height ? data->resolve_size.height : info.render_target_height;
        encoder.setRenderPipelineState(data->pso);
#if !DXMT_DX12_METAL4
        encoder.setDepthStencilState(depth_resolve_dss_);
#endif
        encoder.setFragmentTexture(src_texture, 0);
        encoder.setFragmentBytes(&metadata, sizeof(metadata), 0);
        encoder.setViewport(
            {double(metadata.dst_origin[0]), double(metadata.dst_origin[1]), double(metadata.size[0]),
             double(metadata.size[1]), 0.0, 1.0}
        );
        encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
        data->fence_update.forEach([&](auto id) {
          withFence(id, [&](auto fence) { encoder.updateFence(fence, WMTRenderStageFragment); });
        });
        encoder.endEncoding();
        data->~ResolveEncoderData();
        perf.encodedResolve++;
        break;
      }
      {
        WMT::Texture src_texture;
        WMT::Texture dst_texture;
        if (!ResolveRenderPassColorAttachment(
                "ResolvePass guard: source attachment missing WMTTextureUsageRenderTarget", 0, data->src,
                src_texture
            ) ||
            !ResolveRenderPassColorAttachment(
                "ResolvePass guard: destination attachment missing WMTTextureUsageRenderTarget", 0, data->dst,
                dst_texture
            )) {
          WARN("ResolvePass guard: skipped unsafe resolve pass encoder=", data->id);
          data->~ResolveEncoderData();
          perf.skippedResolve++;
          break;
        }
        auto *src_allocation = data->src ? data->src->allocation : nullptr;
        auto *src_descriptor = src_allocation ? src_allocation->descriptor : nullptr;
        auto *dst_allocation = data->dst ? data->dst->allocation : nullptr;
        auto *dst_descriptor = dst_allocation ? dst_allocation->descriptor : nullptr;

        WMTRenderPassInfo info;
        WMT::InitializeRenderPassInfo(info);
        info.colors[0].texture = data->pso ? dst_texture : src_texture;
        info.colors[0].load_action = WMTLoadActionLoad;
        info.colors[0].store_action =
            data->pso ? WMTStoreActionStore : WMTStoreActionStoreAndMultisampleResolve;
        info.colors[0].resolve_texture = data->pso ? WMT::Texture{} : dst_texture;
        if (dst_descriptor && data->pso) {
          info.render_target_width = dst_descriptor->width(data->dst->key);
          info.render_target_height = dst_descriptor->height(data->dst->key);
        }
        if (src_descriptor) {
          if (!info.render_target_width)
            info.render_target_width = src_descriptor->width(data->src->key);
          if (!info.render_target_height)
            info.render_target_height = src_descriptor->height(data->src->key);
          info.render_target_array_length = 1;
          info.default_raster_sample_count = data->pso ? 1 : src_descriptor->sampleCount();
        }

        NormalizeRenderPassInfo(info);
        if (queue_.apitraceEnabled()) {
          dxmt::apitrace::set_current_d3d_sequence(seqId);
        }
        auto encoder = cmdbuf.renderCommandEncoder(info);
        encoder.setLabel(WMT::String::string("ResolvePass", WMTUTF8StringEncoding));
        data->fence_wait.forEach([&](auto id) { withFence(id, [&](auto fence) { encoder.waitForFence(fence, WMTRenderStageFragment); }); });
        if (data->pso) {
          struct ResolveMetadata {
            uint32_t src_origin[2];
            uint32_t dst_origin[2];
            uint32_t size[2];
          } metadata = {};
          metadata.src_origin[0] = data->src_rect ? data->src_rect->x : 0;
          metadata.src_origin[1] = data->src_rect ? data->src_rect->y : 0;
          metadata.dst_origin[0] = data->dst_origin.x;
          metadata.dst_origin[1] = data->dst_origin.y;
          metadata.size[0] = data->resolve_size.width;
          metadata.size[1] = data->resolve_size.height;
          if (!metadata.size[0])
            metadata.size[0] = info.render_target_width;
          if (!metadata.size[1])
            metadata.size[1] = info.render_target_height;
          encoder.setRenderPipelineState(data->pso);
          encoder.setFragmentTexture(src_texture, 0);
          encoder.setFragmentBytes(&metadata, sizeof(metadata), 0);
          encoder.setViewport({
              double(metadata.dst_origin[0]), double(metadata.dst_origin[1]),
              double(metadata.size[0]), double(metadata.size[1]), 0.0, 1.0});
          encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
        }
        data->fence_update.forEach([&](auto id) { withFence(id, [&](auto fence) { encoder.updateFence(fence, WMTRenderStageFragment); }); });
        encoder.endEncoding();
      }
      data->~ResolveEncoderData();
      perf.encodedResolve++;
      break;
    }
    case EncoderType::StretchBlit: {
      auto data = static_cast<StretchBlitEncoderData *>(current);
      // StretchBlit is d3d9's StretchRect render-pass path (setFragmentSamplerState
      // et al. live on the Metal3 encoder only); the Metal4 backend never enqueues it.
#if !DXMT_DX12_METAL4
      {
        auto *dst_allocation = data->dst ? data->dst->allocation : nullptr;
        auto *dst_descriptor = dst_allocation ? dst_allocation->descriptor : nullptr;

        WMTRenderPassInfo info;
        WMT::InitializeRenderPassInfo(info);
        info.colors[0].texture = data->dst.texture();
        info.colors[0].load_action = WMTLoadActionLoad;
        info.colors[0].store_action = WMTStoreActionStore;
        if (dst_descriptor) {
          info.render_target_width = dst_descriptor->width(data->dst->key);
          info.render_target_height = dst_descriptor->height(data->dst->key);
          info.render_target_array_length = 1;
          // D3-O2: a single-sample -> multisample StretchRect broadcasts the
          // sampled source into every sample, so the pass raster count follows
          // the destination (1 for the common single-sample dst, unchanged).
          info.default_raster_sample_count = dst_descriptor->sampleCount();
        } else {
          info.default_raster_sample_count = 1;
        }

        NormalizeRenderPassInfo(info);
        auto encoder = cmdbuf.renderCommandEncoder(info);
        encoder.setLabel(WMT::String::string("StretchBlitPass", WMTUTF8StringEncoding));
        data->fence_wait.forEach([&](auto id) { withFence(id, [&](auto fence) { encoder.waitForFence(fence, WMTRenderStageFragment); }); });
        if (data->pso && data->sampler) {
          struct {
            float src_uv_origin[2];
            float src_uv_size[2];
          } metadata{};
          metadata.src_uv_origin[0] = data->src_uv_origin[0];
          metadata.src_uv_origin[1] = data->src_uv_origin[1];
          metadata.src_uv_size[0] = data->src_uv_size[0];
          metadata.src_uv_size[1] = data->src_uv_size[1];
          encoder.setRenderPipelineState(data->pso);
          encoder.setFragmentTexture(data->src.texture(), 0);
          encoder.setFragmentSamplerState(data->sampler, 0);
          encoder.setFragmentBytes(&metadata, sizeof(metadata), 0);
          encoder.setViewport({
              double(data->dst_origin.x), double(data->dst_origin.y),
              double(data->dst_size.width), double(data->dst_size.height), 0.0, 1.0});
          encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
        }
        data->fence_update.forEach([&](auto id) { withFence(id, [&](auto fence) { encoder.updateFence(fence, WMTRenderStageFragment); }); });
        encoder.endEncoding();
      }
#endif
      data->~StretchBlitEncoderData();
      break;
    }
    case EncoderType::SpatialUpscale: {
      auto data = static_cast<SpatialUpscaleData *>(current);

      auto begin_scaler = cmdbuf.blitCommandEncoder();
      begin_scaler.setLabel(WMT::String::string("BeginScaler", WMTUTF8StringEncoding));
      data->fence_wait.forEach([&](auto id) { withFence(id, [&](auto fence) { begin_scaler.waitForFence(fence); }); });
      begin_scaler.updateFence(data->scaler->fence());
      begin_scaler.endEncoding();

      cmdbuf.encodeSpatialScale(data->scaler->scaler(), data->backbuffer, data->upscaled, data->scaler->fence());

      auto end_scaler = cmdbuf.blitCommandEncoder();
      end_scaler.waitForFence(data->scaler->fence());
      end_scaler.setLabel(WMT::String::string("EndScaler", WMTUTF8StringEncoding));
      data->fence_update.forEach([&](auto id) { withFence(id, [&](auto fence) { end_scaler.updateFence(fence); }); });
      end_scaler.endEncoding();

      data->~SpatialUpscaleData();
      perf.encodedScaler++;
      break;
    }
    case EncoderType::SignalEvent: {
      auto data = static_cast<SignalEventData *>(current);
      cmdbuf.encodeSignalEvent(data->event, data->value);
      data->~SignalEventData();
      perf.encodedSignalEvent++;
      break;
    }
    case EncoderType::WaitForEvent: {
      auto data = static_cast<WaitForEventData *>(current);
      cmdbuf.encodeWaitForEvent(data->event, data->value);
      data->~WaitForEventData();
      perf.encodedWaitEvent++;
      break;
    }
    case EncoderType::TemporalUpscale: {
      auto data = static_cast<TemporalUpscaleData *>(current);

      auto begin_scaler = cmdbuf.blitCommandEncoder();
      begin_scaler.setLabel(WMT::String::string("BeginScaler", WMTUTF8StringEncoding));
      data->fence_wait.forEach([&](auto id) { withFence(id, [&](auto fence) { begin_scaler.waitForFence(fence); }); });
      begin_scaler.updateFence(data->scaler->fence());
      begin_scaler.endEncoding();

      cmdbuf.encodeTemporalScale(
          data->scaler->scaler(), data->input, data->output, data->depth, data->motion_vector, data->exposure,
          data->scaler->fence(), data->props
      );

      auto end_scaler = cmdbuf.blitCommandEncoder();
      end_scaler.waitForFence(data->scaler->fence());
      end_scaler.setLabel(WMT::String::string("EndScaler", WMTUTF8StringEncoding));
      data->fence_update.forEach([&](auto id) { withFence(id, [&](auto fence) { end_scaler.updateFence(fence); }); });
      end_scaler.endEncoding();
      data->~TemporalUpscaleData();
      perf.encodedScaler++;
      break;
    }
    case EncoderType::SampleTimestamp: {
      auto data = static_cast<SampleTimestampData *>(current);
#if DXMT_DX12_METAL4
      if (auto readback = readbacks.timestamp.get()) {
        auto timestamp_context = readback->timestampContext();
        if (DebugHangDenseEnabled()) {
          WARN("DXMT hang-diag sample-timestamp encoder:"
               " frame=", currentFrameId(),
               " seq=", seqId,
               " queryCount=", data->queries.size(),
               " readbackIndex=", data->readback_index,
               " hasContext=", timestamp_context ? 1u : 0u);
        }
        uint32_t sample_i = 0;
        for (const auto &query : data->queries) {
          auto heap = query->resolveHeap();
          const uint64_t sample_index = query->sampleIndex();
          if (DebugHangDenseEnabled()) {
            WARN("DXMT hang-diag sample-timestamp write:"
                 " frame=", currentFrameId(),
                 " seq=", seqId,
                 " i=", sample_i,
                 " heap=", uint64_t(heap),
                 " index=", sample_index,
                 " nullHeap=", heap ? 0u : 1u,
                 " invalidIndex=", (sample_index == ~0ull) ? 1u : 0u);
          }
          if (heap && sample_index != ~0ull)
            timestamp_context.writeTimestamp(cmdbuf, heap, sample_index);
          sample_i++;
        }
      } else if (DebugHangDenseEnabled()) {
        WARN("DXMT hang-diag sample-timestamp encoder:"
             " frame=", currentFrameId(),
             " seq=", seqId,
             " reason=no-timestamp-readback"
             " queryCount=", data->queries.size());
      }
#else
      if (auto readback = readbacks.timestamp.get(); readback->sampleBuffer()) {

        /**
        Since Metal driver may change the execution order of encoders, implement a "barrier" to prevent that
        FIXME: Not an elegant implementation, should get rid of it when fence-based synchronization is done
        */
        barrierOnQueue(cmdbuf);

        WMTSampleBufferAttachmentInfo sample_buffer_info{};
        sample_buffer_info.sample_buffer = readback->sampleBuffer();
        sample_buffer_info.start_of_encoder_sample_index = data->readback_index;
        sample_buffer_info.end_of_encoder_sample_index = ~0ull; /* MTLCounterDontSample */
        auto encoder = cmdbuf.blitCommandEncoderWithSampleBuffers(&sample_buffer_info, 1);
        encoder.setLabel(WMT::String::string("SampleTimestamp", WMTUTF8StringEncoding));
        {
          /**
          `sampleBufferAttachments` does not work when the blit encoder is empty, just do something
          FIXME: potential perf overhead?
          */
          struct wmtcmd_blit_fillbuffer fill;
          fill.next.set(nullptr);
          fill.type = WMTBlitCommandFillBuffer;
          fill.buffer = dummy_cbuffer_;
          fill.offset = 0;
          fill.length = 4;
          fill.value = 0;
          MTLBlitCommandEncoder_encodeCommands(encoder, (const struct wmtcmd_base *)&fill);
        }
        encoder.endEncoding();

      } else {
        // Use timestamp from command buffer's `gpuEndTime`
      }
#endif
      data->~SampleTimestampData();
      perf.encodedTimestamp++;
      break;
    }
    case EncoderType::ResolveTimestamp: {
      auto data = static_cast<ResolveTimestampData *>(current);
#if DXMT_DX12_METAL4
      if (DebugHangDenseEnabled()) {
        WARN("DXMT hang-diag resolve-timestamp encoder:"
             " frame=", currentFrameId(),
             " seq=", seqId,
             " rangeCount=", data->ranges.size());
      }
      uint32_t range_i = 0;
      for (const auto &range : data->ranges) {
        if (DebugHangDenseEnabled()) {
          WARN("DXMT hang-diag resolve-timestamp range:"
               " frame=", currentFrameId(),
               " seq=", seqId,
               " i=", range_i,
               " srcHeap=", uint64_t(range.src_heap),
               " start=", range.start_index,
               " count=", range.query_count,
               " end=", range.start_index + range.query_count,
               " dstBuf=", uint64_t(range.dst_buffer),
               " dstOff=", range.dst_offset,
               " dstLen=", range.dst_length,
               " nullHeap=", range.src_heap ? 0u : 1u,
               " nullDst=", range.dst_buffer ? 0u : 1u);
        }
        if (range.src_heap) {
          cmdbuf.resolveCounterHeap(
              WMT::CounterHeap(range.src_heap), range.start_index,
              range.query_count, range.dst_buffer, range.dst_offset,
              range.dst_length);
        }
        range_i++;
      }
#else
      if (auto readback = readbacks.timestamp.get()) {
        if (readback->sampleBuffer()) {
          auto encoder = cmdbuf.blitCommandEncoder();
          encoder.setLabel(WMT::String::string("ResolveTimestamp", WMTUTF8StringEncoding));
          for (const auto &range : data->ranges) {
            uint64_t remaining = range.query_count;
            uint64_t start = range.start_index;
            uint64_t dst_offset = range.dst_offset;
            while (remaining) {
              const uint64_t chunk = std::min<uint64_t>(
                  remaining, std::numeric_limits<uint32_t>::max());
              encoder.resolveCounters(readback->sampleBuffer(),
                                      static_cast<uint32_t>(start),
                                      static_cast<uint32_t>(chunk),
                                      range.dst_buffer, dst_offset);
              remaining -= chunk;
              start += chunk;
              dst_offset += chunk * sizeof(uint64_t);
            }
          }
          encoder.endEncoding();
        }
      }
#endif
      data->~ResolveTimestampData();
      perf.encodedTimestamp++;
      break;
    }
    case EncoderType::Null: {
      perf.nullEncoders++;
      break;
    }
    default:
      break;
    }
    const auto encoder_elapsed = clock::now() - encoder_start;
    switch (encoder_type) {
    case EncoderType::Render:
      perf.render += encoder_elapsed;
      break;
    case EncoderType::Compute:
      perf.compute += encoder_elapsed;
      break;
    case EncoderType::Blit:
      perf.blit += encoder_elapsed;
      break;
    case EncoderType::Present:
      perf.present += encoder_elapsed;
      break;
    case EncoderType::Clear:
      perf.clear += encoder_elapsed;
      break;
    case EncoderType::Resolve:
      perf.resolve += encoder_elapsed;
      break;
    case EncoderType::SpatialUpscale:
    case EncoderType::TemporalUpscale:
      perf.scaler += encoder_elapsed;
      break;
    case EncoderType::SignalEvent:
    case EncoderType::WaitForEvent:
      perf.event += encoder_elapsed;
      break;
    case EncoderType::SampleTimestamp:
    case EncoderType::ResolveTimestamp:
      perf.sample += encoder_elapsed;
      break;
    default:
      break;
    }
    encoder_index--;
  }

  if (FenceOnlyBlitOptimizationEnabled() && has_pending_fence_only_blit_) {
    const auto t0 = clock::now();
    perf.pendingFenceWaits = pending_fence_only_blit_wait_.count();
    perf.pendingFenceUpdates = pending_fence_only_blit_update_.count();
    perf.fenceWaits += perf.pendingFenceWaits;
    perf.fenceUpdates += perf.pendingFenceUpdates;
    auto encoder = cmdbuf.blitCommandEncoder();
    pending_fence_only_blit_wait_.forEach([&](auto id) { withFence(id, [&](auto fence) { encoder.waitForFence(fence); }); });
    pending_fence_only_blit_update_.forEach([&](auto id) { withFence(id, [&](auto fence) { encoder.updateFence(fence); }); });
    encoder.endEncoding();
    pending_fence_only_blit_wait_ = {};
    pending_fence_only_blit_update_ = {};
    has_pending_fence_only_blit_ = false;
    perf.pendingFence += clock::now() - t0;
  }

  {
    const auto t0 = clock::now();
    encoder_head.next = nullptr;
    encoder_last = &encoder_head;
    encoder_count_ = 0;
    // GPTK CB boundary: next recording session starts a new ResolveBarriers
    // epoch. Access trackers lazily clear exclusive_/shared_ on first touch;
    // fence locality summaries are CB-local only.
    access_tracker_generation_++;
    if (access_tracker_generation_ == 0)
      access_tracker_generation_ = 1;
    fence_locality_.reset();
    fence_bindings_.reset(0);
    // The fence bindings encoded above do not survive this command buffer, so
    // every id issued so far is now unresolvable and the shared sets holding
    // them can drop those entries on next touch.
    chunk_base_encoder_id_ = encoder_id_;
    perf.cleanup += clock::now() - t0;
  }

  {
    const auto t0 = clock::now();
    cmdbuf.encodeSignalEvent(queue_.event, event_seq_id);
    perf.signal += clock::now() - t0;
  }

  {
    const auto t0 = clock::now();
    for (size_t i = cpu_buffer_chunks_.size() - 1; i > current_buffer_chunk_; i--) {
      if (++cpu_buffer_chunks_[i].underused_times > kEncodingContextCPUHeapLifetime) {
        cpu_buffer_chunks_.pop_back();
      }
    }
    perf.cleanup += clock::now() - t0;
  }

  const auto total = clock::now() - flush_start;
  const uint32_t encoded_encoders =
      perf.encodedRender + perf.encodedCompute + perf.encodedBlit +
      perf.encodedPresent + perf.encodedClear + perf.encodedResolve +
      perf.encodedScaler + perf.encodedSignalEvent + perf.encodedWaitEvent +
      perf.encodedTimestamp;
  if (diagnostic_info) {
    diagnostic_info->input_encoder_count = perf.inputEncoders;
    diagnostic_info->encoded_encoder_count = encoded_encoders;
    diagnostic_info->render_encoder_count = perf.encodedRender;
    diagnostic_info->compute_encoder_count = perf.encodedCompute;
    diagnostic_info->blit_encoder_count = perf.encodedBlit;
    diagnostic_info->present_encoder_count = perf.encodedPresent;
    diagnostic_info->clear_encoder_count = perf.encodedClear;
    diagnostic_info->resolve_encoder_count = perf.encodedResolve;
    diagnostic_info->scaler_encoder_count = perf.encodedScaler;
    diagnostic_info->signal_event_count = perf.encodedSignalEvent;
    diagnostic_info->wait_event_count = perf.encodedWaitEvent;
    diagnostic_info->timestamp_encoder_count = perf.encodedTimestamp;
    diagnostic_info->other_encoder_count =
        encoded_encoders - perf.encodedRender - perf.encodedCompute -
        perf.encodedBlit;
    diagnostic_info->fence_wait_count = perf.fenceWaits;
    diagnostic_info->fence_update_count = perf.fenceUpdates;
    diagnostic_info->encoded_fence_wait_count = perf.fenceWaits;
    diagnostic_info->sparse_access_count =
        sparse_access_diagnostic_.sparse_access_count;
    diagnostic_info->sparse_access_flags =
        sparse_access_diagnostic_.sparse_access_flags;
    diagnostic_info->sparse_access_level =
        sparse_access_diagnostic_.sparse_access_level;
    diagnostic_info->sparse_access_slice =
        sparse_access_diagnostic_.sparse_access_slice;
    diagnostic_info->sparse_access_descriptor =
        sparse_access_diagnostic_.sparse_access_descriptor;
    diagnostic_info->sparse_access_resource_identity =
        sparse_access_diagnostic_.sparse_access_resource_identity;
    diagnostic_info->sparse_access_texture_handle =
        sparse_access_diagnostic_.sparse_access_texture_handle;
    diagnostic_info->sparse_access_gpu_resource_id =
        sparse_access_diagnostic_.sparse_access_gpu_resource_id;
    diagnostic_info->sparse_access_encoder_id =
        sparse_access_diagnostic_.sparse_access_encoder_id;
  }
  {
    auto &stats = currentFrameStatistics();
    const bool event_only_chunk =
        perf.inputEncoders == (perf.encodedSignalEvent + perf.encodedWaitEvent) &&
        (perf.encodedSignalEvent || perf.encodedWaitEvent);
    stats.flush_chunk_count++;
    stats.flush_empty_chunk_count += perf.inputEncoders == 0;
    stats.flush_event_only_chunk_count += event_only_chunk;
    stats.flush_signal_chunk_count += perf.encodedSignalEvent != 0;
    stats.flush_wait_chunk_count += perf.encodedWaitEvent != 0;
    stats.flush_present_chunk_count += perf.encodedPresent != 0;
    stats.flush_encoder_count += perf.inputEncoders;
    stats.flush_encoded_encoder_count += encoded_encoders;
    stats.flush_null_encoder_count += perf.nullEncoders;
    stats.flush_render_encoder_count += perf.encodedRender;
    stats.flush_compute_encoder_count += perf.encodedCompute;
    stats.flush_blit_encoder_count += perf.encodedBlit;
    stats.flush_timestamp_encoder_count += perf.encodedTimestamp;
    stats.flush_total_interval += total;
    stats.flush_collect_interval += perf.collect;
    stats.flush_relation_interval += perf.relation;
    stats.flush_query_interval += perf.queries;
    stats.flush_timestamp_interval += perf.timestamp;
    stats.flush_render_interval += perf.render;
    stats.flush_compute_interval += perf.compute;
    stats.flush_blit_interval += perf.blit;
    stats.flush_clear_interval += perf.clear;
    stats.flush_resolve_interval += perf.resolve;
    stats.flush_scaler_interval += perf.scaler;
    stats.flush_present_interval += perf.present;
    stats.flush_event_interval += perf.event;
    stats.flush_sample_interval += perf.sample;
    stats.flush_pending_fence_interval += perf.pendingFence;
    stats.flush_signal_interval += perf.signal;
    stats.flush_cleanup_interval += perf.cleanup;
    stats.flush_max_chunk_interval = std::max(stats.flush_max_chunk_interval, total);
  }

  if (log_flush_perf && DebugFlushPerfShouldLog()) {
    WARN_FILE_ONLY("DXMT flush perf:"
         " frame=", frame_id_,
         " seq=", seqId,
         " encoders=", perf.inputEncoders,
         " null=", perf.nullEncoders,
         " render=", perf.encodedRender,
         " compute=", perf.encodedCompute,
         " blit=", perf.encodedBlit,
         " clear=", perf.encodedClear,
         " resolve=", perf.encodedResolve,
         " present=", perf.encodedPresent,
         " scaler=", perf.encodedScaler,
         " sig=", perf.encodedSignalEvent,
         " wait=", perf.encodedWaitEvent,
         " timestamp=", perf.encodedTimestamp,
         " skipRender=", perf.skippedRender,
         " skipClear=", perf.skippedClear,
         " skipResolve=", perf.skippedResolve,
         " pendingFenceW=", perf.pendingFenceWaits,
         " pendingFenceU=", perf.pendingFenceUpdates,
         " totalMs=", DebugMillis(total),
         " collectMs=", DebugMillis(perf.collect),
         " relationMs=", DebugMillis(perf.relation),
         " queryMs=", DebugMillis(perf.queries),
         " timestampMs=", DebugMillis(perf.timestamp),
         " renderMs=", DebugMillis(perf.render),
         " computeMs=", DebugMillis(perf.compute),
         " blitMs=", DebugMillis(perf.blit),
         " clearMs=", DebugMillis(perf.clear),
         " resolveMs=", DebugMillis(perf.resolve),
         " presentMs=", DebugMillis(perf.present),
         " scalerMs=", DebugMillis(perf.scaler),
         " eventMs=", DebugMillis(perf.event),
         " sampleMs=", DebugMillis(perf.sample),
         " pendingFenceMs=", DebugMillis(perf.pendingFence),
         " signalMs=", DebugMillis(perf.signal),
         " cleanupMs=", DebugMillis(perf.cleanup));
  }

  return readbacks;
}

static bool
IsFenceOnlyBlitPass(const BlitEncoderData *data);

static void
MergeFenceOnlyBlitPassInto(BlitEncoderData *former, EncoderData *latter);

void
ArgumentEncodingContext::planEncoderSubmissionOrder(
    EncoderData **encoders, unsigned encoder_count) {
  if (encoder_count < 2)
    return;

  // Walk backwards so a later coalesced encoder can absorb every compatible
  // predecessor in original command order. checkEncoderRelation returns SWAP
  // when the two encoders may cross; its merge paths prepend the former
  // command stream into the latter and replace the former with a Null
  // tombstone. SYNCHRONIZE is a hard ordering boundary.
  for (unsigned former_index = encoder_count - 1; former_index-- > 0;) {
    auto *former = encoders[former_index];
    switch (former->type) {
    case EncoderType::Clear:
    case EncoderType::Render:
    case EncoderType::Compute:
    case EncoderType::Blit:
      break;
    default:
      continue;
    }

    for (unsigned latter_index = former_index + 1;
         latter_index < encoder_count; latter_index++) {
      auto *latter = encoders[latter_index];
      if (latter->type == EncoderType::Null)
        continue;
      if (checkEncoderRelation(former, latter) ==
          DXMT_ENCODER_LIST_OP_SYNCHRONIZE)
        break;
      if (former->type == EncoderType::Null)
        break;
    }
  }
}

DXMT_ENCODER_LIST_OP
ArgumentEncodingContext::checkEncoderRelation(EncoderData *former, EncoderData *latter) {

  if (former->type == EncoderType::Null)
    return DXMT_ENCODER_LIST_OP_SWAP;
  if (latter->type == EncoderType::Null)
    return DXMT_ENCODER_LIST_OP_SWAP;
  if (former->type == EncoderType::SignalEvent)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (latter->type == EncoderType::SignalEvent)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (former->type == EncoderType::WaitForEvent)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (latter->type == EncoderType::WaitForEvent)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (former->type == EncoderType::SampleTimestamp)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (latter->type == EncoderType::SampleTimestamp)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (former->type == EncoderType::ResolveTimestamp)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (latter->type == EncoderType::ResolveTimestamp)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;

  if (FenceOnlyBlitOptimizationEnabled() &&
      former->type == EncoderType::Blit &&
      IsFenceOnlyBlitPass(reinterpret_cast<BlitEncoderData *>(former))) {
    MergeFenceOnlyBlitPassInto(reinterpret_cast<BlitEncoderData *>(former), latter);
    currentFrameStatistics().blit_pass_optimized++;
    currentFrameStatistics().blit_pass_merged_fence_only_count++;
    return DXMT_ENCODER_LIST_OP_SWAP;
  }

  if (former->type == EncoderType::Blit && latter->type == EncoderType::Blit) {
    if (tryMergeBlitEncoders(
            reinterpret_cast<BlitEncoderData *>(former),
            reinterpret_cast<BlitEncoderData *>(latter)
        ))
      return DXMT_ENCODER_LIST_OP_SWAP;

    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  }

  if (former->type == EncoderType::Compute && latter->type == EncoderType::Compute) {
    if (tryMergeComputeEncoders(
            reinterpret_cast<ComputeEncoderData *>(former),
            reinterpret_cast<ComputeEncoderData *>(latter)
        ))
      return DXMT_ENCODER_LIST_OP_SWAP;

    return hasDataDependency(latter, former)
               ? DXMT_ENCODER_LIST_OP_SYNCHRONIZE
               : DXMT_ENCODER_LIST_OP_SWAP;
  }

  while (former->type != latter->type) {
    if (former->type == EncoderType::Clear && latter->type == EncoderType::Render) {
      auto render = reinterpret_cast<RenderEncoderData *>(latter);
      auto clear = reinterpret_cast<ClearEncoderData *>(former);

      if (clear->has_rects)
        break;
      if (render->render_target_array_length != clear->array_length)
        break;

      if (clear->clear_dsv) {
        // Consume a plane only when the clear was actually applied to it. The
        // load action is Load when the pass is built and moves to Clear only
        // because an earlier fold already installed one, so a matched
        // attachment that is not Load means this clear cannot be expressed in
        // the pass descriptor. Retiring the bit anyway dropped the clear
        // outright, and a dropped depth clear is invisible at the point it
        // happens: the pass loads the previous contents, and whatever the app
        // draws next is depth-tested against a stale buffer, so geometry simply
        // fails to appear while every draw is still issued.
        if (auto depth_attachment = isClearDepthSignatureMatched(clear, render)) {
          if (depth_attachment->load_action == WMTLoadActionLoad) {
            depth_attachment->clear_depth = clear->depth_stencil.first;
            depth_attachment->load_action = WMTLoadActionClear;
            clear->clear_dsv &= ~1;
          }
        }
        if (auto stencil_attachment = isClearStencilSignatureMatched(clear, render)) {
          if (stencil_attachment->load_action == WMTLoadActionLoad) {
            stencil_attachment->clear_stencil = clear->depth_stencil.second;
            stencil_attachment->load_action = WMTLoadActionClear;
            clear->clear_dsv &= ~2;
          }
        }
        if (clear->clear_dsv == 0) {
          render->fence_update.merge(clear->fence_update);
          render->fence_wait.merge(clear->fence_wait);
          render->fence_wait.subtract(clear->fence_update);
          currentFrameStatistics().clear_pass_optimized++;
          clear->~ClearEncoderData();
          clear->next = nullptr;
          clear->type = EncoderType::Null;
          return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
        }
      } else {
        if (auto attachment = isClearColorSignatureMatched(clear, render)) {
          // Same rule as the depth/stencil planes above: a matched attachment
          // that is no longer Load already carries someone else's clear, so
          // this one cannot be folded in. Leave it standing as its own encoder
          // rather than destroying it, which costs one pass in a case that
          // needs two clears before a single pass, and never loses one.
          if (attachment->load_action != WMTLoadActionLoad)
            break;
          attachment->load_action = WMTLoadActionClear;
          attachment->clear_color = clear->color;
          render->fence_update.merge(clear->fence_update);
          render->fence_wait.merge(clear->fence_wait);
          render->fence_wait.subtract(clear->fence_update);
          currentFrameStatistics().clear_pass_optimized++;
          clear->~ClearEncoderData();
          clear->next = nullptr;
          clear->type = EncoderType::Null;
          return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
        }
      }
      break;
    }
    if (latter->type == EncoderType::Clear && former->type == EncoderType::Render) {
      auto render = reinterpret_cast<RenderEncoderData *>(former);
      auto clear = reinterpret_cast<ClearEncoderData *>(latter);

      // DontCare can be used because it's going to be cleared anyway
      // just keep in mind DontCare != DontStore
      if (!clear->has_rects && clear->clear_dsv & 1 &&
          render->depth.attachment == clear->attachment)
        render->depth.store_action = WMTStoreActionDontCare;
      if (!clear->has_rects && clear->clear_dsv & 2 &&
          render->stencil.attachment == clear->attachment)
        render->stencil.store_action = WMTStoreActionDontCare;
    }
    if (former->type == EncoderType::Render && latter->type == EncoderType::Resolve) {
      auto render = reinterpret_cast<RenderEncoderData *>(former);
      auto resolve = reinterpret_cast<ResolveEncoderData *>(latter);
      auto result = isResolveSignatureMatched(render, resolve);
      if (result.src) {
        result.src->store_action = WMTStoreActionStoreAndMultisampleResolve;
        result.src->resolve_attachment = result.dst;
        auto fence_merge = BuildRenderResolveFenceMergePlan(
            render->fence_wait, render->fence_update,
            resolve->fence_wait, resolve->fence_update);
        render->fence_wait = std::move(fence_merge.fragment_waits);
        render->fence_update = std::move(fence_merge.fragment_updates);

        currentFrameStatistics().resolve_pass_optimized++;
        resolve->~ResolveEncoderData();
        resolve->next = nullptr;
        resolve->type = EncoderType::Null;
        return DXMT_ENCODER_LIST_OP_SWAP; // carry on (RENDER -> RESOLVE -> RESOLVE -> ...)
      }
    }
    return hasDataDependency(latter, former) ? DXMT_ENCODER_LIST_OP_SYNCHRONIZE : DXMT_ENCODER_LIST_OP_SWAP;
  }

  if (former->type == EncoderType::Render) {
    auto r1 = reinterpret_cast<RenderEncoderData *>(latter);
    auto r0 = reinterpret_cast<RenderEncoderData *>(former);

    std::optional<RenderFenceMergePlan> fence_merge;
    if (!r1->prevent_merge_with_previous &&
        isEncoderSignatureMatched(r0, r1)) {
      fence_merge = BuildRenderFenceMergePlan(
          r1->fence_wait, r1->fence_wait_vertex,
          r1->fence_update, r1->fence_update_vertex,
          r0->fence_wait, r0->fence_wait_vertex,
          r0->fence_update, r0->fence_update_vertex);
    }

    if (fence_merge && fence_merge->valid()) {
      for (unsigned i = 0; i < r0->render_target_count; i++) {
        auto &a0 = r0->colors[i];
        auto &a1 = r1->colors[i];
        a1.load_action = a0.load_action;
        a1.clear_color = a0.clear_color;
      }

      r1->depth.load_action = r0->depth.load_action;
      r1->depth.clear_depth = r0->depth.clear_depth;
      r1->depth.store_action = r0->depth.store_action;
      r1->depth.level = r0->depth.level;
      r1->depth.slice = r0->depth.slice;
      r1->depth.depth_plane = r0->depth.depth_plane;
      r1->stencil.load_action = r0->stencil.load_action;
      r1->stencil.clear_stencil = r0->stencil.clear_stencil;
      r1->stencil.store_action = r0->stencil.store_action;
      r1->stencil.level = r0->stencil.level;
      r1->stencil.slice = r0->stencil.slice;
      r1->stencil.depth_plane = r0->stencil.depth_plane;

      if ((void *)r0->cmd_tail != &r0->cmd_head) {
        if (r0->argbuf_slice.gpu_buffer != r1->argbuf_slice.gpu_buffer) {
          auto original_head = r0->cmd_head.next.get();
          auto original_tail = r0->cmd_tail;
          r0->cmd_head.next.set(nullptr);
          r0->cmd_tail = reinterpret_cast<wmtcmd_base *>(&r0->cmd_head);
          appendRenderArgumentBufferBindings(
              r0, r0->argbuf_slice.gpu_buffer, r0->use_geometry,
              r0->use_tessellation);
          r0->cmd_tail->next.set(original_head);
          r0->cmd_tail = original_tail;
          appendRenderArgumentBufferBindings(
              r0, r1->argbuf_slice.gpu_buffer, r1->use_geometry,
              r1->use_tessellation);
        }
        r0->cmd_tail->next.set(r1->cmd_head.next.get());
        r1->cmd_head.next.set(r0->cmd_head.next.get());
        r0->cmd_head.next.set(nullptr);
        r0->cmd_tail = (wmtcmd_base *)&r0->cmd_head;
      }
      r1->use_tessellation = r0->use_tessellation || r1->use_tessellation;
      r1->use_geometry = r0->use_geometry || r1->use_geometry;
      std::move(
        r1->gs_arg_marshal_tasks.begin(),
        r1->gs_arg_marshal_tasks.end(),
        std::back_inserter(r0->gs_arg_marshal_tasks)
      );
      std::move(
        r1->ts_arg_marshal_tasks.begin(),
        r1->ts_arg_marshal_tasks.end(),
        std::back_inserter(r0->ts_arg_marshal_tasks)
      );
      r1->gs_arg_marshal_tasks = std::move(r0->gs_arg_marshal_tasks);
      r1->ts_arg_marshal_tasks = std::move(r0->ts_arg_marshal_tasks);
      r1->use_visibility_result = r0->use_visibility_result || r1->use_visibility_result;

      r1->fence_wait = std::move(fence_merge->fragment_waits);
      r1->fence_wait_vertex = std::move(fence_merge->pre_raster_waits);
      r1->fence_update = std::move(fence_merge->fragment_updates);
      r1->fence_update_vertex = std::move(fence_merge->pre_raster_updates);
      r1->requires_cross_submit_wait = r0->requires_cross_submit_wait ||
                                       r1->requires_cross_submit_wait;

      // r0's commands are prepended into r1, but r0 itself will not be encoded after this point.
      // On 32-bit builds the argument buffer writes live in a shadow allocation until explicitly flushed.
      FlushRenderEncoderArgumentBuffer(r0);

      currentFrameStatistics().render_pass_optimized++;
      r0->~RenderEncoderData();
      r0->next = nullptr;
      r0->type = EncoderType::Null;

      return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
    }
  }

  if (hasDataDependency(latter, former)) {
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  }
  return DXMT_ENCODER_LIST_OP_SWAP;
}

bool
ArgumentEncodingContext::tryMergeBlitEncoders(BlitEncoderData *former, BlitEncoderData *latter) {
  if (hasDataDependency(latter, former))
    return false;

  const bool former_fence_only = IsFenceOnlyBlitPass(former);
  if (former_fence_only)
    currentFrameStatistics().blit_pass_merged_fence_only_count++;

  if ((void *)former->cmd_tail != &former->cmd_head) {
    former->cmd_tail->next.set(latter->cmd_head.next.get());
    latter->cmd_head.next.set(former->cmd_head.next.get());
    if ((void *)latter->cmd_tail == &latter->cmd_head)
      latter->cmd_tail = former->cmd_tail;
    former->cmd_head.next.set(nullptr);
    former->cmd_tail = reinterpret_cast<wmtcmd_base *>(&former->cmd_head);
  }

  latter->fence_update.merge(former->fence_update);
  latter->fence_wait.merge(former->fence_wait);
  latter->fence_wait.subtract(former->fence_update);
  latter->requires_cross_submit_wait = former->requires_cross_submit_wait ||
                                       latter->requires_cross_submit_wait;

  former->~BlitEncoderData();
  former->next = nullptr;
  former->type = EncoderType::Null;
  currentFrameStatistics().blit_pass_optimized++;
  return true;
}

bool
ArgumentEncodingContext::tryMergeComputeEncoders(ComputeEncoderData *former, ComputeEncoderData *latter) {
  if (hasDataDependency(latter, former))
    return false;

  if ((void *)former->cmd_tail != &former->cmd_head) {
    former->argbuf_slice.flush_if_needed();

    if (former->argbuf_slice.length) {
      auto original_head = former->cmd_head.next.get();
      auto original_tail = former->cmd_tail;
      former->cmd_head.next.set(nullptr);
      former->cmd_tail = reinterpret_cast<wmtcmd_base *>(&former->cmd_head);
      appendComputeArgumentBufferBindings(former, former->argbuf_slice.gpu_buffer);
      former->cmd_tail->next.set(original_head);
      former->cmd_tail = original_tail;
      if (latter->argbuf_slice.length &&
          former->argbuf_slice.gpu_buffer != latter->argbuf_slice.gpu_buffer)
        appendComputeArgumentBufferBindings(former, latter->argbuf_slice.gpu_buffer);
    }

    former->cmd_tail->next.set(latter->cmd_head.next.get());
    latter->cmd_head.next.set(former->cmd_head.next.get());
    if ((void *)latter->cmd_tail == &latter->cmd_head)
      latter->cmd_tail = former->cmd_tail;
    former->cmd_head.next.set(nullptr);
    former->cmd_tail = reinterpret_cast<wmtcmd_base *>(&former->cmd_head);
  }

  latter->fence_update.merge(former->fence_update);
  latter->fence_wait.merge(former->fence_wait);
  latter->fence_wait.subtract(former->fence_update);
  latter->requires_cross_submit_wait = former->requires_cross_submit_wait ||
                                       latter->requires_cross_submit_wait;

  currentFrameStatistics().compute_pass_optimized++;
  former->~ComputeEncoderData();
  former->next = nullptr;
  former->type = EncoderType::Null;
  return true;
}

bool
ArgumentEncodingContext::tryDeferFenceOnlyBlitPass(EncoderData *encoder) {
  if (encoder->type != EncoderType::Blit)
    return false;

  auto blit = reinterpret_cast<BlitEncoderData *>(encoder);
  if (!IsFenceOnlyBlitPass(blit))
    return false;

  pending_fence_only_blit_wait_.merge(blit->fence_wait);
  pending_fence_only_blit_update_.merge(blit->fence_update);
  pending_fence_only_blit_wait_.subtract(blit->fence_update);
  has_pending_fence_only_blit_ = true;
  currentFrameStatistics().blit_pass_optimized++;
  blit->~BlitEncoderData();
  blit->next = nullptr;
  blit->type = EncoderType::Null;
  return true;
}

void
ArgumentEncodingContext::appendPendingFenceOnlyBlitPass() {
  if (!has_pending_fence_only_blit_)
    return;

  auto encoder_info = allocate<BlitEncoderData>();
  encoder_info->type = EncoderType::Blit;
  encoder_info->id = ~0ull;
  encoder_info->fence_wait = pending_fence_only_blit_wait_;
  encoder_info->fence_update = pending_fence_only_blit_update_;
  encoder_info->cmd_head.type = WMTBlitCommandNop;
  encoder_info->cmd_head.next.set(0);
  encoder_info->cmd_tail = (wmtcmd_base *)&encoder_info->cmd_head;

  encoder_last->next = encoder_info;
  encoder_last = encoder_info;
  encoder_count_++;

  pending_fence_only_blit_wait_ = {};
  pending_fence_only_blit_update_ = {};
  has_pending_fence_only_blit_ = false;
}

void
ArgumentEncodingContext::mergePendingFenceOnlyBlitPassInto(EncoderData *encoder) {
  if (!has_pending_fence_only_blit_)
    return;

  encoder->fence_wait.merge(pending_fence_only_blit_wait_);
  encoder->fence_update.merge(pending_fence_only_blit_update_);
  encoder->fence_wait.subtract(pending_fence_only_blit_update_);
  pending_fence_only_blit_wait_ = {};
  pending_fence_only_blit_update_ = {};
  has_pending_fence_only_blit_ = false;
}

static bool
IsFenceOnlyBlitPass(const BlitEncoderData *data) {
  return data->cmd_head.next.ptr == nullptr;
}

static void
MergeFenceOnlyBlitPassInto(BlitEncoderData *former, EncoderData *latter) {
  latter->fence_wait.merge(former->fence_wait);
  latter->fence_update.merge(former->fence_update);
  latter->fence_wait.subtract(former->fence_update);
  former->~BlitEncoderData();
  former->next = nullptr;
  former->type = EncoderType::Null;
}

bool
ArgumentEncodingContext::hasDataDependency(EncoderData *latter, EncoderData *former) {
  if (former->type == EncoderType::Render) {
    auto r0 = reinterpret_cast<RenderEncoderData *>(former);
    FenceSet fence_wait_r0 = r0->fence_wait.unionOf(r0->fence_wait_vertex);
    FenceSet fence_update_r0 = r0->fence_update_vertex.unionOf(r0->fence_update);
    if (latter->type == EncoderType::Render) {
      auto r1 = reinterpret_cast<RenderEncoderData *>(latter);
      FenceSet fence_wait_r1 = r1->fence_wait.unionOf(r1->fence_wait_vertex);
      FenceSet fence_update_r1 = r1->fence_update_vertex.unionOf(r1->fence_update);
      return fence_update_r0.intersectedWith(fence_wait_r1) || fence_update_r1.intersectedWith(fence_wait_r0);
    }
    return fence_update_r0.intersectedWith(latter->fence_wait) || latter->fence_update.intersectedWith(fence_wait_r0);
  }
  if (latter->type == EncoderType::Render) {
    auto r1 = reinterpret_cast<RenderEncoderData *>(latter);
    FenceSet fence_wait = r1->fence_wait.unionOf(r1->fence_wait_vertex);
    FenceSet fence_update = r1->fence_update_vertex.unionOf(r1->fence_update);
    return former->fence_update.intersectedWith(fence_wait) || fence_update.intersectedWith(former->fence_wait);
  }
  return former->fence_update.intersectedWith(latter->fence_wait) ||
         latter->fence_update.intersectedWith(former->fence_wait);
}

bool
ArgumentEncodingContext::isEncoderSignatureMatched(RenderEncoderData *r0, RenderEncoderData *r1) {
  // FIXME: it can be different?
  if (r0->render_target_count != r1->render_target_count)
    return false;
  if (r0->dsv_planar_flags != r1->dsv_planar_flags)
    return false;
  if (r0->dsv_readonly_flags != r1->dsv_readonly_flags)
    return false;
  if (r0->render_target_array_length != r1->render_target_array_length)
    return false;
  if (r0->dsv_planar_flags & 1) {
    if (r0->depth.attachment != r1->depth.attachment)
      return false;
    if (r0->depth.level != r1->depth.level || r0->depth.slice != r1->depth.slice ||
        r0->depth.depth_plane != r1->depth.depth_plane)
      return false;
    if (r0->dsv_readonly_flags & 1) {
      if (r1->depth.load_action == WMTLoadActionClear)
        return false;
    } else {
      if (r0->depth.store_action != WMTStoreActionStore)
        return false;
      if (r1->depth.load_action != WMTLoadActionLoad)
        return false;
    }
  }
  if (r0->dsv_planar_flags & 2) {
    if (r0->stencil.attachment != r1->stencil.attachment)
      return false;
    if (r0->stencil.level != r1->stencil.level || r0->stencil.slice != r1->stencil.slice ||
        r0->stencil.depth_plane != r1->stencil.depth_plane)
      return false;
    if (r0->dsv_readonly_flags & 2) {
      if (r1->stencil.load_action == WMTLoadActionClear)
        return false;
    } else {
      if (r0->stencil.store_action != WMTStoreActionStore)
        return false;
      if (r1->stencil.load_action != WMTLoadActionLoad)
        return false;
    }
  }
  for (unsigned i = 0; i < r0->render_target_count; i++) {
    auto &a0 = r0->colors[i];
    auto &a1 = r1->colors[i];
    if (a0.attachment != a1.attachment)
      return false;
    if (a0.buffer_attachment.ptr() != a1.buffer_attachment.ptr())
      return false;
    if (a0.buffer_view_id != a1.buffer_view_id)
      return false;
    if (a0.depth_plane != a1.depth_plane)
      return false;
    if (!a0.attachment && !a0.buffer_texture)
      continue;
    if (a0.store_action != WMTStoreActionStore)
      return false;
    if (a1.load_action != WMTLoadActionLoad)
      return false;
  }
  return true;
}

RenderEncoderColorAttachmentData *
ArgumentEncodingContext::isClearColorSignatureMatched(ClearEncoderData *clear, RenderEncoderData *render) {
  for (unsigned i = 0; i < render->render_target_count; i++) {
    auto &attachment = render->colors[i];
    if (attachment.attachment == clear->attachment &&
        attachment.level == clear->level &&
        attachment.slice == clear->slice &&
        attachment.depth_plane == clear->depth_plane) {
      return &attachment;
    }
    if (attachment.buffer_attachment.ptr() &&
        attachment.buffer_attachment.ptr() == clear->buffer_attachment.ptr() &&
        attachment.buffer_view_id == clear->buffer_view_id) {
      return &attachment;
    }
  }
  return nullptr;
}

RenderEncoderDepthAttachmentData *
ArgumentEncodingContext::isClearDepthSignatureMatched(ClearEncoderData *clear, RenderEncoderData *render) {
  if ((clear->clear_dsv & 1) == 0)
    return nullptr;
  if (render->depth.attachment != clear->attachment)
    return nullptr;
  if (render->depth.level != clear->level || render->depth.slice != clear->slice ||
      render->depth.depth_plane != clear->depth_plane)
    return nullptr;
  return &render->depth;
}

RenderEncoderStencilAttachmentData *
ArgumentEncodingContext::isClearStencilSignatureMatched(ClearEncoderData *clear, RenderEncoderData *render) {
  if ((clear->clear_dsv & 2) == 0)
    return nullptr;
  if (render->stencil.attachment != clear->attachment)
    return nullptr;
  if (render->stencil.level != clear->level || render->stencil.slice != clear->slice ||
      render->stencil.depth_plane != clear->stencil_depth_plane)
    return nullptr;
  return &render->stencil;
}

ArgumentEncodingContext::ResolveSignatureMatchResult
ArgumentEncodingContext::isResolveSignatureMatched(RenderEncoderData *render, ResolveEncoderData *resolve) {
  ResolveSignatureMatchResult ret{};
  for (unsigned i = 0; i < render->render_target_count; i++) {
    auto &color = render->colors[i];
    if (!color.attachment)
      continue;
    if (color.store_action != WMTStoreActionStore)
      continue;
    if (color.resolve_attachment)
      continue;
    if (color.attachment->allocation != resolve->src->allocation)
      continue;
    if (color.attachment->key == resolve->src->key) {
      ret.src = &color;
      ret.dst = resolve->dst;
      break;
    };
    auto &descriptor_src = resolve->src->allocation->descriptor;
    auto &descriptor_dst = resolve->dst->allocation->descriptor;
    auto color_format = descriptor_src->pixelFormat(color.attachment->key);
    auto view_src_in_color_format = descriptor_src->checkViewUseFormat(resolve->src->key, color_format);
    if (color.attachment->key == view_src_in_color_format) {
      auto view_dst_in_color_format = descriptor_dst->checkViewUseFormat(resolve->dst->key, color_format);
      ret.src = &color;
      ret.dst = descriptor_dst->view(view_dst_in_color_format, resolve->dst->allocation);
      break;
    }
  }
  return ret;
}

} // namespace dxmt
