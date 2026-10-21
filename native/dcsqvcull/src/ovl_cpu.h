// R32: the cockpit symbology's camera set where DCS builds it, on the CPU
// ([Perception] CockpitTrueIpdOverlayPath=cpu, the default; design:
// docs/research R32_symbology_dual_camera.md). Included from cockpit_ipd.h
// inside namespace ckipd (after the overlay MVP scan, before OvlEffectUpload).
//
// Why (R32 0-2): DCS builds every indicator MVP as world * ContextBase::
// currentContext()->viewProj (+0x1b4; GraphicsVista's indicator materials,
// [V]) and FX11 uploads the whole effect buffer before every symbology draw.
// The old d3d11 path (patch the uploaded bytes) could never be complete: after
// a d3d11 table toggle the next upload always goes out before any hook of ours
// runs (one frame of symbols at the boosted eye: the flicker). It also moved
// the collimated HUD, which DCS builds per eye at infinity (O = E_t + d F): with
// DCS's own (boosted) camera its disparity against the far horizon is 0 px,
// patched it is 20 px (dump 214533, R32 2.6). So:
//  - The record replay (CockpitBase ccMTHandlerImpl vtable slot 3) is wrapped:
//    DCS's 64 bytes at currentContext+0x1b4 saved, the original called, the
//    bytes put back (unless DCS wrote its own camera there meanwhile: counted
//    "restore mismatch", left as DCS's).
//  - The per-record filter (std::function _Impl vt+0x10: the Visualizer overlay
//    lambda and CockpitBase's composite lambda) is wrapped: right after a record
//    passes it (its MVP is built and drawn before the next record's filter
//    call), +0x1b4 is set to VP_true = T(-Dw) * VP_dcs for a record of class T
//    (cockpit- or head-fixed: DED, MFD LCD quads, EHSI) and to DCS's VP for class
//    B (eye-relative: the HUD and its DED repeater) and for unknown keys. DCS
//    then builds and uploads correct bytes itself, whatever the d3d11 table
//    state is: no toggle can change an image.
//  - One decision per FRAME for all four views (R32 r3): the overlays' on / inert
//    state, the cockpit's frame-wide off and a pending learner reset are latched
//    at the frame's first replay; one decision per view (all of a view's replays
//    moved, or none). Once any HMD view of the frame cannot be moved (a failure,
//    its own cockpit not moved, no per-record point and not every record of the
//    frame's views committed T) no view is moved for the rest of that frame and
//    the next 30 (the frame stop); a stop that comes after views of the frame
//    were drawn moved is counted ovlCpuPairLate (views of one eye then differ in
//    that one frame: a later view's failure cannot be known before it is drawn).
//    A replay without a per-record point (null / unknown filter, no composite)
//    moves only when every drawable record of every HMD view's range has a
//    committed T key (one scan per frame, so the views decide alike).
//  - The record a filter call belongs to is known exactly: the replay's records
//    are DCS's own array (handler +0x20, 0xd0 bytes each, the view's packed range
//    at handler +0x98[view]), and 0x3c5bd0 calls the filter once per record in
//    order (checked per call: the record's +0xc8 / +0xc9 equal the bytes DCS
//    passed). A record key is the record's element identity: its two object
//    pointers (+0 / +8, the element's RenderObject +0x68 / +0x60, CockpitBase
//    0x3c6d70), +0xc9 / +0xc8, and the ordinal among records with exactly these
//    four in the replay (an element drawn twice). Records of other elements
//    never shift a key, whatever their count, order or per-view culling; views
//    2 / 3 name the same elements as views 0 / 1. (The former key, the ordinal
//    within (c9, c8), aliased between HUD and display records of one pair.)
//  - The class of a key is learned from the d3d11 observer (the overlay passes'
//    upload hook, which writes nothing on this path): the model origin of the
//    record (its world matrix, or the uploaded MVP through DCS's VP^-1), for the
//    two eyes of a pair in one frame (views 0 / 1 and views 2 / 3, both
//    measured). Its offset from the view's own (boosted) eye equal in both views
//    (< 3 mm): built relative to the eye, B. The origin itself equal: fixed to
//    the cockpit or the head, T. 16 agreeing frames commit a class. A key that
//    shows the other outcome once after its commit, or both outcomes twice before
//    it, is frozen at B (DCS's own camera) at once, "mixed key" (a contradiction
//    of a committed key is counted; it is the key's failure, not the view's).
//    Keys not seen for 600 frames are forgotten, frozen ones too (checked a few
//    slots per frame). A key names resources (the geometry / material heap
//    pointers), not an element: the table is cleared on the render thread at the
//    frame end after a mission event (load, slot change, simulation stop: the
//    Lua hook through main.cpp), a gap scene cut (scene_cut.h: a load; jump /
//    turn cuts free nothing and keep the table) or a (re)install, and
//    the frame in which such a reset is pending moves nothing. A key never seen
//    since the last reset is unknown: DCS's eye.
//  - A failure that names one record key (the observer's cpu miss: a T record
//    still at DCS's camera; wrong B: a B record at VP_true) freezes that key at
//    DCS's eye at once and is not a failure of the view (no frame stop, no
//    inert count); the same key failing again once frozen is not the key's: a
//    failure of the view.
//  - All hook sites are verified byte for byte (code hashes, vtable targets) at
//    install; a mismatch leaves the overlays at DCS's camera (the d3d11 path does
//    not take over).
// Every failure draws DCS's own symbology (today's image without option 3's
// overlay part), counted; failures (camera identity, split, context switched
// or rewritten inside a replay, a record not aligned with DCS's array, the
// observer's wrong B that no key explains) feed the overlays' inert window
// (> 1 % of the attempts over 300 frames: overlays at DCS's camera until
// reload). A frame stop (a failure, or a rule stop after views were drawn
// moved) within 300 frames of the previous one makes the overlays inert at
// once (each costs a 30-frame slide of the displays against their bezels; a
// recurring one is worse than the steady fail-safe). Frames inside a stop's
// 30-frame block are not checked at all (a 2-frame camera mismatch is one
// failure, not a recurrence).
// A view's replay is moved entirely or not at all, with one exception that
// cannot be undone: DCS switching to another camera inside the replay (counted
// ctxForeign, a failure). Records drawn before the switch keep the true eye; at
// the switch DCS's bytes are put back into the saved context (and into a pushed
// copy that carries ours), so every later record, the HUD included, is DCS's.
// cPerView (b7) of the overlay passes is left as DCS's: under a pure eye
// translation it does not change any symbol position (R32 3).
// R32 r4 (live F-16, 2026-10-14: every replay "null filter", the overlays inert after two camera failures, 0
// records moved): (1) every HMD overlay replay of DCS 2.9.30 comes from Visualizer 0x15d640 with mask -1, which
// passes an empty std::function, and the handler's extra filter (+0x4b0) is null: no filter point at all. The
// per-record point is now CockpitBase's import slot of RenderObject::render (the record loop's only call, its
// RenderObject naming the record: kRenderIat); without it the whole-frame scan still decides. (2) The context's VP
// at +0x1b4 is row-major (LoadCtxVp): read in cPerView's layout it was no camera, so the camera identity failed in
// every frame, and VP_true would have been built on the transpose.
// Not done here (R32 phase 2 / open): the cursor after the records (vt[10])
// stays at DCS's camera; the learned table is not persisted across reloads.
#pragma once

namespace ocpu {

// ---------------------------------------------------------------------------
// DCS 2.9.30 sites (R32 4.1); checked at install, byte for byte
// ---------------------------------------------------------------------------

enum Mod : int { kModCockpit = 0, kModVis, kModGc, kModGv, kModCount };
const wchar_t* const kModNames[kModCount] = {L"CockpitBase.dll", L"Visualizer.dll", L"GraphicsCore.dll",
                                             L"GraphicsVista.dll"};
struct CodeSig {
  int mod;
  uint32_t begin, end;
  uint64_t hash;  // FNV-1a 64 of the bytes (no relocation inside any range)
};
constexpr CodeSig kCode[] = {
    {kModCockpit, 0x3c6570, 0x3c65e0, 0xab8a9abfaab9598aull},  // the replay: view -> range, composite filter, call
    {kModCockpit, 0x3c5c50, 0x3c5c87, 0x6e64c89a3b51438bull},  // per record: _Impl+0x38, vt[+0x10](&c8, &c9)
    {kModCockpit, 0x3c3de0, 0x3c3e1f, 0x92531274d9d9e4d5ull},  // composite filter: the inner filter first
    {kModCockpit, 0x3c6051, 0x3c6077, 0x40ec9594c0f347dcull},  // per record: getCurrentContext, render (immediate)
    {kModCockpit, 0x3c6667, 0x3c66b0, 0xf507ecc6487779c5ull},  // range: lo16 / hi16 records of handler+0x20
    {kModCockpit, 0x3c678f, 0x3c679e, 0xf56241eb839403f5ull},  // {first record, count} handed to 0x3c5bd0
    {kModCockpit, 0x3c5c21, 0x3c5c38, 0x683c382924a86118ull},  // 0x3c5bd0: rbx = first, end = first + n * 0xd0
    {kModCockpit, 0x3c5c87, 0x3c5c94, 0x932b794bdb3beed1ull},  // a record with +8 and +0 null: a state record
    {kModCockpit, 0x3c6077, 0x3c6087, 0x48f99d266883161eull},  // every record in order, 0xd0 apart
    {kModCockpit, 0x3c6d70, 0x3c6d9e, 0xaaefba7b5a773c69ull},  // the recorder: +0 / +8 the element's object pointers
    {kModVis, 0x160710, 0x16071e, 0x5120b1051f19c035ull},      // overlay lambda: bt mask, c9
    {kModGc, 0x1d70, 0x1d78, 0x1866e6365dc61adbull},           // getCurrentContext: the static at 0x11d510
    {kModGc, 0x18ce6, 0x18cf4, 0x0b499d763064535cull},         // updateBuffers: +0x1b4 -> cPerView gViewProj
    {kModGv, 0x1f700, 0x1f70b, 0x531431716b974be7ull},         // Graphics::Context::getCurrentContext
    {kModGv, 0x18906, 0x18953, 0xe8bae405aa31f6c1ull},         // indicator material: mul(world, ctx+0x1b4)
    {kModGv, 0x13e92, 0x13e99, 0x0c04099b488b96adull},         // the other MVP builders: lea r8, [rax+0x1b4]
    {kModGv, 0x18d6a, 0x18d71, 0x0c04099b488b96adull},
    {kModGv, 0x1a85a, 0x1a861, 0x0c04099b488b96adull},
    {kModCockpit, 0x3c5cef, 0x3c5cff, 0x9cf3b0309218fc41ull},  // per record: RenderObject at rsp+0x70 constructed
    {kModCockpit, 0x3c6016, 0x3c6051, 0x40c037b526c9d93dull},  // RenderObject +0x78 = record+0x40, +0x60/+0x68 ptrs
};
// The render point (R32 r4): CockpitBase's only record-loop call of Graphics::RenderObject::render goes through its
// import slot (0x3c6065, returning to 0x3c606b) with rcx = the record's RenderObject, which carries the record
// (+0x78 = record + 0x40, +0x60 / +0x68 = the record's +8 / +0 object pointers). Every drawing record reaches it,
// with or without a filter, so it is a per-record point for replays whose filter is empty (Visualizer 0x15d6ca:
// mask -1 passes an empty std::function, the case of every HMD overlay replay in DCS 2.9.30 with
// HUD_MFD_after_DLSS off). The MVP is built inside render (GraphicsVista indicator materials): the bytes at
// +0x1b4 when it is called are the camera of that record.
constexpr uint32_t kRenderIat = 0x448530, kRenderRet = 0x3c606b;
const char kRenderImport[] = "?render@RenderObject@Graphics@@UEAAXW4Pass@2@@Z";
constexpr uint32_t kRoObj8 = 0x60, kRoObj0 = 0x68, kRoRec = 0x78, kRecFromRo = 0x40;
struct VtSig {
  int mod;
  uint32_t vt;
  int slot;
  uint32_t target;
};
enum : int { kVtReplay = 0, kVtComp, kVtVis, kVtCount };
constexpr VtSig kVt[kVtCount] = {
    {kModCockpit, 0x46d1d8, 3, 0x3c6570},  // ccMTHandlerImpl: replay a view's records
    {kModCockpit, 0x46d260, 2, 0x3c3de0},  // the composite filter lambda's _Do_call
    {kModVis, 0x215778, 2, 0x160710},      // Visualizer's overlay filter lambda's _Do_call
};
constexpr uint32_t kCurCtxRva = 0x11d510;  // GraphicsCore ContextBase::currentContext (exported)
const char kCurCtxExport[] = "?currentContext@ContextBase@GraphicsCore@@1PEAV12@EA";
constexpr uint32_t kCtxVP = 0x1b4, kCtxProj = 0x134;  // ContextBase: viewProj, proj (row vectors; see LoadCtxVp)
constexpr uint32_t kFnImpl = 0x38;                    // MSVC std::function: the _Impl pointer
// ccMTHandlerImpl (CockpitBase 0x3c6570 / 0x3c6640 / 0x3c5bd0, [V]): the record array, the per-view packed ranges
// (int vector: lo16 the first record, hi16 the end), the extra filter that makes DCS wrap the passed one in its
// composite lambda. A record: +0 / +8 object pointers (both 0: a state record), +0xc8 / +0xc9 the filter bytes.
constexpr uint32_t kHRecs = 0x20, kHViews = 0x98, kHViewsEnd = 0xa0, kHComposite = 0x4b0;
constexpr uint32_t kRecSize = 0xd0, kRecC8 = 0xc8, kRecC9 = 0xc9;

inline uint64_t Fnv(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}
// Pure: the sites in module images (image[m]: the module by RVA, null when not loaded; valueBase[m]: what its
// vtable pointers are relative to, the load address or, in an unrelocated image, the preferred base). slotValue
// reads a vtable slot (the loader registry's original: another payload version may hold it). Null: all match.
using SlotReadFn = void* (*)(void** slot);
inline void* PlainSlot(void** slot) { return *slot; }
// A slot's value through slotValue, only once the slot itself was read without a fault (SEH, no lock held):
// slotValue may take a lock (the loader registry's g_regMutex), and a fault under that lock must never leave it
// held, so no fault handler of ours spans the call (R32 r3).
inline bool ReadSlot(void** slot, SlotReadFn slotValue, void** out) {
  void* probe = nullptr;
  if (!slot || !allocslab::ReadBytes(slot, &probe, sizeof(probe))) return false;
  *out = slotValue(slot);
  return true;
}
const char* VerifySites(uint8_t* const image[kModCount], const uintptr_t valueBase[kModCount],
                        SlotReadFn slotValue = &PlainSlot) {
  for (int m = 0; m < kModCount; ++m)
    if (!image[m]) return "a module is not loaded";
  for (const CodeSig& c : kCode) {
    uint8_t buf[256];
    const size_t n = c.end - c.begin;
    if (n > sizeof(buf) || !allocslab::ReadBytes(image[c.mod] + c.begin, buf, n) || Fnv(buf, n) != c.hash)
      return "code does not match this build";
  }
  for (const VtSig& v : kVt) {
    void* cur = nullptr;
    void** slot = reinterpret_cast<void**>(image[v.mod] + v.vt) + v.slot;
    if (!ReadSlot(slot, slotValue, &cur)) return "a vtable is not readable";
    if (reinterpret_cast<uintptr_t>(cur) != valueBase[v.mod] + v.target) return "a vtable does not match this build";
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Pure helpers (tests call them)
// ---------------------------------------------------------------------------

// VP_true = T(-dw) * vp: row 3 += -(dw . rows 0..2); rows 0..2 keep their bits. The same delta as ApplyMvps.
inline M4 TrueVp(const M4& vp, const double dw[3]) {
  M4 m = vp;
  for (int c = 0; c < 4; ++c) AddTo(m.a[3][c], -(dw[0] * vp.a[0][c] + dw[1] * vp.a[1][c] + dw[2] * vp.a[2][c]));
  return m;
}
// The context's viewProj bytes (R32 r4). DCS 2.9.30 keeps the row-vector VP row-major at +0x1b4 (f[r * 4 + c]):
// the transpose of cPerView's gViewProj layout (LoadM). Live 2026-10-14 (F-16, all four HMD views): read in
// cPerView's layout it is no camera at all (centre ~430 m away, every frame's camera identity failed); read
// row-major it is the view's G-buffer camera up to the DLSS jitter. Both layouts are accepted per replay (the one
// that is the view's camera, DCS 2.9.30's row-major first); ours is written back in the layout read.
inline M4 LoadCtxVp(const uint8_t* p, bool rowMajor) {
  float f[16];
  memcpy(f, p, sizeof(f));
  M4 m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) m.a[r][c] = rowMajor ? f[r * 4 + c] : f[c * 4 + r];
  return m;
}
inline void ToFloats(const M4& m, float f[16], bool rowMajor) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) (rowMajor ? f[r * 4 + c] : f[c * 4 + r]) = static_cast<float>(m.a[r][c]);
}
// The camera identity of R32 4.2: is a (the context's VP now) the camera b (DCS's gViewProj of the view's cockpit
// G-buffer pass in this frame)? 1: the same bytes (relative 1e-6), 2: the same camera centre (1 mm) and axes with
// another projection (a clip jitter x / y += j * w, another near / far), 0: another camera. A lateral eye move
// changes the centre; a rotation changes the 3x3 part beyond what a jitter can explain.
inline int CamSame(const M4& a, const M4& b) {
  double sc = 0, dev = 0;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      if (!std::isfinite(a.a[r][c]) || !std::isfinite(b.a[r][c])) return 0;
      sc = std::max(sc, std::fabs(b.a[r][c]));
      dev = std::max(dev, std::fabs(a.a[r][c] - b.a[r][c]));
    }
  if (!(sc > 0)) return 0;
  if (dev <= 1e-6 * sc) return 1;
  double ca[3], cb[3];
  if (!Centre(a, ca) || !Centre(b, cb)) return 0;
  double d2 = 0, n2 = 0;
  for (int i = 0; i < 3; ++i) d2 += (ca[i] - cb[i]) * (ca[i] - cb[i]), n2 += cb[i] * cb[i];
  if (!(std::sqrt(d2) <= 1e-3 + 1e-6 * std::sqrt(n2))) return 0;
  // The 3x3 (world axes) part: column 3 the same direction and length (w = depth), columns 0..2 changed only along
  // column 3 (jitter) or within span(column 2, column 3) (depth mapping).
  double F[3], f2 = 0, cs = 0;
  for (int i = 0; i < 3; ++i) F[i] = b.a[i][3], f2 += F[i] * F[i];
  for (int i = 0; i < 3; ++i)
    for (int c = 0; c < 4; ++c) cs = std::max(cs, std::fabs(b.a[i][c]));
  if (!(f2 > 0) || !(cs > 0)) return 0;
  const double tol = 1e-5 * cs;
  for (int i = 0; i < 3; ++i)
    if (!(std::fabs(a.a[i][3] - F[i]) <= tol)) return 0;
  for (int c = 0; c < 2; ++c) {
    double d[3], s = 0;
    for (int i = 0; i < 3; ++i) d[i] = a.a[i][c] - b.a[i][c], s += d[i] * F[i];
    s /= f2;
    if (!(std::fabs(s) <= 0.05)) return 0;
    for (int i = 0; i < 3; ++i)
      if (!(std::fabs(d[i] - s * F[i]) <= tol)) return 0;
  }
  // a2 = alpha * b2 + beta * F (least squares on the two vectors, then the residual).
  double B2[3], g11 = 0, g12 = 0, g22 = 0, r1 = 0, r2 = 0;
  for (int i = 0; i < 3; ++i) {
    B2[i] = b.a[i][2];
    g11 += B2[i] * B2[i], g12 += B2[i] * F[i], g22 += F[i] * F[i];
    r1 += a.a[i][2] * B2[i], r2 += a.a[i][2] * F[i];
  }
  const double det = g11 * g22 - g12 * g12;
  if (std::fabs(det) > 1e-18 * (g11 * g22 + 1e-300)) {
    const double al = (r1 * g22 - r2 * g12) / det, be = (r2 * g11 - r1 * g12) / det;
    for (int i = 0; i < 3; ++i)
      if (!(std::fabs(a.a[i][2] - al * B2[i] - be * F[i]) <= tol)) return 0;
  }
  return 2;
}

// Record keys: the element identity of a record (its two object pointers, +0xc9, +0xc8) and the ordinal among
// records with exactly that identity in the replay. Never 0.
inline uint64_t Mix64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}
inline uint64_t ElemBase(uint64_t obj0, uint64_t obj8, uint8_t c9, uint8_t c8) {
  return Mix64(obj0 ^ Mix64(obj8 ^ Mix64((static_cast<uint64_t>(c9) << 8 | c8) + 0x5bd1e995ull)));
}
inline uint64_t RecKey(uint64_t base, uint32_t ord) { return Mix64(base + (ord + 1) * 0x9e3779b97f4a7c15ull) | 1; }

// One record of DCS's array, read under SEH. ok false: not readable.
struct RecId {
  bool ok = false, state = false;
  uint8_t c8 = 0, c9 = 0;
  uint64_t obj0 = 0, obj8 = 0;
};
inline RecId ReadRec(const uint8_t* rec) {
  RecId r;
  uint64_t o[2];
  uint8_t c[2];
  if (!rec || !allocslab::ReadBytes(rec, o, sizeof(o)) || !allocslab::ReadBytes(rec + kRecC8, c, 2)) return r;
  r.ok = true;
  r.obj0 = o[0], r.obj8 = o[1];
  r.state = !o[0] && !o[1];
  r.c8 = c[0], r.c9 = c[1];
  return r;
}
// The ordinals of one replay (per element identity). Pure; the render thread owns one.
struct OrdTab {
  static constexpr int kN = 1024;
  uint64_t key[kN] = {};
  uint16_t n[kN] = {};
  uint32_t gen[kN] = {};
  uint32_t cur = 0;
  void Begin() {
    if (++cur == 0) {
      memset(gen, 0, sizeof(gen));
      cur = 1;
    }
  }
  uint32_t Next(uint64_t base) {
    for (uint32_t i = 0, h = static_cast<uint32_t>(Mix64(base)) & (kN - 1); i < kN; ++i) {
      const uint32_t s = (h + i) & (kN - 1);
      if (gen[s] != cur) {
        gen[s] = cur;
        key[s] = base;
        n[s] = 1;
        return 0;
      }
      if (key[s] == base) return n[s] == 0xffff ? 0xffffu : n[s]++;
    }
    return 0xffffu;  // more than kN identities in one replay: the rest share one ordinal per identity
  }
};

// ---------------------------------------------------------------------------
// The record-class learner (R32 4.3). Pure: the render thread feeds it.
// ---------------------------------------------------------------------------

enum : uint8_t { kClsUnknown = 0, kClsB = 1, kClsT = 2 };
constexpr int kLearnCommit = 16;     // agreeing measured frames that commit a class
constexpr int kLearnWindow = 300;    // frames for one forgetting sweep over the whole table
constexpr int kLearnExpire = 600;    // a key not seen for this many frames is forgotten (frozen keys too)
constexpr int kLearnMixedMin = 2;    // both outcomes this often before a commit: frozen at B
constexpr double kLearnTol = 3e-3;   // metres (DCS units)
constexpr int kLearnKeys = 2048;
constexpr int kLearnSweep = (kLearnKeys + kLearnWindow - 1) / kLearnWindow;  // slots checked per frame (7)
struct KeyRec {
  uint64_t key = 0;
  bool used = false, frozen = false, failFrozen = false;  // failFrozen: frozen by a failure that named this key
  uint8_t cls = kClsUnknown, cand = kClsUnknown, c9 = 0, c8 = 0;
  int streak = 0;
  uint32_t totB = 0, totT = 0;
  uint64_t lastSeen = 0, lastMeasured = 0;
  uint64_t touch = 0;  // frame + 1 of its entry in the touched list (sampled in that frame)
  uint64_t sTag[2][2] = {{~0ull, ~0ull}, {~0ull, ~0ull}};  // [pair (views 0/1, 2/3)][eye]: frame of the sample
  double ow[2][2][3] = {}, oe[2][2][3] = {};  // the model origin (world) and its offset from the eye
};
// R32 r3 (cost): the frame end walks only the keys sampled in that frame (the touched list), and the forgetting
// is a sweep of kLearnSweep slots per frame with backward-shift deletion (linear probing stays valid without
// tombstones); the class counts are kept as the classes change. No pass over the whole 2048 x KeyRec table and no
// rebuild in a normal frame (was: two full walks per frame and a 1.1 MB table rebuild every 300 frames).
struct Learner {
  KeyRec k[kLearnKeys];
  uint16_t touched[kLearnKeys] = {};
  int nTouched = 0;
  uint64_t touchedFrame = ~0ull;
  int used = 0, keysT = 0, keysB = 0;
  uint32_t sweep = 0;
  uint64_t commits = 0, flips = 0, mixed = 0, contradictions = 0, noise = 0, measB = 0, measT = 0, full = 0,
           samples = 0, expired = 0, resets = 0, keyFails = 0;
  // The table cleared (a mission / slot event, a scene cut, a reinstall); the counters are kept (statistics).
  void Reset() {
    for (KeyRec& e : k) e = KeyRec{};
    used = keysT = keysB = 0;
    nTouched = 0;
    touchedFrame = ~0ull;
    sweep = 0;
    ++resets;
  }
  static uint32_t Hash(uint64_t key) { return static_cast<uint32_t>((key * 0x9e3779b97f4a7c15ull) >> 53); }  // 11 bits
  // 0 none, 1 B (committed B or frozen: DCS's camera), 2 committed T.
  static int CountCls(const KeyRec& e) { return e.frozen || e.cls == kClsB ? 1 : e.cls == kClsT ? 2 : 0; }
  void Count(const KeyRec& e, int d) {
    const int c = CountCls(e);
    if (c == 1) keysB += d;
    if (c == 2) keysT += d;
  }
  KeyRec* Find(uint64_t key, bool add) {
    for (uint32_t i = 0, h = Hash(key); i < kLearnKeys; ++i) {
      KeyRec& e = k[(h + i) % kLearnKeys];
      if (e.used && e.key == key) return &e;
      if (!e.used) {
        if (!add) return nullptr;
        if (used >= kLearnKeys * 3 / 4) {
          ++full;
          return nullptr;
        }
        e = KeyRec{};
        e.used = true;
        e.key = key;
        ++used;
        return &e;
      }
    }
    return nullptr;
  }
  const KeyRec* FindC(uint64_t key) const { return const_cast<Learner*>(this)->Find(key, false); }
  // The committed class (a frozen key: B, DCS's camera; a key not in the table: unknown, also DCS's camera).
  uint8_t ClassOf(uint64_t key) const {
    const KeyRec* e = FindC(key);
    if (!e || e->frozen) return e ? kClsB : kClsUnknown;
    return e->cls;
  }
  // A record seen in a replay (any view): keeps the key alive.
  void Seen(uint64_t key, uint64_t frame, uint8_t c9 = 0, uint8_t c8 = 0) {
    if (KeyRec* e = Find(key, true)) {
      e->lastSeen = frame + 1;
      e->c9 = c9, e->c8 = c8;
    }
  }
  // The key's record origin in world space and relative to the eye (as DCS built it): pair 0 (views 0 / 1) or 1
  // (views 2 / 3), eye 0 or 1.
  void Sample(int pair, int eye, uint64_t key, uint64_t frame, const double ow[3], const double oe[3]) {
    if (pair < 0 || pair > 1 || eye < 0 || eye > 1) return;
    KeyRec* e = Find(key, true);
    if (!e || e->sTag[pair][eye] == frame) return;  // the first sample of the frame
    e->sTag[pair][eye] = frame;
    for (int i = 0; i < 3; ++i) e->ow[pair][eye][i] = ow[i], e->oe[pair][eye][i] = oe[i];
    ++samples;
    if (touchedFrame != frame) {  // the list belongs to one frame (a frame whose end never came is dropped)
      touchedFrame = frame;
      nTouched = 0;
    }
    if (e->touch != frame + 1 && nTouched < kLearnKeys) {
      e->touch = frame + 1;
      touched[nTouched++] = static_cast<uint16_t>(e - k);
    }
  }
  // Pure: the outcome of one frame's pair of samples.
  static uint8_t Outcome(const double ow[2][3], const double oe[2][3]) {
    double dw = 0, de = 0;
    for (int i = 0; i < 3; ++i) {
      dw += (ow[0][i] - ow[1][i]) * (ow[0][i] - ow[1][i]);
      de += (oe[0][i] - oe[1][i]) * (oe[0][i] - oe[1][i]);
    }
    dw = std::sqrt(dw), de = std::sqrt(de);
    const bool eyeRel = de < kLearnTol, fixed = dw < kLearnTol;
    if (eyeRel == fixed) return kClsUnknown;  // both (eyes not apart) or neither: noise
    return eyeRel ? kClsB : kClsT;
  }
  static void Event(std::string* events, const KeyRec& e, const char* what) {
    if (!events) return;
    char b[128];
    snprintf(b, sizeof(b), " %u/%u/%08x=%s", e.c9, e.c8, static_cast<unsigned>(e.key >> 32), what);
    *events += b;
  }
  void Freeze(KeyRec& e, int* changes, std::string* events, bool contradiction) {
    Count(e, -1);
    e.frozen = true;
    if (e.cls != kClsB && changes) ++*changes;
    e.cls = kClsB;
    Count(e, +1);
    ++mixed;
    if (contradiction) ++contradictions;
    char w[64];
    snprintf(w, sizeof(w), "mixed(B%u,T%u%s)", e.totB, e.totT, contradiction ? ",after commit" : "");
    Event(events, e, w);
  }
  // A failure that names this key (render thread, at once): frozen at B. False when the key cannot carry it (not
  // in the table and no room, or frozen by such a failure already: the failure is not the key's).
  bool FailKey(uint64_t key, std::string* events = nullptr) {
    KeyRec* e = Find(key, true);
    if (!e || e->failFrozen) return false;
    Count(*e, -1);
    e->frozen = e->failFrozen = true;
    e->cls = kClsB;
    Count(*e, +1);
    ++keyFails;
    Event(events, *e, "failed");
    return true;
  }
  // The frame's samples: hysteresis, commits, mixed keys, forgetting. Returns the number of class changes;
  // *contra: committed keys contradicted in this frame (each drew one record at the wrong camera).
  int EndFrame(uint64_t frame, std::string* events = nullptr, int* contra = nullptr) {
    int changes = 0, nContra = 0;
    const int n = touchedFrame == frame ? nTouched : 0;
    for (int i = 0; i < n; ++i) {
      KeyRec& e = k[touched[i]];
      if (!e.used || e.touch != frame + 1) continue;
      bool sawB = false, sawT = false;
      for (int p = 0; p < 2; ++p) {
        if (e.sTag[p][0] != frame || e.sTag[p][1] != frame) continue;
        const uint8_t o = Outcome(e.ow[p], e.oe[p]);
        if (o == kClsUnknown) {
          ++noise;
          continue;
        }
        (o == kClsB ? measB : measT)++;
        (o == kClsB ? sawB : sawT) = true;
      }
      if (!sawB && !sawT) continue;
      e.lastMeasured = frame + 1;
      e.totB += sawB, e.totT += sawT;
      if (e.frozen) continue;
      const bool committed = e.cls != kClsUnknown;
      if (committed && ((e.cls == kClsB && sawT) || (e.cls == kClsT && sawB))) {
        Freeze(e, &changes, events, true);
        ++nContra;
        continue;
      }
      if (!committed && std::min(e.totB, e.totT) >= static_cast<uint32_t>(kLearnMixedMin)) {
        Freeze(e, &changes, events, false);
        continue;
      }
      if (sawB && sawT) continue;  // (committed keys never get here with both)
      const uint8_t o = sawB ? kClsB : kClsT;
      if (o == e.cand) {
        ++e.streak;
      } else {
        e.cand = o;
        e.streak = 1;
      }
      if (e.streak >= kLearnCommit && e.cls != e.cand) {
        if (e.cls != kClsUnknown) ++flips;
        Count(e, -1);
        e.cls = e.cand;
        Count(e, +1);
        ++commits;
        ++changes;
        Event(events, e, e.cls == kClsT ? "T" : "B");
      }
    }
    nTouched = 0;
    touchedFrame = ~0ull;
    Forget(frame);
    if (contra) *contra = nContra;
    return changes;
  }
  // The slot i emptied, the probe chain behind it shifted back (linear probing without tombstones).
  void Erase(uint32_t i) {
    uint32_t j = i;
    for (;;) {
      j = (j + 1) % kLearnKeys;
      if (!k[j].used) break;
      const uint32_t h = Hash(k[j].key);
      const bool stays = i <= j ? (i < h && h <= j) : (i < h || h <= j);  // its home cyclically in (i, j]
      if (stays) continue;
      k[i] = k[j];
      i = j;
    }
    k[i] = KeyRec{};
    --used;
  }
  // Keys not seen for kLearnExpire frames are dropped, frozen ones too: kLearnSweep slots per frame (the whole
  // table every kLearnWindow frames, so a key goes within kLearnExpire + kLearnWindow frames of its last sight).
  void Forget(uint64_t frame) {
    for (int adv = 0, steps = 0; adv < kLearnSweep && steps < 4 * kLearnSweep; ++steps) {
      KeyRec& e = k[sweep];
      if (e.used && e.lastSeen + kLearnExpire < frame + 1) {
        Count(e, -1);
        Erase(sweep);
        ++expired;
        continue;  // the slot may hold a shifted key now: checked again
      }
      sweep = (sweep + 1) % kLearnKeys;
      ++adv;
    }
  }
};

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct Stats {
  // Replays: seen, moved; not moved by rule: not an HMD view / outside OverlayHDR/LDR, the frame's cockpit off (at
  // its first replay), the view's own cockpit not moved, option / overlays off or inert (latched per frame), a
  // learner reset pending, the frame stop (a stop earlier in this frame or within the last 30), a later replay of a
  // view decided not moved this frame (latched), no per-record point and not every record of the frame's views
  // committed T, observe, another thread; failures (the inert window): camera identity (ovlCpuCam), a later
  // replay of a moved view that cannot be moved (ovlCpuSplit), currentContext changed or rewritten with another
  // camera (ctxForeign), a filter call not aligned with DCS's record array (recAlign), an exception through DCS's
  // filter (filterUnwind); a frame stop after views of that frame were drawn moved (ovlCpuPairLate); a stop within
  // 300 frames of the previous one (recurInert).
  std::atomic<uint64_t> replays{0}, moved{0}, natNotHmd{0}, natOutside{0}, natCockpit{0}, natView{0}, natOff{0},
      natReset{0}, natPair{0}, natLatched{0}, natFilter{0}, natObserve{0}, natThread{0}, natPass{0}, failCam{0},
      failSplit{0}, ctxForeign{0}, pairLate{0}, camJitter{0}, recAlign{0}, filterUnwind{0}, recurInert{0};
  // Frame stops: by a failure, by a rule (a view that cannot be moved).
  std::atomic<uint64_t> stopFail{0}, stopRule{0};
  // DCS's view index against the HMD ordinal: a mapping seen once per HMD view, later replays that differ from it.
  std::atomic<uint64_t> viewMapDiff{0};
  // Records: passed the filter, written VP_true / DCS's, skipped (filter false), state records; replays with a
  // null / unknown filter (no composite), whole-frame scans (no per-record point).
  std::atomic<uint64_t> recs{0}, recTrue{0}, recDcs{0}, recSkip{0}, recState{0}, nullFilter{0}, unknownFilter{0},
      scanned{0};
  // The render point: replays decided per record through it; render calls in such a replay, of them matched to
  // DCS's record (the rest: recAlign); render calls passed through (another caller, no replay of ours).
  std::atomic<uint64_t> viaRender{0}, renderCalls{0}, renderRecs{0}, renderOther{0};
  // Replays whose context VP was the view's camera only in cPerView's layout (not DCS 2.9.30's row-major).
  std::atomic<uint64_t> ctxColMajor{0};
  // The context bytes at the replay's return: put back, not ours any more (DCS wrote its camera: left); DCS re-set
  // its own (same) camera inside a replay (continued); DCS's bytes put back at an abort, into a pushed copy of ours.
  std::atomic<uint64_t> restored{0}, restoreMismatch{0}, ctxReset{0}, abortPutBack{0}, copyPutBack{0};
  // The observer: tagged effect uploads examined, with an MVP, the world matrix found; class T records whose MVP
  // is still W * VP_dcs (cpu miss), class B / unknown records drawn with VP_true (wrong B), unverified (no W); of
  // the misses / wrong Bs: the key frozen at DCS's eye (the key's failure), not the key's (a failure of the view).
  std::atomic<uint64_t> obsUploads{0}, obsMvp{0}, obsWorld{0}, cpuOk{0}, cpuMiss{0}, cpuWrongB{0}, cpuUnverified{0},
      keyFail{0}, keyFailView{0};
  // Learner: samples, commits, flips, mixed keys (of them contradictions after a commit), noise, keys known, forgotten,
  // table full; resets (mission / slot events, scene cuts, installs).
  std::atomic<uint64_t> lSamples{0}, lCommits{0}, lFlips{0}, lMixed{0}, lContra{0}, lNoise{0}, lKeys{0}, lKeysT{0},
      lKeysB{0}, lExpired{0}, lFull{0}, lResets{0}, lResetMission{0}, lResetCut{0}, lResetInstall{0};
  // Install: signature failures (0 or 1 per load), uninstalls whose replay outlived the drain (hooks kept,
  // pass-through), partial installs rolled back, reinstalls refused while that replay still ran.
  std::atomic<uint64_t> sigFail{0}, busyAtStop{0}, installRollback{0}, installBusy{0};
};
Stats g_os;

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

using ReplayFn = void (*)(void* self, int view, void* ctx, void* filter);
using FilterFn = bool (*)(void* self, uint8_t* c8, uint8_t* c9);
using RenderFn = void (*)(void* ro, uint32_t pass);
ReplayFn g_replayOrig = nullptr;
FilterFn g_visOrig = nullptr, g_compOrig = nullptr;
RenderFn g_renderOrig = nullptr;
void** g_slotRender = nullptr;       // CockpitBase's import slot of RenderObject::render (null: no render point)
const void* g_renderRet = nullptr;   // the record loop's return address (null: tests, any caller in a replay)
void** g_slotReplay = nullptr;
void** g_slotVis = nullptr;
void** g_slotComp = nullptr;
uint8_t** g_curCtx = nullptr;          // &ContextBase::currentContext
const void* g_visVt = nullptr;         // the Visualizer filter callable's vtable (a per-record point)
// 0 not installed (modules not loaded yet), 1 installed, -1 mismatch / failed, 2 hooks kept in the slots but
// passing through (an uninstall whose replay outlived the drain: nothing reset under it; Install leaves it).
enum : int { kStateNone = 0, kStateOn = 1, kStatePass = 2 };
std::atomic<int> g_state{0};
std::atomic<int> g_hookBusy{0};        // any hook of ours running
std::atomic<int> g_replayBusy{0};      // a replay wrapper running (the uninstall drains it before the filters)
std::atomic<int> g_replayOpen{0};      // a replay wrapper whose replay record (t_rp) is open
std::atomic<bool> g_passThrough{false};  // the hooks only call DCS's (the uninstall, kStatePass)
bool g_installedViaLoader = false;      // HookSlot / UnhookSlot (tests: their own fake slots)
Learner g_learn;                        // render thread
std::string g_learnEvents;              // render thread: commits not yet traced
OrdTab g_ord;                           // render thread: the open replay's ordinals
OrdTab g_ordScan;                       // render thread: a whole-replay scan's ordinals
// Tests: called right after each slot write of InstallAt's test path (another thread calling the slot at once);
// a slot number (1 the composite filter, 2 the Visualizer filter, 3 the replay) whose hook fails.
void (*g_testAfterSlotWrite)(void** slot) = nullptr;
int g_testFailSlot = 0;

// Resets asked for by other threads (config thread: an install; the Lua thread: mission events), done by the
// render thread at a frame end with no replay open (ConsumeResets). The frame in which one is pending moves
// nothing (latched at its first replay).
enum : int { kResetLearner = 1, kResetRender = 2 };
std::atomic<int> g_resetReq{0};
uint64_t g_lastCuts = 0;  // scene_cut.h's cut count at the last reset check (render thread)
// Any thread: the learner's table (and with kResetRender the render state) cleared at the next frame end.
inline void RequestReset(int what) { g_resetReq.fetch_or(what, std::memory_order_acq_rel); }
// main.cpp's OnMissionEvent (Lua thread): a mission loads, the player changes slot, the simulation stops. The
// record keys name heap objects that are about to be freed and reused: no learned class may outlive them.
inline void OnMissionEvent() {
  g_os.lResetMission++;
  RequestReset(kResetLearner);
}
// Only scene_cut.h's gap cuts (no quad frame for more than 1 s: a mission load or a render-graph rebuild, when the
// cockpit's heap objects can be freed and reused) clear the table. A jump or turn cut (an F1 / F2 switch, a
// respawn's camera move, or a false positive: live 2026-10-14, F-14A on the carrier and A-10C in flight, a cut
// every ~2 s from frame hitches of 120 ms) frees nothing: clearing there put every cockpit display at DCS's camera
// for the 16-frame relearn (a jump of h against its bezel at each cut) while no key had changed. A key whose
// pointers do get reused for another element is caught by the learner's own contradiction (frozen at B).
inline uint64_t SceneCuts() { return scenecut::g_cutsByReason[scenecut::kGap].load(std::memory_order_relaxed); }
inline bool ResetPending() {
  return g_resetReq.load(std::memory_order_acquire) != 0 || SceneCuts() != g_lastCuts;
}

// The overlay pass open on the render thread (fixation_depth.h's boundary callback).
bool t_ovlOpen = false, t_ovlLdr = false, t_ovlGui = false;
int t_ovlView = -1;

// R95: the symbology export (cockpit_mask.h's symbology.h) learns each record's indicator type (+0xc9) before DCS
// draws it, in the OverlayHDRPass of the HMD views whose bit is set here (it sets and clears them on the render
// thread), and the replay's end. Set once at the export's install; nothing else of this file depends on them.
std::atomic<uint32_t> g_symViews{0};
void (*g_symRec)(int view, int c9) = nullptr;
void (*g_symReplayEnd)(int view) = nullptr;
std::atomic<uint64_t> g_symFaults{0};
// R115: the export's fault latch (symbology.h's sym::g_fault refers to it): set by the SEH handlers below, cleared at
// the export's next install (sym::NoteInstall). While set, sym::Ready() is false: no view bit, no record, no E.
std::atomic<bool> g_symFault{false};
inline bool SymViewNow(const void* ra, bool recordLoop) {
  const uint32_t m = g_symViews.load(std::memory_order_relaxed);
  return m && t_ovlOpen && !t_ovlLdr && t_ovlView >= 0 && t_ovlView < 4 && (m & (1u << t_ovlView)) &&
         (!recordLoop || !g_renderRet || ra == g_renderRet) &&
         GetCurrentThreadId() == ptiming::g_topTid.load(std::memory_order_relaxed);
}

// One replay (render thread).
struct Replay {
  bool open = false, moved = false, wasMoved = false, perRecord = false, abort = false, sampleOk = false;
  bool viaRender = false;  // the per-record point is the render hook (no filter point in this replay)
  bool lost = false;  // no later filter call of this replay can be matched to DCS's record (alignment lost)
  DWORD tid = 0;
  int t = -1, depth = 0;
  uint8_t* ctx = nullptr;  // the ContextBase whose bytes were saved
  float saved[16] = {}, vpTrue[16] = {}, last[16] = {};
  bool rowMajor = true;  // the layout of the context's VP bytes (LoadCtxVp)
  M4 vpDcs{}, vpTrueM{}, vpInv{};  // DCS's VP, VP_true, VP_dcs^-1
  double vm = 0, dw[3] = {}, C[3] = {};  // C: DCS's camera centre
  const uint8_t* recFirst = nullptr;  // DCS's records of this view (the range 0x3c5bd0 walks)
  uint32_t recN = 0, recIdx = 0;      // their count, the next record's index (the depth-0 filter calls so far)
  bool recOk = false;                 // the handler's range read (a view without records included)
};
Replay t_rp;
// The record drawn now (tags the observer's uploads).
bool t_tagValid = false, t_tagTrue = false;
uint64_t t_tagKey = 0;

// Per view and frame: the latched decision. 0 none, 1 moved, 2 not moved (rule), 3 failed.
struct Latch {
  uint64_t frame = ~0ull;
  int state = 0;
};
Latch g_latch[4];
// The frame decision (R32 r3: one for all four views). Latched at the frame's first replay: the overlays on and
// not inert, the cockpit's frame-wide off, a pending reset, the 30-frame block of an earlier stop. A stop (a
// failure, or an HMD view that cannot be moved) blocks the rest of the frame and the next kPairRecover frames; a
// stop within kPairRecur frames of the previous one makes the overlays inert (a stop that changed no image, a rule
// stop before any view of the frame moved, does not count toward that).
constexpr int kPairRecover = 30;
constexpr uint64_t kPairRecur = 300;
struct FrameDec {
  uint64_t frame = ~0ull;
  bool ovlOn = false, offAtBegin = false, reset = false, blocked = false;
  bool anyMoved = false, fail = false, rule = false;
};
FrameDec g_fd;
int g_frameBlock = 0;              // frames still blocked after the last stop
uint64_t g_lastStop = ~0ull;       // the frame of the last stop that counts toward the recurrence
// DCS's view index seen with each HMD ordinal (the R32 4.1 cross-check; the whole-frame scan's views).
int g_viewOf[4] = {-1, -1, -1, -1};
// The whole-frame scan of a replay without a per-record point (one per frame and handler).
struct ScanCache {
  uint64_t frame = ~0ull;
  const void* handler = nullptr;
  uint32_t views = 0;  // DCS view indices scanned (bit mask)
  bool allT = false;
};
ScanCache g_scan;
// Per-world-matrix cache of the observer (resource -> offset / order of W).
struct WCache {
  ID3D11Resource* res = nullptr;
  UINT off = 0;
  int order = 0;
  bool have = false;
};
WCache g_wCache[64];

inline bool Installed() { return g_state.load(std::memory_order_relaxed) == kStateOn; }

// Render thread only, with no replay open (ConsumeResets at a frame end; tests).
void ResetRender() {
  t_rp = Replay{};
  t_tagValid = t_tagTrue = false;
  for (Latch& l : g_latch) l = Latch{};
  g_fd = FrameDec{};
  g_frameBlock = 0;
  g_lastStop = ~0ull;
  for (int& v : g_viewOf) v = -1;
  g_scan = ScanCache{};
  for (WCache& w : g_wCache) w = WCache{};
  t_ovlOpen = t_ovlLdr = t_ovlGui = false;
  t_ovlView = -1;
}

// The pending resets, at the frame end (render thread). Never with a replay open (its record stays valid): kept
// for the next frame end then.
void ConsumeResets() {
  int req = g_resetReq.exchange(0, std::memory_order_acq_rel);
  const uint64_t cuts = SceneCuts();
  if (cuts != g_lastCuts) {
    g_lastCuts = cuts;
    req |= kResetLearner;
    g_os.lResetCut++;
  }
  if (!req) return;
  if (t_rp.open) {
    g_resetReq.fetch_or(req, std::memory_order_acq_rel);
    return;
  }
  if (req & kResetLearner) {
    g_learn.Reset();
    g_learnEvents.clear();
    g_os.lResets++;
  }
  if (req & kResetRender) ResetRender();
}

// ---------------------------------------------------------------------------
// The per-frame / per-view decision (R32 4.2, r3)
// ---------------------------------------------------------------------------

enum Why : int {
  kMoved = 0, kNatNotHmd, kNatOutside, kNatCockpit, kNatView, kNatOff, kNatReset, kNatPair, kNatFilter, kNatObserve,
  kNatLatched, kFailCam, kFailSplit
};
inline bool IsFail(int w) { return w >= kFailCam; }

// The frame's latched state at its first replay on the render thread.
void FrameBegin() {
  if (g_fd.frame == g_frame) return;
  g_fd = FrameDec{};
  g_fd.frame = g_frame;
  g_fd.ovlOn = g_cfgOverlays.load(std::memory_order_relaxed) && !g_ovlInert.load(std::memory_order_relaxed);
  g_fd.offAtBegin = g_f.off;
  g_fd.reset = ResetPending();
  g_fd.blocked = g_frameBlock > 0;
}

// The frame stop: no view moved for the rest of this frame (and, at the frame end, the next kPairRecover frames).
// Once per frame. Late: views of this frame were drawn moved already (they differ from the rest in this frame).
void FrameStop(int t, bool fail) {
  FrameBegin();
  const bool first = !g_fd.fail && !g_fd.rule;
  g_fd.blocked = true;
  (fail ? g_fd.fail : g_fd.rule) = true;
  if (!first) return;
  (fail ? g_os.stopFail : g_os.stopRule)++;
  const bool late = g_fd.anyMoved;
  if (late) g_os.pairLate++;
  if (!fail && !late) return;  // every view of the frame alike, nothing drawn moved: no recurrence
  const uint64_t prev = g_lastStop;
  g_lastStop = g_frame;
  if (prev != ~0ull && g_frame > prev && g_frame - prev <= kPairRecur && !g_ovlInert.load()) {
    g_ovlInert = true;
    g_os.recurInert++;
    Log("cockpit ipd overlays (cpu): view %d stopped the frame again %llu frames after the previous stop; overlays "
        "at DCS's eye until reload (the cockpit stays patched)",
        t, static_cast<unsigned long long>(g_frame - prev));
  }
}
// A failure of view t: the inert window and the frame stop.
void NoteFail(int t) {
  ++g_ovlFails;
  if (t >= 0 && t <= 3) ++g_f.v[t].cpuFails;  // the frame dump's CSV
  FrameStop(t, true);
}
// A failure of the open replay's view once (the latch says failed; later replays of the view stay native).
void FailReplay(Replay& rp) {
  if (rp.t < 0 || rp.t > 3) return;
  Latch& l = g_latch[rp.t];
  if (l.frame == g_frame && l.state == 3) return;
  l.frame = g_frame;
  l.state = 3;
  NoteFail(rp.t);
}

// One HMD view's replay: moved or why not. The frame-wide reasons first (every view alike), then the frame stop
// (before anything that could count a failure: a blocked frame checks nothing, R32 r3), then the view's own.
int Decide(int t, const M4& vpCtx, bool perRecord, bool scanAllT) {
  if (t < 0 || t > 3) return kNatNotHmd;
  if (!t_ovlOpen || t_ovlGui || t_ovlView != t) return kNatOutside;
  FrameBegin();
  const Frame& f = g_f;
  const ViewFrame& vf = f.v[t];
  // One decision per view and frame: a view not moved at its first replay stays so; one moved can only fail.
  const Latch& l = g_latch[t];
  const bool latched = l.frame == g_frame;
  if (latched && l.state != 1) return kNatLatched;
  if (g_fd.offAtBegin) return kNatCockpit;
  if (!g_fd.ovlOn) return kNatOff;
  if (g_fd.reset) return kNatReset;
  if (f.observe) return kNatObserve;
  // Nothing to move: the frame's shift is 0 (Depth boost at scale 1, or Enabled=0) and so is this view's. Every
  // view alike, as the cockpit's frame-wide off; no stop (was a rule stop every 31 frames at scale 1, whose
  // 30-frame block kept the displays at DCS's boosted eye for up to 30 frames after a ramp began while the
  // cockpit was already at the true eye: live 2026-10-14, F-16, 9 frames at 5 mm/s).
  if (f.h == 0 && !NonZero(vf.dw)) return latched ? kFailSplit : kNatCockpit;
  if (g_fd.blocked) return kNatPair;
  if (f.off || !vf.patched || vf.failed || !NonZero(vf.dw)) return latched ? kFailSplit : kNatView;
  // The camera identity: the bytes at +0x1b4 are this view's boosted camera of its cockpit G-buffer pass.
  const PrevStore& c = g_cur[t];
  const int cam = c.valid && c.tag == g_frame + 1 ? CamSame(vpCtx, c.vp) : 0;
  if (!cam) return latched ? kFailSplit : kFailCam;
  if (cam == 2) g_os.camJitter++;
  if (!perRecord && !scanAllT) return latched ? kFailSplit : kNatFilter;
  return kMoved;
}

// ---------------------------------------------------------------------------
// The hooks (render thread; another thread only passes through)
// ---------------------------------------------------------------------------

bool g_loggedForeign = false, g_loggedMismatch = false, g_loggedAlign = false, g_loggedView = false,
     g_loggedUnwind = false;

// Our bytes out of the context at an abort: DCS's back into the saved context if it still holds ours (and into the
// context now current when it is a copy of ours: a push that copied the saved one), so every later record of the
// replay is built with DCS's camera.
void PutBackSaved(Replay& rp) {
  if (!rp.ctx || !memcmp(rp.last, rp.saved, 64)) return;
  if (!memcmp(rp.ctx + kCtxVP, rp.last, 64)) {
    memcpy(rp.ctx + kCtxVP, rp.saved, 64);
    g_os.abortPutBack++;
  }
  uint8_t* cur = g_curCtx ? *g_curCtx : nullptr;
  if (cur && cur != rp.ctx && !memcmp(cur + kCtxVP, rp.last, 64)) {
    memcpy(cur + kCtxVP, rp.saved, 64);
    g_os.copyPutBack++;
  }
  memcpy(rp.last, rp.saved, 64);
}
// The rest of the replay keeps DCS's camera (what we wrote put back); the view's failure.
void AbortReplay(Replay& rp) {
  if (rp.abort) return;
  rp.abort = true;
  __try {
    PutBackSaved(rp);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fault = true;
  }
  if (rp.wasMoved) FailReplay(rp);
}

void ApplyRecord(Replay& rp, uint64_t key, uint8_t c9, uint8_t c8);
// Our part of a record's filter call (render thread, inside a replay, the outermost filter call only).
void OnRecord(bool pass, uint8_t c8, uint8_t c9) {
  CostScope cost(kCostOvlCpu);
  Replay& rp = t_rp;
  t_tagValid = t_tagTrue = false;
  // The record this call belongs to: DCS's next one (0x3c5bd0 calls the filter once per record, in order).
  const uint32_t idx = rp.recIdx++;
  if (rp.lost) return;  // alignment lost earlier in this replay (counted once): nothing tagged or written
  const RecId id = idx < rp.recN ? ReadRec(rp.recFirst + static_cast<size_t>(idx) * kRecSize) : RecId{};
  if (!id.ok || id.c8 != c8 || id.c9 != c9) {
    g_os.recAlign++;
    if (!g_loggedAlign) {
      g_loggedAlign = true;
      Log("cockpit ipd overlays (cpu): frame %llu view %d: filter call %u does not match DCS's record %u of %u "
          "(bytes %u/%u, record %u/%u); the rest of the replay keeps DCS's camera",
          static_cast<unsigned long long>(g_frame), rp.t, idx, idx, rp.recN, c9, c8, id.c9, id.c8);
    }
    rp.recN = 0;  // no further record is trusted
    rp.lost = true;
    if (rp.moved) AbortReplay(rp);
    return;
  }
  if (id.state) {
    g_os.recState++;
    return;
  }
  const uint64_t key = RecKey(ElemBase(id.obj0, id.obj8, c9, c8), g_ord.Next(ElemBase(id.obj0, id.obj8, c9, c8)));
  if (!pass) {
    g_os.recSkip++;
    return;
  }
  ApplyRecord(rp, key, c9, c8);
}

// A record that draws next (its key known): seen, tagged for the observer, and, in a moved replay, the camera of
// its class written into the context (the filter point and the render point alike).
void ApplyRecord(Replay& rp, uint64_t key, uint8_t c9, uint8_t c8) {
  g_os.recs++;
  if (rp.t >= 0 && rp.t <= 3) {
    g_learn.Seen(key, g_frame, c9, c8);
    ++g_f.v[rp.t].cpuRecs;
  }
  t_tagKey = key;
  t_tagValid = true;
  if (!rp.moved || rp.abort) return;
  // The context is still the one saved and still holds what we (or DCS) left there.
  uint8_t* cur = *g_curCtx;
  if (cur == rp.ctx && memcmp(cur + kCtxVP, rp.last, 64) != 0 && !memcmp(cur + kCtxVP, rp.saved, 64)) {
    memcpy(rp.last, rp.saved, 64);  // DCS set its own (the same) camera again: nothing of ours left, continue
    g_os.ctxReset++;
  }
  if (cur != rp.ctx || memcmp(cur + kCtxVP, rp.last, 64) != 0) {
    g_os.ctxForeign++;
    if (!g_loggedForeign) {
      g_loggedForeign = true;
      Log("cockpit ipd overlays (cpu): frame %llu view %d: currentContext %s inside the replay; DCS's camera put "
          "back, the rest of the replay keeps it",
          static_cast<unsigned long long>(g_frame), rp.t, cur != rp.ctx ? "changed" : "rewritten by DCS");
    }
    AbortReplay(rp);
    return;
  }
  const bool tr = g_learn.ClassOf(key) == kClsT;
  const float* want = tr ? rp.vpTrue : rp.saved;
  if (memcmp(rp.last, want, 64) != 0) {
    memcpy(cur + kCtxVP, want, 64);
    memcpy(rp.last, want, 64);
  }
  t_tagTrue = tr;
  (tr ? g_os.recTrue : g_os.recDcs)++;
  if (rp.t >= 0 && rp.t <= 3) ++(tr ? g_f.v[rp.t].cpuRecT : g_f.v[rp.t].cpuRecB);
}

// An exception went through DCS's filter (a C++ throw unwinding past our hook, R32 r3): the record it belonged to
// is unknown from here on, so nothing more of this replay is tagged or moved (DCS's camera put back).
void FilterUnwound() {
  Replay& rp = t_rp;
  t_tagValid = t_tagTrue = false;
  if (!rp.lost) {
    rp.lost = true;
    rp.recN = 0;
    g_os.filterUnwind++;
    if (!g_loggedUnwind) {
      g_loggedUnwind = true;
      Log("cockpit ipd overlays (cpu): frame %llu view %d: an exception went through DCS's record filter; the rest "
          "of the replay keeps DCS's camera",
          static_cast<unsigned long long>(g_frame), rp.t);
    }
  }
  if (rp.moved) AbortReplay(rp);
}
// DCS's filter inside a replay of ours: the nesting depth restored however the call leaves (no C++ object here).
bool CallFilterInReplay(FilterFn orig, void* self, uint8_t* c8, uint8_t* c9, int d) {
  bool r = false;
  __try {
    r = orig(self, c8, c9);
  } __finally {
    t_rp.depth = d;
    if (AbnormalTermination()) FilterUnwound();
  }
  return r;
}
bool FilterCommon(FilterFn orig, void* self, uint8_t* c8, uint8_t* c9) {
  if (!orig) return false;  // (never: the originals are set before the slots are written)
  Replay& rp = t_rp;
  if (!rp.open || rp.tid != GetCurrentThreadId()) return orig(self, c8, c9);
  const int d = rp.depth++;
  const bool r = CallFilterInReplay(orig, self, c8, c9, d);
  if (d == 0) {
    __try {
      OnRecord(r, c8 ? *c8 : 0, c9 ? *c9 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_fault = true;
      t_tagValid = t_tagTrue = false;
      rp.recN = 0;
      rp.lost = true;
      if (rp.moved) AbortReplay(rp);
    }
  }
  return r;
}
// The busy count is given back however DCS's filter leaves (a C++ exception included: __finally runs on unwind).
bool VisFilterHook(void* self, uint8_t* c8, uint8_t* c9) {
  g_hookBusy.fetch_add(1, std::memory_order_acq_rel);
  bool r = false;
  __try {
    r = FilterCommon(g_visOrig, self, c8, c9);
  } __finally {
    g_hookBusy.fetch_sub(1, std::memory_order_acq_rel);
  }
  return r;
}
bool CompFilterHook(void* self, uint8_t* c8, uint8_t* c9) {
  g_hookBusy.fetch_add(1, std::memory_order_acq_rel);
  bool r = false;
  __try {
    r = FilterCommon(g_compOrig, self, c8, c9);
  } __finally {
    g_hookBusy.fetch_sub(1, std::memory_order_acq_rel);
  }
  return r;
}

// The render point's record (render thread, inside a replay decided through it): the RenderObject names DCS's
// record exactly (+0x78 = record + 0x40 inside the view's range, at or after the next unseen record, and the
// object pointers equal the record's). The ordinals of the records between (skipped by DCS: nothing drawn) are
// counted as the filter point counts them (every non-state record of the replay, in order). True: tagged.
bool g_loggedRenderAlign = false;
bool OnRender(const uint8_t* ro) {
  CostScope cost(kCostOvlCpu);
  Replay& rp = t_rp;
  t_tagValid = t_tagTrue = false;
  if (rp.lost) return false;
  g_os.renderCalls++;
  const uint8_t* recp = nullptr;
  uint64_t o0 = 0, o8 = 0;
  const bool read = allocslab::ReadBytes(ro + kRoRec, &recp, sizeof(recp)) &&
                    allocslab::ReadBytes(ro + kRoObj0, &o0, sizeof(o0)) &&
                    allocslab::ReadBytes(ro + kRoObj8, &o8, sizeof(o8));
  const uint8_t* rec = read && recp ? recp - kRecFromRo : nullptr;
  const ptrdiff_t off = rec && rp.recFirst ? rec - rp.recFirst : -1;
  const uint32_t idx = off >= 0 && off % kRecSize == 0 ? static_cast<uint32_t>(off / kRecSize) : ~0u;
  RecId id;
  if (idx < rp.recN && idx >= rp.recIdx) id = ReadRec(rec);
  if (!id.ok || id.state || id.obj0 != o0 || id.obj8 != o8) {
    g_os.recAlign++;
    if (!g_loggedRenderAlign) {
      g_loggedRenderAlign = true;
      Log("cockpit ipd overlays (cpu): frame %llu view %d: a render call does not name DCS's next record (record "
          "%d of %u, next %u, pointers %s); the rest of the replay keeps DCS's camera",
          static_cast<unsigned long long>(g_frame), rp.t, idx == ~0u ? -1 : static_cast<int>(idx), rp.recN, rp.recIdx,
          id.ok && id.obj0 == o0 && id.obj8 == o8 ? "equal" : "differ");
    }
    rp.recN = 0;
    rp.lost = true;
    if (rp.moved) AbortReplay(rp);
    return false;
  }
  for (uint32_t j = rp.recIdx; j < idx; ++j) {  // records DCS did not draw: their ordinals still count
    const RecId s = ReadRec(rp.recFirst + static_cast<size_t>(j) * kRecSize);
    if (!s.ok) {
      rp.recN = 0;
      rp.lost = true;
      g_os.recAlign++;
      if (rp.moved) AbortReplay(rp);
      return false;
    }
    if (!s.state) g_ord.Next(ElemBase(s.obj0, s.obj8, s.c9, s.c8));
  }
  rp.recIdx = idx + 1;
  const uint64_t base = ElemBase(id.obj0, id.obj8, id.c9, id.c8);
  const uint64_t key = RecKey(base, g_ord.Next(base));
  g_os.renderRecs++;
  ApplyRecord(rp, key, id.c9, id.c8);
  return t_tagValid;
}
// R94: [Perception] SymbologyTrace (sym_trace.h; dev, measure only). In a traced frame every replay on the render
// thread reports its view's records (c9.c8 runs) and the records it draws (the render point) with the blend state of
// the first record of each type; nothing here changes what DCS draws.
void TraceReplay(void* self, int view) {
  const uint8_t* h = static_cast<const uint8_t*>(self);
  const uint8_t* recs = nullptr;
  const int32_t *vb = nullptr, *ve = nullptr;
  std::string s;
  uint32_t n = 0;
  if (h && allocslab::ReadBytes(h + kHRecs, &recs, sizeof(recs)) && allocslab::ReadBytes(h + kHViews, &vb, sizeof(vb)) &&
      allocslab::ReadBytes(h + kHViewsEnd, &ve, sizeof(ve)) && recs && vb && ve >= vb && view >= 0 && view < ve - vb) {
    int32_t packed = 0;
    if (allocslab::ReadBytes(vb + view, &packed, sizeof(packed))) {
      const uint32_t lo = static_cast<uint32_t>(packed) & 0xffffu, hi = static_cast<uint32_t>(packed) >> 16;
      int last = -1, run = 0;
      for (uint32_t i = lo; i < hi; ++i) {
        const RecId id = ReadRec(recs + static_cast<size_t>(i) * kRecSize);
        if (!id.ok) {
          s += "(unreadable) ";
          break;
        }
        ++n;
        symtr::RleAdd(s, last, run, id.state ? -2 : id.c9 * 256 + id.c8);
      }
      symtr::RleAdd(s, last, run, -1);
    } else {
      s = "(range unreadable)";
    }
  } else {
    s = "(handler unreadable)";
  }
  symtr::ReplayBegin(t_ovlOpen ? t_ovlView : -1, t_ovlOpen ? (t_ovlGui ? 'G' : t_ovlLdr ? 'L' : 'H') : '-', view, n,
                     s);
}
bool TraceReplaySeh(void* self, int view) {
  __try {
    TraceReplay(self, view);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
void TraceReplayEndSeh() {
  __try {
    symtr::ReplayEnd();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
// The record a render call draws (its RenderObject +0x78 = record + 0x40): c9, or -1 unreadable.
int TraceRenderPreSeh(const uint8_t* ro) {
  __try {
    const uint8_t* recp = nullptr;
    uint8_t c[2] = {};
    if (!allocslab::ReadBytes(ro + kRoRec, &recp, sizeof(recp)) || !recp ||
        !allocslab::ReadBytes(recp - kRecFromRo + kRecC8, c, 2))
      return -1;
    symtr::RenderPre(c[1], c[0]);
    return c[1];
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}
void TraceRenderPostSeh(int c9) {
  __try {
    symtr::RenderPost(c9);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
inline bool TraceThread() {
  return symtr::g_on.load(std::memory_order_relaxed) &&
         GetCurrentThreadId() == ptiming::g_topTid.load(std::memory_order_relaxed);
}

// R95: the record's indicator type (RenderObject +0x78 = record + 0x40; -1 unreadable) to the symbology export, before
// anything of this file runs for the record (its upload observer is not armed for our copy / dispatch calls yet).
void SymRecordSeh(int view, const uint8_t* ro) {
  __try {
    int c9 = -1;
    const uint8_t* recp = *reinterpret_cast<const uint8_t* const*>(ro + kRoRec);
    if (recp) c9 = recp[-static_cast<ptrdiff_t>(kRecFromRo) + kRecC9];
    g_symRec(view, c9);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_symFaults++;
    g_symFault = true;  // R115: latched until the export's next install (R95 cleared only this frame's bits)
    g_symViews = 0;  // (no record of this frame is reported any more)
  }
}
void SymReplayEndSeh(int view) {
  __try {
    g_symReplayEnd(view);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_symFaults++;
    g_symFault = true;  // R115
    g_symViews = 0;
  }
}

// CockpitBase's import of RenderObject::render. Only the record loop's call inside a replay of ours on this thread
// is ours; every other call passes through.
void RenderHook(void* ro, uint32_t pass) {
  void* const ra = _ReturnAddress();
  g_hookBusy.fetch_add(1, std::memory_order_acq_rel);
  if (g_symRec && ro && SymViewNow(ra, true)) SymRecordSeh(t_ovlView, static_cast<const uint8_t*>(ro));  // R95
  bool mine = false;
  const Replay& rp = t_rp;
  if (rp.open && rp.viaRender && rp.tid == GetCurrentThreadId() && (!g_renderRet || ra == g_renderRet) &&
      !g_passThrough.load(std::memory_order_acquire)) {
    __try {
      mine = OnRender(static_cast<const uint8_t*>(ro));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_fault = true;
      t_tagValid = t_tagTrue = false;
      t_rp.recN = 0;
      t_rp.lost = true;
      if (t_rp.moved) AbortReplay(t_rp);
    }
  } else {
    g_os.renderOther++;
  }
  // R94: SymbologyTrace (a traced frame's replay on the render thread; the record loop's call only).
  const int trC9 = TraceThread() && symtr::g_rep.open && (!g_renderRet || ra == g_renderRet)
                       ? TraceRenderPreSeh(static_cast<const uint8_t*>(ro))
                       : -1;
  __try {
    g_renderOrig(ro, pass);  // (never null: the original is set before the slot is written)
  } __finally {
    if (mine) t_tagValid = t_tagTrue = false;
    g_hookBusy.fetch_sub(1, std::memory_order_acq_rel);
  }
  if (trC9 >= 0) TraceRenderPostSeh(trC9);
}

// One record range without a per-record point: every drawable record has a committed T key (true for a range that
// draws nothing: moving it changes no image).
bool ScanAllT(const uint8_t* first, uint32_t n) {
  g_ordScan.Begin();
  for (uint32_t i = 0; i < n; ++i) {
    const RecId id = ReadRec(first + static_cast<size_t>(i) * kRecSize);
    if (!id.ok) return false;
    if (id.state) continue;
    const uint64_t base = ElemBase(id.obj0, id.obj8, id.c9, id.c8);
    if (g_learn.ClassOf(RecKey(base, g_ordScan.Next(base))) != kClsT) return false;
  }
  return true;
}
// The whole frame's answer for a replay without a per-record point (R32 r3): the ranges of every HMD view known so
// far (DCS's view index per HMD ordinal) and of this view, all committed T, or none of them moves. One scan per
// frame and handler (again when a view not scanned yet appears), so the views of a frame decide alike.
bool ScanFrame(const uint8_t* handler, const uint8_t* recs, const int32_t* vb, int nViews, int view) {
  uint32_t want = view >= 0 && view < 32 ? 1u << view : 0;
  for (int v : g_viewOf)
    if (v >= 0 && v < 32 && v < nViews) want |= 1u << v;
  if (g_scan.frame == g_frame && g_scan.handler == handler && (g_scan.views & want) == want) return g_scan.allT;
  g_os.scanned++;
  bool all = true;
  for (int v = 0; v < nViews && v < 32 && all; ++v) {
    if (!(want >> v & 1u)) continue;
    int32_t packed = 0;
    if (!allocslab::ReadBytes(vb + v, &packed, sizeof(packed))) {
      all = false;
      break;
    }
    const uint32_t lo = static_cast<uint32_t>(packed) & 0xffffu, hi = static_cast<uint32_t>(packed) >> 16;
    if (hi > lo) all = ScanAllT(recs + static_cast<size_t>(lo) * kRecSize, hi - lo);
  }
  g_scan.frame = g_frame;
  g_scan.handler = handler;
  g_scan.views = want;
  g_scan.allT = all;
  return all;
}

// Diagnostics (bounded log lines): the replay's caller and the camera identity against the view's G-buffer camera.
const void* t_replayRa = nullptr;  // the replay's return address (DCS's caller)
std::atomic<int> g_diagReplays{0}, g_diagCam{0};
uint64_t g_diagFrame = ~0ull;
int g_diagFrames = 0;
void FmtAddr(const void* a, char* out, size_t n) {
  HMODULE m = nullptr;
  wchar_t p[MAX_PATH] = {};
  if (a && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(a), &m) &&
      GetModuleFileNameW(m, p, MAX_PATH)) {
    const wchar_t* b = wcsrchr(p, L'\\');
    snprintf(out, n, "%ls+0x%llx", b ? b + 1 : p,
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(a) - reinterpret_cast<uintptr_t>(m)));
  } else {
    snprintf(out, n, "%p", a);
  }
}
void DiagReplay(int t, int view, int why, const Replay& rp, const void* filter, bool hasImpl, const void* vt,
                const void* composite) {
  // The first replays of the first frames (every view), and every camera failure (up to 12) with its matrices.
  const bool camFail = why == kFailCam;
  if (g_frame != g_diagFrame) {
    g_diagFrame = g_frame;
    ++g_diagFrames;
  }
  const bool early = g_diagFrames <= 3 && g_diagReplays.load() < 48;
  if (!early && !(camFail && g_diagCam.load() < 12)) return;
  if (early) g_diagReplays++;
  if (camFail) g_diagCam++;
  char ra[160], fv[160];
  FmtAddr(t_replayRa, ra, sizeof(ra));
  FmtAddr(vt, fv, sizeof(fv));
  const PrevStore* c = t >= 0 && t <= 3 ? &g_cur[t] : nullptr;
  const bool cv = c && c->valid, ctag = cv && c->tag == g_frame + 1;
  const int cam = ctag ? CamSame(rp.vpDcs, c->vp) : -1;
  double ca[3] = {}, cb[3] = {};
  const bool hca = Centre(rp.vpDcs, ca), hcb = cv && Centre(c->vp, cb);
  Log("cockpit ipd overlays (cpu) diag: frame %llu hmd %d DCS view %d %s why %d; caller %s; filter %s impl %d vt %s "
      "composite %d; records %u (ok %d); G-buffer camera %s (tag %llu, frame+1 %llu) CamSame %d; centre ctx %s "
      "%.4f %.4f %.4f gb %s %.4f %.4f %.4f; view patched %d failed %d dw %.2f %.2f %.2f mm",
      static_cast<unsigned long long>(g_frame), t, view, t_ovlLdr ? (t_ovlGui ? "GUI" : "LDR") : "HDR", why, ra,
      filter ? "set" : "null", hasImpl ? 1 : 0, vt ? fv : "-", composite ? 1 : 0, rp.recN, rp.recOk ? 1 : 0,
      cv ? "valid" : "none", static_cast<unsigned long long>(c ? c->tag : 0),
      static_cast<unsigned long long>(g_frame + 1), cam, hca ? "" : "(none)", ca[0], ca[1], ca[2],
      hcb ? "" : "(none)", cb[0], cb[1], cb[2], t >= 0 && t <= 3 ? g_f.v[t].patched : -1,
      t >= 0 && t <= 3 ? g_f.v[t].failed : -1, t >= 0 && t <= 3 ? g_f.v[t].dw[0] * 1e3 : 0.0,
      t >= 0 && t <= 3 ? g_f.v[t].dw[1] * 1e3 : 0.0, t >= 0 && t <= 3 ? g_f.v[t].dw[2] * 1e3 : 0.0);
  if (camFail || (early && cv && cam == 0)) {
    char a[300];
    FmtM(a, sizeof(a), rp.vpDcs);
    Log("cockpit ipd overlays (cpu) diag:   ctx VP %s", a);
    if (cv) {
      FmtM(a, sizeof(a), c->vp);
      Log("cockpit ipd overlays (cpu) diag:   gb  VP %s", a);
    }
  }
}

// The layout of the context's VP bytes for HMD view t: the one that is the view's G-buffer camera this frame
// (row-major, DCS 2.9.30's, when neither or both are; the camera identity then decides).
bool CtxRowMajor(int t, const uint8_t* vp) {
  if (t < 0 || t > 3) return true;
  const PrevStore& c = g_cur[t];
  if (!c.valid || c.tag != g_frame + 1 || CamSame(LoadCtxVp(vp, true), c.vp)) return true;
  if (!CamSame(LoadCtxVp(vp, false), c.vp)) return true;
  g_os.ctxColMajor++;
  return false;
}

// The replay begins: true when a replay record was opened (End must close it).
bool ReplayBegin(void* self, int view, void* filter) {
  if (!g_on.load(std::memory_order_relaxed) || !Active() || !PathCpu() || t_rp.open) return false;
  CostScope cost(kCostOvlCpu);
  g_os.replays++;
  if (g_passThrough.load(std::memory_order_acquire)) {
    g_os.natPass++;
    return false;
  }
  if (GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) {
    g_os.natThread++;
    return false;
  }
  FrameBegin();
  uint8_t* ctx = g_curCtx ? *g_curCtx : nullptr;
  if (!ctx) {
    g_os.ctxForeign++;
    return false;
  }
  Replay& rp = t_rp;
  rp = Replay{};
  rp.open = true;
  rp.tid = GetCurrentThreadId();
  rp.ctx = ctx;
  memcpy(rp.saved, ctx + kCtxVP, 64);
  memcpy(rp.last, rp.saved, 64);
  t_tagValid = t_tagTrue = false;
  g_ord.Begin();
  const int t = t_ovlOpen ? t_ovlView : -1;
  rp.t = t;
  // DCS's records of this view: handler+0x98[view] packed (lo16, hi16) into the array at handler+0x20.
  const uint8_t* h = static_cast<const uint8_t*>(self);
  const uint8_t* recs = nullptr;
  const int32_t *vb = nullptr, *ve = nullptr;
  void* composite = nullptr;
  if (h && allocslab::ReadBytes(h + kHRecs, &recs, sizeof(recs)) && allocslab::ReadBytes(h + kHViews, &vb, sizeof(vb)) &&
      allocslab::ReadBytes(h + kHViewsEnd, &ve, sizeof(ve)) &&
      allocslab::ReadBytes(h + kHComposite, &composite, sizeof(composite)) && recs && vb && ve >= vb && view >= 0 &&
      view < ve - vb) {
    int32_t packed = 0;
    if (allocslab::ReadBytes(vb + view, &packed, sizeof(packed))) {
      rp.recOk = true;
      const uint32_t lo = static_cast<uint32_t>(packed) & 0xffffu, hi = static_cast<uint32_t>(packed) >> 16;
      if (hi > lo) {
        rp.recFirst = recs + static_cast<size_t>(lo) * kRecSize;
        rp.recN = hi - lo;
      }
    }
  }
  // R32 4.1: DCS's view index against the HMD ordinal. The camera identity below is what guards the decision;
  // this map names the views a whole-frame scan covers (the first index seen per HMD view; later replays that
  // differ from it are counted).
  if (t >= 0 && t <= 3) {
    if (g_viewOf[t] < 0) {
      g_viewOf[t] = view;
    } else if (g_viewOf[t] != view) {
      g_os.viewMapDiff++;
      if (!g_loggedView) {
        g_loggedView = true;
        Log("cockpit ipd overlays (cpu): frame %llu: HMD view %d replays DCS view %d (earlier %d)",
            static_cast<unsigned long long>(g_frame), t, view, g_viewOf[t]);
      }
    }
  }
  // The per-record point: our Visualizer filter hook, or DCS's composite lambda (built around any passed filter
  // when handler+0x4b0 is set; its call is our composite hook). Else a whole-frame scan decides.
  const void* vt = nullptr;
  bool hasImpl = false;
  if (filter) {
    void* impl = nullptr;
    if (allocslab::ReadBytes(static_cast<uint8_t*>(filter) + kFnImpl, &impl, sizeof(impl)) && impl) {
      hasImpl = true;
      allocslab::ReadBytes(impl, &vt, sizeof(vt));
    }
  }
  const bool filterPoint = composite != nullptr || (vt && vt == g_visVt);
  if (!filterPoint) (hasImpl ? g_os.unknownFilter : g_os.nullFilter)++;
  // No filter point: the render point decides per record when it is hooked (R32 r4); else a whole-frame scan.
  rp.viaRender = !filterPoint && g_slotRender != nullptr && rp.recOk;
  if (rp.viaRender) g_os.viaRender++;
  rp.perRecord = filterPoint || rp.viaRender;
  rp.rowMajor = CtxRowMajor(t, ctx + kCtxVP);
  rp.vpDcs = LoadCtxVp(ctx + kCtxVP, rp.rowMajor);
  const bool allT = !rp.perRecord && rp.recOk && ScanFrame(h, recs, vb, static_cast<int>(ve - vb), view);
  const int why = Decide(t, rp.vpDcs, rp.perRecord, allT);
  DiagReplay(t, view, why, rp, filter, hasImpl, vt, composite);
  switch (why) {
    case kNatNotHmd: g_os.natNotHmd++; break;
    case kNatOutside: g_os.natOutside++; break;
    case kNatCockpit: g_os.natCockpit++; break;
    case kNatView: g_os.natView++; break;
    case kNatOff: g_os.natOff++; break;
    case kNatReset: g_os.natReset++; break;
    case kNatPair: g_os.natPair++; break;
    case kNatFilter: g_os.natFilter++; break;
    case kNatObserve: g_os.natObserve++; break;
    case kNatLatched: g_os.natLatched++; break;
    case kFailCam: g_os.failCam++; break;
    case kFailSplit: g_os.failSplit++; break;
    default: break;
  }
  if (t >= 0 && t <= 3) {  // the frame dump's CSV: the view's replays and its first replay's decision
    ViewFrame& dv = g_f.v[t];
    ++dv.cpuReplays;
    if (dv.cpuWhy < 0) dv.cpuWhy = why;
    if (why == kMoved) ++dv.cpuMoved;
  }
  if (t >= 0 && t <= 3 && why != kNatNotHmd && why != kNatOutside && why != kNatLatched) {
    // The view's decision for the frame (a latched-moved view whose later replay meets the frame stop keeps its
    // latch: its later replays meet the stop too).
    Latch& l = g_latch[t];
    if (l.frame != g_frame) {
      l.frame = g_frame;
      l.state = why == kMoved ? 1 : IsFail(why) ? 3 : 2;
    } else if (IsFail(why)) {
      l.state = 3;  // a later replay of a moved view could not be moved
    }
    if (why == kMoved) g_fd.anyMoved = true;
    if (why == kMoved || IsFail(why)) ++g_ovlAttempts;
    if (IsFail(why)) {
      NoteFail(t);
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 10)
        Log("cockpit ipd overlays (cpu): frame %llu view %d not moved: %s", static_cast<unsigned long long>(g_frame), t,
            why == kFailCam ? "the context's camera is not the view's cockpit camera"
                            : "a later replay of the view could not be moved (split)");
    } else if (why == kNatView || why == kNatFilter) {
      FrameStop(t, false);  // this view cannot be moved: neither can any other in this frame and the next 30
    }
  }
  // The observer's samples need DCS's VP^-1 and its camera centre (the boosted eye).
  rp.sampleOk = Inv4(rp.vpDcs, &rp.vpInv) && Centre(rp.vpDcs, rp.C);
  if (rp.sampleOk) rp.vm = MaxAbs3(rp.vpInv);
  if (why != kMoved) return true;
  const ViewFrame& vf = g_f.v[t];
  for (int i = 0; i < 3; ++i) rp.dw[i] = vf.dw[i];
  rp.vpTrueM = TrueVp(rp.vpDcs, rp.dw);
  ToFloats(rp.vpTrueM, rp.vpTrue, rp.rowMajor);
  rp.moved = rp.wasMoved = true;
  g_os.moved++;
  if (!rp.perRecord) {  // every record committed T: the whole replay at the true eye
    memcpy(ctx + kCtxVP, rp.vpTrue, 64);
    memcpy(rp.last, rp.vpTrue, 64);
  }
  return true;
}
// The replay returns (also on an exception unwinding through it): DCS's bytes back, unless DCS wrote its own.
void ReplayEndImpl() {
  Replay& rp = t_rp;
  if (!rp.open) return;
  bool foreign = false;
  __try {
    if (memcmp(rp.ctx + kCtxVP, rp.last, 64) == 0) {
      if (memcmp(rp.last, rp.saved, 64) != 0) memcpy(rp.ctx + kCtxVP, rp.saved, 64);
      g_os.restored++;
    } else {
      g_os.restoreMismatch++;  // DCS's camera written inside the replay: left as DCS wrote it
      if (!g_loggedMismatch) {
        g_loggedMismatch = true;
        Log("cockpit ipd overlays (cpu): frame %llu: currentContext's viewProj changed by DCS inside a replay; left "
            "as DCS wrote it",
            static_cast<unsigned long long>(g_frame));
      }
    }
    foreign = g_curCtx && *g_curCtx != rp.ctx;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fault = true;
  }
  if (foreign) {
    g_os.ctxForeign++;  // another context current at the return (pushed, not popped)
    if (rp.wasMoved && !rp.abort) FailReplay(rp);
  }
  rp.open = false;
  rp.moved = false;
  t_tagValid = t_tagTrue = false;
}
void ReplayEnd() {
  CostScope cost(kCostOvlCpu);
  ReplayEndImpl();
}
void ReplayHook(void* self, int view, void* ctx, void* filter) {
  g_replayBusy.fetch_add(1, std::memory_order_acq_rel);
  g_hookBusy.fetch_add(1, std::memory_order_acq_rel);
  const ReplayFn orig = g_replayOrig;
  bool mine = false;
  t_replayRa = _ReturnAddress();
  if (orig) {
    __try {
      mine = ReplayBegin(self, view, filter);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_fault = true;
      mine = t_rp.open;
    }
  }
  if (mine) g_replayOpen.fetch_add(1, std::memory_order_acq_rel);
  const bool traced = orig && TraceThread() && TraceReplaySeh(self, view);  // R94: SymbologyTrace (dev)
  __try {
    if (orig) orig(self, view, ctx, filter);  // (never null: the original is set before the slot is written)
  } __finally {
    if (traced) TraceReplayEndSeh();
    if (g_symReplayEnd && SymViewNow(nullptr, false)) SymReplayEndSeh(t_ovlView);  // R95
    if (mine) {
      ReplayEnd();
      g_replayOpen.fetch_sub(1, std::memory_order_acq_rel);
    }
    g_hookBusy.fetch_sub(1, std::memory_order_acq_rel);
    g_replayBusy.fetch_sub(1, std::memory_order_acq_rel);
  }
}

// ---------------------------------------------------------------------------
// The observer (OvlEffectUpload on this path: nothing written)
// ---------------------------------------------------------------------------

// Pure: a 4x4 matrix W in data (either order, not the MVP's own 64 bytes) with W * vpA == mvp (1) or W * vpB ==
// mvp (2), relative 1e-4; 0 none. *off / *order: where (start the search at the cached one).
int MatchWorld(const uint8_t* data, UINT len, const M4& mvp, UINT mvpOff, const M4& vpA, const M4& vpB, UINT* off,
               int* order, bool cached) {
  double sc = 0;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) sc = std::max(sc, std::fabs(mvp.a[r][c]));
  if (!(sc > 0)) return 0;
  auto test = [&](UINT o, int ord) -> int {
    if (o + 64 > len || (o < mvpOff + 64 && mvpOff < o + 64)) return 0;
    M4 w = LoadM(data, o);
    if (ord) {
      M4 tr;
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) tr.a[r][c] = w.a[c][r];
      w = tr;
    }
    if (std::fabs(w.a[0][3]) + std::fabs(w.a[1][3]) + std::fabs(w.a[2][3]) + std::fabs(w.a[3][3] - 1) > 1e-4) return 0;
    for (int k = 1; k <= 2; ++k) {
      const M4 p = MulM(w, k == 1 ? vpA : vpB);
      double dev = 0;
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) dev = std::max(dev, std::fabs(p.a[r][c] - mvp.a[r][c]));
      if (dev <= 1e-4 * sc) return k;
    }
    return 0;
  };
  if (cached) {
    const int k = test(*off, *order);
    if (k) return k;
  }
  for (UINT o = 0; o + 64 <= len && o + 64 <= kMaxSrc; o += 16)
    for (int ord = 0; ord < 2; ++ord) {
      const int k = test(o, ord);
      if (k) {
        *off = o;
        *order = ord;
        return k;
      }
    }
  return 0;
}

// A failure the observer traced to the record key drawn now: that key frozen at DCS's eye at once (not a failure
// of the view). A key that cannot carry it (frozen by such a failure already, or no room in the table) makes it the
// view's: wrong B then puts DCS's camera back for the rest of the replay; both count toward the inert window and
// stop the frame.
void KeyFailure(Replay& rp, uint64_t key, bool wrongB) {
  if (g_learn.FailKey(key, &g_learnEvents)) {
    g_os.keyFail++;
    if (rp.t >= 0 && rp.t <= 3) ++g_f.v[rp.t].cpuKeyFails;
    return;
  }
  g_os.keyFailView++;
  if (wrongB) AbortReplay(rp);
  FailReplay(rp);
}
void ObserveUploadImpl(ID3D11Resource* r, const uint8_t* s, UINT len) {
  Replay& rp = t_rp;
  if (!rp.open || !t_tagValid || !rp.sampleOk || rp.t < 0 || rp.t > 3 || len < 64) return;
  g_os.obsUploads++;
  const MvpScan sc = ScanMvpsCached(r, s, std::min<UINT>(len, kMaxSrc), rp.vpInv, rp.vm, rp.C);
  int hi = -1;
  for (int i = 0; i < sc.n && hi < 0; ++i)
    if (sc.hit[i].usable) hi = i;
  if (hi < 0) return;
  g_os.obsMvp++;
  const MvpHit& h = sc.hit[hi];
  M4 m = LoadM(s, h.off);
  if (h.order) {
    M4 tr;
    for (int a = 0; a < 4; ++a)
      for (int b = 0; b < 4; ++b) tr.a[a][b] = m.a[b][a];
    m = tr;
  }
  // The record's world matrix (FX11 sets matWorld next to matWorldViewProj): which camera the MVP was built with.
  WCache& wc = g_wCache[(reinterpret_cast<uintptr_t>(r) >> 4) % 64];
  const bool cached = wc.res == r && wc.have;
  UINT woff = cached ? wc.off : 0;
  int word = cached ? wc.order : 0;
  const M4& used = t_tagTrue ? rp.vpTrueM : rp.vpDcs;
  const M4& other = t_tagTrue ? rp.vpDcs : rp.vpTrueM;
  const int k = rp.wasMoved ? MatchWorld(s, std::min<UINT>(len, kMaxSrc), m, h.off, used, other, &woff, &word, cached)
                            : MatchWorld(s, std::min<UINT>(len, kMaxSrc), m, h.off, rp.vpDcs, rp.vpDcs, &woff, &word,
                                         cached);
  double ow[3];
  if (k) {
    wc.res = r, wc.off = woff, wc.order = word, wc.have = true;
    g_os.obsWorld++;
    M4 w = LoadM(s, woff);
    if (word) {
      M4 tr;
      for (int a = 0; a < 4; ++a)
        for (int b = 0; b < 4; ++b) tr.a[a][b] = w.a[b][a];
      w = tr;
    }
    for (int i = 0; i < 3; ++i) ow[i] = w.a[3][i];
  } else {
    // No W: X = MVP * VP_dcs^-1 = W (DCS's camera) or W * T(-dw) (VP_true written).
    const M4 x = MulM(m, rp.vpInv);
    for (int i = 0; i < 3; ++i) ow[i] = x.a[3][i] + (t_tagTrue ? rp.dw[i] : 0.0);
  }
  if (rp.wasMoved) {
    if (t_tagTrue) {
      (k == 1 ? g_os.cpuOk : k == 2 ? g_os.cpuMiss : g_os.cpuUnverified)++;
      // A cockpit display drawn at DCS's camera (an MVP builder we do not reach): the key's failure (R32 r3). It
      // is frozen at DCS's eye at once, so every later record of it, in every view, is drawn as it was here; the
      // view and the frame go on (no flash of the other displays, no inert count).
      if (k == 2) KeyFailure(rp, t_tagKey, false);
    } else if (k == 2) {
      g_os.cpuWrongB++;  // a B / unknown record built with VP_true: our bytes reached a record they were not for
      KeyFailure(rp, t_tagKey, true);
    } else {
      (k == 1 ? g_os.cpuOk : g_os.cpuUnverified)++;
    }
  }
  double oe[3];  // the origin relative to the view's own (boosted) eye
  for (int i = 0; i < 3; ++i) oe[i] = ow[i] - rp.C[i];
  g_learn.Sample(rp.t >> 1, rp.t & 1, t_tagKey, g_frame, ow, oe);  // pairs 0/1 and 2/3, both verified
}
void ObserveUpload(ID3D11Resource* r, const uint8_t* s, UINT len) {
  __try {
    ObserveUploadImpl(r, s, len);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fault = true;
  }
}

// ---------------------------------------------------------------------------
// Pass boundaries, frame end (render thread)
// ---------------------------------------------------------------------------

void PassBoundary(void* pass, bool begin, int hmdOrd) {
  char n[48];
  PassName(pass, n, sizeof(n));
  const bool hdr = strcmp(n, "OverlayHDRPass") == 0, ldr = strcmp(n, "OverlayLDRPass") == 0,
             gui = strcmp(n, "GUIPass") == 0;
  if (!hdr && !ldr && !gui) return;
  if (begin) {
    t_ovlOpen = true;
    t_ovlLdr = ldr || gui;
    t_ovlGui = gui;
    t_ovlView = hmdOrd;
    return;
  }
  // Backstop (R32 4.1 point 3): a replay of ours still open at its pass end (not expected) is closed.
  if (t_rp.open) ReplayEnd();
  t_ovlOpen = t_ovlLdr = t_ovlGui = false;
  t_ovlView = -1;
}

void EndFrame() {
  CostScope cost(kCostOvlCpu);
  // The frame stop's block: a stop in this frame blocks the next kPairRecover frames; each frame without one
  // counts the block down.
  if (g_fd.frame == g_frame && (g_fd.fail || g_fd.rule))
    g_frameBlock = kPairRecover;
  else if (g_frameBlock > 0)
    --g_frameBlock;
  int contra = 0;
  g_learn.EndFrame(g_frame, &g_learnEvents, &contra);
  // A contradicted key (frozen at B in this frame) is the key's failure, not the view's: not in the inert window
  // (R32 r3); counted (lContra).
  if (!g_learnEvents.empty()) {
    TraceLine("overlay keys at frame %llu:%s", static_cast<unsigned long long>(g_frame), g_learnEvents.c_str());
    g_learnEvents.clear();
  }
  g_os.lSamples = g_learn.samples;
  g_os.lCommits = g_learn.commits;
  g_os.lFlips = g_learn.flips;
  g_os.lMixed = g_learn.mixed;
  g_os.lContra = g_learn.contradictions;
  g_os.lNoise = g_learn.noise;
  g_os.lKeys = static_cast<uint64_t>(g_learn.used);
  g_os.lExpired = g_learn.expired;
  g_os.lFull = g_learn.full;
  g_os.lKeysT = static_cast<uint64_t>(g_learn.keysT);
  g_os.lKeysB = static_cast<uint64_t>(g_learn.keysB);
  ConsumeResets();  // last: the table and the render state cleared for the next frame
}

// ---------------------------------------------------------------------------
// Install / uninstall (config thread)
// ---------------------------------------------------------------------------

// One slot back to its original (the loader's registry, or the test's plain write).
void UnhookOne(void** slot, void* orig) {
  if (!slot) return;
  if (g_installedViaLoader)
    UnhookSlot(slot, orig);
  else
    *slot = orig;
}
// One slot to our hook again (the uninstall keeping the hooks, kStatePass).
bool RehookOne(void** slot, void* fn) {
  if (!slot) return false;
  if (g_installedViaLoader) return HookSlot(slot, fn, nullptr);
  *slot = fn;
  return true;
}
// Waits until no call counted by busy runs (at most 1 s); false when one is still running.
bool Drain(const std::atomic<int>& busy) {
  for (int i = 0; i < 200 && busy.load(); ++i) Sleep(5);
  return busy.load() == 0;
}

// From kStatePass back to installed: the hooks are still in their slots. Refused (retried) while the replay that
// outlived the uninstall's drain still runs; the render state and the learner are reset by the render thread at
// its next frame end (never from here, R32 r3).
bool LeavePassThrough() {
  if (g_replayOpen.load(std::memory_order_acquire) != 0) {
    g_os.installBusy++;
    return false;
  }
  if (g_slotReplay && *g_slotReplay != reinterpret_cast<void*>(&ReplayHook) &&
      !RehookOne(g_slotReplay, reinterpret_cast<void*>(&ReplayHook)))
    return false;
  g_os.lResetInstall++;
  RequestReset(kResetLearner | kResetRender);
  g_passThrough = false;
  g_state = kStateOn;
  Log("cockpit ipd overlays (cpu): hooks kept since the uninstall are active again");
  return true;
}

// The hooks into the given slots (tests: fake vtables; Install: DCS's after the signature check). Every original
// is in place before its slot points at our hook (a render-thread call may come at once); a slot that cannot be
// hooked rolls back the ones already hooked. Nothing of the render thread's state is touched here (a replay may be
// running on it): the learner and the render state are reset by the render thread at its next frame end, and
// that frame moves nothing (R32 r3).
bool InstallAt(void** slotReplay, void** slotVis, void** slotComp, uint8_t** curCtx, const void* visVt,
               bool viaLoader, void** slotRender = nullptr, const void* renderRet = nullptr) {
  const int st = g_state.load();
  if (st == kStateOn) return true;
  if (st == kStatePass) return LeavePassThrough();
  g_os.lResetInstall++;
  RequestReset(kResetLearner | kResetRender);
  g_curCtx = curCtx;
  g_visVt = visVt;
  g_installedViaLoader = viaLoader;
  g_passThrough = false;
  int n = 0;
  auto hook = [&](void** slot, void* fn) {
    ++n;
    if (g_testFailSlot == n) return false;
    if (viaLoader) return HookSlot(slot, fn, nullptr);
    *slot = fn;
    if (g_testAfterSlotWrite) g_testAfterSlotWrite(slot);
    return true;
  };
  auto original = [&](void** slot) { return viaLoader ? SlotOriginal(slot) : *slot; };
  g_compOrig = reinterpret_cast<FilterFn>(original(slotComp));
  g_visOrig = reinterpret_cast<FilterFn>(original(slotVis));
  g_replayOrig = reinterpret_cast<ReplayFn>(original(slotReplay));
  g_renderOrig = slotRender ? reinterpret_cast<RenderFn>(original(slotRender)) : nullptr;
  g_renderRet = renderRet;
  bool ok = g_compOrig && g_visOrig && g_replayOrig && (!slotRender || g_renderOrig);
  if (ok && hook(slotComp, reinterpret_cast<void*>(&CompFilterHook))) {
    g_slotComp = slotComp;
    if (hook(slotVis, reinterpret_cast<void*>(&VisFilterHook))) {
      g_slotVis = slotVis;
      // The render point (optional) before the replay: a replay only decides through it once it is in place.
      if (!slotRender || hook(slotRender, reinterpret_cast<void*>(&RenderHook))) {
        g_slotRender = slotRender;
        if (hook(slotReplay, reinterpret_cast<void*>(&ReplayHook))) {  // last: the per-record points are ready
          g_slotReplay = slotReplay;
          g_state = kStateOn;
          return true;
        }
      }
    }
  }
  // Roll back (the hooks keep their originals: a call already in flight stays valid).
  if (g_slotVis || g_slotComp || g_slotRender) g_os.installRollback++;
  UnhookOne(g_slotRender, reinterpret_cast<void*>(g_renderOrig));
  UnhookOne(g_slotVis, reinterpret_cast<void*>(g_visOrig));
  UnhookOne(g_slotComp, reinterpret_cast<void*>(g_compOrig));
  Drain(g_hookBusy);
  g_slotReplay = g_slotVis = g_slotComp = g_slotRender = nullptr;
  return false;
}

bool g_waitLogged = false;
// DCS's modules: verified, then hooked. False while a module is not loaded yet (retried each second), and while
// hooks kept by an uninstall cannot be taken back yet.
bool Install() {
  const int st = g_state.load();
  if (st == kStatePass) return LeavePassThrough();
  if (st != kStateNone) return st > 0;
  uint8_t* img[kModCount];
  uintptr_t vb[kModCount];
  for (int m = 0; m < kModCount; ++m) {
    img[m] = reinterpret_cast<uint8_t*>(GetModuleHandleW(kModNames[m]));
    vb[m] = reinterpret_cast<uintptr_t>(img[m]);
    if (!img[m]) {
      if (!g_waitLogged) {
        g_waitLogged = true;
        Log("cockpit ipd overlays (cpu): waiting for %ls", kModNames[m]);
      }
      return false;
    }
  }
  const char* why = VerifySites(img, vb, &SlotOriginal);
  uint8_t** cc = reinterpret_cast<uint8_t**>(GetProcAddress(reinterpret_cast<HMODULE>(img[kModGc]), kCurCtxExport));
  if (!why && cc != reinterpret_cast<uint8_t**>(img[kModGc] + kCurCtxRva)) why = "currentContext export moved";
  if (why) {
    g_state = -1;
    g_os.sigFail++;
    Log("cockpit ipd overlays (cpu): %s; the cockpit symbology stays at DCS's camera (the d3d11 path does not take "
        "over)",
        why);
    return false;
  }
  // The render point: CockpitBase's import slot of RenderObject::render, holding GraphicsVista's export. Without it
  // the replays without a filter point are decided by the whole-frame scan (as before R32 r4).
  void** renderSlot = nullptr;
  {
    const uint32_t iat = sigscan::ImportSlotRva(img[kModCockpit], "GraphicsVista.dll", kRenderImport);
    void* exp = reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(img[kModGv]), kRenderImport));
    void** s = iat == kRenderIat ? reinterpret_cast<void**>(img[kModCockpit] + iat) : nullptr;
    void* cur = nullptr;
    if (s && exp && ReadSlot(s, &SlotOriginal, &cur) && cur == exp)
      renderSlot = s;
    else
      Log("cockpit ipd overlays (cpu): RenderObject::render import not as expected (slot 0x%x); replays without a "
          "filter point use the whole-frame scan",
          iat);
  }
  void** slot[kVtCount];
  for (int i = 0; i < kVtCount; ++i) slot[i] = reinterpret_cast<void**>(img[kVt[i].mod] + kVt[i].vt) + kVt[i].slot;
  if (!InstallAt(slot[kVtReplay], slot[kVtVis], slot[kVtComp], cc, img[kModVis] + kVt[kVtVis].vt, true, renderSlot,
                 renderSlot ? img[kModCockpit] + kRenderRet : nullptr)) {
    g_state = -1;
    Log("cockpit ipd overlays (cpu): a vtable slot could not be hooked (the others put back); overlays at DCS's "
        "camera");
    return false;
  }
  Log("cockpit ipd overlays (cpu): hooked the record replay, the two record filters%s (DCS's MVP camera set per "
      "record: HUD at DCS's camera, cockpit displays at the true eye once learned)",
      renderSlot ? " and the record render point" : "");
  return true;
}
// TickCfg: Install wanted (not installed yet, or hooks kept in pass-through by an uninstall).
inline bool NeedsInstall() {
  const int st = g_state.load();
  return st == kStateNone || st == kStatePass;
}

// The hooks pass through first; the replay slot back, its calls in flight drained (their records still go through
// our filters, so a replay never ends half-wrapped), then the filter slots, drained. The slots hold their true
// originals (an old payload is never unloaded: a call still in flight stays valid). A replay still running 1 s
// after its slot was put back (R32 r3): the replay slot is hooked again and every hook stays, passing through
// (kStatePass); nothing of the render thread's state is reset under that replay. Install takes them back once it
// has ended; a later Uninstall tries the drain again.
void Uninstall() {
  const int st = g_state.load();
  if (st != kStateOn && st != kStatePass) {
    if (st < 0) g_state = kStateNone;
    return;
  }
  g_passThrough = true;
  UnhookOne(g_slotReplay, reinterpret_cast<void*>(g_replayOrig));
  if (!Drain(g_replayBusy)) {
    g_os.busyAtStop++;
    const bool back = RehookOne(g_slotReplay, reinterpret_cast<void*>(&ReplayHook));
    g_state = kStatePass;
    Log("cockpit ipd overlays (cpu): a replay was still running 1 s after its slot was put back; the hooks stay "
        "installed, passing through (%s), until it has ended",
        back ? "replay slot hooked again" : "the replay slot could not be hooked again: DCS's");
    return;
  }
  UnhookOne(g_slotRender, reinterpret_cast<void*>(g_renderOrig));
  UnhookOne(g_slotVis, reinterpret_cast<void*>(g_visOrig));
  UnhookOne(g_slotComp, reinterpret_cast<void*>(g_compOrig));
  if (!Drain(g_hookBusy)) {
    // Only filter calls outside any replay of ours can be left (the replays have drained): they pass through.
    g_os.busyAtStop++;
    Log("cockpit ipd overlays (cpu): a record filter call was still running 1 s after its slot was put back (it "
        "passes through)");
  }
  g_slotReplay = g_slotVis = g_slotComp = g_slotRender = nullptr;
  g_state = kStateNone;
  g_waitLogged = false;
}

void LogStats() {
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  const int st = g_state.load();
  Log("cockpit ipd overlays (cpu) %s: replays %llu moved %llu; not moved: not HMD %llu, outside OverlayHDR/LDR "
      "%llu, cockpit off %llu, view's cockpit not moved %llu, off/inert %llu, reset pending %llu, frame stop %llu, "
      "latched %llu, no filter point %llu, observe %llu, other thread %llu, pass-through %llu; ovlCpuCam %llu "
      "ovlCpuSplit %llu ctxForeign %llu recAlign %llu filterUnwind %llu; stops: failure %llu, rule %llu, "
      "ovlCpuPairLate %llu, recurInert %llu (jittered camera %llu, view map diff %llu)",
      st == kStateOn ? "on" : st == kStatePass ? "PASS-THROUGH (uninstalled while busy)"
                            : st < 0       ? "OFF (signature)"
                                           : "not installed",
      u(g_os.replays), u(g_os.moved), u(g_os.natNotHmd), u(g_os.natOutside), u(g_os.natCockpit), u(g_os.natView),
      u(g_os.natOff), u(g_os.natReset), u(g_os.natPair), u(g_os.natLatched), u(g_os.natFilter), u(g_os.natObserve),
      u(g_os.natThread), u(g_os.natPass), u(g_os.failCam), u(g_os.failSplit), u(g_os.ctxForeign), u(g_os.recAlign),
      u(g_os.filterUnwind), u(g_os.stopFail), u(g_os.stopRule), u(g_os.pairLate), u(g_os.recurInert),
      u(g_os.camJitter), u(g_os.viewMapDiff));
  Log("cockpit ipd overlays (cpu) records %llu: true eye %llu, DCS's %llu, skipped %llu, state %llu; null filter "
      "%llu, unknown filter %llu, frame scans %llu; restored %llu, restore mismatch %llu, ctx reset %llu, put back "
      "%llu / copy %llu; observer: uploads %llu, MVP %llu, world %llu, ok %llu, cpu miss %llu, wrong B %llu, "
      "unverified %llu, key frozen %llu, not the key's %llu; learner: samples %llu keys %llu (T %llu, B %llu) "
      "commits %llu flips %llu mixed key %llu (after commit %llu) noise %llu expired %llu full %llu, resets %llu "
      "(mission/slot %llu, scene cut %llu, install %llu); signature failures %llu, rollbacks %llu, busy at the "
      "uninstall %llu, reinstall refused while busy %llu",
      u(g_os.recs), u(g_os.recTrue), u(g_os.recDcs), u(g_os.recSkip), u(g_os.recState), u(g_os.nullFilter),
      u(g_os.unknownFilter), u(g_os.scanned), u(g_os.restored), u(g_os.restoreMismatch), u(g_os.ctxReset),
      u(g_os.abortPutBack), u(g_os.copyPutBack), u(g_os.obsUploads), u(g_os.obsMvp), u(g_os.obsWorld), u(g_os.cpuOk),
      u(g_os.cpuMiss), u(g_os.cpuWrongB), u(g_os.cpuUnverified), u(g_os.keyFail), u(g_os.keyFailView),
      u(g_os.lSamples), u(g_os.lKeys), u(g_os.lKeysT), u(g_os.lKeysB), u(g_os.lCommits), u(g_os.lFlips),
      u(g_os.lMixed), u(g_os.lContra), u(g_os.lNoise), u(g_os.lExpired), u(g_os.lFull), u(g_os.lResets),
      u(g_os.lResetMission), u(g_os.lResetCut), u(g_os.lResetInstall), u(g_os.sigFail), u(g_os.installRollback),
      u(g_os.busyAtStop), u(g_os.installBusy));
  Log("cockpit ipd overlays (cpu) render point %s: replays through it %llu, render calls %llu, records %llu, passed "
      "through %llu; context VP in cPerView's layout %llu",
      g_slotRender ? "hooked" : "not hooked", u(g_os.viaRender), u(g_os.renderCalls), u(g_os.renderRecs),
      u(g_os.renderOther), u(g_os.ctxColMajor));
}

}  // namespace ocpu
