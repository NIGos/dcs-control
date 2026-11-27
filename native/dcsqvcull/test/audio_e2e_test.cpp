// End-to-end offline test of 3D headphone audio (docs/BINAURAL_AUDIO.md 8.3, integration): the audio module of the
// payload (src/main/audio.inc) on the fake ed_sound image (audio_fake_ed.h), with the REAL BinauralCore
// (native/binaural/src, the library the payload links) and the synthetic spherical-head HRTF of the core's harness
// (native/binaural/test/synthetic_hrtf.h, so nothing is downloaded). Fake voices go through ed_sound's call sites, the
// hooks own them and feed the core, the core's stereo comes out of ed_sound's mix:
//   1. a broadband source circling the head at 45 deg/s, 5 m away: the interaural time and level differences of the
//      hooked output, measured in 80 ms windows, follow the trajectory: the same as the core driven directly with the
//      same signal and positions, Woodworth's ITD for the configured head size, zero in front, mirror-symmetric
//      left / right, the far ear quieter;
//   2. the radio (a sound in a 2D context) next to an owned 3D source: exactly centred and bit-identical to DCS's own
//      mix at its frequency, while the 3D source is panned by the core;
//   3. no heap allocation on the simulation / render thread with the real core, every sample finite;
//   4. stereo waves (point sources, loudness, kill switch) and wide voices: the kept side reaches each ear with the HRTF
//      mid, ITD / ILD at the sides, level against DCS's own output by direction, no click between point and wide;
//   5. the cockpit space with the core's Room (docs/COCKPIT_SPACE.md 13.2): direct-to-room ratio and interaural
//      coherence per preset, level against the kill switch and against Off, no click on any live change, Off
//      bit-identical to no Room, CPU at 48 voices. Scenarios 1-4 run with CockpitSpace=0 (the direct path).
// Built as DcsQvCullAudioE2ETests (ctest); the module's own scenarios run against the stand-in core in DcsQvCullTests.
#include "../src/main.cpp"
#include "audio_fake_core.h"  // only the real-time allocation counter (audio_rt_alloc.cpp) is linked here
#include "synthetic_hrtf.h"   // native/binaural/test

#include <complex>
#include <functional>
#include <memory>
#include <string>

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}
}  // namespace

#include "audio_rig.h"

namespace e2e {

using namespace audiotest;

constexpr uint32_t kRate = 48000, kFrames = 480;  // 10 ms blocks; one DCS sound frame per block
constexpr double kPi = 3.14159265358979323846;
constexpr double kC = 343.0;
constexpr float kCircumferenceCm = 57.0f;
const char* kIni =
    "[Audio]\r\nBinaural=1\r\nHrtf=default\r\nHeadCircumferenceCm=57\r\nLateHeadTracking=0\r\nNearField=1\r\n"
    "MaxObjects=48\r\nCockpitSpace=0\r\n";  // scenarios 1-4 measure the direct path; the space (default Subtle) is 5's

std::vector<uint32_t> g_set;  // the synthetic set (.dcshrtf image, 4-byte words)
bool SyntheticHrtf(const wchar_t* name, std::vector<uint8_t>* out) {
  if (wcscmp(name, L"default")) return false;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(g_set.data());
  out->assign(p, p + g_set.size() * 4);
  return true;
}

void SetContext2D(Rig& r, int ctx, bool is2d) { W<uint8_t>(r.im.base + kCtxArray + 0x388 * ctx, 0xe0, is2d ? 1 : 0); }

// Interaural time difference (seconds, > 0 when the left ear lags: source on the right) by cross-correlation within
// +-1.25 ms, refined by a parabola through the peak; level difference right / left (dB, > 0: source on the right).
struct Cue {
  double itd = 0, ild = 0;
};
Cue Measure(const float* lr, size_t frames) {
  constexpr int kMaxLag = 60;
  double best = -1e300;
  int bestLag = 0;
  std::vector<double> corr(2 * kMaxLag + 1);
  for (int k = -kMaxLag; k <= kMaxLag; ++k) {
    double s = 0;
    for (size_t n = kMaxLag; n + kMaxLag < frames; ++n) s += static_cast<double>(lr[2 * n]) * lr[2 * (n - k) + 1];
    corr[k + kMaxLag] = s;
    if (s > best) best = s, bestLag = k;
  }
  double frac = 0;
  if (bestLag > -kMaxLag && bestLag < kMaxLag) {
    const double a = corr[bestLag + kMaxLag - 1], b = corr[bestLag + kMaxLag], c = corr[bestLag + kMaxLag + 1];
    const double den = a - 2 * b + c;
    if (den < 0) frac = 0.5 * (a - c) / den;
  }
  double el = 0, er = 0;
  for (size_t n = 0; n < frames; ++n) el += static_cast<double>(lr[2 * n]) * lr[2 * n], er += static_cast<double>(lr[2 * n + 1]) * lr[2 * n + 1];
  Cue q;
  q.itd = (bestLag + frac) / kRate;
  q.ild = 10.0 * std::log10((er + 1e-30) / (el + 1e-30));
  return q;
}

// Woodworth's ITD of a horizontal-plane source at azimuth `deg` (0 ahead, 90 right) for head radius r.
double Woodworth(double deg, double r) {
  const double phi = std::asin(std::sin(deg * kPi / 180.0));  // lateral angle, -90..90
  return r / kC * (phi + std::sin(phi));
}

bool AllFinite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}

// ---- 1. a source circling the head -----------------------------------------------------------------------------
void Circle() {
  Rig r;
  RigUp(r, kIni);
  const uint32_t seed = 5;
  const float amp = 0.3f;
  AddBoth(r, 0, true, 1, kNoise, amp, 0, 1.0f, seed);
  PosBoth(r, 0, 5, 0, 0);
  CreateVoice(r, 0);

  // The same core driven directly: same set, rate, head radius; the same noise (the fake voice's generator, same seed)
  // and the same positions, block by block.
  char err[160] = {};
  binaural::HrtfSet* set = binaural::LoadHrtf(g_set.data(), g_set.size() * 4, kRate, err, sizeof(err));
  // The head radius the module gives the core: the set's own fitted radius scaled by the circumference (57 cm: 1).
  const float radius = binaural_config::PilotItdRadiusM(set ? binaural::DescribeHrtf(set).datasetHeadRadiusM : 0.0f, kCircumferenceCm);
  auto direct = std::make_unique<binaural::Renderer>();
  binaural::Config cc;
  cc.sampleRate = kRate;
  cc.maxSources = 4;
  cc.headRadiusM = radius;
  cc.earRadiusM = binaural_config::HeadRadiusFromCircumferenceCm(kCircumferenceCm);
  cc.nearField = true;
  const bool directOk = set && direct->Init(cc, err, sizeof(err));
  if (directOk) direct->SetHrtf(set);
  if (!directOk) printf("     direct core: %s\n", err);
  std::vector<uint8_t> gen(kVoiceSize, 0);
  W<uint32_t>(gen.data(), kVKind, kNoise);
  W<float>(gen.data(), kVAmp, amp);
  W<uint32_t>(gen.data(), kVRng, seed);
  bool directFirst = true;
  std::vector<float> mono(kFrames);
  auto directBlock = [&](const float pos[3], float* out) {
    for (uint32_t i = 0; i < kFrames; ++i) mono[i] = NextSample(gen.data(), nullptr);
    memset(out, 0, sizeof(float) * 2 * kFrames);
    if (!directOk) return;
    binaural::SourceUpdate u{{pos[0], pos[1], pos[2]}, 1.0f, directFirst ? binaural::source_flags::kReset : 0u};
    directFirst = false;
    direct->BeginBlock(kFrames);
    direct->Render(0, u, {mono.data(), nullptr, 1, 1.0f}, kFrames, 1.0f, 1.0f, out);
    direct->EndBlock();
  };

  std::vector<float> a(2 * kFrames), b(2 * kFrames), d(2 * kFrames), hooked, directAll;
  const float front[3] = {5, 0, 0};
  Block(r, a.data(), b.data(), kFrames);  // the first block binds the renderer (control thread)
  directBlock(front, d.data());
  Tick();
  for (int k = 0; k < 30; ++k) {  // settle in front
    SimFrame(r);
    Block(r, a.data(), b.data(), kFrames);
    directBlock(front, d.data());
  }
  // 45 deg/s, clockwise seen from above (front, right, back, left): 800 blocks of 0.45 deg, block k at (k + 0.5) steps,
  // so the 80 ms windows centre on 1.8, 5.4, ... 358.2 deg (each has its mirror at 360 - x and 180 - x).
  constexpr int kBlocks = 800;
  constexpr double kStepDeg = 360.0 / kBlocks;
  hooked.reserve(size_t(2) * kFrames * kBlocks);
  directAll.reserve(size_t(2) * kFrames * kBlocks);
  audiofake::g_rtAllocs = 0;
  for (int k = 0; k < kBlocks; ++k) {
    const double th = (k + 0.5) * kStepDeg * kPi / 180.0;
    const float pos[3] = {static_cast<float>(5 * std::cos(th)), 0.0f, static_cast<float>(5 * std::sin(th))};
    PosBoth(r, 0, pos[0], pos[1], pos[2]);
    audiofake::t_rtThread = true;  // counted: ed_sound's frame (the hooks' simulation side) and the mix (render side)
    SimFrame(r);
    Block(r, a.data(), b.data(), kFrames);
    audiofake::t_rtThread = false;
    directBlock(pos, d.data());
    hooked.insert(hooked.end(), a.begin(), a.end());
    directAll.insert(directAll.end(), d.begin(), d.end());
  }
  const uint64_t allocs = audiofake::g_rtAllocs.load();
  const uint32_t owned = audio::g_cnt.ownedNow.load();
  const uint64_t hrtfCalls = audio::g_cnt.hrtfCalls.load();

  // 80 ms windows (8 blocks, 3.6 deg); the azimuth of a window is its middle.
  constexpr int kWin = 8;
  const int windows = kBlocks / kWin;
  std::vector<double> az(windows);
  std::vector<Cue> hq(windows), dq(windows);
  double worstItdDirect = 0, worstIldDirect = 0, worstItdWood = 0;
  for (int w = 0; w < windows; ++w) {
    az[w] = (w * kWin + kWin / 2.0) * kStepDeg;
    hq[w] = Measure(hooked.data() + 2 * size_t(w) * kWin * kFrames, size_t(kWin) * kFrames);
    dq[w] = Measure(directAll.data() + 2 * size_t(w) * kWin * kFrames, size_t(kWin) * kFrames);
    worstItdDirect = std::max(worstItdDirect, std::fabs(hq[w].itd - dq[w].itd));
    worstIldDirect = std::max(worstIldDirect, std::fabs(hq[w].ild - dq[w].ild));
    worstItdWood = std::max(worstItdWood, std::fabs(hq[w].itd - Woodworth(az[w], radius)));
  }
  printf("     azimuth  ITD hooked  ITD direct  Woodworth   ILD hooked  ILD direct\n");
  for (int w = 0; w < windows; w += 5)
    printf("     %6.1f  %8.1f us  %8.1f us  %7.1f us  %8.2f dB  %8.2f dB\n", az[w], hq[w].itd * 1e6, dq[w].itd * 1e6,
           Woodworth(az[w], radius) * 1e6, hq[w].ild, dq[w].ild);
  printf("     owned %u, HRTF calls %llu, allocations on the audio threads %llu; worst vs direct core: ITD %.1f us, "
         "ILD %.2f dB; worst ITD vs Woodworth (r = %.2f cm): %.1f us\n",
         owned, static_cast<unsigned long long>(hrtfCalls), static_cast<unsigned long long>(allocs), worstItdDirect * 1e6,
         worstIldDirect, radius * 100.0, worstItdWood * 1e6);

  // Symmetry: the window at azimuth x against the one at 360 - x (left / right mirror) and 180 - x (front / back).
  const double winDeg = kWin * kStepDeg;
  auto index = [&](double deg) {  // the window centred on `deg` (one of 1.8, 5.4, ...), or -1
    deg = std::fmod(deg + 360.0, 360.0);
    const int w = static_cast<int>(std::lround(deg / winDeg - 0.5));
    return w >= 0 && w < windows && std::fabs(az[w] - deg) < 0.01 ? w : -1;
  };
  double worstMirrorItd = 0, worstMirrorIld = 0, worstFrontBackItd = 0;
  int pairs = 0;
  for (int w = 0; w < windows; ++w) {
    const int m = index(360.0 - az[w]), f = index(180.0 - az[w]);
    if (m >= 0) {
      worstMirrorItd = std::max(worstMirrorItd, std::fabs(hq[w].itd + hq[m].itd));
      worstMirrorIld = std::max(worstMirrorIld, std::fabs(hq[w].ild + hq[m].ild));
      ++pairs;
    }
    if (f >= 0) worstFrontBackItd = std::max(worstFrontBackItd, std::fabs(hq[w].itd - hq[f].itd)), ++pairs;
  }
  // Straight ahead / behind: the two windows either side (1.8 deg off), averaged; the sides: the windows nearest.
  auto mean = [](const Cue& x, const Cue& y) { return Cue{(x.itd + y.itd) / 2, (x.ild + y.ild) / 2}; };
  const Cue ahead = mean(hq[index(1.8)], hq[index(358.2)]), behind = mean(hq[index(178.2)], hq[index(181.8)]);
  const Cue right = mean(hq[index(88.2)], hq[index(91.8)]), left = mean(hq[index(268.2)], hq[index(271.8)]);
  const double side = Woodworth(88.2, radius);
  printf("     right %.1f us %.2f dB, left %.1f us %.2f dB, ahead %.1f us %.2f dB, behind %.1f us %.2f dB; mirror pairs "
         "%d: ITD %.1f us, ILD %.2f dB; front/back ITD %.1f us\n",
         right.itd * 1e6, right.ild, left.itd * 1e6, left.ild, ahead.itd * 1e6, ahead.ild, behind.itd * 1e6, behind.ild,
         pairs, worstMirrorItd * 1e6, worstMirrorIld, worstFrontBackItd * 1e6);
  bool signs = true;
  for (int w = 0; w < windows; ++w) {
    const double s = std::sin(az[w] * kPi / 180.0);
    if (std::fabs(s) > 0.15) signs = signs && (hq[w].itd > 0) == (s > 0) && (hq[w].ild > 0) == (s > 0);
  }

  Check(directOk && owned == 1 && hrtfCalls >= kBlocks, "audio e2e: the circling source is owned by the hooks and rendered "
        "by the real BinauralCore on every block");
  Check(worstItdDirect <= 10e-6 && worstIldDirect <= 0.25,
        "audio e2e: through ed_sound's call sites, the hooked output's ITD / ILD trajectory is the core's own (same "
        "signal and positions driven directly): within 10 us and 0.25 dB in every 80 ms window");
  Check(worstItdWood <= 30e-6 && std::fabs(right.itd - side) <= 30e-6 && std::fabs(left.itd + side) <= 30e-6,
        "audio e2e: the ITD follows Woodworth for the configured head (57 cm: the set's own 8.75 cm head) all around the head, "
        "within 30 us (about +-680 us at the sides)");
  Check(signs, "audio e2e: a source on the right leads at the right ear and is louder there, on the left the "
               "opposite (every window more than 9 degrees off the median plane)");
  Check(std::fabs(ahead.itd) <= 15e-6 && std::fabs(ahead.ild) <= 0.5 && std::fabs(behind.itd) <= 15e-6 &&
            std::fabs(behind.ild) <= 0.5,
        "audio e2e: straight ahead and straight behind, no interaural difference (ITD within 15 us, ILD within 0.5 dB)");
  Check(right.ild >= 3.0 && left.ild <= -3.0, "audio e2e: at the sides the far ear is at least 3 dB quieter");
  // The mirrored window plays another stretch of the noise, whose spectrum weights the two ears' filters differently:
  // about 0.7 dB of the ILD spread is the signal's (the direct core gives the same numbers), hence 1 dB.
  Check(pairs >= 2 * windows - 4 && worstMirrorItd <= 20e-6 && worstMirrorIld <= 1.0 && worstFrontBackItd <= 30e-6,
        "audio e2e: left / right mirror-symmetric (ITD within 20 us, ILD within 1 dB) and the front / back ITD equal "
        "within 30 us");
  Check(allocs == 0 && AllFinite(hooked), "audio e2e: no heap allocation on the simulation / render thread with the "
                                          "real core, every output sample finite");
  direct.reset();  // the renderer before its set
  if (set) binaural::ReleaseHrtf(set);
  RigDown(r);
}

// ---- 2. the radio stays centred -------------------------------------------------------------------------------------
// One channel's DFT at `hz` over `frames` frames, Hann-windowed when asked (a long Hann window keeps the panned
// sounds' spectral spread from leaking into another sound's frequency).
std::complex<double> Bin(const float* lr, int ch, double hz, uint32_t frames = kFrames, bool hann = false) {
  std::complex<double> s = 0;
  for (uint32_t n = 0; n < frames; ++n) {
    const double w = hann ? 0.5 - 0.5 * std::cos(2 * kPi * n / frames) : 1.0;
    s += w * static_cast<double>(lr[2 * n + ch]) * std::polar(1.0, -2 * kPi * hz * n / kRate);
  }
  return s;
}

void Radio() {
  Rig r;
  RigUp(r, kIni);
  SetContext2D(r, 7, true);
  // Frequencies with a whole number of cycles per 480-frame block: each one's DFT bin sees only its own voice.
  const int radio = AddBoth(r, 7, false, 1, kSine, 0.3f, 1000, 1.0f);    // a 2D sound in the 2D context: the radio
  const int intercom = AddBoth(r, 7, true, 1, kSine, 0.2f, 2000, 1.0f);  // a 3D sound in the 2D context
  const int engine = AddBoth(r, 0, true, 1, kSine, 0.3f, 3000, 1.0f);    // a world sound
  PosBoth(r, radio, 1, 0, 0);
  PosBoth(r, intercom, 1, 0, 0);
  PosBoth(r, engine, 0, 0, 5);
  CreateVoice(r, radio);
  CreateVoice(r, intercom);
  CreateVoice(r, engine);
  Bind(r);
  std::vector<float> a(2 * kFrames), b(2 * kFrames), hooked, stock;
  double worstCentre = 0, worstStock = 0, worstIntercom = 0, engineRatio = 0, peak = 0;
  constexpr int kSkip = 10, kBlocks = 400, kWin = 10;  // 100 ms windows: 100 / 200 / 300 cycles
  for (int k = 0; k < kSkip + kBlocks; ++k) {
    const double th = k * 0.9 * kPi / 180.0;
    PosBoth(r, radio, 2 * std::cos(th), 0.3, 2 * std::sin(th));  // the radio's sound object moves: it must not matter
    PosBoth(r, intercom, 0.5 * std::cos(th), 0, 0.5 * std::sin(th));
    PosBoth(r, engine, 5 * std::sin(th), 0, 5 * std::cos(th));   // starts at the right
    SimFrame(r);
    Block(r, a.data(), b.data(), kFrames);
    if (k < kSkip) continue;
    hooked.insert(hooked.end(), a.begin(), a.end());
    stock.insert(stock.end(), b.begin(), b.end());
    if (k == kSkip)  // the world sound at the right: the core's level difference at 3 kHz
      engineRatio = std::abs(Bin(a.data(), 1, 3000)) / std::max(1e-12, std::abs(Bin(a.data(), 0, 3000)));
  }
  for (int w = 0; w < kBlocks / kWin; ++w) {
    const float* h = hooked.data() + size_t(2) * w * kWin * kFrames;
    const float* s = stock.data() + size_t(2) * w * kWin * kFrames;
    const uint32_t n = kWin * kFrames;
    const std::complex<double> l = Bin(h, 0, 1000, n, true), rr = Bin(h, 1, 1000, n, true), sl = Bin(s, 0, 1000, n, true);
    peak = std::max(peak, std::abs(l));
    worstCentre = std::max(worstCentre, std::abs(l - rr) / std::abs(sl));
    worstStock = std::max(worstStock, std::abs(l - sl) / std::abs(sl));
    for (int ch = 0; ch < 2; ++ch) {
      const std::complex<double> hi = Bin(h, ch, 2000, n, true), si = Bin(s, ch, 2000, n, true);
      worstIntercom = std::max(worstIntercom, std::abs(hi - si) / std::max(std::abs(si), 1e-3 * peak));
    }
  }
  printf("     radio: |L - R| / |DCS| worst %.2e, |hooked - DCS| / |DCS| worst %.2e (1 kHz bin %.1f); intercom vs DCS "
         "worst %.2e; world sound R/L at 3 kHz on the right: %.2f; owned %u\n",
         worstCentre, worstStock, peak, worstIntercom, engineRatio, audio::g_cnt.ownedNow.load());
  Check(audio::g_cnt.ownedNow.load() == 1 && engineRatio > 2.0,
        "audio e2e: next to the radio, only the world sound is owned, and the core pans it (on the right, the right "
        "ear over twice the left at 3 kHz)");
  // The bounds are the circling world sound's spectral spread at the other frequencies (about 1e-5 at 1 kHz, 2e-4 at
  // 2 kHz next to the 3 kHz one); a re-panned voice would be off by tens of percent.
  Check(peak > 10 && worstCentre <= 1e-4 && worstStock <= 1e-4 && worstIntercom <= 1e-3,
        "audio e2e: the radio (2D sound, 2D context) stays DCS's own and centred (left = right = DCS's mix within "
        "0.01 %), and a 3D sound in the 2D context stays DCS's own (within 0.1 %), while a world sound circles the head");
  RigDown(r);
}

// ---- 3. stereo waves ------------------------------------------------------------------------------------------------
// The hooked output of one sound (kind, channels, auto_width inner radius or < 0) held at a position: `blocks` blocks
// after `settle` blocks, and DCS's own output of it (the reference world). toggle: every `toggle` blocks the kill
// switch flips (0: never).
struct Take {
  std::vector<float> hooked, stock;
  uint64_t stereoCalls = 0;
};
Take Hold(uint32_t channels, Kind kind, const float pos[3], int settle, int blocks, int toggle = 0, uint32_t seed = 31,
          double hz = 0) {
  Rig r;
  RigUp(r, kIni);
  AddBoth(r, 0, true, channels, kind, 0.3f, hz, 1.0f, seed);
  Bind(r);
  PosBoth(r, 0, pos[0], pos[1], pos[2]);
  CreateVoice(r, 0);
  std::vector<float> a(2 * kFrames), b(2 * kFrames);
  Take t;
  for (int k = 0; k < settle + blocks; ++k) {
    if (toggle && k > settle && (k - settle) % toggle == 0) audio::g_userOff = !audio::g_userOff.load();
    SimFrame(r);
    Block(r, a.data(), b.data(), kFrames);
    if (k < settle) continue;
    t.hooked.insert(t.hooked.end(), a.begin(), a.end());
    t.stock.insert(t.stock.end(), b.begin(), b.end());
  }
  t.stereoCalls = audio::g_cnt.stereoCalls.load();
  audio::g_userOff = false;
  RigDown(r);
  return t;
}
double Energy(const std::vector<float>& v) {
  double e = 0;
  for (float x : v) e += static_cast<double>(x) * x;
  return e;
}
// The largest second difference of either channel (a click is a step: its second difference is the step's size).
double MaxD2(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
  double m = 0;
  to = std::min(to, v.size() / 2);
  for (size_t n = std::max<size_t>(from, 2); n < to; ++n)
    for (int ch = 0; ch < 2; ++ch)
      m = std::max(m, std::fabs(static_cast<double>(v[2 * n + ch]) - 2.0 * v[2 * (n - 1) + ch] + v[2 * (n - 2) + ch]));
  return m;
}

void Stereo() {
  char err[160] = {};
  binaural::HrtfSet* set = binaural::LoadHrtf(g_set.data(), g_set.size() * 4, kRate, err, sizeof(err));
  const double radius = binaural_config::PilotItdRadiusM(set ? binaural::DescribeHrtf(set).datasetHeadRadiusM : 0.0f, kCircumferenceCm);
  if (set) binaural::ReleaseHrtf(set);
  // 1. A stereo wave with channels correlated in part (a sine plus independent noise), a point source: ahead, at the
  // right, at the left.
  const float ahead[3] = {5, 0, 0}, right[3] = {0, 0, 5}, left[3] = {0, 0, -5};
  const Take ta = Hold(2, kPartial, ahead, 20, 100, 0, 31, 500), tr = Hold(2, kPartial, right, 20, 100, 0, 31, 500),
             tl = Hold(2, kPartial, left, 20, 100, 0, 31, 500);
  const Cue ca = Measure(ta.hooked.data(), ta.hooked.size() / 2), cr = Measure(tr.hooked.data(), tr.hooked.size() / 2),
            cl = Measure(tl.hooked.data(), tl.hooked.size() / 2);
  const double side = Woodworth(90, radius);
  printf("     stereo point source: ahead %.1f us %.2f dB, right %.1f us %.2f dB, left %.1f us %.2f dB (Woodworth at the "
         "side %.1f us); stereo HRTF calls %llu\n",
         ca.itd * 1e6, ca.ild, cr.itd * 1e6, cr.ild, cl.itd * 1e6, cl.ild, side * 1e6,
         static_cast<unsigned long long>(tr.stereoCalls));
  Check(tr.stereoCalls >= 100 && std::fabs(ca.itd) <= 15e-6 && std::fabs(ca.ild) <= 0.5 && std::fabs(cr.itd - side) <= 30e-6 &&
            std::fabs(cl.itd + side) <= 30e-6 && cr.ild >= 3.0 && cl.ild <= -3.0,
        "audio e2e stereo: a stereo wave (channels correlated in part) is rendered by the core as a point source: no "
        "interaural difference ahead, Woodworth's ITD at the sides within 30 us, the far ear at least 3 dB quieter");
  // 2. Loudness ahead against DCS's own stereo output: partly correlated, dual mono, a decorrelated bed (its side kept)
  // and a mono wave.
  const Take td = Hold(2, kDual, ahead, 20, 100, 0, 31, 500), tm = Hold(1, kNoise, ahead, 20, 100), tmd = Hold(1, kSine, ahead, 20, 100, 0, 31, 500);
  const Take tbed = Hold(2, kNoise, ahead, 40, 100);
  const double dbBed = 10 * std::log10(Energy(tbed.hooked) / Energy(tbed.stock));
  const double dbStereo = 10 * std::log10(Energy(ta.hooked) / Energy(ta.stock));
  const double dbDual = 10 * std::log10(Energy(td.hooked) / Energy(td.stock));
  const double dbMono = 10 * std::log10(Energy(tm.hooked) / Energy(tm.stock));
  double worstDual = 0, peakDual = 0;
  for (size_t i = 0; i < td.hooked.size(); ++i) {
    worstDual = std::max(worstDual, static_cast<double>(std::fabs(td.hooked[i] - tmd.hooked[i])));
    peakDual = std::max(peakDual, static_cast<double>(std::fabs(tmd.hooked[i])));
  }
  printf("     loudness ahead vs DCS's own output: stereo (partly correlated) %+.2f dB, stereo (dual mono) %+.2f dB, "
         "stereo (decorrelated bed) %+.2f dB, mono %+.2f dB; dual-mono stereo vs the same mono wave: worst %.2e of %.3f\n",
         dbStereo, dbDual, dbBed, dbMono, worstDual, peakDual);
  Check(std::fabs(dbStereo) <= 1.0 && std::fabs(dbDual) <= 1.0 && std::fabs(dbBed) <= 1.0,
        "audio e2e stereo: ahead, a stereo wave is as loud as DCS's own stereo output within 1 dB (partly correlated, "
        "dual-mono and decorrelated channels)");
  Check(peakDual > 0.05 && worstDual <= 1e-5 * peakDual + 1e-7,
        "audio e2e stereo: a dual-mono stereo wave (most of DCS's stereo files) renders exactly as the same mono wave");
  // 2b. Loudness away from the front. The flight of 2026-11-25: the ownship's cockpit sounds sit behind and above the
  // pilot (DCS's pan law -3 to -5 dB there), most of their power under 1 kHz, where the HRTF is about as loud as ahead;
  // with the mid at DCS's frontal gain the kill switch stepped 2.3-2.8 dB. A 250 Hz tone, mono and dual-mono stereo, at
  // those two places and close at the side (the near-field shelf): within 1 dB of DCS's own output.
  {
    const float behind[3] = {-0.83f, -0.58f, 0.0f}, above[3] = {-0.13f, 0.42f, 0.0f}, sideNear[3] = {0.05f, 0.0f, 0.3f},
                farBehind[3] = {-6.0f, 0.0f, -2.0f};
    const float* where[] = {behind, above, sideNear, farBehind};
    const char* names[] = {"behind-below 1 m", "above-behind 0.44 m", "right 0.3 m", "behind-left 6 m"};
    double worst = 0;
    for (int p = 0; p < 4; ++p) {
      const Take m1 = Hold(1, kSine, where[p], 20, 100, 0, 31, 250), s2 = Hold(2, kDual, where[p], 20, 100, 0, 31, 250);
      const double d1 = 10 * std::log10(Energy(m1.hooked) / Energy(m1.stock));
      const double d2 = 10 * std::log10(Energy(s2.hooked) / Energy(s2.stock));
      printf("     loudness %s vs DCS's own output (250 Hz): mono %+.2f dB, stereo %+.2f dB\n", names[p], d1, d2);
      worst = std::max({worst, std::fabs(d1), std::fabs(d2)});
    }
    Check(worst <= 1.0,
          "audio e2e: away from the front (behind, above, close at the side, far behind) a low sound is as loud as DCS's own "
          "output within 1 dB: the HRTF part carries DCS's level for the direction, the near-field shelf keeps the level");
  }
  // 3. The kill switch flipping every 50 blocks: no click (second difference) beyond the steady state's own.
  const float at[3] = {4, 0, 3};
  bool clickFree = true;
  for (uint32_t ch : {2u, 1u}) {
    const Take tk = Hold(ch, kSine, at, 20, 400, 50, 31, 200);
    double steadyD2 = 0;
    for (int seg = 0; seg < 8; ++seg) {  // the middle 20 blocks of each half-period: no change there
      const size_t from = size_t(seg * 50 + 15) * kFrames, to = size_t(seg * 50 + 35) * kFrames;
      steadyD2 = std::max(steadyD2, MaxD2(tk.hooked, from, to));
    }
    const double allD2 = MaxD2(tk.hooked);
    printf("     kill switch every 0.5 s on a %s wave: largest second difference %.2e (steady %.2e)\n",
           ch == 2 ? "stereo" : "mono", allD2, steadyD2);
    // A step of s shows as a second difference of s; a 10 ms linear crossfade between two different signals only bends
    // the slope (about 1 / 480 of their difference per sample). Before the onset fade the later ear's step gave 2e-2.
    clickFree = clickFree && steadyD2 > 0 && allD2 <= steadyD2 + 1e-3 && AllFinite(tk.hooked);
  }
  Check(clickFree, "audio e2e: switching a stereo or a mono wave between binaural and DCS's panning makes no click (no "
                   "step above -60 dBFS beyond the steady output's own second difference; the core's fresh source is "
                   "faded in at its input)");
}

// ---- 4. wide voices (review round 5) ---------------------------------------------------------------------------------
// One stereo sound driven through the hooks with a position per block (pos(k, p)), the hooked and DCS's own output after
// `settle` blocks, the voice's own channels L, R (the fake voice's generator with the same seed: before DCS's gains),
// and the module's count of voices kept wide after each block.
struct Path {
  std::vector<float> hooked, stock, l, r;
  std::vector<uint32_t> wide;
};
template <typename PosFn>
Path HoldPath(Kind kind, PosFn pos, int settle, int blocks, uint32_t seed = 31, double hz = 0, float amp = 0.3f) {
  Rig r;
  RigUp(r, kIni);
  AddBoth(r, 0, true, 2, kind, amp, hz, 1.0f, seed);
  float p[3];
  pos(0, p);
  PosBoth(r, 0, p[0], p[1], p[2]);
  CreateVoice(r, 0);
  // The generator: a copy of the voice's signal state (as Circle does for a mono wave).
  std::vector<uint8_t> gen(kVoiceSize, 0);
  memcpy(gen.data(), r.ref.voices[0], kVoiceSize);  // kind, amplitude, phase, step and noise state as created
  std::vector<float> a(2 * kFrames), b(2 * kFrames);
  Path t;
  for (int k = -1; k < settle + blocks; ++k) {
    if (k >= 0) {
      pos(k, p);
      PosBoth(r, 0, p[0], p[1], p[2]);
      SimFrame(r);
    }
    Block(r, a.data(), b.data(), kFrames);
    if (k < 0) Tick();  // the first block binds the renderer
    std::vector<float> gl(kFrames), gr(kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) gl[i] = NextSample(gen.data(), &gr[i]);
    if (k < settle) continue;
    t.hooked.insert(t.hooked.end(), a.begin(), a.end());
    t.stock.insert(t.stock.end(), b.begin(), b.end());
    t.l.insert(t.l.end(), gl.begin(), gl.end());
    t.r.insert(t.r.end(), gr.begin(), gr.end());
    t.wide.push_back(audio::g_cnt.ownedWideNow.load());
  }
  RigDown(r);
  return t;
}
// The lag (samples, 0..kMax) at which x best explains channel ch of `lr` (largest |cross-correlation|).
int BestLag(const std::vector<float>& lr, int ch, const std::vector<double>& x) {
  constexpr int kMax = 120;
  const size_t n = lr.size() / 2;
  double best = -1;
  int lag = 0;
  for (int k = 0; k <= kMax; ++k) {
    double s = 0;
    for (size_t i = kMax; i < n; ++i) s += static_cast<double>(lr[2 * i + ch]) * x[i - k];
    if (std::fabs(s) > best) best = std::fabs(s), lag = k;
  }
  return lag;
}

void Wide() {
  char err[160] = {};
  binaural::HrtfSet* set = binaural::LoadHrtf(g_set.data(), g_set.size() * 4, kRate, err, sizeof(err));
  const double radius = binaural_config::PilotItdRadiusM(set ? binaural::DescribeHrtf(set).datasetHeadRadiusM : 0.0f, kCircumferenceCm);
  if (set) binaural::ReleaseHrtf(set);
  // 1. Mid / side alignment: two independent noises (M and S uncorrelated) 1 m away (near: wide). At each ear the side
  // must arrive with the HRTF mid: the lag of S in that ear's output equals the lag of M (before round 5 the side came
  // undelayed, 24-48 samples early: comb notches from ~1 kHz wherever M and S correlate).
  {
    const float where[4][3] = {{1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {-1, 0, 0}};
    const char* names[] = {"ahead", "right", "left", "behind"};
    int worst = 0;
    bool wideAll = true;
    for (int p = 0; p < 4; ++p) {
      const Path t = HoldPath(kNoise, [&](int, float* q) { memcpy(q, where[p], sizeof(float) * 3); }, 20, 100);
      std::vector<double> m(t.l.size()), s(t.l.size());
      for (size_t i = 0; i < t.l.size(); ++i) m[i] = 0.5 * (t.l[i] + t.r[i]), s[i] = 0.5 * (t.l[i] - t.r[i]);
      int lm[2], ls[2];
      for (int ch = 0; ch < 2; ++ch) lm[ch] = BestLag(t.hooked, ch, m), ls[ch] = BestLag(t.hooked, ch, s);
      printf("     wide %s 1 m: mid lag L %d R %d, side lag L %d R %d samples (undelayed side: 0)\n", names[p], lm[0], lm[1],
             ls[0], ls[1]);
      worst = std::max({worst, std::abs(lm[0] - ls[0]), std::abs(lm[1] - ls[1])});
      wideAll = wideAll && t.wide.back() == 1 && lm[0] >= 12 && lm[1] >= 12;
    }
    Check(wideAll && worst <= 2,
          "audio e2e wide: a wide voice's side reaches each ear with its HRTF mid (lags equal within 2 samples ahead, "
          "right, left, behind; the mid itself 12 samples or more late), so mid and side do not comb");
  }
  // 2. ITD / ILD at the sides of a wide voice (channels correlated in part, 1 m): the interaural cues are the source's.
  {
    const float right[3] = {0, 0, 1}, left[3] = {0, 0, -1};
    const Path tr = HoldPath(kPartial, [&](int, float* q) { memcpy(q, right, 12); }, 20, 100, 31, 500);
    const Path tl = HoldPath(kPartial, [&](int, float* q) { memcpy(q, left, 12); }, 20, 100, 31, 500);
    const Cue cr = Measure(tr.hooked.data(), tr.hooked.size() / 2), cl = Measure(tl.hooked.data(), tl.hooked.size() / 2);
    const double side = Woodworth(90, radius);
    printf("     wide 1 m: right %.1f us %.2f dB, left %.1f us %.2f dB (Woodworth far field %.1f us); kept wide %u / %u\n",
           cr.itd * 1e6, cr.ild, cl.itd * 1e6, cl.ild, side * 1e6, tr.wide.back(), tl.wide.back());
    Check(tr.wide.back() == 1 && tl.wide.back() == 1 && cr.itd >= 0.8 * side && cl.itd <= -0.8 * side && cr.ild >= 3.0 &&
              cl.ild <= -3.0 && std::fabs(cr.itd + cl.itd) <= 30e-6,
          "audio e2e wide: a wide voice at the sides keeps the source's ITD (at least 80 % of Woodworth's, mirror-equal "
          "within 30 us) and the far ear at least 3 dB quieter");
  }
  // 3. Level against DCS's own output by direction, wide voices (two low tones, 250 / 325 Hz, uncorrelated: diffuse and,
  // at 1 m, near): within 1 dB at 1 m and at 5 m, ahead, right, behind, left, above.
  {
    const float dirs[5][3] = {{1, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {0.2f, 0.98f, 0}};
    const char* names[] = {"ahead", "right", "behind", "left", "above"};
    double worst = 0;
    bool wideAll = true;
    for (float dist : {1.0f, 5.0f})
      for (int p = 0; p < 5; ++p) {
        const Path t = HoldPath(kSine, [&](int, float* q) {
          for (int c = 0; c < 3; ++c) q[c] = dirs[p][c] * dist;
        }, 20, 100, 31, 250);
        const double db = 10 * std::log10(Energy(t.hooked) / Energy(t.stock));
        printf("     wide %s %.0f m: %+.2f dB vs DCS's own output\n", names[p], dist, db);
        worst = std::max(worst, std::fabs(db));
        wideAll = wideAll && t.wide.back() == 1;
      }
    Check(wideAll && worst <= 1.0,
          "audio e2e wide: a wide (diffuse) low sound is as loud as DCS's own output within 1 dB in every direction, "
          "near and far");
  }
  // 4. Point <-> wide: a stereo DC with a side (left 1, right 1/4) moving between 1.0 and 2.4 m on the right every 40
  // blocks (wide under 1.4 m, a point past 1.6 m): no step beyond the same motion with no side (left = right).
  {
    auto path = [](int k, float* q) {
      q[0] = 0.0f, q[1] = 0.0f, q[2] = (k / 40) % 2 ? 2.4f : 1.0f;
    };
    const Path split = HoldPath(kDcSplit, path, 20, 320, 31, 0, 1.0f), dc = HoldPath(kDc, path, 20, 320, 31, 0, 1.0f);
    uint32_t switches = 0;
    for (size_t k = 1; k < split.wide.size(); ++k) switches += split.wide[k] != split.wide[k - 1];
    const double d2 = MaxD2(split.hooked), d2dc = MaxD2(dc.hooked);
    printf("     point <-> wide every 40 blocks: %u switches, largest second difference %.2e (same motion without a side "
           "%.2e)\n", switches, d2, d2dc);
    Check(switches >= 6 && d2 <= d2dc + 1e-3 && AllFinite(split.hooked),
          "audio e2e wide: switching a voice between point and wide (1.0 <-> 2.4 m) makes no click: no step above "
          "-60 dBFS beyond the motion's own (the side's share and its delay crossfaded)");
  }
}

// ---- 5. cockpit space with the real Room (docs/COCKPIT_SPACE.md 10 and 13: R2, R1, R4/M8, R6/M2, R7/M1, R8) --------
// The module's room calls on the real core's Room: per preset the externalisation metrics through ed_sound's call
// sites (direct-to-room ratio and interaural coherence), the level against the kill switch (DCS's own output), no click
// on any live change, the CPU at 48 voices, and Off exact.
namespace space {

constexpr int kCockpitCtx = 2, kWorldCtx = 3;
const char* const kLevelName[] = {"Off", "Subtle", "Normal", "Strong"};
const double kIdentity[4] = {0, 0, 0, 1};

std::string Ini(int level, const char* extra = "") {
  return std::string("[Audio]\r\nBinaural=1\r\nHrtf=default\r\nHeadCircumferenceCm=57\r\nLateHeadTracking=0\r\n"
                     "NearField=1\r\nMaxObjects=48\r\nCockpitSpace=") +
         std::to_string(level) + "\r\n" + extra;
}
// DCS's use-cockpit-gain flag of a context (+0xe1): set on the world context while the camera is in the cockpit.
void SetCtxCockpitGain(Rig& r, int ctx, bool on) { W<uint8_t>(r.im.base + kCtxArray + 0x388 * ctx, 0xe1, on ? 1 : 0); }

struct Voice {
  int ctx;
  uint32_t channels;
  Kind kind;
  float amp;
  double hz;
  float pos[3];
  uint32_t seed;
  bool through;  // an outside voice whose host allows cockpit gain (DCS damps it to 0.5): heard through the canopy
};
struct Run {
  std::vector<float> hooked, stock;
  float dcompDb = 0;
  uint32_t owned = 0, roomVoices = 0;
  int level = 0;
};

// The F-16C (bubble canopy, closed), inside (DCS's flag set on the world context), the cockpit sounds carrying the
// aircraft frame (identity: the head looks forward). perBlock(r, k) runs before block k's sim frame.
template <typename Fn>
Run Scene(const std::string& ini, const std::vector<Voice>& vs, int settle, int blocks, Fn perBlock,
          const std::vector<uint32_t>* sizes = nullptr) {
  Rig r;
  RigUp(r, ini.c_str());
  SetCtxCockpitGain(r, kWorldCtx, true);
  for (const Voice& v : vs) {
    const int i = AddBoth(r, v.ctx, true, v.channels, v.kind, v.amp, v.hz, 1.0f, v.seed);
    PosBoth(r, i, v.pos[0], v.pos[1], v.pos[2]);
    if (v.through) r.w.SetCockpitGain(i, true, 0.5f), r.ref.SetCockpitGain(i, true, 0.5f);
    if (v.ctx == kCockpitCtx) r.w.SetOrient(i, kIdentity);
  }
  for (size_t i = 0; i < vs.size(); ++i) CreateVoice(r, static_cast<int>(i));
  audio::LuaAircraft("F-16C_50");
  audio::LuaState(0.0, 1, 1);
  Bind(r);
  Tick();
  std::vector<float> a(2 * 1024), b(2 * 1024);
  Run t;
  t.hooked.reserve(size_t(2) * kFrames * blocks);
  t.stock.reserve(size_t(2) * kFrames * blocks);
  for (int k = 0; k < settle + blocks; ++k) {
    perBlock(r, k);
    SimFrame(r);
    // The device block: kFrames, or the given sizes in turn (WASAPI periods vary).
    const uint32_t n = sizes ? (*sizes)[static_cast<size_t>(k) % sizes->size()] : kFrames;
    a.resize(2 * size_t(n)), b.resize(2 * size_t(n));
    Block(r, a.data(), b.data(), n);
    if (k == settle / 2) Tick();  // the control thread's pass (stats, Live)
    if (k < settle) continue;
    t.hooked.insert(t.hooked.end(), a.begin(), a.end());
    t.stock.insert(t.stock.end(), b.begin(), b.end());
  }
  t.dcompDb = audio::g_cnt.dcompMeanDb.load();
  t.owned = audio::g_cnt.ownedNow.load();
  t.roomVoices = audio::g_cnt.roomVoicesNow.load();
  t.level = audio::g_spaceSt.level;
  RigDown(r);
  return t;
}
auto kNothing = [](Rig&, int) {};

// A biquad (RBJ), run over one channel of an interleaved buffer into a mono vector.
struct Biquad {
  double b0, b1, b2, a1, a2;
};
Biquad HighPass(double f, double q = 0.70710678) {
  const double w = 2 * kPi * f / kRate, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
  return {(1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - al) / a0};
}
Biquad LowPass(double f, double q = 0.70710678) {
  const double w = 2 * kPi * f / kRate, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
  return {(1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - al) / a0};
}
std::vector<double> Ear(const std::vector<float>& lr, int ch) {
  std::vector<double> x(lr.size() / 2);
  for (size_t n = 0; n < x.size(); ++n) x[n] = lr[2 * n + ch];
  return x;
}
std::vector<double> Filter(std::vector<double> x, const Biquad& f) {
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  for (double& v : x) {
    const double y = f.b0 * v + f.b1 * x1 + f.b2 * x2 - f.a1 * y1 - f.a2 * y2;
    x2 = x1, x1 = v, y2 = y1, y1 = y, v = y;
  }
  return x;
}
// 1-4 kHz (fourth-order high-pass and low-pass), the band of R2.
std::vector<double> Band(const std::vector<double>& x) {
  return Filter(Filter(Filter(Filter(x, HighPass(1000)), HighPass(1000)), LowPass(4000)), LowPass(4000));
}
double Sum2(const std::vector<double>& x, size_t from = 0) {
  double e = 0;
  for (size_t n = from; n < x.size(); ++n) e += x[n] * x[n];
  return e;
}
// Interaural cross-correlation coefficient: the largest |normalised cross-correlation| within +-1 ms.
double Iacc(const std::vector<double>& l, const std::vector<double>& r, size_t from) {
  const int lag = static_cast<int>(kRate / 1000);
  const double el = Sum2(l, from), er = Sum2(r, from);
  if (el <= 0 || er <= 0) return 0;
  double best = 0;
  for (int k = -lag; k <= lag; ++k) {
    double s = 0;
    for (size_t n = from + lag; n + lag < l.size(); ++n) s += l[n] * r[n + k];
    best = std::max(best, std::fabs(s));
  }
  return best / std::sqrt(el * er);
}

// R2 / R1 through the module: a white-noise cockpit sound alone, Off and each preset with the same signal and
// position. The direct sound of a preset is exactly Off's times the voice's direct gain d (RoomDirectGain, the module's
// own figure with one voice), the room is the rest. DRR = direct / room energy (broadband and 1-4 kHz); IACC of the
// whole output (and of the room alone) per preset. The least-squares fit of the output on Off's is printed next to d:
// it equals d unless a reflection arrives within the direct sound's own correlation time (a source close to the
// canopy), which the fit then counts as direct.
void Externalisation() {
  // The room's design figures: the direct sound is Off's times d exactly only without the level match (its slow
  // three-band gain is on direct + room together; M17 and Level check the level with it).
  audio::g_testRoomNoMatch = true;
  struct Src {
    const char* name;
    float pos[3];
  };
  const Src srcs[] = {{"front 0.6 m", {0.6f, 0.0f, 0.0f}},
                      {"above-front 0.45 m", {0.4f, 0.2f, 0.0f}},
                      {"right console 0.5 m", {0.15f, -0.35f, 0.33f}}};
  bool drrRange = true, drrOrder = true, dMatch = true, iaccNotUp = true, iaccDown = true, roomDecorr = true, finite = true;
  printf("     externalisation, F-16C (bubble), white noise, 3 s per run; d fitted vs the module's own direct gain:\n");
  printf("     %-20s %-7s %8s %8s %9s %9s %8s %8s %9s\n", "source", "preset", "d fit", "d module", "DRR", "DRR 1-4k",
         "IACC", "IACC 1-4k", "room 1-4k");
  for (const Src& s : srcs) {
    std::vector<Voice> vs = {{kCockpitCtx, 1, kNoise, 0.3f, 0, {s.pos[0], s.pos[1], s.pos[2]}, 11, false}};
    Run off = Scene(Ini(0), vs, 80, 300, kNothing);
    const std::vector<double> offL = Ear(off.hooked, 0), offR = Ear(off.hooked, 1);
    const std::vector<double> offBL = Band(offL), offBR = Band(offR);
    const size_t skip = kRate / 20;  // the band filters' start
    const double iaccOff = Iacc(offL, offR, 0), iaccOffB = Iacc(offBL, offBR, skip);
    printf("     %-20s %-7s %8s %8s %9s %9s %8.3f %8.3f %9s\n", s.name, "Off", "", "", "", "", iaccOff, iaccOffB, "");
    double lastDrr = 1e9;
    for (int level = 1; level <= 3; ++level) {
      Run on = Scene(Ini(level), vs, 80, 300, kNothing);
      finite = finite && AllFinite(on.hooked);
      const std::vector<double> onL = Ear(on.hooked, 0), onR = Ear(on.hooked, 1);
      double num = 0, den = 0;
      for (size_t n = 0; n < onL.size(); ++n) num += onL[n] * offL[n] + onR[n] * offR[n], den += offL[n] * offL[n] + offR[n] * offR[n];
      const double fit = den > 0 ? num / den : 0, d = std::pow(10.0, on.dcompDb / 20.0);
      std::vector<double> roomL(onL.size()), roomR(onR.size());
      for (size_t n = 0; n < onL.size(); ++n) roomL[n] = onL[n] - d * offL[n], roomR[n] = onR[n] - d * offR[n];
      const double drr = 10 * std::log10(d * d * (Sum2(offL) + Sum2(offR)) / std::max(1e-30, Sum2(roomL) + Sum2(roomR)));
      const std::vector<double> roomBL = Band(roomL), roomBR = Band(roomR), onBL = Band(onL), onBR = Band(onR);
      const double drrB = 10 * std::log10(d * d * (Sum2(offBL, skip) + Sum2(offBR, skip)) /
                                          std::max(1e-30, Sum2(roomBL, skip) + Sum2(roomBR, skip)));
      const double iacc = Iacc(onL, onR, 0), iaccB = Iacc(onBL, onBR, skip), iaccRoom = Iacc(roomBL, roomBR, skip);
      printf("     %-20s %-7s %+7.2f dB %+6.2f dB %+7.1f dB %+7.1f dB %8.3f %8.3f %9.3f\n", s.name, kLevelName[level],
             20 * std::log10(std::max(fit, 1e-9)), on.dcompDb, drr, drrB, iacc, iaccB, iaccRoom);
      // d below 1; the fit equal to it for the sources away from the canopy
      dMatch = dMatch && on.level == level && on.roomVoices == 1 && on.dcompDb < 0.0f && on.dcompDb > -1.0f &&
               (s.pos[1] > 0.1f || std::fabs(20 * std::log10(std::max(fit, 1e-9)) - on.dcompDb) <= 0.1);
      drrOrder = drrOrder && drrB < lastDrr - 1.0;
      lastDrr = drrB;
      iaccNotUp = iaccNotUp && iacc <= iaccOff + 0.005 && iaccB <= iaccOffB + 0.005;
      if (level == 3) iaccDown = iaccDown && iaccB < iaccOffB - 0.01;
      roomDecorr = roomDecorr && iaccRoom < 0.9 * iaccOffB;
      if (&s == &srcs[0]) {  // R2's reference source (design: Subtle 13-19, Normal 9-15, Strong 6-11 dB)
        const double lo[] = {0, 13, 9, 6}, hi[] = {0, 19, 15, 11};
        drrRange = drrRange && drrB >= lo[level] && drrB <= hi[level];
      }
    }
  }
  Check(dMatch, "cockpit space e2e: with the real Room each preset reaches the module (level, one voice sent), the "
                "direct gain is under 0 dB, and the direct sound is Off's times it (fit within 0.1 dB away from the "
                "canopy)");
  Check(drrRange, "cockpit space e2e R2: the 0.6 m front source's direct-to-room ratio (1-4 kHz) is in the design's "
                  "range per preset through ed_sound's call sites (Subtle 13-19, Normal 9-15, Strong 6-11 dB)");
  Check(drrOrder, "cockpit space e2e: the room grows with the preset (DRR 1-4 kHz at least 1 dB lower each step) for "
                  "every cockpit source");
  Check(iaccNotUp && iaccDown && roomDecorr,
        "cockpit space e2e R1: the space never raises the ears' coherence, Strong lowers it in 1-4 kHz for every "
        "source, and the room part itself is less coherent than the direct sound");
  Check(finite, "cockpit space e2e: every output sample finite");
  audio::g_testRoomNoMatch = false;
}

// R4 / M8 and the kill switch: a DCS-like cockpit mix (a low engine tone behind, console noise, a panel tone, a stereo
// sound above, an outside sound through the canopy), and each of its sounds alone. Against DCS's own output (what the
// kill switch plays: the same scene with the kill switch flipping every second, its off halves) within 1 dB, and each
// preset against Off within 0.5 dB for the mix.
void Level() {
  const std::vector<Voice> mix = {{kCockpitCtx, 1, kSine, 0.35f, 110, {-0.3f, -0.4f, 0.0f}, 3, false},
                                  {kCockpitCtx, 1, kNoise, 0.12f, 0, {0.5f, -0.2f, -0.3f}, 5, false},
                                  {kCockpitCtx, 1, kSine, 0.10f, 700, {0.6f, 0.0f, 0.1f}, 7, false},
                                  {kCockpitCtx, 2, kPartial, 0.08f, 300, {0.2f, 0.3f, 0.0f}, 9, false},
                                  {kWorldCtx, 1, kNoise, 0.20f, 0, {20.0f, 0.0f, 5.0f}, 13, true}};
  const char* names[] = {"engine tone behind-below", "left console noise", "panel tone", "stereo above", "outside, canopy"};
  // The mix with the kill switch flipping every 100 blocks: on halves against off halves (the middle 55 blocks each).
  double offDb = 0, worstKill = 0, worstVsOff = 0, worstStock = 0;
  printf("     level, DCS-like cockpit mix, kill switch every 1 s (on halves vs off halves), and vs Off:\n");
  for (int level = 0; level <= 3; ++level) {
    Run t = Scene(Ini(level), mix, 60, 600, [](Rig&, int k) {
      if (k >= 60 && (k - 60) % 100 == 0) audio::g_userOff = ((k - 60) / 100) % 2 == 1;
    });
    double eOn = 0, eOff = 0, eStockOff = 0;
    for (int seg = 0; seg < 6; ++seg) {
      const size_t from = size_t(2) * (seg * 100 + 40) * kFrames, to = size_t(2) * (seg * 100 + 95) * kFrames;
      double e = 0, es = 0;
      for (size_t i = from; i < to; ++i) e += double(t.hooked[i]) * t.hooked[i], es += double(t.stock[i]) * t.stock[i];
      (seg % 2 ? eOff : eOn) += e;
      if (seg % 2) eStockOff += es;
    }
    const double dbKill = 10 * std::log10(eOn / eOff), dbStock = 10 * std::log10(eOff / eStockOff);
    if (level == 0) offDb = 10 * std::log10(eOn);
    const double dbVsOff = 10 * std::log10(eOn) - offDb;
    printf("     %-7s binaural vs kill switch %+.2f dB, vs Off %+.2f dB (kill-switch halves vs DCS's own %+.3f dB)\n",
           kLevelName[level], dbKill, dbVsOff, dbStock);
    worstKill = std::max(worstKill, std::fabs(dbKill));
    if (level) worstVsOff = std::max(worstVsOff, std::fabs(dbVsOff));
    worstStock = std::max(worstStock, std::fabs(dbStock));
  }
  // Each sound alone, every preset, against DCS's own output (printed: Off's own difference is the direct path's,
  // checked by scenarios 1-4) and against Off (R4 per source: the space's own change).
  double worstOne = 0, worstMean = 0, mean[4] = {};
  printf("     each sound alone vs DCS's own output (the space's change vs Off in brackets):\n");
  for (size_t v = 0; v < mix.size(); ++v) {
    printf("     %-26s", names[v]);
    double dbOff = 0;
    for (int level = 0; level <= 3; ++level) {
      Run t = Scene(Ini(level), {mix[v]}, 60, 200, kNothing);
      const double db = 10 * std::log10(Energy(t.hooked) / Energy(t.stock));
      if (level == 0) dbOff = db;
      printf(level ? " %s %+.2f (%+.2f)" : " %s %+.2f", kLevelName[level], db, db - dbOff);
      worstOne = std::max(worstOne, std::fabs(db - dbOff));
      mean[level] += (db - dbOff) / mix.size();
    }
    printf("\n");
  }
  for (int level = 1; level <= 3; ++level) worstMean = std::max(worstMean, std::fabs(mean[level]));
  printf("     mean change vs Off over the five sounds: Subtle %+.2f, Normal %+.2f, Strong %+.2f dB\n", mean[1], mean[2], mean[3]);
  Check(worstStock <= 0.05, "cockpit space e2e: with the kill switch on, the output is DCS's own (within 0.05 dB)");
  Check(worstKill <= 1.0, "cockpit space e2e level: the cockpit mix with the space at every preset is as loud as with "
                          "the kill switch (DCS's own output) within 1 dB");
  Check(worstVsOff <= 0.5, "cockpit space e2e M8: the cockpit mix at Subtle, Normal and Strong is within 0.5 dB of Off");
  Check(worstOne <= 1.0 && worstMean <= 0.5,
        "cockpit space e2e R4: each sound of the mix alone changes by at most 1 dB with the space at any preset, and "
        "by at most 0.5 dB on average over the sounds");
}

// R6 / M2: one run with every live change, 40 blocks apart, the head turning 90 deg/s throughout: preset steps
// (Subtle, Normal, Strong, Normal, Off, Subtle), the family, the canopy opening and closing, outside and back, the kill
// switch off and on. No step above the steady output's own second difference (+ -60 dBFS).
void Clicks() {
  const std::vector<Voice> vs = {{kCockpitCtx, 1, kSine, 0.3f, 200, {0.5f, -0.1f, -0.2f}, 3, false},
                                 {kCockpitCtx, 1, kSine, 0.2f, 90, {-0.2f, -0.3f, 0.2f}, 5, false},
                                 {kWorldCtx, 1, kSine, 0.2f, 150, {15.0f, 0.0f, -4.0f}, 7, true}};
  struct Ev {
    const char* what;
    std::function<void(Rig&)> fn;
  };
  const std::vector<Ev> evs = {
      {"Subtle -> Normal", [](Rig& r) { WriteIni(r.dir, Ini(2).c_str()), Tick(); }},
      {"Normal -> Strong", [](Rig& r) { WriteIni(r.dir, Ini(3).c_str()), Tick(); }},
      {"family helicopter", [](Rig& r) { WriteIni(r.dir, Ini(3, "CockpitSpaceFamily=helicopter\r\n").c_str()), Tick(); }},
      {"family auto", [](Rig& r) { WriteIni(r.dir, Ini(3).c_str()), Tick(); }},
      {"canopy opens", [](Rig&) { audio::LuaState(1.0, 1, 1), Tick(); }},
      {"canopy closes", [](Rig&) { audio::LuaState(0.0, 1, 1), Tick(); }},
      {"outside (F2)", [](Rig& r) { SetCtxCockpitGain(r, kWorldCtx, false); }},
      {"back inside", [](Rig& r) { SetCtxCockpitGain(r, kWorldCtx, true); }},
      {"kill switch on", [](Rig&) { audio::g_userOff = true; }},
      {"kill switch off", [](Rig&) { audio::g_userOff = false; }},
      {"Strong -> Normal", [](Rig& r) { WriteIni(r.dir, Ini(2).c_str()), Tick(); }},
      {"Normal -> Off", [](Rig& r) { WriteIni(r.dir, Ini(0).c_str()), Tick(); }},
      {"Off -> Subtle", [](Rig& r) { WriteIni(r.dir, Ini(1).c_str()), Tick(); }}};
  constexpr int kSeg = 40, kSettle = 60;
  const int blocks = kSeg * static_cast<int>(evs.size() + 1);
  Run t = Scene(Ini(1), vs, kSettle, blocks, [&](Rig& r, int k) {
    double q[4];  // the head turning 90 deg/s: the aircraft seen turning the other way in the listener frame
    const double yaw = -0.5 * (k * 0.9) * kPi / 180.0;
    q[0] = 0, q[1] = std::sin(yaw), q[2] = 0, q[3] = std::cos(yaw);
    for (int i = 0; i < 2; ++i) r.w.SetOrient(i, q);
    const int rel = k - kSettle;
    if (rel > 0 && rel % kSeg == 0 && rel / kSeg - 1 < static_cast<int>(evs.size())) evs[rel / kSeg - 1].fn(r);
  });
  // Steady: blocks 25..38 of each segment (every fade over: wet 0.3 s, canopy about 1 s from its start is still moving
  // at 25 blocks but slowly); the whole run against the largest steady one.
  double steady = 0, worst = 0;
  int worstSeg = 0;
  for (int seg = 0; seg <= static_cast<int>(evs.size()); ++seg) {
    steady = std::max(steady, MaxD2(t.hooked, size_t(seg * kSeg + 25) * kFrames, size_t(seg * kSeg + 39) * kFrames));
    const double d = MaxD2(t.hooked, size_t(seg * kSeg) * kFrames, size_t(seg * kSeg + kSeg) * kFrames);
    if (d > worst) worst = d, worstSeg = seg;
  }
  printf("     live changes (%zu, 0.4 s apart, head turning 90 deg/s): largest second difference %.2e at \"%s\", steady "
         "%.2e\n", evs.size(), worst, worstSeg ? evs[worstSeg - 1].what : "start", steady);
  Check(steady > 0 && worst <= steady + 1e-3 && AllFinite(t.hooked),
        "cockpit space e2e R6: no click on any live change (presets, family, canopy, outside and back, kill switch, "
        "Off and on again) with the head turning: no step above -60 dBFS beyond the steady output's own");
}

// M1 / R7 with the real core: CockpitSpace=0 against the same module with its Room removed: bit-identical.
void OffExact() {
  std::vector<float> out[2];
  const std::vector<Voice> vs = {{kCockpitCtx, 1, kNoise, 0.3f, 0, {0.4f, 0.1f, -0.2f}, 3, false},
                                 {kWorldCtx, 1, kSine, 0.2f, 300, {10.0f, 0.0f, 3.0f}, 5, true}};
  for (int pass = 0; pass < 2; ++pass) {
    Run t = Scene(Ini(pass == 0 ? 0 : 1), vs, 0, 150, [&](Rig&, int k) {
      if (pass == 1 && k == 0) {
        audr::Binding* b = audio::g_binding.load();
        if (b && b->room) audr::RoomDelete(b->room), b->room = nullptr;
      }
    });
    out[pass] = std::move(t.hooked);
  }
  Check(out[0].size() == out[1].size() && memcmp(out[0].data(), out[1].data(), sizeof(float) * out[0].size()) == 0,
        "cockpit space e2e R7: CockpitSpace=0 is bit-identical to the module without a Room (real core)");
}

// R8 through the module: 48 owned voices (24 in the cockpit, 24 outside through the canopy), Strong, the head turning
// 90 deg/s: the module's p99 per block (every voice's HRTF render and the room, wall time) within its CPU budget
// (25 % of a 10 ms block), the room's own p99 against the design's 0.15 ms target, no allocation, nothing dropped.
void Cpu() {
  std::vector<Voice> vs;
  for (int i = 0; i < 24; ++i) {
    const double a = i * 2 * kPi / 24, rr = 0.3 + 0.04 * (i % 10);
    vs.push_back({kCockpitCtx, i % 6 == 5 ? 2u : 1u, i % 2 ? kNoise : kSine, 0.03f, 150.0 + 37 * i,
                  {static_cast<float>(rr * std::cos(a)), static_cast<float>(-0.2 + 0.02 * (i % 7)), static_cast<float>(rr * std::sin(a))},
                  static_cast<uint32_t>(100 + i), false});
  }
  for (int i = 0; i < 24; ++i) {
    const double a = i * 2 * kPi / 24 + 0.1, rr = 5.0 + 2.0 * i;
    vs.push_back({kWorldCtx, 1, i % 2 ? kSine : kNoise, 0.03f, 90.0 + 53 * i,
                  {static_cast<float>(rr * std::cos(a)), static_cast<float>(1.0 + 0.3 * (i % 5)), static_cast<float>(rr * std::sin(a))},
                  static_cast<uint32_t>(200 + i), true});
  }
  for (int level : {2, 3}) {
    audiofake::g_rtAllocs = 0;
    double p99 = 0, roomP50 = 0, roomP99 = 0;
    uint32_t limit = 0;
    bool early = false;
    Run t = Scene(Ini(level), vs, 60, 1100, [&](Rig& r, int k) {
      double q[4];
      const double yaw = -0.5 * (k * 0.9) * kPi / 180.0;
      q[0] = 0, q[1] = std::sin(yaw), q[2] = 0, q[3] = std::cos(yaw);
      for (int i = 0; i < 24; ++i) r.w.SetOrient(i, q);
      audiofake::t_rtThread = k >= 60 && k < 60 + 1099;
      if (k == 60 + 1099) {  // the last block's figures (not counted: the reads allocate), before the rig goes down
        p99 = audio::P99Ms();
        audio::RoomTimes(&roomP50, &roomP99);
        limit = audio::g_cnt.limitNow.load();
        early = audio::g_cnt.roomEarly.load() != 0;
      }
    });
    audiofake::t_rtThread = false;
    const uint64_t allocs = audiofake::g_rtAllocs.load();
    const double budget = 10.0 * 25 / 100.0;
    printf("     48 voices, %s, head 90 deg/s: module p99 %.3f ms per 10 ms block (budget %.2f ms), room p50 %.3f / p99 "
           "%.3f ms (design target 0.15: %s), owned %u, sent to the room %u, early %s, back-off cap %u, allocations %llu\n",
           kLevelName[level], p99, budget, roomP50 / 1000, roomP99 / 1000, roomP99 / 1000 <= 0.15 ? "met" : "MISSED",
           t.owned, t.roomVoices, early ? "on" : "off", limit, static_cast<unsigned long long>(allocs));
    Check(t.owned == 48 && t.roomVoices == 48 && early && limit == binaural::kMaxSources && p99 > 0 && p99 <= budget &&
              roomP99 / 1000 <= 0.5 && allocs == 0 && AllFinite(t.hooked),
          level == 2 ? "cockpit space e2e R8 (Normal): 48 voices owned and sent to the room, the module's p99 within its "
                       "CPU budget, the room's p99 under 0.5 ms, nothing backed off, no allocation"
                     : "cockpit space e2e R8 (Strong): 48 voices owned and sent to the room, the module's p99 within its "
                       "CPU budget, the room's p99 under 0.5 ms, nothing backed off, no allocation");
  }
}

// M17, the live failure of 2026-11-26 (validation-08, F-16C): a cockpit mix like the flight's. 20 wide stereo voices near
// the head carrying the same engine signal (DCS's engine layers: six above-behind outside the box, three behind-below,
// ten at the consoles, panel and rails), a second engine near the head, four outside voices through the canopy; most of
// the power below 1 kHz, as much between 200 and 1000 Hz as below 200. The level match (the room's Render with the
// module's direct sound) holds each level to Off: broadband and K-weighted within 0.5 dB, 200-1000 Hz and every band
// from 100 Hz within 1 dB; the space's step against the kill switch per band stays within 1 dB of Off's own step.
std::vector<double> KWeight(const std::vector<double>& x) {
  const Biquad pre{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585};
  const Biquad rlb{1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
  return Filter(Filter(x, pre), rlb);
}
struct Lv {
  double all, k, band[4];  // 100-200, 200-1000, 1000-4000, 4000-12000
};
Lv Levels(const std::vector<float>& lr, size_t fromFrame) {
  Lv r{};
  const double eps = 1e-30;
  double eAll = 0, eK = 0, eb[4] = {};
  for (int ch = 0; ch < 2; ++ch) {
    const std::vector<double> x = Ear(lr, ch);
    eAll += Sum2(x, fromFrame);
    eK += Sum2(KWeight(x), fromFrame);
    const double lo[4] = {100, 200, 1000, 4000}, hi[4] = {200, 1000, 4000, 12000};
    for (int b = 0; b < 4; ++b)
      eb[b] += Sum2(Filter(Filter(Filter(Filter(x, HighPass(lo[b])), HighPass(lo[b])), LowPass(hi[b])), LowPass(hi[b])), fromFrame);
  }
  r.all = 10 * std::log10(eAll + eps), r.k = 10 * std::log10(eK + eps);
  for (int b = 0; b < 4; ++b) r.band[b] = 10 * std::log10(eb[b] + eps);
  return r;
}
void LiveMix() {
  // ed_sound's master gain after the voices (the F-16's flight: about -9 dB): what skips it shows up at once.
  g_fakeMasterGain = 0.35f;
  const std::vector<uint32_t> sizes = {480, 441, 512, 480, 448, 496};  // per-block device frames, varying
  std::vector<Voice> vs;
  const float A[3] = {-0.134f, 0.419f, 0.0f}, B[3] = {-0.834f, -0.581f, 0.0f};
  for (int k = 0; k < 6; ++k) vs.push_back({kCockpitCtx, 2, kEngine, 0.30f - 0.03f * k, 88, {A[0], A[1], A[2]}, 77, false});
  for (int k = 0; k < 3; ++k) vs.push_back({kCockpitCtx, 2, kEngine, 0.22f, 88, {B[0], B[1], B[2]}, 77, false});
  const float spots[10][3] = {{0.15f, -0.25f, -0.41f}, {0.15f, -0.25f, 0.41f}, {0.75f, -0.28f, 0.0f}, {0.39f, 0.225f, 0.0f},
                              {-0.30f, 0.18f, 0.0f},   {0.0f, 0.2f, -0.35f},   {0.0f, 0.2f, 0.35f},   {-0.2f, -0.3f, 0.5f},
                              {-0.2f, -0.3f, -0.5f},   {0.5f, 0.1f, 0.0f}};
  for (const auto& p : spots) vs.push_back({kCockpitCtx, 2, kEngine, 0.08f, 88, {p[0], p[1], p[2]}, 77, false});
  for (const auto& p : spots)  // the world context near the head too (the F-16's ctx 3 voices of its first flight)
    vs.push_back({kWorldCtx, 2, kEngine, 0.03f, 61, {p[0] * 1.5f, p[1] * 1.5f, p[2] * 1.5f}, 91, false});
  vs.push_back({kCockpitCtx, 2, kEngine, 0.10f, 61, {0.166f, -0.081f, 0.0f}, 91, false});
  for (int k = 0; k < 4; ++k) {
    const double a = k * kPi / 2 + 0.3;
    vs.push_back({kWorldCtx, 1, kNoise, 0.02f, 0, {static_cast<float>(30 * std::cos(a)), 2.0f, static_cast<float>(30 * std::sin(a))},
                  static_cast<uint32_t>(120 + k), true});
  }
  constexpr int kSettle = 60, kBlocks = 900;
  const size_t from = size_t(400) * kFrames;  // the match settles within 3 s (kRoomMatchTauS, its slew)
  uint64_t afterMix = 0, inMix = 0;
  auto run = [&](int level, bool kill) {
    const uint64_t a0 = audio::g_cnt.roomFlushAfterMix.load(), i0 = audio::g_cnt.roomFlushInMix.load();
    Run t = Scene(Ini(level), vs, kSettle, kBlocks, [&](Rig&, int k) {
      if (k == kSettle) audio::g_userOff = kill;
    }, &sizes);
    afterMix += audio::g_cnt.roomFlushAfterMix.load() - a0, inMix += audio::g_cnt.roomFlushInMix.load() - i0;
    return t;
  };
  const Run offRun = run(0, false), killRun = run(0, true);
  audio::g_userOff = false;
  const Lv off = Levels(offRun.hooked, from), kill = Levels(killRun.hooked, from);
  printf("     live-like mix (%zu voices, 20 carrying one engine): Off vs kill switch all %+.2f K %+.2f, 100-200 %+.2f "
         "200-1k %+.2f 1-4k %+.2f 4-12k %+.2f dB\n", vs.size(), off.all - kill.all, off.k - kill.k, off.band[0] - kill.band[0],
         off.band[1] - kill.band[1], off.band[2] - kill.band[2], off.band[3] - kill.band[3]);
  bool level = true, killStep = true, finite = true;
  for (int lv = 1; lv <= 3; ++lv) {
    const Run on = run(lv, false);
    finite = finite && AllFinite(on.hooked);
    const Lv l = Levels(on.hooked, from);
    printf("     %-7s vs Off: all %+.2f K %+.2f, 100-200 %+.2f 200-1k %+.2f 1-4k %+.2f 4-12k %+.2f dB (d %.2f dB, %u voices "
           "sent); vs kill switch 100-200 %+.2f 200-1k %+.2f 1-4k %+.2f 4-12k %+.2f dB\n",
           kLevelName[lv], l.all - off.all, l.k - off.k, l.band[0] - off.band[0], l.band[1] - off.band[1],
           l.band[2] - off.band[2], l.band[3] - off.band[3], on.dcompDb, on.roomVoices, l.band[0] - kill.band[0],
           l.band[1] - kill.band[1], l.band[2] - kill.band[2], l.band[3] - kill.band[3]);
    level = level && std::fabs(l.all - off.all) <= 0.5 && std::fabs(l.k - off.k) <= 0.5;
    for (int b = 0; b < 4; ++b) {
      level = level && std::fabs(l.band[b] - off.band[b]) <= 1.0;
      // The space's own share of the step against the kill switch: Off's step (the direct path's, scenarios 1-4) plus
      // at most 1 dB.
      killStep = killStep && std::fabs((l.band[b] - kill.band[b]) - (off.band[b] - kill.band[b])) <= 1.0;
    }
  }
  Check(level, "cockpit space e2e M17: the live-like mix (correlated engine layers, wide stereo voices near the head, "
               "low-heavy) at Subtle, Normal and Strong: broadband and K-weighted within 0.5 dB of Off, every band from "
               "100 Hz (200-1000 Hz included) within 1 dB");
  Check(killStep, "cockpit space e2e M17: the step against the kill switch, per band from 100 Hz, within 1 dB of Off's own");
  Check(finite, "cockpit space e2e M17: every output sample finite");
  printf("     the room's Render inside the mix (at the last voice) %llu times, after it %llu\n",
         static_cast<unsigned long long>(inMix), static_cast<unsigned long long>(afterMix));
  Check(inMix > 0 && afterMix == 0, "cockpit space e2e M17: the room and the direct sound go in inside ed_sound's mix "
                                    "(at its last voice, before the master gain), never after it");
  g_fakeMasterGain = 1.0f;
}

// M18: the flush point when the call pattern changes (round 3: 2185 / 3579 blocks of a mission load flushed after the
// mix). The list's last voice is never rendered (the mix passes it, as ed_sound passes voices that do not play), voices
// start one by one and end one by one, the master gain is 0.35, block sizes vary. With the room's wet weight above 0 not
// one block may reach `out` after the master stage; the mix stays at Off's level while the voices come and go.
void BuildUp() {
  g_fakeMasterGain = 0.35f;
  const std::vector<uint32_t> sizes = {480, 441, 512, 448};
  auto run = [&](int level, uint64_t* after, uint64_t* inMix) {
    Rig r;
    RigUp(r, Ini(level).c_str());
    SetCtxCockpitGain(r, kWorldCtx, true);
    // voice 0: created first, so the list's tail; never rendered
    const int quiet = AddBoth(r, kWorldCtx, true, 1, kNoise, 0.01f, 0, 1.0f, 5);
    PosBoth(r, quiet, 40, 2, 10);
    W<uint8_t>(r.w.voices[quiet], kVSkip, 1), W<uint8_t>(r.ref.voices[quiet], kVSkip, 1);
    const int anchor = AddBoth(r, kWorldCtx, true, 1, kSine, 0.02f, 220, 1.0f, 6);
    PosBoth(r, anchor, 25, 1, -8);
    CreateVoice(r, quiet), CreateVoice(r, anchor);
    audio::LuaAircraft("F-16C_50");
    audio::LuaState(0.0, 1, 1);
    Bind(r);
    Tick();
    const uint64_t a0 = audio::g_cnt.roomAfterWet.load(), i0 = audio::g_cnt.roomFlushInMix.load();
    std::vector<float> a, b, hooked;
    std::vector<int> voices;
    for (int k = 0; k < 700; ++k) {
      if (k >= 50 && k < 290 && k % 12 == 2) {  // twenty wide engine layers come in, one every 0.12 s
        const int n = static_cast<int>(voices.size());
        const float pos[3] = {-0.134f + 0.02f * (n % 5), 0.419f - 0.03f * (n % 3), 0.05f * (n % 4) - 0.075f};
        const int i = AddBoth(r, kCockpitCtx, true, 2, kEngine, 0.05f, 88, 1.0f, 77);
        PosBoth(r, i, pos[0], pos[1], pos[2]);
        r.w.SetOrient(i, kIdentity);
        CreateVoice(r, i);
        voices.push_back(i);
      }
      if (k >= 420 && k < 660 && k % 12 == 0 && !voices.empty()) {  // and end one by one
        const int i = voices.back();
        voices.pop_back();
        W<int64_t>(r.w.voices[i], kVRemaining, 240), W<int64_t>(r.ref.voices[i], kVRemaining, 240);
      }
      SimFrame(r);
      const uint32_t n = sizes[static_cast<size_t>(k) % sizes.size()];
      a.resize(2 * size_t(n)), b.resize(2 * size_t(n));
      Block(r, a.data(), b.data(), n);
      if (k == 100) Tick();
      hooked.insert(hooked.end(), a.begin(), a.end());
    }
    *after = audio::g_cnt.roomAfterWet.load() - a0, *inMix = audio::g_cnt.roomFlushInMix.load() - i0;
    RigDown(r);
    return hooked;
  };
  uint64_t afterOff = 0, inOff = 0;
  const std::vector<float> off = run(0, &afterOff, &inOff);
  bool ok = true, finite = true;
  for (int level = 1; level <= 3; level += 2) {
    uint64_t after = 0, inMix = 0;
    const std::vector<float> on = run(level, &after, &inMix);
    finite = finite && AllFinite(on);
    // the level with all twenty playing (blocks 300-400)
    double eOn = 0, eOff = 0;
    const size_t from = size_t(300) * 470 * 2, to = std::min(off.size(), size_t(400) * 470 * 2);
    for (size_t i = from; i < to && i < on.size(); ++i) eOn += double(on[i]) * on[i], eOff += double(off[i]) * off[i];
    const double db = 10 * std::log10(eOn / eOff);
    // the first blocks after the bind, one by one (review round 2: the master gain not measured yet)
    double first = 0;
    size_t at = 0;
    for (int k = 0; k < 50; ++k) {
      const uint32_t n = sizes[static_cast<size_t>(k) % sizes.size()];
      double e1 = 0, e0 = 0;
      for (size_t i = at; i < at + 2 * size_t(n) && i < on.size() && i < off.size(); ++i) e1 += double(on[i]) * on[i], e0 += double(off[i]) * off[i];
      at += 2 * size_t(n);
      if (e0 > 1e-12) first = std::max(first, std::fabs(10 * std::log10(e1 / e0)));
    }
    printf("     voices coming and going, the list's last voice never rendered, %s: flushes inside the mix %llu, after it "
           "with the room wet %llu; level with all playing %+.2f dB vs Off, worst of the first 50 blocks %.2f dB\n",
           kLevelName[level], static_cast<unsigned long long>(inMix), static_cast<unsigned long long>(after), db, first);
    ok = ok && after == 0 && inMix > 0 && std::fabs(db) <= 0.5 && first <= 2.0;  // (the old fallback: +9 dB)
  }
  Check(ok, "cockpit space e2e M18: voices coming and going and the list's last voice never rendered: the room and the "
            "direct sound never reach `out` after the master stage, and the level stays at Off's (0.5 dB)");
  Check(finite, "cockpit space e2e M18: every output sample finite");
  g_fakeMasterGain = 1.0f;
}

// Review round 1 and 2: the list grows (new voices come in at its front, as the fake's lock-free list hands them in)
// and shrinks while every voice is rendered. The room flushes at the list's real last node every block (no call
// after it), and no block is louder or softer than Off by more than 1 dB; the first blocks after the bind included
// (the master gain not measured yet: the dry path waits for a block flushed inside the mix).
void GrowShrink() {
  g_fakeMasterGain = 0.35f;
  auto run = [&](int level, uint64_t* late, uint64_t* after) {
    Rig r;
    RigUp(r, Ini(level).c_str());
    SetCtxCockpitGain(r, kWorldCtx, true);
    const int anchor = AddBoth(r, kCockpitCtx, true, 2, kEngine, 0.05f, 88, 1.0f, 77);
    PosBoth(r, anchor, -0.13, 0.42, 0.0);
    r.w.SetOrient(anchor, kIdentity);
    CreateVoice(r, anchor);
    audio::LuaAircraft("F-16C_50");
    audio::LuaState(0.0, 1, 1);
    const uint64_t l0 = audio::g_cnt.roomLateCalls.load(), a0 = audio::g_cnt.roomAfterWet.load();
    Bind(r);
    Tick();
    std::vector<float> a(960), b(960), hooked;
    std::vector<int> voices;
    for (int k = 0; k < 500; ++k) {
      if (k >= 20 && k < 260 && k % 6 == 0) {  // forty voices come in, one every 60 ms
        const int i = AddBoth(r, k % 12 ? kCockpitCtx : kWorldCtx, true, 2, kEngine, 0.02f + 0.001f * (k % 7), 88, 1.0f, 77);
        PosBoth(r, i, 0.3 * std::cos(k * 0.3), -0.2 + 0.01 * (k % 9), 0.3 * std::sin(k * 0.3));
        if (k % 12) r.w.SetOrient(i, kIdentity);
        CreateVoice(r, i);
        voices.push_back(i);
      }
      if (k >= 300 && k < 480 && k % 6 == 0 && !voices.empty()) {
        const int i = voices.back();
        voices.pop_back();
        W<int64_t>(r.w.voices[i], kVRemaining, 100), W<int64_t>(r.ref.voices[i], kVRemaining, 100);
      }
      SimFrame(r);
      Block(r, a.data(), b.data(), 480);
      if (k == 100) Tick();
      hooked.insert(hooked.end(), a.begin(), a.end());
    }
    *late = audio::g_cnt.roomLateCalls.load() - l0, *after = audio::g_cnt.roomAfterWet.load() - a0;
    RigDown(r);
    return hooked;
  };
  uint64_t lateOff = 0, afterOff = 0, late = 0, after = 0;
  const std::vector<float> off = run(0, &lateOff, &afterOff), on = run(2, &late, &after);
  double worst = 0;
  int worstBlock = -1;
  for (size_t k = 0; k < 500 && (k + 1) * 960 <= on.size() && (k + 1) * 960 <= off.size(); ++k) {
    double eOn = 0, eOff = 0;
    for (size_t i = k * 960; i < (k + 1) * 960; ++i) eOn += double(on[i]) * on[i], eOff += double(off[i]) * off[i];
    if (eOff < 1e-9) continue;
    const double db = std::fabs(10 * std::log10(eOn / eOff));
    if (db > worst) worst = db, worstBlock = static_cast<int>(k);
  }
  printf("     the list growing and shrinking (all rendered), Normal: calls after the flush %llu, after the master stage "
         "%llu; worst block vs Off %.2f dB (block %d)\n", static_cast<unsigned long long>(late),
         static_cast<unsigned long long>(after), worst, worstBlock);
  Check(late == 0 && after == 0 && worst <= 1.0 && AllFinite(on),
        "cockpit space e2e: with the list growing at its front and shrinking, the room flushes at its real last node "
        "(no voice after it), and every block, the first after the bind too, stays within 1 dB of Off");
  g_fakeMasterGain = 1.0f;
}

void RoomTailHonoursMute() {
  Rig r;
  RigUp(r, Ini(2).c_str());
  SetCtxCockpitGain(r, kWorldCtx, true);
  const int voice = AddBoth(r, kCockpitCtx, true, 2, kEngine, 0.12f, 88, 1.0f, 77);
  PosBoth(r, voice, -0.13, 0.42, 0.0);
  r.w.SetOrient(voice, kIdentity);
  CreateVoice(r, voice);
  audio::LuaAircraft("F-16C_50");
  audio::LuaState(0.0, 1, 1);
  Bind(r);
  Tick();
  std::vector<float> actual(960), stock(960);
  for (int k = 0; k < 80; ++k) {
    SimFrame(r);
    Block(r, actual.data(), stock.data(), 480);
  }
  // Let the mixer observe the muted gain before it stops invoking the voice renderer.
  g_fakeMasterGain = 0.0f;
  SimFrame(r);
  Block(r, actual.data(), stock.data(), 480);
  W<uint8_t>(r.w.voices[voice], kVSkip, 1), W<uint8_t>(r.ref.voices[voice], kVSkip, 1);
  const auto flushes = audio::g_cnt.roomFlushAfterMix.load();
  SimFrame(r);
  Block(r, actual.data(), stock.data(), 480);
  Check(audio::g_cnt.roomFlushAfterMix.load() > flushes &&
            std::all_of(actual.begin(), actual.end(), [](float sample) { return sample == 0.0f; }),
        "cockpit space e2e: room tail added after the mix stays silent after the master is muted");
  RigDown(r);
  g_fakeMasterGain = 1.0f;
}

void RunAll() {
  printf("---- cockpit space end to end (real Room)\n");
  Externalisation();
  Level();
  LiveMix();
  BuildUp();
  GrowShrink();
  RoomTailHonoursMute();
  Clicks();
  OffExact();
  Cpu();
}

}  // namespace space

}  // namespace e2e

int main() {
  synth::Options o;
  o.headRadius = 0.0875;
  e2e::g_set = synth::Build(o);
  audiotest::g_rigHrtf = &e2e::SyntheticHrtf;
  printf("---- binaural audio end to end (fake ed_sound, real BinauralCore, synthetic spherical-head HRTF)\n");
  e2e::Circle();
  e2e::Radio();
  e2e::Stereo();
  e2e::Wide();
  e2e::space::RunAll();
  audio::g_testNoThread = false;
  printf("%d failure(s)\n", g_fail);
  return g_fail ? 1 : 0;
}
