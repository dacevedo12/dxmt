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
  printf("CreateStateBlock(ALL): hr=0x%08lx out=%s\n", (unsigned long)r1, sb_all ? "non-null" : "null");

  // VERTEXSTATE / PIXELSTATE — should also succeed.
  IDirect3DStateBlock9 *sb_v = NULL;
  HRESULT r2 = dev->CreateStateBlock(D3DSBT_VERTEXSTATE, &sb_v);
  printf("CreateStateBlock(VERTEX): hr=0x%08lx out=%s\n", (unsigned long)r2, sb_v ? "non-null" : "null");
  IDirect3DStateBlock9 *sb_p = NULL;
  HRESULT r3 = dev->CreateStateBlock(D3DSBT_PIXELSTATE, &sb_p);
  printf("CreateStateBlock(PIXEL): hr=0x%08lx out=%s\n", (unsigned long)r3, sb_p ? "non-null" : "null");

  // Bogus type — must reject.
  IDirect3DStateBlock9 *sb_bad = (IDirect3DStateBlock9 *)0xdead;
  HRESULT rb = dev->CreateStateBlock((D3DSTATEBLOCKTYPE)999, &sb_bad);
  printf(
      "CreateStateBlock(bogus): hr=0x%08lx out=%s (must reject)\n", (unsigned long)rb,
      sb_bad == NULL ? "null" : "non-null"
  );

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
    printf("  sb_all Capture: hr=0x%08lx Apply: hr=0x%08lx\n", (unsigned long)cr, (unsigned long)ap);

    IDirect3DDevice9 *back = NULL;
    HRESULT gd = sb_all->GetDevice(&back);
    printf("  sb_all GetDevice: hr=0x%08lx same=%s\n", (unsigned long)gd, (back == dev) ? "yes" : "no");
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
  printf(
      "RS round-trip: pre_apply=%lu post_apply=%lu (expect %lu→%lu)\n", (unsigned long)pre_apply,
      (unsigned long)post_apply, (unsigned long)D3DCULL_CW, (unsigned long)D3DCULL_CCW
  );
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
  printf(
      "Samp round-trip: stage0.MAGFILTER=%lu (expect %lu=LINEAR) "
      "stage1.ADDRESSU=%lu (expect %lu=CLAMP)\n",
      (unsigned long)samp0_post, (unsigned long)D3DTEXF_LINEAR, (unsigned long)samp1_post,
      (unsigned long)D3DTADDRESS_CLAMP
  );
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
  printf(
      "RS re-capture: post_apply=%lu (expect %lu — the recaptured "
      "WIREFRAME, not SOLID)\n",
      (unsigned long)recap_post, (unsigned long)D3DFILL_WIREFRAME
  );
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
  printf(
      "TSS round-trip: stage0.COLOROP=%lu (expect %lu=MODULATE) "
      "stage1.TEXCOORDINDEX=%lu (expect 2)\n",
      (unsigned long)tss0_post, (unsigned long)D3DTOP_MODULATE, (unsigned long)tss1_post
  );
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
  printf(
      "Transform round-trip: view._11=%.1f (expect 2.0) "
      "proj._11=%.1f (expect 3.0)\n",
      (double)m_view_post.m[0][0], (double)m_proj_post.m[0][0]
  );
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
  printf(
      "Viewport round-trip: x=%lu y=%lu w=%lu h=%lu (expect 4 5 32 24) "
      "min=%.2f max=%.2f\n",
      (unsigned long)vp_post.X, (unsigned long)vp_post.Y, (unsigned long)vp_post.Width, (unsigned long)vp_post.Height,
      (double)vp_post.MinZ, (double)vp_post.MaxZ
  );
  printf(
      "Scissor round-trip: l=%ld t=%ld r=%ld b=%ld (expect 1 2 30 20)\n", (long)sc_post.left, (long)sc_post.top,
      (long)sc_post.right, (long)sc_post.bottom
  );
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
  printf(
      "VS const round-trip: c0=%.1f,%.1f,%.1f,%.1f c5=%.1f,%.1f,%.1f,%.1f "
      "(expect 1,2,3,4 / 9,8,7,6)\n",
      (double)vc0_post[0], (double)vc0_post[1], (double)vc0_post[2], (double)vc0_post[3], (double)vc5_post[0],
      (double)vc5_post[1], (double)vc5_post[2], (double)vc5_post[3]
  );
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
    printf("Texture round-trip: post=%s (expect texA)\n", tex_post == texA ? "texA" : "other");
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
  printf("FVF round-trip: post=0x%lx (expect 0x42=XYZ|DIFFUSE)\n", (unsigned long)fvf_post);
  if (sb_fv)
    sb_fv->Release();

  // Begin/End recording.
  HRESULT bs = dev->BeginStateBlock();
  printf("BeginStateBlock: hr=0x%08lx\n", (unsigned long)bs);

  // Re-Begin while already recording — must reject.
  HRESULT bs2 = dev->BeginStateBlock();
  printf("BeginStateBlock(re-entry): hr=0x%08lx (must reject)\n", (unsigned long)bs2);

  // Create-while-recording — wined3d rejects.
  IDirect3DStateBlock9 *sb_dur = (IDirect3DStateBlock9 *)0xdead;
  HRESULT rdur = dev->CreateStateBlock(D3DSBT_ALL, &sb_dur);
  printf(
      "CreateStateBlock(during record): hr=0x%08lx out=%s "
      "(must reject)\n",
      (unsigned long)rdur, sb_dur == NULL ? "null" : "non-null"
  );

  IDirect3DStateBlock9 *sb_recorded = NULL;
  HRESULT es = dev->EndStateBlock(&sb_recorded);
  printf("EndStateBlock: hr=0x%08lx out=%s\n", (unsigned long)es, sb_recorded ? "non-null" : "null");

  // End without matching Begin — must reject.
  IDirect3DStateBlock9 *sb_unbalanced = (IDirect3DStateBlock9 *)0xdead;
  HRESULT eub = dev->EndStateBlock(&sb_unbalanced);
  printf(
      "EndStateBlock(unbalanced): hr=0x%08lx out=%s (must reject)\n", (unsigned long)eub,
      sb_unbalanced == NULL ? "null" : "non-null"
  );

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
