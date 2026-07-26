#include "d3d12_command_queue.hpp"
#include "d3d12_dxgi_backend.hpp"

#include "airconv_dx12_metal4.h"
#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "config/config.hpp"
#include "d3d12_argument_buffer_layout.hpp"
#include "d3d12_argument_upload.hpp"
#include "d3d12_compiled_native_recipe.hpp"
#include "d3d12_compiled_snapshot_access.hpp"
#include "d3d12_live_binding_capture.hpp"
#include "d3d12_live_msaa_demote_query.hpp"
#include "d3d12_native_access_track.hpp"
#include "d3d12_root_buffer_bind.hpp"
#include "d3d12_snapshot_buffer_table.hpp"
#include "d3d12_snapshot_capture_perf.hpp"
#include "d3d12_snapshot_root_constants.hpp"
#include "d3d12_table_recipe_apply.hpp"
#include "d3d12_table_recipe_lookup.hpp"
#include "d3d12_binding_debug_log.hpp"
#include "d3d12_binding_diagnostics.hpp"
#include "d3d12_bindless_mirror_slot_fill.hpp"
#include "d3d12_bindless_window_probe.hpp"
#include "d3d12_bound_descriptor_lookup.hpp"
#include "d3d12_descriptor_access_fingerprint.hpp"
#include "d3d12_native_stage_binding.hpp"
#include "d3d12_native_stage_descriptor_diag.hpp"
#include "d3d12_root_parameter_apply.hpp"
#include "d3d12_legacy_binding_encode.hpp"
#include "d3d12_binding_recipe_cache.hpp"
#include "d3d12_binding_fingerprint.hpp"
#include "d3d12_bindless_mirror_fill.hpp"
#include "d3d12_bindless_mirror_plan.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_compiled_binding_encode.hpp"
#include "d3d12_compiled_compat_state.hpp"
#include "d3d12_compiled_binding_tables.hpp"
#include "d3d12_compiled_direct_access.hpp"
#include "d3d12_compiled_packet_resolve.hpp"
#include "d3d12_bindless_root_offsets.hpp"
#include "d3d12_bindless_stage_encode.hpp"
#include "d3d12_compiled_direct_binding_encode.hpp"
#include "d3d12_frozen_bindless_stage_tables.hpp"
#include "d3d12_graphics_binding_capture.hpp"
#include "d3d12_replay_binding_encode.hpp"
#include "d3d12_replay_dispatch_encode.hpp"
#include "d3d12_snapshot_binding_apply.hpp"
#include "d3d12_submission_binding_context.hpp"
#include "d3d12_compiled_replay_ledger.hpp"
#include "d3d12_replay_query_records.hpp"
#include "d3d12_replay_resource_access_transitions.hpp"
#include "d3d12_descriptor_snapshot_journal.hpp"
#include "d3d12_draw_visibility_scope.hpp"
#include "d3d12_render_encoder_state.hpp"
#include "d3d12_render_pass_attachments.hpp"
#include "d3d12_stage_plan_cache.hpp"
#include "d3d12_vertex_buffer_encode.hpp"
#include "d3d12_copy_footprint.hpp"
#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_descriptor_record_query.hpp"
#include "d3d12_submitted_descriptor_capture.hpp"
#include "d3d12_descriptor_table_access.hpp"
#include "d3d12_diag_readback_staging.hpp"
#include "d3d12_draw_state_audit.hpp"
#include "d3d12_compiled_graphics_emit_plan.hpp"
#include "d3d12_replay_draw_body_encode.hpp"
#include "d3d12_compiled_packet_admission.hpp"
#include "d3d12_descriptor_diagnostics.hpp"
#include "d3d12_device_queue_state.hpp"
#include "d3d12_fence.hpp"
#include "d3d12_replay_perf_frame.hpp"
#include "d3d12_submission_pending_operation.hpp"
#include "d3d12_submission_wake_channel.hpp"
#include "d3d12_submission_batch_shape.hpp"
#include "d3d12_submission_timeline.hpp"
#include "d3d12_submission_wait_state.hpp"
#include "d3d12_frozen_native_descriptor.hpp"
#include "d3d12_sparse_mapping_submission.hpp"
#include "d3d12_replay_queue_work.hpp"
#include "d3d12_heap.hpp"
#include "d3d12_queue_work_types.hpp"
#include "d3d12_retire_submission_work.hpp"
#include "d3d12_indirect_encoding.hpp"
#include "d3d12_indirect_topology.hpp"
#include "d3d12_indirect_argument_stream.hpp"
#include "d3d12_indirect_command_encode.hpp"
#include "d3d12_texture_copy_plan.hpp"
#include "d3d12_tile_copy_plan.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_pipeline_write_policy.hpp"
#include "d3d12_query.hpp"
#include "d3d12_query_resolve_policy.hpp"
#include "d3d12_compiled_bindless_payload.hpp"
#include "d3d12_replay_frame_stats_attribution.hpp"
#include "d3d12_replay_pass_admission.hpp"
#include "d3d12_submitted_descriptor_capture_plan.hpp"
#include "d3d12_submitted_descriptor_capture_telemetry.hpp"
#include "d3d12_query_resolve_range.hpp"
#include "d3d12_queue_config.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_queue_draw_state_dump.hpp"
#include "d3d12_queue_diagnostics_report.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_queue_view_binding.hpp"
#include "d3d12_replay_barrier_encode.hpp"
#include "d3d12_replay_barrier_record.hpp"
#include "d3d12_replay_buffer_immediate.hpp"
#include "d3d12_replay_clear_encode.hpp"
#include "d3d12_replay_query_resolve_ops.hpp"
#include "d3d12_replay_resolve_encode.hpp"
#include "d3d12_replay_resource_access_state.hpp"
#include "d3d12_replay_temporal_upscale.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_replay_blit_hazard.hpp"
#include "d3d12_replay_compiled_payload_types.hpp"
#include "d3d12_replay_api_state_reset.hpp"
#include "d3d12_retire_diag_work.hpp"
#include "d3d12_replay_compiled_execute_stats.hpp"
#include "d3d12_replay_compiled_execute_types.hpp"
#include "d3d12_replay_diagnostics.hpp"
#include "d3d12_replay_inline_pass_commands.hpp"
#include "d3d12_replay_perf_record_buckets.hpp"
#include "d3d12_replay_draw_packet_ops.hpp"
#include "d3d12_replay_draw_packet_types.hpp"
#include "d3d12_replay_mismatch_barrier_policy.hpp"
#include "d3d12_replay_pass_compatibility.hpp"
#include "d3d12_replay_batching_policy.hpp"
#include "d3d12_replay_binding_signature.hpp"
#include "d3d12_replay_pass_batch_ops.hpp"
#include "d3d12_replay_pass_flush_ops.hpp"
#include "d3d12_replay_stall_probe.hpp"
#include "d3d12_replay_state_clone.hpp"
#include "d3d12_replay_root_state_ops.hpp"
#include "d3d12_replay_pass_types.hpp"
#include "d3d12_replay_perf_timers.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_replay_state_types.hpp"
#include "d3d12_replay_state_ops.hpp"
#include "d3d12_resolve_region.hpp"
#include "d3d12_render_state.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_resource_barrier_batch.hpp"
#include "d3d12_resource_state_semantics.hpp"
#include "d3d12_root_binding_capture.hpp"
#include "d3d12_root_signature.hpp"
#include "d3d12_shader_binding.hpp"
#include "d3d12_selected_descriptor_diag.hpp"
#include "d3d12_shader_stage_query.hpp"
#include "d3d12_stage_plan_build.hpp"
#include "d3d12_snapshot_binding_query.hpp"
#include "d3d12_sparse_tile_copy.hpp"
#include "d3d12_submission_capture_stats.hpp"
#include "d3d12_submission_drain_diag.hpp"
#include "d3d12_sampler.hpp"
#include "d3d12_subresource_geometry.hpp"
#include "d3d12_swapchain.hpp"
#include "d3d12_temporal_scaler.hpp"
#include "d3d12_texture_swizzle.hpp"
#include "d3d12_texture_view.hpp"
#include "d3d12_tile_mapping.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "dxmt_context.hpp"
#include "dxmt_bindless_buffer_table.hpp"
#include "dxmt_format.hpp"
#include "dxmt_hud_state.hpp"
#include "dxmt_info.hpp"
#include "dxmt_legacy_buffer_slice.hpp"
#include "dxmt_perf_stats.hpp"
#include "dxmt_presenter.hpp"
#include "dxmt_sampler.hpp"
#include "dxmt_shader_cache.hpp"
#include "log/log.hpp"
#include "sha1/sha1_util.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_lifecycle_telemetry.hpp"
#include "util_noexcept.hpp"
#include "util_string.hpp"
#include "util_win32_compat.h"
#include "wsi_monitor.hpp"
#include "wsi_window.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <deque>
#include <tuple>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dxmt::d3d12 {
// CommandQueueImpl deliberately lives in the named dxmt::d3d12 namespace
// rather than an anonymous one. Replay types such as ReplayPassEncodeCommand
// take it by reference in their signatures, and a type in an anonymous
// namespace cannot be named by any header -- a forward declaration there
// would silently denote a *different* class and break every override. Keeping
// the name reachable is what allows those types to live in real headers and
// be analyzed as independent translation units.

class CommandQueueImpl final
    : public ComObjectWithInitialRef<ID3D12CommandQueue, IMTLDXGIDevice>,
      private D3D12SwapChainHost {
public:
  // SubmissionWakeState and QueueWaitState now live in
  // d3d12_submission_wake_channel.hpp: neither reads queue state, and nesting
  // them here made FenceWaitSubmission -- and with it the whole pending
  // operation FIFO -- unnameable outside this translation unit.

  class QueueSubmissionEndpoint final : public SubmissionQueueEndpoint {
  public:
    explicit QueueSubmissionEndpoint(CommandQueueImpl &queue) noexcept
        : queue_(&queue) {
      queue_->AddRefPrivate();
    }

    QueueSubmissionEndpoint(const QueueSubmissionEndpoint &) = delete;
    QueueSubmissionEndpoint &
    operator=(const QueueSubmissionEndpoint &) = delete;

    ~QueueSubmissionEndpoint() noexcept override {
      auto *queue = queue_;
      queue_ = nullptr;
      queue->ReleasePrivate();
    }

    [[nodiscard]] bool ProcessReadySubmission() noexcept override {
      return queue_->ProcessReadySubmission();
    }

  private:
    CommandQueueImpl *queue_;
  };

  // The deferred-work and retirement payload types that used to live here now
  // sit in d3d12_queue_work_types.hpp so that independent translation units
  // can name them.

  CommandQueueImpl(IMTLD3D12Device *device, const D3D12_COMMAND_QUEUE_DESC &desc,
                   std::shared_ptr<D3D12DeviceQueueState> device_queue_state)
      : device_(device), desc_(desc),
        device_queue_state_(std::move(device_queue_state)),
        submission_wake_state_(std::make_shared<SubmissionWakeState>(
            device_queue_state_->SubmissionService())) {
    auto endpoint = std::make_shared<QueueSubmissionEndpoint>(*this);
    submission_endpoint_ = endpoint;
    if (!device_queue_state_->SubmissionService()->RegisterQueue(
            std::move(endpoint))) {
      ERR("D3D12CommandQueue: failed to register logical queue with the "
          "device submission service");
      std::terminate();
    }
    static std::atomic<uint32_t> log_count = 0;
    if (D3D12DiagShouldLog(log_count, D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE"))) {
      WARN_FILE_ONLY("D3D12 queue diagnostic: CreateCommandQueue"
           " queue=", reinterpret_cast<uintptr_t>(this),
           " type=", desc_.Type,
           " priority=", desc_.Priority,
           " flags=", desc_.Flags,
           " nodeMask=", desc_.NodeMask);
    }
  }

  ~CommandQueueImpl() noexcept {
    dxmt::invokeNoexcept("binding recipe diagnostic summary", []() {
      LogBindingRecipeDiagSummary("command-queue-destroy");
    });
    dxmt::invokeNoexcept("bindless mirror diagnostic summary", []() {
      LogBindlessMirrorDiagSummary("command-queue-destroy");
    });
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const uint32_t ref_count =
        m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (ref_count)
      return ref_count;

    if (!dxmt::invokeNoexcept("D3D12 submission service shutdown",
                              [this]() { StopSubmissionService(); }))
      std::terminate();
    // This final private-reference drop may delete the queue. Do not access
    // members after it.
    ReleasePrivate();
    return 0;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12CommandQueue)) {
      *ppvObject = ref(static_cast<ID3D12CommandQueue *>(this));
      return S_OK;
    }

    if (riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDevice) ||
        riid == __uuidof(IDXGIDevice1) || riid == __uuidof(IDXGIDevice2) ||
        riid == __uuidof(IDXGIDevice3) || riid == __uuidof(IMTLDXGIDevice)) {
      *ppvObject = ref(static_cast<IMTLDXGIDevice *>(this));
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12CommandQueue), riid))
      WARN("D3D12CommandQueue: unknown interface query ", str::format(riid));

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size, void *data) override {
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size, const void *data) override {
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *data) override {
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

#ifdef __MINGW32__
  void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *resource, UINT region_count,
                                            const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
                                            const D3D12_TILE_REGION_SIZE *region_sizes,
                                            ID3D12Heap *heap,
                                            UINT range_count,
                                            const D3D12_TILE_RANGE_FLAGS *range_flags,
                                            const UINT *heap_range_offsets,
                                            const UINT *range_tile_counts,
                                            D3D12_TILE_MAPPING_FLAGS flags) override {
    dxmt::perf::ScopedFrameTimer perf_timer(
        dxmt::perf::FrameTimeBucket::TileMapping);
    UpdateTileMappingsImpl(resource, region_count, region_start_coordinates,
                           region_sizes, heap, range_count, range_flags,
                           heap_range_offsets, range_tile_counts, flags);
  }
#else
  void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *resource, UINT region_count,
                                            const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
                                            const D3D12_TILE_REGION_SIZE *region_sizes,
                                            UINT range_count,
                                            const D3D12_TILE_RANGE_FLAGS *range_flags,
                                            UINT *heap_range_offsets,
                                            UINT *range_tile_counts,
                                            D3D12_TILE_MAPPING_FLAGS flags) override {
    dxmt::perf::ScopedFrameTimer perf_timer(
        dxmt::perf::FrameTimeBucket::TileMapping);
    UpdateTileMappingsImpl(resource, region_count, region_start_coordinates,
                           region_sizes, nullptr, range_count, range_flags,
                           heap_range_offsets, range_tile_counts, flags);
  }
#endif

  void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource *dst_resource,
                                          const D3D12_TILED_RESOURCE_COORDINATE *dst_region_start_coordinate,
                                          ID3D12Resource *src_resource,
                                          const D3D12_TILED_RESOURCE_COORDINATE *src_region_start_coordinate,
                                          const D3D12_TILE_REGION_SIZE *region_size,
                                          D3D12_TILE_MAPPING_FLAGS flags) override {
    dxmt::perf::ScopedFrameTimer perf_timer(
        dxmt::perf::FrameTimeBucket::TileMapping);
    CopyTileMappingsImpl(dst_resource, dst_region_start_coordinate,
                         src_resource, src_region_start_coordinate,
                         region_size, flags);
  }

#include "d3d12_command_queue_tile_mapping.inc"
public:
  void STDMETHODCALLTYPE ExecuteCommandLists(UINT command_list_count,
                                             ID3D12CommandList *const *command_lists) override {
    dxmt::perf::ScopedCodeTimer code_timer(
        dxmt::PerfCodePath::QueueExecuteControl);
    dxmt::perf::ScopedFrameTimer perf_timer(
        dxmt::perf::FrameTimeBucket::ExecuteCommandLists);
    static std::atomic<uint32_t> diag_execute_log_count = 0;
    if (!command_list_count)
      return;

    if (device_->GetDXMTDevice().queue().HasDeviceError())
      return;

    if (!command_lists) {
      Logger::err("D3D12CommandQueue: ExecuteCommandLists called with null command list array");
      return;
    }

    std::vector<GraphicsCommandList *> validated_lists;
    validated_lists.reserve(command_list_count);
    for (UINT i = 0; i < command_list_count; i++) {
      auto *command_list = command_lists[i];
      if (!command_list) {
        Logger::err(str::format(
            "D3D12CommandQueue: null command list at index ", i));
        return;
      }

      auto *state = dynamic_cast<GraphicsCommandList *>(command_list);
      if (!state) {
        Logger::err(str::format(
            "D3D12CommandQueue: foreign command list at index ", i));
        return;
      }

      if (state->GetParentDevice() != device_.ptr()) {
        Logger::err(str::format(
            "D3D12CommandQueue: cross-device command list at index ", i));
        return;
      }

      if (state->GetCommandListType() != desc_.Type) {
        Logger::err(str::format(
            "D3D12CommandQueue: command list type ",
            state->GetCommandListType(), " does not match queue type ",
            desc_.Type));
        return;
      }

      if (!state->IsClosed()) {
        Logger::err(str::format(
            "D3D12CommandQueue: command list at index ", i,
            " is not closed"));
        return;
      }

      validated_lists.push_back(state);
    }

    auto *perf_stats = dxmt::perf::currentFrameStatistics();
    auto perf_mark = clock::now();

    {
      dxmt::perf::ScopedCodeTimer phase_timer(
          dxmt::PerfCodePath::QueueExecuteValidation);
      if (D3D12DiagShouldLog(diag_execute_log_count,
                             D3D12DiagEnabledEnv("DXMT_DIAG_D3D12_DEVICE") ||
                                 D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE"))) {
        WARN_FILE_ONLY("D3D12 diagnostic: ExecuteCommandLists"
             " queueType=", desc_.Type,
             " listCount=", command_list_count,
             " queue=", reinterpret_cast<uintptr_t>(this),
             " submittedBatches=", submitted_batches_.load(std::memory_order_relaxed),
             " pendingOps=", PendingOperationCountForDiag());
      }
    }
    dxmt::perf::recordExecuteTime(
        perf_stats, dxmt::perf::ExecuteTimeBucket::Validate,
        clock::now() - perf_mark);
    perf_mark = clock::now();

    {
      dxmt::perf::ScopedCodeTimer phase_timer(
          dxmt::PerfCodePath::QueueExecuteApitrace);
      dxmt::apitrace::on_d3d12_execute_command_lists(
          this, command_list_count, command_lists);
    }

    ExecuteSubmission submission;
    // One Execute submission has one descriptor freeze boundary. Sharing the
    // immutable record store across command lists captures a slot/version only
    // once even when several lists bind the same heap window.
    auto submitted_descriptor_records =
        std::make_shared<SubmittedDescriptorRecordStore>();
    auto submitted_native_descriptor_spans =
        std::make_shared<SubmittedNativeDescriptorSpanStore>();
    submitted_native_descriptor_spans->frozen_native =
        std::make_shared<SubmittedFrozenNativeDescriptorStore>();
    submitted_native_descriptor_spans->spans.reserve(4096);
    submitted_descriptor_records->records.reserve(4096);
    submitted_descriptor_records->reserveHeapRecords(4096);
    {
      dxmt::perf::ScopedCodeTimer phase_timer(
          dxmt::PerfCodePath::QueueExecuteCollect);
      for (auto *state : validated_lists) {
        if (SUCCEEDED(state->MarkSubmittedToQueue(
                desc_.Type, submission.allocator_uses))) {
          auto compiled = state->GetCompiledCommands();
          if (compiled) {
            if (compiled->test_telemetry) {
              compiled->test_telemetry->submitted_generation_shares.fetch_add(
                  1, std::memory_order_relaxed);
            }
            {
              dxmt::perf::ScopedCodeTimer prepare_timer(
                  dxmt::PerfCodePath::QueueExecutePreparePlan);
              submission.submitted_command_lists.emplace_back(
                  PrepareSubmittedCompiledCommandList(compiled));
            }
          } else {
            submission.submitted_command_lists.emplace_back(nullptr);
          }
          {
            dxmt::perf::ScopedCodeTimer capture_timer(
                dxmt::PerfCodePath::QueueExecuteCaptureDescriptors);
            submission.compiled_descriptor_snapshots.emplace_back(
                compiled ? CaptureSubmittedDescriptorSnapshots(
                               BindingContext(), *compiled,
                               submitted_descriptor_records,
                               submitted_native_descriptor_spans)
                         : CompiledCommandDescriptorSnapshots{});
          }
        }
      }
      FinalizeFrozenNativeDescriptorStore(
          *submitted_native_descriptor_spans->frozen_native,
          device_->GetMTLDevice(), device_->GetDXMTDevice().queue());
    }
    submission.capture_statistics.generic_descriptor_span_lookups =
        submitted_native_descriptor_spans->lookup_count;
    submission.capture_statistics.generic_descriptor_span_unique =
        submitted_native_descriptor_spans->spans.size();
    submission.capture_statistics.generic_descriptor_span_reuses =
        submitted_native_descriptor_spans->reuse_count;
    const auto &frozen =
        *submitted_native_descriptor_spans->frozen_native;
    submission.capture_statistics.frozen_native_direct_packets =
        frozen.direct_packet_count;
    submission.capture_statistics.frozen_range_lookups =
        frozen.range_lookups;
    submission.capture_statistics.frozen_range_unique =
        frozen.range_bases.size();
    submission.capture_statistics.frozen_range_reuses =
        frozen.range_reuses;
    submission.capture_statistics.frozen_root_word_lookups =
        frozen.root_word_lookups;
    submission.capture_statistics.frozen_root_word_reuses =
        frozen.root_word_reuses;

    dxmt::perf::recordExecuteTime(
        perf_stats, dxmt::perf::ExecuteTimeBucket::Collect,
        clock::now() - perf_mark);
    perf_mark = clock::now();

    if (!submission.submitted_command_lists.empty()) {
      static std::atomic<uint32_t> log_count = 0;
      if (D3D12DiagShouldLog(log_count, D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE"))) {
        WARN_FILE_ONLY("D3D12 queue diagnostic: enqueue execute"
             " queue=", reinterpret_cast<uintptr_t>(this),
             " queueType=", desc_.Type,
             " commandLists=", submission.submitted_command_lists.size(),
             " allocatorUses=", submission.allocator_uses.size());
      }
      PendingOperation op(std::move(submission));
      EnqueuePendingOperation(std::move(op), perf_stats);
    }
  }

#include "d3d12_command_queue_dxgi_compat.inc"
public:
private:
  IMTLD3D12Device &SwapChainDevice() noexcept override {
    return *device_.ptr();
  }
  ID3D12CommandQueue *SwapChainQueue() noexcept override {
    return this;
  }
  bool PrepareSwapChainResize(
      const std::vector<Com<ID3D12Resource>> &backbuffers) override;

  void SubmitPresent(D3D12PresentSubmission submission,
                     uint64_t frame_id) override {
    PendingOperation operation(
        QueueWorkSubmission{std::move(submission)}, frame_id);
    EnqueuePendingOperation(std::move(operation), nullptr);
  }

  void NotifyPresentBoundary() noexcept override {
    dxmt::invokeNoexcept("D3D12 present boundary", [this]() {
      device_->GetDXMTDevice().queue().PresentBoundary(); });
    dxmt::invokeNoexcept("D3D12 present diagnostics", []() {
      LogBindlessMirrorDiagSummary("present"); });
  }

#include "d3d12_command_queue_replay_types.inc"
private:
#include "d3d12_command_queue_pass_batching.inc"
private:
#include "d3d12_command_queue_query_resolve.inc"
private:
#include "d3d12_command_queue_pass_queue.inc"
private:

#include "d3d12_command_queue_execute.inc"

#include "d3d12_command_queue_replay_records.inc"
private:
#include "d3d12_command_queue_replay_state_ops.inc"
private:
#include "d3d12_command_queue_indirect.inc"
private:
#include "d3d12_command_queue_descriptor_binding.inc"
private:

#include "d3d12_command_queue_bindless_mirror.inc"
private:
#include "d3d12_command_queue_binding_plans.inc"
private:
#include "d3d12_command_queue_native_binding.inc"
private:
#include "d3d12_command_queue_binding_snapshot.inc"
private:

#include "d3d12_command_queue_compiled_encode.inc"
private:
#include "d3d12_command_queue_render_state.inc"
private:
#include "d3d12_command_queue_debug_dump.inc"
private:
#include "d3d12_command_queue_draw_encode.inc"
private:
#include "d3d12_command_queue_copy_clear.inc"
private:
#include "d3d12_command_queue_queue_work.inc"
private:
#include "d3d12_command_queue_submission.inc"
private:
  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  D3D12_COMMAND_QUEUE_DESC desc_ = {};
  std::atomic<UINT64> submitted_batches_{0};
  std::atomic<UINT64> signal_count_{0};
  std::atomic<UINT64> last_signal_value_{0};
  std::atomic<uint64_t> lifecycle_sequence_{1};
  std::atomic<bool> has_waited_{false};
  uint64_t current_timestamp_sample_sequence_ = ~0ull;
  uint64_t current_timestamp_sample_count_ = 0;
  std::vector<CachedTemporalScaler> temporal_scaler_cache_;
  std::shared_ptr<D3D12DeviceQueueState> device_queue_state_;
  // D3D12 barriers are queue-local ordering tokens. Keep trailing barriers
  // across ExecuteCommandLists calls and consume them only when this queue
  // encodes a real render, compute, or blit pass.
  ResourceAccessBarrierBatch pending_queue_resource_barriers_;
  // Immutable binding-plan caches are keyed by monotonic root-signature and
  // pipeline identities, making cross-Execute reuse safe from pointer ABA.
  std::unordered_map<uint64_t, BindingPlan> binding_plan_cache_;
  SubmissionStagePlanCache<BindlessMirrorStagePlan> bindless_stage_plan_cache_;
  SubmissionStagePlanCache<NativeRootBaseStagePlan> native_stage_plan_cache_;
  std::deque<PendingOperation> pending_operations_ DXMT_GUARDED_BY(mutex_);
  // Replay runs on the submission worker, not on the Present thread. Keep its
  // performance ledger worker-owned and flush one aggregate per frame so the
  // Present ring can be reset without racing replay writes.
  ReplayPerfFrameAccumulator replay_perf_frame_;
  dxmt::mutex mutex_;
  bool submission_worker_active_ DXMT_GUARDED_BY(mutex_) = false;
  bool submission_worker_waiting_for_wait_ DXMT_GUARDED_BY(mutex_) = false;
  bool submission_worker_stopping_ DXMT_GUARDED_BY(mutex_) = false;
  bool submission_service_stopped_ DXMT_GUARDED_BY(mutex_) = false;
  std::atomic<uint64_t> submission_worker_dependency_pair_{0};
  std::atomic<uint64_t> submission_worker_dependency_value_{0};
  std::shared_ptr<SubmissionWakeState> submission_wake_state_;
  std::weak_ptr<SubmissionQueueEndpoint> submission_endpoint_;
  std::string name_;
};

bool CommandQueueImpl::PrepareSwapChainResize(
    const std::vector<Com<ID3D12Resource>> &backbuffers) {
  auto result = std::make_shared<SwapChainResizeResult>();
  PendingOperation operation(QueueWorkSubmission{
      SwapChainResizeQueueWork{
          .backbuffers = backbuffers,
          .result = result,
      }});
  EnqueuePendingOperation(std::move(operation), nullptr);
  FlushPendingOperations();
  return result->ready.load(std::memory_order_acquire);
}



bool SubmitDxmtQueueWork(
    IMTLD3D12Device *device,
    std::unique_ptr<DxmtQueueSubmissionTarget> target,
    uint64_t &submitted_sequence) noexcept {
  if (!device || !target)
    return false;
  auto states = AcquireD3D12DeviceQueueState(*device);
  bool submitted = false;
  const bool invoked = dxmt::invokeNoexcept(
      "D3D12 typed device queue submission",
      [&]() {
        submitted = states->SubmissionService()->SubmitDeviceWork(
            *device, std::move(target), submitted_sequence);
      });
  return invoked && submitted;
}

HRESULT
CreateCommandQueue(IMTLD3D12Device *device, const D3D12_COMMAND_QUEUE_DESC *desc,
                   REFIID riid, void **command_queue) {
  InitReturnPtr(command_queue);
  if (!device || !command_queue)
    return WARN_E_INVALIDARG(__func__);

  D3D12_COMMAND_QUEUE_DESC normalized = {};
  HRESULT hr = NormalizeQueueDesc(desc, normalized);
  if (FAILED(hr))
    return hr;

  auto queue = Com<ID3D12CommandQueue>::transfer(
      new CommandQueueImpl(device, normalized,
                           AcquireD3D12DeviceQueueState(*device)));
  return queue->QueryInterface(riid, command_queue);
}

} // namespace dxmt::d3d12
