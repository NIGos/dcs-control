// Clock reads inside edCore's SyncTaskQueue::proceed (RVA 0x37b40). proceed
// runs queued render-thread tasks and calls ED_get_time after every task to
// check its time budget. The frame-start drain (make_render_thread_tasks)
// passes a budget of 1e10 s, so those checks can never stop it, yet with
// terrain and texture streaming (camera translating) they cost about 19% of
// the render thread in QueryPerformanceCounter.
//
// The four `call ED_get_time` sites in proceed are redirected to a stub: when
// the budget (xmm7, set by proceed before the loop) is above 1e9 s and the
// feature is on, it returns the timer cache's value (refreshed every 1 ms);
// otherwise it jumps to the real ED_get_time, so queues with a finite budget
// (the 0.5 ms terrain queue) keep exactly their scheduling.
// Included once from main.cpp inside its anonymous namespace, after pacer.h
// and pose_sweep.h.
#pragma once

namespace edtime {

constexpr uint32_t kGetTimeRva = 0xa3750;
const uint32_t kSites[4] = {0x37b6d, 0x37baa, 0x37bca, 0x37c1c};
const uint8_t kGetTimePrologue[8] = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x77, 0xFF, 0xFF};
constexpr size_t kPage = 4096;
constexpr uint32_t kMagic = 0x54515044;  // "DPQT"

std::atomic<int> g_state{0};
uint8_t* g_flag = nullptr;  // data page: 1 = return the cached time for unlimited budgets

bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  if (!timercache::g_orig) return false;  // not yet: retried once the timer cache is installed
  g_state = -1;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"edCore.dll"));
  if (!base) return false;
  // Recorded RVAs, or where reloc.h re-found them in another build (0: not found).
  // In another build the prologue's rel32 bytes are not compared.
  const uint32_t getTimeRva = reloc::Rva(hooksig::ED_ED_get_time, kGetTimeRva);
  const uint32_t sites[4] = {reloc::Rva(hooksig::ED_SyncTaskQueue_proceed_getTimeCall0, kSites[0]),
                             reloc::Rva(hooksig::ED_SyncTaskQueue_proceed_getTimeCall1, kSites[1]),
                             reloc::Rva(hooksig::ED_SyncTaskQueue_proceed_getTimeCall2, kSites[2]),
                             reloc::Rva(hooksig::ED_SyncTaskQueue_proceed_getTimeCall3, kSites[3])};
  uint8_t* getTime = base + getTimeRva;
  uint8_t pro[8];
  if (!getTimeRva || !allocslab::ReadBytes(getTime, pro, 8) ||
      !reloc::BytesAre(hooksig::ED_ED_get_time, pro, kGetTimePrologue, 8)) {
    Log("task-queue clock: edCore.dll does not match this build; skipped");
    return false;
  }
  // All four sites must call ED_get_time (fresh) or the same existing stub (reload).
  uint8_t* existing = nullptr;
  bool fresh = true;
  for (uint32_t rva : sites) {
    uint8_t* site = base + rva;
    uint8_t b[5];
    if (!rva || !allocslab::ReadBytes(site, b, 5) || b[0] != 0xE8) {
      Log("task-queue clock: unexpected bytes at edCore+0x%x; skipped", rva);
      return false;
    }
    int32_t rel;
    memcpy(&rel, b + 1, 4);
    uint8_t* target = site + 5 + rel;
    if (target == getTime) continue;
    fresh = false;
    if (existing && existing != target) return false;
    existing = target;
  }
  if (!fresh) {
    uint32_t magic = 0;
    if (!allocslab::ReadBytes(existing + 128, &magic, 4) || magic != kMagic) {
      Log("task-queue clock: unknown patch in SyncTaskQueue::proceed; skipped");
      return false;
    }
    g_flag = existing + kPage;
    // The stub's cached-time pointer (mov rax, imm64 at +21) is the earlier
    // payload's timer cache, whose updater stopped with it: keep it fresh.
    void* cached = nullptr;
    if (allocslab::ReadBytes(existing + 21, &cached, 8) && cached && cached != &timercache::g_cached)
      timercache::g_mirror = static_cast<std::atomic<double>*>(cached);
    g_state = 1;
    Log("task-queue clock: existing patch reused");
    return true;
  }
  uint8_t* page = pacer::AllocNear(base);
  if (!page) return false;
  memset(page, 0xCC, kPage);
  g_flag = page + kPage;
  *g_flag = 0;
  uint8_t* p = page;
  auto rel32 = [](uint8_t* from_next, const void* to) {
    return static_cast<int32_t>(static_cast<const uint8_t*>(to) - from_next);
  };
  const double kUnlimited = 1e9;
  memcpy(page + 64, &kUnlimited, 8);
  *reinterpret_cast<uint32_t*>(page + 128) = kMagic;
  // cmp byte ptr [rip+flag], 0
  p[0] = 0x80; p[1] = 0x3D;
  int32_t r = rel32(page + 7, g_flag);
  memcpy(p + 2, &r, 4);
  p[6] = 0x00;
  // je real (+25)
  p[7] = 0x74; p[8] = 25;
  // comisd xmm7, [rip+1e9]
  p[9] = 0x66; p[10] = 0x0F; p[11] = 0x2F; p[12] = 0x3D;
  r = rel32(page + 17, page + 64);
  memcpy(p + 13, &r, 4);
  // jbe real (+15)
  p[17] = 0x76; p[18] = 15;
  // mov rax, &timercache::g_cached ; movsd xmm0, [rax] ; ret
  p[19] = 0x48; p[20] = 0xB8;
  void* cached = &timercache::g_cached;
  memcpy(p + 21, &cached, 8);
  p[29] = 0xF2; p[30] = 0x0F; p[31] = 0x10; p[32] = 0x00;
  p[33] = 0xC3;
  // real: mov rax, ED_get_time ; jmp rax
  p[34] = 0x48; p[35] = 0xB8;
  memcpy(p + 36, &getTime, 8);
  p[44] = 0xFF; p[45] = 0xE0;
  DWORD old;
  VirtualProtect(page, kPage, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, kPage);
  for (uint32_t rva : sites) {
    uint8_t* site = base + rva;
    uint8_t b[5] = {0xE8};
    int32_t rr = rel32(site + 5, page);
    memcpy(b + 1, &rr, 4);
    if (!posesweep::PatchAllSuspended(site, b, 5)) {
      Log("task-queue clock: could not patch edCore+0x%x safely", rva);
      return false;  // sites already patched keep working: the stub falls through to ED_get_time while off
    }
  }
  g_state = 1;
  Log("task-queue clock: SyncTaskQueue::proceed clock reads redirected (unlimited budgets only)");
  return true;
}

void SetOn(bool on) {
  if (g_state.load() <= 0 || !g_flag) return;
  *reinterpret_cast<volatile uint8_t*>(g_flag) = on ? 1 : 0;
}

}  // namespace edtime
