#include "d3d12_snapshot_binding_apply.hpp"

#include "d3d12_argument_buffer_layout.hpp"
#include "d3d12_binding_debug_log.hpp"
#include "d3d12_bindless_mirror_fill.hpp"
#include "d3d12_bindless_mirror_slot_fill.hpp"
#include "d3d12_bindless_stage_encode.hpp"
#include "d3d12_compiled_binding_encode.hpp"
#include "d3d12_descriptor_table_access.hpp"
#include "d3d12_legacy_binding_encode.hpp"
#include "d3d12_root_parameter_apply.hpp"
#include "d3d12_snapshot_root_constants.hpp"
#include "dxmt_statistics.hpp"

#include <cassert>

#include <d3d12.h>

namespace dxmt::d3d12 {

void ApplyGraphicsBindingSnapshot(const SubmissionBindingContext &ctx,
                                  ArgumentEncodingContext &enc,
                                  const GraphicsBindingSnapshot &snapshot,
                                  const PipelineState &pipeline,
                                  bool use_geometry, bool use_tessellation,
                                  uint64_t &argbuf_offset,
                                  BindlessMirrorDrawDiag *draw_diag) {
  auto *perf_stats = dxmt::perf::frameStatisticsForContext(enc);
  dxmt::perf::ScopedFrameDuration snapshot_scope(
      perf_stats,
      &dxmt::FrameStatistics::frame_compiled_draw_binding_snapshot_interval);
  dxmt::perf::addFrameCounter(
      perf_stats,
      &dxmt::FrameStatistics::frame_compiled_draw_binding_snapshot_applied);
  dxmt::perf::addFrameCounter(perf_stats,
                    &dxmt::FrameStatistics::frame_compiled_snapshot_entries,
                    SnapshotBindingEntryCount(snapshot));

  if (auto *root = snapshot.root_signature_impl) {
    dxmt::perf::ScopedFrameDuration static_sampler_scope(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_snapshot_static_samplers_interval);
    if (snapshot.bindless && snapshot.legacy_identity)
      ApplyRootDescriptorTables(ctx.device, ctx.queue, enc,
                                *snapshot.legacy_identity, pipeline, false);
    else
      ApplyStaticSamplers(ctx.device, enc, pipeline, *root, false);
  }

  // GPTK materialize-early / bind-late: ordinary bindless with capture-time
  // frozen tables only needs residency (above) + vertex buffers + frozen
  // upload. Skip the interpreted BindDescriptor walk and live
  // BuildBindlessRootOffsets rebuild — that was the main replay CPU cost
  // and the source of MissingTable root-offset gaps on GraphicsBindingSnapshot.
  const bool frozen_ordinary_bindless =
      snapshot.bindless && !use_geometry && !use_tessellation &&
      snapshot.root_signature_impl &&
      snapshot.frozen_bindless_vertex.valid &&
      snapshot.frozen_bindless_pixel.valid;

  if (!frozen_ordinary_bindless) {
  {
    dxmt::perf::ScopedFrameDuration entries_scope(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_snapshot_entries_interval);
    if (snapshot.native_descriptor_recipe) {
      const auto &recipe_entries = snapshot.native_descriptor_recipe->entries;
      assert(recipe_entries.size() ==
             snapshot.native_descriptor_indices.size());
      for (size_t i = 0; i < recipe_entries.size(); ++i) {
        const auto &entry = recipe_entries[i];
        const auto stage = static_cast<PipelineStage>(entry.stage);
        const auto range_type =
            static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(entry.range_type);
        const auto descriptor_index = snapshot.native_descriptor_indices[i];
        if (descriptor_index == UINT32_MAX) {
          dxmt::perf::addFrameCounter(
              perf_stats,
              &dxmt::FrameStatistics::
                  frame_compiled_snapshot_clear_descriptors);
          ClearDescriptorBinding(enc, stage, range_type, entry.slot);
          continue;
        }
        const auto &descriptor = SnapshotNativeDescriptor(snapshot, i);
        dxmt::perf::addFrameCounter(
            perf_stats,
            &dxmt::FrameStatistics::frame_compiled_snapshot_descriptors);
        DebugLogRootBinding(
            DescriptorRangeTypeName(range_type), pipeline, false, stage,
            entry.root_index, entry.slot, entry.shader_register,
            entry.argument.RegisterCount ? entry.argument.RegisterSpace : 0,
            DescriptorRecordSizeBytes(descriptor), 0, &descriptor,
            &entry.argument);
        BindDescriptor(ctx.device, enc, stage, range_type, entry.slot,
                       descriptor, &entry.argument);
      }
    }
    for (const auto &entry : snapshot.entries) {
      if (entry.kind == GraphicsBindingSnapshotEntry::Kind::RootConstants) {
        dxmt::perf::addFrameCounter(
            perf_stats,
            &dxmt::FrameStatistics::frame_compiled_snapshot_root_constants);
        dxmt::perf::ScopedFrameDuration root_constants_scope(
            perf_stats,
            &dxmt::FrameStatistics::frame_compiled_snapshot_root_constants_interval);
        DebugLogRootBinding(
            entry.debug_kind ? entry.debug_kind : "snapshot", pipeline, false,
            entry.stage, entry.root_index, entry.slot, entry.shader_register,
            entry.register_space, entry.debug_size, entry.debug_address);
        BindRootConstantsSnapshot(ctx.queue, enc, entry);
      } else if (entry.has_descriptor) {
        const auto &descriptor = SnapshotDescriptor(snapshot, entry);
        dxmt::perf::addFrameCounter(
            perf_stats,
            &dxmt::FrameStatistics::frame_compiled_snapshot_descriptors);
        // Bindless-mirror (③.3): the snapshot path does not run ApplyDescriptorTableBindingRecipe,
        // so fill the persistent mirror here from the captured descriptor (record.mirror travels
        // with the DescriptorRecord). The live path fills it in ApplyDescriptorTableBindingRecipe.
        if (snapshot.bindless) {
          const auto bindless_fill_begin = dxmt::clock::now();
          const auto fill_kind = MaybeFillBindlessMirrorSlot(
              ctx.device, enc, entry.range_type, descriptor, entry.stage,
              entry.argument);
          if (fill_kind != BindlessMirrorFillKind::None) {
            dxmt::perf::addFrameCounter(
                perf_stats,
                &dxmt::FrameStatistics::frame_compiled_snapshot_bindless_fills);
            if (perf_stats)
              perf_stats->frame_compiled_snapshot_bindless_fill_interval +=
                  dxmt::clock::now() - bindless_fill_begin;
            switch (fill_kind) {
            case BindlessMirrorFillKind::Texture:
              dxmt::perf::addFrameCounter(
                  perf_stats,
                  &dxmt::FrameStatistics::
                      frame_compiled_snapshot_bindless_fill_texture);
              break;
            case BindlessMirrorFillKind::Sampler:
              dxmt::perf::addFrameCounter(
                  perf_stats,
                  &dxmt::FrameStatistics::
                      frame_compiled_snapshot_bindless_fill_sampler);
              break;
            case BindlessMirrorFillKind::TextureBuffer:
              dxmt::perf::addFrameCounter(
                  perf_stats,
                  &dxmt::FrameStatistics::
                      frame_compiled_snapshot_bindless_fill_texture_buffer);
              break;
            case BindlessMirrorFillKind::Null:
              dxmt::perf::addFrameCounter(
                  perf_stats,
                  &dxmt::FrameStatistics::
                      frame_compiled_snapshot_bindless_fill_null);
              break;
            case BindlessMirrorFillKind::None:
              break;
            }
          }
        }
        DebugLogRootBinding(
            entry.debug_kind ? entry.debug_kind : "snapshot", pipeline,
            false, entry.stage, entry.root_index, entry.slot,
            entry.shader_register, entry.register_space, entry.debug_size,
            entry.debug_address, &descriptor, entry.argument);
        {
          dxmt::perf::ScopedFrameDuration descriptor_scope(
              perf_stats,
              &dxmt::FrameStatistics::frame_compiled_snapshot_descriptors_interval);
          BindDescriptor(ctx.device, enc, entry.stage, entry.range_type,
                         entry.slot, descriptor, entry.argument);
        }
      } else {
        dxmt::perf::addFrameCounter(
            perf_stats,
            &dxmt::FrameStatistics::frame_compiled_snapshot_clear_descriptors);
        dxmt::perf::ScopedFrameDuration clear_scope(
            perf_stats,
            &dxmt::FrameStatistics::frame_compiled_snapshot_clear_descriptors_interval);
        ClearDescriptorBinding(enc, entry.stage, entry.range_type,
                               entry.slot);
      }
    }
  }
  } // !frozen_ordinary_bindless

  const auto pipeline_kind = use_tessellation
                                 ? PipelineKind::Tessellation
                                 : use_geometry ? PipelineKind::Geometry
                                                : PipelineKind::Ordinary;
  {
    dxmt::perf::addFrameCounter(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_snapshot_vertex_buffers,
        snapshot.vertex_buffers.size());
    dxmt::perf::ScopedFrameDuration vertex_buffers_scope(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_snapshot_vertex_buffers_interval);
    const auto max_slot = snapshot.vertex_slot_mask
                              ? 32u - __builtin_clz(snapshot.vertex_slot_mask)
                              : 0u;
    for (UINT slot = 0; slot < max_slot; ++slot) {
      if (snapshot.vertex_slot_mask & (1u << slot))
        enc.bindVertexBuffer(slot, 0, 0, Rc<Buffer>());
    }
    for (const auto &binding : snapshot.vertex_buffers) {
      auto buffer = binding.buffer;
      enc.bindVertexBuffer(binding.slot, binding.offset, binding.stride,
                           std::move(buffer));
    }
  }
  if (snapshot.vertex_slot_mask) {
    dxmt::perf::ScopedFrameDuration vertex_table_scope(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_snapshot_vertex_table_interval);
    const auto table_size =
        uint64_t(__builtin_popcount(snapshot.vertex_slot_mask)) * 16u;
    const auto offset = AllocateArgumentBuffer(argbuf_offset, table_size);
    if (pipeline_kind == PipelineKind::Geometry)
      enc.encodeVertexBuffers<PipelineKind::Geometry>(
          snapshot.vertex_slot_mask, offset);
    else if (pipeline_kind == PipelineKind::Tessellation)
      enc.encodeVertexBuffers<PipelineKind::Tessellation>(
          snapshot.vertex_slot_mask, offset);
    else
      enc.encodeVertexBuffers<PipelineKind::Ordinary>(
          snapshot.vertex_slot_mask, offset);
  }

  const auto &shaders = pipeline.GetDxilShaders();
  const auto &key = pipeline.GetShaderCacheKey();
  BindlessMirrorDrawDiag local_diag = {};
  BindlessMirrorDrawDiag &diag = draw_diag ? *draw_diag : local_diag;
  const bool native =
      pipeline.GetShaderAbiVersion() ==
      DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE;
  if (native) {
    diag.path = "compiled-native";
    diag.bindless_bound = snapshot.root_signature_impl != nullptr;
    if (!snapshot.root_signature_impl)
      return;

    EncodeNativeArgumentTables(
        enc, snapshot, false,
        WMTRenderStageVertex | WMTRenderStageFragment);
    for (const auto &shader : shaders) {
      if (shader.stage == PipelineShaderStage::Vertex) {
        EncodeShaderBindingsForStageBindless<PipelineStage::Vertex>(
            ctx, enc, snapshot, pipeline, *snapshot.root_signature_impl,
            shader, key, false, &diag, &snapshot.native_vertex);
      } else if (shader.stage == PipelineShaderStage::Pixel) {
        EncodeShaderBindingsForStageBindless<PipelineStage::Pixel>(
            ctx, enc, snapshot, pipeline, *snapshot.root_signature_impl,
            shader, key, false, &diag, &snapshot.native_pixel);
      }
    }
    return;
  }
  diag.uses_bindless_mirror = pipeline.UsesBindlessMirror();
  // The submission-owned bindless token retains the heaps and root state.
  // Resolve descriptor tables directly from that token instead of rebuilding
  // them from a per-draw DescriptorRecord snapshot.
  const bool snapshot_bindless = snapshot.bindless;
  diag.bindless_bound = snapshot_bindless;
  diag.path = BindlessMirrorDiagPathName(snapshot_bindless, true);
  if (!snapshot_bindless || !snapshot.root_signature_impl)
    return;
  {
    dxmt::perf::ScopedFrameDuration shader_bindings_scope(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_snapshot_shader_bindings_interval);
    for (const auto &shader : shaders) {
      if (use_geometry && shader.stage == PipelineShaderStage::Vertex) {
          EncodeShaderBindingsForStageBindless<
              PipelineStage::Vertex, PipelineKind::Geometry>(
              ctx, enc, snapshot, pipeline, *snapshot.root_signature_impl,
              shader, key, false, &diag);
        } else if (use_geometry &&
                   shader.stage == PipelineShaderStage::Geometry) {
          EncodeShaderBindingsForStageBindless<
              PipelineStage::Geometry, PipelineKind::Geometry>(
              ctx, enc, snapshot, pipeline, *snapshot.root_signature_impl,
              shader, key, false, &diag);
        } else if (use_geometry &&
                   shader.stage == PipelineShaderStage::Pixel) {
          EncodeShaderBindingsForStageBindless<
              PipelineStage::Pixel, PipelineKind::Geometry>(
              ctx, enc, snapshot, pipeline, *snapshot.root_signature_impl,
              shader, key, false, &diag);
        } else if (use_tessellation &&
                   shader.stage == PipelineShaderStage::Vertex) {
          EncodeShaderBindingsForStageBindless<
              PipelineStage::Vertex, PipelineKind::Tessellation>(
              ctx, enc, snapshot, pipeline, *snapshot.root_signature_impl,
              shader, key, false, &diag);
        } else if (use_tessellation &&
                   shader.stage == PipelineShaderStage::Hull) {
          EncodeShaderBindingsForStageBindless<
              PipelineStage::Hull, PipelineKind::Tessellation>(
              ctx, enc, snapshot, pipeline, *snapshot.root_signature_impl,
              shader, key, false, &diag);
        } else if (use_tessellation &&
                   shader.stage == PipelineShaderStage::Domain) {
          EncodeShaderBindingsForStageBindless<
              PipelineStage::Domain, PipelineKind::Tessellation>(
              ctx, enc, snapshot, pipeline, *snapshot.root_signature_impl,
              shader, key, false, &diag);
        } else if (use_tessellation &&
                   shader.stage == PipelineShaderStage::Pixel) {
          EncodeShaderBindingsForStageBindless<
              PipelineStage::Pixel, PipelineKind::Tessellation>(
              ctx, enc, snapshot, pipeline, *snapshot.root_signature_impl,
              shader, key, false, &diag);
        } else if (!use_geometry && !use_tessellation &&
                   shader.stage == PipelineShaderStage::Vertex) {
          dxmt::perf::addFrameCounter(
              perf_stats,
              &dxmt::FrameStatistics::frame_compiled_snapshot_shader_bindings);
          dxmt::perf::addFrameCounter(
              perf_stats,
              &dxmt::FrameStatistics::frame_compiled_snapshot_bindless_shader_bindings);
          dxmt::perf::ScopedFrameDuration bindless_shader_scope(
              perf_stats,
              &dxmt::FrameStatistics::frame_compiled_snapshot_bindless_shader_bindings_interval);
          // Prefer frozen upload (materialize-early) over live rebuild.
          // pack_live_buffer_table: ApplyRootDescriptorTables already filled
          // encoder state; pack slot 27 from live buffers (address churn).
          if (snapshot.frozen_bindless_vertex.valid) {
            EncodeShaderBindingsForStageBindlessSnapshot<PipelineStage::Vertex>(
                ctx.device, ctx.queue, enc, shader, key, snapshot, &diag,
                /*pack_live_buffer_table=*/true);
          } else {
            EncodeShaderBindingsForStageBindless<PipelineStage::Vertex>(
                ctx, enc, snapshot, pipeline, *snapshot.root_signature_impl,
                shader, key, false, &diag);
          }
        } else if (!use_geometry && !use_tessellation &&
                   shader.stage == PipelineShaderStage::Pixel) {
          dxmt::perf::addFrameCounter(
              perf_stats,
              &dxmt::FrameStatistics::frame_compiled_snapshot_shader_bindings);
          dxmt::perf::addFrameCounter(
              perf_stats,
              &dxmt::FrameStatistics::frame_compiled_snapshot_bindless_shader_bindings);
          dxmt::perf::ScopedFrameDuration bindless_shader_scope(
              perf_stats,
              &dxmt::FrameStatistics::frame_compiled_snapshot_bindless_shader_bindings_interval);
          if (snapshot.frozen_bindless_pixel.valid) {
            EncodeShaderBindingsForStageBindlessSnapshot<PipelineStage::Pixel>(
                ctx.device, ctx.queue, enc, shader, key, snapshot, &diag,
                /*pack_live_buffer_table=*/true);
          } else {
            EncodeShaderBindingsForStageBindless<PipelineStage::Pixel>(
                ctx, enc, snapshot, pipeline, *snapshot.root_signature_impl,
                shader, key, false, &diag);
          }
      }
    }
  }
}

} // namespace dxmt::d3d12
