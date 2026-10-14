"""Hand-curated meaning of every hard-coded DCS location in DcsQvCull's src/.

extract_manifest.py binds each entry to the source lines that hold it
(`src`), takes the current value from the source when the source has one,
and derives signatures from the DCS binaries. What the parser cannot know
reliably lives here: which module a bare offset belongs to, what kind of
location it is, the RTTI class, the slot, and which install-time byte checks
apply.

Source bindings ("file:key"):
  file:ns::kName          a scanned constant (src_scan.py symbol)
  file:ns::kArr[2].0      field 0 of row 2 of a scanned table
  file:literal:0x590d8    an inline `base + 0x590d8` (every occurrence in file)
  file:symbol:?name...    an export/import name string literal
  file:re:<regex>         the first line matching the regex (no value)
The first binding with a value supplies the entry's value; every binding's
value must agree, else extract_manifest.py reports a conflict.

Entries without a value binding take their value from the binary
(value_from = "binary"): slot targets the code hooks without comparing,
export/import addresses, and similar.

Kinds: vtable, vtable_slot, function, instruction, call_site, code_hash,
data, iat_import, export, external.
"""

ENTRIES: list[dict] = []


def E(**kw):
    ENTRIES.append(kw)
    return kw["id"]


def vt(id, module, rtti, src, col_offset=0, prefix=False, export=None, checks=None, rtti_checked=True, **kw):
    return E(id=id, module=module, kind="vtable", rtti=rtti, rtti_prefix=prefix, col_offset=col_offset,
             src=src, export=export, checks=checks or [], rtti_checked=rtti_checked, **kw)


def slot(id, vtable, n, src=(), compared=True, export=None, checks=None, **kw):
    """Slot n of `vtable` (an entry id). compared: the code compares the slot's
    current target with the hard-coded value before hooking."""
    return E(id=id, kind="vtable_slot", vtable=vtable, slot=n, src=list(src), target_compared=compared,
             export=export, checks=checks or [], **kw)


def fn(id, module, src, checks=None, export=None, **kw):
    return E(id=id, module=module, kind="function", src=src, checks=checks or [], export=export, **kw)


def B(hexstr, at=0, alt=None, note=None):
    d = {"type": "bytes", "at": at, "hex": hexstr}
    if alt:
        d["alt"] = alt
    if note:
        d["note"] = note
    return d


# ===========================================================================
# NGModel.dll
# ===========================================================================
NG = "NGModel.dll"
vt("NG.StructBufferManager.vtbl", NG, ".?AVStructBufferManager@model@@", ["alloc_slab.h:allocslab::kVtableRva"],
   used_by="allocslab")
slot("NG.StructBufferManager.reset", "NG.StructBufferManager.vtbl", 7, ["alloc_slab.h:re:kResetSlot = 7"],
     compared=False, used_by="allocslab, bigpages (per-frame reset hook)")
fn("NG.StructBufferManager.allocate", NG, ["alloc_slab.h:allocslab::kAllocRva"], used_by="allocslab (14-byte detour)",
   checks=[B("48 89 5C 24 10 48 89 6C 24 18 56 57 41 56",
             alt=["FF 25 00 00 00 00"], note="kPrologue; at runtime FF 25 ... = our own earlier detour")])
fn("NG.StructBufferManager.scan", NG, ["alloc_slab.h:allocslab::kScanRva"], used_by="allocslab (called directly)",
   checks=[B("48 89 5C 24 08 48 89 6C", note="kScanPrologue")])
E(id="NG.iat.ed_mutex_lock", module=NG, kind="iat_import", import_=("edcore.dll", "?lock@mutex@ed@@QEAAXXZ"),
  src=["alloc_slab.h:allocslab::kLockIat"], used_by="allocslab")
E(id="NG.iat.ed_mutex_unlock", module=NG, kind="iat_import", import_=("edcore.dll", "?unlock@mutex@ed@@QEAAXXZ"),
  src=["alloc_slab.h:allocslab::kUnlockIat"], used_by="allocslab")
E(id="NG.SceneRenderable.render.triCounterAdd", module=NG, kind="instruction",
  src=["alloc_slab.h:tricount::kRva", "gb_batch.h:gbbatch::kTriSite"],
  checks=[B("F0 48 01 82 E8 01 00 00", alt=["3E 48 01 82 E8 01 00 00"],
            note="lock add [rdx+0x1e8], rax; 3E = tricount's plain add")],
  used_by="tricount (1-byte prefix patch), gbbatch (verify)")
E(id="NG.StructBufferManager.scan.pageBytesImm", module=NG, kind="instruction",
  src=["big_pages.h:bigpages::kInsnRva"],
  checks=[B("B8 00 FE 00 00", note="mov eax, 0xfe00 (kStockInsn); at runtime the immediate may be BigPageBytes")],
  used_by="bigpages (immediate rewrite)")

vt("NG.SceneRenderable.vtbl", NG, ".?AVSceneRenderable@model@@",
   ["gb_count.h:gbcount::kVtableRva", "gb_batch.h:gbbatch::kSrVtbl"], used_by="gbcount, gbbatch")
slot("NG.SceneRenderable.render", "NG.SceneRenderable.vtbl", 1,
     ["gb_count.h:literal:0x44350", "gb_batch.h:gbbatch::kNgCode[0].0", "shadow_inst.h:shadowinst::kSceneRenderable"],
     used_by="gbcount (hook), gbbatch, shadowinst (hash)")
vt("NG.ShadowMapRenderable.vtbl", NG, ".?AVShadowMapRenderable@model@@",
   ["inst_count.h:instcount::kVtableRva", "shadow_batch.h:shadowbatch::kSmrVtbl"], used_by="instcount, shadowbatch")
slot("NG.ShadowMapRenderable.render", "NG.ShadowMapRenderable.vtbl", 1, ["inst_count.h:literal:0x443e0"],
     used_by="instcount (hook)")
vt("NG.ModelMaterialMT.vtbl", NG, ".?AVModelMaterialMT@model@@",
   ["binder_count.h:literal:0x592f0", "gb_count.h:literal:0x592f0", "inst_count.h:literal:0x592f0",
    "gb_batch.h:gbbatch::kModelMatVtbl", "shadow_batch.h:shadowbatch::kModelMatVtbl",
    "shadow_inst.h:shadowinst::kModelMatVtbl", "shadow_tex.h:shadowtex::kModelVtbl"],
   used_by="shadowcount, gbcount, instcount, gbbatch, shadowbatch, shadowinst, shadowtex")
slot("NG.ModelMaterialMT.draw", "NG.ModelMaterialMT.vtbl", 4,
     ["gb_batch.h:gbbatch::kModelDraw", "shadow_inst.h:shadowinst::kModelDraw", "gb_batch.h:gbbatch::kNgCode[2].0"],
     used_by="gbbatch, shadowinst (compared, hashed)")
slot("NG.ModelMaterialMT.drawShadow", "NG.ModelMaterialMT.vtbl", 5,
     ["binder_count.h:literal:0x17750", "shadow_tex.h:shadowtex::kSlot5"], used_by="shadowcount, shadowtex (hook)")
vt("NG.GlassMaterialMT.vtbl", NG, ".?AVGlassMaterialMT@model@@", ["binder_count.h:literal:0x590d8"],
   used_by="shadowcount")
slot("NG.GlassMaterialMT.drawShadow", "NG.GlassMaterialMT.vtbl", 5, ["binder_count.h:literal:0x14980"],
     used_by="shadowcount (hook)")
vt("NG.NGModelLodsMT.vtbl", NG, ".?AVNGModelLodsMT@model@@", ["model_timing.h:mtiming::kVtableRva"],
   rtti_checked=False, used_by="mtiming (checked against the ParseMT2 export, not RTTI)")
slot("NG.NGModelLodsMT.ParseMT2", "NG.NGModelLodsMT.vtbl", 31, ["model_timing.h:re:kParseSlot = 31"],
     export="?ParseMT2@NGModelLodsMT@model@@UEAAXAEAVModelInstance@Graphics@@IPEAPEBUCollectionInfo@render@@"
            "QEB_NPEAPEAV?$vector@PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@"
            "AEAUILightProbeSampler@6@AEBV?$wPosition3@M@@@Z",
     used_by="mtiming (hook)")
E(id="NG.model_globals", module=NG, kind="data", export="?globals@model@@3PEAVGlobalsMT@1@EA",
  src=["gb_batch.h:gbbatch::kGlobals", "shadow_tex.h:shadowtex::kGlobals"], used_by="gbbatch, shadowtex (read)")
E(id="NG.iat.PropertiesSet_getTexture", module=NG, kind="iat_import",
  import_=("modeldesc.dll", "?getTexture@PropertiesSet@model@@QEBAAEBUTexture2dProperties@2@I@Z"),
  src=["shadow_tex.h:shadowtex::kIatGetTexture"], used_by="shadowtex (calls through it)")
E(id="NG.iat.Texture2dProperties_valid", module=NG, kind="iat_import",
  import_=("modeldesc.dll", "?valid@Texture2dProperties@model@@QEBA_NXZ"),
  src=["shadow_tex.h:shadowtex::kIatValid"], used_by="shadowtex (calls through it)")
fn("NG.evalAnimatedUniforms", NG, ["gb_batch.h:gbbatch::kEvalAnim", "gb_batch.h:gbbatch::kNgCode[6].0"],
   used_by="gbbatch (called directly)")
fn("NG.ModelMaterialMT.submit", NG, ["shadow_tex.h:shadowtex::kSubmit", "gb_batch.h:gbbatch::kNgCode[9].0"],
   used_by="shadowtex (called directly), gbbatch (hash)")
E(id="NG.flirThreshold", module=NG, kind="data", src=["shadow_inst.h:shadowinst::kFlirThreshold"],
  checks=[B("00 00 80 3F", note="float 1.0")], used_by="shadowinst (read once)")
E(id="NG.ModelMaterialMT.draw.jumpTable", module=NG, kind="code_hash",
  src=["shadow_inst.h:shadowinst::kDrawTable"], anchor="NG.ModelMaterialMT.draw",
  note="switch jump table inside .text (data): located as an offset from ModelMaterialMT::draw; its entries are"
       " RVAs and change whenever the function moves")

# ===========================================================================
# dx11backend.dll
# ===========================================================================
DX = "dx11backend.dll"
vt("DX.DX11Shader.vtbl", DX, ".?AVDX11Shader@RenderAPI@@",
   ["binder_count.h:literal:0xb43b8", "shadow_tex.h:shadowtex::kShaderVtbl", "shadow_inst.h:shadowinst::kShaderVtbl"],
   used_by="shadowcount, shadowtex, shadowinst, gbbatch")
slot("DX.DX11Shader.dtor", "DX.DX11Shader.vtbl", 0, ["shadow_tex.h:literal:0x1d3c0"], used_by="shadowtex (hook)")
slot("DX.DX11Shader.setTexture", "DX.DX11Shader.vtbl", 26,
     ["binder_count.h:literal:0x1fd70", "shadow_tex.h:shadowtex::kSetTexture"], used_by="shadowcount (hook), shadowtex")
slot("DX.DX11Shader.setStructBuffer", "DX.DX11Shader.vtbl", 27,
     ["gb_batch.h:literal:0x1fe70", "gb_batch.h:gbbatch::kDxCode[0].0"], used_by="gbbatch (compared, hashed)")
vt("DX.CEffect.vtbl", DX, ".?AVCEffect@D3DX11Effects@@", ["shadow_inst.h:shadowinst::kEffectVtbl"], used_by="shadowinst")
slot("DX.CEffect.GetDevice", "DX.CEffect.vtbl", 4, ["shadow_inst.h:shadowinst::kEffectGetDevice"],
     used_by="shadowinst (compared, hashed)")
vt("DX.SShaderGlobalVariable.vtbl", DX, ".?AUSShaderGlobalVariable@D3DX11Effects@@",
   ["shadow_inst.h:shadowinst::kShaderVarVtbl"], used_by="shadowinst")
slot("DX.SShaderGlobalVariable.GetVertexShader", "DX.SShaderGlobalVariable.vtbl", 32,
     ["shadow_inst.h:shadowinst::kGetVertexShader"], used_by="shadowinst (compared, hashed)")
vt("DX.STechnique.vtbl", DX, ".?AUSTechnique@D3DX11Effects@@",
   ["shadow_inst.h:shadowinst::kTechVtbl", "shadow_tex.h:shadowtex::kTechVtbl"], used_by="shadowinst, shadowtex")
vt("DX.SPassBlock.vtbl", DX, ".?AUSPassBlock@D3DX11Effects@@",
   ["shadow_inst.h:shadowinst::kPassVtbl", "shadow_tex.h:shadowtex::kPassVtbl", "fx_apply_count.h:fxapply::kPassVtbl"],
   used_by="shadowinst, shadowtex, fxapply")
slot("DX.SPassBlock.Apply", "DX.SPassBlock.vtbl", 13, ["fx_apply_count.h:fxapply::kApply", "fx_apply_count.h:re:kSlot = 13"],
     used_by="fxapply (hook; compared, hashed)")
vt("DX.DX11Texture.vtbl", DX, ".?AVDX11Texture@RenderAPI@@",
   ["tex_bind.h:texbind::kVtableRva", "shadow_tex.h:shadowtex::kTexVtbl"], used_by="texbind, shadowtex, gbbatch")
slot("DX.DX11Texture.requestMip", "DX.DX11Texture.vtbl", 23, ["tex_bind.h:literal:0x49e90"], used_by="texbind (hook)")
slot("DX.DX11Texture.slot18", "DX.DX11Texture.vtbl", 18, ["shadow_tex.h:literal:0x120a0"], used_by="shadowtex (compared)")
slot("DX.DX11Texture.slot11", "DX.DX11Texture.vtbl", 11, ["shadow_tex.h:literal:0x11190"], used_by="shadowtex (compared)")
for cls, sym, s4 in (("DX11TextureFromFileInternalImpl", "kInnerFile", "331f0"),
                     ("DX11TextureArrayFromFileInternalImpl", "kInnerArray", "331f0"),
                     ("DX11TextureInternalDummy", "kInnerDummy", "5250")):
    v = vt(f"DX.{cls}.vtbl", DX, f".?AV{cls}@RenderAPI@@", [f"shadow_tex.h:shadowtex::{sym}"],
           used_by="shadowtex, gbbatch (identity compare)")
    slot(f"DX.{cls}.slot8", v, 8, ["shadow_tex.h:literal:0x5010"], used_by="shadowtex (compared: `ret 0`)")
    slot(f"DX.{cls}.slot4", v, 4, [f"shadow_tex.h:literal:0x{s4}"], used_by="shadowtex (compared)")
fn("DX.ret0_stub", DX, ["shadow_tex.h:literal:0x5010"], checks=[B("C2 00 00", note="ret 0")],
   used_by="shadowtex", note="a 3-byte `ret 0`: no unique pattern possible; located through the inner texture vtables")
vt("DX.DX11StructuredBuffer.vtbl", DX, ".?AVDX11StructuredBuffer@RenderAPI@@", ["par_upload.h:parupload::kVtableRva"],
   used_by="parupload")
slot("DX.DX11StructuredBuffer.update", "DX.DX11StructuredBuffer.vtbl", 2, ["par_upload.h:literal:0x32290"],
     used_by="parupload (hook)")
E(id="DX.DX11StructuredBuffer.update.dynamicCheck", module=DX, kind="instruction",
  src=["par_upload.h:literal:0x322a8"], checks=[B("83 79 44 03", note="cmp dword [rcx+0x44], 3 (kShape)")],
  anchor_hint="DX.DX11StructuredBuffer.update", used_by="parupload (verify)")
vt("DX.SConstantBuffer.vtbl", DX, ".?AUSConstantBuffer@D3DX11Effects@@", ["tex_bind.h:cbskip::kVtableRva"],
   used_by="cbskip")
slot("DX.SConstantBuffer.SetConstantBuffer", "DX.SConstantBuffer.vtbl", 31, ["tex_bind.h:literal:0x67970"],
     used_by="cbskip (hook)")
vt("DX.DX11ConstantBuffer.vtbl", DX, ".?AVDX11ConstantBuffer@RenderAPI@@", ["tex_bind.h:cbupload::kVtableRva"],
   used_by="cbupload")
slot("DX.DX11ConstantBuffer.update", "DX.DX11ConstantBuffer.vtbl", 2, ["tex_bind.h:literal:0xb970"],
     used_by="cbupload (hook)")
vt("DX.DX11Renderer.vtbl", DX, ".?AVDX11Renderer@RenderAPI@@",
   ["shadow_batch.h:shadowbatch::kRendererVtbl", "d3dstate.h:symbol:??_7DX11Renderer@RenderAPI@@6B@"],
   export="??_7DX11Renderer@RenderAPI@@6B@", rtti_checked=False,
   checks=[{"type": "vtable_len", "value": 92, "src": "shadow_batch.h:re:kVtblCopy = 92",
            "note": "shadowbatch copies exactly kVtblCopy entries"}],
   used_by="shadowbatch, gbbatch (vtable copy), d3ds (export)")
slot("DX.DX11Renderer.draw", "DX.DX11Renderer.vtbl", 35,
     ["shadow_batch.h:shadowbatch::kDrawRva", "d3dstate.h:symbol:?draw@DX11Renderer@RenderAPI@@UEAAXHPEAUIShader@2@W4PRIMTYPE_ENUM@render@@HHHPEBD@Z",
      "split_filter.h:sfilt::kDrawRva",
      "split_filter.h:symbol:?draw@DX11Renderer@RenderAPI@@UEAAXHPEAUIShader@2@W4PRIMTYPE_ENUM@render@@HHHPEBD@Z"],
     export="?draw@DX11Renderer@RenderAPI@@UEAAXHPEAUIShader@2@W4PRIMTYPE_ENUM@render@@HHHPEBD@Z",
     used_by="shadowbatch, gbbatch (DrawN in the vtable copy), d3ds (one-shot hook)")
# split_filter.h (R15 F5, [D3D] SplitFilter, default off): call sites rewritten
# to `nop; call stub`, FX call-table entries swapped. Fixed RVAs + exact FNV.
fn("DX.DX11Renderer.setVertexBuffers", DX,
   ["split_filter.h:sfilt::kSvbRva",
    "split_filter.h:symbol:?setVertexBuffers@DX11Renderer@RenderAPI@@IEAAXPEAVDX11Shader@2@AEAVDX11ShaderTechnique@2@H@Z"],
   export="?setVertexBuffers@DX11Renderer@RenderAPI@@IEAAXPEAVDX11Shader@2@AEAVDX11ShaderTechnique@2@H@Z",
   used_by="sfilt (hashed; IASetInputLayout sites)")
for _i, (_rva, _op, _anchor) in enumerate((
        (0x67ff1, "18 01", "DX.code.67f90"), (0x68022, "20 01", "DX.code.67f90"), (0x6804c, "58 01", "DX.code.67f90"),
        *((_r, "C0 00", "DX.DX11Renderer.draw") for _r in (0x149f2, 0x14a13, 0x14a38, 0x14a6a, 0x14a8f, 0x14abd,
                                                             0x14ade, 0x14b0d, 0x14b38)),
        (0x1ad61, "88 00", "DX.DX11Renderer.setVertexBuffers"), (0x1ada4, "88 00", "DX.DX11Renderer.setVertexBuffers"))):
    E(id=f"DX.sfilt.site{_i}", module=DX, kind="instruction", src=[f"split_filter.h:sfilt::kSites[{_i}].0"],
      checks=[B(f"FF 90 {_op} 00 00", alt=["90 E8"], note="call [rax+off] (context method); 90 E8 = our patch")],
      anchor=_anchor, used_by="sfilt (6-byte call-site patch)")
E(id="DX.FxCallTable.PS", module=DX, kind="data", src=["split_filter.h:sfilt::kPsTable"],
  used_by="sfilt (entries +0, +8, +0x10 swapped; thunk shapes checked)",
  note="static .data array of five ID3D11DeviceContext member-function pointers (vcall thunks) per stage")
E(id="DX.FxCallTable.VS", module=DX, kind="data", src=["split_filter.h:sfilt::kVsTable"],
  used_by="sfilt (entries +0, +8, +0x10 swapped; thunk shapes checked)")
slot("DX.DX11Renderer.getFramebuffer", "DX.DX11Renderer.vtbl", 54,
     ["gb_batch.h:literal:0x15d60", "gb_batch.h:gbbatch::kDxCode[2].0"], used_by="gbbatch (compared, hashed)")
slot("DX.DX11Renderer.getViewport", "DX.DX11Renderer.vtbl", 40,
     ["gb_batch.h:literal:0x17010", "gb_batch.h:gbbatch::kDxCode[3].0"], used_by="gbbatch (compared, hashed)")
v = vt("DX.FuncImpl.mipFinalize.vtbl", DX, None, ["motion_probes.h:mprobe::g_probes[1].0"],
       rtti_part="_Func_impl", used_by="mprobe")
slot("DX.FuncImpl.mipFinalize.doCall", v, 2, ["motion_probes.h:re:texture mip finalize"], compared=False,
     used_by="mprobe (hook)")
fn("DX.DX11Texture.getDesc", DX, ["shadow_tex.h:shadowtex::kGetDesc"], used_by="shadowtex, gbbatch (called, hashed)")
fn("DX.DX11Texture.compatible", DX, ["shadow_tex.h:shadowtex::kCompat"], used_by="shadowtex, gbbatch (called, hashed)")
fn("DX.DX11Texture.getSRV", DX, ["shadow_tex.h:shadowtex::kGetSrv"], used_by="shadowtex (called, hashed)")
fn("DX.FX.SetResource", DX, ["gb_batch.h:gbbatch::kSetResource", "gb_batch.h:gbbatch::kDxCode[1].0"],
   used_by="gbbatch (called, hashed)")
E(id="DX.DX11Shader.ctor.fieldStores", module=DX, kind="code_hash", src=["shadow_inst.h:shadowinst::kCtorFields"],
  used_by="shadowinst (hashed: object layout)")
fn("DX.DX11Shader.keyBuilder", DX, ["shadow_inst.h:shadowinst::kKeyBuilder"], used_by="shadowinst (hashed)")
fn("DX.DX11Shader.macroBuilder", DX, ["shadow_inst.h:shadowinst::kMacroBuilder"], used_by="shadowinst (hashed)")
E(id="DX.iat.pRenderApi", module=DX, kind="iat_import", import_=("renderer.dll", "?pRenderApi@RenderAPI@@3PEAUIRenderAPI@1@EA"),
  src=["gb_batch.h:gbbatch::kApiGlobal"], used_by="gbbatch (reads the global through dx11backend's IAT)")
E(id="DX.iat.ED_get_time", module=DX, kind="iat_import", import_=("edcore.dll", "ED_get_time"),
  src=["timer_cache.h:symbol:ED_get_time"], used_by="timercache (IAT hook)")

# ===========================================================================
# edCore.dll
# ===========================================================================
ED = "edCore.dll"
fn("ED.ED_get_time", ED, ["edtime.h:edtime::kGetTimeRva"], export="ED_get_time",
   checks=[B("48 83 EC 28 E8 77 FF FF",
             note="kGetTimePrologue; bytes 5..7 are a call displacement and change whenever the callee moves")],
   used_by="edtime (stub falls through to it)")
for i in range(4):
    E(id=f"ED.SyncTaskQueue.proceed.getTimeCall{i}", module=ED, kind="call_site",
      src=[f"edtime.h:edtime::kSites[{i}]"], container_export="?proceed@SyncTaskQueue@ed@@QEAANN@Z",
      call_target="ED.ED_get_time", call_index=i,
      checks=[B("E8"), {"type": "call_target", "target": "ED.ED_get_time"}],
      used_by="edtime (call rel32 rewrite)")
for name, users in (("?quick_get_memory@?$TFrameMemoryHeap@$00@@QEAAPEAX_K@Z", "frameheap (export + IAT hooks)"),
                    ("?reset_memory@?$TFrameMemoryHeap@$00@@QEAAXXZ", "frameheap (export + IAT hooks)")):
    short = name.split("@")[0].lstrip("?")
    E(id=f"ED.export.{short}", module=ED, kind="export", export=name, src=[f"frame_heap.h:symbol:{name}"], used_by=users)

# ===========================================================================
# GraphicsCore.dll
# ===========================================================================
GC = "GraphicsCore.dll"
vt("GC.GBufferPass.vtbl", GC,
   ".?AV?$TBaseRenderingPassWithData@UGBufferPassData@?1??addGBufferPass@DeferredShadingModule@render@@QEAAXAEAVRenderGraph@4@AEBUISceneRenderContextBinde",
   ["gb_batch.h:gbpass::kVtableRva"], prefix=True, used_by="gbpass (RttiIs compares the first 63 chars: kRtti)")
slot("GC.GBufferPass.execute", "GC.GBufferPass.vtbl", 19, ["gb_batch.h:gbpass::kThunkRva", "gb_batch.h:gbpass::kCode[0].0"],
     used_by="gbpass (hook)")
vt("GC.CascadeShadowPass.vtbl", GC,
   ".?AV?$TBaseRenderingPassWithData@UCascadeShadowPassData@?1??parseShadowMap@ShadowMapModule@render@@EEAAXHAEBVMatrixf@osg@@0@Z@V<lambda_1>@",
   ["shadow_pass.h:shadowpass::kVtableRva"], prefix=True, rtti_checked=False,
   used_by="shadowpass (no RTTI check in code; checked by slot target only)")
slot("GC.CascadeShadowPass.execute", "GC.CascadeShadowPass.vtbl", 19, ["shadow_pass.h:shadowpass::kThunkRva"],
     used_by="shadowpass (hook)")
E(id="GC.export.BaseRenderingPass_execute", module=GC, kind="export",
  export="?execute@BaseRenderingPass@render@@UEAAX_K@Z", src=["pass_timing.h:symbol:?execute@BaseRenderingPass@render@@UEAAX_K@Z"],
  used_by="passtiming (every .rdata pointer to it)")

# ===========================================================================
# Visualizer.dll
# ===========================================================================
VIS = "Visualizer.dll"
E(id="VIS.framePacer.pauses", module=VIS, kind="instruction", src=["pacer.h:pacer::kSiteRva"],
  checks=[B("F3 90 F3 90 F3 90 F3 90", alt=["FF 15"], note="kPauses; FF 15 = our own earlier patch"),
          B("0F B6 46 0C 90 84 C0 75", at=-9, note="kBefore"),
          B("48 8D 4C 24 70", at=8, note="kAfter")],
  used_by="pacer (8-byte call patch)")
vt("VIS.GraphSceneObject.vtbl", VIS, ".?AVGraphSceneObject@Graphics@@",
   ["part_weights.h:partw::kVtableRva", "part_weights.h:partw::g_targets[0].0"], col_offset=0x48,
   used_by="partw", note="secondary vtable (COL offset 0x48): the primary GraphSceneObject vtable is a different one")
slot("VIS.GraphSceneObject.collectRenderables", "VIS.GraphSceneObject.vtbl", 1, ["part_weights.h:re:kSlot = 1"],
     compared=False, used_by="partw (hook)")
slot("VIS.GraphSceneObject.partitionWeight", "VIS.GraphSceneObject.vtbl", 2, ["part_weights.h:re:SlotOriginal\\(&vtbl\\[2\\]\\)"],
     compared=False, used_by="partw (hook)")
v = vt("VIS.FuncImpl.renderThreadTaskDrain.vtbl", VIS, None, ["motion_probes.h:mprobe::g_probes[0].0"],
       rtti_part="_Func_impl", used_by="mprobe")
slot("VIS.FuncImpl.renderThreadTaskDrain.doCall", v, 2, ["motion_probes.h:re:frame-start task drain"], compared=False,
     used_by="mprobe (hook)")
E(id="VIS.iat.collectRenderablesAggregated", module=VIS, kind="iat_import",
  import_=("graphicscore.dll", "?collectRenderablesAggregated@SceneAggregator@Graphics@@YAXIQEAUCollectionRequest@12@"
                               "PEAVTaskQueue@ed@@_NPEAUCollectRenderablesResult@SceneStatistics@@@Z"),
  src=["collect_timing.h:symbol:?collectRenderablesAggregated@SceneAggregator@Graphics@@YAXIQEAUCollectionRequest@12@PEAVTaskQueue@ed@@_NPEAUCollectRenderablesResult@SceneStatistics@@@Z"],
  used_by="ctiming (IAT hook)")

# frame heap imports, one per importing module
_QG = "?quick_get_memory@?$TFrameMemoryHeap@$00@@QEAAPEAX_K@Z"
_RM = "?reset_memory@?$TFrameMemoryHeap@$00@@QEAAXXZ"
for mod, short in (("Scene.dll", "SCENE"), ("GraphicsVista.dll", "GV"), ("Visualizer.dll", "VIS")):
    E(id=f"{short}.iat.reset_memory", module=mod, kind="iat_import", import_=("edcore.dll", _RM),
      src=[f"frame_heap.h:symbol:{_RM}"], used_by="frameheap (IAT hook)")
for mod, short in (("NGModel.dll", "NG"), ("Scene.dll", "SCENE"), ("GraphicsVista.dll", "GV"),
                   ("GraphicsCore.dll", "GC"), ("Visualizer.dll", "VIS")):
    E(id=f"{short}.iat.quick_get_memory", module=mod, kind="iat_import", import_=("edcore.dll", _QG),
      src=[f"frame_heap.h:symbol:{_QG}"], used_by="frameheap (IAT hook)")

# ===========================================================================
# animator.dll, edterrainGraphics41.dll, metaShader.dll, renderer.dll
# ===========================================================================
AN = "animator.dll"
vt("ANIM.CharacterInstanceGraphicsReflection.vtbl", AN, ".?AVCharacterInstanceGraphicsReflection@Animator@@",
   ["part_weights.h:partw::g_targets[1].0"], used_by="partw")
slot("ANIM.CharacterInstanceGraphicsReflection.collectRenderables", "ANIM.CharacterInstanceGraphicsReflection.vtbl", 1,
     ["part_weights.h:re:kSlot = 1"], compared=False, used_by="partw (hook)")
slot("ANIM.CharacterInstanceGraphicsReflection.partitionWeight", "ANIM.CharacterInstanceGraphicsReflection.vtbl", 2,
     ["part_weights.h:re:SlotOriginal\\(&vtbl\\[2\\]\\)"], compared=False, used_by="partw (hook)")

TG = "edterrainGraphics41.dll"
vt("TERR.InstanceManager2.vtbl", TG, ".?AVInstanceManager2@edtg41@@", ["binder_count.h:imcount::kVtableRva"],
   used_by="imcount")
slot("TERR.InstanceManager2.markVisibility", "TERR.InstanceManager2.vtbl", 19, ["binder_count.h:literal:0x9fbf0"],
     used_by="imcount (hook)")
vt("TERR.TerrainRenderable.vtbl", TG, ".?AVTerrainRenderable@edtg41@@", ["terrain_count.h:terrcount::kVtableRva"],
   used_by="terrcount")
slot("TERR.TerrainRenderable.render", "TERR.TerrainRenderable.vtbl", 6, ["terrain_count.h:re:kSlot = 6"],
     compared=False, used_by="terrcount (hook)")
for i, (label, idx) in enumerate((("buildSpatial", 2), ("buildObjects", 3))):
    v = vt(f"TERR.FuncImpl.{label}.vtbl", TG, None, [f"motion_probes.h:mprobe::g_probes[{idx}].0"],
           rtti_part="_Func_impl", used_by="mprobe")
    slot(f"TERR.FuncImpl.{label}.doCall", v, 2, [f"motion_probes.h:re:terrain build {'spatial' if i == 0 else 'objects'}"],
         compared=False, used_by="mprobe (hook)")

MS = "metaShader.dll"
vt("MS.MetaShaderImpl2.vtbl", MS, ".?AVMetaShaderImpl2@render@@", ["binder_count.h:bindcount::kVtableRva"],
   used_by="bindcount")
slot("MS.MetaShaderImpl2.setupParams", "MS.MetaShaderImpl2.vtbl", 14, ["binder_count.h:re:vtbl\\[14\\]"],
     compared=False, used_by="bindcount (hook)")
slot("MS.MetaShaderImpl2.render", "MS.MetaShaderImpl2.vtbl", 15, ["binder_count.h:re:vtbl\\[15\\]"],
     compared=False, used_by="bindcount (hook)")

E(id="REND.export.pRenderApi", module="renderer.dll", kind="export",
  export="?pRenderApi@RenderAPI@@3PEAUIRenderAPI@1@EA", src=["shadow_batch.h:symbol:?pRenderApi@RenderAPI@@3PEAUIRenderAPI@1@EA"],
  used_by="shadowbatch (finds the DX11Renderer object)")

# ===========================================================================
# ModelDesc.dll
# ===========================================================================
MD = "ModelDesc.dll"
for i, rtti in enumerate((".?AV?$AnimatedProperty@M@model@@", ".?AV?$AnimatedProperty@VVec2f@osg@@@model@@",
                          ".?AV?$AnimatedProperty@VVec3f@osg@@@model@@", ".?AV?$AnimatedProperty@VVec4f@osg@@@model@@",
                          ".?AVArgumentProperty@model@@")):
    name = ("AnimatedProperty_float", "AnimatedProperty_Vec2f", "AnimatedProperty_Vec3f", "AnimatedProperty_Vec4f",
            "ArgumentProperty")[i]
    v = vt(f"MD.{name}.vtbl", MD, rtti, [f"gb_batch.h:gbbatch::kProps[{i}].0"], used_by="gbbatch")
    slot(f"MD.{name}.evaluate", v, 9, [f"gb_batch.h:gbbatch::kProps[{i}].1", f"gb_batch.h:gbbatch::kMdCode[{i}].0"],
         used_by="gbbatch (compared, hashed)")
for name in ("?getTexture@PropertiesSet@model@@QEBAAEBUTexture2dProperties@2@I@Z", "?valid@Texture2dProperties@model@@QEBA_NXZ"):
    short = name.split("@")[0].lstrip("?") + "_" + name.split("@")[1]
    E(id=f"MD.export.{short}", module=MD, kind="export", export=name, src=[f"inst_count.h:symbol:{name}"],
      used_by="instcount (GetProcAddress)")

# ===========================================================================
# Scene.dll
# ===========================================================================
SC = "Scene.dll"
_COLL = ["$0A@", "$00", "$01", "$02", "$03", "$04", "$05"]
for i in range(7):
    v = vt(f"SCENE.CollectTask{i}.vtbl", SC,
           ".?AV?$_Func_impl_no_alloc@V<lambda_1>@?L@???$collectSceneObjectsInFrustum2@" + _COLL[i] +
           "@?$forEachCollection@$06@?$SceneBase@UObjectCollections@DCSSceneCollections@@",
           [f"task_timing.h:ttiming::kSites[{i}].0"], prefix=True,
           used_by="ttiming (RTTI checked by substring collectSceneObjectsInFrustum2)")
    slot(f"SCENE.CollectTask{i}.doCall", v, 2, [f"task_timing.h:ttiming::kSites[{i}].1"], used_by="ttiming (hook)")
_SB = "?$SceneBase@UObjectCollections@DCSSceneCollections@@ULightCollections@2@@Graphics@@"
E(id="SCENE.export.DCSScene_IView_vtbl", module=SC, kind="export", export="??_7DCSScene@@6BIView@SceneAggregator@Graphics@@@",
  src=["main.cpp:symbol:??_7DCSScene@@6BIView@SceneAggregator@Graphics@@@",
       "collect_timing.h:symbol:??_7DCSScene@@6BIView@SceneAggregator@Graphics@@@"],
  slots={3: "?collectSceneObjectsRenderables@" + _SB + "MEBAXIQEBUCollectionInfo@render@@QEAV?$vector@"
             "PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@PEAVTaskQueue@6@I@Z",
         4: "?sortAndBatchRenderables@" + _SB + "MEBAXIQEBUCollectionInfo@render@@QEAV?$vector@"
            "PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@PEAVTaskQueue@6@@Z"},
  used_by="main Install (slot 3 hook), ctiming (slot 4 hook)")
E(id="SCENE.export.collectSceneObjectsRenderables", module=SC, kind="export",
  export="?collectSceneObjectsRenderables@" + _SB + "MEBAXIQEBUCollectionInfo@render@@QEAV?$vector@"
         "PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@PEAVTaskQueue@6@I@Z",
  src=["main.cpp:symbol:?collectSceneObjectsRenderables@" + _SB + "MEBAXIQEBUCollectionInfo@render@@QEAV?$vector@"
       "PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@PEAVTaskQueue@6@I@Z"],
  used_by="main Install (expected IView slot 3)")
E(id="SCENE.export.sortAndBatchRenderables", module=SC, kind="export",
  export="?sortAndBatchRenderables@" + _SB + "MEBAXIQEBUCollectionInfo@render@@QEAV?$vector@"
         "PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@PEAVTaskQueue@6@@Z",
  src=["collect_timing.h:symbol:?sortAndBatchRenderables@" + _SB + "MEBAXIQEBUCollectionInfo@render@@QEAV?$vector@"
       "PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@PEAVTaskQueue@6@@Z"],
  used_by="ctiming (expected IView slot 4)")
E(id="SCENE.export.DCSScene_ISceneObjectsProvider_vtbl", module=SC, kind="export",
  export="??_7DCSScene@@6BISceneObjectsProvider@render@@@",
  src=["collect_timing.h:symbol:??_7DCSScene@@6BISceneObjectsProvider@render@@@"],
  slots={1: "?collectRenderables@" + _SB + "UEBA_NIQEBUCollectionInfo@render@@QEAV?$vector@PEAUISceneRenderable@render@@"
             "V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@PEAVTaskQueue@6@I@Z"},
  used_by="ctiming (slot 1 hook)")
E(id="SCENE.export.collectRenderables", module=SC, kind="export",
  export="?collectRenderables@" + _SB + "UEBA_NIQEBUCollectionInfo@render@@QEAV?$vector@PEAUISceneRenderable@render@@"
         "V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@PEAVTaskQueue@6@I@Z",
  src=["collect_timing.h:symbol:?collectRenderables@" + _SB + "UEBA_NIQEBUCollectionInfo@render@@QEAV?$vector@"
       "PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@PEAVTaskQueue@6@I@Z"],
  used_by="ctiming (expected provider slot 1)")
E(id="SR.iat.BaseRenderingPass_execute", module="SceneRenderer.dll", kind="iat_import",
  import_=("graphicscore.dll", "?execute@BaseRenderingPass@render@@UEAAX_K@Z"),
  src=["pass_timing.h:symbol:?execute@BaseRenderingPass@render@@UEAAX_K@Z"], used_by="passtiming (IAT hook)")
E(id="NG.export.ParseMT2", module=NG, kind="export",
  export="?ParseMT2@NGModelLodsMT@model@@UEAAXAEAVModelInstance@Graphics@@IPEAPEBUCollectionInfo@render@@"
         "QEB_NPEAPEAV?$vector@PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@"
         "AEAUILightProbeSampler@6@AEBV?$wPosition3@M@@@Z",
  src=["model_timing.h:symbol:?ParseMT2@NGModelLodsMT@model@@UEAAXAEAVModelInstance@Graphics@@IPEAPEBUCollectionInfo@render@@"
       "QEB_NPEAPEAV?$vector@PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@"
       "AEAUILightProbeSampler@6@AEBV?$wPosition3@M@@@Z"],
  used_by="mtiming (expected NGModelLodsMT slot 31)")

# ===========================================================================
# Not a DCS module
# ===========================================================================
E(id="EXT.QVFR.xrLocateViews", module="XR_APILAYER_MBUCCHIA_quad_views_foveated.dll", kind="external",
  src=["pose_sweep.h:posesweep::kRva"],
  checks=[B("40 53 56 57 41 54 41 56 41 57 48 81 EC E8 00 00 00", note="kPrologue")],
  used_by="posesweep (test-only detour)",
  note="Quad-Views-Foveated API layer bundled with DcsVrControl, not part of DCS; checked only when --qvfr is given")

# Source constants that look like locations but are not (scanner noise).
IGNORE = {
    # D3DX11Effects shader-block member offsets per stage (struct layout, not addresses)
    *{("fx_apply_count.h", f"fxapply::kStageOff[{i}]") for i in range(6)},
}
