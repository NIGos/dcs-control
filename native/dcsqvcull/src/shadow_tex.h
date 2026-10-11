// Shadow casters: skip the material texture sets the shadow pass never reads
// (R10 candidate C2). [Model] ShadowTextureSkip.
//
// Tags: [V] verified in the binary (dx11backend.dll / NGModel.dll of DCS
// 2.9.30, or the shader sources), [I] inferred.
//
// What DCS does per shadow caster [V NGModel 0x17750, ModelMaterialMT slot 5]:
//   mat+0x18c = item+0xd4; shader->vt[27](mat+0x68, sbPositions);
//   alpha = props->getTexture(0xF).valid() || props->getTexture(0x12).valid();
//   if (props+8 != 0 || alpha)
//     for i < mat+0x2d8: if (h = mat+0x240[i]) != -1:
//       shader->vt[26](h, tex = entry[i]+8, aux = entry[i]+0, &Vec2i(-1,-1));
//   tail jump submit 0x131e0(mat, tech = props+0x33 ? mat+0x218 : mat+0x210, pass 0, a3).
// The submit sets that technique (DX11Shader vt[7] -> DX11Renderer::setShader)
// and DX11Renderer::draw applies pass 0 of it: technique object =
// [shader+0xb0][tech-1].+0x20 (records of 0x50 bytes, handles 1-based),
// pass = tech->vt[7] (GetPassByIndex), pass->vt[13] (Apply) [V 0x1ab80,
// 0x14870]. DeckMaterialMT slot 5 (0x10760) binds no textures [V], so only
// ModelMaterialMT is handled. Glass casters: 0 per frame (measured).
//
// DX11Shader slot 26 (0x1fd70) = (shader, h, tex, aux, const Vec2i* size) [V]:
//   if (!tex) return;                                   (no effect at all)
//   rec = [shader+0xc8] + h*0x50;  var = rec+0x40;
//   tex->vt[23](*size);                                 (streaming request)
//   desc = getDesc(tex) 0x479a0; if (!compat(rec+0xc, desc+0x20)) { var->SetResource(0); return; }
//   tex->vt[18]();
//   srv = [tex+0x678] == 0 ? getSRV 0x47c60(tex, aux, size)
//       : rec+0xc in {0x20,0x21} ? getSRV(tex, aux, &0) : 0;
//   var->vt[31] SetResource(srv).
// SetResource in DCS's D3DX11Effects fork is `*var->pData = srv; return 0`
// (0x61ca0): no AddRef, no dirty flag [V]. So the FX part is trivial; the
// cost is everything that computes srv.
//
// Exactness, part 1: the skipped variable is not read by this draw.
//   The mask of "read" parameters is built per (shader, lockon_shadows
//   handle, lockon_shadows_transparent handle) from shadow_inst.h's own
//   compile of the shader's key (variant (a), B0: DCS's sources, the key's
//   exact defines, D3DCompile2 fx_5_0 O3 as DCS calls it [V 0x2e720]).
//   DCS's live effects are optimized: no stage keeps its bytecode, so the
//   live effect cannot be reflected (the first version of this file tried,
//   and every shader logged "shader without reflection data").
//   - (a) is DCS's shader: shadow_inst rebuilds the key from the define
//     vector at +0x98 and rejects the key unless it equals DCS's key string
//     at +0x78; our pipeline reproduces DCS's cached effects byte for byte
//     (offline test over the fxo cache); the live pass counts of both
//     techniques must equal (a)'s; every live parameter record name
//     (rec+0x30, the FX variable's GetDesc name [V builder 0x1e000]) must be
//     a variable, constant buffer or interface of (a), else the shader keeps
//     every texture set. [I] (a) reads Bazar\shaders: a shader mod in DCS's
//     VFS that changes what a shadow pass reads without adding or renaming a
//     variable would not be detected (shadow_inst.h has the same premise).
//   - Read set: for every pass of both techniques in (a), every shader its
//     assignments select (any stage, inline or by variable), the resources
//     the DXBC RDEF chunk lists. A parameter record is "read" when its name
//     matches a bound resource name (NameRefers: the variable itself, "v[i]"
//     or "v.m"; unnamed records are read). FX binds SRVs at Apply only from
//     per-shader dependency lists that FX itself builds from the same
//     reflection by name [V Apply 0x65850 -> 0x67f90 -> ApplyShaderBlock
//     0x68470 per stage block; I: name mapping as in FX11's loader, and the
//     live effect's lists were built from the same bytecode before it was
//     optimized]. So a variable whose name no stage binds is not read by the
//     pass's Apply.
//   - Rejected (the technique, hence the shader, keeps every set): in (a), a
//     pass assignment selected at run time (index or expression), naming no
//     variable, or a shader without a program or a readable RDEF; in the live
//     effect, a pass whose assignments can switch shaders (assignment type
//     other than numeric 1-4) [V layout 0x67e10: count pass+0x10, array
//     pass+0x18, 0x38 bytes, type +4], an invalid pass, or not 1-16 passes.
//   - Until shadow_inst has compiled the key (or when it is not running) the
//     shader has no mask: slot 5's copy keeps every texture set, and the
//     mask is looked up again at its next caster.
//   The union over both shadow techniques is used because opaque casters
//   (props+8 == 0, not alpha-tested) draw with either technique without
//   binding any texture [V 0x177f2]: such a draw must never read a variable
//   a previous caster of the same shader skipped. Every pass (not only pass
//   0, the one DX11Renderer::draw applies) is included: a superset.
//   Anything unexpected (vtable, more than kMaxRecords parameters, handle out
//   of range, shader not DX11Shader, access violation) -> that shader is
//   marked "keep".
//
// Exactness, part 2: the side effects of a skipped set are kept.
//   tex->vt[23](*size) is always called, through the vtable, exactly as
//   slot 26 calls it (so the streaming dedupe hook, if on, sees the same
//   call). The rest of slot 26 except SetResource has no effect outside the
//   variable when all of the following hold, and is then not run:
//   - [tex] is the DX11Texture vtable and its inner object (+0x10) is a
//     DX11TextureFromFile/ArrayFromFile/Dummy InternalImpl: vt[18] -> inner
//     vt[8] is `ret` (0x5010) and getDesc's vt[11] -> inner vt[4] is a plain
//     getter (0x331f0 / 0x5250) [V]. (The render-target inner class has a
//     one-time GenerateMips-like vt[8] (0x33f70) and is not on the list.)
//   - getSRV would not swap the streamed mip sets: it only does when aux ==
//     -1, [tex+0x190] == 0 and the back set [tex+0x1a0] is ready (state +0x40
//     == 5, +0x20 != 0), then calls 0x49ca0 [V 0x47c91-0x47cee]. Otherwise it
//     only reads [V].
//   - getDesc's one-time static init (0x479a0) is idempotent.
//   Otherwise the remainder (getDesc, compat, vt[18], getSRV) is replayed
//   exactly, minus SetResource. The swap check runs after vt[23], as getSRV
//   would; the loader thread can still finish a mip set at any time, which is
//   the same race slot 26 has [I: equivalent to a legal interleaving].
//
// Exactness, part 3: the invariant "every later Apply that reads the variable
// re-sets it first".
//   [V] Every NGModel ModelMaterialMT path that applies a pass first calls
//   slot 26 for every handle != -1: main draw slot 4 (0x16140 -> 0xcf80 binds
//   all, unconditionally, before every case of its pass switch), shadow slot
//   5 (all, or none for opaque casters: covered by the union mask above),
//   flat shadow slot 6 (0x17640, all). The shader manager caches shaders by
//   md5 (0x2c390), so a DX11Shader is only shared between materials using
//   the same effect and defines.
//   [I] Slot 26 itself is a no-op for a NULL texture, so a later draw whose
//   texture entry is NULL keeps whatever was set before. In stock DCS that is
//   the last texture set by any draw, with the skip it can be an older one.
//   NGModel textures come from the texture manager (missing files get a
//   Dummy inner, [V class exists]); the terrain suite's slot-26 counter
//   reports NULL-texture sets so this can be checked (expected 0).
//   [I] No module other than NGModel applies passes of NGModel's material
//   shaders (shader name and defines come from the model's materials).
//   Recording the SRV each skip would have stored was rejected: computing it
//   is the whole cost of slot 26.
//   The cache is keyed by the DX11Shader pointer and invalidated by a hook on
//   DX11Shader's scalar deleting destructor (vt[0], the only caller of
//   ~DX11Shader 0x1d160 [V]); records, techniques and effect are set only by
//   the constructor (0x1c750 -> 0x1ee40) [V], and are also compared as a
//   fingerprint. The same hook makes shadow_inst forget the shader's
//   texture-read entries (and its queued snapshots), so a new shader at a
//   reused address is compiled and mapped as itself.
//
// Shadow batching (shadow_batch.h) on top: the leader calls slot 5 the way
// ShadowMapRenderable vt[1] does (a tail jump with the material, item and
// mesh), through DrawPso below with the group base as posStructOffset, i.e.
// this copy. The copy makes the original's calls in the original order
// (offline differential test), so the leader's binds,
// streaming requests and DX11Renderer::draw (with the batch's instance count
// from the renderer vtable copy) are what stock slot 5 would do, minus the
// SetResource of variables no shadow pass reads. The instanced VS shadow_batch
// swaps in binds (a)'s resources plus t127 (shadow_inst check 3b), so it reads
// no variable the mask skips. Members are skipped before slot 5; the planner
// groups casters by every texture entry slot 5 would set (instcount::
// TextureKey hashes the (aux, texture) pair of every handle, read or not), so
// each member's sets equal the leader's and the skip decisions are the same
// (a hash collision is the batching's own risk, unchanged by the skip). Member skipping and texture
// skipping compose: what the batched draw reads was set by the leader exactly
// as each member would have set it. Counters below count only casters that
// reach slot 5 (leaders and solos).
//
// Hook placement: ModelMaterialMT slot 5 is replaced by an exact C copy of
// 0x17750 (byte hash checked against this build, as is the submit 0x131e0
// and slot 26 with its callees) whose texture loop calls slot 26 only for
// parameters the shadow passes read. Casters whose shader has no mask (not
// compiled yet, or "keep") also go through the copy, with every set kept, so
// the per-frame counters see every texture set. Slot 26 itself is not hooked:
// it sees ~115k calls per frame, slot 5 ~14k.
// Included once from main.cpp inside its anonymous namespace, after
// binder_count.h and before shadow_inst.h (whose query is declared below).
#pragma once

namespace shadowtex {
// shadow_inst.h's ShadowReadTextures results.
enum : int { kReadsFailed = -1, kReadsPending = 0, kReadsReady = 1 };
}  // namespace shadowtex

namespace shadowinst {  // defined in shadow_inst.h
int ShadowReadTextures(void* shader, uint64_t tech, std::vector<std::string>* bound, std::vector<std::string>* vars,
                       std::string* why, std::string* key);
void ForgetShader(void* shader);
}  // namespace shadowinst

namespace shadowtex {

// ---------------------------------------------------------------------------
// Pure helpers (unit tested)
// ---------------------------------------------------------------------------

inline uint32_t Rd32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

// Resource names bound by a compiled shader, from the DXBC RDEF chunk.
// Returns the number of bindings written to names (pointers into the blob),
// or -1 if the blob is not a well-formed DXBC with an RDEF chunk or has more
// than maxNames bindings.
int ParseDxbcBindings(const uint8_t* p, size_t n, const char** names, int maxNames) {
  if (!p || n < 32 || memcmp(p, "DXBC", 4) != 0) return -1;
  const uint32_t total = Rd32(p + 24);
  if (total < 32 || total > n) return -1;
  n = total;
  const uint32_t chunks = Rd32(p + 28);
  if (chunks > 64 || 32ull + 4ull * chunks > n) return -1;
  for (uint32_t c = 0; c < chunks; ++c) {
    const uint32_t off = Rd32(p + 32 + 4 * c);
    if (off > n || n - off < 8) return -1;
    const uint32_t size = Rd32(p + off + 4);
    if (size > n - off - 8) return -1;
    if (memcmp(p + off, "RDEF", 4) != 0) continue;
    const uint8_t* d = p + off + 8;
    if (size < 28) return -1;
    const uint32_t count = Rd32(d + 8), rbOff = Rd32(d + 12), target = Rd32(d + 16);
    const uint32_t major = (target >> 8) & 0xff, minor = target & 0xff;
    const uint32_t stride = (major > 5 || (major == 5 && minor >= 1)) ? 40 : 32;
    if (count > static_cast<uint32_t>(maxNames)) return -1;
    if (rbOff > size || static_cast<uint64_t>(count) * stride > size - rbOff) return -1;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t nameOff = Rd32(d + rbOff + i * stride);
      if (nameOff >= size || !memchr(d + nameOff, 0, size - nameOff)) return -1;
      names[i] = reinterpret_cast<const char*>(d + nameOff);
    }
    return static_cast<int>(count);
  }
  return -1;
}

// Length of the top-level variable name: up to the first '[' or '.'.
inline size_t BaseLen(const char* s) {
  size_t i = 0;
  while (s[i] && s[i] != '[' && s[i] != '.') ++i;
  return i;
}

// True when a shader binding name refers to the effect variable varName (the
// variable itself, one of its elements "v[2]" or a member "v.x"). Errs
// towards true, never false for a real reference.
bool NameRefers(const char* varName, const char* bindName) {
  if (!varName || !bindName) return true;
  const size_t a = BaseLen(varName), b = BaseLen(bindName);
  return a == b && strncmp(varName, bindName, a) == 0;
}

// Per-shader mask: bit i set = parameter record i is read by a shadow pass.
constexpr uint32_t kMaxRecords = 512;
constexpr int kMaskWords = kMaxRecords / 64;

struct MaskEntry {
  std::atomic<void*> key{nullptr};  // DX11Shader*, nullptr = empty, kTomb = deleted
  // Fingerprint (also checked on every lookup).
  void* effect = nullptr;
  void* recBegin = nullptr;
  void* recEnd = nullptr;
  void* techBegin = nullptr;
  uint64_t techA = 0, techB = 0;
  uint32_t recCount = 0;
  int32_t state = 0;  // 1 = mask usable, -1 = keep every texture set
  uint64_t read[kMaskWords] = {};

  MaskEntry() = default;
  MaskEntry(const MaskEntry& o) { *this = o; }
  // Copies everything but the key (the cache publishes the key last).
  MaskEntry& operator=(const MaskEntry& o) {
    effect = o.effect;
    recBegin = o.recBegin;
    recEnd = o.recEnd;
    techBegin = o.techBegin;
    techA = o.techA;
    techB = o.techB;
    recCount = o.recCount;
    state = o.state;
    memcpy(read, o.read, sizeof(read));
    return *this;
  }
};

inline bool Skippable(const MaskEntry& e, int64_t h) {
  return e.state > 0 && h >= 0 && static_cast<uint64_t>(h) < e.recCount &&
         ((e.read[h >> 6] >> (h & 63)) & 1) == 0;
}

// Open-addressing table. Lookups are lock-free (render thread); inserts and
// invalidations take the mutex. Entries are never freed: a lookup racing an
// invalidation sees either the old (still consistent) or the dead entry.
class MaskCache {
 public:
  static constexpr size_t kSize = 2048;  // power of two
  static void* Tomb() { return reinterpret_cast<void*>(uintptr_t{1}); }

  static size_t Hash(const void* p) {
    return static_cast<size_t>((reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 53) &
           (kSize - 1);
  }

  const MaskEntry* Find(const void* shader, uint64_t techA, uint64_t techB, void* effect, void* recBegin,
                        void* recEnd, void* techBegin) const {
    size_t i = Hash(shader);
    for (size_t probe = 0; probe < kSize; ++probe, i = (i + 1) & (kSize - 1)) {
      void* k = slots_[i].key.load(std::memory_order_acquire);
      if (!k) return nullptr;
      if (k != shader) continue;
      const MaskEntry& e = slots_[i];
      if (e.techA == techA && e.techB == techB && e.effect == effect && e.recBegin == recBegin &&
          e.recEnd == recEnd && e.techBegin == techBegin)
        return &e;
    }
    return nullptr;
  }

  // Copies src (key = shader) into a free slot. Returns the stored entry, or
  // nullptr when the table is full.
  const MaskEntry* Insert(void* shader, const MaskEntry& src) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t i = Hash(shader), freeSlot = kSize;
    for (size_t probe = 0; probe < kSize; ++probe, i = (i + 1) & (kSize - 1)) {
      void* k = slots_[i].key.load(std::memory_order_relaxed);
      if (k == Tomb() && freeSlot == kSize) freeSlot = i;
      if (!k) {
        if (freeSlot == kSize) freeSlot = i;
        break;
      }
    }
    if (freeSlot == kSize) return nullptr;
    MaskEntry& e = slots_[freeSlot];
    e = src;
    e.key.store(shader, std::memory_order_release);
    ++used_;
    return &e;
  }

  // Forgets every entry of a shader (it is being destroyed).
  void Invalidate(const void* shader) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t i = Hash(shader);
    for (size_t probe = 0; probe < kSize; ++probe, i = (i + 1) & (kSize - 1)) {
      void* k = slots_[i].key.load(std::memory_order_relaxed);
      if (!k) return;
      if (k == shader) {
        slots_[i].key.store(Tomb(), std::memory_order_release);
        --used_;
        ++invalidated_;
      }
    }
  }

  size_t Used() const { return used_; }
  size_t Invalidated() const { return invalidated_; }

 private:
  MaskEntry slots_[kSize];
  std::mutex mutex_;
  size_t used_ = 0, invalidated_ = 0;
};

// ---------------------------------------------------------------------------
// Build checks and addresses
// ---------------------------------------------------------------------------

// NGModel.dll
constexpr uint32_t kModelVtbl = 0x592f0;     // .?AVModelMaterialMT@model@@
constexpr uint32_t kSlot5 = 0x17750, kSlot5End = 0x17892;
constexpr uint64_t kSlot5Hash = 0x49534eb2132118cfull;
constexpr uint32_t kSubmit = 0x131e0, kSubmitEnd = 0x13315;
constexpr uint64_t kSubmitHash = 0xfa5b29e55d76ca0cull;
constexpr uint32_t kGlobals = 0x72e50;       // ?globals@model@@ (GlobalsMT*)
constexpr uint32_t kIatGetTexture = 0x57238; // PropertiesSet::getTexture(uint) const
constexpr uint32_t kIatValid = 0x57230;      // Texture2dProperties::valid() const
// dx11backend.dll
constexpr uint32_t kShaderVtbl = 0xb43b8;    // .?AVDX11Shader@RenderAPI@@
constexpr uint32_t kSetTexture = 0x1fd70, kSetTextureEnd = 0x1fe66;  // slot 26
constexpr uint64_t kSetTextureHash = 0xd7c1df3dc2fb5bccull;
constexpr uint32_t kGetDesc = 0x479a0, kGetDescEnd = 0x47a1d;
constexpr uint64_t kGetDescHash = 0x88604560f884ef35ull;
constexpr uint32_t kCompat = 0x1df30, kCompatEnd = 0x1df82;
constexpr uint64_t kCompatHash = 0x275cd10c465c1af9ull;
constexpr uint32_t kGetSrv = 0x47c60, kGetSrvEnd = 0x47d79;
constexpr uint64_t kGetSrvHash = 0xf31eee13d0c2176dull;
constexpr uint32_t kTexVtbl = 0xb7140;       // .?AVDX11Texture@RenderAPI@@
constexpr uint32_t kInnerFile = 0xb5b28;     // .?AVDX11TextureFromFileInternalImpl@RenderAPI@@
constexpr uint32_t kInnerArray = 0xb5a60;    // .?AVDX11TextureArrayFromFileInternalImpl@RenderAPI@@
constexpr uint32_t kInnerDummy = 0xb6db0;    // .?AVDX11TextureInternalDummy@RenderAPI@@
constexpr uint32_t kTechVtbl = 0xb9608;      // .?AUSTechnique@D3DX11Effects@@
constexpr uint32_t kPassVtbl = 0xb96b0;      // .?AUSPassBlock@D3DX11Effects@@

// Where this build has them: the recorded RVAs above, or where reloc.h
// re-found them in another build (0 = not found). Set by ResolveAddrs; until
// then the recorded RVAs (the tests' fake modules use those).
struct Addrs {
  uint32_t model = kModelVtbl, slot5 = kSlot5, submit = kSubmit, globals = kGlobals;  // NGModel.dll
  uint32_t iatGetTexture = kIatGetTexture, iatValid = kIatValid;
  uint32_t shader = kShaderVtbl, setTexture = kSetTexture, getDesc = kGetDesc, compat = kCompat, getSrv = kGetSrv;
  uint32_t tex = kTexVtbl, innerFile = kInnerFile, innerArray = kInnerArray, innerDummy = kInnerDummy;
  uint32_t tech = kTechVtbl, pass = kPassVtbl;
  uint32_t shaderDtor = 0, texSlot18 = 0, texSlot11 = 0, innerSlot4 = 0, dummySlot4 = 0, ret0 = 0;  // checks only
};
Addrs g_at;

using Slot5Fn = uint64_t(__fastcall*)(void* mat, void* item, void* a3, void* a4);
using SetTexFn = uint64_t(__fastcall*)(void* shader, int64_t h, void* tex, void* aux, const uint64_t* size);
using SetSbFn = uint64_t(__fastcall*)(void* shader, void* h, void* value);
using SubmitFn = uint64_t(__fastcall*)(void* mat, uint64_t tech, uint64_t pass, void* a4);
using GetTextureFn = const void*(__fastcall*)(const void* props, uint32_t idx);
using ValidFn = bool(__fastcall*)(const void* texProps);
using Tex23Fn = uint64_t(__fastcall*)(void* tex, uint64_t packedSize);
using Tex18Fn = uint64_t(__fastcall*)(void* tex);
using GetDescFn = const uint8_t*(__fastcall*)(void* tex);
using CompatFn = bool(__fastcall*)(int32_t type, int32_t format);
using GetSrvFn = void*(__fastcall*)(void* tex, void* aux, const uint64_t* size);
using DtorFn = void*(__fastcall*)(void* self, uint32_t flags);

std::atomic<bool>& g_on = g_shadowTexSkipOn;
Slot5Fn g_orig = nullptr;
DtorFn g_origDtor = nullptr;
void** g_slot = nullptr;
std::atomic<bool> g_attached{false};
std::atomic<int> g_state{0};  // 0 = not tried, 1 = installed, -1 = failed
uint8_t* g_ng = nullptr;
uint8_t* g_dx = nullptr;
SubmitFn g_submit = nullptr;
GetDescFn g_getDesc = nullptr;
CompatFn g_compat = nullptr;
GetSrvFn g_getSrv = nullptr;
void* g_shaderVtblPtr = nullptr;
void* g_texVtblPtr = nullptr;
void* g_innerFile = nullptr;
void* g_innerArray = nullptr;
void* g_innerDummy = nullptr;
MaskCache* g_cache = nullptr;
std::atomic<bool> g_cacheFull{false};  // no mask builds until a destructor frees a slot

// Per caster through the hook: casters; fallback = not a DX11Shader (DCS's
// own slot 5); pending = no mask yet (key not compiled), unmapped = shader
// marked "keep" (both drawn by the copy with every set kept). Per texture
// set: kept by the mask, keptNoMask (pending/unmapped casters), skipped
// (streaming request only), skippedReplayed (slot 26 replayed without
// SetResource), nullTex (no texture: slot 26 does nothing).
struct Counters {
  uint64_t casters = 0, fallback = 0, pending = 0, unmapped = 0;
  uint64_t kept = 0, keptNoMask = 0, skipped = 0, skippedReplayed = 0, nullTex = 0;
};
std::mutex g_regMutex;
std::vector<Counters*> g_counters;
thread_local Counters* t_counters = nullptr;
std::atomic<uint32_t> g_masksBuilt{0}, g_masksKept{0}, g_logBudget{12};

Counters* MyCounters() {
  Counters* c = t_counters;
  if (c) return c;
  c = new Counters;
  std::lock_guard<std::mutex> lock(g_regMutex);
  g_counters.push_back(c);
  t_counters = c;
  return c;
}

uint64_t Fnv(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

bool CodeIs(uint8_t* base, uint32_t begin, uint32_t end, uint64_t hash) {
  uint8_t buf[512];
  const size_t n = end - begin;
  return n <= sizeof(buf) && allocslab::ReadBytes(base + begin, buf, n) && Fnv(buf, n) == hash;
}

template <typename F>
F VSlot(void* obj, int slot) {
  return reinterpret_cast<F>((*static_cast<void***>(obj))[slot]);
}

// ---------------------------------------------------------------------------
// Mask construction (first sight of a shader; render thread)
// ---------------------------------------------------------------------------

constexpr int kMaxBindings = 128;  // per shader blob (shadow_inst.h's RDEF reads)

// Sets the read bit of every parameter record (0x50 bytes, name at +0x30)
// that one of the shader's bindings refers to.
void MarkRead(MaskEntry& e, const char* const* bindings, int n) {
  for (uint32_t r = 0; r < e.recCount; ++r) {
    const char* name = *reinterpret_cast<const char* const*>(static_cast<uint8_t*>(e.recBegin) + r * 0x50 + 0x30);
    for (int b = 0; b < n; ++b) {
      if (NameRefers(name, bindings[b])) {
        e.read[r >> 6] |= 1ull << (r & 63);
        break;
      }
    }
  }
}

// True when s is in the sorted (strcmp order) array v.
inline bool InSorted(const char* const* v, int n, const char* s) {
  int lo = 0, hi = n;
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    const int c = strcmp(v[mid], s);
    if (c == 0) return true;
    if (c < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return false;
}

// The read mask from (a)'s binding names (bound: both shadow techniques) and
// (a)'s variable names (vars, sorted). Every live record name must be a
// variable of (a); unnamed records count as read. *bad = the record that is
// not. Plain C: called under __try.
const char* MapRecords(MaskEntry& e, const char* const* bound, int nBound, const char* const* vars, int nVars,
                       uint32_t* bad) {
  for (uint32_t r = 0; r < e.recCount; ++r) {
    const char* name = *reinterpret_cast<const char* const*>(static_cast<uint8_t*>(e.recBegin) + r * 0x50 + 0x30);
    if (!name) {
      e.read[r >> 6] |= 1ull << (r & 63);
      continue;
    }
    if (!InSorted(vars, nVars, name)) {
      *bad = r;
      return "a live parameter is not a variable of our compile of the key";
    }
  }
  MarkRead(e, bound, nBound);
  return nullptr;
}

// Live cross-checks of one technique of the live effect: an FX technique of
// 1-16 valid passes whose assignments are numeric only (no shader or state
// object selected by a variable). Plain C: called under __try.
const char* CheckLiveTechnique(uint8_t* shader, uint64_t tech) {
  uint8_t* techBegin = *reinterpret_cast<uint8_t**>(shader + 0xb0);
  uint8_t* techEnd = *reinterpret_cast<uint8_t**>(shader + 0xb8);
  const uint64_t techCount = (techEnd - techBegin) / 0x50;
  if (tech < 1 || tech > techCount) return "technique handle out of range";
  void* t = *reinterpret_cast<void**>(techBegin + (tech - 1) * 0x50 + 0x20);
  if (!t || *static_cast<void**>(t) != g_dx + g_at.tech) return "technique is not an FX technique";
  struct {
    const char* name;
    uint32_t passes, annotations;
  } td = {};
  if (VSlot<long(__fastcall*)(void*, void*)>(t, 4)(t, &td) < 0) return "technique GetDesc failed";
  if (td.passes == 0 || td.passes > 16) return "unexpected pass count";
  for (uint32_t p = 0; p < td.passes; ++p) {
    void* pass = VSlot<void*(__fastcall*)(void*, uint32_t)>(t, 7)(t, p);
    if (!pass || *static_cast<void**>(pass) != g_dx + g_at.pass) return "pass is not an FX pass block";
    if (!VSlot<bool(__fastcall*)(void*)>(pass, 3)(pass)) return "pass is not valid";
    // Pass assignments left after load: numeric ones only.
    const uint32_t assignments = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pass) + 0x10);
    uint8_t* assign = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(pass) + 0x18);
    for (uint32_t a = 0; a < assignments; ++a) {
      const uint32_t type = *reinterpret_cast<uint32_t*>(assign + a * 0x38 + 4);
      if (type < 1 || type > 4) return "pass selects an object by variable";
    }
  }
  return nullptr;
}

const char* BuildRaw(uint8_t* shader, MaskEntry& e, const char* const* bound, int nBound, const char* const* vars,
                     int nVars, uint32_t* bad) {
  if (e.recCount > kMaxRecords) return "too many parameters";
  if (const char* why = CheckLiveTechnique(shader, e.techA)) return why;
  if (e.techB != e.techA)
    if (const char* why = CheckLiveTechnique(shader, e.techB)) return why;
  return MapRecords(e, bound, nBound, vars, nVars, bad);
}

const char* BuildGuarded(uint8_t* shader, MaskEntry& e, const char* const* bound, int nBound, const char* const* vars,
                         int nVars, uint32_t* bad) {
  __try {
    return BuildRaw(shader, e, bound, nBound, vars, nVars, bad);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return "access violation while reading the effect";
  }
}

// Technique name for the log (std::string at the technique record, SSO 16).
const char* TechName(uint8_t* shader, uint64_t tech) {
  uint8_t* b = *reinterpret_cast<uint8_t**>(shader + 0xb0);
  uint8_t* r = b + (tech - 1) * 0x50;
  return *reinterpret_cast<uint64_t*>(r + 0x18) > 0xf ? *reinterpret_cast<const char**>(r)
                                                       : reinterpret_cast<const char*>(r);
}

void LogMask(uint8_t* shader, const MaskEntry& e, const char* why, const std::string& key, uint32_t bad) {
  if (g_logBudget.load() == 0) return;
  g_logBudget.fetch_sub(1);
  char keyShort[200];
  snprintf(keyShort, sizeof(keyShort), "%s", key.empty() ? "?" : key.c_str());
  if (why) {
    const char* badName =
        bad < e.recCount ? *reinterpret_cast<const char**>(static_cast<uint8_t*>(e.recBegin) + bad * 0x50 + 0x30)
                         : nullptr;
    Log("shadow texture skip: shader %p: %s%s%s%s; its texture sets are kept (key %s)", shader, why,
        badName ? " (" : "", badName ? badName : "", badName ? ")" : "", keyShort);
    return;
  }
  char readNames[512] = {}, skipNames[512] = {};
  uint32_t nRead = 0;
  for (uint32_t r = 0; r < e.recCount; ++r) {
    const char* name = *reinterpret_cast<const char**>(static_cast<uint8_t*>(e.recBegin) + r * 0x50 + 0x30);
    const bool read = (e.read[r >> 6] >> (r & 63)) & 1;
    nRead += read;
    char* dst = read ? readNames : skipNames;
    const size_t len = strlen(dst);
    if (name && len + strlen(name) + 2 < sizeof(readNames)) snprintf(dst + len, sizeof(readNames) - len, "%s%s", len ? " " : "", name);
  }
  Log("shadow texture skip: shader %p (%s/%s): %u parameters, %u read by the shadow passes of our compile [%s], "
      "not read: %s; key %s",
      shader, TechName(shader, e.techA), TechName(shader, e.techB), e.recCount, nRead, readNames, skipNames,
      keyShort);
}

// The shader's mask: cached, else built from shadow_inst's reads. nullptr
// with *pending = true while the key is not compiled (nothing is cached: it
// is asked again at the next caster); nullptr with *pending = false when the
// cache is full.
const MaskEntry* Lookup(uint8_t* shader, uint64_t techA, uint64_t techB, bool* pending) {
  *pending = false;
  void* effect = *reinterpret_cast<void**>(shader + 0x50);
  void* recBegin = *reinterpret_cast<void**>(shader + 0xc8);
  void* recEnd = *reinterpret_cast<void**>(shader + 0xd0);
  void* techBegin = *reinterpret_cast<void**>(shader + 0xb0);
  if (const MaskEntry* e = g_cache->Find(shader, techA, techB, effect, recBegin, recEnd, techBegin)) return e;
  if (g_cacheFull.load(std::memory_order_relaxed)) return nullptr;
  std::vector<std::string> bound, boundB, vars;
  std::string readsWhy, key;
  int st = shadowinst::ShadowReadTextures(shader, techA, &bound, &vars, &readsWhy, &key);
  if (st == kReadsReady && techB != techA)
    st = shadowinst::ShadowReadTextures(shader, techB, &boundB, nullptr, &readsWhy, nullptr);
  if (st == kReadsPending) {
    *pending = true;
    return nullptr;
  }
  MaskEntry e;
  e.effect = effect;
  e.recBegin = recBegin;
  e.recEnd = recEnd;
  e.techBegin = techBegin;
  e.techA = techA;
  e.techB = techB;
  e.recCount = static_cast<uint32_t>((static_cast<uint8_t*>(recEnd) - static_cast<uint8_t*>(recBegin)) / 0x50);
  uint32_t bad = ~0u;
  const char* why = nullptr;
  if (st != kReadsReady) {
    why = readsWhy.empty() ? "shadow inst: technique not usable" : readsWhy.c_str();
  } else if (!effect) {
    why = "shader has no effect";
  } else {
    bound.insert(bound.end(), boundB.begin(), boundB.end());
    std::vector<const char*> b, v;
    for (const std::string& n : bound) b.push_back(n.c_str());
    for (const std::string& n : vars) v.push_back(n.c_str());  // sorted: std::string order is strcmp order
    why = BuildGuarded(shader, e, b.data(), static_cast<int>(b.size()), v.data(), static_cast<int>(v.size()), &bad);
  }
  e.state = why ? -1 : 1;
  if (why) memset(e.read, 0, sizeof(e.read));
  g_masksBuilt.fetch_add(1, std::memory_order_relaxed);
  if (why) g_masksKept.fetch_add(1, std::memory_order_relaxed);
  LogMask(shader, e, why, key, bad);
  const MaskEntry* stored = g_cache->Insert(shader, e);
  if (!stored) {
    g_cacheFull = true;
    static std::atomic<bool> once{false};
    if (!once.exchange(true)) Log("shadow texture skip: shader cache full; new shaders keep their texture sets");
  }
  return stored;
}

// ---------------------------------------------------------------------------
// Per-call path
// ---------------------------------------------------------------------------

// A texture set whose variable the shadow passes do not read: everything
// slot 26 does except var->SetResource (see the header).
void SkipSet(uint8_t* shader, int64_t h, void* tex, void* aux, const uint64_t* size, Counters& c) {
  if (!tex) {  // slot 26 does nothing at all
    ++c.nullTex;
    return;
  }
  VSlot<Tex23Fn>(tex, 23)(tex, *size);
  uint8_t* t = static_cast<uint8_t*>(tex);
  if (*reinterpret_cast<void**>(t) == g_texVtblPtr) {
    void* inner = *reinterpret_cast<void**>(t + 0x10);
    void* iv = inner ? *static_cast<void**>(inner) : nullptr;
    if (iv && (iv == g_innerFile || iv == g_innerArray || iv == g_innerDummy)) {
      bool swap = false;
      if (reinterpret_cast<intptr_t>(aux) == -1 && *reinterpret_cast<void**>(t + 0x190) == nullptr) {
        uint8_t* back = *reinterpret_cast<uint8_t**>(t + 0x1a0);
        swap = !back || (reinterpret_cast<std::atomic<int32_t>*>(back + 0x40)->load(std::memory_order_acquire) == 5 &&
                         *reinterpret_cast<void**>(back + 0x20) != nullptr);
      }
      if (!swap) {
        ++c.skipped;
        return;
      }
    }
  }
  // Replay the rest of slot 26 without SetResource.
  ++c.skippedReplayed;
  uint8_t* rec = *reinterpret_cast<uint8_t**>(shader + 0xc8) + h * 0x50;
  const int32_t type = *reinterpret_cast<int32_t*>(rec + 0xc);
  const uint8_t* desc = g_getDesc(tex);
  if (!g_compat(type, *reinterpret_cast<const int32_t*>(desc + 0x20))) return;
  VSlot<Tex18Fn>(tex, 18)(tex);
  if (*reinterpret_cast<void**>(t + 0x678) == nullptr) {
    g_getSrv(tex, aux, size);
  } else if (type == 0x20 || type == 0x21) {
    const uint64_t zero = 0;
    g_getSrv(tex, aux, &zero);
  }
}

// Shadow batching (shadow_batch.h) groups textured casters by the texture
// sets the shadow passes read only. A skipped group member therefore still
// makes, for each texture set the mask skips, exactly what the slot-5 copy
// would have made for it (SkipSet: the streaming request and any replay).
// Its read sets equal its leader's by the group key, and the leader binds them.
// Returns the sets replayed. Render thread.
uint32_t ReplayMemberSkippedSets(uint8_t* mat, uint8_t* item, const MaskEntry* e) {
  if (!e || e->state <= 0) return 0;
  uint8_t* shader = *reinterpret_cast<uint8_t**>(mat + 0x30);
  uint8_t* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  if (!shader || !props) return 0;
  auto** arr = *reinterpret_cast<uint8_t***>(item + 0x18);
  if (!arr || !*arr) return 0;
  Counters& c = *MyCounters();
  uint64_t size = ~0ull;  // Vec2i(-1, -1), as slot 5
  uint8_t* entry = *arr + static_cast<uint64_t>(*reinterpret_cast<uint32_t*>(props + 0x26c)) * 24;
  uint32_t n = 0;
  for (uint64_t i = 0; i < *reinterpret_cast<uint32_t*>(mat + 0x2d8); ++i, entry += 24) {
    const int64_t h = *reinterpret_cast<int64_t*>(mat + 0x240 + i * 8);
    if (h == -1 || !Skippable(*e, h)) continue;
    SkipSet(shader, h, *reinterpret_cast<void**>(entry + 8), *reinterpret_cast<void**>(entry), &size, c);
    ++n;
  }
  return n;
}

// Exact copy of ModelMaterialMT slot 5 (0x17750) with the texture loop
// filtered by the mask. noMask: the shader has no usable mask (e skips
// nothing); its sets are counted as keptNoMask. pso != nullptr: the value
// slot 5 would read from [item+0xd4] (shadow batching's leaders, DrawPso).
uint64_t Draw(uint8_t* mat, uint8_t* item, void* a3, uint8_t* shader, const MaskEntry& e, Counters& c,
              bool noMask = false, const uint32_t* pso = nullptr) {
  *reinterpret_cast<uint32_t*>(mat + 0x18c) = pso ? *pso : *reinterpret_cast<uint32_t*>(item + 0xd4);
  uint8_t* globals = *reinterpret_cast<uint8_t**>(g_ng + g_at.globals);
  uint8_t* sb = *reinterpret_cast<uint8_t**>(globals + 0x78 + 8) + 0x20 +
                static_cast<uint64_t>(*reinterpret_cast<uint32_t*>(item + 0xd0)) * 0x30;
  void* sh = *reinterpret_cast<void**>(mat + 0x30);
  VSlot<SetSbFn>(sh, 27)(sh, *reinterpret_cast<void**>(mat + 0x68), *reinterpret_cast<void**>(sb));
  uint8_t* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  const uint8_t transparent = props[0x33];
  auto getTexture = *reinterpret_cast<GetTextureFn*>(g_ng + g_at.iatGetTexture);
  auto valid = *reinterpret_cast<ValidFn*>(g_ng + g_at.iatValid);
  bool alpha = valid(getTexture(props, 0xf));
  if (!alpha) alpha = valid(getTexture(props, 0x12));
  props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  if (*reinterpret_cast<int32_t*>(props + 8) != 0 || alpha) {
    uint64_t size = ~0ull;  // Vec2i(-1, -1)
    uint8_t* entry = **reinterpret_cast<uint8_t***>(item + 0x18) +
                     static_cast<uint64_t>(*reinterpret_cast<uint32_t*>(props + 0x26c)) * 24;
    for (uint64_t i = 0; i < *reinterpret_cast<uint32_t*>(mat + 0x2d8); ++i, entry += 24) {
      const int64_t h = *reinterpret_cast<int64_t*>(mat + 0x240 + i * 8);
      if (h == -1) continue;
      void* s = *reinterpret_cast<void**>(mat + 0x30);
      void* tex = *reinterpret_cast<void**>(entry + 8);
      void* aux = *reinterpret_cast<void**>(entry);
      if (s == shader && Skippable(e, h)) {
        SkipSet(shader, h, tex, aux, &size, c);
      } else {
        ++(noMask ? c.keptNoMask : c.kept);
        VSlot<SetTexFn>(s, 26)(s, h, tex, aux, &size);
      }
    }
  }
  const uint64_t tech = *reinterpret_cast<uint64_t*>(mat + (transparent ? 0x218 : 0x210));
  return g_submit(mat, tech, 0, a3);
}

// DCS's own slot 5, or with pso set the copy with every set kept, which
// makes the same calls (offline differential test) with posStructOffset =
// *pso. Its texture sets are not counted (DCS's slot 5 is not either).
uint64_t OrigOrCopy(void* matp, void* itemp, void* a3, void* a4, const uint32_t* pso) {
  if (!pso) return g_orig(matp, itemp, a3, a4);
  static const MaskEntry kKeepAll;
  thread_local Counters uncounted;
  uint8_t* mat = static_cast<uint8_t*>(matp);
  return Draw(mat, static_cast<uint8_t*>(itemp), a3, *reinterpret_cast<uint8_t**>(mat + 0x30), kKeepAll, uncounted,
              true, pso);
}

// The hook's body; pso: see Draw.
uint64_t HookPso(void* matp, void* itemp, void* a3, void* a4, const uint32_t* pso) {
  if (!g_on.load(std::memory_order_relaxed)) return OrigOrCopy(matp, itemp, a3, a4, pso);
  uint8_t* mat = static_cast<uint8_t*>(matp);
  uint8_t* shader = *reinterpret_cast<uint8_t**>(mat + 0x30);
  Counters& c = *MyCounters();
  ++c.casters;
  if (!shader || *reinterpret_cast<void**>(shader) != g_shaderVtblPtr) {
    ++c.fallback;
    return OrigOrCopy(matp, itemp, a3, a4, pso);
  }
  bool pending = false;
  const MaskEntry* e =
      Lookup(shader, *reinterpret_cast<uint64_t*>(mat + 0x210), *reinterpret_cast<uint64_t*>(mat + 0x218), &pending);
  if (!e || e->state <= 0) {
    // No mask (yet): the copy with every set kept (Skippable is false for a
    // state-0 entry), so the counters see these sets too.
    static const MaskEntry kKeepAll;
    ++(pending ? c.pending : c.unmapped);
    return Draw(mat, static_cast<uint8_t*>(itemp), a3, shader, kKeepAll, c, true, pso);
  }
  return Draw(mat, static_cast<uint8_t*>(itemp), a3, shader, *e, c, false, pso);
}

uint64_t __fastcall Hook(void* matp, void* itemp, void* a3, void* a4) { return HookPso(matp, itemp, a3, a4, nullptr); }

// Slot 5 for shadow batching's leaders (shadow_batch.h LeaderRaw) without
// touching [item+0xd4]: what the slot would run now, with posStructOffset =
// pso. Slot5Mode (just before the call) tells what the slot holds: this hook
// (directly or behind the shadow caster counter, binder_count.h), whose path
// DrawPso takes with pso; DCS's slot 5, which the copy with every set kept
// reproduces; or anything else (kPsoNone: the caller must not use DrawPso).
// SMR vt[1] passes (material, item, mesh) [V NGModel 0x443e0]; a4 is not
// read by slot 5 [V 0x17750]. The counter's own per-call count misses
// leaders (measurement only).
enum : int { kPsoNone = -1, kPsoHook = 0, kPsoCopy = 1 };
int Slot5Mode() {
  if (g_state.load() != 1 || !g_slot) return kPsoNone;
  void* cur = *g_slot;
  if (cur == reinterpret_cast<void*>(&Hook)) return kPsoHook;
  if (cur == reinterpret_cast<void*>(&shadowcount::HookModel))
    return shadowcount::g_modelNext.load() == &Hook ? kPsoHook : kPsoCopy;
  if (cur == reinterpret_cast<void*>(g_orig)) return kPsoCopy;
  return kPsoNone;
}

uint64_t DrawPso(void* mat, void* item, void* mesh, uint32_t pso, int mode) {
  return mode == kPsoHook ? HookPso(mat, item, mesh, nullptr, &pso) : OrigOrCopy(mat, item, mesh, nullptr, &pso);
}

void* __fastcall HookDtor(void* self, uint32_t flags) {
  if (g_cache) {
    g_cache->Invalidate(self);
    g_cacheFull = false;
  }
  shadowinst::ForgetShader(self);  // its texture-read entries and seen mark
  return g_origDtor(self, flags);
}

// ---------------------------------------------------------------------------
// Install / attach
// ---------------------------------------------------------------------------

// False when a location is not found in this build (reloc.h logged which).
bool ResolveAddrs(Addrs& a) {
  a.model = reloc::Rva(hooksig::NG_ModelMaterialMT_vtbl, kModelVtbl);
  a.slot5 = reloc::Rva(hooksig::NG_ModelMaterialMT_drawShadow, kSlot5);
  a.submit = reloc::Rva(hooksig::NG_ModelMaterialMT_submit, kSubmit);
  a.globals = reloc::Rva(hooksig::NG_model_globals, kGlobals);
  a.iatGetTexture = reloc::Rva(hooksig::NG_iat_PropertiesSet_getTexture, kIatGetTexture);
  a.iatValid = reloc::Rva(hooksig::NG_iat_Texture2dProperties_valid, kIatValid);
  a.shader = reloc::Rva(hooksig::DX_DX11Shader_vtbl, kShaderVtbl);
  a.setTexture = reloc::Rva(hooksig::DX_DX11Shader_setTexture, kSetTexture);
  a.shaderDtor = reloc::Rva(hooksig::DX_DX11Shader_dtor, 0x1d3c0);
  a.getDesc = reloc::Rva(hooksig::DX_DX11Texture_getDesc, kGetDesc);
  a.compat = reloc::Rva(hooksig::DX_DX11Texture_compatible, kCompat);
  a.getSrv = reloc::Rva(hooksig::DX_DX11Texture_getSRV, kGetSrv);
  a.tex = reloc::Rva(hooksig::DX_DX11Texture_vtbl, kTexVtbl);
  a.texSlot18 = reloc::Rva(hooksig::DX_DX11Texture_slot18, 0x120a0);
  a.texSlot11 = reloc::Rva(hooksig::DX_DX11Texture_slot11, 0x11190);
  a.innerFile = reloc::Rva(hooksig::DX_DX11TextureFromFileInternalImpl_vtbl, kInnerFile);
  a.innerArray = reloc::Rva(hooksig::DX_DX11TextureArrayFromFileInternalImpl_vtbl, kInnerArray);
  a.innerDummy = reloc::Rva(hooksig::DX_DX11TextureInternalDummy_vtbl, kInnerDummy);
  a.innerSlot4 = reloc::Rva(hooksig::DX_DX11TextureFromFileInternalImpl_slot4, 0x331f0);
  a.dummySlot4 = reloc::Rva(hooksig::DX_DX11TextureInternalDummy_slot4, 0x5250);
  a.ret0 = reloc::Rva(hooksig::DX_ret0_stub, 0x5010);
  a.tech = reloc::Rva(hooksig::DX_STechnique_vtbl, kTechVtbl);
  a.pass = reloc::Rva(hooksig::DX_SPassBlock_vtbl, kPassVtbl);
  static_assert(sizeof(Addrs) == 23 * sizeof(uint32_t), "Addrs holds RVAs only");
  const uint32_t* v = &a.model;
  for (size_t i = 0; i < sizeof(Addrs) / sizeof(uint32_t); ++i)
    if (!v[i]) return false;
  return true;
}

// Returns nullptr when NGModel and dx11backend are the analysed build (or
// another build with the same code, see reloc.h), else the log line saying
// what does not match. Sets g_at.
const char* VerifyBuild(uint8_t* ng, uint8_t* dx) {
  Addrs& a = g_at;
  if (!ResolveAddrs(a)) return "shadow texture skip: a NGModel/dx11backend location is not in this build; skipped";
  auto** model = reinterpret_cast<void**>(ng + a.model);
  auto** shaderVtbl = reinterpret_cast<void**>(dx + a.shader);
  auto** texVtbl = reinterpret_cast<void**>(dx + a.tex);
  if (!allocslab::RttiIs(ng, model, ".?AVModelMaterialMT@model@@") ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&model[5])) != ng + a.slot5 ||
      !reloc::CodeIs(hooksig::NG_ModelMaterialMT_drawShadow, ng, kSlot5, kSlot5End, kSlot5Hash) ||
      !reloc::CodeIs(hooksig::NG_ModelMaterialMT_submit, ng, kSubmit, kSubmitEnd, kSubmitHash))
    return "shadow texture skip: NGModel ModelMaterialMT does not match this build; skipped";
  if (!allocslab::RttiIs(dx, shaderVtbl, ".?AVDX11Shader@RenderAPI@@") ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&shaderVtbl[26])) != dx + a.setTexture ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&shaderVtbl[0])) != dx + a.shaderDtor ||
      !reloc::CodeIs(hooksig::DX_DX11Shader_setTexture, dx, kSetTexture, kSetTextureEnd, kSetTextureHash) ||
      !reloc::CodeIs(hooksig::DX_DX11Texture_getDesc, dx, kGetDesc, kGetDescEnd, kGetDescHash) ||
      !reloc::CodeIs(hooksig::DX_DX11Texture_compatible, dx, kCompat, kCompatEnd, kCompatHash) ||
      !reloc::CodeIs(hooksig::DX_DX11Texture_getSRV, dx, kGetSrv, kGetSrvEnd, kGetSrvHash) ||
      !allocslab::RttiIs(dx, texVtbl, ".?AVDX11Texture@RenderAPI@@") ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&texVtbl[18])) != dx + a.texSlot18 ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&texVtbl[11])) != dx + a.texSlot11 ||
      !allocslab::RttiIs(dx, reinterpret_cast<void**>(dx + a.innerFile), ".?AVDX11TextureFromFileInternalImpl@RenderAPI@@") ||
      !allocslab::RttiIs(dx, reinterpret_cast<void**>(dx + a.innerArray),
                         ".?AVDX11TextureArrayFromFileInternalImpl@RenderAPI@@") ||
      !allocslab::RttiIs(dx, reinterpret_cast<void**>(dx + a.innerDummy), ".?AVDX11TextureInternalDummy@RenderAPI@@") ||
      !allocslab::RttiIs(dx, reinterpret_cast<void**>(dx + a.tech), ".?AUSTechnique@D3DX11Effects@@") ||
      !allocslab::RttiIs(dx, reinterpret_cast<void**>(dx + a.pass), ".?AUSPassBlock@D3DX11Effects@@"))
    return "shadow texture skip: dx11backend DX11Shader/DX11Texture/effects code does not match this build; skipped";
  // Inner texture classes: vt[8] must be `ret 0` and vt[4] the known getters.
  auto innerOk = [&](uint32_t vt, uint32_t slot4) {
    auto** v = reinterpret_cast<void**>(dx + vt);
    return reinterpret_cast<uint8_t*>(v[8]) == dx + a.ret0 && reinterpret_cast<uint8_t*>(v[4]) == dx + slot4;
  };
  uint8_t ret0[3] = {};
  if (!innerOk(a.innerFile, a.innerSlot4) || !innerOk(a.innerArray, a.innerSlot4) ||
      !innerOk(a.innerDummy, a.dummySlot4) || !allocslab::ReadBytes(dx + a.ret0, ret0, 3) || ret0[0] != 0xc2 ||
      ret0[1] != 0 || ret0[2] != 0)
    return "shadow texture skip: dx11backend texture classes do not match this build; skipped";
  return nullptr;
}

bool Install() {
  // Claimed atomically (-3 while installing): the worker, shadow batching's
  // install and a suite phase can race on the first call.
  int idle = 0;
  if (!g_state.compare_exchange_strong(idle, -3)) return g_state.load() > 0;
  auto* ng = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  if (!ng || !dx) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      Log("shadow texture skip: could not find %s yet; not installed (retried every second)",
          ng ? "dx11backend.dll" : "NGModel.dll");
    g_state = 0;
    return false;
  }
  if (const char* why = VerifyBuild(ng, dx)) {
    Log("%s", why);
    g_state = -1;
    return false;
  }
  auto** model = reinterpret_cast<void**>(ng + g_at.model);
  auto** shaderVtbl = reinterpret_cast<void**>(dx + g_at.shader);
  auto** texVtbl = reinterpret_cast<void**>(dx + g_at.tex);
  g_ng = ng;
  g_dx = dx;
  g_submit = reinterpret_cast<SubmitFn>(ng + g_at.submit);
  g_getDesc = reinterpret_cast<GetDescFn>(dx + g_at.getDesc);
  g_compat = reinterpret_cast<CompatFn>(dx + g_at.compat);
  g_getSrv = reinterpret_cast<GetSrvFn>(dx + g_at.getSrv);
  g_shaderVtblPtr = shaderVtbl;
  g_texVtblPtr = texVtbl;
  g_innerFile = dx + g_at.innerFile;
  g_innerArray = dx + g_at.innerArray;
  g_innerDummy = dx + g_at.innerDummy;
  g_cache = new MaskCache;
  g_orig = reinterpret_cast<Slot5Fn>(SlotOriginal(&model[5]));
  g_origDtor = reinterpret_cast<DtorFn>(SlotOriginal(&shaderVtbl[0]));
  g_slot = &model[5];
  // Destructor first: no cached shader may outlive its entry.
  if (!HookSlot(&shaderVtbl[0], reinterpret_cast<void*>(&HookDtor), nullptr)) {
    Log("shadow texture skip: could not patch the DX11Shader destructor; not installed");
    g_state = -1;
    return false;
  }
  g_state = 1;
  Log("shadow texture skip: installed (ModelMaterialMT slot 5; texture sets no shadow pass reads are skipped, "
      "masks from shadow inst's compile of each shader's key)");
  return true;
}

// Slot 5 is shared with the shadow caster counter (binder_count.h): when that
// counter owns the slot, it forwards to this hook instead.
void SetAttached(bool on) {
  if (g_state.load() <= 0) return;
  shadowcount::g_modelNext.store(on ? &Hook : nullptr);
  if (on == g_attached.load()) return;
  if (!shadowcount::g_origModel) {
    if (on)
      HookSlot(g_slot, reinterpret_cast<void*>(&Hook), nullptr);
    else
      UnhookSlot(g_slot, reinterpret_cast<void*>(g_orig));
  }
  g_attached = on;
}

using Totals = Counters;
Totals Snapshot() {
  Totals t;
  std::lock_guard<std::mutex> lock(g_regMutex);
  for (Counters* c : g_counters) {
    t.casters += c->casters;
    t.fallback += c->fallback;
    t.pending += c->pending;
    t.unmapped += c->unmapped;
    t.kept += c->kept;
    t.keptNoMask += c->keptNoMask;
    t.skipped += c->skipped;
    t.skippedReplayed += c->skippedReplayed;
    t.nullTex += c->nullTex;
  }
  return t;
}

// Per-frame figures between two snapshots (the [Suite] BenchShadowTex counter
// phase). Texture sets: skipped (any of the three skip paths) vs kept (by the
// mask, plus every set of casters whose shader has no mask yet).
void Report(const Totals& a, const Totals& b, uint64_t frames) {
  if (!frames) return;
  const double f = static_cast<double>(frames);
  const uint64_t skipped = (b.skipped - a.skipped) + (b.skippedReplayed - a.skippedReplayed) + (b.nullTex - a.nullTex);
  const uint64_t keptMask = b.kept - a.kept, keptNoMask = b.keptNoMask - a.keptNoMask;
  const uint64_t kept = keptMask + keptNoMask;
  Log("  shadow casters through the hook %.0f/frame: %.0f/frame with a mask, %.0f/frame waiting for their key's "
      "compile, %.0f/frame on shaders kept whole (not mappable), %.0f/frame not a DX11Shader (DCS's own slot 5)",
      (b.casters - a.casters) / f,
      (b.casters - a.casters - (b.pending - a.pending) - (b.unmapped - a.unmapped) - (b.fallback - a.fallback)) / f,
      (b.pending - a.pending) / f, (b.unmapped - a.unmapped) / f, (b.fallback - a.fallback) / f);
  Log("  shadow texture sets per frame: skipped %.0f, kept %.0f (%.1f%% skipped); kept by the mask %.0f, on casters "
      "without a mask %.0f; of the skipped, %.1f%% replayed the streaming bookkeeping, %.1f%% had no texture",
      skipped / f, kept / f, skipped + kept ? 100.0 * skipped / (skipped + kept) : 0.0, keptMask / f, keptNoMask / f,
      skipped ? 100.0 * (b.skippedReplayed - a.skippedReplayed) / skipped : 0.0,
      skipped ? 100.0 * (b.nullTex - a.nullTex) / skipped : 0.0);
  Log("  shadow texture masks: %u shaders mapped from shadow_inst's compiles, %u kept whole (not mappable), cache "
      "%zu entries, %zu invalidated",
      g_masksBuilt.load() - g_masksKept.load(), g_masksKept.load(), g_cache ? g_cache->Used() : size_t{0},
      g_cache ? g_cache->Invalidated() : size_t{0});
}

}  // namespace shadowtex
