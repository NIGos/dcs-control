// Offline checks of native/quadviews/dcsvr_gaze.h: the saccade lead and the fixation deadzone of the eye-tracked
// focus area, on simulated gaze at DCS's rendered frame rates with frame generation (45 FPS in 2X, 30 FPS in 3X).
// Built and run by scripts/build-quadviews.ps1.
#include <cmath>
#include <cstdio>
#include <random>

#include "dcsvr_gaze.h"

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

constexpr float Rad = 0.0174532925f;
// A Crystal-like eye view: about 50 degrees each way.
const float HalfTan = std::tan(50.f * Rad);
const float NdcPerDeg = dcsvr::GazeFilter::NdcPerDegree(-HalfTan, HalfTan);

// Gaze unit vector for a horizontal angle (degrees, right positive) looking down -Z, and its NDC position.
void Unit(float deg, float& x, float& y, float& z) { x = std::sin(deg * Rad); y = 0.f; z = -std::cos(deg * Rad); }
float Ndc(float deg) { return std::tan(deg * Rad) / HalfTan; }

struct Frame {
    bool widened;
    dcsvr::GazeVec2 min, max, gaze;
};

// One located frame for both eyes with the gaze at `deg`; the focus box is +-section around the (filtered) gaze.
Frame Locate(dcsvr::GazeFilter& f, const dcsvr::GazeSettings& s, int64_t t, float deg, float section = 0.34f) {
    float x, y, z;
    Unit(deg, x, y, z);
    f.UpdateSpeed(t, x, y, z, s);
    Frame out{};
    for (int eye = 0; eye < 2; ++eye) {
        const dcsvr::GazeVec2 g = f.Eye(eye, t, {Ndc(deg), 0.f}, NdcPerDeg, NdcPerDeg, s);
        dcsvr::GazeVec2 lo{std::fmax(-1.f, g.x - section), std::fmax(-1.f, g.y - section)};
        dcsvr::GazeVec2 hi{std::fmin(1.f, g.x + section), std::fmin(1.f, g.y + section)};
        const bool widened = f.Extend(eye, t, lo, hi, s);
        f.Record(eye, t, lo, hi);
        if (eye == 0) out = {widened, lo, hi, g};
    }
    return out;
}
} // namespace

int main() {
    dcsvr::GazeSettings on;
    on.widening = true;
    on.deadzoneDeg = 0.5f;
    on.leadMs = 55.f;

    // A 25-degree saccade at about 450 deg/s, sampled at 45 FPS (2X): the gaze moves 0 -> 10 -> 20 -> 25 degrees.
    {
        dcsvr::GazeFilter f;
        const int64_t dt = 22'222'222;
        int64_t t = 1'000'000'000;
        Locate(f, on, t, 0.f);
        const Frame a = Locate(f, on, t += dt, 10.f);
        Check(f.InSaccade() && f.Stats().saccades == 1, "2X: a 450 deg/s move starts a saccade");
        Check(a.widened && a.max.x > Ndc(10.f) + 0.34f + 0.05f && std::fabs(a.min.x - (Ndc(10.f) - 0.34f)) < 1e-5f,
              "2X: the focus box reaches ahead to the right only");
        const Frame b = Locate(f, on, t += dt, 20.f);
        Check(b.widened && b.max.x >= Ndc(25.f), "2X: the box rendered mid-saccade already covers the 25-degree landing");
        Check(b.max.x - (Ndc(20.f) + 0.34f) <= on.maxExtend + 1e-5f, "2X: the reach is clamped to maxExtend");
        // At rest a real tracker never repeats a sample exactly: a hundredth of a degree of noise (an exact repeat
        // is the tracker having nothing new, see the repeated-sample checks below).
        const Frame c = Locate(f, on, t += dt, 25.f);
        const Frame d = Locate(f, on, t += dt, 25.01f);
        Check(!f.InSaccade() && d.widened && d.max.x - (Ndc(25.f) + 0.34f) < c.max.x - (Ndc(25.f) + 0.34f),
              "2X: after the eye rests the reach fades out over the 40 ms hold");
        const Frame e = Locate(f, on, t += dt, 24.99f);
        Check(!e.widened && std::fabs(e.max.x - (Ndc(25.f) + 0.34f)) < 1e-5f, "2X: then the normal box again");
    }

    // The same saccade in 3X (30 FPS, longer lead): the reach still covers the landing.
    {
        dcsvr::GazeFilter f;
        dcsvr::GazeSettings s = on;
        s.leadMs = 85.f;
        const int64_t dt = 33'333'333;
        int64_t t = 1'000'000'000;
        Locate(f, s, t, 0.f);
        const Frame a = Locate(f, s, t += dt, 15.f);
        Check(a.widened && a.max.x >= Ndc(25.f), "3X: the first frame of the saccade covers the 25-degree landing");
    }

    // A leftward, upward-free saccade extends the left edge.
    {
        dcsvr::GazeFilter f;
        int64_t t = 1'000'000'000;
        Locate(f, on, t, 0.f);
        const Frame a = Locate(f, on, t += 22'222'222, -12.f);
        Check(a.widened && a.min.x < Ndc(-12.f) - 0.34f - 0.05f && std::fabs(a.max.x - (Ndc(-12.f) + 0.34f)) < 1e-5f,
              "a leftward saccade extends only the left edge");
    }

    // Fixation with tracker noise (0.3 degrees RMS): the focus stays put, and a real 3-degree move still follows at once.
    {
        dcsvr::GazeFilter f;
        std::mt19937 rng(7);
        std::normal_distribution<float> noise(0.f, 0.3f);
        int64_t t = 1'000'000'000;
        const float first = Locate(f, on, t, 5.f).gaze.x;
        int moved = 0, widened = 0;
        for (int i = 0; i < 200; ++i) {
            const Frame fr = Locate(f, on, t += 22'222'222, 5.f + std::fmax(-0.45f, std::fmin(0.45f, noise(rng))));
            if (fr.gaze.x != first) ++moved;
            if (fr.widened) ++widened;
        }
        Check(moved == 0, "fixation noise within the deadzone never moves the focus");
        Check(widened == 0 && f.Stats().saccades == 0, "fixation noise is never taken for a saccade");
        const Frame shift = Locate(f, on, t += 22'222'222, 8.f);
        Check(std::fabs(shift.gaze.x - Ndc(8.f)) < 1e-6f, "a 3-degree move is followed in the same frame (no lag)");
    }

    // Off: no widening, no deadzone.
    {
        dcsvr::GazeFilter f;
        dcsvr::GazeSettings off;
        int64_t t = 1'000'000'000;
        Locate(f, off, t, 0.f);
        const Frame a = Locate(f, off, t += 22'222'222, 10.f);
        const Frame b = Locate(f, off, t += 22'222'222, 10.2f);
        Check(!a.widened && std::fabs(b.gaze.x - Ndc(10.2f)) < 1e-6f, "with both options off the focus follows the gaze unchanged");
    }

    // The same display time located twice counts once; a gap (blink, pause) is never a saccade.
    {
        dcsvr::GazeFilter f;
        int64_t t = 1'000'000'000;
        Locate(f, on, t, 0.f);
        Locate(f, on, t, 0.f);
        Check(f.Stats().eyeFrames == 2, "a frame located twice is counted once per eye");
        Locate(f, on, t += 400'000'000, 20.f);
        Check(!f.InSaccade(), "a 400 ms gap (blink) is not a saccade");
        float x, y, z;
        Unit(0.f, x, y, z);
        f.UpdateSpeed(t + 22'222'222, std::nanf(""), y, z, on);
        Check(!f.InSaccade(), "a non-finite gaze changes nothing");
    }

    // DCS locating views for nearly the same display time twice: never a speed sample (no bogus saccade or peak), and a
    // frame held by the deadzone is counted once however often its views are located.
    {
        dcsvr::GazeFilter f;
        int64_t t = 1'000'000'000;
        Locate(f, on, t, 5.f);
        Locate(f, on, t + 1'000, 5.3f);  // 1 us later, 0.3 degrees of noise: would read as 300,000 deg/s
        Check(!f.InSaccade() && f.Stats().saccades == 0 && f.Stats().peakSpeed < 1000.f, "a display time 1 us later is not a speed sample");
        for (int i = 0; i < 20; ++i) {
            t += 22'222'222;
            Locate(f, on, t, 5.1f);
            Locate(f, on, t, 5.1f);
        }
        Check(f.Stats().held <= f.Stats().eyeFrames, "held frames never exceed eye-frames");
    }

    // Saccade tail: a saccade that decelerates before landing (0 -> 12 -> 20 -> 22.5 -> 23.5 degrees at 45 FPS, i.e. 540,
    // 360, 112, 45 deg/s). The 112 deg/s frame is the last one rendered before the landing and reaches the headset as the
    // eye arrives: it stays in the saccade and keeps the farthest reach; the old single threshold dropped it.
    {
        const int64_t dt = 22'222'222;
        auto run = [&](const dcsvr::GazeSettings& s, bool& tailInSaccade, bool& landedInSaccade, Frame& peak, Frame& tail) {
            dcsvr::GazeFilter f;
            int64_t t = 1'000'000'000;
            Locate(f, s, t, 0.f);
            Locate(f, s, t += dt, 12.f);
            peak = Locate(f, s, t += dt, 20.f);
            tail = Locate(f, s, t += dt, 22.5f);
            tailInSaccade = f.InSaccade();
            Locate(f, s, t += dt, 23.5f);
            landedInSaccade = f.InSaccade();
        };
        bool tailIn{}, landedIn{}, oldTailIn{}, oldLandedIn{};
        Frame peak{}, tail{}, oldPeak{}, oldTail{};
        run(on, tailIn, landedIn, peak, tail);
        dcsvr::GazeSettings old = on;
        old.endSpeed = old.speed;
        old.latch = false;
        run(old, oldTailIn, oldLandedIn, oldPeak, oldTail);
        Check(tailIn && !landedIn, "tail: the decelerating frame stays in the saccade, the 45 deg/s landing ends it");
        Check(!oldTailIn, "tail: with one threshold and no latch (the old behaviour) that frame had left the saccade");
        Check(tail.widened && tail.max.x >= peak.max.x - 1e-5f, "tail: the leading edge does not retreat while the eye decelerates");
        Check(tail.max.x > oldTail.max.x + 0.05f, "tail: the frame before the landing reaches farther than before");
    }

    // A saccade that turns back (0 -> 10 -> 20 -> 8 degrees, never slow enough to end): the latch lets go at once and
    // the box reaches left only, never toward the abandoned point on the right.
    {
        dcsvr::GazeFilter f;
        const int64_t dt = 22'222'222;
        int64_t t = 1'000'000'000;
        Locate(f, on, t, 0.f);
        Locate(f, on, t += dt, 10.f);
        Locate(f, on, t += dt, 20.f);
        const Frame back = Locate(f, on, t += dt, 8.f);
        Check(f.Stats().saccades == 1 && back.widened && back.min.x < Ndc(8.f) - 0.34f - 0.05f &&
                  std::fabs(back.max.x - (Ndc(8.f) + 0.34f)) < 1e-5f,
              "reversal: the box follows the new direction, the right edge is back to normal");
    }

    // The next saccade starts a new reach: a small 4-degree move right after a large one in the same direction gets
    // its own lead, not the previous saccade's farther point.
    {
        dcsvr::GazeFilter f;
        const int64_t dt = 22'222'222;
        int64_t t = 1'000'000'000;
        Locate(f, on, t, 0.f);
        Locate(f, on, t += dt, 10.f);
        Locate(f, on, t += dt, 20.f);
        for (int i = 0; i < 6; ++i) Locate(f, on, t += dt, 20.f + ((i % 2) ? 0.f : 0.01f));  // ends on exactly 20
        Check(!f.InSaccade(), "second saccade: the first one has ended");
        const Frame small = Locate(f, on, t += dt, 24.f);
        const float lead = std::fmin(on.maxExtend, (Ndc(24.f) - Ndc(20.f)) * on.leadMs / 1000.f / (dt / 1e9f));
        Check(f.Stats().saccades == 2 && std::fabs(small.max.x - (Ndc(24.f) + 0.34f + lead)) < 1e-4f,
              "second saccade: its reach is its own velocity times the latency");
    }

    // Tracker noise right after a landing (0.3 degrees RMS) ends the saccade and never keeps it going.
    {
        dcsvr::GazeFilter f;
        std::mt19937 rng(11);
        std::normal_distribution<float> noise(0.f, 0.3f);
        const int64_t dt = 22'222'222;
        int64_t t = 1'000'000'000;
        Locate(f, on, t, 0.f);
        Locate(f, on, t += dt, 10.f);
        Locate(f, on, t += dt, 20.f);
        int inSaccade = 0;
        for (int i = 0; i < 200; ++i) {
            Locate(f, on, t += dt, 20.f + std::fmax(-0.45f, std::fmin(0.45f, noise(rng))));
            if (i >= 1 && f.InSaccade()) ++inSaccade;
        }
        Check(inSaccade == 0 && f.Stats().saccades == 1, "noise after a landing is below the end speed: the saccade ends");
    }

    // The hold outlasts the latency (3X: 85 ms lead, 100 ms hold): frames displayed up to 85 ms after the last saccade
    // frame are still widened, so the frames in flight at the landing all reach toward it.
    {
        dcsvr::GazeFilter f;
        dcsvr::GazeSettings s = on;
        s.leadMs = 85.f;
        s.holdMs = 100.f;
        s.maxExtend = 0.6f;
        const int64_t dt = 11'111'111;  // display times at 90 Hz (generated frames are located too)
        int64_t t = 1'000'000'000;
        Locate(f, s, t, 0.f);
        Locate(f, s, t += 33'333'333, 15.f);
        const int64_t last = t;
        int widenedWithinLatency = 0, frames = 0;
        for (t += dt; t - last <= 85'000'000; t += dt, ++frames)
            if (Locate(f, s, t, 15.f + ((frames % 2) ? 0.01f : -0.01f)).widened) ++widenedWithinLatency;
        Check(frames > 0 && widenedWithinLatency == frames, "hold: every frame within the latency after the saccade is widened");
    }

    // Repeated samples: the tracker has nothing new for one frame in the middle of a saccade at 90 Hz and Quad Views
    // hands out the last gaze again. Before, that frame read as 0 deg/s and ended the saccade and its lead.
    {
        dcsvr::GazeFilter f;
        dcsvr::GazeSettings s = on;
        s.leadMs = 25.f;
        const int64_t dt = 11'111'111;
        int64_t t = 1'000'000'000;
        Locate(f, s, t, 0.f);
        Locate(f, s, t += dt, 4.f);
        const Frame before = Locate(f, s, t += dt, 9.f);
        const Frame repeat = Locate(f, s, t += dt, 9.f);  // the same sample again
        Check(f.InSaccade() && repeat.widened && repeat.max.x >= before.max.x - 1e-5f && f.Stats().repeated == 1,
              "repeat: a repeated sample keeps the saccade and its reach");
        Locate(f, s, t += dt, 18.f);
        Check(f.InSaccade() && f.Speed() > 300.f && f.Speed() < 500.f,
              "repeat: the next real sample measures the speed over both frames (9 deg in 22 ms)");
        // The tracker loses the eyes and Quad Views repeats the last gaze for up to 600 ms: the saccade ends after 100 ms.
        for (int i = 0; i < 10; ++i) Locate(f, s, t += dt, 18.f);
        Check(!f.InSaccade(), "repeat: a repeat lasting 100 ms ends the saccade");
        Check(f.Stats().eyeFrames == 2 * 15, "repeat: repeated frames still count as frames");
    }

    // Landing check: a 30-degree saccade over 3 frames at 90 Hz with one frame of lead (25 ms). Without the lead the box
    // on screen as the eye lands is still centred on the gaze of 25 ms before: a miss. With it, it reaches the landing.
    {
        auto landing = [&](bool widening, uint64_t& landings, uint64_t& missed) {
            dcsvr::GazeFilter f;
            dcsvr::GazeSettings s = on;
            s.widening = widening;
            s.leadMs = 25.f;
            const int64_t dt = 11'111'111;
            int64_t t = 1'000'000'000;
            for (int i = 0; i < 5; ++i) Locate(f, s, t += dt, (i % 2) ? 0.01f : -0.01f);
            Locate(f, s, t += dt, 10.f);
            Locate(f, s, t += dt, 20.f);
            Locate(f, s, t += dt, 30.f);
            for (int i = 0; i < 12; ++i) Locate(f, s, t += dt, 30.f + ((i % 2) ? 0.01f : -0.01f));
            landings = f.Stats().landings;
            missed = f.Stats().missed;
        };
        uint64_t landingsOff{}, missedOff{}, landingsOn{}, missedOn{};
        landing(false, landingsOff, missedOff);
        landing(true, landingsOn, missedOn);
        Check(landingsOff == 1 && missedOff == 1, "landing: without the lead the eye lands outside the box on screen");
        Check(landingsOn == 1 && missedOn == 0, "landing: with the lead it lands inside");
    }

    // Lost gaze: the focus eases from the last gaze to the fixed focus over 150 ms, then the caller shows the fixed one;
    // a gaze seen again resets it.
    {
        dcsvr::GazeFilter f;
        dcsvr::GazeSettings s = on;
        s.lostEaseMs = 150.f;
        const int64_t dt = 11'111'111;
        int64_t t = 1'000'000'000;
        Locate(f, s, t, 20.f);
        const dcsvr::GazeVec2 last{Ndc(20.f), 0.f}, fixed{-0.05f, 0.f};
        dcsvr::GazeVec2 out{};
        bool eased = true, monotonic = true;
        float previous = last.x;
        int frames = 0;
        for (t += dt; f.Ease(0, t, last, fixed, s, out); t += dt, ++frames) {
            monotonic = monotonic && out.x <= previous + 1e-6f && out.x >= fixed.x - 1e-6f;
            previous = out.x;
        }
        eased = frames >= 12 && frames <= 14;
        Check(eased && monotonic, "lost gaze: the focus eases to the fixed one over 150 ms (13 frames at 90 Hz)");
        Check(f.Stats().eased == 1, "lost gaze: counted once");
        Locate(f, s, t += dt, 5.f);
        Check(f.Ease(0, t += dt, {Ndc(5.f), 0.f}, fixed, s, out) && std::fabs(out.x - Ndc(5.f)) < 1e-4f,
              "lost gaze: after the gaze returns, the next loss eases from the new gaze");
        dcsvr::GazeFilter g;
        dcsvr::GazeSettings off = on;
        Locate(g, off, t, 20.f);
        Check(!g.Ease(0, t + dt, last, fixed, off, out), "lost gaze: with lostEaseMs 0 the fixed focus shows at once");
    }

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
