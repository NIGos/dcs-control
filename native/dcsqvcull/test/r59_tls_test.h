// R59 tls-128k-per-thread: texbind::HookMeasured's 128 KB table no longer
// lives in static TLS (allocated and copied for every thread of the process).
#pragma once

namespace r59tls {

// This module's static TLS template (the bytes Windows gives every thread).
size_t OwnTlsTemplate() {
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
  if (!d.VirtualAddress) return 0;
  auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(base + d.VirtualAddress);
  return static_cast<size_t>(tls->EndAddressOfRawData - tls->StartAddressOfRawData) + tls->SizeOfZeroFill;
}

uint32_t g_calls = 0;
uint64_t __fastcall FakeBind(void*, uint64_t) {
  ++g_calls;
  return 1;
}

void Run() {
  const size_t tls = OwnTlsTemplate();
  printf("     static TLS template: %zu bytes\n", tls);
  // The measured path still works with its heap table: the second identical
  // request in the same tick is skipped (deduplication on).
  const texbind::Fn savedOrig = texbind::g_orig;
  const bool savedDedupe = texbind::g_dedupe.load();
  texbind::g_orig = &FakeBind;
  texbind::g_dedupe = true;
  bool skipped = false;
  int tex = 0;
  for (int attempt = 0; attempt < 5 && !skipped; ++attempt) {  // a tick boundary between the two calls: retry
    g_calls = 0;
    const uint64_t size = 0x1234500 + attempt;
    const uint64_t a = texbind::HookMeasured(&tex, size);
    const uint64_t b = texbind::HookMeasured(&tex, size);
    skipped = a == 1 && b == 0 && g_calls == 1;
  }
  texbind::g_orig = savedOrig;
  texbind::g_dedupe = savedDedupe;
  Check(tls < 64 * 1024 && skipped,
        "R59 tex bind: the measured hook's table is per-thread heap, not 128 KB of static TLS; dedupe still works");
}

}  // namespace r59tls
