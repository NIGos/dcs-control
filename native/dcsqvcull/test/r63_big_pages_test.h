// R63 bigpages-left-patched-after-reload: a payload with big model pages off undoes what an earlier payload of
// this process left (page size patched, small pages retired), and a stopped payload's leftover reset hook stops
// retiring (big_pages.h UndoAt / UndoLeftover, main.cpp PassThroughAfterStop).
#pragma once

namespace r63big {

using namespace bigpages;

constexpr uint32_t kPages = 24;

struct Rig {
  std::vector<uint8_t> pages = std::vector<uint8_t>(kPages * allocslab::kPageStride, 0);
  uint64_t mgr64[8] = {};
  uint8_t* mgr = reinterpret_cast<uint8_t*>(mgr64);
  std::vector<uint32_t> cap0 = std::vector<uint32_t>(kPages);
  Rig() {
    for (uint32_t i = 0; i < kPages; ++i) {
      allocslab::Page* pg = At(i);
      pg->idx = i;
      pg->elemSize = i & 1 ? 96 : 112;
      pg->capCount = 0xfe00 / pg->elemSize;
      pg->capBytes = pg->capCount * pg->elemSize;
      cap0[i] = pg->capBytes;
    }
    *reinterpret_cast<uint8_t**>(mgr + 8) = pages.data();
    *reinterpret_cast<uint8_t**>(mgr + 0x10) = pages.data() + pages.size();
  }
  allocslab::Page* At(uint32_t i) { return reinterpret_cast<allocslab::Page*>(pages.data() + i * allocslab::kPageStride); }
  uint32_t Retired() {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kPages; ++i) n += At(i)->capBytes == 0;
    return n;
  }
  bool AllBack() {
    for (uint32_t i = 0; i < kPages; ++i)
      if (At(i)->capBytes != cap0[i]) return false;
    return true;
  }
};

uint32_t Imm(const uint8_t* insn) {
  uint32_t v;
  memcpy(&v, insn + 1, 4);
  return v;
}

void Run() {
  // Saved state (this test plays two payload generations on the same globals).
  uint8_t* const insn0 = g_insn;
  const bool want0 = g_wanted.load(), applied0 = g_applied, undo0 = g_undoDone.load();
  const int state0 = g_state.load();
  const uint32_t big0 = g_bigBytes;
  SharedRetired* const shared0 = g_shared;
  void (*const onReset0)(void*) = allocslab::g_onReset;
  const bool tc0 = timercache::g_on.load(), fh0 = g_frameHeapOn.load(), as0 = g_allocSlabsOn.load();
  SharedRetired* sh = SharedList();
  if (!sh || sh->count != 0) {
    printf("SKIP R63 big pages: shared list unavailable or in use\n");
    return;
  }
  alignas(16) static uint8_t insn[5] = {0xB8, 0x00, 0xFE, 0x00, 0x00};

  // Payload A: big pages on, small pages retired.
  {
    Rig r;
    g_insn = insn;
    g_bigBytes = 4u << 20;
    g_state = 1;
    g_applied = false;
    g_wanted = true;
    OnReset(r.mgr);
    const bool aOn = Imm(insn) == (4u << 20) && r.Retired() == kPages && sh->count == kPages;
    // Payload A stops; payload B (BigModelPages=0) has its own reset hook: A's leftovers undone at B's next reset.
    PassThroughAfterStop();
    const bool aWantCleared = !g_wanted.load();
    g_state = 0;  // B's statics
    g_insn = nullptr;
    g_applied = false;
    allocslab::g_onReset = nullptr;
    const bool armed = UndoAt(insn, Imm(insn), true) && g_state.load() == 1 && allocslab::g_onReset == &OnReset &&
                       !g_wanted.load();
    OnReset(r.mgr);
    Check(aOn && aWantCleared && armed && Imm(insn) == kStockBytes && r.AllBack() && sh->count == 0,
          "R63 big pages: a next payload with big pages off arms its reset hook and restores the stock page size and "
          "every page an earlier payload retired");
    // Nothing left: not armed again, nothing written.
    g_state = 0;
    allocslab::g_onReset = nullptr;
    Check(UndoAt(insn, Imm(insn), true) && g_state.load() == 0 && allocslab::g_onReset == nullptr,
          "R63 big pages: nothing left by an earlier payload: big pages stay uninstalled");
  }

  // Payload A's leftover reset hook after Stop (still in the vtable): restores instead of retiring more.
  {
    Rig r;
    g_insn = insn;
    g_state = 1;
    g_applied = false;
    g_wanted = true;
    OnReset(r.mgr);
    PassThroughAfterStop();
    OnReset(r.mgr);  // A's hook, after Stop
    Check(Imm(insn) == kStockBytes && r.AllBack() && sh->count == 0,
          "R63 big pages: a stopped payload's reset hook gives the pages and the stock page size back (was: kept "
          "retiring with g_wanted set)");
  }

  // B without the model allocator's reset hook: the page size goes back at once; the pages wait (not done).
  {
    Rig r;
    g_insn = insn;
    g_state = 1;
    g_applied = false;
    g_wanted = true;
    OnReset(r.mgr);
    g_state = 0;
    g_insn = nullptr;
    allocslab::g_onReset = nullptr;
    const bool done = UndoAt(insn, Imm(insn), false);
    const bool constBack = Imm(insn) == kStockBytes && g_state.load() == 0 && r.Retired() == kPages;
    // The reset hook arrives (worker retry): armed, the pages come back.
    const bool done2 = UndoAt(insn, Imm(insn), true);
    OnReset(r.mgr);
    Check(!done && constBack && done2 && r.AllBack() && sh->count == 0,
          "R63 big pages: no reset hook yet: the page size is put back at once and the retired pages once the hook "
          "is there");
  }

  // The reset reads the instruction: another generation restored it meanwhile, this one still wants big pages.
  {
    Rig r;
    g_insn = insn;
    g_state = 1;
    g_applied = true;  // stale belief
    g_wanted = true;
    memcpy(insn, kStockInsn, 5);
    OnReset(r.mgr);
    const bool reapplied = Imm(insn) == (4u << 20) && r.Retired() == kPages;
    g_wanted = false;
    OnReset(r.mgr);
    Check(reapplied && Imm(insn) == kStockBytes && r.AllBack(),
          "R63 big pages: the reset trusts the instruction, not its cached state (written back by another payload)");
  }

  sh->count = 0;
  g_insn = insn0;
  g_wanted = want0;
  g_applied = applied0;
  g_undoDone = undo0;
  g_state = state0;
  g_bigBytes = big0;
  g_shared = shared0;
  allocslab::g_onReset = onReset0;
  timercache::g_on = tc0;
  g_frameHeapOn = fh0;
  g_allocSlabsOn = as0;
}

}  // namespace r63big
