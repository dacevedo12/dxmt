// Host-side Metal capability probe kernels.
//
// These kernels encode the descriptor-heap shape DXMT wants to migrate onto:
// an *unbounded* `device` array of 24-byte array-of-structures entries whose
// middle qword is an `MTLResourceID`, indexed by a value that is only known at
// runtime.  The static asserts below are the compile-time half of the probe:
// if a future toolchain changes the size of a texture or sampler handle, or
// the packing rules for a struct that mixes 64-bit scalars with resource
// handles, this fixture stops compiling instead of silently changing the ABI.
#include <metal_stdlib>

using namespace metal;

struct DescriptorEntry {
  uint64_t gpu_address;
  texture2d<float> texture;
  uint64_t metadata;
};

struct SamplerEntry {
  sampler state;
  uint64_t reserved;
  uint64_t metadata;
};

static_assert(sizeof(DescriptorEntry) == 24,
              "texture descriptor entry must stay 24 bytes");
static_assert(sizeof(SamplerEntry) == 24,
              "sampler descriptor entry must stay 24 bytes");
static_assert(alignof(DescriptorEntry) == 8,
              "texture descriptor entry must stay 8-byte aligned");
static_assert(alignof(SamplerEntry) == 8,
              "sampler descriptor entry must stay 8-byte aligned");
static_assert(sizeof(texture2d<float>) == 8, "texture handle must stay 8 bytes");
static_assert(sizeof(sampler) == 8, "sampler handle must stay 8 bytes");

// Reads one texel out of the texture referenced by a runtime-selected heap
// slot, and mirrors the two scalar fields of the same entry so the host can
// prove the 24-byte AoS layout at runtime as well as at compile time.
kernel void probe_texture_heap(device const DescriptorEntry *heap [[buffer(0)]],
                               device const uint *slots [[buffer(1)]],
                               device float4 *texels [[buffer(2)]],
                               device uint2 *scalars [[buffer(3)]],
                               uint tid [[thread_position_in_grid]]) {
  const uint slot = slots[tid];
  texels[tid] = heap[slot].texture.read(uint2(0, 0));
  scalars[tid] = uint2(static_cast<uint>(heap[slot].gpu_address),
                       static_cast<uint>(heap[slot].metadata));
}

// Samples through a runtime-selected slot of an unbounded sampler heap.  The
// sample coordinate is supplied by the host: the address-mode discriminator
// only works when the coordinate is *outside* the 1x1 texture, and the host
// deliberately exercises both an in-bounds and an out-of-bounds coordinate.
kernel void probe_sampler_heap(device const DescriptorEntry *texture_heap
                               [[buffer(0)]],
                               device const SamplerEntry *sampler_heap
                               [[buffer(1)]],
                               device const uint *texture_slots [[buffer(2)]],
                               device const uint *sampler_slots [[buffer(3)]],
                               device float4 *texels [[buffer(4)]],
                               constant float2 &coordinate [[buffer(5)]],
                               uint tid [[thread_position_in_grid]]) {
  texels[tid] = texture_heap[texture_slots[tid]].texture.sample(
      sampler_heap[sampler_slots[tid]].state, coordinate);
}

// Control group: the same sampling, with the texture and the sampler bound
// directly instead of reached through a heap entry.
kernel void probe_direct_binding(texture2d<float> texture [[texture(0)]],
                                 sampler state [[sampler(0)]],
                                 device float4 *texels [[buffer(0)]],
                                 constant float2 &coordinate [[buffer(1)]],
                                 uint tid [[thread_position_in_grid]]) {
  texels[tid] = texture.sample(state, coordinate);
}
