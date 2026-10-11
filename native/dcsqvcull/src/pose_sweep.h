// Test-only synthetic head motion. Hooks the Quad-Views-Foveated layer's
// xrLocateViews (the function DCS calls) and rotates every returned view pose
// about the vertical axis by a time-varying yaw, so DCS renders exactly as if
// the head were turning: same culling, LOD and streaming work. Used only
// while a motion profile records with [Suite] MotionSweep=1, by the suite's
// YawScan / YawProfile (held yaw) and RotationProfile (constant-rate turn),
// and by [Suite] HoldYawDeg; off otherwise. The headset shows a swinging image
// while it runs.
//
// The detour follows the allocator detour's rules: code and target pointer on
// separate pages, and the 14-byte patch is written with every other thread of
// the process suspended and outside the patched bytes.
// Included once from main.cpp inside its anonymous namespace.
#pragma once

namespace posesweep {

const wchar_t* const kModule = L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll";
constexpr uint32_t kRva = 0x1aa70;  // DcsVrControl's bundled QVFR build of 2026-10-07
const uint8_t kPrologue[17] = {0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41,
                               0x57, 0x48, 0x81, 0xEC, 0xE8, 0x00, 0x00, 0x00};
constexpr size_t kPage = 4096;

using Fn = int32_t(__fastcall*)(void* session, const void* info, void* state, uint32_t cap, uint32_t* count,
                                uint8_t* views);
Fn g_tramp = nullptr;
void** g_slot = nullptr;
std::atomic<bool> g_on{false};
std::atomic<int> g_state{0};
std::atomic<int64_t> g_t0{0};
std::atomic<double> g_yawDeg{0};

// Why the sweep did not rotate (2026-10-09 runs): YawAt returned the held yaw
// whenever a hold was active and 0 whenever [Suite] MotionTaxi=1, so with
// MotionTaxi=1 (the dev ini) every frame was "still" while the head pose slid
// forward at 15 m/s; and the sine sweep also slid the pose 1.5 km forward after
// 67 s. Now the time-varying yaw is independent of both: it is added to the
// held yaw (0 when not holding), and the pose moves only with MotionTaxi=1.
std::atomic<bool> g_taxi{false};    // [Suite] MotionTaxi: also slide the pose forward (15 m/s from t = 5 s)
std::atomic<bool> g_sweep{false};   // a time-varying yaw is running (Start .. Stop)
std::atomic<double> g_rateDps{0};   // > 0: constant-rate rotation (RotationProfile) instead of the sine sweep

// Held yaw ([Suite] HoldYawDeg / YawScan / YawProfile): a constant view
// rotation, no translation; < -1000 = not holding. Used to aim an unattended
// benchmark view, and the base the sweep rotates about.
std::atomic<double> g_hold{-2000.0};
bool Holding() { return g_hold.load(std::memory_order_relaxed) > -1000.0; }

// Pure parts (offline tested). Sine sweep phases (seconds): still, slow,
// medium, fast, still. Yaw = A*sin(2*pi*t/T).
inline double SweepYaw(double t) {
  const double A = 60.0;
  auto sweep = [&](double tt, double period) { return A * std::sin(2.0 * 3.14159265358979 * tt / period); };
  if (t < 5) return 0;
  if (t < 25) return sweep(t - 5, 8.0);   // peak about 47 deg/s
  if (t < 45) return sweep(t - 25, 4.0);  // peak about 94 deg/s
  if (t < 65) return sweep(t - 45, 2.0);  // peak about 188 deg/s
  return 0;
}
// Yaw in degrees: hold base (0 when not holding) plus the running sweep:
// rate > 0 = a constant-rate turn (wrapped to [0, 360)), else the sine sweep.
inline double ComposeYaw(bool holding, double hold, bool sweeping, double rateDps, double t) {
  const double base = holding ? hold : 0.0;
  if (!sweeping) return base;
  if (rateDps > 0) return base + std::fmod(rateDps * (t > 0 ? t : 0.0), 360.0);
  return base + SweepYaw(t);
}
// Forward slide in metres (-Z of the view): only a running sweep with MotionTaxi.
inline double ComposeForward(bool sweeping, bool taxi, double t) {
  if (!sweeping || !taxi) return 0;
  return t < 5 ? 0 : (t - 5) * 15.0;
}

double YawAt(double t) {
  return ComposeYaw(Holding(), g_hold.load(std::memory_order_relaxed), g_sweep.load(std::memory_order_relaxed),
                    g_rateDps.load(std::memory_order_relaxed), t);
}
double ForwardAt(double t) {
  return ComposeForward(g_sweep.load(std::memory_order_relaxed), g_taxi.load(std::memory_order_relaxed), t);
}

int32_t __fastcall Hook(void* session, const void* info, void* state, uint32_t cap, uint32_t* count, uint8_t* views) {
  int32_t r = g_tramp(session, info, state, cap, count, views);
  if (r < 0 || !g_on.load(std::memory_order_relaxed) || !views || !count) return r;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  double t = (now.QuadPart - g_t0.load()) * g_qpcToUs / 1e6;
  const double yawDeg = YawAt(t);
  double yaw = yawDeg * 3.14159265358979 / 180.0;
  const double fwd = ForwardAt(t);
  g_yawDeg = yawDeg;
  const float s = static_cast<float>(std::sin(yaw / 2)), c = static_cast<float>(std::cos(yaw / 2));
  const uint32_t n = std::min(*count, cap);
  for (uint32_t i = 0; i < n; ++i) {
    float* q = reinterpret_cast<float*>(views + i * 64 + 16);  // XrView.pose.orientation x,y,z,w
    float* p = q + 4;                                          // pose.position x,y,z
    // q' = qy * q with qy = (0, s, 0, c)
    float x = q[0], y = q[1], z = q[2], w = q[3];
    q[0] = c * x + s * z;
    q[1] = c * y + s * w;
    q[2] = c * z - s * x;
    q[3] = c * w - s * y;
    // p' = rotate p about Y
    float cy = static_cast<float>(std::cos(yaw)), sy = static_cast<float>(std::sin(yaw));
    float px = p[0], pz = p[2];
    p[0] = cy * px + sy * pz;
    p[2] = -sy * px + cy * pz - static_cast<float>(fwd);
  }
  return r;
}

// Suspends every other thread, checks that none is inside [addr, addr+len),
// writes, resumes. Retries a few times.
bool PatchAllSuspended(uint8_t* addr, const uint8_t* bytes, size_t len) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    std::vector<HANDLE> held;
    held.reserve(4096);  // no heap allocation while threads are suspended (one may hold the heap lock)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te{sizeof(te)};
    bool clear = true;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
      if (!h) continue;
      if (SuspendThread(h) == static_cast<DWORD>(-1)) {
        CloseHandle(h);
        continue;
      }
      held.push_back(h);
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(h, &ctx)) {
        uintptr_t rip = ctx.Rip, a = reinterpret_cast<uintptr_t>(addr);
        if (rip > a && rip < a + len) clear = false;
      }
    }
    CloseHandle(snap);
    bool done = false;
    if (clear) {
      DWORD old;
      if (VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(addr, bytes, len);
        VirtualProtect(addr, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), addr, len);
        done = true;
      }
    }
    for (HANDLE h : held) {
      ResumeThread(h);
      CloseHandle(h);
    }
    if (done) return true;
    Sleep(2);
  }
  return false;
}

bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  g_state = -1;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(kModule));
  if (!base) {
    Log("pose sweep: Quad-Views-Foveated layer not loaded; skipped");
    return false;
  }
  uint8_t* target = base + kRva;
  uint8_t cur[17];
  if (!allocslab::ReadBytes(target, cur, 17)) return false;
  if (cur[0] == 0xFF && cur[1] == 0x25) {
    // Patched by an earlier payload: its jump leads to the stub page; the
    // slot is on the next page and the trampoline right after the stub.
    uint8_t* page = nullptr;
    memcpy(&page, cur + 6, 8);
    // Only our own layout (jmp [rip+0] to a page whose stub jumps through
    // page+4096 and whose trampoline holds the original prologue): another
    // tool's absolute jump here made us write the hook to target+4096.
    uint8_t stub[6] = {}, tramp[17] = {};
    const int32_t wantRel = static_cast<int32_t>(kPage - 6);
    if (cur[2] || cur[3] || cur[4] || cur[5] || !page || !allocslab::ReadBytes(page, stub, 6) ||
        !allocslab::ReadBytes(page + 16, tramp, 17) || stub[0] != 0xFF || stub[1] != 0x25 ||
        memcmp(stub + 2, &wantRel, 4) != 0 || memcmp(tramp, kPrologue, 17) != 0) {
      Log("pose sweep: xrLocateViews carries an unknown patch; skipped");
      return false;
    }
    g_slot = reinterpret_cast<void**>(page + kPage);
    g_tramp = reinterpret_cast<Fn>(static_cast<void*>(page + 16));
  } else if (memcmp(cur, kPrologue, 17) == 0) {
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 * kPage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!page) return false;
    memset(page, 0xCC, kPage);
    // stub at 0: jmp [rip -> slot]
    int32_t rel = static_cast<int32_t>(kPage - 6);
    page[0] = 0xFF;
    page[1] = 0x25;
    memcpy(page + 2, &rel, 4);
    // trampoline at 16: original 17 bytes + jmp back
    uint8_t* tr = page + 16;
    memcpy(tr, kPrologue, 17);
    tr[17] = 0xFF;
    tr[18] = 0x25;
    memset(tr + 19, 0, 4);
    uint8_t* back = target + 17;
    memcpy(tr + 23, &back, 8);
    g_slot = reinterpret_cast<void**>(page + kPage);
    *g_slot = tr;  // pass-through until switched on
    DWORD old;
    VirtualProtect(page, kPage, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, kPage);
    g_tramp = reinterpret_cast<Fn>(static_cast<void*>(tr));
    uint8_t patch[14] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(patch + 6, &page, 8);
    if (!PatchAllSuspended(target, patch, 14)) {
      Log("pose sweep: could not patch xrLocateViews safely; skipped");
      return false;
    }
  } else {
    Log("pose sweep: Quad-Views-Foveated build does not match; skipped");
    return false;
  }
  InterlockedExchangePointer(g_slot, reinterpret_cast<void*>(&Hook));
  g_state = 1;
  Log("pose sweep: hooked Quad-Views-Foveated xrLocateViews");
  return true;
}

// Starts the time-varying yaw about the held yaw (or 0): rateDps > 0 = a
// constant-rate turn, else the sine sweep. False when the hook is unavailable.
bool Start(double rateDps = 0) {
  if (!Install()) return false;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  g_rateDps = rateDps > 0 ? rateDps : 0.0;
  g_t0 = now.QuadPart;
  g_sweep = true;
  g_on = true;
  return true;
}

// Ends the time-varying yaw; a hold stays.
void Stop() {
  g_sweep = false;
  g_rateDps = 0.0;
  g_on = Holding();
}

// Holds a constant yaw (degrees), or releases it (hold = false); a running
// sweep keeps running about the new base.
void Hold(bool hold, double deg) {
  if (!hold) {
    g_hold = -2000.0;
    g_on = g_sweep.load();
    return;
  }
  if (!Install()) return;
  g_hold = deg;
  g_on = true;
}

// Payload stop: hold and sweep off and the stub's slot back to the trampoline
// (pass-through). The old payload's hook stays loaded but is no longer called,
// so a hold of an older payload generation cannot outlive it (the new payload
// re-applies [Suite] HoldYawDeg from its ini).
void Release() {
  g_sweep = false;
  g_hold = -2000.0;
  g_on = false;
  if (g_state.load() > 0 && g_slot && g_tramp) InterlockedExchangePointer(g_slot, reinterpret_cast<void*>(g_tramp));
}

}  // namespace posesweep
