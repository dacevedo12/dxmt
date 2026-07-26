// POSITIVE control for the runtime-compilation probe.
//
// Same descriptor-heap access as metal_handle_reinterpret_rejected.metal, but
// with the texture handle kept as a typed member of the descriptor struct.
// The spec compiles this source through the same runtime MSL front end and the
// same MTLCompileOptions as the negative fixture, so a failure of the negative
// case cannot be blamed on the compile options, the language version, or the
// runtime-compilation path itself.
#include <metal_stdlib>

using namespace metal;

struct DescriptorEntry {
  uint64_t gpu_address;
  texture2d<float> texture;
  uint64_t metadata;
};

kernel void structured_texture_handle(device const DescriptorEntry *heap
                                      [[buffer(0)]],
                                      device float4 *texels [[buffer(1)]],
                                      constant uint &slot [[buffer(2)]],
                                      uint tid [[thread_position_in_grid]]) {
  texels[tid] = heap[slot].texture.read(uint2(0, 0));
}
