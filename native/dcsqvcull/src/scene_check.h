// R63 U2: is DCSScene+0x40 (kSceneCollectThreadsMax) still DCS's collect
// thread limit, and the collect call's last argument still the cap it is
// compared with? Partition boost writes the field and raises the argument on
// every collect call, so both must be verified on the running Scene.dll, not
// guessed from the value seen at +0x40.
//
// Scene.dll 2.9.30, collectSceneObjectsRenderables (this = IView = scene+8):
//   41 8B 4C 24 38          mov   ecx, [r12+0x38]        ; scene+0x40
//   ...                     (test / je / mov / cmp / cmova, 0x1d bytes)
//   39 8C 24 88 20 00 00    cmp   [rsp+0x2088], ecx      ; last argument
//   0F 42 8C 24 88 20 00 00 cmovb ecx, [rsp+0x2088]      ; min(field, argument)
// The matcher is register- and frame-agnostic (a rebuild may pick other
// registers or another frame size) but requires the +0x38 displacement on
// the IView `this` and the min() against one stack argument. A Scene.dll
// whose SceneBase moved the field fails it, and partition boost then leaves
// both the field and the argument alone (HookCollect).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace scenecheck {

constexpr uint8_t kFieldDisp = 0x38;   // kSceneCollectThreadsMax - 8 (IView this)
constexpr size_t kScanBytes = 0x1000;  // the use sits at +0x69b on 2.9.30
constexpr size_t kMinWindow = 48;      // mov ... cmp distance (0x1d on 2.9.30)

// `reg` (0-15) of a 32-bit `op /r` with an [rsp+disp32] memory operand at p
// (optional REX without W/B/X, then op bytes, modrm mod 10 rm 100, SIB 0x24).
inline bool RspDisp32(const uint8_t* p, size_t avail, const uint8_t* op, size_t opLen, int* reg, int32_t* disp,
                      size_t* len) {
  size_t i = 0;
  int rexR = 0;
  if (avail > 0 && (p[0] & 0xF0) == 0x40) {
    if (p[0] & 0x0B) return false;  // W, X or B: not a 32-bit [rsp+d]
    rexR = (p[0] >> 2) & 1;
    i = 1;
  }
  if (avail < i + opLen + 6 || memcmp(p + i, op, opLen) != 0) return false;
  const uint8_t modrm = p[i + opLen];
  if ((modrm & 0xC7) != 0x84 || p[i + opLen + 1] != 0x24) return false;
  *reg = ((modrm >> 3) & 7) | (rexR << 3);
  memcpy(disp, p + i + opLen + 2, 4);
  *len = i + opLen + 6;
  return true;
}

// Finds `mov r32, [base+0x38]` followed within kMinWindow bytes by
// `cmp [rsp+d], r32` and, right after it (within 16 bytes), `cmovb r32,
// [rsp+d]` with the same d (a positive stack slot). Returns the offset of
// the mov, or SIZE_MAX.
inline size_t FindThreadsMaxUse(const uint8_t* code, size_t len) {
  for (size_t at = 0; at + 4 <= len; ++at) {
    size_t i = at;
    int rexR = 0, rexB = 0;
    if ((code[i] & 0xF0) == 0x40) {
      if (code[i] & 0x0A) continue;  // W or X
      rexR = (code[i] >> 2) & 1;
      rexB = code[i] & 1;
      ++i;
    }
    if (i + 3 > len || code[i] != 0x8B) continue;
    const uint8_t modrm = code[i + 1];
    if ((modrm >> 6) != 1) continue;  // [base+disp8]
    size_t d = i + 2;
    if ((modrm & 7) == 4) {           // SIB: base only (index 100, no REX.X)
      if (d >= len || ((code[d] >> 3) & 7) != 4) continue;
      if ((code[d] & 7) == 4 && !rexB) continue;  // [rsp+0x38]: a local, not `this`
      ++d;
    }
    if (d >= len || code[d] != kFieldDisp) continue;
    const int reg = ((modrm >> 3) & 7) | (rexR << 3);
    const size_t after = d + 1;
    for (size_t j = after; j < after + kMinWindow && j < len; ++j) {
      int r1 = -1, r2 = -1;
      int32_t d1 = 0, d2 = 0;
      size_t l1 = 0, l2 = 0;
      static const uint8_t kCmp[] = {0x39};
      static const uint8_t kCmovb[] = {0x0F, 0x42};
      if (!RspDisp32(code + j, len - j, kCmp, 1, &r1, &d1, &l1) || r1 != reg || d1 <= 0) continue;
      for (size_t k = j + l1; k < j + l1 + 16 && k < len; ++k)
        if (RspDisp32(code + k, len - k, kCmovb, 2, &r2, &d2, &l2) && r2 == reg && d2 == d1) return at;
    }
  }
  return SIZE_MAX;
}

}  // namespace scenecheck
