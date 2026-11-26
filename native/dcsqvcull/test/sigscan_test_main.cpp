// Standalone runner for sigscan_test.h and reloc_test.h (built by
// build_sigscan_test.bat). `sigscan_test.exe --reloc DIR` instead resolves
// every hook_sigs.h location in the DLLs of DIR (tools/test_reloc.py).
#include "sigscan_test.h"
#include "reloc_test.h"

#include "../src/ng_build.h"

static void TestModelLayout() {
  struct Header {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
  } header;
  header.dos.e_magic = IMAGE_DOS_SIGNATURE;
  header.dos.e_lfanew = offsetof(Header, nt);
  header.nt.Signature = IMAGE_NT_SIGNATURE;
  header.nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  header.nt.OptionalHeader.SizeOfImage = ngb::kSize2932;
  const auto* image = reinterpret_cast<const uint8_t*>(&header);

  alignas(8) uint8_t material[0x300]{};
  const int64_t handles[] = {17, 31, -1};
  const uint32_t legacyCount = 3, currentCount = 2;
  memcpy(material + 0x240, handles, sizeof(handles));
  memcpy(material + 0x2d8, &legacyCount, sizeof(legacyCount));
  memcpy(material + 0x2e0, &currentCount, sizeof(currentCount));
  const auto saved = ngb::g_build.load();
  header.nt.FileHeader.TimeDateStamp = 0x6acb85e4u;
  ngb::Use(image);
  sigscan_test::Check(ngb::Tex(material)[0] == 17 && ngb::TexN(material) == 3,
                      "model layout: 2.9.31 reads the original first texture and count");
  header.nt.FileHeader.TimeDateStamp = ngb::kStamp2932;
  ngb::Use(image);
  sigscan_test::Check(ngb::Tex(material)[0] == 31 && ngb::Tex(material)[1] == -1 && ngb::TexN(material) == 2,
                      "model layout: 2.9.32 skips the water-mask handle and reads the new count");
  header.nt.Signature = 0;
  sigscan_test::Check(ngb::BuildOf(image) != ngb::k2932, "model layout: invalid PE cannot select 2.9.32");
  ngb::g_build.store(saved);
}

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && wcscmp(argv[1], L"--reloc") == 0) return reloc_test::RelocMain(argv[2]);
  TestModelLayout();
  sigscan_test::RunAll();
  reloc_test::RunAll();
  printf("%d passed, %d failed (sigscan and reloc)\n", sigscan_test::g_pass, sigscan_test::g_fail);
  return sigscan_test::g_fail ? 1 : 0;
}
