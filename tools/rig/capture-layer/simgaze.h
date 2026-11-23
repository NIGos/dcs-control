// Simulated eye gaze for unattended rig flights (TEST-ONLY, part of the rig capture layer, rigtap.cpp).
// Pure model: a program (text) -> the gaze at a time. No OpenXR, no Windows; tested by simgaze_harness.cpp.
//
// The gaze is a direction in the head (VIEW) frame: x = degrees to the right, y = degrees up, (0, 0) straight ahead.
// Program text: key=value pairs separated by newlines or ';' ('#' starts a comment line). Unknown keys are an error.
//   mode=fixed      x, y
//   mode=targets    targets=x,y|x,y|...  dwell_ms (default 600)  loop (default 1)
//                   Fixates targets[0], then jumps to each next target with a main-sequence saccade (duration
//                   21 ms + 2.2 ms/deg, minimum-jerk profile: peak speed 1.875 x mean, about 440 deg/s for 10 deg), dwells.
//                   move_ms > 0: every move lasts this long instead (same profile; for head turns, e.g. 30 deg in 300 ms).
//   mode=sweep      amp_x, amp_y (deg; negative = opposite phase, e.g. eyes countering a head sweep), period_ms (default 4000), phase_deg (of y against x, default 90), x, y = centre:
//                   smooth pursuit (sinusoid). The peak speed is logged; pursuit above ~30 deg/s is not human.
//   mode=replay     file=<csv: t_s,x_deg,y_deg[,valid]> loop (default 0): linear interpolation, invalid where either
//                   neighbour is invalid; past the end the last sample holds (or it loops).
//   mode=lost       never valid (nobody wearing the headset / tracker lost the eyes)
//   mode=off        not simulated: the runtime's own answer is passed through
// Any mode except off/lost:
//   blink_every_ms, blink_ms (default 150), blink_first_ms (default blink_every_ms): invalid while blinking
//   noise_deg: fixational drift (smooth value noise at 30 Hz knots, about this standard deviation per axis), seed
//   latency_ms: the sample time handed out (XrEyeGazeSampleTimeEXT) is this much before the located time
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace simgaze {

enum class Mode { off, fixed, targets, sweep, replay, lost };

struct Point { float x = 0, y = 0; };
struct ReplaySample { double t = 0; float x = 0, y = 0; bool valid = true; };

struct Program {
    Mode mode = Mode::fixed;
    Point at{};                         // fixed gaze, sweep centre
    std::vector<Point> targets;
    double dwell_ms = 600;
    double move_ms = 0;                 // > 0: fixed move duration instead of the main sequence
    bool loop = true;
    bool loop_set = false;
    float amp_x = 0, amp_y = 0;
    double period_ms = 4000, phase_deg = 90;
    std::string file;
    std::vector<ReplaySample> replay;
    double blink_every_ms = 0, blink_ms = 150, blink_first_ms = -1;
    float noise_deg = 0;
    std::uint32_t seed = 1;
    double latency_ms = 0;
    std::string text;                   // as given (for the log)
};

struct Sample {
    bool simulated = false;             // false: mode off, pass the runtime's answer through
    bool valid = false;
    bool saccade = false;
    float x = 0, y = 0;                 // degrees, head frame
};

inline const char* mode_name(Mode m) {
    switch (m) {
    case Mode::off: return "off";
    case Mode::fixed: return "fixed";
    case Mode::targets: return "targets";
    case Mode::sweep: return "sweep";
    case Mode::replay: return "replay";
    case Mode::lost: return "lost";
    }
    return "?";
}

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

inline bool number(const std::string& s, double& out) {
    const std::string t = trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end != t.c_str() + t.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// Reads a replay CSV: t_s,x_deg,y_deg[,valid]; lines that do not start with a number (a header, '#') are skipped.
inline bool load_replay(const std::string& path, std::vector<ReplaySample>& out, std::string& error) {
    out.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { error = "replay file not readable: " + path; return false; }
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string l = trim(line);
        if (l.empty() || l[0] == '#') continue;
        double v[4] = {0, 0, 0, 1};
        int n = 0;
        size_t a = 0;
        bool ok = true;
        while (a <= l.size() && n < 4) {
            size_t z = l.find(',', a);
            if (z == std::string::npos) z = l.size();
            if (!number(l.substr(a, z - a), v[n])) { ok = false; break; }
            ++n;
            a = z + 1;
        }
        if (!ok || n < 3) continue;
        if (!out.empty() && v[0] <= out.back().t) { std::fclose(f); error = "replay times must increase"; return false; }
        out.push_back({v[0], static_cast<float>(v[1]), static_cast<float>(v[2]), v[3] != 0});
    }
    std::fclose(f);
    if (out.empty()) { error = "replay file has no samples: " + path; return false; }
    return true;
}

// Parses a program. On error returns false with a message; `out` is then unchanged.
inline bool parse(const std::string& text, Program& out, std::string& error) {
    Program p;
    for (char c : text) { if (c == '\r') continue; p.text += c == '\n' ? ';' : c; }  // one line, for the log
    std::vector<std::string> items;
    {
        std::string cur;
        for (char c : text) {
            if (c == '\n' || c == ';') { items.push_back(cur); cur.clear(); } else cur += c;
        }
        items.push_back(cur);
    }
    bool have_mode = false;
    for (const std::string& raw : items) {
        const std::string item = trim(raw);
        if (item.empty() || item[0] == '#') continue;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) { error = "not key=value: " + item; return false; }
        const std::string k = trim(item.substr(0, eq)), v = trim(item.substr(eq + 1));
        double d = 0;
        auto num = [&](double& dst, double lo, double hi) {
            if (!number(v, d) || d < lo || d > hi) { error = k + ": number out of range: " + v; return false; }
            dst = d;
            return true;
        };
        auto numf = [&](float& dst, double lo, double hi) { double t = 0; if (!num(t, lo, hi)) return false; dst = static_cast<float>(t); return true; };
        if (k == "mode") {
            have_mode = true;
            if (v == "off") p.mode = Mode::off;
            else if (v == "fixed") p.mode = Mode::fixed;
            else if (v == "targets" || v == "saccades") p.mode = Mode::targets;
            else if (v == "sweep") p.mode = Mode::sweep;
            else if (v == "replay") p.mode = Mode::replay;
            else if (v == "lost") p.mode = Mode::lost;
            else { error = "unknown mode: " + v; return false; }
        } else if (k == "x") { if (!numf(p.at.x, -60, 60)) return false; }
        else if (k == "y") { if (!numf(p.at.y, -60, 60)) return false; }
        else if (k == "targets") {
            p.targets.clear();
            size_t a = 0;
            while (a <= v.size()) {
                size_t z = v.find('|', a);
                if (z == std::string::npos) z = v.size();
                const std::string pair = v.substr(a, z - a);
                const size_t c = pair.find(',');
                double tx = 0, ty = 0;
                if (c == std::string::npos || !number(pair.substr(0, c), tx) || !number(pair.substr(c + 1), ty) ||
                    std::fabs(tx) > 60 || std::fabs(ty) > 60) { error = "targets: not x,y|x,y|... within 60 deg: " + v; return false; }
                p.targets.push_back({static_cast<float>(tx), static_cast<float>(ty)});
                a = z + 1;
            }
        } else if (k == "dwell_ms") { if (!num(p.dwell_ms, 0, 600000)) return false; }
        else if (k == "move_ms") { if (!num(p.move_ms, 0, 60000)) return false; }
        else if (k == "loop") { double t = 0; if (!num(t, 0, 1)) return false; p.loop = t != 0; p.loop_set = true; }
        else if (k == "amp_x") { if (!numf(p.amp_x, -60, 60)) return false; }
        else if (k == "amp_y") { if (!numf(p.amp_y, -60, 60)) return false; }
        else if (k == "period_ms") { if (!num(p.period_ms, 50, 600000)) return false; }
        else if (k == "phase_deg") { if (!num(p.phase_deg, -360, 360)) return false; }
        else if (k == "file") p.file = v;
        else if (k == "blink_every_ms") { if (!num(p.blink_every_ms, 0, 600000)) return false; }
        else if (k == "blink_ms") { if (!num(p.blink_ms, 0, 10000)) return false; }
        else if (k == "blink_first_ms") { if (!num(p.blink_first_ms, 0, 600000)) return false; }
        else if (k == "noise_deg") { if (!numf(p.noise_deg, 0, 5)) return false; }
        else if (k == "seed") { double t = 0; if (!num(t, 0, 4294967295.0)) return false; p.seed = static_cast<std::uint32_t>(t); }
        else if (k == "latency_ms") { if (!num(p.latency_ms, 0, 500)) return false; }
        else { error = "unknown key: " + k; return false; }
    }
    if (!have_mode) { error = "mode= missing"; return false; }
    if (p.mode == Mode::targets && p.targets.empty()) { error = "mode=targets needs targets="; return false; }
    if (p.mode == Mode::replay) {
        if (p.file.empty()) { error = "mode=replay needs file="; return false; }
        if (!p.loop_set) p.loop = false;
        if (!load_replay(p.file, p.replay, error)) return false;
    }
    if (p.blink_first_ms < 0) p.blink_first_ms = p.blink_every_ms;
    out = std::move(p);
    return true;
}

// Main-sequence saccade duration (seconds) for an amplitude in degrees.
inline double saccade_duration_s(double amplitude_deg) { return 0.021 + 0.0022 * amplitude_deg; }
// The duration of a move between targets: the main sequence, or the program's fixed move_ms.
inline double move_duration_s(const Program& p, double amplitude_deg) { return p.move_ms > 0 ? p.move_ms / 1000.0 : saccade_duration_s(amplitude_deg); }
// Minimum-jerk position fraction for a time fraction u in [0, 1].
inline double min_jerk(double u) { u = std::min(1.0, std::max(0.0, u)); return u * u * u * (10 - 15 * u + 6 * u * u); }

inline double hash01(std::uint32_t seed, std::int64_t i, std::uint32_t axis) {
    std::uint64_t h = (static_cast<std::uint64_t>(seed) * 0x9E3779B97F4A7C15ull) ^ (static_cast<std::uint64_t>(i) * 0xBF58476D1CE4E5B9ull) ^ (axis * 0x94D049BB133111EBull);
    h ^= h >> 31; h *= 0xD6E8FEB86659FD93ull; h ^= h >> 32;
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}
// Smooth value noise with about unit standard deviation (sum of 3 uniforms, cosine-interpolated between 30 Hz knots).
inline double drift(std::uint32_t seed, std::uint32_t axis, double t) {
    const double k = t * 30.0;
    const std::int64_t i = static_cast<std::int64_t>(std::floor(k));
    const double f = k - static_cast<double>(i), w = 0.5 - 0.5 * std::cos(f * 3.14159265358979);
    auto knot = [&](std::int64_t j) { return (hash01(seed, j, axis) + hash01(seed + 1, j, axis) + hash01(seed + 2, j, axis) - 1.5) * 2.0; };
    return knot(i) * (1 - w) + knot(i + 1) * w;
}

// The gaze `t` seconds after the program started.
inline Sample evaluate(const Program& p, double t) {
    Sample s;
    if (p.mode == Mode::off) return s;
    s.simulated = true;
    if (p.mode == Mode::lost) return s;
    if (t < 0) t = 0;
    s.valid = true;
    switch (p.mode) {
    case Mode::fixed: s.x = p.at.x; s.y = p.at.y; break;
    case Mode::sweep: {
        const double w = 2 * 3.14159265358979 * t / (p.period_ms / 1000.0);
        s.x = p.at.x + p.amp_x * static_cast<float>(std::sin(w));
        s.y = p.at.y + p.amp_y * static_cast<float>(std::sin(w + p.phase_deg * 3.14159265358979 / 180));
        break;
    }
    case Mode::targets: {
        const size_t n = p.targets.size();
        const double dwell = p.dwell_ms / 1000.0;
        // Segment 0: dwell on targets[0]. Segment k >= 1: saccade targets[k-1] -> targets[k], then dwell.
        auto seg_len = [&](size_t k) {
            if (k == 0) return dwell;
            const Point& a = p.targets[(k - 1) % n]; const Point& b = p.targets[k % n];
            return move_duration_s(p, std::hypot(b.x - a.x, b.y - a.y)) + dwell;
        };
        double rest = t;
        if (p.loop && n > 1) {
            double cycle = 0;  // segments 1..n (back to targets[0])
            for (size_t k = 1; k <= n; ++k) cycle += seg_len(k);
            if (rest >= seg_len(0) && cycle > 0) rest = seg_len(0) + std::fmod(rest - seg_len(0), cycle);
        }
        size_t k = 0;
        const size_t last = p.loop && n > 1 ? n : n - 1;
        while (k < last && rest >= seg_len(k)) { rest -= seg_len(k); ++k; }
        if (k == 0 || (k == last && rest >= seg_len(k))) { const Point& q = p.targets[k % n]; s.x = q.x; s.y = q.y; break; }
        const Point& a = p.targets[(k - 1) % n]; const Point& b = p.targets[k % n];
        const double d = move_duration_s(p, std::hypot(b.x - a.x, b.y - a.y));
        if (rest < d) {
            const double f = min_jerk(rest / d);
            s.x = static_cast<float>(a.x + (b.x - a.x) * f); s.y = static_cast<float>(a.y + (b.y - a.y) * f);
            s.saccade = true;
        } else { s.x = b.x; s.y = b.y; }
        break;
    }
    case Mode::replay: {
        const auto& r = p.replay;
        double u = t;
        const double span = r.back().t - r.front().t;
        if (p.loop && span > 0) u = r.front().t + std::fmod(std::max(0.0, u - r.front().t), span);
        if (u <= r.front().t) { s.x = r.front().x; s.y = r.front().y; s.valid = r.front().valid; break; }
        if (u >= r.back().t) { s.x = r.back().x; s.y = r.back().y; s.valid = r.back().valid; break; }
        const auto it = std::upper_bound(r.begin(), r.end(), u, [](double v, const ReplaySample& e) { return v < e.t; });
        const ReplaySample& b = *it; const ReplaySample& a = *(it - 1);
        const double f = (u - a.t) / (b.t - a.t);
        s.x = static_cast<float>(a.x + (b.x - a.x) * f); s.y = static_cast<float>(a.y + (b.y - a.y) * f);
        s.valid = a.valid && b.valid;
        break;
    }
    default: break;
    }
    if (p.noise_deg > 0 && !s.saccade) {
        s.x += p.noise_deg * static_cast<float>(drift(p.seed, 0, t));
        s.y += p.noise_deg * static_cast<float>(drift(p.seed, 1, t));
    }
    if (p.blink_every_ms > 0 && p.blink_ms > 0) {
        const double ms = t * 1000.0 - p.blink_first_ms;
        if (ms >= 0 && std::fmod(ms, p.blink_every_ms) < p.blink_ms) s.valid = false;
    }
    return s;
}

// The gaze direction in the head frame (OpenXR VIEW space: -Z forward, +X right, +Y up) as a unit vector, and as the
// orientation of a pose whose -Z axis points along it (yaw about +Y, then pitch about the yawed +X; no roll).
struct Quat { float x = 0, y = 0, z = 0, w = 1; };
inline Quat gaze_orientation(float x_deg, float y_deg) {
    const double yaw = -x_deg * 3.14159265358979 / 180 / 2, pitch = y_deg * 3.14159265358979 / 180 / 2;
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    // q = qy * qp, qy = (0, sy, 0, cy), qp = (sp, 0, 0, cp)
    return {static_cast<float>(cy * sp), static_cast<float>(sy * cp), static_cast<float>(-sy * sp), static_cast<float>(cy * cp)};
}
inline Quat multiply(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline void rotate(const Quat& q, const float v[3], float out[3]) {
    // v' = v + 2w(u x v) + 2 u x (u x v)
    const float ux = q.x, uy = q.y, uz = q.z;
    const float cx = uy * v[2] - uz * v[1], cy = uz * v[0] - ux * v[2], cz = ux * v[1] - uy * v[0];
    const float dx = uy * cz - uz * cy, dy = uz * cx - ux * cz, dz = ux * cy - uy * cx;
    out[0] = v[0] + 2 * (q.w * cx + dx); out[1] = v[1] + 2 * (q.w * cy + dy); out[2] = v[2] + 2 * (q.w * cz + dz);
}

}  // namespace simgaze
