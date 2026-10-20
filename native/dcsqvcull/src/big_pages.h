// Bigger pages for NGModel's per-instance data (StructBufferManager).
//
// When no page of an element size has room, the scan (NGModel 0xc1b0)
// creates one of max(count, 0xfe00 / elemSize) elements, about 63.5 KB
// [V 0xc206]. A busy airfield fills about 150 such pages per frame (7.0 MB of
// 96-byte and 2.9 MB of 112-byte elements, measured 2026-10-08). Each page is
// its own GPU structured buffer, uploaded with one Map(WRITE_DISCARD) of its
// used bytes at EndParse (slot 8, 0xbdb0) [V], and bound per draw as the
// sbPositions SRV selected by [item+0xd0] (the page index), with the element
// index in [item+0xd4].
//
// With big pages on, the page-size constant becomes g_bigBytes (4 MB by default; 12 MB measured -1.3% fps, 4 MB neutral), and at
// each per-frame reset the small pages are retired by setting their byte
// capacity to 0: the scan then never picks them (room = capBytes - used) and
// the upload skips them (used stays 0). New allocations land in one big page
// per element size. Every object keeps exactly the same bytes; only the page
// that holds them (and so the SRV bound with them) changes, which already
// varies from frame to frame. Fewer Map/Unmap calls, fewer SRV changes
// between draws, and same-model casters share one buffer (needed for shadow
// instancing, R12 §7).
//
// Turning it off restores the constant and the retired pages' capacities;
// big pages created meanwhile stay (DCS handles any page size).
//
// Payload hot reload (dev): the list of retired pages used to live in the
// payload, so a reload lost it and those pages (about 63.5 KB of GPU buffer
// each) stayed at capacity 0 for the rest of the session, never used again
// once big pages were turned off. The list now lives in a named mapping of
// this process (SharedList), which every payload generation opens: the new
// payload's per-frame reset restores what an older one retired. A page is
// retired only when the list has room for it (else it simply stays in use).
// R63: that covered a next payload that also wants big pages on. One that has
// them off (BigModelPages=0, engine off, PerceptionOnly) never installed, so
// nothing undid the patched page size or gave the retired pages back; and the
// old payload's reset hook, still in the vtable until the new one repoints it
// (or the loader's stale restore), kept retiring with g_wanted still set.
// Now: Stop clears g_wanted (main.cpp PassThroughAfterStop, so a leftover old
// hook restores at its next reset), a payload with big pages off arms its
// reset hook only to undo what an earlier one left (UndoLeftover), and the
// reset reads the instruction to know what is applied (another generation may
// have written it).
// Included once from main.cpp inside its anonymous namespace, after
// alloc_slab.h and pose_sweep.h.
#pragma once

namespace bigpages {

constexpr uint32_t kInsnRva = 0xc206;  // mov eax, 0xfe00
constexpr uint32_t kStockBytes = 0xfe00;
uint32_t g_bigBytes = 4u << 20;  // [Model] BigPageBytes; fixed once installed
const uint8_t kStockInsn[5] = {0xB8, 0x00, 0xFE, 0x00, 0x00};

std::atomic<int> g_state{0};  // 0 = not tried, -1 = unavailable, 1 = ready
std::atomic<bool> g_wanted{false};
bool g_applied = false;  // render-side state, changed only in OnReset
uint8_t* g_insn = nullptr;
struct Retired {
  uint32_t idx;
  uint32_t capBytes;
};
bool WriteConst(uint32_t bytes) {
  uint8_t insn[5] = {0xB8};
  memcpy(insn + 1, &bytes, 4);
  return posesweep::PatchAllSuspended(g_insn, insn, 5);
}

// The retired pages, shared by every payload generation of this process
// (reset thread only: one reset hook runs at a time, whichever payload's).
constexpr uint32_t kSharedMagic = 0x31504751;  // "QGP1"
constexpr uint32_t kMaxRetired = 8192;
struct SharedRetired {
  uint32_t magic;
  uint32_t count;
  Retired r[kMaxRetired];
};
SharedRetired* g_shared = nullptr;  // this payload's view (never unmapped: the mapping lives with the process)
std::atomic<uint64_t> g_retiredCount{0};
std::atomic<uint64_t> g_notRetired{0};  // small pages left in use because the list was full

// The process's list (created zeroed by the first payload that asks).
SharedRetired* SharedList() {
  if (g_shared) return g_shared;
  wchar_t name[96];
  swprintf_s(name, L"Local\\DcsQvCull.BigPages.v1.%lu", GetCurrentProcessId());
  HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedRetired), name);
  if (!m) return nullptr;
  auto* v = static_cast<SharedRetired*>(MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedRetired)));
  if (!v) {
    CloseHandle(m);
    return nullptr;
  }
  // The handle stays open (the mapping must outlive this payload: old
  // payloads are never unloaded, and the next one opens it by name).
  if (v->magic != kSharedMagic) {
    v->count = 0;
    v->magic = kSharedMagic;
  }
  g_shared = v;
  return v;
}

// The page size the instruction holds now (0 when unreadable or not mov eax, imm32).
uint32_t CurrentImm(const uint8_t* insn) {
  uint8_t cur[5];
  if (!insn || !allocslab::ReadBytes(insn, cur, 5) || cur[0] != 0xB8) return 0;
  uint32_t imm;
  memcpy(&imm, cur + 1, 4);
  return imm;
}

// The per-frame reset of the model data pages (allocslab::HookReset, reset thread).
void OnReset(void* mgr) {
  const bool want = g_wanted.load(std::memory_order_relaxed);
  if (const uint32_t imm = CurrentImm(g_insn)) g_applied = imm != kStockBytes;  // R63: what is really applied
  if (want != g_applied) {
    if (!WriteConst(want ? g_bigBytes : kStockBytes)) return;  // try again at the next reset
    g_applied = want;
  }
  SharedRetired* sh = SharedList();
  auto* b = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(mgr) + 8);
  auto* e = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(mgr) + 0x10);
  if (g_applied) {
    // Retire every small page that still has capacity (and fits the list).
    for (uint8_t* p = b; p && p < e; p += allocslab::kPageStride) {
      auto* pg = reinterpret_cast<allocslab::Page*>(p);
      if (pg->capBytes && pg->capBytes < g_bigBytes / 2) {
        if (!sh || sh->count >= kMaxRetired) {
          g_notRetired.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        sh->r[sh->count++] = {pg->idx, pg->capBytes};
        pg->capBytes = 0;
      }
    }
  } else if (sh && sh->count) {
    for (uint32_t k = 0; k < sh->count; ++k) {
      const Retired& r = sh->r[k];
      for (uint8_t* p = b; p && p < e; p += allocslab::kPageStride) {
        auto* pg = reinterpret_cast<allocslab::Page*>(p);
        if (pg->idx == r.idx && pg->capBytes == 0) pg->capBytes = r.capBytes;
      }
    }
    Log("big model pages: off; %u retired small pages given back their capacity", sh->count);
    sh->count = 0;
  }
  g_retiredCount.store(sh ? sh->count : 0, std::memory_order_relaxed);
}

// The page-size instruction of this NGModel build and its immediate; nullptr when NGModel is not loaded (*fatal
// false: retried) or does not match (*fatal true).
uint8_t* FindInsn(uint32_t* imm, bool* fatal) {
  *fatal = false;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  if (!base) return nullptr;
  *fatal = true;
  const uint32_t rva = reloc::Rva(hooksig::NG_StructBufferManager_scan_pageBytesImm, kInsnRva);
  if (!rva) return nullptr;
  uint8_t* insn = base + rva;
  *imm = CurrentImm(insn);
  if (!*imm || (*imm != kStockBytes && *imm < (1u << 20))) return nullptr;
  return insn;
}

// Install's tail: the reset hook armed on `insn` holding `imm`.
const SharedRetired* Arm(uint8_t* insn, uint32_t imm) {
  g_insn = insn;
  const SharedRetired* sh = SharedList();  // before the reset hook can run (it reads g_shared)
  allocslab::g_onReset = &OnReset;
  g_applied = imm != kStockBytes;
  if (g_applied) g_bigBytes = imm;  // left patched by a previous payload
  g_state = 1;
  return sh;
}

bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  if (!GetModuleHandleW(L"NGModel.dll") || allocslab::g_state.load() < 1) return false;  // not yet: retried later
  g_state = -1;
  uint32_t imm = 0;
  bool fatal = false;
  uint8_t* insn = FindInsn(&imm, &fatal);
  if (!insn) {
    if (fatal) Log("big model pages: NGModel.dll does not match this build; skipped");
    return false;
  }
  const SharedRetired* sh = Arm(insn, imm);
  Log("big model pages: ready (page size %u bytes when on; %u small pages retired by an earlier payload of this "
      "process%s)",
      g_bigBytes, sh ? sh->count : 0, sh ? "" : "; no shared list (named mapping failed): small pages stay in use");
  return true;
}

// Takes effect at the next per-frame reset of the model data pages.
void SetOn(bool on) {
  if (g_state.load() <= 0) return;
  g_wanted = on;
}

// R63: big pages off in a payload that never installed them. What an earlier payload of this process left at
// `insn` (page size patched) and in the shared list (small pages retired) is undone: through this payload's reset
// hook when it has one (`resetHook`: armed with g_wanted false, the next reset writes the stock size and gives the
// pages back), else the page size alone at once (the pages need the reset: retried while any stay retired).
// Returns true when nothing is left to do.
std::atomic<bool> g_undoDone{false};
bool g_undoConstLogged = false;
bool UndoAt(uint8_t* insn, uint32_t imm, bool resetHook) {
  SharedRetired* sh = SharedList();
  const uint32_t retired = sh ? sh->count : 0;
  const bool patched = imm != kStockBytes;
  if (!patched && !retired) return true;
  if (resetHook) {
    g_wanted = false;
    Arm(insn, imm);
    Log("big model pages: off; an earlier payload left page size %u and %u small pages retired: undone at the next "
        "reset", imm, retired);
    return true;
  }
  if (patched) {
    uint8_t* const insn0 = g_insn;
    g_insn = insn;
    const bool ok = WriteConst(kStockBytes);
    g_insn = insn0;
    if (ok && !g_undoConstLogged) {
      g_undoConstLogged = true;
      Log("big model pages: off; page size put back to stock (left at %u by an earlier payload); %u retired small "
          "pages wait for the model allocator's reset hook", imm, retired);
    }
  }
  return retired == 0;
}
void UndoLeftover() {
  if (g_undoDone.load() || g_state.load() != 0) return;
  uint32_t imm = 0;
  bool fatal = false;
  uint8_t* insn = FindInsn(&imm, &fatal);
  if (!insn) {
    if (fatal) g_undoDone = true;  // another build: nothing of ours to undo there
    return;
  }
  if (UndoAt(insn, imm, allocslab::g_state.load() >= 1)) g_undoDone = true;
}

}  // namespace bigpages
