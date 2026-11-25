// A fake ed_sound.dll for the binaural audio tests: an executable image in memory with a PE header, the four call
// sites of docs/BINAURAL_AUDIO_HOOKS.md with ed_sound's own byte sequences at ed_sound's own RVAs, and small drivers
// around them; the panner, voice render and mix behind those sites are real code in the image that ends in (or, for
// the mix, loops around) C++ stand-ins with ed_sound's struct offsets:
//   - FakePanner: ed_sound's pan law (hook spec 2.1) for 3D voices, the 2D weights otherwise;
//   - FakeVoiceRender: snapshot copy, per-channel linear gain ramps from the last targets to the new ones, an ADDING
//     kernel (mono: s (a + b); stereo: sL a + sR b), returns 0 when the voice ended;
//   - the mix: machine code that walks renderer+0x20 and calls the voice render through the P4 site.
// The same C++ stand-ins called directly (never through the image) give the unhooked reference.
//
// Layout (RVAs, as ed_sound 2.9.31): .text 0x1000-0x2d000, .rdata 0x2d000-0x3a000 (ed::mutex lock / unlock slots at
// 0x2d4a0 / 0x2d4a8), .data 0x3a000-0x40000 (frame counter 0x3ad68, engine state 0x3ad70, context count 0x3ad74,
// mutex 0x3ad90, contexts 0x3d370).
//   SND_process  0x4980-0x4dc2: lock / state / contexts / frame counter sequences; driver at 0x4d00 runs P3a for one
//                sound (void drv(Sound** p)); the unlock at 0x4d87.
//   voice create 0x1b870-0x1bff2: driver at 0x1b9e0 runs P3b (void drv(Voice*, uint8_t ctx2d)).
//   mix          0x1b180-0x1b45a: prologue, a loop over the voices with the P4 site at 0x1b1fb.
//   voice render 0x165d0-0x16a9d: prologue, then a jump to FakeVoiceRender.
//   panner       0x1c1c0-0x1c6b9: prologue, then a jump to FakePanner; the layout bytes at +0x263/+0x28c/+0x29c/+0x2a6.
//   WASAPI loop  0x1ff00-0x20312: driver at 0x20160 calls the mix through P5 (void drv(Wasapi*, float* out, frames));
//                the overrun counter's lock inc at 0x201c1.
#pragma once

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fakeed {

constexpr uint32_t kImageSize = 0x40000;
constexpr uint32_t kTextBegin = 0x1000, kTextEnd = 0x2d000, kRdataBegin = 0x2d000, kDataBegin = 0x3a000;
constexpr uint32_t kLockSlot = 0x2d4a0, kUnlockSlot = 0x2d4a8;
constexpr uint32_t kFrameCounter = 0x3ad68, kEngineState = 0x3ad70, kCtxCount = 0x3ad74, kMutex = 0x3ad90,
                   kCtxArray = 0x3d370;
constexpr uint32_t kDrvP3a = 0x4d00, kDrvP3b = 0x1b9e0, kDrvP5 = 0x20160, kMixFn = 0x1b180;
constexpr uint32_t kWasapiSize = 0x2800, kSoundSize = 0x400, kVoiceSize = 0x1f10;
// test data inside a voice (bytes ed_sound's offsets never use here)
constexpr uint32_t kVPhase = 0x1800, kVStep = 0x1808, kVAmp = 0x1810, kVKind = 0x1814, kVRng = 0x1818,
                   kVRemaining = 0x1820, kVAlive = 0x1828, kVEnded = 0x182c;
constexpr uint32_t kSndCrosstalk = 0x300;  // test field: crosstalk x of the pan law
// Stereo waves: kSine plays sin(ph) left and sin(1.3 ph + 0.5) right, kNoise two independent noises (decorrelated
// channels), kDual the same sine on both (DCS's dual-mono stereo files), kDc the same DC on both, kDcSplit DC amp left
// and amp / 4 right (a side for click tests).
enum Kind : uint32_t { kSine = 0, kDc = 1, kNoise = 2, kDual = 3, kDcSplit = 4 };

using DrvP3a = void (*)(void** soundSlot);
using DrvP3b = void (*)(void* voice, uint8_t ctx2d);
using DrvP5 = void (*)(void* wasapi, float* out, uint32_t frames);
using MixFn = void (*)(void* renderer, float* out, uint32_t frames);

template <typename T>
inline T R(const void* p, uint32_t o) {
  T v;
  memcpy(&v, static_cast<const uint8_t*>(p) + o, sizeof(T));
  return v;
}
template <typename T>
inline void W(void* p, uint32_t o, T v) {
  memcpy(static_cast<uint8_t*>(p) + o, &v, sizeof(T));
}

// ---- the stand-ins behind the image's code (ed_sound's behaviour, ed_sound's offsets) -------------------------------
inline void FakePanner(void* v) {
  uint8_t* s = R<uint8_t*>(v, 0x10);
  const uint8_t* r = R<uint8_t*>(v, 0x08);
  if (!s || !r) return;
  // speed factor (ed_sound 0x1c208..0x1c2ae): s at +0x178, relative velocity at +0x108; single precision
  float sf = R<float>(s, 0x178);
  if (sf != 1.0f) {
    const double rv[3] = {R<double>(s, 0x108), R<double>(s, 0x110), R<double>(s, 0x118)};
    const double vel = std::sqrt(rv[0] * rv[0] + rv[1] * rv[1] + rv[2] * rv[2]);
    if (200.0 > vel) {
      const float vf = static_cast<float>(vel);
      if (!(vf > 0.0f)) sf = 1.0f;
      else if (vf < 200.0f) sf = sf * (vf / 200.0f) + (1.0f - vf / 200.0f);
    }
  }
  const double base = static_cast<float>(sf * R<float>(s, 0x1c0));
  const double x = R<float>(s, kSndCrosstalk);
  const bool is3d = R<uint64_t>(s, 0x18) != 0;
  double u[3];
  memcpy(u, s + 0x1d8, sizeof(u));
  const uint32_t chans = R<uint32_t>(r, 0x2274);
  for (uint32_t ch = 0; ch < chans && ch < 17; ++ch) {
    const uint8_t* spk = r + 0x2278 + 0x30 * ch;
    float a, b;
    if (is3d && !R<uint8_t>(spk, 0x28)) {
      const double c = -(u[0] * R<double>(spk, 0) + u[1] * R<double>(spk, 8) + u[2] * R<double>(spk, 16));
      const double p = (0.4 - 0.35 * x) * (1.0 + c) + (0.2 + 0.7 * x);
      a = static_cast<float>(R<float>(spk, 0x20) * p * base);
      b = static_cast<float>(R<float>(spk, 0x24) * p * base);
    } else {
      a = static_cast<float>(base * R<float>(spk, 0x18));
      b = static_cast<float>(base * R<float>(spk, 0x1c));
    }
    W<float>(v, 0x48 + 8 * ch, a);
    W<float>(v, 0x4c + 8 * ch, b);
  }
  // A stereo wave's width (panner 0x1c5ee): 1, or with auto_width (sound+0x1a6) 1 within inner_radius (sound+0x144),
  // falling to 0 at inner_radius + 1200 m of the attenuation distance (sound+0x1d0).
  float width = 1.0f;
  if (R<uint8_t>(s, 0x1a6)) {
    const float rin = R<float>(s, 0x144);
    const double d = R<double>(s, 0x1d0);
    if (!(rin >= d)) width = d >= rin + 1200.0f ? 0.0f : static_cast<float>((rin + 1200.0f - d) / 1200.0);
  }
  W<float>(v, 0x24, width);
}

inline float NextSample(void* v, float* right) {
  const uint32_t kind = R<uint32_t>(v, kVKind);
  const float amp = R<float>(v, kVAmp);
  double ph = R<double>(v, kVPhase);
  float l = 0, rr = 0;
  if (kind == kDc) {
    l = rr = amp;
  } else if (kind == kDcSplit) {
    l = amp;
    rr = 0.25f * amp;
  } else if (kind == kDual) {
    l = rr = amp * static_cast<float>(std::sin(ph));
    ph += R<double>(v, kVStep);
    if (ph > 6.283185307179586 * 1000) ph -= 6.283185307179586 * 1000;
    W<double>(v, kVPhase, ph);
  } else if (kind == kNoise) {
    uint32_t st = R<uint32_t>(v, kVRng);
    st = st * 1664525u + 1013904223u;
    l = amp * (static_cast<float>(st >> 8) / 8388608.0f - 1.0f);
    st = st * 1664525u + 1013904223u;
    rr = amp * (static_cast<float>(st >> 8) / 8388608.0f - 1.0f);
    W<uint32_t>(v, kVRng, st);
  } else {
    l = amp * static_cast<float>(std::sin(ph));
    rr = amp * static_cast<float>(std::sin(ph * 1.3 + 0.5));
    ph += R<double>(v, kVStep);
    if (ph > 6.283185307179586 * 1000) ph -= 6.283185307179586 * 1000;
    W<double>(v, kVPhase, ph);
  }
  if (right) *right = rr;
  return l;
}

// Ramps: a at 0xa54 + 0x30 ch, b at 0xa6c + 0x30 ch: {target, start}. Gain at sample i = start + (target - start) (i+1)/n.
inline int FakeVoiceRender(void* v, float* out, uint32_t frames) {
  if (!reinterpret_cast<std::atomic<uint32_t>*>(static_cast<uint8_t*>(v) + kVAlive)->load(std::memory_order_acquire))
    return 0;
  memcpy(static_cast<uint8_t*>(v) + 0xd0, static_cast<uint8_t*>(v) + 0x20, 0xb0);
  const uint8_t* r = R<uint8_t*>(v, 0x08);
  const uint32_t chans = R<uint32_t>(r, 0x2274);
  float sa[17], ta[17], sb[17], tb[17];
  for (uint32_t ch = 0; ch < chans && ch < 17; ++ch) {
    sa[ch] = R<float>(v, 0xa54 + 0x30 * ch);
    sb[ch] = R<float>(v, 0xa6c + 0x30 * ch);
    ta[ch] = R<float>(v, 0xf8 + 8 * ch);
    tb[ch] = R<float>(v, 0xfc + 8 * ch);
  }
  const bool mono = R<uint32_t>(v, 0xec4) == 1;
  // The stereo width stage (kernel 0x183e0, 0x185ac..0x186a9): L' = M + w S, R' = M - w S, w ramped from its current
  // value (voice+0x618) to the snapshot's width (voice+0xd4).
  const float w0 = R<float>(v, 0x618), w1 = R<float>(v, 0xd4);
  const float inv = 1.0f / static_cast<float>(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    float sr = 0;
    float sl = NextSample(v, &sr);
    const float t = static_cast<float>(i + 1) * inv;
    if (!mono) {
      const float m = (sr + sl) * 0.5f, sd = (sl - sr) * 0.5f, w = w0 + (w1 - w0) * t;
      sl = sd * w + m;
      sr = m - sd * w;
    }
    for (uint32_t ch = 0; ch < chans && ch < 17; ++ch) {
      const float a = sa[ch] + (ta[ch] - sa[ch]) * t, b = sb[ch] + (tb[ch] - sb[ch]) * t;
      out[i * chans + ch] += mono ? sl * (a + b) : sl * a + sr * b;
    }
  }
  W<float>(v, 0x618, w1);
  for (uint32_t ch = 0; ch < chans && ch < 17; ++ch) {
    W<float>(v, 0xa54 + 0x30 * ch, ta[ch]);
    W<float>(v, 0xa6c + 0x30 * ch, tb[ch]);
  }
  int64_t rem = R<int64_t>(v, kVRemaining);
  if (rem >= 0) {
    rem -= frames;
    W<int64_t>(v, kVRemaining, rem < 0 ? 0 : rem);
    if (rem <= 0) {
      reinterpret_cast<std::atomic<uint32_t>*>(static_cast<uint8_t*>(v) + kVAlive)->store(0, std::memory_order_release);
      reinterpret_cast<std::atomic<uint32_t>*>(static_cast<uint8_t*>(v) + kVEnded)->store(1, std::memory_order_release);
      return 0;
    }
  }
  return 1;
}

inline void FakeMixRef(void* renderer, float* out, uint32_t frames) {
  for (void* v = R<void*>(renderer, 0x20); v; v = R<void*>(v, 0)) FakeVoiceRender(v, out, frames);
}

SRWLOCK g_edLock = SRWLOCK_INIT;
std::atomic<uint8_t*> g_lockExpect{nullptr};
std::atomic<uint64_t> g_lockBad{0};
inline void FakeLock(void* m) {
  if (m != g_lockExpect.load()) g_lockBad.fetch_add(1);
  AcquireSRWLockExclusive(&g_edLock);
}
inline void FakeUnlock(void* m) {
  if (m != g_lockExpect.load()) g_lockBad.fetch_add(1);
  ReleaseSRWLockExclusive(&g_edLock);
}

// ---- the image --------------------------------------------------------------------------------------------------------
struct Image {
  uint8_t* base = nullptr;
  std::vector<uint32_t> rel;    // RVAs of every 4-byte displacement written (masked in the hashes)
  std::vector<uint32_t> imm64;  // RVAs of every 8-byte absolute address written

  void B(uint32_t rva, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) base[rva++] = b;
  }
  // A 4-byte rip-relative field at `field`, for an instruction ending at `end`, pointing at `target`.
  void Rel(uint32_t field, uint32_t end, uint32_t target) {
    const int32_t d = static_cast<int32_t>(target) - static_cast<int32_t>(end);
    memcpy(base + field, &d, 4);
    rel.push_back(field);
  }
  void Abs(uint32_t field, const void* p) {
    memcpy(base + field, &p, 8);
    imm64.push_back(field);
  }
  // mov rax, imm64; jmp rax
  void JumpTo(uint32_t rva, const void* p) {
    B(rva, {0x48, 0xB8});
    Abs(rva + 2, p);
    B(rva + 10, {0xFF, 0xE0});
  }
};

inline void WriteHeaders(Image& im, uint32_t timestamp, uint32_t sizeOfImage) {
  uint8_t* b = im.base;
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(b);
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_lfanew = 0x80;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(b + 0x80);
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
  nt->FileHeader.NumberOfSections = 3;
  nt->FileHeader.TimeDateStamp = timestamp;
  nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
  nt->FileHeader.Characteristics = IMAGE_FILE_DLL | IMAGE_FILE_EXECUTABLE_IMAGE;
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  nt->OptionalHeader.SizeOfImage = sizeOfImage;
  nt->OptionalHeader.ImageBase = reinterpret_cast<ULONGLONG>(b);
  nt->OptionalHeader.SectionAlignment = 0x1000;
  nt->OptionalHeader.FileAlignment = 0x200;
  nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
  IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
  memcpy(s[0].Name, ".text", 5);
  s[0].VirtualAddress = kTextBegin;
  s[0].Misc.VirtualSize = kTextEnd - kTextBegin;
  s[0].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
  memcpy(s[1].Name, ".rdata", 6);
  s[1].VirtualAddress = kRdataBegin;
  s[1].Misc.VirtualSize = kDataBegin - kRdataBegin;
  s[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
  memcpy(s[2].Name, ".data", 5);
  s[2].VirtualAddress = kDataBegin;
  s[2].Misc.VirtualSize = kImageSize - kDataBegin;
  s[2].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
}

// Builds the image. `timestamp` / `sizeOfImage` go into its header (the analysed build or not, per the test's table).
inline Image Build(uint32_t timestamp, uint32_t sizeOfImage = kImageSize) {
  Image im;
  im.base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!im.base) return im;
  memset(im.base + kTextBegin, 0xCC, kTextEnd - kTextBegin);
  WriteHeaders(im, timestamp, sizeOfImage);
  // ---- SND_process 0x4980-0x4dc2 ----
  im.B(0x49ac, {0x48, 0x8D, 0x0D});  // lea rcx, [mutex]
  im.Rel(0x49af, 0x49b3, kMutex);
  im.B(0x49b3, {0xFF, 0x15});        // call [lock]
  im.Rel(0x49b5, 0x49b9, kLockSlot);
  im.B(0x49b9, {0x83, 0x3D});        // cmp dword [state], 1
  im.Rel(0x49bb, 0x49c0, kEngineState);
  im.B(0x49bf, {0x01, 0x0F, 0x8E});  // jle rel32
  im.Rel(0x49c2, 0x49c6, 0x4d87);
  im.B(0x49c6, {0xE8});              // call rel32
  im.Rel(0x49c7, 0x49cb, 0x4980);
  im.B(0x49d5, {0x33, 0xF6, 0x44, 0x8B, 0xFE, 0x4C, 0x8D, 0x2D});  // xor esi, esi; mov r15d, esi; lea r13, [contexts]
  im.Rel(0x49dd, 0x49e1, kCtxArray);
  im.B(0x49e1, {0xF2, 0x44, 0x0F, 0x10, 0x05});  // movsd xmm8, [rip]
  im.Rel(0x49e6, 0x49ea, kDataBegin + 0x10);
  im.B(0x49ea, {0x8B, 0x05});        // mov eax, [context count]
  im.Rel(0x49ec, 0x49f0, kCtxCount);
  im.B(0x49f0, {0x85, 0xC0});
  im.B(0x4b23, {0xFF, 0x05});        // inc dword [frame counter]
  im.Rel(0x4b25, 0x4b29, kFrameCounter);
  im.B(0x4b29, {0x8B, 0xC6, 0x89, 0x05});
  im.Rel(0x4b2d, 0x4b31, kDataBegin + 0x20);
  im.B(0x4b31, {0x8B, 0xFE, 0x4C, 0x8D, 0x25});
  im.Rel(0x4b36, 0x4b3a, kDataBegin + 0x30);
  // driver: void drv(Sound** slot)
  im.B(0x4d00, {0x53, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0xE1, 0xEB, 0x24});
  im.B(0x4d30, {0x49, 0x8B, 0x1C, 0x24, 0x48, 0x8B, 0x8B, 0xF0, 0x01, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x07, 0xE8});
  im.Rel(0x4d41, 0x4d45, 0x1c1c0);   // P3a: call panner
  im.B(0x4d45, {0xEB, 0x32, 0x83, 0xBB, 0xBC, 0x01, 0x00, 0x00, 0x00, 0xEB, 0x29});
  im.B(0x4d79, {0x48, 0x83, 0xC4, 0x28, 0x41, 0x5C, 0x5B, 0xC3});
  im.B(0x4d87, {0x48, 0x8D, 0x0D});  // lea rcx, [mutex]; call [unlock]
  im.Rel(0x4d8a, 0x4d8e, kMutex);
  im.B(0x4d8e, {0xFF, 0x15});
  im.Rel(0x4d90, 0x4d94, kUnlockSlot);
  // ---- voice creation 0x1b870-0x1bff2: driver void drv(Voice*, uint8_t ctx2d) ----
  im.B(0x1b9e0, {0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x8A, 0xC2, 0xEB, 0x04});
  im.B(0x1b9f0, {0xC7, 0x83, 0xBC, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3B, 0x88, 0x83, 0xC2, 0x0E, 0x00, 0x00, 0xE8});
  im.Rel(0x1ba01, 0x1ba05, 0x1c1c0);  // P3b: call panner
  im.B(0x1ba05, {0x0F, 0x10, 0x4B, 0x30, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3});
  // ---- mix 0x1b180-0x1b45a ----
  im.B(0x1b180, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41,
                 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF1, 0x4C, 0x8D, 0x79, 0x20});
  im.B(0x1b19d, {0x45, 0x8B, 0xE0, 0x48, 0x8B, 0xDA, 0x33, 0xED, 0x49, 0x8B, 0x3F, 0x48, 0x85, 0xFF, 0x74, 0x76, 0xEB, 0x41});
  im.B(0x1b1f0, {0x45, 0x8B, 0xC4, 0x48, 0x8B, 0xD3, 0x48, 0x8B, 0xCF, 0xFF, 0xC5, 0xE8});
  im.Rel(0x1b1fc, 0x1b200, 0x165d0);  // P4: call voice render
  im.B(0x1b200, {0x85, 0xC0, 0x75, 0x17, 0xEB, 0x15});
  im.B(0x1b21b, {0x48, 0x8B, 0x3F, 0x48, 0x85, 0xFF, 0x75, 0xCD});
  im.B(0x1b223, {0x48, 0x83, 0xC4, 0x20, 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5C, 0x5F, 0x5E, 0x48, 0x8B, 0x5C, 0x24, 0x10, 0x48,
                 0x8B, 0x6C, 0x24, 0x18, 0xC3});
  // ---- voice render 0x165d0 ----
  im.B(0x165d0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x80, 0xB9, 0xC0, 0x0E, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x0F, 0x10,
                 0x41, 0x20, 0x48, 0x83, 0xC4, 0x40, 0x5B});
  im.JumpTo(0x165e9, reinterpret_cast<const void*>(&FakeVoiceRender));
  // ---- panner 0x1c1c0 ----
  im.B(0x1c1c0, {0x48, 0x8B, 0xC4, 0x53, 0x55, 0x56, 0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x59, 0x10, 0x48,
                 0x8B, 0xE9, 0x48, 0x8B, 0x71, 0x08, 0x48, 0x81, 0xC4, 0xE0, 0x00, 0x00, 0x00, 0x5E, 0x5D, 0x5B});
  im.JumpTo(0x1c1e2, reinterpret_cast<const void*>(&FakePanner));
  im.B(0x1c1c0 + 0x263, {0xF3, 0x42, 0x0F, 0x11, 0x44, 0xFD, 0x48});
  im.B(0x1c1c0 + 0x28c, {0xF3, 0x42, 0x0F, 0x11, 0x44, 0xFD, 0x48});
  im.B(0x1c1c0 + 0x29c, {0xF3, 0x42, 0x0F, 0x11, 0x74, 0xFD, 0x4C});
  im.B(0x1c1c0 + 0x2a6, {0x44, 0x3B, 0xBE, 0x74, 0x22, 0x00, 0x00});
  im.B(0x1c1c0 + 0x42e, {0x80, 0xBB, 0xA6, 0x01, 0x00, 0x00, 0x00});  // cmp byte [rbx+0x1a6], 0 (auto_width)
  // ---- WASAPI loop 0x1ff00-0x20312: driver void drv(Wasapi*, float* out, uint32_t frames) ----
  im.B(0x20160, {0x53, 0x57, 0x55, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9, 0x41, 0x8B, 0xF8, 0x48, 0x89, 0x54, 0x24, 0x20,
                 0x48, 0x8D, 0x6C, 0x24, 0x41, 0x90});
  im.B(0x20178, {0x48, 0x8B, 0x55, 0xDF, 0x48, 0x8D, 0x4B, 0x08, 0x44, 0x8B, 0xC7, 0xE8});
  im.Rel(0x20184, 0x20188, 0x1b180);  // P5: call mix
  im.B(0x20188, {0x48, 0x8B, 0x8B, 0x30, 0x27, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x30, 0x5D, 0x5F, 0x5B, 0xC3});
  im.B(0x201c1, {0xF0, 0xFF, 0x83, 0x74, 0x22, 0x00, 0x00});
  // ---- data ----
  void* lockFn = reinterpret_cast<void*>(&FakeLock);
  void* unlockFn = reinterpret_cast<void*>(&FakeUnlock);
  memcpy(im.base + kLockSlot, &lockFn, 8);
  memcpy(im.base + kUnlockSlot, &unlockFn, 8);
  W<int32_t>(im.base, kEngineState, 3);
  W<int32_t>(im.base, kCtxCount, 8);
  DWORD old;
  VirtualProtect(im.base + kTextBegin, kTextEnd - kTextBegin, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), im.base, kImageSize);
  return im;
}

inline void Free(Image& im) {
  if (im.base) VirtualFree(im.base, 0, MEM_RELEASE);
  im.base = nullptr;
}

// ---- a world: one WASAPI renderer, sounds and voices (heap; the image only provides code and globals) --------------
struct World {
  uint8_t* wasapi = nullptr;
  std::vector<uint8_t*> sounds;  // parallel to voices
  std::vector<uint8_t*> voices;
  std::vector<uint8_t*> slots;   // the soundPtr cells the P3a driver reads (one per sound)
  void* Renderer() const { return wasapi + 8; }

  void Init(double rate = 48000.0) {
    wasapi = static_cast<uint8_t*>(_aligned_malloc(kWasapiSize, 64));
    memset(wasapi, 0, kWasapiSize);
    uint8_t* r = wasapi + 8;
    W<double>(r, 0, rate);
    W<uint32_t>(r, 0x2274, 2);
    const double dirs[2][3] = {{0.5, 0.0, -0.8660254037844386}, {0.5, 0.0, 0.8660254037844386}};
    for (int ch = 0; ch < 2; ++ch) {
      uint8_t* spk = r + 0x2278 + 0x30 * ch;
      memcpy(spk, dirs[ch], 24);
      W<float>(spk, 0x18, ch == 0 ? 1.0f : 0.0f);
      W<float>(spk, 0x1c, ch == 0 ? 0.0f : 1.0f);
      W<float>(spk, 0x20, ch == 0 ? 1.0f : 0.0f);
      W<float>(spk, 0x24, ch == 0 ? 0.0f : 1.0f);
    }
  }
  void Destroy() {
    for (uint8_t* p : sounds) _aligned_free(p);
    for (uint8_t* p : voices) _aligned_free(p);
    for (uint8_t* p : slots) _aligned_free(p);
    sounds.clear(), voices.clear(), slots.clear();
    if (wasapi) _aligned_free(wasapi);
    wasapi = nullptr;
  }
  // A sound with its voice, linked at the head of the renderer's list (ED pushes new voices the same way).
  int Add(uint8_t* image, int ctx, bool is3d, uint32_t channels, Kind kind, float amp, double hz, float gain,
          uint32_t seed = 1) {
    uint8_t* s = static_cast<uint8_t*>(_aligned_malloc(kSoundSize, 64));
    uint8_t* v = static_cast<uint8_t*>(_aligned_malloc(kVoiceSize, 64));
    uint8_t* slot = static_cast<uint8_t*>(_aligned_malloc(16, 16));
    memset(s, 0, kSoundSize);
    memset(v, 0, kVoiceSize);
    W<void*>(s, 0x000, image ? image + kCtxArray + 0x388 * ctx : nullptr);
    W<uint64_t>(s, 0x018, is3d ? 1 : 0);
    W<float>(s, 0x1c0, gain);
    W<void*>(s, 0x1f0, v);
    W<void*>(v, 0x008, Renderer());
    W<void*>(v, 0x010, s);
    W<uint32_t>(v, 0xec4, channels);
    W<uint32_t>(v, 0xec8, 48000);
    W<float>(v, 0x24, 1.0f);   // width: pending, snapshot, current
    W<float>(v, 0xd4, 1.0f);
    W<float>(v, 0x618, 1.0f);
    W<uint8_t>(v, 0xec2, image ? R<uint8_t>(image + kCtxArray + 0x388 * ctx, 0xe0) : 0);
    W<double>(v, kVStep, 6.283185307179586 * hz / 48000.0);
    W<float>(v, kVAmp, amp);
    W<uint32_t>(v, kVKind, kind);
    W<uint32_t>(v, kVRng, seed);
    W<int64_t>(v, kVRemaining, -1);
    W<uint32_t>(v, kVAlive, 1);
    W<void*>(slot, 0, s);
    W<void*>(v, 0, R<void*>(Renderer(), 0x20));
    W<void*>(Renderer(), 0x20, v);
    sounds.push_back(s), voices.push_back(v), slots.push_back(slot);
    SetPos(static_cast<int>(sounds.size()) - 1, 10, 0, 0);
    return static_cast<int>(sounds.size()) - 1;
  }
  void SetPos(int i, double x, double y, double z) {
    uint8_t* s = sounds[i];
    const double p[3] = {x, y, z};
    memcpy(s + 0x0d0, p, 24);
    const double d = std::sqrt(x * x + y * y + z * z);
    W<double>(s, 0x1c8, d);
    W<double>(s, 0x1d0, d);
    const double u[3] = {d >= 0.01 ? -x / d : 0.0, d >= 0.01 ? -y / d : 0.0, d >= 0.01 ? -z / d : 0.0};
    memcpy(s + 0x1d8, u, 24);
  }
  // ED's voice creation: after the first panner call the ramps start at their targets (no ramp in the first block).
  void InitRamps(int i) {
    uint8_t* v = voices[i];
    for (int ch = 0; ch < 2; ++ch) {
      W<float>(v, 0xa54 + 0x30 * ch, R<float>(v, 0x48 + 8 * ch));
      W<float>(v, 0xa6c + 0x30 * ch, R<float>(v, 0x4c + 8 * ch));
    }
    W<float>(v, 0x618, R<float>(v, 0x24));
  }
  // The sdef's auto_width with its inner_radius (as DCS stores them in the sound: +0x1a6, +0x144).
  void SetAutoWidth(int i, float innerRadius) {
    W<uint8_t>(sounds[i], 0x1a6, 1);
    W<float>(sounds[i], 0x144, innerRadius);
  }
};

}  // namespace fakeed
