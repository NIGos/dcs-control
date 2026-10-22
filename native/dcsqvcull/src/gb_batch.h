// G-buffer instancing, stage 2 (R13): one instanced draw per group of
// identical opaque model draws in each G-buffer pass execution.
//
// Tags: [V] verified in the binary (DCS 2.9.30 GraphicsCore / NGModel /
// ModelDesc / dx11backend), [I] inferred, [M] measured.
//
// Pass [V]: GraphicsCore vtable 0xd8770 (TBaseRenderingPassWithData<
//   GBufferPassData, addGBufferPass ...>) slot 19 = thunk 0x8b9e0 ->
//   0x88de0(pass+0x90, pass+0x48, ctx). Slot 18 is BaseRenderingPass::execute
//   (pass timing redirects that one), so slot 19 has a single owner: gbpass
//   below, which calls an optional wrapper. Item vector [V 0x88ecc-0x88efc]:
//   [[ctx]+0x438] + word[[pass+0x88]+0x38] * 24 (begin, end), read once
//   before the loop 0x89700: (*p)->vt[1](p, pass+0x98), result ignored.
// Item [V 0x44350]: SceneRenderable vt[1]: pass [r+0x60] in {1,2} ->
//   lock add [globals+0x1e8], [r+0x78]; then ModelMaterialMT vt[4] 0x16140
//   (mat, item=[r+0x10], &{1,1,1e5}, pass, mesh [item+0xc0], mesh2
//   [item+0xc8], &ctx = r+0x64, &size = r+0x6c).
// Slot 4 0x16140 [V], what it does besides D3D state:
//   1. mat+0x18c = item+0xd4;
//   2. DX11Shader vt[27] 0x1fe70(h=[mat+0x68], buf): when the flag
//      [[dx+0xb0898]+0x2120] & 0x20 is clear and buf (page [item+0xd0] of
//      StructBufferManager, 0xc310) is set: var->SetResource([buf+0x30]);
//   3. 0xeb60(mat, copy of item[0..16]): for each {dst, prop} at
//      [mat+8..mat+0x10) (stride 16): prop->vt[9](prop, span, dst). The five
//      ModelDesc classes (AnimatedProperty<float|Vec2f|Vec3f|Vec4f>,
//      ArgumentProperty) read only prop and args[[prop+0x18]] and write only
//      *dst (4/8/12/16/4 bytes) [V ModelDesc 0xa800-0xa8c0, 0xafb0, 0xcc20,
//      0xcd40, 0xce00, 0xcf00]; a keyframe list of 0 entries writes nothing.
//      So the CB bytes they produce are fixed by the argument values: the
//      group key holds those 32-bit values (exact compare);
//   4. 0xcf80: DX11Shader slot 26 per handle h != -1 (see PredictView);
//   5. 0x15e20 for model pass 1, opaque (props+0x33 == 0): mat+0xb0..0xef =
//      item+0x60..0x9c (prevFrameTransform, proven unused by the stage-1
//      gate), mat+0x110 = 1.0, mat+0x114/0x118 = viewport / framebuffer
//      ratios (constant while the framebuffer and viewport stay the same;
//      checked per member, see RendererState), technique [mat+0x1d8] P0 when
//      ctx byte 0 (cockpit) is 0; then the submit 0x131e0 (CB upload, draw).
//
// Group key (exact): material, mesh, mesh2, page [item+0xd0], the predicted
// view of every bound texture entry, the animated argument values. Draws
// whose view cannot be predicted (null texture, swap pending, unknown class,
// [tex+0x678] with another type than 0x20/0x21) are solo.
//
// Ordering: opaque G-buffer draws (BM_NONE: no blend, depth GREATER_EQUAL
// with writes) commute except at exact depth ties. A member moves to its
// leader's position; it must not move across
//   - a draw that may blend or is not known to be such an opaque draw
//     (barrier: any other renderable class or material, another pass number,
//     transparent, cockpit, BLEND_MODE != BM_NONE, shader not yet mapped);
//   - a draw of the same object (same [item+0x18], the object's texture
//     entry vector, shared by all items of one model instance [V 0x1ce29]),
//     unless that draw is in the same group (R13 rule R).
//
// Members (skipped) still make: the triangle counter add, and per bound
// texture tex->vt[23](member size) through the vtable (streaming request).
// Restores after the pass (stock leaves the last draw's state):
//   - every touched material: mat+0x18c = last item's offset; when the last
//     item was skipped, mat+0xb0..0xef from it and 0xeb60 for it;
//   - every effect variable whose last writer in original order was a
//     skipped member: var->SetResource(that member's predicted value).
// A shader with a draw that leaves a texture variable unset (null texture
// entry or no sbPositions buffer) in this pass is not batched in it (a later
// read would see a different left-over value).
//
// Leader: the original vt[1] with item+0xd4 = base, our instanced model_vs in
// [[fxpass+0xc0]+0x18] of normal* P0, t127 = our offsets, instance count via
// shadow_batch.h's renderer vtable copy. Just before the draw (DrawN check)
// the views actually bound are compared with the prediction; on a difference
// the leader draws alone and the members draw themselves.
//
// Included once from main.cpp inside its anonymous namespace, after
// gb_count.h, shadow_tex.h, shadow_inst.h and shadow_batch.h.
#pragma once

namespace gbpass {

constexpr uint32_t kVtableRva = 0xd8770;
constexpr int kExecSlot = 19;
constexpr uint32_t kThunkRva = 0x8b9e0;
constexpr const char* kRtti = ".?AV?$TBaseRenderingPassWithData@UGBufferPassData@?1??addGBuffe";  // first 63 chars
struct Code {
  uint32_t begin, end;
  uint64_t hash;
};
constexpr Code kCode[] = {
    {0x8b9e0, 0x8b9f3, 0x4510ed053ea6cb03ull},  // slot 19 thunk
    {0x88ecc, 0x88efc, 0x2503ea11534e7a33ull},  // item vector
    {0x896e1, 0x8971a, 0x69b08482daa8b795ull},  // item loop
};

using ExecFn = void(__fastcall*)(void* pass, void* ctx);
ExecFn g_orig = nullptr;
// Optional wrapper (gbbatch): calls orig itself (possibly twice when verifying).
using WrapFn = void (*)(void* pass, void* ctx, ExecFn orig);
std::atomic<WrapFn> g_wrap{nullptr};
int g_state = 0;  // 0 = not tried, 1 = hooked, -1 = build mismatch
// Optional observer around the whole execution, before any wrapper
// (cockpit_ipd.h, [Perception] CockpitTrueIpd: classifies the execution
// before DCS's binder uploads cPerView inside it, restores after it).
// nullptr unless that option is on: then Hook is exactly as before.
using ObsFn = void (*)(void* pass, void* ctx, bool begin);
std::atomic<ObsFn> g_obs{nullptr};
// R123 (focus_twin.h, [Model] FocusTwin): a second observer around the whole execution, outermost; nullptr unless
// the focus twin is installed (then Hook is exactly as before plus this call before and after).
std::atomic<ObsFn> g_obs2{nullptr};

void __fastcall HookBody(void* pass, void* ctx) {
  const ObsFn obs = g_obs.load(std::memory_order_acquire);
  if (!obs) {
    if (WrapFn w = g_wrap.load(std::memory_order_relaxed)) return w(pass, ctx, g_orig);
    g_orig(pass, ctx);
    return;
  }
  obs(pass, ctx, true);
  if (WrapFn w = g_wrap.load(std::memory_order_relaxed))
    w(pass, ctx, g_orig);
  else
    g_orig(pass, ctx);
  obs(pass, ctx, false);
}
void __fastcall Hook(void* pass, void* ctx) {
  const ObsFn obs2 = g_obs2.load(std::memory_order_acquire);
  if (!obs2) return HookBody(pass, ctx);
  obs2(pass, ctx, true);
  HookBody(pass, ctx);
  obs2(pass, ctx, false);
}

// The pass's item vector {begin, end, cap}, or nullptr (empty static vector).
void** ItemVector(void* pass, void* ctx) {
  __try {
    auto* node = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(pass) + 0x48 + 0x40);
    if (!node) return nullptr;
    const int16_t idx = *reinterpret_cast<int16_t*>(node + 0x38);
    if (idx == -1) return nullptr;
    auto* base = *reinterpret_cast<uint8_t**>(*static_cast<uint8_t**>(ctx) + 0x438);
    if (!base) return nullptr;
    return reinterpret_cast<void**>(base + static_cast<int64_t>(idx) * 24);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

const char* VerifyBuild(uint8_t* gc) {
  auto** vtbl = reinterpret_cast<void**>(gc + kVtableRva);
  if (!allocslab::RttiIs(gc, vtbl, kRtti) || reinterpret_cast<uint8_t*>(SlotOriginal(&vtbl[kExecSlot])) != gc + kThunkRva)
    return "g-buffer pass: GraphicsCore G-buffer pass vtable does not match this build";
  for (const Code& c : kCode)
    if (!shadowtex::CodeIs(gc, c.begin, c.end, c.hash)) return "g-buffer pass: GraphicsCore pass code does not match this build";
  return nullptr;
}

bool Install() {
  if (g_state != 0) return g_state > 0;
  auto* gc = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"GraphicsCore.dll"));
  if (!gc) return false;
  g_state = -1;
  if (const char* why = VerifyBuild(gc)) {
    Log("%s; skipped", why);
    return false;
  }
  auto** vtbl = reinterpret_cast<void**>(gc + kVtableRva);
  if (!HookSlot(&vtbl[kExecSlot], reinterpret_cast<void*>(&Hook), reinterpret_cast<void**>(&g_orig))) return false;
  g_state = 1;
  Log("g-buffer pass: hooked execute (slot 19)");
  return true;
}

// Slot 19 back to DCS's thunk, for a measurement phase that installed the
// hook itself (gb_rec_count.h) and only when nothing wraps it any more;
// g_orig stays valid for calls in flight.
void Uninstall() {
  if (g_state != 1 || g_wrap.load()) return;
  auto* gc = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"GraphicsCore.dll"));
  if (!gc) return;
  UnhookSlot(&reinterpret_cast<void**>(gc + kVtableRva)[kExecSlot], reinterpret_cast<void*>(g_orig));
  g_state = 0;
}

}  // namespace gbpass

namespace gbbatch {

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------
constexpr uint32_t kSrVtbl = 0x59810;        // NGModel SceneRenderable
constexpr uint32_t kModelMatVtbl = 0x592f0;  // NGModel ModelMaterialMT
constexpr uint32_t kModelDraw = 0x16140;     // ModelMaterialMT vt[4]
constexpr uint32_t kGlobals = 0x72e50;       // model::globals (GlobalsMT*)
constexpr uint32_t kEvalAnim = 0xeb60;
constexpr uint32_t kTriSite = 0x44371;       // lock add (or tricount's plain add)
constexpr gbpass::Code kNgCode[] = {
    {0x44350, 0x44371, 0x62507992c340cb98ull},  // SceneRenderable vt[1] up to the counter add
    {0x44372, 0x443dc, 0xb2afd5dce29e49cfull},  // ... the rest (byte 0x44371 = F0 or 3E)
    {0x16140, 0x161e5, 0xd3fe18746d88136dull},  // slot 4: offset, sbPositions, 0xeb60, 0xcf80
    {0x16433, 0x1647c, 0xb6f40efc7d4a2849ull},  // slot 4: default case -> 0x15e20, epilogue
    {0x15e20, 0x15fd1, 0x151925c8ef3a0160ull},  // 0x15e20: CB writes, renderer getters
    {0x15fd1, 0x16134, 0x1675bab468ed1984ull},  // 0x15e20: technique / pass selection
    {0x0eb60, 0x0ebb3, 0x913bcabc38ea3e7cull},  // animated uniforms
    {0x0cf80, 0x0d00f, 0xd4bf672b29c07178ull},  // texture loop
    {0x0c310, 0x0c326, 0x9eebbcdef6f4a55dull},  // page -> buffer slot
    {0x131e0, 0x13315, 0xfa5b29e55d76ca0cull},  // submit
};
// dx11backend
constexpr uint32_t kApiGlobal = 0xb0898;  // read by DX11Shader vt[27]
constexpr uint32_t kSetResource = 0x61ca0;
constexpr gbpass::Code kDxCode[] = {
    {0x1fe70, 0x1feab, 0x186c5cd4732ccec6ull},  // DX11Shader vt[27] (sbPositions)
    {0x61ca0, 0x61cab, 0x5df236952cef1f6aull},  // FX SetResource: *[var+8] = srv
    {0x15d60, 0x15d9c, 0x84a869acd4c71803ull},  // DX11Renderer vt[54] framebuffer getter
    {0x17010, 0x1706c, 0xd0126d4a60420dbeull},  // DX11Renderer vt[40] viewport getter
};
// ModelDesc animated property classes: vtable, vt[9], bytes written.
struct PropClass {
  uint32_t vtbl, slot9, bytes;
  const char* rtti;
};
constexpr PropClass kProps[] = {
    {0x44138, 0xa800, 4, ".?AV?$AnimatedProperty@M@model@@"},
    {0x441a0, 0xa840, 8, ".?AV?$AnimatedProperty@VVec2f@osg@@@model@@"},
    {0x44208, 0xa880, 12, ".?AV?$AnimatedProperty@VVec3f@osg@@@model@@"},
    {0x44270, 0xa8c0, 16, ".?AV?$AnimatedProperty@VVec4f@osg@@@model@@"},
    {0x440d0, 0xafb0, 4, ".?AVArgumentProperty@model@@"},
};
constexpr int kPropClasses = 5;
constexpr gbpass::Code kMdCode[] = {
    {0xa800, 0xa816, 0x377bf8b22caea856ull}, {0xa840, 0xa856, 0x3a25f48711d05fd6ull},
    {0xa880, 0xa896, 0x8ede453bdbf4a884ull}, {0xa8c0, 0xa8d6, 0x9bec1972e8d231e3ull},
    {0xafb0, 0xafbd, 0x03dca208683a958eull}, {0xcd40, 0xcdf1, 0x12f3d0ed0560a2adull},
    {0xce00, 0xcef6, 0x8a744f23efb4a832ull}, {0xcc20, 0xcd32, 0x9f021b33612bf51full},
    {0xcf00, 0xd08a, 0x4e5ad9ac30cdd410ull},
};

constexpr uint32_t kMaxItems = 1 << 14;  // items per pass execution
constexpr size_t kTable = 1 << 15;       // hash tables (power of two, >= 2 * kMaxItems)
constexpr uint32_t kPool = 1 << 19;      // predicted views + argument values per pass
constexpr uint32_t kMaxOffsets = 1 << 16;
constexpr uint32_t kMaxTex = 32, kMaxAnim = 64;

// ---------------------------------------------------------------------------
// Solo reasons (counters). 1..8 are barriers: no member moves across them.
// ---------------------------------------------------------------------------
enum Reason : uint8_t {
  kOk = 0,
  kForeign,       // not an NGModel SceneRenderable
  kOtherMat,      // material is not ModelMaterialMT
  kPassNum,       // model pass number != 1
  kTransparent,   // props+0x33: forward technique pass
  kCockpit,       // normal_cockpit* (ctx byte 0)
  kNotMapped,     // shader not (yet) compiled and gated: blend mode unknown
  kBlend,         // BLEND_MODE != BM_NONE (decal, alpha test, transparent, additive)
  kNotDeferred,   // normal* P0 is not the G-buffer PS
  kNoVariant,     // no instanced VS, or prevFrameTransform used / posStructOffset read outside the VS
  kTooManyTex,
  kNullTex,       // a bound entry has no texture: slot 26 leaves the variable as it was
  kTexClass,      // texture or inner class not replicated
  kSwapPending,   // getSRV would swap the streamed mip sets (0x49ca0)
  kTex678,        // [tex+0x678] path with a type other than 0x20/0x21
  kMipSet,        // mip-set scan outside the analysed shape
  kAnimClass,     // animated property class not replicated
  kAnimRange,     // argument index >= count, or dst outside the CB / over posStructOffset
  kNullBind,      // another draw of the shader in this pass leaves a variable unset
  kAlone,         // eligible, no other draw with the same key
  kPoolFull,
  kReasons
};
const char* const kReasonName[kReasons] = {
    "grouped",       "other renderable", "other material", "pass number",      "transparent",
    "cockpit",       "shader not mapped", "blend mode",    "not deferred P0",  "no variant",
    "too many textures", "null texture", "texture class",  "swap pending",     "tex+0x678 path",
    "mip-set shape", "animated class",  "animated range",  "unset variable in pass", "alone",
    "pool full"};
inline bool IsBarrier(uint8_t r) { return r >= kForeign && r <= kNotDeferred; }

// ---------------------------------------------------------------------------
// View prediction: DX11Shader slot 26 0x1fd70 and getSRV 0x47c60 (pure)
// ---------------------------------------------------------------------------
// slot 26 (shader, h, tex, aux, &size) [V]:
//   tex == 0 -> return (variable unchanged);
//   rec = [shader+0xc8] + h*0x50; tex->vt[23](size);
//   if (!compat([rec+0xc], [getDesc(tex)+0x20])) { var->SetResource(0); return; }
//   tex->vt[18]();   (File/Array/Dummy inner classes: inner vt[8] = ret)
//   [tex+0x678] == 0 ? SetResource(getSRV(tex, aux, size))
//     : type in {0x20, 0x21} ? SetResource(getSRV(tex, aux, &0)) : SetResource(0).
// getSRV [V]:
//   aux != -1 -> [[tex+0x90] + aux*24 + 0x10];
//   [tex+0x190] != 0 -> it;
//   P = [tex+0x1a0] ready (state [+0x40] == 5, [+0x20] != 0) -> swap 0x49ca0 (not predicted);
//   S = [tex+0x198] not ready -> [tex+0x1b8] ?: [tex+0x80];
//   else area = int32(size.x*size.y), views [S+8..S+0x10), thresholds int32
//   [S+0x28..S+0x30): empty views -> 0; first i with area >= thr[i] -> views[i];
//   none (or no thresholds) -> views[0].
using GetDescFn = const uint8_t*(__fastcall*)(void* tex);
using CompatFn = bool(__fastcall*)(int32_t type, int32_t format);
struct TexEnv {
  const void* texVtbl = nullptr;
  const void* inner[3] = {};
  GetDescFn getDesc = nullptr;
  CompatFn compat = nullptr;
};

inline bool SetReady(const uint8_t* set) {
  return reinterpret_cast<const std::atomic<int32_t>*>(set + 0x40)->load(std::memory_order_acquire) == 5 &&
         *reinterpret_cast<void* const*>(set + 0x20) != nullptr;
}

inline uint8_t PredictSrv(const uint8_t* tex, int64_t aux, uint64_t size, void** view) {
  if (aux != -1) {
    const uint8_t* views = *reinterpret_cast<uint8_t* const*>(tex + 0x90);
    *view = *reinterpret_cast<void* const*>(views + aux * 24 + 0x10);
    return kOk;
  }
  if (void* v = *reinterpret_cast<void* const*>(tex + 0x190)) {
    *view = v;
    return kOk;
  }
  const uint8_t* s = *reinterpret_cast<uint8_t* const*>(tex + 0x198);
  const uint8_t* p = *reinterpret_cast<uint8_t* const*>(tex + 0x1a0);
  if (!s || !p) return kMipSet;
  if (SetReady(p)) return kSwapPending;
  if (!SetReady(s)) {
    void* v = *reinterpret_cast<void* const*>(tex + 0x1b8);
    *view = v ? v : *reinterpret_cast<void* const*>(tex + 0x80);
    return kOk;
  }
  const int32_t area = static_cast<int32_t>(static_cast<uint32_t>(size) * static_cast<uint32_t>(size >> 32));
  void* const* b = *reinterpret_cast<void* const* const*>(s + 8);
  void* const* e = *reinterpret_cast<void* const* const*>(s + 0x10);
  if (b == e) {
    *view = nullptr;
    return kOk;
  }
  const int32_t* thr = *reinterpret_cast<const int32_t* const*>(s + 0x28);
  const int64_t cnt =
      (reinterpret_cast<intptr_t>(*reinterpret_cast<const int32_t* const*>(s + 0x30)) - reinterpret_cast<intptr_t>(thr)) >> 2;
  const int64_t nviews = e - b;
  if (cnt < 0 || cnt > 256 || nviews <= 0) return kMipSet;
  for (int64_t i = 0; i < cnt; ++i)
    if (area >= thr[i]) {
      if (i >= nviews) return kMipSet;  // DCS would read past the view vector
      *view = b[i];
      return kOk;
    }
  *view = b[0];
  return kOk;
}

inline uint8_t PredictView(const TexEnv& env, uint8_t* tex, int64_t aux, int32_t type, uint64_t size, void** view) {
  if (!tex) return kNullTex;
  if (*reinterpret_cast<void**>(tex) != env.texVtbl) return kTexClass;
  void* inner = *reinterpret_cast<void**>(tex + 0x10);
  void* iv = inner ? *static_cast<void**>(inner) : nullptr;
  if (!iv || (iv != env.inner[0] && iv != env.inner[1] && iv != env.inner[2])) return kTexClass;
  const uint8_t* desc = env.getDesc(tex);
  if (!env.compat(type, *reinterpret_cast<const int32_t*>(desc + 0x20))) {
    *view = nullptr;
    return kOk;
  }
  if (*reinterpret_cast<void**>(tex + 0x678)) {
    if (type == 0x20 || type == 0x21) return PredictSrv(tex, aux, 0, view);
    return kTex678;
  }
  return PredictSrv(tex, aux, size, view);
}

// ---------------------------------------------------------------------------
// Animated uniforms: argument values that fix 0xeb60's output (pure)
// ---------------------------------------------------------------------------
struct AnimEnv {
  const void* vtbl[kPropClasses] = {};
  uint32_t bytes[kPropClasses] = {};
};

inline uint8_t AnimInputs(const AnimEnv& env, uint8_t* mat, uint8_t* item, uint32_t* out, uint32_t max, uint32_t* n) {
  uint8_t* b = *reinterpret_cast<uint8_t**>(mat + 8);
  uint8_t* e = *reinterpret_cast<uint8_t**>(mat + 0x10);
  const intptr_t bytes = e - b;
  if (bytes < 0 || bytes % 16 || static_cast<uintptr_t>(bytes / 16) > max) return kAnimRange;
  const uint32_t* args = *reinterpret_cast<uint32_t**>(item);
  const uint64_t nargs = *reinterpret_cast<uint64_t*>(item + 8);
  uint32_t k = 0;
  for (uint8_t* p = b; p < e; p += 16, ++k) {
    uint8_t* dst = *reinterpret_cast<uint8_t**>(p);
    uint8_t* prop = *reinterpret_cast<uint8_t**>(p + 8);
    const void* vt = prop ? *reinterpret_cast<void**>(prop) : nullptr;
    int c = 0;
    while (c < kPropClasses && env.vtbl[c] != vt) ++c;
    if (!vt || c == kPropClasses) return kAnimClass;
    const intptr_t off = dst - mat;
    const intptr_t end = off + static_cast<intptr_t>(env.bytes[c]);
    if (off < 0x90 || end > 0x1c0 || (off < 0x190 && end > 0x18c)) return kAnimRange;
    const uint32_t idx = *reinterpret_cast<uint32_t*>(prop + 0x18);
    if (idx >= nargs) return kAnimRange;
    out[k] = args[idx];
  }
  *n = k;
  return kOk;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
std::atomic<bool> g_on{false};
std::atomic<bool> g_verify{false};
std::atomic<int> g_state{0};  // 0 = not tried, 1 = ready, -1 = unavailable
std::atomic<bool> g_disabled{false};
std::atomic<bool> g_shutdown{false};
std::atomic<int> g_inWrap{0};
void Disable(const char* why) {
  if (!g_disabled.exchange(true)) Log("g-buffer batching: disabled for this session (%s)", why);
}

uint8_t* g_ng = nullptr;
uint8_t* g_dx = nullptr;
void* g_srVtbl = nullptr;
void* g_modelMatVtbl = nullptr;
void* g_shaderVtbl = nullptr;
void* g_setResource = nullptr;
TexEnv g_texEnv;
AnimEnv g_animEnv;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11Buffer* g_buf = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
DWORD g_renderThread = 0;

using Tex23Fn = uint64_t(__fastcall*)(void* tex, uint64_t packedSize);
using EvalAnimFn = void(__fastcall*)(void* mat, const void* span);
using SetResFn = long(__fastcall*)(void* var, void* srv);
using FbFn = void*(__fastcall*)(void* renderer);
using VpFn = int32_t*(__fastcall*)(void* renderer, int32_t* out);
EvalAnimFn g_evalAnim = nullptr;

// Counters (render thread writes; the suite reads).
std::atomic<uint64_t> g_passes{0}, g_items{0}, g_groups{0}, g_leaders{0}, g_skipped{0}, g_fallbacks{0},
    g_tooLarge{0}, g_splitBarrier{0}, g_splitObject{0}, g_leaderMismatch{0}, g_rendererChanged{0},
    g_matRestores{0}, g_varRestores{0};
std::atomic<uint64_t> g_reason[kReasons];

// ---- Plan (per pass execution) ----
enum : uint8_t { kSolo = 0, kLeader = 1, kMember = 2 };
struct Rec {
  uint8_t* r;
  uint8_t* item;
  uint8_t* mat;
  uint8_t* shader;
  void* obj;
  uint8_t reason, role, tracked, nView, nAnim;
  uint32_t viewOff, animOff;
  int32_t group, next;
  uint64_t viewHash, animHash;
};
struct Group {
  int32_t first, last;
  uint32_t count, epoch, base;
  uint8_t failed;  // the leader drew alone: members draw themselves
};
struct PtrSlot {  // generic pointer -> int table entry
  const void* key;
  uint32_t gen;
  int32_t a, b;
};
Rec g_rec[kMaxItems];
Group g_grp[kMaxItems];
uint32_t g_grpCount = 0;
uint64_t g_pool[kPool];
uint32_t g_poolUsed = 0;
uint32_t g_offsets[kMaxOffsets];
uint32_t g_n = 0;
uint32_t g_gen = 0;
PtrSlot g_byR[kTable];      // renderable -> index
PtrSlot g_byKey[kTable];    // key hash slot -> group (key = first member's fields)
PtrSlot g_byObj[kTable];    // object -> (last index, group)
PtrSlot g_byMat[kTable];    // material -> last index
PtrSlot g_nullBind[4096];   // shaders with an unset variable in this pass
PtrSlot g_involved[4096];   // shaders of skipped members (restore)
PtrSlot g_byVar[kTable];    // effect variable -> (last writer index, entry or -1)
uint32_t g_matList[kMaxItems];
uint32_t g_matCount = 0;
uint64_t g_keyHash[kTable];  // full hash per g_byKey slot
bool g_planActive = false;
bool g_batching = false;
bool g_firstItem = false;
void* g_fb0 = nullptr;
int32_t g_vp0[2] = {};
uint32_t g_skippedPass = 0;
uint64_t g_passReason[kReasons];

inline size_t Mix(uint64_t h) { return static_cast<size_t>((h * 0x9E3779B97F4A7C15ull) >> 40); }

// Find or insert key in a table of `size` (power of two); returns nullptr when full.
PtrSlot* Slot(PtrSlot* t, size_t size, const void* key, bool insert, bool* fresh = nullptr) {
  size_t i = Mix(reinterpret_cast<uint64_t>(key)) & (size - 1);
  for (size_t k = 0; k < size; ++k, i = (i + 1) & (size - 1)) {
    PtrSlot& s = t[i];
    if (s.gen != g_gen) {
      if (!insert) return nullptr;
      s = {key, g_gen, -1, -1};
      if (fresh) *fresh = true;
      return &s;
    }
    if (s.key == key) {
      if (fresh) *fresh = false;
      return &s;
    }
  }
  return nullptr;
}

inline uint8_t* Globals() { return *reinterpret_cast<uint8_t**>(g_ng + kGlobals); }
// StructBufferManager page buffer object for page index (0xc310 + deref), or nullptr.
inline uint8_t* PageBuffer(uint32_t page) {
  uint8_t* pages = *reinterpret_cast<uint8_t**>(Globals() + 0x78 + 8);
  return *reinterpret_cast<uint8_t**>(pages + 0x20 + static_cast<uint64_t>(page) * 0x30);
}
// DX11Shader vt[27] writes the variable only when this flag is clear.
inline bool SbWritesEnabled() {
  const uint8_t* api = *reinterpret_cast<uint8_t**>(g_dx + kApiGlobal);
  return (api[0x2120] & 0x20) == 0;
}
inline uint8_t* VarOf(uint8_t* shader, int64_t h) {
  return *reinterpret_cast<uint8_t**>(*reinterpret_cast<uint8_t**>(shader + 0xc8) + h * 0x50 + 0x40);
}
inline uint8_t* Entries(uint8_t* mat, uint8_t* item) {
  const uint8_t* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  return **reinterpret_cast<uint8_t***>(item + 0x18) +
         static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(props + 0x26c)) * 0x18;
}

// Phase 1 for one item: class, barrier, prediction.
void Classify(uint32_t i, void* r0, bool sbWrites) {
  Rec& rc = g_rec[i];
  memset(&rc, 0, sizeof(rc));
  rc.r = static_cast<uint8_t*>(r0);
  rc.group = rc.next = -1;
  if (*reinterpret_cast<void**>(rc.r) != g_srVtbl) {
    rc.reason = kForeign;
    return;
  }
  rc.item = *reinterpret_cast<uint8_t**>(rc.r + 0x10);
  rc.mat = rc.item ? *reinterpret_cast<uint8_t**>(rc.item + 0x10) : nullptr;
  if (!rc.mat || *reinterpret_cast<void**>(rc.mat) != g_modelMatVtbl) {
    rc.reason = kOtherMat;
    return;
  }
  rc.tracked = 1;  // ModelMaterialMT::draw: writes mat+0x18c and the shader's variables
  rc.shader = *reinterpret_cast<uint8_t**>(rc.mat + 0x30);
  rc.obj = *reinterpret_cast<void**>(rc.item + 0x18);
  bool fresh = false;
  PtrSlot* ms = Slot(g_byMat, kTable, rc.mat, true, &fresh);
  if (ms) {
    if (fresh) g_matList[g_matCount++] = static_cast<uint32_t>(ms - g_byMat);
    ms->a = static_cast<int32_t>(i);
  }
  const bool dxShader = rc.shader && *reinterpret_cast<void**>(rc.shader) == g_shaderVtbl;
  const uint32_t ntex = *reinterpret_cast<uint32_t*>(rc.mat + 0x2d8);
  // Unset variables (any pass number: every slot 4 call binds every handle).
  if (dxShader) {
    bool unset = sbWrites && !PageBuffer(*reinterpret_cast<uint32_t*>(rc.item + 0xd0));
    if (!unset && ntex) {
      const uint8_t* e = Entries(rc.mat, rc.item);
      for (uint32_t k = 0; k < ntex && k < 256; ++k, e += 0x18)
        if (*reinterpret_cast<int64_t*>(rc.mat + 0x240 + 8 * k) != -1 && !*reinterpret_cast<void* const*>(e + 8)) unset = true;
    }
    if (unset) Slot(g_nullBind, 4096, rc.shader, true);
  }
  if (*reinterpret_cast<uint32_t*>(rc.r + 0x60) != 1) {
    rc.reason = kPassNum;
    return;
  }
  const uint8_t* props = *reinterpret_cast<uint8_t**>(rc.mat + 0x28);
  if (props[0x33]) {
    rc.reason = kTransparent;
    return;
  }
  if (rc.r[0x64]) {
    rc.reason = kCockpit;
    return;
  }
  if (!dxShader) {
    rc.reason = kNotMapped;
    return;
  }
  const uint64_t tech = *reinterpret_cast<uint64_t*>(rc.mat + 0x1d8);
  const uint8_t flags = shadowinst::EntryFlags(rc.shader, tech, 0);
  if (!(flags & shadowinst::kFlagGate)) {
    rc.reason = kNotMapped;
    return;
  }
  if (!(flags & shadowinst::kFlagBlendNone)) {
    rc.reason = kBlend;
    return;
  }
  if (!(flags & shadowinst::kFlagDeferredP0)) {
    rc.reason = kNotDeferred;
    return;
  }
  if (!(flags & shadowinst::kFlagPrevUnused) || !(flags & shadowinst::kFlagPsoVsOnly) ||
      !shadowinst::FindVs(rc.shader, tech, 0)) {
    rc.reason = kNoVariant;
    return;
  }
  if (ntex > kMaxTex) {
    rc.reason = kTooManyTex;
    return;
  }
  if (g_poolUsed + kMaxTex + kMaxAnim > kPool) {
    rc.reason = kPoolFull;
    return;
  }
  rc.viewOff = g_poolUsed;
  const uint64_t size = *reinterpret_cast<uint64_t*>(rc.r + 0x6c);
  const uint8_t* recs = *reinterpret_cast<uint8_t**>(rc.shader + 0xc8);
  const uint8_t* e = ntex ? Entries(rc.mat, rc.item) : nullptr;
  uint64_t vh = 0xcbf29ce484222325ull;
  for (uint32_t k = 0; k < ntex; ++k, e += 0x18) {
    const int64_t h = *reinterpret_cast<int64_t*>(rc.mat + 0x240 + 8 * k);
    if (h == -1) continue;
    const int32_t type = *reinterpret_cast<const int32_t*>(recs + h * 0x50 + 0xc);
    void* view = nullptr;
    const uint8_t why = PredictView(g_texEnv, *reinterpret_cast<uint8_t* const*>(e + 8),
                                    *reinterpret_cast<const int64_t*>(e), type, size, &view);
    if (why) {
      rc.reason = why;
      return;
    }
    g_pool[g_poolUsed++] = reinterpret_cast<uint64_t>(view);
    vh = (vh ^ reinterpret_cast<uint64_t>(view)) * 0x100000001b3ull;
    ++rc.nView;
  }
  rc.animOff = g_poolUsed;
  uint32_t anim[kMaxAnim], na = 0;
  if (const uint8_t why = AnimInputs(g_animEnv, rc.mat, rc.item, anim, kMaxAnim, &na)) {
    g_poolUsed = rc.viewOff;
    rc.reason = why;
    return;
  }
  uint64_t ah = 0x84222325cbf29ce4ull;
  for (uint32_t k = 0; k < na; ++k) {
    g_pool[g_poolUsed++] = anim[k];
    ah = (ah ^ anim[k]) * 0x100000001b3ull;
  }
  rc.nAnim = static_cast<uint8_t>(na);
  rc.viewHash = vh;
  rc.animHash = ah;
}

inline bool SameKey(const Rec& a, const Rec& b) {
  if (a.mat != b.mat || a.nView != b.nView || a.nAnim != b.nAnim || a.viewHash != b.viewHash ||
      a.animHash != b.animHash)
    return false;
  if (*reinterpret_cast<void**>(a.item + 0xc0) != *reinterpret_cast<void**>(b.item + 0xc0) ||
      *reinterpret_cast<void**>(a.item + 0xc8) != *reinterpret_cast<void**>(b.item + 0xc8) ||
      *reinterpret_cast<uint32_t*>(a.item + 0xd0) != *reinterpret_cast<uint32_t*>(b.item + 0xd0))
    return false;
  return memcmp(g_pool + a.viewOff, g_pool + b.viewOff, a.nView * sizeof(uint64_t)) == 0 &&
         memcmp(g_pool + a.animOff, g_pool + b.animOff, a.nAnim * sizeof(uint64_t)) == 0;
}

inline uint64_t KeyHash(const Rec& a) {
  uint64_t h = reinterpret_cast<uint64_t>(a.mat) * 0x9E3779B97F4A7C15ull;
  h ^= reinterpret_cast<uint64_t>(*reinterpret_cast<void**>(a.item + 0xc0)) * 0xC2B2AE3D27D4EB4Full;
  h ^= reinterpret_cast<uint64_t>(*reinterpret_cast<void**>(a.item + 0xc8)) * 0x27D4EB2F165667C5ull;
  h ^= (*reinterpret_cast<uint32_t*>(a.item + 0xd0) + a.viewHash) * 0x165667B19E3779F9ull;
  return h ^ (a.animHash * 0x9E3779B97F4A7C15ull);
}

// Builds the plan (no D3D calls). Returns the number of offsets written, or
// 0 when nothing is batched / on anything unexpected (plan inactive).
uint32_t PlanCore(void* const* begin, size_t n) {
  if (n == 0) return 0;
  if (n > kMaxItems) {
    g_tooLarge++;
    return 0;
  }
  if (++g_gen == 0) {
    for (PtrSlot* t : {g_byR, g_byKey, g_byObj, g_byMat, g_byVar}) memset(t, 0, sizeof(PtrSlot) * kTable);
    memset(g_nullBind, 0, sizeof(g_nullBind));
    memset(g_involved, 0, sizeof(g_involved));
    g_gen = 1;
  }
  g_n = static_cast<uint32_t>(n);
  g_matCount = 0;
  g_poolUsed = 0;
  g_grpCount = 0;
  memset(g_passReason, 0, sizeof(g_passReason));
  const bool sbWrites = SbWritesEnabled();
  for (uint32_t i = 0; i < n; ++i) {
    bool fresh = false;
    PtrSlot* s = Slot(g_byR, kTable, begin[i], true, &fresh);
    if (!s || !fresh) return 0;  // full, or the same renderable twice
    s->a = static_cast<int32_t>(i);
    Classify(i, begin[i], sbWrites);
  }
  // Phase 2: grouping in original order.
  uint32_t epoch = 0;
  for (uint32_t i = 0; i < n; ++i) {
    Rec& rc = g_rec[i];
    if (IsBarrier(rc.reason)) ++epoch;
    if (!rc.tracked) continue;
    if (rc.reason == kOk && Slot(g_nullBind, 4096, rc.shader, false)) rc.reason = kNullBind;
    PtrSlot* os = Slot(g_byObj, kTable, rc.obj, true);
    if (!os) return 0;
    // os->a = the latest position where a draw of this object executes so far
    // (its own index, or its group leader's index when it is a member);
    // os->b = that draw's group. Keeping every new draw of the object at or
    // after it keeps each object's own draws in their original order.
    if (rc.reason != kOk) {
      os->a = static_cast<int32_t>(i);
      os->b = -1;
      continue;
    }
    const uint64_t kh = KeyHash(rc);
    size_t ki = Mix(kh) & (kTable - 1);
    PtrSlot* ks = nullptr;
    for (size_t k = 0; k < kTable; ++k, ki = (ki + 1) & (kTable - 1)) {
      PtrSlot& s = g_byKey[ki];
      if (s.gen != g_gen) {
        s = {nullptr, g_gen, -1, -1};
        g_keyHash[ki] = kh;
        ks = &s;
        break;
      }
      if (g_keyHash[ki] == kh && SameKey(g_rec[g_grp[s.a].first], rc)) {
        ks = &s;
        break;
      }
    }
    if (!ks) return 0;
    int32_t g = ks->a;
    if (g >= 0) {
      const Group& gr = g_grp[g];
      if (gr.epoch != epoch) {
        g_splitBarrier++;
        g = -1;
      } else if (os->a > gr.first || (os->a == gr.first && os->b != g)) {
        g_splitObject++;  // an earlier draw of this object executes after (or apart from) this group's draw
        g = -1;
      }
    }
    if (g >= 0) {
      Group& gr = g_grp[g];
      g_rec[gr.last].next = static_cast<int32_t>(i);
      gr.last = static_cast<int32_t>(i);
      ++gr.count;
    } else {
      g = static_cast<int32_t>(g_grpCount++);
      g_grp[g] = {static_cast<int32_t>(i), static_cast<int32_t>(i), 1, epoch, 0, 0};
      ks->a = g;
    }
    rc.group = g;
    const int32_t exec = g_grp[g].first;  // where this draw executes (itself when it leads)
    if (exec >= os->a) {
      os->a = exec;
      os->b = g;
    }
  }
  // Roles and offsets.
  uint32_t cursor = 0, leaders = 0, skipped = 0, groups = 0;
  for (uint32_t g = 0; g < g_grpCount; ++g) {
    Group& gr = g_grp[g];
    if (gr.count < 2 || cursor + gr.count > kMaxOffsets) {
      for (int32_t i = gr.first; i >= 0; i = g_rec[i].next) g_rec[i].reason = gr.count < 2 ? kAlone : kPoolFull;
      continue;
    }
    gr.base = cursor;
    for (int32_t i = gr.first; i >= 0; i = g_rec[i].next) {
      g_offsets[cursor++] = *reinterpret_cast<uint32_t*>(g_rec[i].item + 0xd4);
      g_rec[i].role = i == gr.first ? kLeader : kMember;
    }
    ++groups;
    ++leaders;
    skipped += gr.count - 1;
  }
  for (uint32_t i = 0; i < n; ++i) ++g_passReason[g_rec[i].reason];
  g_items += n;
  g_groups += groups;
  g_leaders += leaders;
  g_skipped += skipped;
  for (int k = 0; k < kReasons; ++k) g_reason[k] += g_passReason[k];
  return cursor;
}

bool PlanGuarded(void** vec) {
  __try {
    auto* const* begin = static_cast<void* const*>(vec[0]);
    auto* const* end = static_cast<void* const*>(vec[1]);
    if (!begin || end < begin) return false;
    const uint32_t offsets = PlanCore(begin, static_cast<size_t>(end - begin));
    if (offsets == 0) return false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(g_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, g_offsets, offsets * sizeof(uint32_t));
    g_ctx->Unmap(g_buf, 0);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ---- Restores after the pass ----
inline bool Skipped(uint32_t i) {
  const Rec& rc = g_rec[i];
  return rc.role == kMember && !g_grp[rc.group].failed;
}

// The value member i's draw would have written to entry k's variable.
void* MemberView(uint32_t i, uint32_t k) {
  const Rec& rc = g_rec[i];
  uint32_t v = 0;
  for (uint32_t j = 0; j < k; ++j) v += *reinterpret_cast<int64_t*>(rc.mat + 0x240 + 8 * j) != -1;
  return reinterpret_cast<void*>(g_pool[rc.viewOff + v]);
}

void RestoreRaw() {
  for (uint32_t m = 0; m < g_matCount; ++m) {
    const PtrSlot& s = g_byMat[g_matList[m]];
    const uint32_t i = static_cast<uint32_t>(s.a);
    const Rec& rc = g_rec[i];
    *reinterpret_cast<uint32_t*>(rc.mat + 0x18c) = *reinterpret_cast<uint32_t*>(rc.item + 0xd4);
    if (!Skipped(i)) continue;
    memcpy(rc.mat + 0xb0, rc.item + 0x60, 0x40);
    alignas(16) uint8_t span[16];
    memcpy(span, rc.item, 16);
    g_evalAnim(rc.mat, span);
    g_matRestores++;
  }
  if (!g_skippedPass) return;
  // Effect variables: last writer in original order.
  bool any = false;
  for (uint32_t i = 0; i < g_n; ++i)
    if (Skipped(i)) any |= Slot(g_involved, 4096, g_rec[i].shader, true) != nullptr;
  if (!any) return;
  const bool sbWrites = SbWritesEnabled();
  for (uint32_t i = 0; i < g_n; ++i) {
    const Rec& rc = g_rec[i];
    if (!rc.tracked || !Slot(g_involved, 4096, rc.shader, false)) continue;
    if (sbWrites && PageBuffer(*reinterpret_cast<uint32_t*>(rc.item + 0xd0))) {
      if (PtrSlot* v = Slot(g_byVar, kTable, VarOf(rc.shader, *reinterpret_cast<int64_t*>(rc.mat + 0x68)), true)) {
        v->a = static_cast<int32_t>(i);
        v->b = -1;
      }
    }
    const uint32_t ntex = *reinterpret_cast<uint32_t*>(rc.mat + 0x2d8);
    const uint8_t* e = ntex ? Entries(rc.mat, rc.item) : nullptr;
    for (uint32_t k = 0; k < ntex && k < 256; ++k, e += 0x18) {
      const int64_t h = *reinterpret_cast<int64_t*>(rc.mat + 0x240 + 8 * k);
      if (h == -1 || !*reinterpret_cast<void* const*>(e + 8)) continue;
      if (PtrSlot* v = Slot(g_byVar, kTable, VarOf(rc.shader, h), true)) {
        v->a = static_cast<int32_t>(i);
        v->b = static_cast<int32_t>(k);
      }
    }
  }
  for (size_t s = 0; s < kTable; ++s) {
    const PtrSlot& v = g_byVar[s];
    if (v.gen != g_gen || v.a < 0 || !Skipped(static_cast<uint32_t>(v.a))) continue;
    const Rec& rc = g_rec[v.a];
    void* value = v.b < 0
                      ? *reinterpret_cast<void**>(PageBuffer(*reinterpret_cast<uint32_t*>(rc.item + 0xd0)) + 0x30)
                      : MemberView(static_cast<uint32_t>(v.a), static_cast<uint32_t>(v.b));
    void* var = const_cast<void*>(v.key);
    (*reinterpret_cast<SetResFn**>(var))[31](var, value);
    g_varRestores++;
  }
}

void RestoreGuarded() {
  __try {
    RestoreRaw();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Disable("access violation while restoring material state");
  }
}

// ---- Per item ----
void* Renderer() { return *shadowbatch::g_rendererApi; }
void* FbNow() { return reinterpret_cast<FbFn>(shadowbatch::g_rendererVtbl[54])(Renderer()); }
void VpNow(int32_t out[2]) { reinterpret_cast<VpFn>(shadowbatch::g_rendererVtbl[40])(Renderer(), out); }

// 0x15e20 writes mat+0x114/0x118 from the framebuffer and viewport at each
// draw: a skipped member gets the leader's. They must not change in the pass.
bool RendererStateSame() {
  int32_t vp[2];
  VpNow(vp);
  return FbNow() == g_fb0 && vp[0] == g_vp0[0] && vp[1] == g_vp0[1];
}

void ReplayMember(const Rec& rc) {
  // 0x44350: lock add [globals+0x1e8], [r+0x78] (pass 1).
  _InterlockedExchangeAdd64(reinterpret_cast<volatile long long*>(Globals() + 0x1e8),
                            *reinterpret_cast<long long*>(rc.r + 0x78));
  // Slot 26's streaming request per bound texture, with this member's size.
  const uint64_t size = *reinterpret_cast<uint64_t*>(rc.r + 0x6c);
  const uint32_t ntex = *reinterpret_cast<uint32_t*>(rc.mat + 0x2d8);
  const uint8_t* e = ntex ? Entries(rc.mat, rc.item) : nullptr;
  for (uint32_t k = 0; k < ntex; ++k, e += 0x18) {
    if (*reinterpret_cast<int64_t*>(rc.mat + 0x240 + 8 * k) == -1) continue;
    void* tex = *reinterpret_cast<void* const*>(e + 8);
    if (tex) (*reinterpret_cast<Tex23Fn**>(tex))[23](tex, size);
  }
}

// DrawN check: the views bound by the leader's own slot-26 calls equal the
// group's prediction (and its sbPositions buffer is the page's).
bool g_checkCalled = false, g_checkFailed = false;
bool LeaderCheck(void* arg) {
  g_checkCalled = true;
  const Rec& rc = *static_cast<const Rec*>(arg);
  bool ok = true;
  __try {
    const uint32_t ntex = *reinterpret_cast<uint32_t*>(rc.mat + 0x2d8);
    uint32_t v = 0;
    for (uint32_t k = 0; k < ntex && ok; ++k) {
      const int64_t h = *reinterpret_cast<int64_t*>(rc.mat + 0x240 + 8 * k);
      if (h == -1) continue;
      uint8_t* var = VarOf(rc.shader, h);
      ok = var && (*reinterpret_cast<void***>(var))[31] == g_setResource &&
           **reinterpret_cast<void***>(var + 8) == reinterpret_cast<void*>(g_pool[rc.viewOff + v]);
      ++v;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  g_checkFailed = !ok;
  return ok;
}

bool LeaderRaw(void* self, void* ctx, Rec& rc, uint64_t* ret) {
  Group& gr = g_grp[rc.group];
  uint8_t* shader = rc.shader;
  const uint64_t tech = *reinterpret_cast<uint64_t*>(rc.mat + 0x1d8);
  void* expectVs = nullptr;
  ID3D11VertexShader* ours = shadowinst::FindVsEx(shader, tech, 0, &expectVs);
  if (!ours || !expectVs || !g_ctx || !g_srv) return false;
  if (*shadowbatch::g_rendererApi != static_cast<void*>(shadowbatch::g_rendererObj) ||
      *shadowbatch::g_rendererObj != static_cast<void*>(shadowbatch::g_rendererVtbl))
    return false;
  auto* techBegin = *reinterpret_cast<uint8_t**>(shader + 0xb0);
  auto* techEnd = *reinterpret_cast<uint8_t**>(shader + 0xb8);
  if (tech < 1 || tech > static_cast<uint64_t>((techEnd - techBegin) / 0x50)) return false;
  auto* techObj = *reinterpret_cast<uint8_t**>(techBegin + (tech - 1) * 0x50 + 0x20);
  using PassFn = uint8_t*(__fastcall*)(void*, uint32_t);
  auto* fxpass = (*reinterpret_cast<PassFn**>(techObj))[7](techObj, 0);
  if (!fxpass) return false;
  auto* vsBlock = *reinterpret_cast<uint8_t**>(fxpass + 0xc0);
  auto* dcsVs = *reinterpret_cast<ID3D11VertexShader**>(vsBlock + 0x18);
  if (static_cast<void*>(dcsVs) != expectVs) return false;
  uint8_t* item = rc.item;
  const uint32_t pso = *reinterpret_cast<uint32_t*>(item + 0xd4);
  bool psoSet = false, vsSet = false, srvSet = false, vtblSet = false;
  g_checkCalled = g_checkFailed = false;
  __try {
    *reinterpret_cast<uint32_t*>(item + 0xd4) = gr.base;
    psoSet = true;
    *reinterpret_cast<ID3D11VertexShader**>(vsBlock + 0x18) = ours;
    vsSet = true;
    g_ctx->VSSetShaderResources(127, 1, &g_srv);
    srvSet = true;
    shadowbatch::t_check = &LeaderCheck;
    shadowbatch::t_checkArg = &rc;
    shadowbatch::t_instances = gr.count;
    *shadowbatch::g_rendererObj = shadowbatch::g_myVtbl;
    vtblSet = true;
    *ret = gbcount::g_orig(self, ctx);
  } __finally {
    if (vtblSet) *shadowbatch::g_rendererObj = shadowbatch::g_rendererVtbl;
    shadowbatch::t_instances = 0;
    shadowbatch::t_check = nullptr;
    shadowbatch::t_checkArg = nullptr;
    if (vsSet) {
      *reinterpret_cast<ID3D11VertexShader**>(vsBlock + 0x18) = dcsVs;
      g_ctx->VSSetShader(dcsVs, nullptr, 0);
    }
    if (srvSet) {
      ID3D11ShaderResourceView* none = nullptr;
      g_ctx->VSSetShaderResources(127, 1, &none);
    }
    if (psoSet) *reinterpret_cast<uint32_t*>(item + 0xd4) = pso;
  }
  if (!g_checkCalled || g_checkFailed) {
    gr.failed = 1;  // drawn alone (instances 0); the members draw themselves
    g_leaderMismatch++;
  }
  return true;
}

int AvFilter(unsigned long code) {
  return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

void (*g_onFirstItem)() = nullptr;  // verification capture (set by the verifier)

bool Override(void* self, void* ctx, uint64_t* ret) {
  if (!g_planActive || GetCurrentThreadId() != g_renderThread) return false;
  if (g_firstItem) {
    g_firstItem = false;
    __try {
      g_fb0 = FbNow();
      VpNow(g_vp0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_fb0 = nullptr;
    }
    if (g_onFirstItem) g_onFirstItem();
  }
  if (!g_batching) return false;
  PtrSlot* s = Slot(g_byR, kTable, self, false);
  if (!s) return false;
  Rec& rc = g_rec[s->a];
  if (rc.role == kSolo) return false;
  Group& gr = g_grp[rc.group];
  if (rc.role == kMember) {
    if (gr.failed) return false;
    __try {
      if (!RendererStateSame()) {
        g_rendererChanged++;
        Disable("framebuffer or viewport changed inside the G-buffer pass");
      }
      ReplayMember(rc);
    } __except (AvFilter(GetExceptionCode())) {
      Disable("access violation in a member's replay");
    }
    ++g_skippedPass;
    *ret = 0;  // the item loop ignores the result [V GC 0x89709]
    return true;
  }
  bool done = false;
  __try {
    done = LeaderRaw(self, ctx, rc, ret);
  } __except (AvFilter(GetExceptionCode())) {
    done = false;
    Disable("access violation in the instanced leader draw");
  }
  if (done) return true;
  g_fallbacks++;
  gr.failed = 1;
  return false;  // the original draws the leader alone; the members follow
}

// ---- Pass wrapper ----
// Runs one pass execution with the plan: batched or stock (capture only).
void RunPlanned(void* pass, void* ctx, gbpass::ExecFn orig, bool batched) {
  g_planActive = true;
  g_batching = batched;
  g_firstItem = true;
  g_skippedPass = 0;
  orig(pass, ctx);
  g_batching = false;
  g_planActive = false;
  if (batched) RestoreGuarded();  // a stock run leaves stock state itself
}

// No plan (nothing batched in this pass): RunPlanned(..., false) still
// reaches the first-item callback.
void ClearPlan() {
  g_n = 0;
  g_matCount = 0;
  g_grpCount = 0;
}

// Verification (stage-2 step 2) plugs in here; nullptr = plain batched run.
using VerifyFn = void (*)(void* pass, void* ctx, gbpass::ExecFn orig, void** vec);
VerifyFn g_verifier = nullptr;

void WrapInner(void* pass, void* ctx, gbpass::ExecFn orig) {
  if (!g_renderThread) g_renderThread = GetCurrentThreadId();
  if (GetCurrentThreadId() != g_renderThread) return orig(pass, ctx);
  void** vec = gbpass::ItemVector(pass, ctx);
  if (g_verify.load(std::memory_order_relaxed) && g_verifier && vec) return g_verifier(pass, ctx, orig, vec);
  if (!vec || !PlanGuarded(vec)) return orig(pass, ctx);
  g_passes++;
  RunPlanned(pass, ctx, orig, true);
}

void Wrap(void* pass, void* ctx, gbpass::ExecFn orig) {
  g_inWrap.fetch_add(1);
  if (g_shutdown.load() || g_disabled.load(std::memory_order_relaxed) || !g_on.load(std::memory_order_relaxed) ||
      g_state.load() != 1)
    orig(pass, ctx);
  else
    WrapInner(pass, ctx, orig);
  g_inWrap.fetch_sub(1);
}

// ---- Install ----
const char* VerifyBuild(uint8_t* ng, uint8_t* dx, uint8_t* md) {
  if (const char* why = shadowtex::VerifyBuild(ng, dx)) return why;  // slot 26, getDesc, compat, getSRV, texture classes
  auto** matVt = reinterpret_cast<void**>(ng + kModelMatVtbl);
  if (reinterpret_cast<uint8_t*>(SlotOriginal(&matVt[4])) != ng + kModelDraw)
    return "g-buffer batching: ModelMaterialMT vt[4] does not match this build";
  for (const gbpass::Code& c : kNgCode)
    if (!shadowtex::CodeIs(ng, c.begin, c.end, c.hash)) return "g-buffer batching: NGModel draw code does not match this build";
  uint8_t tri[8];
  if (!allocslab::ReadBytes(ng + kTriSite, tri, 8) || (tri[0] != 0xF0 && tri[0] != 0x3E) ||
      memcmp(tri + 1, tricount::kLocked + 1, 7) != 0)
    return "g-buffer batching: NGModel triangle counter does not match this build";
  auto** shVt = reinterpret_cast<void**>(dx + shadowtex::kShaderVtbl);
  if (reinterpret_cast<uint8_t*>(SlotOriginal(&shVt[27])) != dx + 0x1fe70)
    return "g-buffer batching: DX11Shader vt[27] does not match this build";
  for (const gbpass::Code& c : kDxCode)
    if (!shadowtex::CodeIs(dx, c.begin, c.end, c.hash)) return "g-buffer batching: dx11backend code does not match this build";
  for (const PropClass& p : kProps) {
    auto** vt = reinterpret_cast<void**>(md + p.vtbl);
    if (!allocslab::RttiIs(md, vt, p.rtti) || reinterpret_cast<uint8_t*>(vt[9]) != md + p.slot9)
      return "g-buffer batching: ModelDesc animated property classes do not match this build";
  }
  for (const gbpass::Code& c : kMdCode)
    if (!shadowtex::CodeIs(md, c.begin, c.end, c.hash)) return "g-buffer batching: ModelDesc property code does not match this build";
  return nullptr;
}

void SetupEnv(uint8_t* ng, uint8_t* dx, uint8_t* md) {
  g_ng = ng;
  g_dx = dx;
  g_srVtbl = ng + kSrVtbl;
  g_modelMatVtbl = ng + kModelMatVtbl;
  g_shaderVtbl = dx + shadowtex::kShaderVtbl;
  g_setResource = dx + kSetResource;
  g_evalAnim = reinterpret_cast<EvalAnimFn>(ng + kEvalAnim);
  g_texEnv.texVtbl = dx + shadowtex::kTexVtbl;
  g_texEnv.inner[0] = dx + shadowtex::kInnerFile;
  g_texEnv.inner[1] = dx + shadowtex::kInnerArray;
  g_texEnv.inner[2] = dx + shadowtex::kInnerDummy;
  g_texEnv.getDesc = reinterpret_cast<GetDescFn>(dx + shadowtex::kGetDesc);
  g_texEnv.compat = reinterpret_cast<CompatFn>(dx + shadowtex::kCompat);
  for (int c = 0; c < kPropClasses; ++c) {
    g_animEnv.vtbl[c] = md + kProps[c].vtbl;
    g_animEnv.bytes[c] = kProps[c].bytes;
  }
}

bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  if (shadowinst::g_state.load() != 1 || !shadowinst::g_gbInstalled.load() || !shadowinst::g_device) return false;
  if (!gbcount::Install()) return false;
  if (!shadowbatch::InstallRenderer()) {
    if (shadowbatch::g_rendererState < 0) g_state = -1;
    return false;
  }
  auto* ng = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  auto* md = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"ModelDesc.dll"));
  if (!ng || !dx || !md) return false;
  g_state = -1;
  if (const char* why = VerifyBuild(ng, dx, md)) {
    Log("%s; skipped", why);
    return false;
  }
  auto** rv = shadowbatch::g_rendererVtbl;
  if (reinterpret_cast<uint8_t*>(SlotOriginal(&rv[54])) != dx + 0x15d60 ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&rv[40])) != dx + 0x17010) {
    Log("g-buffer batching: DX11Renderer getters do not match this build; skipped");
    return false;
  }
  if (!gbpass::Install()) return false;
  SetupEnv(ng, dx, md);
  shadowinst::g_device->GetImmediateContext(&g_ctx);
  if (!shadowbatch::CreateOffsetBuffer(shadowinst::g_device, kMaxOffsets, &g_buf, &g_srv)) return false;
  gbcount::g_override = &Override;
  gbpass::g_wrap = &Wrap;
  g_state = 1;
  Log("g-buffer batching: ready (G-buffer pass and SceneRenderable hooks, view prediction, DX11Renderer instanced draw)");
  return true;
}

// True when its objects were released (false: a pass still runs; the
// caller then keeps gbverify's and shadow_inst's objects alive as well).
bool Shutdown() {
  g_shutdown = true;
  g_on = false;
  if (gbpass::g_wrap.load() == &Wrap) gbpass::g_wrap = nullptr;
  if (gbcount::g_override.load() == &Override) gbcount::g_override = nullptr;
  for (int i = 0; i < 400 && g_inWrap.load() != 0; ++i) Sleep(5);
  if (g_inWrap.load() != 0) {
    Log("g-buffer batching: a pass was still running at unload; its objects are left alive");
    return false;
  }
  if (g_srv) g_srv->Release();
  if (g_buf) g_buf->Release();
  if (g_ctx) g_ctx->Release();
  g_srv = nullptr;
  g_buf = nullptr;
  g_ctx = nullptr;
  return true;
}

void ResetCounters() {
  for (auto* c : {&g_passes, &g_items, &g_groups, &g_leaders, &g_skipped, &g_fallbacks, &g_tooLarge, &g_splitBarrier,
                  &g_splitObject, &g_leaderMismatch, &g_rendererChanged, &g_matRestores, &g_varRestores})
    c->store(0);
  for (auto& r : g_reason) r.store(0);
}

void LogCounters(const char* label) {
  Log("  g-buffer batching %s: %llu passes, items %llu, groups %llu, draws skipped %llu, leader fallbacks %llu, "
      "leader view mismatches %llu, oversized passes %llu, group splits (barrier %llu, same object %llu), "
      "framebuffer/viewport changes %llu, restores (materials %llu, variables %llu)%s",
      label, static_cast<unsigned long long>(g_passes.load()), static_cast<unsigned long long>(g_items.load()),
      static_cast<unsigned long long>(g_leaders.load()), static_cast<unsigned long long>(g_skipped.load()),
      static_cast<unsigned long long>(g_fallbacks.load()), static_cast<unsigned long long>(g_leaderMismatch.load()),
      static_cast<unsigned long long>(g_tooLarge.load()), static_cast<unsigned long long>(g_splitBarrier.load()),
      static_cast<unsigned long long>(g_splitObject.load()), static_cast<unsigned long long>(g_rendererChanged.load()),
      static_cast<unsigned long long>(g_matRestores.load()), static_cast<unsigned long long>(g_varRestores.load()),
      g_disabled.load() ? ", DISABLED" : "");
  std::string line;
  char b[64];
  for (int k = 0; k < kReasons; ++k) {
    const uint64_t v = g_reason[k].load();
    if (!v) continue;
    snprintf(b, sizeof(b), "%s%s %llu", line.empty() ? "" : ", ", kReasonName[k], static_cast<unsigned long long>(v));
    line += b;
  }
  Log("  g-buffer batching %s: items by outcome: %s", label, line.empty() ? "none" : line.c_str());
}

}  // namespace gbbatch

// ---------------------------------------------------------------------------
// [Suite] GBufferInstVerify: same-frame compare of every bound render target
// and the depth-stencil target.
//
// Every g_every-th pass execution while verifying runs twice:
//   1. run 1 stock; at its first SceneRenderable item the bound views are
//      read (OMGetRenderTargets, 8 RTVs + DSV) and each target subresource is
//      copied ("init");
//   2. after run 1: copy each target ("A"), then copy init back into it;
//   3. run 2: stock again for the first g_aaPasses compared passes (A/A: the
//      stock pass is reproducible from the same start), batched after that
//      (A/B). Its first item must see the same views;
//   4. after run 2: copy each target ("B"), compare A and B bit for bit.
// The frame keeps run 2's result. Any difference latches batching off (A/A:
// the pass is not reproducible; A/B: batching changed it) and logs the count
// per target.
// Single-sample targets: staging copies, raw bytes compared on the CPU (any
// format with a known texel size). Multisampled targets: DEFAULT copies read
// per sample by a compute shader through a UINT view of the same typeless
// family (exact bits). Formats without a UINT view (B8G8R8A8, R11G11B10,
// D24S8, D32S8X24) have no exact per-sample compare: batching is refused.
// The CS state is saved and restored around each dispatch.
// ---------------------------------------------------------------------------
namespace gbverify {

using namespace gbbatch;

constexpr int kMaxTargets = 9;  // 8 RTVs + DSV (index 8)
std::atomic<int> g_every{8};
std::atomic<int> g_aaPasses{6};

// Bytes per texel of uncompressed formats (0 = not handled).
inline UINT TexelBytes(DXGI_FORMAT f) {
  const int v = static_cast<int>(f);
  if (v >= 1 && v <= 4) return 16;
  if (v >= 5 && v <= 8) return 12;
  if (v >= 9 && v <= 22) return 8;
  if (v >= 23 && v <= 47) return 4;
  if (v >= 48 && v <= 59) return 2;
  if (v >= 60 && v <= 64) return 1;
  if (v == 67 || (v >= 87 && v <= 93)) return 4;  // R9G9B9E5, B8G8R8A8/X8 family, R10G10B10_XR
  if (v == 85 || v == 86) return 2;                // B5G6R5, B5G5R5A1
  return 0;
}

// Typeless family and UINT view for the per-sample compare; false = none.
inline bool UintFamily(DXGI_FORMAT f, DXGI_FORMAT* typeless, DXGI_FORMAT* view) {
  struct F {
    int lo, hi;
    DXGI_FORMAT t, u;
  };
  static const F kF[] = {
      {1, 4, DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_UINT},
      {9, 14, DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UINT},
      {15, 18, DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_UINT},
      {23, 25, DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UINT},
      {27, 32, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UINT},
      {33, 38, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UINT},
      {39, 43, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT},  // includes D32_FLOAT
      {48, 52, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UINT},
      {53, 59, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UINT},  // includes D16_UNORM
      {60, 64, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UINT},
  };
  const int v = static_cast<int>(f);
  for (const F& x : kF)
    if (v >= x.lo && v <= x.hi) {
      *typeless = x.t;
      *view = x.u;
      return true;
    }
  return false;
}

struct Target {
  ID3D11View* view = nullptr;          // the bound RTV / DSV (referenced)
  ID3D11Texture2D* res = nullptr;      // its texture (referenced)
  UINT mip = 0, first = 0, slices = 0, resMips = 1;
  D3D11_TEXTURE2D_DESC copyDesc = {};  // of init / a
  bool ms = false;
  ID3D11Texture2D* init = nullptr;     // init, then B
  ID3D11Texture2D* a = nullptr;
  ID3D11Texture2D* m = nullptr;        // the ownship mask's run (gb_rec.h stage 2; staging targets only, on demand)
  // R133: the bracketed control (gb_rec.h VerifyPass): the recorded run B kept in .b so that init can be restored
  // once more for a second stock control C2 after it (.c2). Staging targets only, created on demand.
  ID3D11Texture2D* b = nullptr;
  ID3D11Texture2D* c2 = nullptr;
  ID3D11ShaderResourceView* srvInit = nullptr;  // MS only
  ID3D11ShaderResourceView* srvA = nullptr;
};
Target g_t[kMaxTargets];
int g_count = 0;
bool g_captureWanted = false;  // run 1: capture at the first item
bool g_checkWanted = false;    // run 2: same views at the first item
bool g_captured = false, g_sameViews = false;
const char* g_why = nullptr;   // why this pass cannot be compared
ID3D11Device* g_dev = nullptr;
ID3D11ComputeShader* g_cs = nullptr;
ID3D11Buffer* g_counter = nullptr;
ID3D11Buffer* g_counterRead = nullptr;
ID3D11UnorderedAccessView* g_counterUav = nullptr;
uint32_t g_tick = 0;

std::atomic<uint64_t> g_aa{0}, g_ab{0}, g_aaBad{0}, g_abBad{0}, g_texels{0}, g_badTexels{0}, g_skippedCmp{0},
    g_errors{0}, g_targetsSeen{0}, g_msTargets{0};
std::atomic<uint64_t> g_badPerTarget[kMaxTargets];
std::string g_lastFormats;  // render thread; read by the suite after verifying
const char* g_lastWhy = nullptr;

template <typename T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

void ReleaseCopies(Target& t) {
  SafeRelease(t.init);
  SafeRelease(t.a);
  SafeRelease(t.m);
  SafeRelease(t.b);
  SafeRelease(t.c2);
  SafeRelease(t.srvInit);
  SafeRelease(t.srvA);
}

void ReleaseViews() {
  for (Target& t : g_t) {
    SafeRelease(t.view);
    SafeRelease(t.res);
  }
  g_count = 0;
}

// Fills the view range of target t from its view; false = unsupported.
bool ViewRange(Target& t, bool depth) {
  if (depth) {
    D3D11_DEPTH_STENCIL_VIEW_DESC d;
    static_cast<ID3D11DepthStencilView*>(t.view)->GetDesc(&d);
    switch (d.ViewDimension) {
      case D3D11_DSV_DIMENSION_TEXTURE2D:
        t.mip = d.Texture2D.MipSlice, t.first = 0, t.slices = 1;
        return true;
      case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
        t.mip = d.Texture2DArray.MipSlice, t.first = d.Texture2DArray.FirstArraySlice,
        t.slices = d.Texture2DArray.ArraySize;
        return true;
      case D3D11_DSV_DIMENSION_TEXTURE2DMS:
        t.mip = 0, t.first = 0, t.slices = 1;
        return true;
      case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
        t.mip = 0, t.first = d.Texture2DMSArray.FirstArraySlice, t.slices = d.Texture2DMSArray.ArraySize;
        return true;
      default:
        return false;
    }
  }
  D3D11_RENDER_TARGET_VIEW_DESC d;
  static_cast<ID3D11RenderTargetView*>(t.view)->GetDesc(&d);
  switch (d.ViewDimension) {
    case D3D11_RTV_DIMENSION_TEXTURE2D:
      t.mip = d.Texture2D.MipSlice, t.first = 0, t.slices = 1;
      return true;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
      t.mip = d.Texture2DArray.MipSlice, t.first = d.Texture2DArray.FirstArraySlice,
      t.slices = d.Texture2DArray.ArraySize;
      return true;
    case D3D11_RTV_DIMENSION_TEXTURE2DMS:
      t.mip = 0, t.first = 0, t.slices = 1;
      return true;
    case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
      t.mip = 0, t.first = d.Texture2DMSArray.FirstArraySlice, t.slices = d.Texture2DMSArray.ArraySize;
      return true;
    default:
      return false;
  }
}

// Gets t's texture and creates (or keeps) its copies.
const char* PrepareCopies(Target& t) {
  ID3D11Resource* r = nullptr;
  t.view->GetResource(&r);
  if (!r) return "view without resource";
  const HRESULT hr = r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t.res));
  r->Release();
  if (FAILED(hr) || !t.res) return "target is not a 2D texture";
  D3D11_TEXTURE2D_DESC rd;
  t.res->GetDesc(&rd);
  t.resMips = rd.MipLevels;
  t.ms = rd.SampleDesc.Count > 1;
  D3D11_TEXTURE2D_DESC cd = {};
  cd.Width = (std::max)(1u, rd.Width >> t.mip);
  cd.Height = (std::max)(1u, rd.Height >> t.mip);
  cd.MipLevels = 1;
  cd.ArraySize = t.slices;
  cd.SampleDesc = rd.SampleDesc;
  DXGI_FORMAT uintView = DXGI_FORMAT_UNKNOWN;
  if (t.ms) {
    if (!UintFamily(rd.Format, &cd.Format, &uintView)) return "MSAA target format without an exact per-sample compare";
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  } else {
    if (!TexelBytes(rd.Format)) return "target format with an unknown texel size";
    cd.Format = rd.Format;
    cd.Usage = D3D11_USAGE_STAGING;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
  }
  // Reused only when complete: a partly failed create (t.init made, t.a not)
  // used to pass this check on the next pass, and CopyAll then copied into a
  // null texture on the render thread.
  if (t.init && t.a && (!t.ms || (t.srvInit && t.srvA)) && memcmp(&cd, &t.copyDesc, sizeof(cd)) == 0) return nullptr;
  ReleaseCopies(t);
  t.copyDesc = cd;
  if (FAILED(g_dev->CreateTexture2D(&cd, nullptr, &t.init)) || FAILED(g_dev->CreateTexture2D(&cd, nullptr, &t.a))) {
    ReleaseCopies(t);
    return "could not create the target copies";
  }
  if (t.ms) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = uintView;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
    sd.Texture2DMSArray.FirstArraySlice = 0;
    sd.Texture2DMSArray.ArraySize = t.slices;
    if (FAILED(g_dev->CreateShaderResourceView(t.init, &sd, &t.srvInit)) ||
        FAILED(g_dev->CreateShaderResourceView(t.a, &sd, &t.srvA))) {
      ReleaseCopies(t);
      return "could not create the per-sample views";
    }
  }
  return nullptr;
}

// Copies every target subresource into a copy (toCopy) or back from init.
void CopyAll(bool toCopy, bool intoA) {
  for (Target& t : g_t) {
    if (!t.res) continue;
    ID3D11Texture2D* c = intoA ? t.a : t.init;
    for (UINT s = 0; s < t.slices; ++s) {
      const UINT rs = D3D11CalcSubresource(t.mip, t.first + s, t.resMips);
      if (toCopy)
        g_ctx->CopySubresourceRegion(c, s, 0, 0, 0, t.res, rs, nullptr);
      else
        g_ctx->CopySubresourceRegion(t.res, rs, 0, 0, 0, c, s, nullptr);
    }
  }
}

// The ownship mask (gb_rec.h, CockpitTrueIpdOwnship=2): a third run with the ownship at DCS's camera, copied into
// .m; a texel whose value in any staging target differs between A (the ownship at the true eye) and that run is an
// ownship texel (its value depends on the ownship's camera). Per staging target of the same size as the first.
std::vector<uint8_t> g_mask;
UINT g_maskW = 0, g_maskH = 0, g_maskS = 0;
bool g_maskOn = false;  // CompareStaging also counts the differing texels outside the mask (g_badOut)
int64_t g_badOut = 0;   // the last CompareAll's differing texels outside the mask (staging targets)
int64_t g_badIn = 0;    // ... inside it
// R129: the mask can also come from a same-frame stock control run (gb_rec.h VerifyPass: a second stock run from
// the same init, copied into .m): its texels are those where DCS's own output is not reproducible within the frame.
// With g_maskControl, a difference inside it whose B also differs from the control run's value (B matches neither
// stock run there) is counted in g_badNeither as well (diagnostic: it is still inside stock's own variation).
bool g_maskControl = false;
int64_t g_badNeither = 0;
// R133: the bracketed control. With g_bracket, B is read from .b (not .init) and the second stock control C2 (.c2,
// run after B) is a stock run too: g_badNeither then means "matches none of A, C1, C2" inside the mask. Over every
// differing texel (inside the mask or not) of a compare with g_maskControl:
//  g_badNoStock: B matches no stock run (A, C1, and C2 when bracketed): the texels that cannot be DCS's own output at
//                any of the sampled moments (gb_rec.h counts these as unexplained);
//  g_badLater:   B differs from A and C1 but equals C2: B looks like a later stock run (R131's burst onset).
bool g_bracket = false;
bool g_c2Valid = false;  // .c2 holds this compare's C2 (the second control ran in the same views)
int64_t g_badNoStock = 0, g_badLater = 0;
// R129: per target, the largest per-component difference of the last compare, in the format's value units
// (UNORM: 0..1, float formats: absolute); -1 when the format is not decoded (nothing differed: 0).
double g_maxDelta[kMaxTargets] = {};
// IEEE half to float (exact; no denormal flush).
inline float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1fu, man = h & 0x3ffu;
  float v;
  if (exp == 0)
    v = std::ldexp(static_cast<float>(man), -24);
  else if (exp == 31)
    v = man ? std::nanf("") : HUGE_VALF;
  else
    v = std::ldexp(static_cast<float>(man | 0x400u), static_cast<int>(exp) - 25);
  return sign ? -v : v;
}
// The largest per-component difference between two texels of format f, in value units; -1: format not decoded.
// A NaN or infinity on one side only counts as 1e30 (a gross difference).
inline double TexelMaxDelta(DXGI_FORMAT f, const uint8_t* a, const uint8_t* b) {
  auto fl = [](float x, float y) -> double {
    if (!std::isfinite(x) || !std::isfinite(y)) return (x == y || (std::isnan(x) && std::isnan(y))) ? 0.0 : 1e30;
    return std::fabs(static_cast<double>(x) - static_cast<double>(y));
  };
  int unorm8 = 0, half = 0, f32 = 0;
  switch (static_cast<int>(f)) {
    case 28: case 29: case 87: case 88: case 91: case 93: unorm8 = 4; break;  // R8G8B8A8 / B8G8R8A8(X8) UNORM(_SRGB)
    case 49: unorm8 = 2; break;                                                 // R8G8_UNORM
    case 61: case 65: unorm8 = 1; break;                                        // R8_UNORM, A8_UNORM
    case 10: half = 4; break;                                                   // R16G16B16A16_FLOAT
    case 34: half = 2; break;                                                   // R16G16_FLOAT
    case 54: half = 1; break;                                                   // R16_FLOAT
    case 2: f32 = 4; break;                                                     // R32G32B32A32_FLOAT
    case 6: f32 = 3; break;                                                     // R32G32B32_FLOAT
    case 16: f32 = 2; break;                                                    // R32G32_FLOAT
    case 40: case 41: f32 = 1; break;                                           // D32_FLOAT, R32_FLOAT
    case 19: case 20: case 21: f32 = 1; break;  // R32G8X24 family: the depth float only (stencil not decoded)
    default: return -1.0;
  }
  double m = 0.0;
  for (int i = 0; i < unorm8; ++i) m = (std::max)(m, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])) / 255.0);
  for (int i = 0; i < half; ++i) {
    uint16_t x, y;
    memcpy(&x, a + 2 * i, 2);
    memcpy(&y, b + 2 * i, 2);
    m = (std::max)(m, fl(HalfToFloat(x), HalfToFloat(y)));
  }
  for (int i = 0; i < f32; ++i) {
    float x, y;
    memcpy(&x, a + 4 * i, 4);
    memcpy(&y, b + 4 * i, 4);
    m = (std::max)(m, fl(x, y));
  }
  return m;
}
bool CopyAllMask() {  // the current targets into .m (created on demand); false: a target cannot take a mask
  for (Target& t : g_t) {
    if (!t.res) continue;
    if (t.ms) return false;
    if (!t.m && FAILED(g_dev->CreateTexture2D(&t.copyDesc, nullptr, &t.m))) return false;
    for (UINT s = 0; s < t.slices; ++s)
      g_ctx->CopySubresourceRegion(t.m, s, 0, 0, 0, t.res, D3D11CalcSubresource(t.mip, t.first + s, t.resMips), nullptr);
  }
  return true;
}
// R133: withC2 also marks the texels where A differs from the second stock control (.c2, after the recorded run):
// the mask is then every texel where any of the three stock runs (A, C1, C2) disagree.
// R133: the current targets into .b or .c2 (created on demand, staging); false: a target cannot take one (MSAA, or
// the create failed).
bool CopyAllTo(ID3D11Texture2D* Target::*which) {
  for (Target& t : g_t) {
    if (!t.res) continue;
    if (t.ms) return false;
    if (!(t.*which) && FAILED(g_dev->CreateTexture2D(&t.copyDesc, nullptr, &(t.*which)))) return false;
    for (UINT s = 0; s < t.slices; ++s)
      g_ctx->CopySubresourceRegion(t.*which, s, 0, 0, 0, t.res, D3D11CalcSubresource(t.mip, t.first + s, t.resMips),
                                   nullptr);
  }
  return true;
}
bool BuildMask(bool withC2 = false) {  // A (.a) vs the mask run (.m): the ownship's texels
  g_mask.clear();
  g_maskW = g_maskH = g_maskS = 0;
  for (Target& t : g_t) {
    if (!t.res || !t.m) continue;
    const D3D11_TEXTURE2D_DESC& d = t.copyDesc;
    if (!g_maskW) {
      g_maskW = d.Width, g_maskH = d.Height, g_maskS = d.ArraySize;
      g_mask.assign(static_cast<size_t>(g_maskW) * g_maskH * g_maskS, 0);
    }
    if (d.Width != g_maskW || d.Height != g_maskH || d.ArraySize != g_maskS) continue;
    const UINT bpt = TexelBytes(d.Format);
    if (!bpt) return false;
    for (UINT s = 0; s < d.ArraySize; ++s) {
      D3D11_MAPPED_SUBRESOURCE ma, mm;
      if (FAILED(g_ctx->Map(t.a, s, D3D11_MAP_READ, 0, &ma))) return false;
      if (FAILED(g_ctx->Map(t.m, s, D3D11_MAP_READ, 0, &mm))) {
        g_ctx->Unmap(t.a, s);
        return false;
      }
      D3D11_MAPPED_SUBRESOURCE mc = {};
      const bool haveC2 = withC2 && t.c2 && SUCCEEDED(g_ctx->Map(t.c2, s, D3D11_MAP_READ, 0, &mc));
      for (UINT y = 0; y < d.Height; ++y) {
        const auto* ra = static_cast<const uint8_t*>(ma.pData) + static_cast<size_t>(y) * ma.RowPitch;
        const auto* rm = static_cast<const uint8_t*>(mm.pData) + static_cast<size_t>(y) * mm.RowPitch;
        const auto* rc = haveC2 ? static_cast<const uint8_t*>(mc.pData) + static_cast<size_t>(y) * mc.RowPitch : nullptr;
        const size_t rowBytes = static_cast<size_t>(d.Width) * bpt;
        if (memcmp(ra, rm, rowBytes) == 0 && (!rc || memcmp(ra, rc, rowBytes) == 0)) continue;
        uint8_t* row = g_mask.data() + (static_cast<size_t>(s) * d.Height + y) * d.Width;
        for (UINT x = 0; x < d.Width; ++x)
          row[x] |= memcmp(ra + x * bpt, rm + x * bpt, bpt) != 0 || (rc && memcmp(ra + x * bpt, rc + x * bpt, bpt) != 0);
      }
      if (haveC2) g_ctx->Unmap(t.c2, s);
      g_ctx->Unmap(t.m, s);
      g_ctx->Unmap(t.a, s);
    }
  }
  return !g_mask.empty();
}

// Reads the bound views; nullptr, or why the pass cannot be compared.
const char* Capture() {
  ReleaseViews();
  ID3D11RenderTargetView* rtv[8] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  g_ctx->OMGetRenderTargets(8, rtv, &dsv);
  for (int i = 0; i < 8; ++i) g_t[i].view = rtv[i];
  g_t[8].view = dsv;
  const char* why = nullptr;
  std::string formats;
  for (int i = 0; i < kMaxTargets; ++i) {
    Target& t = g_t[i];
    if (!t.view) continue;
    ++g_count;
    if (!why && !ViewRange(t, i == 8)) why = "unsupported view dimension";
    if (!why) why = PrepareCopies(t);
    if (!t.res) continue;
    D3D11_TEXTURE2D_DESC rd;
    t.res->GetDesc(&rd);
    char b[80];
    snprintf(b, sizeof(b), "%s%s%d format %d %ux%u x%u, %u sample(s)", formats.empty() ? "" : "; ",
             i == 8 ? "DS" : "RT", i == 8 ? 0 : i, static_cast<int>(rd.Format), (std::max)(1u, rd.Width >> t.mip),
             (std::max)(1u, rd.Height >> t.mip), t.slices, rd.SampleDesc.Count);
    formats += b;
    g_targetsSeen++;
    if (t.ms) g_msTargets++;
  }
  g_lastFormats = formats;
  if (!why && !g_count) why = "no render target bound at the first item";
  return why;
}

void OnFirstItem() {
  if (g_captureWanted) {
    g_captureWanted = false;
    g_why = Capture();
    g_captured = !g_why;
    if (g_captured) CopyAll(true, false);  // init
  } else if (g_checkWanted) {
    g_checkWanted = false;
    ID3D11RenderTargetView* rtv[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    g_ctx->OMGetRenderTargets(8, rtv, &dsv);
    bool same = static_cast<ID3D11View*>(dsv) == g_t[8].view;
    for (int i = 0; i < 8; ++i) same &= static_cast<ID3D11View*>(rtv[i]) == g_t[i].view;
    for (int i = 0; i < 8; ++i) SafeRelease(rtv[i]);
    SafeRelease(dsv);
    g_sameViews = same;
  }
}

// ---- Compare ----
// The bounding box (x0, y0, x1, y1) of the texels that differed in the last staging compares (diagnostics).
int g_badBox[4] = {1 << 30, 1 << 30, -1, -1};
// m (optional, R129): the control run's copy of the same target; with g_maskControl, a difference inside the mask
// whose B also differs from m is counted in g_badNeither. maxDelta (optional): the largest component difference.
int64_t CompareStaging(ID3D11Texture2D* a, ID3D11Texture2D* b, uint64_t* texels, ID3D11Texture2D* m = nullptr,
                       double* maxDelta = nullptr, ID3D11Texture2D* c2 = nullptr) {
  D3D11_TEXTURE2D_DESC d;
  a->GetDesc(&d);
  const UINT bpt = TexelBytes(d.Format);
  if (!bpt) return -1;
  const size_t rowBytes = static_cast<size_t>(d.Width) * bpt;
  int64_t bad = 0;
  if (maxDelta) *maxDelta = 0.0;
  for (UINT s = 0; s < d.ArraySize; ++s) {
    D3D11_MAPPED_SUBRESOURCE ma, mb, mm = {};
    if (FAILED(g_ctx->Map(a, s, D3D11_MAP_READ, 0, &ma))) return -1;
    if (FAILED(g_ctx->Map(b, s, D3D11_MAP_READ, 0, &mb))) {
      g_ctx->Unmap(a, s);
      return -1;
    }
    const bool haveM = m && SUCCEEDED(g_ctx->Map(m, s, D3D11_MAP_READ, 0, &mm));
    D3D11_MAPPED_SUBRESOURCE mc = {};
    const bool haveC = c2 && SUCCEEDED(g_ctx->Map(c2, s, D3D11_MAP_READ, 0, &mc));
    if (ma.RowPitch < rowBytes || mb.RowPitch < rowBytes || (haveM && mm.RowPitch < rowBytes) ||
        (haveC && mc.RowPitch < rowBytes)) {
      if (haveC) g_ctx->Unmap(c2, s);
      if (haveM) g_ctx->Unmap(m, s);
      g_ctx->Unmap(b, s);
      g_ctx->Unmap(a, s);
      return -1;
    }
    for (UINT y = 0; y < d.Height; ++y) {
      const auto* ra = static_cast<const uint8_t*>(ma.pData) + static_cast<size_t>(y) * ma.RowPitch;
      const auto* rb = static_cast<const uint8_t*>(mb.pData) + static_cast<size_t>(y) * mb.RowPitch;
      if (memcmp(ra, rb, rowBytes) == 0) continue;
      const auto* rm = haveM ? static_cast<const uint8_t*>(mm.pData) + static_cast<size_t>(y) * mm.RowPitch : nullptr;
      const auto* rc = haveC ? static_cast<const uint8_t*>(mc.pData) + static_cast<size_t>(y) * mc.RowPitch : nullptr;
      const bool masked = g_maskOn && d.Width == g_maskW && d.Height == g_maskH && d.ArraySize == g_maskS;
      const uint8_t* mrow = masked ? g_mask.data() + (static_cast<size_t>(s) * d.Height + y) * d.Width : nullptr;
      for (UINT x = 0; x < d.Width; ++x) {
        if (memcmp(ra + x * bpt, rb + x * bpt, bpt) == 0) continue;
        ++bad;
        if (maxDelta && *maxDelta >= 0.0) {
          const double dd = TexelMaxDelta(d.Format, ra + x * bpt, rb + x * bpt);
          *maxDelta = dd < 0.0 ? -1.0 : (std::max)(*maxDelta, dd);
        }
        if (g_maskOn) (mrow && mrow[x] ? g_badIn : g_badOut)++;
        if (g_maskOn && g_maskControl && rm) {
          const bool eqC1 = memcmp(rm + x * bpt, rb + x * bpt, bpt) == 0;
          const bool eqC2 = rc && memcmp(rc + x * bpt, rb + x * bpt, bpt) == 0;
          if (!eqC1 && !eqC2) {
            ++g_badNoStock;
            if (mrow && mrow[x]) ++g_badNeither;
          }
          if (!eqC1 && eqC2) ++g_badLater;
        }
        g_badBox[0] = (std::min)(g_badBox[0], static_cast<int>(x));
        g_badBox[1] = (std::min)(g_badBox[1], static_cast<int>(y));
        g_badBox[2] = (std::max)(g_badBox[2], static_cast<int>(x));
        g_badBox[3] = (std::max)(g_badBox[3], static_cast<int>(y));
      }
    }
    *texels += static_cast<uint64_t>(d.Width) * d.Height;
    if (haveC) g_ctx->Unmap(c2, s);
    if (haveM) g_ctx->Unmap(m, s);
    g_ctx->Unmap(b, s);
    g_ctx->Unmap(a, s);
  }
  return bad;
}

const char* const kCs =
    "Texture2DMSArray<uint4> A : register(t0);\n"
    "Texture2DMSArray<uint4> B : register(t1);\n"
    "RWByteAddressBuffer R : register(u0);\n"
    "[numthreads(8, 8, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "  uint w, h, n, s;\n"
    "  A.GetDimensions(w, h, n, s);\n"
    "  if (id.x >= w || id.y >= h || id.z >= n) return;\n"
    "  uint bad = 0;\n"
    "  for (uint i = 0; i < s; ++i)\n"
    "    bad += any(A.Load(int3(id.xy, id.z), i) != B.Load(int3(id.xy, id.z), i)) ? 1 : 0;\n"
    "  if (bad) { uint o; R.InterlockedAdd(0, bad, o); }\n"
    "}\n";

bool PrepareCs() {
  if (g_cs) return true;
  const shadowinst::Compiler& c = shadowinst::g_compiler;
  if (!c.compile2 || !g_dev) return false;
  ID3DBlob* code = nullptr;
  ID3DBlob* err = nullptr;
  const HRESULT hr = c.compile2(kCs, strlen(kCs), "qv_gbuffer_compare", nullptr, nullptr, "cs", "cs_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, 0, nullptr, 0, &code, &err);
  SafeRelease(err);
  if (FAILED(hr) || !code) return false;
  const bool ok = SUCCEEDED(g_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cs));
  code->Release();
  if (!ok) return false;
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = 16;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_counter))) return false;
  bd.Usage = D3D11_USAGE_STAGING;
  bd.BindFlags = 0;
  bd.MiscFlags = 0;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_counterRead))) return false;
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = DXGI_FORMAT_R32_TYPELESS;
  ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = 4;
  ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  return SUCCEEDED(g_dev->CreateUnorderedAccessView(g_counter, &ud, &g_counterUav));
}

// Per-sample compare of two MS copies on the GPU; CS state saved/restored.
int64_t CompareMs(Target& t, uint64_t* texels) {
  if (!PrepareCs()) return -1;
  ID3D11ComputeShader* oldCs = nullptr;
  ID3D11ClassInstance* inst[256] = {};
  UINT nInst = 256;
  g_ctx->CSGetShader(&oldCs, inst, &nInst);
  ID3D11ShaderResourceView* oldSrv[2] = {};
  g_ctx->CSGetShaderResources(0, 2, oldSrv);
  ID3D11UnorderedAccessView* oldUav = nullptr;
  g_ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
  const uint32_t zero[4] = {};
  g_ctx->UpdateSubresource(g_counter, 0, nullptr, zero, 0, 0);
  ID3D11ShaderResourceView* srv[2] = {t.srvA, t.srvInit};
  g_ctx->CSSetShader(g_cs, nullptr, 0);
  g_ctx->CSSetShaderResources(0, 2, srv);
  g_ctx->CSSetUnorderedAccessViews(0, 1, &g_counterUav, nullptr);
  g_ctx->Dispatch((t.copyDesc.Width + 7) / 8, (t.copyDesc.Height + 7) / 8, t.slices);
  g_ctx->CSSetShader(oldCs, nInst ? inst : nullptr, nInst);
  g_ctx->CSSetShaderResources(0, 2, oldSrv);
  const UINT keep = static_cast<UINT>(-1);
  g_ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, &keep);
  SafeRelease(oldCs);
  for (UINT i = 0; i < nInst && i < 256; ++i) SafeRelease(inst[i]);
  SafeRelease(oldSrv[0]);
  SafeRelease(oldSrv[1]);
  SafeRelease(oldUav);
  g_ctx->CopyResource(g_counterRead, g_counter);
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(g_ctx->Map(g_counterRead, 0, D3D11_MAP_READ, 0, &m))) return -1;
  const uint32_t bad = *static_cast<const uint32_t*>(m.pData);
  g_ctx->Unmap(g_counterRead, 0);
  *texels += static_cast<uint64_t>(t.copyDesc.Width) * t.copyDesc.Height * t.slices * t.copyDesc.SampleDesc.Count;
  return bad;
}

// Compares A (.a) with B (.init) for every captured target; per-target
// counts in bad[]; returns the total, or -1 on an error.
int64_t CompareAll(int64_t bad[kMaxTargets], uint64_t* texels) {
  int64_t total = 0;
  g_badIn = g_badOut = g_badNeither = g_badNoStock = g_badLater = 0;
  for (int i = 0; i < kMaxTargets; ++i) {
    bad[i] = 0;
    g_maxDelta[i] = 0.0;
    Target& t = g_t[i];
    if (!t.res) continue;
    // MSAA: compared per sample on the GPU, no value decode (-1 when it differs).
    // R133: bracketed, B is in .b (init was restored once more for C2) and C2 in .c2.
    const bool br = g_bracket && t.b;
    const int64_t b = t.ms ? CompareMs(t, texels)
                           : CompareStaging(t.a, br ? t.b : t.init, texels, g_maskOn && g_maskControl ? t.m : nullptr,
                                            &g_maxDelta[i], br && g_c2Valid && g_maskOn && g_maskControl ? t.c2 : nullptr);
    if (t.ms && b > 0) g_maxDelta[i] = -1.0;
    if (b < 0) return -1;
    bad[i] = b;
    total += b;
  }
  return total;
}

int64_t CompareGuarded(int64_t bad[kMaxTargets], uint64_t* texels) {
  __try {
    return CompareAll(bad, texels);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// R133 ([Suite] GBufferRecVerifyDump, test only): one compare's runs as raw files for offline analysis, written by
// gb_rec.h VerifyPass after the compare. Into dir (created; ends with a backslash): per staging target i and run r
// ("A" first stock run, "C1" stock control before the recorded run, "B" the recorded run, "C2" stock control after
// it; a run without a copy is skipped), "<RTi|DS0>_<r>.raw": the texels of box (x0, y0, x1, y1, inclusive; clamped
// to the target) row by row top-down, tightly packed (width * bytes per texel per row), slice after slice; and
// "header.txt": the caller's lines, then one line per target (format number, full size, slices, bytes per texel,
// the crop, the files). control: C1 / C2 exist (else only A and B). Not for MSAA targets (skipped). Returns the
// files written (header included), or -1 when nothing could be.
int DumpCompare(const std::wstring& dir, const std::string& head, const int box[4], bool control) {
  if (dir.empty() || box[2] < box[0] || box[3] < box[1]) return -1;
  for (size_t i = 1; i <= dir.size(); ++i)  // every level of the folder
    if (i == dir.size() || dir[i] == L'\\') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
  std::string hdr = head;
  int files = 0;
  for (int i = 0; i < kMaxTargets; ++i) {
    Target& t = g_t[i];
    if (!t.res || t.ms) continue;
    const D3D11_TEXTURE2D_DESC& d = t.copyDesc;
    const UINT bpt = TexelBytes(d.Format);
    if (!bpt || !d.Width || !d.Height) continue;
    const UINT x0 = static_cast<UINT>((std::max)(0, box[0])), y0 = static_cast<UINT>((std::max)(0, box[1]));
    const UINT x1 = (std::min)(d.Width - 1, static_cast<UINT>((std::max)(0, box[2])));
    const UINT y1 = (std::min)(d.Height - 1, static_cast<UINT>((std::max)(0, box[3])));
    if (x0 > x1 || y0 > y1) continue;
    const UINT w = x1 - x0 + 1, h = y1 - y0 + 1;
    char tn[8];
    snprintf(tn, sizeof(tn), "%s%d", i == 8 ? "DS" : "RT", i == 8 ? 0 : i);
    struct Run {
      const char* name;
      ID3D11Texture2D* tex;
    } runs[4] = {{"A", t.a}, {"C1", control ? t.m : nullptr}, {"B", g_bracket && t.b ? t.b : t.init},
                     {"C2", control && g_bracket && g_c2Valid ? t.c2 : nullptr}};
    std::string names;
    for (const Run& r : runs) {
      if (!r.tex) continue;
      std::vector<uint8_t> buf(static_cast<size_t>(w) * bpt * h * d.ArraySize);
      bool ok = true;
      for (UINT s = 0; s < d.ArraySize && ok; ++s) {
        D3D11_MAPPED_SUBRESOURCE mp;
        if (FAILED(g_ctx->Map(r.tex, s, D3D11_MAP_READ, 0, &mp))) {
          ok = false;
          break;
        }
        for (UINT y = 0; y < h; ++y)
          memcpy(buf.data() + ((static_cast<size_t>(s) * h + y) * w) * bpt,
                 static_cast<const uint8_t*>(mp.pData) + static_cast<size_t>(y0 + y) * mp.RowPitch +
                     static_cast<size_t>(x0) * bpt,
                 static_cast<size_t>(w) * bpt);
        g_ctx->Unmap(r.tex, s);
      }
      if (!ok) continue;
      const std::wstring file = std::wstring(tn, tn + strlen(tn)) + L"_" +
                                std::wstring(r.name, r.name + strlen(r.name)) + L".raw";
      FILE* f = nullptr;
      if (_wfopen_s(&f, (dir + file).c_str(), L"wb") || !f) continue;
      const bool wrote = fwrite(buf.data(), 1, buf.size(), f) == buf.size();
      fclose(f);
      if (!wrote) continue;
      ++files;
      names += names.empty() ? "" : " ";
      names += std::string(tn) + "_" + r.name + ".raw";
    }
    char line[256];
    snprintf(line, sizeof(line), "%s format %d size %ux%u slices %u bpt %u crop %u,%u %ux%u files %s\n", tn,
             static_cast<int>(d.Format), d.Width, d.Height, d.ArraySize, bpt, x0, y0, w, h,
             names.empty() ? "-" : names.c_str());
    hdr += line;
  }
  FILE* f = nullptr;
  if (_wfopen_s(&f, (dir + L"header.txt").c_str(), L"wb") || !f) return files ? files : -1;
  fwrite(hdr.data(), 1, hdr.size(), f);
  fclose(f);
  return files + 1;
}

void ReportMismatch(bool ab, int64_t total, uint64_t texels, const int64_t bad[kMaxTargets]) {
  (ab ? g_abBad : g_aaBad)++;
  g_badTexels += static_cast<uint64_t>(total);
  std::string per;
  char b[48];
  for (int i = 0; i < kMaxTargets; ++i) {
    if (!g_t[i].res) continue;
    g_badPerTarget[i] += static_cast<uint64_t>(bad[i]);
    snprintf(b, sizeof(b), "%s%s%d %lld", per.empty() ? "" : ", ", i == 8 ? "DS" : "RT", i == 8 ? 0 : i,
             static_cast<long long>(bad[i]));
    per += b;
  }
  Log("g-buffer verify: %s differs: %lld of %llu texels/samples (%s)", ab ? "stock vs batched" : "stock vs stock",
      static_cast<long long>(total), static_cast<unsigned long long>(texels), per.c_str());
  Disable(ab ? "G-buffer differs between the stock and the batched pass"
             : "the stock G-buffer pass is not reproducible (stock vs stock differs)");
}

// One pass execution while verifying (gbbatch::g_verifier). Passes that are
// not compared run stock during A/A and batched during A/B.
void Verify(void* pass, void* ctx, gbpass::ExecFn orig, void** vec) {
  const bool ab = g_aa.load() >= static_cast<uint64_t>(g_aaPasses.load());
  const int every = (std::max)(1, g_every.load());
  if (++g_tick % static_cast<uint32_t>(every) != 0) {
    if (ab && PlanGuarded(vec)) {
      g_passes++;
      RunPlanned(pass, ctx, orig, true);
    } else {
      orig(pass, ctx);
    }
    return;
  }
  // Run 1: stock; capture at the first item.
  ClearPlan();
  g_captureWanted = true;
  g_captured = false;
  g_why = nullptr;
  g_onFirstItem = &OnFirstItem;
  RunPlanned(pass, ctx, orig, false);
  g_captureWanted = false;
  if (!g_captured) {
    g_onFirstItem = nullptr;
    if (g_why) {
      g_errors++;
      g_lastWhy = g_why;
      if (strncmp(g_why, "MSAA", 4) == 0 || strncmp(g_why, "target format", 13) == 0 ||
          strncmp(g_why, "unsupported", 11) == 0)
        Disable(g_why);
    }
    ReleaseViews();
    return;  // ran once, stock
  }
  CopyAll(true, true);    // A
  CopyAll(false, false);  // init back into the targets
  // Run 2: stock again (A/A) or batched (A/B).
  bool batched = false;
  if (ab) {
    batched = PlanGuarded(vec);
    if (batched)
      g_passes++;
    else
      ClearPlan();
  }
  g_checkWanted = true;
  g_sameViews = false;
  RunPlanned(pass, ctx, orig, batched);
  g_checkWanted = false;
  g_onFirstItem = nullptr;
  if (!g_sameViews || (ab && !batched)) {  // other views, or nothing batched: no sample
    g_skippedCmp++;
    ReleaseViews();
    return;
  }
  CopyAll(true, false);  // B
  int64_t bad[kMaxTargets];
  uint64_t texels = 0;
  const int64_t total = CompareGuarded(bad, &texels);
  if (total < 0) {
    g_errors++;
    g_lastWhy = "compare failed";
  } else {
    (ab ? g_ab : g_aa)++;
    g_texels += texels;
    if (total > 0) ReportMismatch(ab, total, texels, bad);
  }
  ReleaseViews();
}

bool Install() {
  if (!g_dev && shadowinst::g_device) {
    g_dev = shadowinst::g_device;
    g_dev->AddRef();
  }
  if (!g_dev) return false;
  g_verifier = &Verify;
  return true;
}

void Shutdown() {
  g_verifier = nullptr;
  g_onFirstItem = nullptr;
  ReleaseViews();
  for (Target& t : g_t) ReleaseCopies(t);
  SafeRelease(g_cs);
  SafeRelease(g_counterUav);
  SafeRelease(g_counter);
  SafeRelease(g_counterRead);
  SafeRelease(g_dev);
}

void ResetCounters() {
  for (auto* c : {&g_aa, &g_ab, &g_aaBad, &g_abBad, &g_texels, &g_badTexels, &g_skippedCmp, &g_errors, &g_targetsSeen,
                  &g_msTargets})
    c->store(0);
  for (auto& b : g_badPerTarget) b.store(0);
  g_tick = 0;
  g_lastWhy = nullptr;
}

void LogCounters() {
  std::string per;
  char b[48];
  for (int i = 0; i < kMaxTargets; ++i) {
    const uint64_t v = g_badPerTarget[i].load();
    if (!v) continue;
    snprintf(b, sizeof(b), "%s%s%d %llu", per.empty() ? "" : ", ", i == 8 ? "DS" : "RT", i == 8 ? 0 : i,
             static_cast<unsigned long long>(v));
    per += b;
  }
  Log("  g-buffer verify: stock vs stock %llu passes (%llu differ), stock vs batched %llu passes (%llu differ); "
      "%llu of %llu texels/samples differ%s%s; %llu passes not comparable, %llu errors%s%s; targets seen %llu "
      "(%llu MSAA)",
      static_cast<unsigned long long>(g_aa.load()), static_cast<unsigned long long>(g_aaBad.load()),
      static_cast<unsigned long long>(g_ab.load()), static_cast<unsigned long long>(g_abBad.load()),
      static_cast<unsigned long long>(g_badTexels.load()), static_cast<unsigned long long>(g_texels.load()),
      per.empty() ? "" : "; by target: ", per.c_str(), static_cast<unsigned long long>(g_skippedCmp.load()),
      static_cast<unsigned long long>(g_errors.load()), g_lastWhy ? " (last: " : "", g_lastWhy ? g_lastWhy : "",
      static_cast<unsigned long long>(g_targetsSeen.load()), static_cast<unsigned long long>(g_msTargets.load()));
  if (!g_lastFormats.empty()) Log("  g-buffer verify: last captured targets: %s", g_lastFormats.c_str());
}

}  // namespace gbverify
