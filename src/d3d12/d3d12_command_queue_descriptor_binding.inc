// Descriptor resolution, pipeline descriptor access, and binding plans.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  // ShaderDigestForStage (and the process-wide digest memo behind it),
  // D3D12DiagPipelineStageSelected and DebugLogRootBinding now live in
  // d3d12_binding_debug_log.hpp.

  // BindRootConstants now lives in d3d12_root_parameter_apply.hpp, together
  // with ApplyRootDescriptorTables, its only caller.

  // GetTableHandle, DescriptorRangeOffset, GetBoundDescriptorRecordFromHeap,
  // GetBoundDescriptorHeap, GetBoundDescriptorRecordInRangeFromHeap and
  // GetBoundDescriptorRecordInRangeFromLockedHeap now live in
  // d3d12_descriptor_table_access.hpp; their ReplayState-resolving wrappers
  // GetBoundDescriptorRecord and GetBoundDescriptorRecordInRange in
  // d3d12_bound_descriptor_lookup.hpp.

  // BindDescriptor itself now lives in d3d12_legacy_binding_encode.hpp,
  // together with ClearDescriptorBinding; this shim only supplies the Metal
  // device.
  void BindDescriptor(ArgumentEncodingContext &enc, PipelineStage stage,
                      D3D12_DESCRIPTOR_RANGE_TYPE range_type, UINT slot,
                      const DescriptorRecord &descriptor,
                      const DXMT12_MTL4_SHADER_ARGUMENT *argument) {
    dxmt::d3d12::BindDescriptor(device_->GetMTLDevice(), enc, stage, range_type,
                                slot, descriptor, argument);
  }

  // DescriptorRangeTypeName and ShaderReadStateForStage now live in
  // d3d12_descriptor_table_access.hpp.

  void RecordDescriptorResourceAccess(CommandChunk *chunk, ReplayState &state, PipelineStage stage,
                                      D3D12_DESCRIPTOR_RANGE_TYPE range_type,
                                      const DescriptorRecord &descriptor,
                                      const char *context) {
    const auto expected_type = ExpectedDescriptorTypeForRange(range_type);
    if (expected_type != DescriptorRecordType::Empty &&
        descriptor.type != expected_type) {
      if (descriptor.type != DescriptorRecordType::Empty) {
        static std::atomic<uint32_t> log_count = 0;
        if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
          WARN("D3D12CommandQueue: descriptor table type mismatch;"
               " skipping resource-state tracking context=",
               context,
               " rangeType=", DescriptorRangeTypeName(range_type),
               " descriptorType=", DescriptorRecordTypeName(descriptor.type),
               " expectedType=", DescriptorRecordTypeName(expected_type),
               " resource=", descriptor.resource.ptr());
        }
      }
      return;
    }

    switch (range_type) {
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: {
      Resource *resource = nullptr;
      if (descriptor.has_desc)
        ResolveBufferGpuAddress(descriptor.desc.cbv.BufferLocation, resource);
      if (!resource)
        resource = GetResource(descriptor.resource.ptr());
      if (resource)
        RecordReplayResourceAccess(
            chunk, state, resource->GetD3D12Resource(),
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, context);
      break;
      }
      case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
        if (auto *resource = GetResource(descriptor.resource.ptr())) {
          RecordDescriptorResourceAccessRanges(
              chunk, state, *resource, ShaderReadStateForStage(stage), descriptor,
              context, GetShaderResourceSubresourceRanges);
        }
        break;
      case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
        if (auto *resource = GetResource(descriptor.resource.ptr())) {
          RecordDescriptorResourceAccessRanges(
              chunk, state, *resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              descriptor, context, GetUnorderedAccessSubresourceRanges);
        }
        RecordReplayResourceAccess(chunk, state, descriptor.counter_resource.ptr(),
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   "UAV counter");
      break;
    case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
      break;
    }
  }

  void RecordRootBufferDescriptorAccess(
      CommandChunk *chunk, ReplayState &state, PipelineStage stage, bool compute, UINT root_index,
      const RootSignatureParameter &parameter, DescriptorRecordType type) {
    const auto &map =
        type == DescriptorRecordType::ConstantBufferView
            ? (compute ? state.compute_cbv_roots : state.graphics_cbv_roots)
            : type == DescriptorRecordType::ShaderResourceView
                  ? (compute ? state.compute_srv_roots
                             : state.graphics_srv_roots)
                  : (compute ? state.compute_uav_roots
                             : state.graphics_uav_roots);
    if (root_index >= ReplayState::kMaxRootParameters)
      return;
    const auto &slot = map[root_index];
    if (!slot.valid)
      return;

    Resource *resource = nullptr;
    ResolveBufferGpuAddress(slot.address, resource);
    if (!resource)
      return;

    D3D12_RESOURCE_STATES desired;
    const char *context;
    if (type == DescriptorRecordType::ConstantBufferView) {
      desired = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
      context = "root CBV";
    } else if (type == DescriptorRecordType::ShaderResourceView) {
      desired = ShaderReadStateForStage(stage);
      context = "root SRV";
    } else {
      desired = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      context = "root UAV";
    }

    (void)parameter;
    RecordReplayResourceAccess(chunk, state, resource->GetD3D12Resource(), desired,
                               context);
  }

  // BindingEntryKind, BindingPlanEntry, BindingPlan and BuildBindingPlan now
  // live in d3d12_stage_plan_build.hpp; kSubmissionPlanCacheLimit and
  // HashSubmissionPlanIdentity in d3d12_descriptor_table_access.hpp.

  // P1: per-draw cached path. Plan lookup builds the invariant traversal
  // structure on miss, then each draw resolves only current heaps, table bases
  // and descriptor records before recording resource access.
  // GetRootBufferAddressForFingerprint now lives in
  // d3d12_bound_descriptor_lookup.hpp.

  void RecordPipelineDescriptorAccess_Cached(CommandChunk *chunk,
                                             ReplayState &state,
                                             PipelineState &pipeline,
                                             bool compute) {
    auto *root = compute ? state.compute_root_signature_impl
                         : state.graphics_root_signature_impl;
    if (!root)
      return;
    const auto root_identity = root->GetCacheIdentity();
    const auto pipeline_identity = pipeline.GetCacheIdentity();
    const uint64_t key = HashSubmissionPlanIdentity(
        root_identity, pipeline_identity,
        compute ? PipelineStage::Compute : PipelineStage::Vertex, compute);
    auto it = binding_plan_cache_.find(key);
    if (it == binding_plan_cache_.end() ||
        it->second.root_identity != root_identity ||
        it->second.pipeline_identity != pipeline_identity ||
        it->second.compute != compute) {
      if (it == binding_plan_cache_.end() &&
          binding_plan_cache_.size() >= kSubmissionPlanCacheLimit)
        binding_plan_cache_.erase(binding_plan_cache_.begin());
      it = binding_plan_cache_
               .insert_or_assign(key, BuildBindingPlan(root, pipeline, compute))
               .first;
    }
    const BindingPlan &plan = it->second;

    // heap re-resolution once per draw (mirrors _Live :8528-8541)
    const auto resolve_heap =
        [&](D3D12_DESCRIPTOR_HEAP_TYPE t) -> DescriptorHeap * {
      const auto &heap = t == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                             ? state.sampler_heap
                             : state.cbv_srv_uav_heap;
      return dynamic_cast<DescriptorHeap *>(heap.ptr());
    };
    auto *cbv_heap = resolve_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto *smp_heap = resolve_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    const auto parameters = root->GetParameters();
    const bool cache_on = DescAccessCacheEnabled() && !compute;
    auto *resource_mirror = cbv_heap ? cbv_heap->GetMirror() : nullptr;
    // HashGraphicsBindingFull() already folds in the PSO and root-signature
    // *pointers*, but the journal cache stores indices into `plan.entries`,
    // and `plan` is keyed on the monotonic cache identities instead. A PSO
    // that is destroyed and reallocated at the same address keeps the binding
    // identity while selecting a different plan, so mix the plan identity in
    // as well: `entries_by_slot` must only ever be replayed against the plan
    // it was built from. A mismatch now misses the fast path (correct, just
    // slower) instead of indexing a semantically different entry.
    uint64_t binding_identity = 0;
    if (cache_on) {
      const auto mix = [](uint64_t h, uint64_t v) {
        return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
      };
      binding_identity = mix(mix(HashGraphicsBindingFull(state),
                                 plan.root_identity),
                             plan.pipeline_identity);
    }

    // Fast path for persistent bindless heaps. Table/root identity is cheap to
    // hash and descriptor contents are tracked by the per-heap journal. When
    // both the binding identity and resource-state epoch are stable, only the
    // slots written since the preceding draw can introduce new hazards.
    if (cache_on && !DescAccessVerify()) {
      auto &journal_cache = state.descriptor_journal_access_cache;
      if (journal_cache.valid &&
          journal_cache.binding_identity == binding_identity &&
          journal_cache.access_epoch == state.access_epoch &&
          journal_cache.mirror == resource_mirror) {
        const auto changes = resource_mirror
                                 ? resource_mirror->changesSince(
                                       journal_cache.cursor)
                                 : DescriptorChangeSet{};
        if (changes.complete) {
          if (changes.changes.empty()) {
            if (ReplayPerfEnabled())
              perDrawSubTimers().descAccessHits++;
            return;
          }

          for (const auto &change : changes.changes) {
            const auto mapped =
                journal_cache.entries_by_slot.find(change.slot);
            if (mapped == journal_cache.entries_by_slot.end())
              continue;
            for (const auto entry_index : mapped->second) {
              if (entry_index >= plan.entries.size())
                continue;
              const auto &entry = plan.entries[entry_index];
              const auto base =
                  GetTableHandle(state, compute, entry.root_index);
              if (!base.ptr)
                continue;
              const auto descriptor = GetBoundDescriptorRecordInRangeFromHeap(
                  cbv_heap, base, entry.range_offset, entry.descriptor_index,
                  entry.count, entry.heap_type);
              if (!descriptor)
                continue;
              RecordDescriptorResourceAccess(
                  chunk, state, entry.stage, entry.range_type, *descriptor,
                  DescriptorRangeTypeName(entry.range_type));
            }
          }
          journal_cache.cursor = changes.cursor;
          journal_cache.access_epoch = state.access_epoch;
          if (ReplayPerfEnabled())
            perDrawSubTimers().descAccessHits++;
          return;
        }
      }
    }

    // descAccess content-fingerprint cache: read each binding's resolved CONTENT
    // (resource pointer + type) — this sees through ring-buffer handle churn
    // (same resources at new handles -> same fp). If (PSO, root-sig, content fp,
    // access_epoch) matches the previous draw, the per-entry hazard records are
    // identical and the resource state is frozen, so the whole hazard pass is
    // skipped. Resolve (cheap pointer arithmetic) is kept; the heavy get-ranges
    // + RecordReplayResourceAccess hazard work is what's elided. Touch is NOT
    // needed on a hit: the first miss with this fp already Touched the same
    // resources into the per-list (accumulated) touched set.
    uint64_t fp = 0;
    std::unordered_map<uint32_t, std::vector<uint32_t>> entries_by_slot;
    if (cache_on) {
      fp = HashDescriptorAccessBindingFingerprint(state, compute, root,
                                                  &pipeline, plan, cbv_heap,
                                                  smp_heap, entries_by_slot);
      auto cit = state.da_cache.find(fp);
      const bool hit =
          cit != state.da_cache.end() && cit->second == state.access_epoch;
      if (hit && !DescAccessVerify()) {
        auto &journal_cache = state.descriptor_journal_access_cache;
        journal_cache.binding_identity = binding_identity;
        journal_cache.access_epoch = state.access_epoch;
        journal_cache.mirror = resource_mirror;
        journal_cache.cursor =
            resource_mirror ? resource_mirror->changeJournalCursor() : 0;
        journal_cache.entries_by_slot = std::move(entries_by_slot);
        journal_cache.valid = true;
        if (ReplayPerfEnabled())
          perDrawSubTimers().descAccessHits++;
        return; // identical bindings + frozen state -> hazard result unchanged
      }
      if (ReplayPerfEnabled())
        perDrawSubTimers().descAccessMiss++;
    }

    for (const auto &e : plan.entries) {
      if (e.kind == BindingEntryKind::Table) {
        const auto base = GetTableHandle(state, compute, e.root_index);
        if (!base.ptr)
          continue;
        auto *dh = e.heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? smp_heap
                                                                    : cbv_heap;
        const auto descriptor = GetBoundDescriptorRecordInRangeFromHeap(
            dh, base, e.range_offset, e.descriptor_index, e.count, e.heap_type);
        if (!descriptor)
          continue;
        RecordDescriptorResourceAccess(chunk, state, e.stage, e.range_type,
                                       *descriptor,
                                       DescriptorRangeTypeName(e.range_type));
      } else {
        // `parameters` is a std::span, whose operator[] is unchecked, and
        // `e.root_index` comes from a cached plan. BuildBindingPlan only ever
        // derives root_index from iterating this same span, so a plan whose
        // root_identity matches cannot exceed it -- but that invariant is
        // spread across three files and the plan outlives the draw, so verify
        // it here rather than let a stale plan read past the parameter list.
        if (e.root_index >= parameters.size()) {
          static std::atomic<uint32_t> log_count = 0;
          if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
            WARN("D3D12CommandQueue: binding plan root parameter index out of"
                 " range; skipping resource-state tracking rootIndex=",
                 e.root_index, " parameterCount=", parameters.size());
          }
          continue;
        }
        RecordRootBufferDescriptorAccess(chunk, state, e.stage, compute,
                                         e.root_index,
                                         parameters[e.root_index], e.buffer_type);
      }
    }
    if (cache_on) {
      state.da_cache[fp] = state.access_epoch; // fp -> epoch after the hazard pass
      auto &journal_cache = state.descriptor_journal_access_cache;
      journal_cache.binding_identity = binding_identity;
      journal_cache.access_epoch = state.access_epoch;
      journal_cache.mirror = resource_mirror;
      journal_cache.cursor =
          resource_mirror ? resource_mirror->changeJournalCursor() : 0;
      journal_cache.entries_by_slot = std::move(entries_by_slot);
      journal_cache.valid = true;
    }
  }

  void RecordPipelineDescriptorAccess(CommandChunk *chunk, ReplayState &state,
                                      PipelineState &pipeline, bool compute) {
    RecordPipelineDescriptorAccess_Cached(chunk, state, pipeline, compute);
  }

  // DescriptorRecordSizeBytes now lives in d3d12_descriptor_table_access.hpp.
  // SelectedDescriptorConsistency, AddSelectedDescriptorReason and
  // DiagnoseSelectedDescriptor now live in
  // d3d12_selected_descriptor_diag.hpp.
  // DebugLogRootBinding now lives in d3d12_binding_debug_log.hpp.
