// NGModel.dll builds the model hooks know, beyond the one their constants were
// written for (DCS 2.9.30 / 2.9.31, the same NGModel code: hook_sigs.h's
// analysed build).
//
// DCS 2.9.32.30490 (NGModel PE 0x6ae72a12, size 0x7b000) added a 14th
// ModelMaterialMT technique handle, water_mask (name table .data 0x706a8), at
// mat+0x240: ModelMaterialMT::draw (vt[4]) got a model pass 14 that draws it
// on every pass, like radar. Every field from +0x240 on moved by 8: the bound
// texture handles from +0x240 to +0x248 and their count from +0x2d8 to +0x2e0;
// every field below +0x240 (techniques +0x1d8..+0x238, CB +0x90..+0x1c0,
// shader +0x30, props +0x28, +0x18c) is where it was. Re-derived offline from
// the installed binary (R-2932):
//  - ModelMaterialMT::drawShadow (vt[5], 0x177e0), the submit (0x131f0),
//    0x15e40 (the old 0x15e20: CB writes, technique / pass selection and its
//    jump table) and SceneRenderable vt[1] (0x44680) are byte for byte the
//    2.9.31 code once every rel32 / RIP displacement and jump-table entry is
//    put back to its 2.9.31 target and, in drawShadow, the three texture
//    displacements back to +0x240 / +0x2d8: the 2.9.31 FNV hashes then match;
//  - 0xeb60, 0xcf80 and 0xc310 did not move or change; ModelDesc's animated
//    property classes, dx11backend's DX11Shader / DX11Texture / effects code
//    and GraphicsCore's G-buffer pass are unchanged;
//  - ModelMaterialMT::draw itself was recompiled (another register
//    allocation, the new case): read instruction by instruction, its head
//    (mat+0x18c = item+0xd4, shader vt[27] with the page buffer, 0xeb60,
//    0xcf80 on mat+0x248) and its default case (model passes outside 4..14:
//    0x15e40 with mesh = [item+0xc0] and &ctx, then return) do what 2.9.31's
//    did; those two ranges get their own 2.9.32 hashes (kCode2932). The
//    switch over passes 4..14 adds water_mask (the same all-pass handling as radar).
// Vtables moved +0x10 in .rdata (GlassMaterialMT did not); .data, the IAT and
// the FLIR threshold did not move.
//
// At() maps a 2.9.31 RVA to this build's, CodeIs() checks a 2.9.31 range at
// this build's place with this build's hash; Tex() / TexN() read the texture
// handles and their count where this build has them. Use() picks the build
// from the module's PE header (timestamp and size, as kVisBuilds does); any
// other NGModel.dll is treated as the analysed build and its own checks
// decide (reloc.h for the relocated ones). Header-only, included from
// main/views.inc before the model hooks (model_timing.h first).
#pragma once

#include <atomic>
#include "sigscan.h"

namespace ngb {

enum : int { k2931 = 0, k2932 = 1 };
constexpr uint32_t kStamp2932 = 0x6ae72a12u, kSize2932 = 0x7b000u;

struct Moved {
  uint32_t from, to;
};
// 2.9.31 -> 2.9.32, every hard-coded NGModel RVA that moved.
constexpr Moved kMoved2932[] = {
    {0x592f0, 0x59300},  // ModelMaterialMT vtable
    {0x59810, 0x59820},  // SceneRenderable vtable
    {0x59828, 0x59838},  // ShadowMapRenderable vtable
    {0x5a008, 0x5a018},  // NGModelLodsMT vtable
    {0x14980, 0x149a0},  // GlassMaterialMT::drawShadow (vt[5])
    {0x16140, 0x16160},  // ModelMaterialMT::draw (vt[4])
    {0x17750, 0x177e0},  // ModelMaterialMT::drawShadow (vt[5])
    {0x17892, 0x17922},  // ... its end
    {0x131e0, 0x131f0},  // ModelMaterialMT submit
    {0x13315, 0x13325},  // ... its end
    {0x15e20, 0x15e40},  // draw's default route: CB writes, renderer getters
    {0x15fd1, 0x15ff1},  // ... technique / pass selection
    {0x16134, 0x16154},  // ... the end of its jump table
    {0x44350, 0x44680},  // SceneRenderable vt[1]
    {0x44371, 0x446a1},  // ... the triangle counter add
    {0x44372, 0x446a2},
    {0x443dc, 0x4470c},  // ... its end
    {0x443e0, 0x44710},  // ShadowMapRenderable vt[1]
};
struct Code {
  uint32_t begin, end;  // 2.9.31 range (as the hooks name it)
  uint64_t hash;        // its 2.9.31 FNV-1a 64
  uint32_t begin32, end32;
  uint64_t hash32;  // the same code in 2.9.32
};
constexpr Code kCode2932[] = {
    {0x17750, 0x17892, 0x49534eb2132118cfull, 0x177e0, 0x17922, 0xb2b3d868d0193ae9ull},  // drawShadow
    {0x131e0, 0x13315, 0xfa5b29e55d76ca0cull, 0x131f0, 0x13325, 0x82e74e97110a663cull},  // submit
    {0x44350, 0x44371, 0x62507992c340cb98ull, 0x44680, 0x446a1, 0x79e84882e351acffull},  // SR vt[1] to the add
    {0x44372, 0x443dc, 0xb2afd5dce29e49cfull, 0x446a2, 0x4470c, 0xb2afd5dce29e49cfull},  // ... the rest
    {0x44350, 0x443dc, 0x48801b542c9549beull, 0x44680, 0x4470c, 0xead5f295245e06c7ull},  // ... whole (add = F0)
    {0x15e20, 0x15fd1, 0x151925c8ef3a0160ull, 0x15e40, 0x15ff1, 0x5b30d2e39dfed3f9ull},  // CB writes
    {0x15fd1, 0x16134, 0x1675bab468ed1984ull, 0x15ff1, 0x16154, 0xc0749c2aa8104f0cull},  // selection + table
    // draw (recompiled, read by hand): head through the 0xcf80 call; default case through the return.
    {0x16140, 0x161e5, 0xd3fe18746d88136dull, 0x16160, 0x161ff, 0x95c976dfc1872750ull},
    {0x16433, 0x1647c, 0xb6f40efc7d4a2849ull, 0x163ad, 0x163f0, 0x51f8b55569bbecb5ull},
    // Measurement only: the model-pass switch and its table (eleven entries including water_mask).
    {0x161e5, 0x16301, 0xeb2115bc10a89889ull, 0x161ff, 0x163f0, 0x197c2b235c596d94ull},
    {0x1647c, 0x164a4, 0x24760084557e75b3ull, 0x163f0, 0x1641c, 0xbe6f0539a62de174ull},
};

inline std::atomic<int> g_build{k2931};
// ModelMaterialMT: the texture handles (int64, -1 = none) and their count (uint32).

inline int BuildOf(const uint8_t* ng) {
  if (!ng) return k2931;
  __try {
    const auto* nt = sigscan::NtHeaders(ng);
    return nt && nt->FileHeader.TimeDateStamp == kStamp2932 && nt->OptionalHeader.SizeOfImage == kSize2932 ? k2932 : k2931;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return k2931;
  }
}
// Picks the build of this NGModel.dll image (and the material layout with it); returns it.
inline int Use(const uint8_t* ng) {
  const int b = BuildOf(ng);
  g_build.store(b, std::memory_order_relaxed);
  return b;
}
inline const char* Name(int b) { return b == k2932 ? "2.9.32" : "2.9.31"; }

// rva (a 2.9.31 RVA) in the build of ng.
inline uint32_t At(const uint8_t* ng, uint32_t rva) {
  if (Use(ng) != k2932) return rva;
  for (const Moved& m : kMoved2932)
    if (m.from == rva) return m.to;
  return rva;
}

// shadowtex::CodeIs for a 2.9.31 range [begin, end) with its 2.9.31 hash: on
// 2.9.32 the same code at its 2.9.32 place with its 2.9.32 hash (a range not
// in kCode2932 is checked unmoved, as it is when it did not change).
inline bool RawIs(const uint8_t* ng, uint32_t begin, uint32_t end, uint64_t hash) {
  if (end < begin) return false;
  uint64_t h = 0xcbf29ce484222325ull;
  __try {
    for (uint32_t i = begin; i < end; ++i) h = (h ^ ng[i]) * 0x100000001b3ull;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return h == hash;
}
inline bool CodeIs(uint8_t* ng, uint32_t begin, uint32_t end, uint64_t hash) {
  if (Use(ng) == k2932)
    for (const Code& c : kCode2932)
      if (c.begin == begin && c.end == end && c.hash == hash) return RawIs(ng, c.begin32, c.end32, c.hash32);
  return RawIs(ng, begin, end, hash);
}
// The 2.9.32 counterpart of a 2.9.31 range (itself when not listed): for range copies (SceneRenderableBytesMatch).
inline Code Range(const uint8_t* ng, uint32_t begin, uint32_t end, uint64_t hash) {
  if (BuildOf(ng) == k2932)
    for (const Code& c : kCode2932)
      if (c.begin == begin && c.end == end && c.hash == hash) return c;
  return {begin, end, hash, begin, end, hash};
}

// Both texture fields follow the technique handles. The extra handle moves them together.
inline uint32_t TextureFieldShift() {
  return g_build.load(std::memory_order_relaxed) == k2932 ? sizeof(int64_t) : 0;
}
inline int64_t* Tex(uint8_t* mat) { return reinterpret_cast<int64_t*>(mat + 0x240 + TextureFieldShift()); }
inline const int64_t* Tex(const uint8_t* mat) { return reinterpret_cast<const int64_t*>(mat + 0x240 + TextureFieldShift()); }
inline uint32_t TexN(const uint8_t* mat) {
  return *reinterpret_cast<const uint32_t*>(mat + 0x2d8 + TextureFieldShift());
}

}  // namespace ngb
