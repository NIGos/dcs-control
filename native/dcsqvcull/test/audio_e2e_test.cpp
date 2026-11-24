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
//   3. no heap allocation on the simulation / render thread with the real core, every sample finite.
// Built as DcsQvCullAudioE2ETests (ctest); the module's own scenarios run against the stand-in core in DcsQvCullTests.
#include "../src/main.cpp"
#include "audio_fake_core.h"  // only the real-time allocation counter (audio_rt_alloc.cpp) is linked here
#include "synthetic_hrtf.h"   // native/binaural/test

#include <complex>
#include <memory>

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
    "MaxObjects=48\r\n";

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
  const float radius = binaural_config::HeadRadiusFromCircumferenceCm(kCircumferenceCm);
  char err[160] = {};
  binaural::HrtfSet* set = binaural::LoadHrtf(g_set.data(), g_set.size() * 4, kRate, err, sizeof(err));
  auto direct = std::make_unique<binaural::Renderer>();
  binaural::Config cc;
  cc.sampleRate = kRate;
  cc.maxSources = 4;
  cc.headRadiusM = radius;
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
        "audio e2e: the ITD follows Woodworth for the configured head (57 cm, r = 9.07 cm) all around the head, "
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

}  // namespace e2e

int main() {
  synth::Options o;
  o.headRadius = 0.0875;
  e2e::g_set = synth::Build(o);
  audiotest::g_rigHrtf = &e2e::SyntheticHrtf;
  printf("---- binaural audio end to end (fake ed_sound, real BinauralCore, synthetic spherical-head HRTF)\n");
  e2e::Circle();
  e2e::Radio();
  audio::g_testNoThread = false;
  printf("%d failure(s)\n", g_fail);
  return g_fail ? 1 : 0;
}
