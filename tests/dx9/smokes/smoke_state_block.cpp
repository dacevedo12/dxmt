// State-block API smoke. Covers the create/begin/end surface (the
// LoL scoreboard create-and-apply pattern) plus Capture/Apply
// round-trips spanning POD categories (render states, sampler
// states, texture-stage states, transforms, viewport, scissor, FVF,
// VS constants) and reference-pinned slots (textures).

#include "../dx9_smoke.h"
void
test_state_block(void) {
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    printf("Direct3DCreate9: NULL\n");
    return;
  }

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

  IDirect3DDevice9 *dev = NULL;
  HRESULT cdhr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  printf("CreateDevice: hr=0x%08lx\n", (unsigned long)cdhr);
  if (FAILED(cdhr) || !dev) {
    d3d->Release();
    return;
  }

  // CreateStateBlock: D3DSBT_ALL is the LoL scoreboard shape.
  IDirect3DStateBlock9 *sb_all = NULL;
  HRESULT r1 = dev->CreateStateBlock(D3DSBT_ALL, &sb_all);
  check_hr(r1);
  check_true(sb_all != nullptr);

  // VERTEXSTATE / PIXELSTATE — should also succeed.
  IDirect3DStateBlock9 *sb_v = NULL;
  HRESULT r2 = dev->CreateStateBlock(D3DSBT_VERTEXSTATE, &sb_v);
  check_hr(r2);
  check_true(sb_v != nullptr);
  IDirect3DStateBlock9 *sb_p = NULL;
  HRESULT r3 = dev->CreateStateBlock(D3DSBT_PIXELSTATE, &sb_p);
  check_hr(r3);
  check_true(sb_p != nullptr);

  // Bogus type — must reject and leave the out-pointer NULL.
  IDirect3DStateBlock9 *sb_bad = (IDirect3DStateBlock9 *)0xdead;
  HRESULT rb = dev->CreateStateBlock((D3DSTATEBLOCKTYPE)999, &sb_bad);
  check_hr_eq(rb, D3DERR_INVALIDCALL);
  check_true(sb_bad == NULL);

  // Capture / Apply round-trip on render states. Steps:
  //   1. set marker render state
  //   2. Capture
  //   3. mutate render state to a different value
  //   4. Apply
  //   5. read back — must equal the captured value, not the mutation
  // CULLMODE is a stable per-frame state apps actually use, so it's
  // a more honest probe than e.g. LIGHTING (some shaders ignore it).
  if (sb_all) {
    HRESULT cr = sb_all->Capture();
    HRESULT ap = sb_all->Apply();
    check_hr(cr);
    check_hr(ap);

    IDirect3DDevice9 *back = NULL;
    HRESULT gd = sb_all->GetDevice(&back);
    check_hr(gd);
    check_eq_ptr(back, dev);
    if (back)
      back->Release();
  }

  // Render-state round-trip.
  IDirect3DStateBlock9 *sb_rs = NULL;
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
  dev->CreateStateBlock(D3DSBT_ALL, &sb_rs);       // captures at create
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW); // mutate
  DWORD pre_apply = 0;
  dev->GetRenderState(D3DRS_CULLMODE, &pre_apply);
  if (sb_rs)
    sb_rs->Apply();
  DWORD post_apply = 0;
  dev->GetRenderState(D3DRS_CULLMODE, &post_apply);
  check_true(sb_rs != nullptr);
  check_eq_u32(pre_apply, D3DCULL_CW);   // the mutation, pre-Apply
  check_eq_u32(post_apply, D3DCULL_CCW); // the captured value, restored
  if (sb_rs)
    sb_rs->Release();

  // Sampler-state round-trip — same shape, separate array.
  IDirect3DStateBlock9 *sb_samp = NULL;
  dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  dev->CreateStateBlock(D3DSBT_ALL, &sb_samp);
  dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
  if (sb_samp)
    sb_samp->Apply();
  DWORD samp0_post = 0, samp1_post = 0;
  dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &samp0_post);
  dev->GetSamplerState(1, D3DSAMP_ADDRESSU, &samp1_post);
  check_eq_u32(samp0_post, D3DTEXF_LINEAR);
  check_eq_u32(samp1_post, D3DTADDRESS_CLAMP);
  if (sb_samp)
    sb_samp->Release();

  // Capture-after-mutate: block.Capture re-snapshots current device
  // state, so a subsequent Apply must restore THAT, not the old one.
  IDirect3DStateBlock9 *sb_recap = NULL;
  dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
  dev->CreateStateBlock(D3DSBT_ALL, &sb_recap);
  dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
  if (sb_recap)
    sb_recap->Capture(); // re-snapshot WIREFRAME
  dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_POINT);
  if (sb_recap)
    sb_recap->Apply();
  DWORD recap_post = 0;
  dev->GetRenderState(D3DRS_FILLMODE, &recap_post);
  // Apply must restore the re-captured WIREFRAME, not the create-time SOLID.
  check_eq_u32(recap_post, D3DFILL_WIREFRAME);
  if (sb_recap)
    sb_recap->Release();

  // Texture-stage round-trip — D3DTSS_COLOROP / D3DTSS_TEXCOORDINDEX
  // are FFP states LoL exercises heavily.
  IDirect3DStateBlock9 *sb_tss = NULL;
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 2);
  dev->CreateStateBlock(D3DSBT_ALL, &sb_tss);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 5);
  if (sb_tss)
    sb_tss->Apply();
  DWORD tss0_post = 0, tss1_post = 0;
  dev->GetTextureStageState(0, D3DTSS_COLOROP, &tss0_post);
  dev->GetTextureStageState(1, D3DTSS_TEXCOORDINDEX, &tss1_post);
  check_eq_u32(tss0_post, D3DTOP_MODULATE);
  check_eq_u32(tss1_post, 2);
  if (sb_tss)
    sb_tss->Release();

  // Transform round-trip — D3DTS_VIEW + D3DTS_PROJECTION are the
  // primary FFP cameras. Use a marker matrix (identity vs zero is too
  // boring; pick a recognisable scale).
  IDirect3DStateBlock9 *sb_xf = NULL;
  D3DMATRIX m_view0 = {{2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1}};
  D3DMATRIX m_proj0 = {{3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 1}};
  dev->SetTransform(D3DTS_VIEW, &m_view0);
  dev->SetTransform(D3DTS_PROJECTION, &m_proj0);
  dev->CreateStateBlock(D3DSBT_ALL, &sb_xf);
  D3DMATRIX m_zero = {};
  dev->SetTransform(D3DTS_VIEW, &m_zero);
  dev->SetTransform(D3DTS_PROJECTION, &m_zero);
  if (sb_xf)
    sb_xf->Apply();
  D3DMATRIX m_view_post = {}, m_proj_post = {};
  dev->GetTransform(D3DTS_VIEW, &m_view_post);
  dev->GetTransform(D3DTS_PROJECTION, &m_proj_post);
  check_float_eq_eps(m_view_post.m[0][0], 2.0f, 0.0001f);
  check_float_eq_eps(m_proj_post.m[0][0], 3.0f, 0.0001f);
  if (sb_xf)
    sb_xf->Release();

  // Viewport + scissor round-trip — both POD, single struct copy.
  IDirect3DStateBlock9 *sb_vp = NULL;
  D3DVIEWPORT9 vp0 = {4, 5, 32, 24, 0.25f, 0.75f};
  RECT sc0 = {1, 2, 30, 20};
  dev->SetViewport(&vp0);
  dev->SetScissorRect(&sc0);
  dev->CreateStateBlock(D3DSBT_ALL, &sb_vp);
  D3DVIEWPORT9 vp_mut = {0, 0, 64, 64, 0.0f, 1.0f};
  RECT sc_mut = {0, 0, 64, 64};
  dev->SetViewport(&vp_mut);
  dev->SetScissorRect(&sc_mut);
  if (sb_vp)
    sb_vp->Apply();
  D3DVIEWPORT9 vp_post = {};
  RECT sc_post = {};
  dev->GetViewport(&vp_post);
  dev->GetScissorRect(&sc_post);
  check_eq_u32(vp_post.X, 4);
  check_eq_u32(vp_post.Y, 5);
  check_eq_u32(vp_post.Width, 32);
  check_eq_u32(vp_post.Height, 24);
  check_float_eq_eps(vp_post.MinZ, 0.25f, 0.0001f);
  check_float_eq_eps(vp_post.MaxZ, 0.75f, 0.0001f);
  check_eq_u32(sc_post.left, 1);
  check_eq_u32(sc_post.top, 2);
  check_eq_u32(sc_post.right, 30);
  check_eq_u32(sc_post.bottom, 20);
  if (sb_vp)
    sb_vp->Release();

  // VS constant register file round-trip — c0 + c5 covers near + far.
  IDirect3DStateBlock9 *sb_vc = NULL;
  float vc0[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  float vc5[4] = {9.0f, 8.0f, 7.0f, 6.0f};
  dev->SetVertexShaderConstantF(0, vc0, 1);
  dev->SetVertexShaderConstantF(5, vc5, 1);
  dev->CreateStateBlock(D3DSBT_ALL, &sb_vc);
  float zero[4] = {};
  dev->SetVertexShaderConstantF(0, zero, 1);
  dev->SetVertexShaderConstantF(5, zero, 1);
  if (sb_vc)
    sb_vc->Apply();
  float vc0_post[4] = {}, vc5_post[4] = {};
  dev->GetVertexShaderConstantF(0, vc0_post, 1);
  dev->GetVertexShaderConstantF(5, vc5_post, 1);
  check_float_eq_eps(vc0_post[0], 1.0f, 0.0001f);
  check_float_eq_eps(vc0_post[1], 2.0f, 0.0001f);
  check_float_eq_eps(vc0_post[2], 3.0f, 0.0001f);
  check_float_eq_eps(vc0_post[3], 4.0f, 0.0001f);
  check_float_eq_eps(vc5_post[0], 9.0f, 0.0001f);
  check_float_eq_eps(vc5_post[1], 8.0f, 0.0001f);
  check_float_eq_eps(vc5_post[2], 7.0f, 0.0001f);
  check_float_eq_eps(vc5_post[3], 6.0f, 0.0001f);
  if (sb_vc)
    sb_vc->Release();

  // Texture binding round-trip — exercises the reference-pinned
  // capture path. Set stage 0 to texA, capture, mutate to texB,
  // apply, GetTexture must hand back texA.
  IDirect3DTexture9 *texA = NULL;
  IDirect3DTexture9 *texB = NULL;
  dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texA, NULL);
  dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texB, NULL);
  if (texA && texB) {
    IDirect3DStateBlock9 *sb_tex = NULL;
    dev->SetTexture(0, texA);
    dev->CreateStateBlock(D3DSBT_ALL, &sb_tex);
    dev->SetTexture(0, texB);
    if (sb_tex)
      sb_tex->Apply();
    IDirect3DBaseTexture9 *tex_post = NULL;
    dev->GetTexture(0, &tex_post);
    // Apply must restore the captured texA over the mutated-to texB.
    check_eq_ptr(tex_post, static_cast<IDirect3DBaseTexture9 *>(texA));
    if (tex_post)
      tex_post->Release();
    dev->SetTexture(0, NULL); // unbind before releasing
    if (sb_tex)
      sb_tex->Release();
  }
  if (texB)
    texB->Release();
  if (texA)
    texA->Release();

  // FVF round-trip.
  IDirect3DStateBlock9 *sb_fv = NULL;
  dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
  dev->CreateStateBlock(D3DSBT_ALL, &sb_fv);
  dev->SetFVF(D3DFVF_XYZRHW);
  if (sb_fv)
    sb_fv->Apply();
  DWORD fvf_post = 0;
  dev->GetFVF(&fvf_post);
  check_eq_u32(fvf_post, D3DFVF_XYZ | D3DFVF_DIFFUSE);
  if (sb_fv)
    sb_fv->Release();

  // Begin/End recording.
  HRESULT bs = dev->BeginStateBlock();
  check_hr(bs);

  // Re-Begin while already recording — must reject.
  HRESULT bs2 = dev->BeginStateBlock();
  check_hr_eq(bs2, D3DERR_INVALIDCALL);

  // Create-while-recording — wined3d rejects and leaves the out NULL.
  IDirect3DStateBlock9 *sb_dur = (IDirect3DStateBlock9 *)0xdead;
  HRESULT rdur = dev->CreateStateBlock(D3DSBT_ALL, &sb_dur);
  check_hr_eq(rdur, D3DERR_INVALIDCALL);
  check_true(sb_dur == NULL);

  IDirect3DStateBlock9 *sb_recorded = NULL;
  HRESULT es = dev->EndStateBlock(&sb_recorded);
  check_hr(es);
  check_true(sb_recorded != nullptr);

  // End without matching Begin — must reject and leave the out NULL.
  IDirect3DStateBlock9 *sb_unbalanced = (IDirect3DStateBlock9 *)0xdead;
  HRESULT eub = dev->EndStateBlock(&sb_unbalanced);
  check_hr_eq(eub, D3DERR_INVALIDCALL);
  check_true(sb_unbalanced == NULL);

  if (sb_recorded)
    sb_recorded->Release();
  if (sb_p)
    sb_p->Release();
  if (sb_v)
    sb_v->Release();
  if (sb_all)
    sb_all->Release();

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);
  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
