// NEGATIVE fixture -- this file is expected to FAIL to compile.
//
// It is deliberately *not* part of the offline `metal` compilation chain in
// tests/metal/meson.build.  It is embedded as source text and handed to
// `-[MTLDevice newLibraryWithSource:options:error:]` at runtime, where the
// spec asserts that the MSL front end rejects it.
//
// Why it matters: reinterpreting a raw 64-bit descriptor word as a texture
// handle with `as_type<>` is how some other D3D-on-Metal translation layers
// address their descriptor heaps.  MSL does not accept it, so DXMT has to keep
// the handle typed inside the descriptor struct instead.  This fixture is the
// regression guard against someone reintroducing the bit-cast form.
#include <metal_stdlib>

using namespace metal;

kernel void reinterpret_texture_handle(device const uint64_t *heap [[buffer(0)]],
                                       device float4 *texels [[buffer(1)]],
                                       constant uint &slot [[buffer(2)]],
                                       uint tid [[thread_position_in_grid]]) {
  texture2d<float> texture = as_type<texture2d<float>>(heap[slot * 3 + 1]);
  texels[tid] = texture.read(uint2(0, 0));
}
