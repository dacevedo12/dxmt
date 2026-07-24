/*
 * Redirect to the mingw-w64 SDK's d3d12.h. shader_runner.c and utils.h pull
 * in the D3D12 types for the (stubbed) d3d12 executor and for a handful of
 * enums the d3d9 executor reuses (D3D12_TEXTURE_ADDRESS_MODE, DXGI formats).
 * The d3d12 executor itself never runs in this build (see local/d3d9_only_stubs.c).
 */
#include <d3d12.h>
