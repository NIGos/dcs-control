// Binaural audio: ed_sound.dll's hook sites (docs/BINAURAL_AUDIO_HOOKS.md sections 0-2, 6), found, validated and
// patched.
//
// Resolve() runs every check of the hook spec's section 6 against a mapped image before anything is written:
//   1. identity: the analysed build (timestamp and size of src/audio_sigs.h) uses the recorded RVAs when the pattern
//      matches there; any other build looks each location up by its unique pattern;
//   2. every location's whole function hashes the same with addresses masked (audio_sigs.h);
//   3. each call site calls the expected function (P3a/P3b the panner, P4 the voice render, P5 the mix), or a thunk of
//      an earlier payload generation whose recorded original is that function;
//   4. the panner's per-channel stores and channel bound, the voice render's kill flag and snapshot copy, the WASAPI
//      overrun counter next to P5;
//   5. SND_process's ed::mutex lock (lea rcx, [mutex]; call [lock]) resolves to edCore's ?lock@mutex@ed@@QEAAXXZ and an
//      unlock in the same function uses the same mutex; the frame counter, engine state and context array are found
//      by their patterns inside SND_process;
//   6. the sites can be patched atomically: P4/P5's rel32 4-byte aligned, P3a/P3b's call inside one aligned 8 bytes.
// Any failure: nothing is patched, one reason.
//
// Patching: each site's new target is a thunk (mov rax, imm64; jmp rax) in a page within +-2 GB of ed_sound, one page
// per payload generation, never freed (a hot reload can never leave a site pointing at freed code; old payloads stay
// mapped, loader_api.h). P4 and P5: one aligned 32-bit store of the rel32 (safe while the render thread runs). P3a and
// P3b: one aligned 64-bit store, with ed_sound's own mutex held (SND_process holds it around the whole panner loop, so
// no thread is inside those bytes). Restore writes the original rel32 the same way.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "audio_sigs.h"
#include "audio_voice.h"
#include "sigscan.h"

namespace audhk {

enum Site : int { kP3a = 0, kP3b, kP4, kP5, kSiteCount };

struct SigTable {
  const audsig::Loc* locs;   // audsig::kCount rows, in audsig::Id order
  const audsig::Mask* masks;
  uint32_t timestamp, sizeOfImage;
};
inline SigTable Generated() { return {audsig::kLocs, audsig::kMasks, audsig::kTimestamp, audsig::kSizeOfImage}; }

// edCore's ed::mutex lock / unlock (tests substitute their own).
using ImportFn = void* (*)(const char* dll, const char* name);
inline void* DefaultImport(const char* dll, const char* name) {
  HMODULE m = GetModuleHandleA(dll);
  return m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
}
constexpr const char* kEdCore = "edCore.dll";
constexpr const char* kLockName = "?lock@mutex@ed@@QEAAXXZ";
constexpr const char* kUnlockName = "?unlock@mutex@ed@@QEAAXXZ";

using MutexFn = void (*)(void* mutex);
using PannerFn = void (*)(void* voice);
using VoiceRenderFn = int (*)(void* voice, float* out, uint32_t frames);
using MixFn = void (*)(void* renderer, float* out, uint32_t frames);

struct Resolved {
  uint8_t* base = nullptr;
  bool analysedBuild = false;
  uint8_t* site[kSiteCount] = {};
  void* expected[kSiteCount] = {};    // the function each site calls in stock DCS
  void* current[kSiteCount] = {};     // what it calls now (expected, or an earlier generation's thunk)
  PannerFn panner = nullptr;
  VoiceRenderFn voiceRender = nullptr;
  MixFn mix = nullptr;
  void* mutex = nullptr;
  MutexFn lock = nullptr, unlock = nullptr;
  volatile uint32_t* frameCounter = nullptr;
  volatile int32_t* engineState = nullptr;
  uint8_t* ctxArray = nullptr;
  volatile int32_t* ctxCount = nullptr;
  char why[200] = {};
  // Cockpit space (docs/COCKPIT_SPACE.md 3.1-3.2; the optional rows of audio_sigs.h): ed_sound's use-cockpit-gain flag
  // (ctx+0xe1), the host's allow flag (sound+0x1a8) and the applied cockpit_gain (sound+0x1b0) are read only with
  // spaceGain; a sound's listener-frame orientation (sound+0xe8) only with spaceOrient. A mismatch turns those reads off
  // (the space falls back to the camera check and has no aircraft frame), never binaural.
  bool spaceGain = false, spaceOrient = false;
  char spaceWhy[200] = {};
};

// ---- thunk pages ----------------------------------------------------------------------------------------------------
constexpr char kThunkMagic[16] = {'D', 'c', 's', 'Q', 'v', 'A', 'u', 'd', 'i', 'o', 'T', 'h', 'u', 'n', 'k', '1'};
constexpr uint32_t kThunkCodeAt = 0x100, kThunkStride = 16;
struct ThunkHeader {
  char magic[16];
  void* original[kSiteCount];  // the function each site called before any payload
  void* wrapper[kSiteCount];
};

inline bool ReadMem(const void* p, void* out, size_t n) {
  __try {
    memcpy(out, p, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The thunk `target` is one of ours (any generation) for site k: its recorded original.
inline void* ThunkOriginal(const uint8_t* target, int k) {
  const uint8_t* page = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(target) & ~uintptr_t(0xFFF));
  if (target != page + kThunkCodeAt + kThunkStride * k) return nullptr;
  ThunkHeader h;
  if (!ReadMem(page, &h, sizeof(h)) || memcmp(h.magic, kThunkMagic, 16) != 0) return nullptr;
  return h.original[k];
}

// The page protection call (tests substitute a failing one).
using ProtectFn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
inline ProtectFn g_thunkProtect = &VirtualProtect;

// Allocates the page (within rel32 reach of `near`), writes the header and four thunks, makes it execute-read. Null
// with a reason when there is no page in reach or it cannot be made executable (a site pointing at a non-executable
// page would crash DCS at its next call: DEP).
inline uint8_t* MakeThunks(uint8_t* nearAddr, void* const original[kSiteCount], void* const wrapper[kSiteCount],
                           const char** why = nullptr) {
  if (why) *why = "no memory within reach of ed_sound.dll";
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const uintptr_t gran = si.dwAllocationGranularity, start = reinterpret_cast<uintptr_t>(nearAddr);
  uint8_t* page = nullptr;
  for (uintptr_t o = gran; o < (1ull << 30) && !page; o += gran)
    for (int dir = -1; dir <= 1 && !page; dir += 2) {
      const uintptr_t a = (dir < 0 ? start - o : start + o) & ~(gran - 1);
      page = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(a), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
  if (!page) return nullptr;
  memset(page, 0xCC, 4096);
  ThunkHeader h{};
  memcpy(h.magic, kThunkMagic, 16);
  for (int k = 0; k < kSiteCount; ++k) {
    h.original[k] = original[k];
    h.wrapper[k] = wrapper[k];
    uint8_t* c = page + kThunkCodeAt + kThunkStride * k;
    c[0] = 0x48;
    c[1] = 0xB8;  // mov rax, imm64
    memcpy(c + 2, &wrapper[k], 8);
    c[10] = 0xFF;
    c[11] = 0xE0;  // jmp rax
  }
  memcpy(page, &h, sizeof(h));
  DWORD old;
  if (!g_thunkProtect(page, 4096, PAGE_EXECUTE_READ, &old)) {
    VirtualFree(page, 0, MEM_RELEASE);  // nothing points at it yet
    if (why) *why = "the call thunks could not be made executable";
    return nullptr;
  }
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  return page;
}
inline uint8_t* ThunkOf(uint8_t* page, int k) { return page + kThunkCodeAt + kThunkStride * k; }

// ---- resolution -----------------------------------------------------------------------------------------------------
inline uint64_t Fnv(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

// Masked FNV of loc's function at its resolved location. False when out of the image or unreadable.
inline bool MaskedHash(const uint8_t* base, const SigTable& t, const audsig::Loc& l, const uint8_t* at, uint64_t* out) {
  const uint8_t* p = at + l.fnAt;
  if (l.fnLen > 65536 || !sigscan::InImage(base, p, l.fnLen)) return false;
  static uint8_t buf[65536];  // Resolve runs on one thread at a time (the control thread, or a test)
  if (!ReadMem(p, buf, l.fnLen)) return false;
  for (uint32_t i = 0; i < l.maskCount; ++i) {
    const audsig::Mask& m = t.masks[l.maskFirst + i];
    for (uint32_t k = m.at; k < static_cast<uint32_t>(m.at) + m.len && k < l.fnLen; ++k) buf[k] = 0;
  }
  *out = Fnv(buf, l.fnLen);
  return true;
}

inline bool InCode(const uint8_t* base, const uint8_t* p, size_t n) {
  bool in = false;
  sigscan::ForEachCodeSection(base, [&](sigscan::Range r) { in |= p >= r.begin && p + n <= r.end; });
  return in;
}

inline bool Fail(Resolved* r, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r->why, sizeof(r->why), fmt, ap);
  va_end(ap);
  return false;
}

// One location: recorded RVA (analysed build, pattern there) or the unique pattern match; then the function hash.
inline uint8_t* Locate(const uint8_t* base, const SigTable& t, int id, bool analysed, Resolved* r) {
  const audsig::Loc& l = t.locs[id];
  sigscan::Pattern pat;
  if (!sigscan::Parse(l.pattern, pat)) return Fail(r, "%s not found (bad pattern)", l.id), nullptr;
  uint8_t* at = nullptr;
  if (analysed) {
    const uint8_t* p = base + l.rva - l.patOffset;
    uint8_t tmp[64];
    if (pat.size() <= sizeof(tmp) && InCode(base, p, pat.size()) && ReadMem(p, tmp, pat.size()) && sigscan::MatchAt(tmp, pat))
      at = const_cast<uint8_t*>(base) + l.rva;
  }
  if (!at) {
    int n = 0;
    at = sigscan::FindUnique(base, l.pattern, l.patOffset, &n);
    if (!at) return Fail(r, "%s not found (%s)", l.id, n > 1 ? "pattern not unique" : "pattern not found"), nullptr;
  }
  uint64_t h = 0;
  if (!MaskedHash(base, t, l, at, &h) || h != l.fnHash)
    return Fail(r, "%s not found (code differs: masked hash of its function)", l.id), nullptr;
  return at;
}

// Unique match of `pattern` inside [fn, fn + len).
inline const uint8_t* FindIn(const uint8_t* fn, uint32_t len, const char* pattern) {
  sigscan::Pattern pat;
  if (!sigscan::Parse(pattern, pat)) return nullptr;
  const uint8_t* hit[2];
  const size_t n = sigscan::FindAll(sigscan::Range{fn, fn + len}, pat, hit, 2, 2);
  return n == 1 ? hit[0] : nullptr;
}

inline bool BytesAt(const uint8_t* p, const char* pattern) {
  sigscan::Pattern pat;
  uint8_t tmp[64];
  return sigscan::Parse(pattern, pat) && pat.size() <= sizeof(tmp) && ReadMem(p, tmp, pat.size()) && sigscan::MatchAt(tmp, pat);
}

inline bool InData(const uint8_t* base, const void* p, size_t n) {
  return sigscan::InImage(base, p, n) && !InCode(base, static_cast<const uint8_t*>(p), n);
}

// The optional rows (cockpit space): each found and hashed like the required ones; the orientation also needs
// lea rdi, [rcx+0xd0] earlier in its function (rdi + 0x18 = sound+0xe8). Sets spaceGain / spaceOrient and, when one is
// missing, spaceWhy. Never fails Resolve.
inline void ResolveSpace(const uint8_t* base, const SigTable& t, Resolved* r) {
  static Resolved tmp;  // Resolve runs on one thread at a time (as MaskedHash's buffer)
  tmp.why[0] = 0;
  r->spaceGain = Locate(base, t, audsig::kSpaceGain, r->analysedBuild, &tmp) != nullptr;
  if (!r->spaceGain) snprintf(r->spaceWhy, sizeof(r->spaceWhy), "%s", tmp.why);
  tmp.why[0] = 0;
  const uint8_t* o = Locate(base, t, audsig::kSpaceOrient, r->analysedBuild, &tmp);
  if (o && !BytesAt(o - 0x46, "48 8D B9 D0 00 00 00")) {
    o = nullptr;
    snprintf(tmp.why, sizeof(tmp.why), "SpaceOrient not found (the sound's listener-frame fields differ)");
  }
  r->spaceOrient = o != nullptr;
  if (!r->spaceOrient && !r->spaceWhy[0]) snprintf(r->spaceWhy, sizeof(r->spaceWhy), "%s", tmp.why);
}

inline bool Resolve(const uint8_t* base, const SigTable& t, ImportFn imports, Resolved* r) {
  *r = Resolved{};
  r->base = const_cast<uint8_t*>(base);
  const IMAGE_NT_HEADERS64* nt = sigscan::NtHeaders(base);
  if (!nt) return Fail(r, "ed_sound.dll not found (no PE image)");
  r->analysedBuild = nt->FileHeader.TimeDateStamp == t.timestamp && nt->OptionalHeader.SizeOfImage == t.sizeOfImage;
  uint8_t* loc[audsig::kCount] = {};
  for (int i = 0; i < audsig::kRequired; ++i)
    if (!(loc[i] = Locate(base, t, i, r->analysedBuild, r))) return false;
  r->panner = reinterpret_cast<PannerFn>(loc[audsig::kPanner]);
  r->voiceRender = reinterpret_cast<VoiceRenderFn>(loc[audsig::kVoiceRender]);
  r->mix = reinterpret_cast<MixFn>(loc[audsig::kMix]);
  // 3. Call targets.
  const int siteLoc[kSiteCount] = {audsig::kP3a, audsig::kP3b, audsig::kP4, audsig::kP5};
  void* const want[kSiteCount] = {loc[audsig::kPanner], loc[audsig::kPanner], loc[audsig::kVoiceRender],
                                  loc[audsig::kMix]};
  const char* names[kSiteCount] = {"P3a", "P3b", "P4", "P5"};
  for (int k = 0; k < kSiteCount; ++k) {
    uint8_t* s = loc[siteLoc[k]];
    uint8_t insn[5];
    if (!ReadMem(s, insn, 5) || insn[0] != 0xE8) return Fail(r, "%s not found (no call at the site)", names[k]);
    int32_t rel;
    memcpy(&rel, insn + 1, 4);
    uint8_t* target = s + 5 + rel;
    if (target != want[k] && ThunkOriginal(target, k) != want[k])
      return Fail(r, "%s not found (the call goes to neither %s nor a thunk of ours)", names[k],
                  k < 2 ? "the panner" : (k == 2 ? "the voice render" : "the mix"));
    r->site[k] = s;
    r->expected[k] = want[k];
    r->current[k] = target;
  }
  // 6. Atomic patch rules.
  for (int k = 0; k < 2; ++k)
    if ((reinterpret_cast<uintptr_t>(r->site[k]) & 7) > 3) return Fail(r, "%s not found (call not inside one aligned 8 bytes)", names[k]);
  for (int k = 2; k < 4; ++k)
    if ((reinterpret_cast<uintptr_t>(r->site[k]) + 1) & 3) return Fail(r, "%s not found (rel32 not 4-byte aligned)", names[k]);
  // 4. Layouts.
  const uint8_t* pan = loc[audsig::kPanner];
  if (!BytesAt(pan + 0x263, "F3 42 0F 11 44 FD 48") || !BytesAt(pan + 0x28c, "F3 42 0F 11 44 FD 48") ||
      !BytesAt(pan + 0x29c, "F3 42 0F 11 74 FD 4C") || !BytesAt(pan + 0x2a6, "44 3B BE 74 22 00 00"))
    return Fail(r, "panner not found (per-channel gain stores or channel bound differ)");
  // The sound's auto_width flag (sound+0x1a6, read for stereo waves): cmp byte [rbx+0x1a6], 0 at 0x1c5ee.
  if (!BytesAt(pan + 0x42e, "80 BB A6 01 00 00 00")) return Fail(r, "panner not found (the sound's auto_width flag differs)");
  const uint8_t* vr = loc[audsig::kVoiceRender];
  if (!BytesAt(vr + 6, "80 B9 C0 0E 00 00 00") || !BytesAt(vr + 0x10, "0F 10 41 20"))
    return Fail(r, "voice render not found (kill flag or snapshot copy differ)");
  if (!BytesAt(r->site[kP5] + 0x3e, "F0 FF 83 74 22 00 00"))
    return Fail(r, "P5 not found (ED's overrun counter is not where expected)");
  // 5. SND_process: mutex, frame counter, engine state, contexts.
  const audsig::Loc& l3 = t.locs[audsig::kP3a];
  const uint8_t* snd = loc[audsig::kP3a] + l3.fnAt;
  const uint32_t sndLen = l3.fnLen;
  if (!BytesAt(snd + 0x2c, "48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ??")) return Fail(r, "mutex not found (no lock call in SND_process)");
  uint8_t* mutex = sigscan::RipTarget(snd + 0x2c, 3, 7);
  void** lockSlot = reinterpret_cast<void**>(sigscan::RipTarget(snd + 0x33, 2, 6));
  void* lockFn = nullptr;
  if (!InData(base, mutex, 8) || !sigscan::InImage(base, lockSlot, 8) || !ReadMem(lockSlot, &lockFn, 8))
    return Fail(r, "mutex not found (outside the image)");
  void* wantLock = imports ? imports(kEdCore, kLockName) : nullptr;
  void* wantUnlock = imports ? imports(kEdCore, kUnlockName) : nullptr;
  if (!wantLock || lockFn != wantLock) return Fail(r, "mutex not found (the lock call is not edCore's ed::mutex::lock)");
  {  // an unlock of the same mutex in SND_process
    sigscan::Pattern pat;
    sigscan::Parse("48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ??", pat);
    const uint8_t* hit[8];
    const size_t n = sigscan::FindAll(sigscan::Range{snd, snd + sndLen}, pat, hit, 8, 8);
    bool unlockOk = false;
    for (size_t i = 0; i < n && i < 8; ++i) {
      if (sigscan::RipTarget(hit[i], 3, 7) != mutex) continue;
      void** slot = reinterpret_cast<void**>(sigscan::RipTarget(hit[i] + 7, 2, 6));
      void* fn = nullptr;
      if (sigscan::InImage(base, slot, 8) && ReadMem(slot, &fn, 8) && fn && fn == wantUnlock) unlockOk = true;
    }
    if (!unlockOk) return Fail(r, "mutex not found (no unlock of the same mutex in SND_process)");
  }
  r->mutex = mutex;
  r->lock = reinterpret_cast<MutexFn>(wantLock);
  r->unlock = reinterpret_cast<MutexFn>(wantUnlock);
  const uint8_t* fc = FindIn(snd, sndLen, "FF 05 ?? ?? ?? ?? 8B C6 89 05");
  const uint8_t* es = FindIn(snd, sndLen, "83 3D ?? ?? ?? ?? 01 0F 8E");
  const uint8_t* cx = FindIn(snd, sndLen, "4C 8D 2D ?? ?? ?? ?? F2 44 0F 10 05 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 85 C0");
  if (!fc || !es || !cx) return Fail(r, "SND_process globals not found (frame counter, state or contexts)");
  r->frameCounter = reinterpret_cast<volatile uint32_t*>(sigscan::RipTarget(fc, 2, 6));
  r->engineState = reinterpret_cast<volatile int32_t*>(sigscan::RipTarget(es, 2, 7));
  r->ctxArray = sigscan::RipTarget(cx, 3, 7);
  r->ctxCount = reinterpret_cast<volatile int32_t*>(sigscan::RipTarget(cx + 16, 2, 6));
  if (!InData(base, const_cast<uint32_t*>(r->frameCounter), 4) || !InData(base, const_cast<int32_t*>(r->engineState), 4) ||
      !InData(base, r->ctxArray, 8 * audv::off::kCtxStride) || !InData(base, const_cast<int32_t*>(r->ctxCount), 4))
    return Fail(r, "SND_process globals not found (outside the data sections)");
  ResolveSpace(base, t, r);
  return true;
}

// ---- patching -------------------------------------------------------------------------------------------------------
// Writes a call's rel32 so the site calls `target`. P4/P5: an aligned 32-bit store; P3a/P3b: an aligned 64-bit store
// (the caller holds ed_sound's mutex).
inline bool WriteCall(uint8_t* site, const void* target, bool eightByte) {
  const int64_t rel64 = static_cast<const uint8_t*>(target) - (site + 5);
  if (rel64 < INT32_MIN || rel64 > INT32_MAX) return false;
  const int32_t rel = static_cast<int32_t>(rel64);
  uint8_t* lo = eightByte ? reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(site) & ~uintptr_t(7)) : site + 1;
  DWORD old;
  if (!VirtualProtect(lo, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
  if (eightByte) {
    uint64_t w;
    memcpy(&w, lo, 8);
    uint8_t* b = reinterpret_cast<uint8_t*>(&w);
    const size_t o = static_cast<size_t>(site - lo);
    b[o] = 0xE8;
    memcpy(b + o + 1, &rel, 4);
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(lo), static_cast<LONG64>(w));
  } else {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(lo), static_cast<LONG>(rel));
  }
  DWORD tmp;
  VirtualProtect(lo, 8, old, &tmp);
  FlushInstructionCache(GetCurrentProcess(), lo, 8);
  return true;
}

inline void* CallTarget(const uint8_t* site) {
  uint8_t insn[5];
  if (!ReadMem(site, insn, 5) || insn[0] != 0xE8) return nullptr;
  int32_t rel;
  memcpy(&rel, insn + 1, 4);
  return const_cast<uint8_t*>(site) + 5 + rel;
}

}  // namespace audhk
