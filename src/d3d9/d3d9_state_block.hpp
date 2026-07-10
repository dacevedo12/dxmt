#pragma once

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "d3d9_device.hpp" // for D3D9_MAX_TEXTURE_UNITS

#include <vector>

namespace dxmt {

class MTLD3D9Device;
class MTLD3D9Surface;
class MTLD3D9CommonTexture;
class MTLD3D9VertexBuffer;
class MTLD3D9IndexBuffer;
class MTLD3D9VertexDeclaration;
class MTLD3D9VertexShader;
class MTLD3D9PixelShader;

// D3D9StateBlockChanges is defined in d3d9_state_block_changes.hpp (pulled in
// via d3d9_device.hpp) so the host tier can pin its subset membership; the
// device's recording arms mark bits on the block's instance directly.

// IDirect3DStateBlock9 snapshots device state and restores via Apply,
// covering ~30 categories (wined3d stateblock.c semantics).
// Reference-pinned slots (textures, shaders, buffers) use Com<,false>
// to pin targets across their lifetime. wined3d is the reference.
class MTLD3D9StateBlock final : public ComObject<IDirect3DStateBlock9> {
public:
  MTLD3D9StateBlock(MTLD3D9Device *device, D3DSTATEBLOCKTYPE type);
  ~MTLD3D9StateBlock();

  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
  HRESULT STDMETHODCALLTYPE Capture() override;
  HRESULT STDMETHODCALLTYPE Apply() override;

  // Replace the block's changed mask wholesale. CreateStateBlock uses
  // this to mark the D3DSBT_* category subset before its immediate
  // Capture; BeginStateBlock uses it to seed-capture everything and
  // then clear the mask so recording marks exactly the touched bits.
  void
  setChanges(const D3D9StateBlockChanges &changes) {
    m_changes = changes;
  }

  // Freeze the captured stream offset against further Captures (the wined3d
  // store_stream_offset quirk). CreateStateBlock calls this after the initial
  // capture so a re-Capture updates the bound buffer and stride but not the
  // offset.
  void
  freezeStreamOffset() {
    m_changes.store_stream_offset = false;
  }

  // Count this block in the device's losable-resource gate from the
  // moment it is handed to the app: a non-Ex Reset with a live state
  // block fails INVALIDCALL, the gate DXVK applies to match native
  // D3D9. Called by CreateStateBlock and EndStateBlock; the pub-to-zero
  // Release uncounts.
  void markLosable();

  // Seed the block's light-index set from the device's current lights
  // (wined3d stateblock_init_lights). CreateStateBlock calls this for the
  // predefined block types that track lights, before the initial Capture;
  // a later Capture then refreshes only these recorded indices. Recorded
  // blocks skip it and grow the set as SetLight / LightEnable fire.
  void seedLightsFromDevice();

private:
  // The device's Set* recording arms write straight into the snapshot
  // storage + changed mask of the in-progress recording block (wine
  // d3d9 repoints device->update_state at the recording stateblock the
  // same way; DXVK routes via m_recorder->Set*). Mirror of the
  // friendship Capture / Apply already use in the other direction.
  friend class MTLD3D9Device;
  MTLD3D9Device *m_device;
  D3DSTATEBLOCKTYPE m_type;
  // Per-category snapshots. Each Capture copies the device's live
  // values into these arrays; each Apply copies them back. Sized to
  // match the device's storage exactly (see d3d9_device.hpp's
  // m_renderStates).
  //
  // Full coverage of D3D9 state-block categories, with per-state-bit masking:
  // Capture and Apply touch only the states changed between Begin/End, tracked
  // in the per-render-state changed mask below (the same bookkeeping applies to
  // every category).
  DWORD m_snapRenderStates[256] = {};
  // Per-category + per-render-state changed mask. CreateStateBlock
  // (D3DSBT_ALL) sets every bit via markAll(); EndStateBlock-recorded
  // blocks only set the bits for states actually touched between
  // Begin/End. Apply restores only the marked entries: wined3d
  // dlls/wined3d/stateblock.c walks num_contained_render_states with
  // the same bit logic. dxmt's previous unconditional memcpy would
  // stomp ALPHABLENDENABLE / ZENABLE / ZWRITEENABLE etc. that the app
  // had explicitly mutated since the snapshot was taken.
  D3D9StateBlockChanges m_changes;
  // Sampler state snapshot. Same shape as the device's
  // m_samplerStates: 20 stages × 14 sampler-state types. Indexed by
  // D3DSAMPLERSTATETYPE 1..D3DSAMP_DMAPOFFSET (slot 0 unused).
  DWORD m_snapSamplerStates[D3D9_MAX_TEXTURE_UNITS][D3DSAMP_DMAPOFFSET + 1] = {};
  // FFP texture-stage state snapshot: same shape as the device's
  // m_textureStageStates: 8 stages × D3DTSS_CONSTANT+1 entries.
  DWORD m_snapTextureStageStates[8][D3DTSS_CONSTANT + 1] = {};
  // Transform snapshot: D3DTS_VIEW/PROJECTION/WORLD/TEXTURE0..7
  // plus 256 world-matrix slots, mirroring the device's m_transforms.
  // Sized via the device's kMaxTransforms constant; the static_assert
  // in Capture catches drift.
  D3DMATRIX m_snapTransforms[10 + 256] = {};
  // User clip planes: 8 planes × 4 floats. Mirrors m_clipPlanes.
  float m_snapClipPlanes[8][4] = {};
  // Viewport + scissor. Both POD; bitwise copy is the right shape.
  D3DVIEWPORT9 m_snapViewport = {};
  RECT m_snapScissorRect = {};
  // FVF code (legacy fixed-function bookkeeping).
  DWORD m_snapFvf = 0;
  // Per-stream offset/stride pair. Stored alongside the buffer ref
  // (m_snapVertexBuffers below), so SetStreamSource(slot, buf, off,
  // stride) round-trips all three components together.
  UINT m_snapStreamOffsets[D3D9_MAX_VERTEX_STREAMS] = {};
  UINT m_snapStreamStrides[D3D9_MAX_VERTEX_STREAMS] = {};
  // Per-stream frequency/divider for SetStreamSourceFreq (D3D9
  // hardware instancing). Captured/applied in the same memcpy
  // shape as offsets/strides since the wined3d D3DSBT_VERTEXSTATE
  // table includes WINED3DTS_STREAM_FREQ / DIVIDER together with
  // the source-binding fields.
  UINT m_snapStreamFreq[D3D9_MAX_VERTEX_STREAMS] = {};
  // FFP material. POD; full struct copy.
  D3DMATERIAL9 m_snapMaterial = {};
  // Shader constant register files: VS + PS, F/I/B each. Sizing
  // matches the device's hardware-VP storage (D3D9_MAX_VS_CONST_F
  // etc.). Largest component of the snapshot at ~36 KB total.
  float m_snapVsConstantsF[D3D9_MAX_VS_CONST_F][4] = {};
  int m_snapVsConstantsI[D3D9_MAX_VS_CONST_I][4] = {};
  BOOL m_snapVsConstantsB[D3D9_MAX_VS_CONST_B] = {};
  float m_snapPsConstantsF[D3D9_MAX_PS_CONST_F][4] = {};
  int m_snapPsConstantsI[D3D9_MAX_PS_CONST_I][4] = {};
  BOOL m_snapPsConstantsB[D3D9_MAX_PS_CONST_B] = {};
  // F-constant coverage marks travelling with the register files.
  // Apply raises (never lowers) the device's sticky upload-clamp
  // marks to these, so registers written only through a recorded or
  // captured block still reach the encode-side upload memcpy.
  uint16_t m_snapVsConstFMax = 0;
  uint16_t m_snapPsConstFMax = 0;
  // Reference-pinned slots. Each Com<,false> assignment runs
  // AddRefPrivate on the new target before ReleasePrivate on the old;
  // safe even when the snap and live slot point at the same object.
  // The block holds private refs to every captured target for its
  // lifetime, so an app that releases its last public ref to a bound
  // resource between Capture and Apply still gets the resource back.
  //
  // No RT[]/DS slots: wined3d's stateblock doesn't track framebuffer
  // bindings (stateblock.c) and Apply doesn't restore them.
  Com<MTLD3D9CommonTexture, false> m_snapTextures[D3D9_MAX_TEXTURE_UNITS];
  Com<MTLD3D9VertexBuffer, false> m_snapVertexBuffers[D3D9_MAX_VERTEX_STREAMS];
  Com<MTLD3D9IndexBuffer, false> m_snapIndexBuffer;
  Com<MTLD3D9VertexDeclaration, false> m_snapVertexDeclaration;
  Com<MTLD3D9VertexShader, false> m_snapVertexShader;
  Com<MTLD3D9PixelShader, false> m_snapPixelShader;
  // FFP lights + per-index enable flags. std::vector copy-assignment
  // handles allocation and the trivially-copyable element layout.
  std::vector<D3DLIGHT9> m_snapLights;
  std::vector<BOOL> m_snapLightEnables;
  // The light indices this block actually restores (wined3d's
  // changed.changed_lights list). A predefined block records every existing
  // light at capture; a Begin/End-recorded block records only the indices its
  // SetLight / LightEnable touched, so Apply leaves the others alone.
  std::vector<DWORD> m_snapLightIndices;
  // Self-pin shape mirrors MTLD3D9Surface / MTLD3D9Texture: the
  // ctor's AddRefPrivate keeps the block alive across the public 1→0
  // transition long enough for the override to drop the device pin.
  // Released exactly once on the first pub→0 transition.
  bool m_self_pinned = true;
  // Losable-resource accounting; see markLosable.
  bool m_isLosable = false;
};

} // namespace dxmt
