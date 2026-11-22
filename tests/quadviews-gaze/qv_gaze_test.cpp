// Offline checks of native/quadviews/dcsvr_gaze.h: the saccade lead and the fixation deadzone of the eye-tracked
// focus area, on simulated gaze at DCS's rendered frame rates with frame generation (45 FPS in 2X, 30 FPS in 3X), and the
// saccade tail and lost-gaze ease (GitHub PR #26), checked against the filter before them when switched off.
// Built and run by scripts/build-quadviews.ps1.
#include <cmath>
#include <cstdio>
#include <random>

#include "dcsvr_gaze.h"
#include "reference/dcsvr_gaze_before_tail.h"

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

    // The saccade tail (tailOn). Without it (`on`) every check above is the filter as before the tail; the reference
    // check further down compares the two filters frame by frame.
    dcsvr::GazeSettings tailOn = on;
    tailOn.tail = true;

    // Saccade tail: a saccade that decelerates before landing (0 -> 12 -> 20 -> 22.5 -> 23.5 degrees at 45 FPS, i.e. 540,
    // 360, 112, 45 deg/s). The 112 deg/s frame is the last one rendered before the landing and reaches the headset as the
    // eye arrives: it stays in the lead's saccade and keeps the farthest reach; a single threshold drops it.
    {
        const int64_t dt = 22'222'222;
        struct Run { bool tailLead, tailPlain, landedLead; Frame peak, tail; };
        auto run = [&](const dcsvr::GazeSettings& s) {
            Run r{};
            dcsvr::GazeFilter f;
            int64_t t = 1'000'000'000;
            Locate(f, s, t, 0.f);
            Locate(f, s, t += dt, 12.f);
            r.peak = Locate(f, s, t += dt, 20.f);
            r.tail = Locate(f, s, t += dt, 22.5f);
            r.tailLead = f.InLeadSaccade();
            r.tailPlain = f.InSaccade();
            Locate(f, s, t += dt, 23.5f);
            r.landedLead = f.InLeadSaccade();
            return r;
        };
        const Run with = run(tailOn), without = run(on);
        Check(with.tailLead && !with.landedLead, "tail: the decelerating frame stays in the saccade, the 45 deg/s landing ends it");
        Check(!with.tailPlain, "tail: the plain saccade (Pupil shift, periphery stability, Far depth) still ends below 120 deg/s");
        Check(!without.tailLead, "tail: with the tail off that frame has left the saccade");
        Check(with.tail.widened && with.tail.max.x >= with.peak.max.x - 1e-5f, "tail: the leading edge does not retreat while the eye decelerates");
        Check(with.tail.max.x > without.tail.max.x + 0.05f, "tail: the frame before the landing reaches farther than without the tail");
    }

    // A saccade that turns back (0 -> 10 -> 20 -> 8 degrees, never slow enough to end): the latch lets go at once and
    // the box reaches left only, never toward the abandoned point on the right.
    {
        dcsvr::GazeFilter f;
        const int64_t dt = 22'222'222;
        int64_t t = 1'000'000'000;
        Locate(f, tailOn, t, 0.f);
        Locate(f, tailOn, t += dt, 10.f);
        Locate(f, tailOn, t += dt, 20.f);
        const Frame back = Locate(f, tailOn, t += dt, 8.f);
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
        Locate(f, tailOn, t, 0.f);
        Locate(f, tailOn, t += dt, 10.f);
        Locate(f, tailOn, t += dt, 20.f);
        for (int i = 0; i < 6; ++i) Locate(f, tailOn, t += dt, 20.f + ((i % 2) ? 0.f : 0.01f));  // ends on exactly 20
        Check(!f.InLeadSaccade(), "second saccade: the first one has ended");
        const Frame small = Locate(f, tailOn, t += dt, 24.f);
        const float lead = std::fmin(tailOn.maxExtend, (Ndc(24.f) - Ndc(20.f)) * tailOn.leadMs / 1000.f / (dt / 1e9f));
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
        Locate(f, tailOn, t, 0.f);
        Locate(f, tailOn, t += dt, 10.f);
        Locate(f, tailOn, t += dt, 20.f);
        int inSaccade = 0;
        for (int i = 0; i < 200; ++i) {
            Locate(f, tailOn, t += dt, 20.f + std::fmax(-0.45f, std::fmin(0.45f, noise(rng))));
            if (i >= 1 && f.InLeadSaccade()) ++inSaccade;
        }
        Check(inSaccade == 0 && f.Stats().saccades == 1, "noise after a landing is below the end speed: the saccade ends");
    }

    // Hold after the landing, without frame generation (90 Hz, 40 ms lead, 0.5 reach). A 20-degree saccade (minimum
    // jerk, 65 ms) whose gaze samples are 40 ms old at their display time. Frames whose eye has already landed are still
    // widened while the samples show the end of the saccade (the tail), and then for the hold. The hold adds nothing:
    // the landed eye is inside the box on every frame with any hold, so the tail uses none (Models.cs
    // QuadSaccadeHoldMs) and the box is back to its own sharpness as soon as the samples show the landing.
    {
        auto run = [&](float holdMs, int& widenedAfterLanding, int& outside) {
            dcsvr::GazeSettings s = tailOn;
            s.leadMs = 40.f;
            s.maxExtend = 0.5f;
            s.holdMs = holdMs;
            dcsvr::GazeFilter f;
            widenedAfterLanding = outside = 0;
            const float amplitude = 20.f, start = 0.2f, duration = (2.2f * amplitude + 21.f) / 1000.f, latency = 0.04f;
            auto eyeAt = [&](float time) {
                const float u = std::fmin(1.f, std::fmax(0.f, (time - start) / duration));
                return amplitude * u * u * u * (10.f - 15.f * u + 6.f * u * u);
            };
            for (int i = 0; i < 60; ++i) {
                const float time = i / 90.f;
                const int64_t t = 1'000'000'000 + static_cast<int64_t>(time * 1e9f);
                const Frame fr = Locate(f, s, t, eyeAt(time - latency) + ((i % 2) ? 0.01f : -0.01f));
                if (time < start + duration) continue;
                if (fr.widened) ++widenedAfterLanding;
                const float centre = (fr.min.x + fr.max.x) * 0.5f, half = (fr.max.x - fr.min.x) * 0.5f;
                if (std::fabs(Ndc(eyeAt(time)) - centre) > 0.8f * half) ++outside;
            }
        };
        int widened0{}, outside0{}, widened55{}, outside55{};
        run(0.f, widened0, outside0);
        run(55.f, widened55, outside55);
        std::printf("     hold 0 ms: %d frames widened after the landing, %d with the eye outside; hold 55 ms: %d, %d\n",
                    widened0, outside0, widened55, outside55);
        Check(outside0 == 0 && outside55 == 0, "hold: the landed eye is inside the box on every frame, with or without a hold");
        Check(widened0 <= 5 && widened55 >= widened0 + 4, "hold: without a hold the box is widened only while the samples lag the landing");
    }

    // Repeated samples: the tracker has nothing new for one frame in the middle of a saccade at 90 Hz and Quad Views
    // hands out the last gaze again. Without the tail that frame reads as 0 deg/s and ends the saccade and its lead.
    {
        dcsvr::GazeFilter f;
        dcsvr::GazeSettings s = tailOn;
        s.leadMs = 25.f;
        const int64_t dt = 11'111'111;
        int64_t t = 1'000'000'000;
        Locate(f, s, t, 0.f);
        Locate(f, s, t += dt, 4.f);
        const Frame before = Locate(f, s, t += dt, 9.f);
        const Frame repeat = Locate(f, s, t += dt, 9.f);  // the same sample again
        Check(f.InLeadSaccade() && repeat.widened && repeat.max.x >= before.max.x - 1e-5f && f.Stats().repeated == 1,
              "repeat: a repeated sample keeps the saccade and its reach");
        Check(!f.InSaccade(), "repeat: the plain saccade reads the repeat as before (0 deg/s)");
        Locate(f, s, t += dt, 18.f);
        Check(f.InLeadSaccade() && f.LeadSpeed() > 300.f && f.LeadSpeed() < 500.f,
              "repeat: the next real sample measures the speed over both frames (9 deg in 22 ms)");
        // The tracker loses the eyes and Quad Views repeats the last gaze for up to 600 ms: the saccade ends after 100 ms.
        for (int i = 0; i < 10; ++i) Locate(f, s, t += dt, 18.f);
        Check(!f.InLeadSaccade(), "repeat: a repeat lasting 100 ms ends the saccade");
        Check(f.Stats().eyeFrames == 2 * 15, "repeat: repeated frames still count as frames");
    }

    // After a long run of exact repeats (100, 150 and 200 ms: a blink, a stalled tracker) the first frame of a saccade
    // (2 degrees in one 90 Hz frame, 180 deg/s) still starts one, as it does without repeats.
    for (const int repeats : {9, 14, 18}) {
        dcsvr::GazeFilter f;
        const int64_t dt = 11'111'111;
        int64_t t = 1'000'000'000;
        for (int i = 0; i < 4; ++i) Locate(f, tailOn, t += dt, (i % 2) ? 0.01f : -0.01f);
        for (int i = 0; i < repeats; ++i) Locate(f, tailOn, t += dt, -0.01f);
        Locate(f, tailOn, t += dt, 2.f);
        char what[128];
        std::snprintf(what, sizeof(what), "repeat: the first saccade frame after %d ms of repeats starts a saccade", repeats * 11 + 1);
        Check(f.InLeadSaccade() && f.Stats().saccades == 1, what);
    }

    // Landing check: a 30-degree saccade over 3 frames at 90 Hz with one frame of lead (25 ms). Without the lead the box
    // on screen as the eye lands is still centred on the gaze of 25 ms before: a miss. With it, it reaches the landing.
    {
        auto landing = [&](bool widening, uint64_t& landings, uint64_t& missed) {
            dcsvr::GazeFilter f;
            dcsvr::GazeSettings s = tailOn;
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
        dcsvr::GazeSettings s = tailOn;
        s.lostEaseMs = 150.f;
        const int64_t dt = 11'111'111;
        int64_t t = 1'000'000'000;
        Locate(f, s, t, 20.f);
        const dcsvr::GazeVec2 last{Ndc(20.f), 0.f}, fixed{-0.05f, 0.f};
        dcsvr::GazeVec2 out{};
        bool monotonic = true;
        float previous = last.x;
        int frames = 0;
        for (t += dt; f.Ease(0, t, last, fixed, s, out); t += dt, ++frames) {
            monotonic = monotonic && out.x <= previous + 1e-6f && out.x >= fixed.x - 1e-6f;
            previous = out.x;
        }
        Check(frames >= 12 && frames <= 14 && monotonic, "lost gaze: the focus eases to the fixed one over 150 ms (13 frames at 90 Hz)");
        Check(f.Stats().eased == 1, "lost gaze: counted once");
        {
            // The box's size eases with its centre: tracked 0.35 to fixed 0.5 (Quad Views' defaults). The last eased
            // frame is within 5 % of the fixed size, so the hand-over to the fixed focus is no step in size.
            dcsvr::GazeFilter p;
            Locate(p, s, 1'000'000'000, 20.f);
            float progress = -1.f, lastProgress = 0.f, lastSection = 0.f;
            bool rising = true;
            for (int64_t tp = 1'000'000'000 + dt; p.Ease(0, tp, last, fixed, s, out, &progress); tp += dt) {
                const float section = dcsvr::GazeFilter::EaseSection(0.35f, 0.5f, progress);
                rising = rising && progress >= lastProgress && section >= lastSection;
                lastProgress = progress;
                lastSection = section;
            }
            Check(rising && lastProgress > 0.95f && std::fabs(lastSection - 0.5f) < 0.025f &&
                      dcsvr::GazeFilter::EaseSection(0.35f, 0.5f, 0.f) == 0.35f && dcsvr::GazeFilter::EaseSection(0.35f, 0.5f, 1.f) == 0.5f,
                  "lost gaze: the focus size eases to the fixed focus's with the centre (no size step at the hand-over)");
        }
        Locate(f, s, t += dt, 5.f);
        Check(f.Ease(0, t += dt, {Ndc(5.f), 0.f}, fixed, s, out) && std::fabs(out.x - Ndc(5.f)) < 1e-4f,
              "lost gaze: after the gaze returns, the next loss eases from the new gaze");
        dcsvr::GazeFilter g;
        Locate(g, on, t, 20.f);
        Check(!g.Ease(0, t + dt, last, fixed, on, out), "lost gaze: with lostEaseMs 0 the fixed focus shows at once");
        // With frame generation the focus is placed once per rendered frame: in 3X (30 FPS) the same 150 ms are 4-5 steps,
        // still a slide (the generated frames in between blend two of them), never a jump.
        dcsvr::GazeFilter h;
        const int64_t dt3 = 33'333'333;
        int64_t t3 = 1'000'000'000;
        Locate(h, s, t3, 20.f);
        int steps = 0;
        monotonic = true;
        previous = last.x;
        for (t3 += dt3; h.Ease(0, t3, last, fixed, s, out); t3 += dt3, ++steps) {
            monotonic = monotonic && out.x <= previous + 1e-6f && out.x >= fixed.x - 1e-6f;
            previous = out.x;
        }
        Check(steps >= 4 && steps <= 5 && monotonic, "lost gaze, 3X: the focus eases to the fixed one over 150 ms (4-5 frames at 30 FPS)");
    }

    // The saccade tail with frame generation (Models.cs: UsesQuadSaccadeTail, QuadSaccadeLeadMs/HoldMs/MaxExtend). DCS
    // places the focus once per rendered frame (45 FPS in 2X, 30 FPS in 3X at 90 Hz) and every box is shown on 2 or 3
    // refreshes; the gaze is 55 / 85 ms old (the frame generation leads) at the first of them (A) or at the last (B).
    // Minimum-jerk saccades of 10-40 degrees, both ways, starting at 8 phases of the rendered frame. Counted on every
    // refresh: the eye outside the box's sharp part (inside the fade band, 0.2 of the half size), from the first widened
    // box on (before it no filter can know: a saccade shorter than the latency has landed before its first sample), and
    // the widened rendered frames. Today's frame generation values (no tail, 0.35, hold 40) against the tail with the
    // same lead, reach 0.5 and no hold: the tail must never put the eye outside more often and must widen fewer
    // frames. The misses left are the latency's, the same either way (2X: a few 20-30 degree saccades at some phases).
    {
        struct Totals { int outside = 0, outsideSmall = 0, widened = 0; };
        const auto sweep = [&](int factor, bool tail, float maxExtend, float holdMs) {
            Totals totals;
            const float lead = factor == 2 ? 55.f : 85.f, latency = lead / 1000.f, refresh = 1.f / 90.f;
            for (const int model : {0, 1})
                for (const float amplitude : {10.f, 15.f, 20.f, 30.f, 40.f})
                    for (const float sign : {1.f, -1.f})
                        for (int phase = 0; phase < 8; ++phase) {
                            dcsvr::GazeSettings s = on;
                            s.tail = tail;
                            s.leadMs = lead;
                            s.maxExtend = maxExtend;
                            s.holdMs = holdMs;
                            dcsvr::GazeFilter f;
                            const float start = 0.2f + phase * factor * refresh / 8.f, duration = (2.2f * amplitude + 21.f) / 1000.f;
                            const auto eyeAt = [&](float time) {
                                const float u = std::fmin(1.f, std::fmax(0.f, (time - start) / duration));
                                return sign * amplitude * u * u * u * (10.f - 15.f * u + 6.f * u * u);
                            };
                            bool seen = false;
                            for (int i = 0; i < 40; ++i) {
                                const float time = i * factor * refresh;
                                const int64_t t = 1'000'000'000 + static_cast<int64_t>(time * 1e9f);
                                const Frame fr = Locate(f, s, t, eyeAt(time - latency) + ((i % 2) ? 0.01f : -0.01f));
                                if (fr.widened) {
                                    seen = true;
                                    ++totals.widened;
                                }
                                for (int k = 0; k < factor; ++k) {
                                    const float shown = time + (k - (model ? factor - 1 : 0)) * refresh;
                                    const float eye = Ndc(eyeAt(shown));
                                    const bool outside = eye < fr.min.x + 0.2f * 0.34f || eye > fr.max.x - 0.2f * 0.34f;
                                    if (seen && outside) {
                                        ++totals.outside;
                                        if (amplitude <= 30.f) ++totals.outsideSmall;
                                    }
                                }
                            }
                        }
            return totals;
        };
        for (const int factor : {2, 3}) {
            const Totals today = sweep(factor, false, 0.35f, 40.f), tail = sweep(factor, true, 0.5f, 0.f);
            std::printf("     %dX: eye outside after detection %d (up to 30 deg %d), widened frames %d; with the tail %d (%d), %d\n",
                        factor, today.outside, today.outsideSmall, today.widened, tail.outside, tail.outsideSmall, tail.widened);
            Check(tail.outside <= today.outside && tail.outsideSmall <= today.outsideSmall,
                  factor == 2 ? "frame generation 2X: with the tail the eye is never outside the sharp part more often"
                              : "frame generation 3X: with the tail the eye is never outside the sharp part more often");
            Check(tail.widened < today.widened,
                  factor == 2 ? "frame generation 2X: the tail widens fewer rendered frames (no hold)"
                              : "frame generation 3X: the tail widens fewer rendered frames (no hold)");
        }
    }

    // Switched off, exactly the filter before the tail: the same random gaze (saccades of every size, fixation noise,
    // exact repeats, near-duplicate display times, gaps, 72 and 90 Hz) through this filter with the tail and the
    // lost-gaze ease off and through the filter as it was (tests/quadviews-gaze/reference). Every returned gaze, box,
    // widened flag, saccade state and statistic is the same.
    {
        bool same = true, plain = true;
        int frames = 0;
        for (const float hz : {72.f, 90.f}) {
            for (const float lead : {25.f, 55.f, 85.f}) {
                dcsvr::GazeSettings s = on;
                s.leadMs = lead;
                dcsvr_before::GazeSettings b;
                b.widening = s.widening; b.speed = s.speed; b.leadMs = s.leadMs; b.maxExtend = s.maxExtend;
                b.holdMs = s.holdMs; b.deadzoneDeg = s.deadzoneDeg;
                dcsvr::GazeFilter f, withTail;
                dcsvr_before::GazeFilter r;
                dcsvr::GazeSettings st = s;
                st.tail = true;
                std::mt19937 rng(static_cast<unsigned>(hz + lead));
                std::uniform_real_distribution<float> uni(0.f, 1.f);
                std::normal_distribution<float> noise(0.f, 0.2f);
                int64_t t = 1'000'000'000;
                float deg = 0.f, target = 0.f, x{}, y{}, z{};
                for (int i = 0; i < 4000; ++i) {
                    const float pick = uni(rng);
                    t += pick < 0.01f ? 300'000'000 : pick < 0.03f ? 2'000 : static_cast<int64_t>(1e9f / hz);
                    if (uni(rng) < 0.03f) target = (uni(rng) - 0.5f) * 60.f;
                    const bool repeat = uni(rng) < 0.15f;
                    if (!repeat) {
                        deg += std::fmax(-12.f, std::fmin(12.f, target - deg)) + noise(rng);
                        Unit(deg, x, y, z);
                    }
                    f.UpdateSpeed(t, x, y, z, s);
                    r.UpdateSpeed(t, x, y, z, b);
                    withTail.UpdateSpeed(t, x, y, z, st);
                    plain = plain && withTail.InSaccade() == r.InSaccade();
                    same = same && f.InSaccade() == r.InSaccade() && f.InLeadSaccade() == r.InSaccade() && f.Speed() == r.Speed();
                    for (int eye = 0; eye < 2; ++eye) {
                        const dcsvr::GazeVec2 g = f.Eye(eye, t, {Ndc(deg), 0.f}, NdcPerDeg, NdcPerDeg, s);
                        const dcsvr_before::GazeVec2 h = r.Eye(eye, t, {Ndc(deg), 0.f}, NdcPerDeg, NdcPerDeg, b);
                        dcsvr::GazeVec2 lo{g.x - 0.34f, -0.34f}, hi{g.x + 0.34f, 0.34f};
                        dcsvr_before::GazeVec2 rlo{h.x - 0.34f, -0.34f}, rhi{h.x + 0.34f, 0.34f};
                        const bool w = f.Extend(eye, t, lo, hi, s), rw = r.Extend(eye, t, rlo, rhi, b);
                        f.Record(eye, t, lo, hi);
                        dcsvr::GazeVec2 eased{};
                        same = same && !f.Ease(eye, t, lo, hi, s, eased);
                        same = same && g.x == h.x && g.y == h.y && w == rw && lo.x == rlo.x && hi.x == rhi.x && lo.y == rlo.y && hi.y == rhi.y;
                    }
                    ++frames;
                }
                const auto& a = f.Stats();
                const auto& c = r.Stats();
                same = same && a.eyeFrames == c.eyeFrames && a.widened == c.widened && a.held == c.held &&
                       a.saccades == c.saccades && a.peakSpeed == c.peakSpeed && a.repeated == 0 && a.eased == 0;
            }
        }
        std::printf("     %d frames compared\n", frames);
        Check(same && frames == 24000, "off: the tail and the lost-gaze ease switched off give exactly the filter before them");
        Check(plain, "tail on: the plain saccade (InSaccade) is still the one before the tail on every frame");
    }
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
