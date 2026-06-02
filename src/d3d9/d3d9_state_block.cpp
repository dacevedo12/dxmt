#include "d3d9_state_block.hpp"

#include "d3d9_buffer.hpp"
#include "d3d9_common_texture.hpp"
#include "d3d9_device.hpp"
#include "d3d9_shader.hpp"
#include "d3d9_surface.hpp"
#include "d3d9_vertex_declaration.hpp"

#include <array>
#include <cstring>
#include <memory>

namespace dxmt {

MTLD3D9StateBlock::MTLD3D9StateBlock(MTLD3D9Device *device, D3DSTATEBLOCKTYPE type) : m_device(device), m_type(type) {
  // Snapshot members are zero-initialised by their `= {}` defaults in
  // d3d9_state_block.hpp. Until the first Capture lands, Apply writes
  // that zero-state: CreateStateBlock calls Capture immediately so
  // apps shouldn't see it in practice, but the init guards against UB
  // if a future code path Apply's without Capture.
  AddRefPrivate();
  // Register with the device so Reset can invalidate every
  // outstanding block. Pairs with the unregister in the dtor.
  m_device->registerStateBlock(this);
}

MTLD3D9StateBlock::~MTLD3D9StateBlock() {
  m_device->unregisterStateBlock(this);
}

ULONG STDMETHODCALLTYPE
MTLD3D9StateBlock::AddRef() {
  ULONG ref = ComObject::AddRef();
  // Pin the device. Same shape as MTLD3D9Texture::AddRef: currently
  // safe because CreateStateBlock runs post-device-ctor (the device
  // already has a non-zero pub refcount when the block is created).
  // If a future Reset / device-recreate path constructs state blocks
  // during the device's own ctor window, the first pub-AddRef here
  // will walk the device refcount through 0→1 and recursively destruct
  // it; switch to private-ref pinning if that scenario arises.
  if (ref == 1)
    m_device->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9StateBlock::Release() {
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    m_device->Release();
    if (m_self_pinned) {
      m_self_pinned = false;
      ReleasePrivate();
    }
  }
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9StateBlock::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DStateBlock9)) {
    *ppvObject = static_cast<IDirect3DStateBlock9 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9StateBlock::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9StateBlock::Capture() {
  // Reset-invalidation gate: the device's last Reset called invalidate()
  // on every outstanding block. Capture / Apply on a marked block
  // return INVALIDCALL: its reference-pinned slots may now be
  // orphaned and the snapshot's stale values must not propagate.
  // wined3d / DXVK match this shape.
  if (m_invalid)
    return D3DERR_INVALIDCALL;
  // Coupling checks: each snapshot must match the device's storage
  // exactly, otherwise the memcpy walks off. Member-function scope
  // gives access to both private sides; namespace-level statics
  // can't see m_device's private members. Fires at compile time if
  // a future commit trims one side but forgets the other.
  static_assert(
      sizeof(m_snapRenderStates) == sizeof(m_device->m_renderStates),
      "render-state snapshot size must match device array"
  );
  static_assert(
      sizeof(m_snapSamplerStates) == sizeof(m_device->m_samplerStates),
      "sampler-state snapshot size must match device array"
  );
  static_assert(
      sizeof(m_snapTextureStageStates) == sizeof(m_device->m_textureStageStates),
      "texture-stage snapshot size must match device array"
  );
  static_assert(
      sizeof(m_snapTransforms) == sizeof(m_device->m_transforms), "transform snapshot size must match device array"
  );
  static_assert(
      sizeof(m_snapClipPlanes) == sizeof(m_device->m_clipPlanes), "clip-planes snapshot size must match device array"
  );
  static_assert(
      sizeof(m_snapStreamOffsets) == sizeof(m_device->m_streamOffsets),
      "stream-offsets snapshot size must match device array"
  );
  static_assert(
      sizeof(m_snapStreamStrides) == sizeof(m_device->m_streamStrides),
      "stream-strides snapshot size must match device array"
  );
  static_assert(
      sizeof(m_snapStreamFreq) == sizeof(m_device->m_streamFreq), "stream-freq snapshot size must match device array"
  );
  static_assert(
      sizeof(m_snapVsConstantsF) == sizeof(m_device->m_vsConstantsF),
      "VS F-constants snapshot size must match device array"
  );
  static_assert(
      sizeof(m_snapVsConstantsI) == sizeof(m_device->m_vsConstantsI),
      "VS I-constants snapshot size must match device array"
  );
  static_assert(
      sizeof(m_snapVsConstantsB) == sizeof(m_device->m_vsConstantsB),
      "VS B-constants snapshot size must match device array"
  );
  static_assert(
      sizeof(m_snapPsConstantsF) == sizeof(m_device->m_psConstantsF),
      "PS F-constants snapshot size must match device array"
  );
  static_assert(
      sizeof(m_snapPsConstantsI) == sizeof(m_device->m_psConstantsI),
      "PS I-constants snapshot size must match device array"
  );
  static_assert(
      sizeof(m_snapPsConstantsB) == sizeof(m_device->m_psConstantsB),
      "PS B-constants snapshot size must match device array"
  );
  // Per-category snapshot, gated by m_changes: only the touched
  // states restore via memcpy; skip untouched ones (wined3d semantics).
  // Render-state slots use per-state masking; others use category bools.
  for (uint32_t i = 0; i < 256; ++i) {
    if (m_changes.render_states[i])
      m_snapRenderStates[i] = m_device->m_renderStates[i];
  }
  if (m_changes.sampler_states)
    std::memcpy(m_snapSamplerStates, m_device->m_samplerStates, sizeof(m_snapSamplerStates));
  if (m_changes.texture_stage_states)
    std::memcpy(m_snapTextureStageStates, m_device->m_textureStageStates, sizeof(m_snapTextureStageStates));
  if (m_changes.transforms)
    std::memcpy(m_snapTransforms, m_device->m_transforms, sizeof(m_snapTransforms));
  if (m_changes.clip_planes)
    std::memcpy(m_snapClipPlanes, m_device->m_clipPlanes, sizeof(m_snapClipPlanes));
  if (m_changes.viewport)
    m_snapViewport = m_device->m_viewport;
  if (m_changes.scissor)
    m_snapScissorRect = m_device->m_scissorRect;
  if (m_changes.fvf)
    m_snapFvf = m_device->m_fvf;
  if (m_changes.streams) {
    std::memcpy(m_snapStreamOffsets, m_device->m_streamOffsets, sizeof(m_snapStreamOffsets));
    std::memcpy(m_snapStreamStrides, m_device->m_streamStrides, sizeof(m_snapStreamStrides));
    std::memcpy(m_snapStreamFreq, m_device->m_streamFreq, sizeof(m_snapStreamFreq));
    for (size_t i = 0; i < D3D9_MAX_VERTEX_STREAMS; ++i)
      m_snapVertexBuffers[i] = m_device->m_vertexBuffers[i];
  }
  if (m_changes.material)
    m_snapMaterial = m_device->m_material;
  if (m_changes.vs_constants) {
    std::memcpy(m_snapVsConstantsF, m_device->m_vsConstantsF, sizeof(m_snapVsConstantsF));
    std::memcpy(m_snapVsConstantsI, m_device->m_vsConstantsI, sizeof(m_snapVsConstantsI));
    std::memcpy(m_snapVsConstantsB, m_device->m_vsConstantsB, sizeof(m_snapVsConstantsB));
  }
  if (m_changes.ps_constants) {
    std::memcpy(m_snapPsConstantsF, m_device->m_psConstantsF, sizeof(m_snapPsConstantsF));
    std::memcpy(m_snapPsConstantsI, m_device->m_psConstantsI, sizeof(m_snapPsConstantsI));
    std::memcpy(m_snapPsConstantsB, m_device->m_psConstantsB, sizeof(m_snapPsConstantsB));
  }
  // Com<,false>::operator= AddRefs new target before releasing old.
  // RT/DS excluded (wined3d stateblock doesn't capture framebuffer).
  if (m_changes.textures) {
    for (size_t i = 0; i < D3D9_MAX_TEXTURE_UNITS; ++i)
      m_snapTextures[i] = m_device->m_textures[i];
  }
  if (m_changes.index_buffer)
    m_snapIndexBuffer = m_device->m_indexBuffer;
  if (m_changes.vertex_declaration)
    m_snapVertexDeclaration = m_device->m_vertexDeclaration;
  if (m_changes.vertex_shader)
    m_snapVertexShader = m_device->m_vertexShader;
  if (m_changes.pixel_shader)
    m_snapPixelShader = m_device->m_pixelShader;
  if (m_changes.lights) {
    m_snapLights = m_device->m_lights;
    m_snapLightEnables = m_device->m_lightEnables;
  }
  (void)m_type;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9StateBlock::Apply() {
  // Reset-invalidation gate: see Capture() above for the rationale.
  if (m_invalid)
    return D3DERR_INVALIDCALL;
  // Each queued draw holds its own pod_snapshot; Apply rewrites
  // calling-thread shadows. Restore only touched render states
  // (wined3d semantics); unconditional memcpy would clobber unsnapshot
  // state mutations, causing render-state layering bugs.
  uint32_t pod_dirty = 0;
  for (uint32_t i = 0; i < 256; ++i) {
    if (!m_changes.render_states[i])
      continue;
    m_device->m_renderStates[i] = m_snapRenderStates[i];
    pod_dirty |= dxmt::D9ES_DIRTY_RENDER_STATES;
  }
  if (m_changes.sampler_states) {
    std::memcpy(m_device->m_samplerStates, m_snapSamplerStates, sizeof(m_snapSamplerStates));
    pod_dirty |= dxmt::D9ES_DIRTY_SAMPLER_STATES;
  }
  if (m_changes.texture_stage_states) {
    std::memcpy(m_device->m_textureStageStates, m_snapTextureStageStates, sizeof(m_snapTextureStageStates));
    // No POD-snapshot bump: texture_stage_states isn't carried in
    // BatchedDraw::pod_snapshot today (FFP shader generator hasn't
    // landed).
  }
  if (m_changes.transforms) {
    std::memcpy(m_device->m_transforms, m_snapTransforms, sizeof(m_snapTransforms));
    // Same as texture_stage_states: transforms aren't in pod_snapshot.
  }
  if (m_changes.clip_planes) {
    std::memcpy(m_device->m_clipPlanes, m_snapClipPlanes, sizeof(m_snapClipPlanes));
    pod_dirty |= dxmt::D9ES_DIRTY_CLIP_PLANES;
  }
  if (m_changes.viewport) {
    m_device->m_viewport = m_snapViewport;
    m_device->m_viewportDirty = true;
    pod_dirty |= dxmt::D9ES_DIRTY_VIEWPORT;
  }
  if (m_changes.scissor) {
    m_device->m_scissorRect = m_snapScissorRect;
    m_device->m_scissorDirty = true;
    pod_dirty |= dxmt::D9ES_DIRTY_SCISSOR_RECT;
  }
  if (m_changes.fvf)
    m_device->m_fvf = m_snapFvf;
  if (m_changes.streams) {
    std::memcpy(m_device->m_streamOffsets, m_snapStreamOffsets, sizeof(m_snapStreamOffsets));
    std::memcpy(m_device->m_streamStrides, m_snapStreamStrides, sizeof(m_snapStreamStrides));
    std::memcpy(m_device->m_streamFreq, m_snapStreamFreq, sizeof(m_snapStreamFreq));
    for (size_t i = 0; i < D3D9_MAX_VERTEX_STREAMS; ++i) {
      // Push SetRef op alongside the calling-thread shadow assign so
      // m_encodeSideRefs tracks. Only push when the slot actually
      // changes (same shape as SetStreamSource's buffer-changed gate).
      if (m_device->m_vertexBuffers[i].ptr() != m_snapVertexBuffers[i].ptr()) {
        auto *new_vb = m_snapVertexBuffers[i].ptr();
        if (new_vb)
          new_vb->AddRefPrivate();
        m_device->QueueRefOp(
            static_cast<MTLD3D9Device::PendingRefOp::Slot>(MTLD3D9Device::PendingRefOp::VertexBuffer0 + i), new_vb
        );
      }
      m_device->m_vertexBuffers[i] = m_snapVertexBuffers[i];
    }
    pod_dirty |= dxmt::D9ES_DIRTY_STREAM_FREQ;
  }
  if (m_changes.material)
    m_device->m_material = m_snapMaterial;
  if (m_changes.vs_constants) {
    std::memcpy(m_device->m_vsConstantsF, m_snapVsConstantsF, sizeof(m_snapVsConstantsF));
    std::memcpy(m_device->m_vsConstantsI, m_snapVsConstantsI, sizeof(m_snapVsConstantsI));
    std::memcpy(m_device->m_vsConstantsB, m_snapVsConstantsB, sizeof(m_snapVsConstantsB));
    pod_dirty |= dxmt::D9ES_DIRTY_VS_CONST_F | dxmt::D9ES_DIRTY_VS_CONST_I | dxmt::D9ES_DIRTY_VS_CONST_B;
  }
  if (m_changes.ps_constants) {
    std::memcpy(m_device->m_psConstantsF, m_snapPsConstantsF, sizeof(m_snapPsConstantsF));
    std::memcpy(m_device->m_psConstantsI, m_snapPsConstantsI, sizeof(m_snapPsConstantsI));
    std::memcpy(m_device->m_psConstantsB, m_snapPsConstantsB, sizeof(m_snapPsConstantsB));
    pod_dirty |= dxmt::D9ES_DIRTY_PS_CONST_F | dxmt::D9ES_DIRTY_PS_CONST_I | dxmt::D9ES_DIRTY_PS_CONST_B;
  }
  if (m_changes.textures) {
    for (size_t i = 0; i < D3D9_MAX_TEXTURE_UNITS; ++i) {
      if (m_device->m_textures[i].ptr() != m_snapTextures[i].ptr()) {
        auto *new_tex = m_snapTextures[i].ptr();
        if (new_tex)
          new_tex->AddRefPrivate();
        m_device->QueueRefOp(
            static_cast<MTLD3D9Device::PendingRefOp::Slot>(MTLD3D9Device::PendingRefOp::Texture0 + i), new_tex
        );
      }
      m_device->m_textures[i] = m_snapTextures[i];
    }
  }
  if (m_changes.index_buffer) {
    if (m_device->m_indexBuffer.ptr() != m_snapIndexBuffer.ptr()) {
      auto *new_ib = m_snapIndexBuffer.ptr();
      if (new_ib)
        new_ib->AddRefPrivate();
      m_device->QueueRefOp(MTLD3D9Device::PendingRefOp::IndexBuffer, new_ib);
    }
    m_device->m_indexBuffer = m_snapIndexBuffer;
  }
  if (m_changes.vertex_declaration) {
    if (m_device->m_vertexDeclaration.ptr() != m_snapVertexDeclaration.ptr()) {
      auto *new_decl = m_snapVertexDeclaration.ptr();
      if (new_decl)
        new_decl->AddRefPrivate();
      m_device->QueueRefOp(MTLD3D9Device::PendingRefOp::VertexDeclaration, new_decl);
    }
    m_device->m_vertexDeclaration = m_snapVertexDeclaration;
  }
  if (m_changes.vertex_shader) {
    if (m_device->m_vertexShader.ptr() != m_snapVertexShader.ptr()) {
      auto *new_vs = m_snapVertexShader.ptr();
      if (new_vs)
        new_vs->AddRefPrivate();
      m_device->QueueRefOp(MTLD3D9Device::PendingRefOp::VertexShader, new_vs);
    }
    m_device->m_vertexShader = m_snapVertexShader;
  }
  if (m_changes.pixel_shader) {
    if (m_device->m_pixelShader.ptr() != m_snapPixelShader.ptr()) {
      auto *new_ps = m_snapPixelShader.ptr();
      if (new_ps)
        new_ps->AddRefPrivate();
      m_device->QueueRefOp(MTLD3D9Device::PendingRefOp::PixelShader, new_ps);
    }
    m_device->m_pixelShader = m_snapPixelShader;
  }
  m_device->m_encShadowDirty |= pod_dirty;
  if (m_changes.lights) {
    m_device->m_lights = m_snapLights;
    m_device->m_lightEnables = m_snapLightEnables;
  }
  // RT/DS not restored: wined3d stateblock excludes framebuffer.
  // Capturing would un-bind post-snapshot DS, forcing no-DS fallback.
  return D3D_OK;
}

} // namespace dxmt
