#pragma once

#include "airconv_public.h"
#include <cstdint>

/**
 * Bindless-mirror (Stage-1) HYBRID-ABI per-draw BUFFER table (Metal slot 27).
 *
 * Buffer half of the hybrid ABI (slot 27). Texture/sampler compact windows and
 * root_offsets (slots 28-30) are frozen at capture/submit time following the
 * D3DMetal materialize-early / bind-late model; encode only uploads those
 * frozen tables. Slot 27 still packs buffer descriptors each draw because
 * suballocation gpuAddress can churn.
 *
 * TODO(d3d12, bindless): Replace the remaining per-draw buffer-table pack with
 * a descriptor-write-time / Metal4 argument-table path once buffer descriptor
 * lifetime and suballocation address stability are modeled explicitly.
 *
 * See fh4-debug/BINDLESS-ABI.md §4.5. Under the hybrid ABI, TEXTURES and SAMPLERS
 * live in the persistent slot-30 mirror, but BUFFER descriptors (CBV + SRV/UAV
 * buffer + UAV counter) do NOT: their gpuAddress = allocation base +
 * currentSuballocationOffset() churns every draw under FH4's ring-buffer
 * per-draw sub-allocation, so they are packed FRESH each draw into a small
 * `uint64 buf_table[]` bound at slot 27.
 *
 * The shader (airconv `--bindless-mirror`) reads a buffer field as:
 *     buf_table[compact_base + (operand_register - range.LowerBound) * 3]
 * where `compact_base` is baked at COMPILE time by walking the shader reflection in
 * its native order and counting only buffer fields. The runtime MUST fill buf_table
 * by the IDENTICAL walk so compact_base matches without any runtime communication.
 *
 * THE ORDERING CONTRACT (must match airconv `bindless_buffer_table_qword_count` /
 * `setup_binding_table`, dxbc_converter.cpp): walk in native reflection order
 *   1. CBVs   — args_reflection_cbuffer (== constantBufferInfo(), the first
 *               NumConstantBuffers entries of argument_info), all are buffers.
 *   2. SRVs   — args_reflection (== resourceArgumentInfo()) SRV entries that are
 *               buffers (Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER).
 *   3. UAVs   — args_reflection UAV entries that are buffers.
 * (Samplers and texture SRV/UAV fields are skipped — they are not buffers.) For each
 * buffer field, assign compact_base = running_qword_total, then advance by
 * range_capacity*3 qw, where range_capacity = (RegisterCount == UINT_MAX ?
 * kBindlessMirrorCapacity : RegisterCount). Every descriptor reserves qw2 for an
 * optional UAV counter, so adjacent records never alias.
 *
 * The per-slot payload bytes are byte-identical to what
 * ArgumentEncodingContext::encodeShaderResources / encodeConstantBuffers write today
 * (dxmt_context.cpp:2092 SRV-buf, :2169 UAV-buf, :2220 counter; CBV path). The only
 * change is WHERE: a per-draw densely-packed buf_table instead of the per-shader
 * packed argbuf at StructurePtrOffset.
 */

namespace dxmt {

/**
 * Mirror/array GEP-typing capacity for an UNBOUNDED descriptor range (range.size ==
 * UINT_MAX). The shared public ABI constants keep airconv and the runtime's compact
 * bases in lockstep without pulling the heavy converter internals into the runtime.
 */
static constexpr uint32_t kBindlessMirrorCapacity =
    MTL_BINDLESS_MIRROR_CAPACITY;

/** qword stride: qw0=address, qw1=metadata, qw2=optional UAV counter. */
static constexpr uint32_t kBufferTableQwordsPerDescriptor =
    MTL_BINDLESS_BUFFER_DESCRIPTOR_QWORDS;

/**
 * Number of buf_table qwords reserved for a reflection range whose D3D12 register
 * count is `register_count` (== MTL_SM50_SHADER_ARGUMENT::RegisterCount, which the
 * airconv emission set to `range.size ? range.size : 1`; an unbounded range yields
 * UINT_MAX). Mirrors airconv `descriptor_range_qword_count`.
 */
inline uint32_t
BufferTableRangeQwordCount(uint32_t register_count) {
  const uint32_t range_size = register_count ? register_count : 1;
  const uint32_t capacity =
      (range_size == UINT32_MAX) ? kBindlessMirrorCapacity : range_size;
  return capacity * kBufferTableQwordsPerDescriptor;
}

/** Whether a reflected argument is a BUFFER field that goes into buf_table. */
inline bool
IsBufferTableField(const MTL_SM50_SHADER_ARGUMENT &arg) {
  return (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER) != 0;
}

/**
 * Walk a shader's reflection (CBVs then SRV/UAV resources) in native order and invoke
 * `fn(arg, compact_base)` for every BUFFER field, in the exact slot-27 fill order.
 * `cbuffers` / `num_cbuffers` is constantBufferInfo() (all CBV buffers);
 * `arguments` / `num_arguments` is resourceArgumentInfo() (samplers + SRV/UAV; only
 * the BUFFER ones are visited). Returns the total buf_table qword count.
 *
 * Both the runtime fill (③) and the compact-index baking (airconv ①) use this same
 * sequence, so the per-field `compact_base` agrees on both sides.
 */
template <typename Fn>
inline uint32_t
ForEachBufferTableField(const MTL_SM50_SHADER_ARGUMENT *cbuffers,
                        uint32_t num_cbuffers,
                        const MTL_SM50_SHADER_ARGUMENT *arguments,
                        uint32_t num_arguments, Fn &&fn) {
  uint32_t running_qword = 0;
  // 1. CBVs — every constant-buffer field is a buffer.
  for (uint32_t i = 0; i < num_cbuffers; i++) {
    const auto &arg = cbuffers[i];
    fn(arg, running_qword);
    running_qword += BufferTableRangeQwordCount(arg.RegisterCount);
  }
  // 2. SRV buffers, then 3. UAV buffers — in reflection order. args_reflection is
  // emitted samplers→SRVs→UAVs (dxbc_converter.cpp); SRV entries precede UAV entries,
  // and within each the std::map iteration is ascending range_id, so a single forward
  // pass over `arguments` visits SRV buffers then UAV buffers in the same order airconv
  // walks srvMap then uavMap. (Samplers carry neither the BUFFER flag, so are skipped.)
  for (uint32_t i = 0; i < num_arguments; i++) {
    const auto &arg = arguments[i];
    if (arg.Type != SM50BindingType::SRV)
      continue;
    if (!IsBufferTableField(arg))
      continue;
    fn(arg, running_qword);
    running_qword += BufferTableRangeQwordCount(arg.RegisterCount);
  }
  for (uint32_t i = 0; i < num_arguments; i++) {
    const auto &arg = arguments[i];
    if (arg.Type != SM50BindingType::UAV)
      continue;
    if (!IsBufferTableField(arg))
      continue;
    fn(arg, running_qword);
    running_qword += BufferTableRangeQwordCount(arg.RegisterCount);
  }
  return running_qword;
}

/** Total buf_table qword count for a shader (sum over all buffer fields). */
inline uint32_t
BufferTableQwordCount(const MTL_SM50_SHADER_ARGUMENT *cbuffers,
                      uint32_t num_cbuffers,
                      const MTL_SM50_SHADER_ARGUMENT *arguments,
                      uint32_t num_arguments) {
  return ForEachBufferTableField(
      cbuffers, num_cbuffers, arguments, num_arguments,
      [](const MTL_SM50_SHADER_ARGUMENT &, uint32_t) {});
}

/** Sentinel compact base for a non-buffer argument (sampler / texture field). */
static constexpr uint32_t kNotABufferTableField = UINT32_MAX;

/**
 * Compute the per-argument buf_table compact qword bases, by index into each reflection
 * array, using the ORDERING CONTRACT walk. `cb_bases[i]` is the qword base of CBV `i`
 * (every CBV is a buffer); `res_bases[i]` is the qword base of resource arg `i`, or
 * kNotABufferTableField if arg `i` is not a buffer (sampler / texture). Returns the total
 * buf_table qword count (== the slice size, in qwords, the runtime must allocate). The
 * per-index bases agree with airconv's compile-time compact_base because they come from
 * the same walk. `cb_bases` must have >= num_cbuffers entries and `res_bases` >=
 * num_arguments entries.
 */
inline uint32_t
BuildBufferTableCompactBases(const MTL_SM50_SHADER_ARGUMENT *cbuffers,
                             uint32_t num_cbuffers,
                             const MTL_SM50_SHADER_ARGUMENT *arguments,
                             uint32_t num_arguments, uint32_t *cb_bases,
                             uint32_t *res_bases) {
  for (uint32_t i = 0; i < num_arguments; i++)
    res_bases[i] = kNotABufferTableField;
  uint32_t running_qword = 0;
  for (uint32_t i = 0; i < num_cbuffers; i++) {
    cb_bases[i] = running_qword;
    running_qword += BufferTableRangeQwordCount(cbuffers[i].RegisterCount);
  }
  for (uint32_t i = 0; i < num_arguments; i++) {
    const auto &arg = arguments[i];
    if (arg.Type != SM50BindingType::SRV || !IsBufferTableField(arg))
      continue;
    res_bases[i] = running_qword;
    running_qword += BufferTableRangeQwordCount(arg.RegisterCount);
  }
  for (uint32_t i = 0; i < num_arguments; i++) {
    const auto &arg = arguments[i];
    if (arg.Type != SM50BindingType::UAV || !IsBufferTableField(arg))
      continue;
    res_bases[i] = running_qword;
    running_qword += BufferTableRangeQwordCount(arg.RegisterCount);
  }
  return running_qword;
}

// ---- per-slot writers (single source of truth, byte-identical to encodeShaderResources) ----

/**
 * Write a buffer descriptor's qw0/qw1 into buf_table at compact slot base `qw_base`.
 * `gpu_address_with_offset` == alloc->gpuAddress() + offset + slice.byteOffset
 * (CBV: the constant-buffer base address). `meta` == byteLength for raw/structured/
 * typed buffers (or the tbuffer-style elementCount/firstElement bits for the BUFFER
 * branch — exactly what encodeShaderResources stores in [StructurePtrOffset+1]; CBV
 * uses 0). Identical bytes to dxmt_context.cpp:2092/2169 and encodeConstantBuffers.
 */
inline void
WriteBufferTableSlot(uint64_t *buf_table, uint32_t qw_base,
                     uint64_t gpu_address_with_offset, uint64_t meta) {
  buf_table[qw_base] = gpu_address_with_offset;
  buf_table[qw_base + 1] = meta;
}

/**
 * Write a UAV-buffer counter address into the owning field's qw2 (the 3rd qword of a
 * UAV-with-counter slot). Matches dxmt_context.cpp:2220.
 */
inline void
WriteBufferTableCounter(uint64_t *buf_table, uint32_t qw_base,
                        uint64_t counter_gpu_address) {
  buf_table[qw_base + 2] = counter_gpu_address;
}

} // namespace dxmt
