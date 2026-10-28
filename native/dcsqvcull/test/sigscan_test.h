// Unit tests for src/sigscan.h. Built standalone by build_sigscan_test.bat
// (sigscan_test_main.cpp); not part of test_main.cpp.
//
//   1. pattern parsing;
//   2. pattern search in a buffer: function bytes placed at another offset,
//      wildcards over displacements, duplicates reported as ambiguous;
//   3. a synthetic PE image in memory: code-section search, RTTI vtable
//      lookup by name, COL offset (primary vs secondary vtable), prefix match;
//   4. this test executable's own RTTI (a real MSVC x64 image);
//   5. optional, read-only: DCS's NGModel.dll mapped as an image resource
//      (never executed) when DCS_BIN is set; skipped for other builds.
#pragma once

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/sigscan.h"

namespace sigscan_test {

int g_fail = 0;
int g_pass = 0;
void Check(bool ok, const char* what) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  ok ? ++g_pass : ++g_fail;
}

// A function body as it might appear in .text: prologue, a RIP-relative load,
// a call rel32 and an epilogue.
const uint8_t kFunc[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,              // mov [rsp+8], rbx
    0x57,                                      // push rdi
    0x48, 0x83, 0xEC, 0x20,                    // sub rsp, 0x20
    0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44,  // mov rax, [rip+disp]
    0x8B, 0x48, 0x60,                          // mov ecx, [rax+0x60]
    0xE8, 0xAA, 0xBB, 0xCC, 0x0D,              // call rel32
    0x48, 0x83, 0xC4, 0x20,                    // add rsp, 0x20
    0x5F,                                      // pop rdi
    0xC3,                                      // ret
};
const char kFuncPattern[] = "48 89 5C 24 08 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 8B 48 60 E8 ?? ?? ?? ??";

void TestParse() {
  sigscan::Pattern p;
  Check(sigscan::Parse("48 8B ?? 05 ? C3", p) && p.size() == 6 && p.care[2] == 0 && p.care[4] == 0 &&
            p.bytes[1] == 0x8B && p.bytes[5] == 0xC3,
        "parse: bytes and both wildcard spellings");
  Check(sigscan::Parse("  48\t8b  ", p) && p.size() == 2 && p.bytes[1] == 0x8B, "parse: whitespace and lower case");
  Check(!sigscan::Parse("48 8G", p), "parse: rejects a non-hex digit");
  Check(!sigscan::Parse("488B", p), "parse: rejects unseparated bytes");
  Check(!sigscan::Parse("?? ??", p), "parse: rejects an all-wildcard pattern");
  Check(!sigscan::Parse("", p), "parse: rejects an empty pattern");
}

void TestBufferSearch() {
  sigscan::Pattern pat;
  sigscan::Parse(kFuncPattern, pat);
  std::vector<uint8_t> buf(0x3000, 0xCC);
  for (size_t i = 0; i < buf.size(); i += 7) buf[i] = static_cast<uint8_t>(i * 31);  // noise
  // The function moved to another offset, and its displacements changed (as
  // after a recompile that moves its callee and data).
  const size_t at = 0x1a37;
  memcpy(&buf[at], kFunc, sizeof(kFunc));
  buf[at + 13] ^= 0x5A;
  buf[at + 22] ^= 0x77;
  sigscan::Range r{buf.data(), buf.data() + buf.size()};
  const uint8_t* hits[4];
  size_t n = sigscan::FindAll(r, pat, hits, 4);
  Check(n == 1 && hits[0] == buf.data() + at, "buffer: moved function found once despite new displacements");
  // A fixed byte differs: no match.
  buf[at + 18] = 0x49;
  Check(sigscan::FindAll(r, pat, hits, 4) == 0, "buffer: a changed fixed byte is not matched");
  buf[at + 18] = 0x48;
  // A second copy: ambiguous.
  memcpy(&buf[0x400], kFunc, sizeof(kFunc));
  n = sigscan::FindAll(r, pat, hits, 4);
  Check(n == 2 && hits[0] == buf.data() + 0x400 && hits[1] == buf.data() + at, "buffer: duplicate reported twice");
  Check(sigscan::FindAll(r, pat, nullptr, 0, 2) == 2, "buffer: count limit for uniqueness checks");
  // Match at the very end of the range, and a pattern starting with a wildcard.
  std::vector<uint8_t> tail(64, 0x90);
  memcpy(&tail[64 - sizeof(kFunc)], kFunc, sizeof(kFunc));
  sigscan::Pattern p2;
  sigscan::Parse("?? 89 5C 24 08 57", p2);
  n = sigscan::FindAll({tail.data(), tail.data() + tail.size()}, p2, hits, 4);
  Check(n == 1 && hits[0] == tail.data() + 64 - sizeof(kFunc), "buffer: leading wildcard, match at range end");
  Check(sigscan::FindAll({tail.data(), tail.data() + 3}, p2, hits, 4) == 0, "buffer: range shorter than pattern");
}

// ---- a synthetic PE image ----
// .text at 0x1000, .rdata at 0x2000, .data at 0x3000, image size 0x4000.
struct FakeImage {
  std::vector<uint8_t> mem;
  uint8_t* base;
  FakeImage() : mem(0x4000 + 0x1000, 0) {
    // 4 KB-aligned base inside the vector (vtable pointers hold base + RVA).
    base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(mem.data()) + 0xFFF) & ~uintptr_t(0xFFF));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 3;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.ImageBase = 0x180000000ull;  // "preferred" base, differs from the buffer
    nt->OptionalHeader.SizeOfImage = 0x4000;
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    const char* names[3] = {".text", ".rdata", ".data"};
    const DWORD chars[3] = {IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_CODE,
                            IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA,
                            IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_CNT_INITIALIZED_DATA};
    for (int i = 0; i < 3; ++i) {
      memcpy(s[i].Name, names[i], strlen(names[i]));
      s[i].VirtualAddress = 0x1000 * (i + 1);
      s[i].Misc.VirtualSize = 0x1000;
      s[i].SizeOfRawData = 0x1000;
      s[i].Characteristics = chars[i];
    }
    memset(base + 0x1000, 0xCC, 0x1000);
  }
  // Type descriptor in .data at `td`, COL in .rdata at `col`, vtable right
  // after a pointer to the COL at `vt - 8`, slots pointing into .text.
  void AddClass(uint32_t td, const char* name, uint32_t col, uint32_t colOffset, uint32_t vt, uint64_t ptrBase) {
    const uint64_t typeInfoVt = 0x7ff800001000ull;  // pVFTable of type_info (another module)
    memcpy(base + td, &typeInfoVt, 8);
    memcpy(base + td + 16, name, strlen(name) + 1);
    sigscan::Col c{1, colOffset, 0, td, 0, col};
    memcpy(base + col, &c, sizeof(c));
    const uint64_t colPtr = ptrBase + col;
    memcpy(base + vt - 8, &colPtr, 8);
    for (int i = 0; i < 4; ++i) {
      const uint64_t fn = ptrBase + 0x1100 + 0x40 * i;
      memcpy(base + vt + 8 * i, &fn, 8);
    }
  }
};

void TestFakeImage() {
  FakeImage img;
  uint8_t* b = img.base;
  const uint64_t live = reinterpret_cast<uint64_t>(b);
  // Code: the function at 0x1600 of .text, a near-copy (one fixed byte
  // differs) at 0x1200.
  memcpy(b + 0x1600, kFunc, sizeof(kFunc));
  memcpy(b + 0x1200, kFunc, sizeof(kFunc));
  b[0x1200 + 19] = 0x50;
  int n = -1;
  uint8_t* f = sigscan::FindUnique(b, kFuncPattern, 0, &n);
  Check(f == b + 0x1600 && n == 1, "image: unique pattern in .text");
  f = sigscan::FindUnique(b, kFuncPattern, 13, &n);
  Check(f == b + 0x1600 + 13, "image: signature offset applied");
  Check(sigscan::RipTarget(b + 0x1600 + 10, 3, 7) == b + 0x1600 + 17 + 0x44332211, "image: RIP-relative target");
  Check(sigscan::Rel32Target(b + 0x1600 + 20) == b + 0x1600 + 25 + 0x0DCCBBAA && !sigscan::Rel32Target(b + 0x1600),
        "image: rel32 call target");
  // Same bytes outside .text (in .data) are not code: still unique.
  memcpy(b + 0x3800, kFunc, sizeof(kFunc));
  Check(sigscan::FindUnique(b, kFuncPattern, 0, &n) == b + 0x1600 && n == 1, "image: data sections are not searched");
  // A second real copy in .text: ambiguous, nullptr.
  memcpy(b + 0x1A00, kFunc, sizeof(kFunc));
  Check(!sigscan::FindUnique(b, kFuncPattern, 0, &n) && n == 2, "image: duplicate in .text is ambiguous");
  Check(!sigscan::FindUnique(b, "48 8G", 0, &n) && n == 0, "image: malformed pattern");

  // RTTI. Foo: primary vtable (COL offset 0) and a secondary one (0x48).
  img.AddClass(0x3000, ".?AVFoo@test@@", 0x2100, 0, 0x2208, live);
  img.AddClass(0x3040, ".?AVFooBar@test@@", 0x2120, 0, 0x2308, live);
  // The secondary vtable shares Foo's type descriptor.
  sigscan::Col c2{1, 0x48, 0, 0x3000, 0, 0x2140};
  memcpy(b + 0x2140, &c2, sizeof(c2));
  const uint64_t colPtr = live + 0x2140;
  memcpy(b + 0x2400 - 8, &colPtr, 8);
  void** vt = sigscan::FindVtable(b, ".?AVFoo@test@@", 0, false, &n);
  Check(vt == reinterpret_cast<void**>(b + 0x2208) && n == 1, "rtti: primary vtable by exact name");
  vt = sigscan::FindVtable(b, ".?AVFoo@test@@", 0x48, false, &n);
  Check(vt == reinterpret_cast<void**>(b + 0x2400) && n == 1, "rtti: secondary vtable by COL offset");
  const char* name = sigscan::RttiName(b, reinterpret_cast<void**>(b + 0x2308));
  Check(name && strcmp(name, ".?AVFooBar@test@@") == 0, "rtti: name of a vtable");
  Check(!sigscan::FindVtable(b, ".?AVFoo", 0, true, &n) && n == 2, "rtti: ambiguous prefix (Foo, FooBar)");
  vt = sigscan::FindVtable(b, ".?AVFooB", 0, true, &n);
  Check(vt == reinterpret_cast<void**>(b + 0x2308) && n == 1, "rtti: unique prefix (63-char RttiIs style)");
  Check(!sigscan::FindVtable(b, ".?AVMissing@test@@", 0, false, &n) && n == 0, "rtti: unknown class");
  Check(!sigscan::RttiName(b, reinterpret_cast<void**>(b + 0x1600)), "rtti: no COL before code");

  // An image mapped without relocation: pointers hold the preferred base.
  FakeImage raw;
  raw.AddClass(0x3000, ".?AVBaz@test@@", 0x2100, 0, 0x2208, 0x180000000ull);
  vt = sigscan::FindVtable(raw.base, ".?AVBaz@test@@", 0, false, &n);
  Check(vt == reinterpret_cast<void**>(raw.base + 0x2208) && n == 1, "rtti: unrelocated mapping (header ImageBase)");
}

// ---- this executable ----
struct Probe {
  virtual ~Probe() = default;
  virtual int Value() { return 1; }
};
struct ProbeChild : Probe {
  int Value() override { return 2; }
};

void TestOwnImage() {
  Probe* p = new ProbeChild;
  void** real = *reinterpret_cast<void***>(p);
  HMODULE self = GetModuleHandleW(nullptr);
  int n = -1;
  void** vt = sigscan::FindVtable(self, ".?AUProbeChild@sigscan_test@@", 0, false, &n);
  Check(vt == real && n == 1, "own image: vtable of ProbeChild by RTTI name");
  const char* name = sigscan::RttiName(sigscan::ImageBase(self), real);
  Check(name && strcmp(name, ".?AUProbeChild@sigscan_test@@") == 0, "own image: RttiName of a live object");
  // The first bytes of a function in our own .text are found again (at least
  // once; short prologues may repeat).
  const uint8_t* code = reinterpret_cast<const uint8_t*>(&TestFakeImage);
  if (code[0] == 0xE9) code = sigscan::Rel32Target(code);  // incremental-link thunk
  char pat[3 * 48 + 1] = {};
  for (int i = 0; i < 48; ++i) snprintf(pat + 3 * i, 4, "%02X ", code[i]);
  sigscan::Pattern pp;
  sigscan::Parse(pat, pp);
  sigscan::Range text;
  sigscan::Section(sigscan::ImageBase(self), ".text", text);
  const uint8_t* hits[8];
  const size_t k = sigscan::FindAll(text, pp, hits, 8);
  bool found = false;
  for (size_t i = 0; i < k && i < 8; ++i) found |= hits[i] == code;
  Check(found, "own image: 48 bytes of a function found in .text");
  delete p;
}

// ---- optional: DCS NGModel.dll, read-only image mapping ----
void TestDcs() {
  char dir[MAX_PATH] = {};
  if (!GetEnvironmentVariableA("DCS_BIN", dir, MAX_PATH)) {
    printf("SKIP dcs: DCS_BIN not set\n");
    return;
  }
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\NGModel.dll", dir);
  HMODULE m = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
  if (!m) {
    printf("SKIP dcs: %s not mapped\n", path);
    return;
  }
  const uint8_t* base = sigscan::ImageBase(m);
  // Expectations of the analysed builds (tools/hooks_manifest.json): 0x6aae1e68 and 0x6acb85e4 (DCS 2.9.30 hotfix and
  // 2.9.31, R147) have every RVA below in the same place.
  const uint32_t stamp = sigscan::NtHeaders(base)->FileHeader.TimeDateStamp;
  if (stamp != 0x6aae1e68 && stamp != 0x6acb85e4) {
    printf("SKIP dcs: NGModel.dll is not the analysed build (run tools/dcs_patch_check.py instead)\n");
    FreeLibrary(m);
    return;
  }
  int n = -1;
  void** vt = sigscan::FindVtable(m, ".?AVStructBufferManager@model@@", 0, false, &n);
  Check(vt == reinterpret_cast<void* const*>(base + 0x58650) && n == 1, "dcs: StructBufferManager vtable by RTTI");
  vt = sigscan::FindVtable(m, ".?AVModelMaterialMT@model@@", 0, false, &n);
  Check(vt == reinterpret_cast<void* const*>(base + 0x592f0) && n == 1, "dcs: ModelMaterialMT vtable by RTTI");
  // NG.StructBufferManager.scan and the big-pages immediate (manifest signatures).
  uint8_t* scan = sigscan::FindUnique(
      m, "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 50 41 8B E8", 0, &n);
  Check(scan == base + 0xc1b0 && n == 1, "dcs: StructBufferManager scan by pattern");
  uint8_t* imm = sigscan::FindUnique(m, "B8 00 FE 00 00 F7 F7 3B E8 0F 42 E8 4D 2B C8 48 B8 AB AA AA AA AA AA AA 2A", 0, &n);
  Check(imm == base + 0xc206 && n == 1, "dcs: 0xfe00 page-size immediate by pattern");
  // The scan function's bytes copied to another offset of a buffer: found there.
  std::vector<uint8_t> buf(0x2000, 0xCC);
  memcpy(&buf[0x777], base + 0xc1b0, 0x100);
  sigscan::Pattern pat;
  sigscan::Parse("48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 50 41 8B E8", pat);
  const uint8_t* hits[2];
  Check(sigscan::FindAll({buf.data(), buf.data() + buf.size()}, pat, hits, 2) == 1 && hits[0] == &buf[0x777],
        "dcs: scan function bytes found at a new offset in a buffer");
  FreeLibrary(m);
}

int RunAll() {
  TestParse();
  TestBufferSearch();
  TestFakeImage();
  TestOwnImage();
  TestDcs();
  printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}

}  // namespace sigscan_test
