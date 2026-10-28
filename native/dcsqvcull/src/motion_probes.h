// Streaming probes for the motion profile (measurement only, nothing is
// skipped or reordered). Each probe wraps one vtable slot, checked against its
// RTTI class name, and counts calls and TSC cycles. Addresses from
// docs/research/R5_motion.md (DCS 2.9.30).
// Included once from main.cpp inside its anonymous namespace.
#pragma once

namespace mprobe {

using Fn = uint64_t(__fastcall*)(void*, void*, void*, void*);

struct Probe {
  const char* name;
  const wchar_t* module;
  uint32_t vtRva;
  int slot;
  const char* rttiPart;  // substring of the class name
  uint32_t vtRvaAlt;     // another analysed build's vtable RVA (0: none); the one whose class name matches is hooked
  Fn orig = nullptr;
  std::atomic<uint64_t> calls{0}, cycles{0};
};

// Only std::function _Do_call slots (known arity: this + up to 3 arguments
// in registers); functions with unknown signatures are not wrapped.
Probe g_probes[] = {
    // DCS 2.9.30 0x215698; 2.9.31 0x243da8 (R147: the same RTTI class, RenderThreadHandler's lambda_1).
    {"frame-start task drain", L"Visualizer.dll", 0x215698, 2, "_Func_impl_no_alloc@V<lambda_1>@?1???R1?1??RenderThreadHandler@", 0x243da8},
    {"texture mip finalize", L"dx11backend.dll", 0xc0638, 2, "_Func_impl", 0},
    {"terrain build spatial", L"edterrainGraphics41.dll", 0x155290, 2, "_Func_impl", 0},
    {"terrain build objects", L"edterrainGraphics41.dll", 0x155300, 2, "_Func_impl", 0},
};
constexpr int kProbes = sizeof(g_probes) / sizeof(g_probes[0]);

template <int N>
uint64_t __fastcall Hook(void* a, void* b, void* c, void* d) {
  uint64_t t0 = __rdtsc();
  uint64_t r = g_probes[N].orig(a, b, c, d);
  g_probes[N].calls.fetch_add(1, std::memory_order_relaxed);
  g_probes[N].cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
  return r;
}

template <int... I>
constexpr std::array<void*, sizeof...(I)> MakeHooks(std::integer_sequence<int, I...>) {
  return {reinterpret_cast<void*>(&Hook<I>)...};
}

std::string RttiName(uint8_t* base, void** vtbl) {
  void* col = nullptr;
  if (!allocslab::ReadBytes(vtbl - 1, &col, sizeof(col)) || !col) return "";
  uint32_t td = 0;
  if (!allocslab::ReadBytes(static_cast<uint8_t*>(col) + 12, &td, 4)) return "";
  char buf[200] = {};
  if (!allocslab::ReadBytes(base + td + 16, buf, sizeof(buf) - 1)) return "";
  return buf;
}

bool g_installed = false;

void Install() {
  if (g_installed) return;
  g_installed = true;
  static const auto hooks = MakeHooks(std::make_integer_sequence<int, kProbes>{});
  int ok = 0;
  for (int i = 0; i < kProbes; ++i) {
    Probe& p = g_probes[i];
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(p.module));
    if (!base) continue;
    auto** vtbl = reinterpret_cast<void**>(base + p.vtRva);
    std::string name = RttiName(base, vtbl);
    if (p.vtRvaAlt && (name.empty() || name.find(p.rttiPart) == std::string::npos)) {
      vtbl = reinterpret_cast<void**>(base + p.vtRvaAlt);
      name = RttiName(base, vtbl);
    }
    if (name.empty() || (p.rttiPart[0] && name.find(p.rttiPart) == std::string::npos)) {
      Log("motion probe %s: vtable class '%s' unexpected; skipped", p.name, name.c_str());
      continue;
    }
    p.orig = reinterpret_cast<Fn>(SlotOriginal(&vtbl[p.slot]));
    if (HookSlot(&vtbl[p.slot], hooks[i], nullptr)) {
      ++ok;
      Log("motion probe %s: %.80s", p.name, name.c_str());
    } else {
      p.orig = nullptr;
    }
  }
  Log("motion probes: %d/%d installed", ok, kProbes);
}

struct Snap {
  uint64_t calls[kProbes];
  uint64_t cycles[kProbes];
};
Snap Take() {
  Snap s{};
  for (int i = 0; i < kProbes; ++i) {
    s.calls[i] = g_probes[i].calls.load(std::memory_order_relaxed);
    s.cycles[i] = g_probes[i].cycles.load(std::memory_order_relaxed);
  }
  return s;
}

}  // namespace mprobe
