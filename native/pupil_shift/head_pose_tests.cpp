// Offline tests of the head pose feed for binaural audio: the layer's writer (head_pose_publisher.hpp) and the audio
// module's reader (native/binaural/include/head_pose_reader.h) over Local\DcsVrHeadPose_v1's layout (head_pose_shm.h),
// in process memory. The end-to-end checks through the layer and a fake runtime are in pupil_shift_harness.cpp.
//   - Publisher: once per display time, display-time order, refused poses, normalisation, the eye-view fallback, the
//     velocity derived when the runtime gives none;
//   - seqlock: two writers (the layer's lock) and three readers hammering a 16-entry ring: no accepted copy is torn,
//     entry numbers and display times only go forward; the raw contract writer lapping readers the same;
//   - reader: not ready / other writer / empty, staleness, extrapolation by angular and linear velocity, its horizon and
//     speed limits, the late correction.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "head_pose_publisher.hpp"
#include "head_pose_reader.h"

namespace hp = dcsvr_headpose;

static int failures = 0;
static int checks = 0;
static void check(bool ok, const char* what, double a = 0, double b = 0) {
    ++checks;
    if (!ok && ++failures <= 40) std::printf("FAIL %s (%.9g vs %.9g)\n", what, a, b);
}

constexpr double kPi = 3.14159265358979323846;
constexpr std::int64_t kFreq = 10'000'000;  // QPC ticks per second in these tests

static hp::Quat yaw(double r) { return {0, std::sin(r / 2), 0, std::cos(r / 2)}; }
static double angle_between(const hp::Quat& a, const hp::Quat& b) {
    // atan2 of the relative rotation (acos near 1 loses half the digits).
    const hp::Quat r = hp::Mul(hp::Conj(a), b);
    return 2.0 * std::atan2(std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z), std::fabs(r.w));
}
static double deg(double r) { return r * 180.0 / kPi; }

// The truth of a display time for the stress tests: every field a function of t that changes by >= 1e-3 per step, so a
// copy mixing two entries never matches either.
struct Truth {
    float q[4], p[3], w[3], v[3];
    std::int64_t qpc;
};
static Truth truth(std::int64_t t) {
    Truth r{};
    const double s = static_cast<double>(t) * 1e-3;  // t counts 1..N
    const hp::Quat q = hp::Normalize(hp::Mul(yaw(10.0 * s), hp::Quat{std::sin(3.0 * s), 0, 0, std::cos(3.0 * s)}));
    r.q[0] = static_cast<float>(q.x); r.q[1] = static_cast<float>(q.y); r.q[2] = static_cast<float>(q.z); r.q[3] = static_cast<float>(q.w);
    head_pose::normalize_orientation(r.q);  // the publisher's normalisation (idempotent here)
    r.p[0] = static_cast<float>(std::fmod(s, 7.0)); r.p[1] = static_cast<float>(1.0 + std::fmod(2 * s, 5.0)); r.p[2] = static_cast<float>(-std::fmod(3 * s, 11.0));
    r.w[0] = static_cast<float>(std::fmod(s, 13.0)); r.w[1] = static_cast<float>(t % 1000) * 0.002f; r.w[2] = -r.w[0];
    r.v[0] = static_cast<float>(std::fmod(5 * s, 3.0)); r.v[1] = -r.v[0]; r.v[2] = static_cast<float>(t % 777) * 0.003f;
    r.qpc = t * 37;
    return r;
}
static head_pose::Sample sample_of(const Truth& tr) {
    head_pose::Sample s;
    std::copy(tr.q, tr.q + 4, s.orientation);
    std::copy(tr.p, tr.p + 3, s.position);
    std::copy(tr.w, tr.w + 3, s.angular_velocity);
    std::copy(tr.v, tr.v + 3, s.linear_velocity);
    namespace ef = hp::entry_flags;
    s.flags = ef::orientation_valid | ef::position_valid | ef::orientation_tracked | ef::position_tracked | ef::angular_velocity_valid | ef::linear_velocity_valid;
    return s;
}
static bool consistent(const hp::Entry& e) {
    const Truth tr = truth(e.displayTime);
    bool ok = e.qpcLocate == tr.qpc && e.qpcDisplay == tr.qpc + 5 && e.baseSpace == 0x77 && (e.seq & 1u) == 0;
    for (int i = 0; i < 4; ++i) ok = ok && std::fabs(e.orientation[i] - tr.q[i]) < 1e-6f;  // normalised again: 1 ulp
    for (int i = 0; i < 3; ++i) ok = ok && e.position[i] == tr.p[i] && e.angularVelocity[i] == tr.w[i] && e.linearVelocity[i] == tr.v[i];
    const double n = std::sqrt(static_cast<double>(e.orientation[0]) * e.orientation[0] + static_cast<double>(e.orientation[1]) * e.orientation[1] +
                               static_cast<double>(e.orientation[2]) * e.orientation[2] + static_cast<double>(e.orientation[3]) * e.orientation[3]);
    return ok && std::fabs(n - 1.0) < 1e-5;
}

static std::unique_ptr<hp::Block> new_block() {
    auto b = std::make_unique<hp::Block>();
    std::memset(b.get(), 0, sizeof(hp::Block));
    hp::InitWriter(b.get(), kFreq, 4242, hp::writer_flags::qpc_display);
    return b;
}

// ---- Publisher ----------------------------------------------------------------------------------------------------
static void publisher_tests() {
    namespace ef = hp::entry_flags;
    head_pose::Publisher pub;
    check(pub.claim(100) && !pub.claim(100) && !pub.claim(99) && pub.claim(101), "claim: once per display time, never older");
    check(!pub.claim(0) && !pub.claim(-5), "claim: no zero or negative time");
    pub.reset();
    check(pub.claim(50), "claim: a new instance starts over");

    auto b = new_block();
    head_pose::Sample s;
    s.flags = ef::orientation_valid | ef::position_valid;
    s.orientation[0] = 0; s.orientation[1] = 0; s.orientation[2] = 0; s.orientation[3] = 2.0f;  // not unit: normalised
    s.position[0] = 1; s.position[1] = 2; s.position[2] = 3;
    check(pub.publish(b.get(), 1000, 10, 20, 0x77, s), "publish: first entry");
    hp::Entry e{};
    check(hp::ReadLatest(b.get(), e) && e.index == 1 && e.displayTime == 1000 && e.orientation[3] == 1.0f && e.frame == 1, "publish: entry 1, normalised",
          static_cast<double>(e.index), e.orientation[3]);
    check(!pub.publish(b.get(), 1000, 11, 21, 0x77, s) && !pub.publish(b.get(), 999, 11, 21, 0x77, s), "publish: an equal or older display time is refused");
    head_pose::Sample bad = s;
    bad.orientation[3] = std::nanf("");
    check(!pub.publish(b.get(), 2000, 11, 21, 0x77, bad), "publish: NaN orientation refused");
    bad = s; bad.orientation[0] = bad.orientation[1] = bad.orientation[2] = 0; bad.orientation[3] = 0.1f;
    check(!pub.publish(b.get(), 2000, 11, 21, 0x77, bad), "publish: a near-zero quaternion is refused");
    bad = s; bad.flags = 0;
    check(!pub.publish(b.get(), 2000, 11, 21, 0x77, bad), "publish: no orientation, nothing published");
    check(!pub.publish(nullptr, 2000, 11, 21, 0x77, s), "publish: no block");
    bad = s; bad.position[1] = INFINITY;
    check(pub.publish(b.get(), 3000, 11, 21, 0x77, bad), "publish: a non-finite position is dropped, the rotation kept");
    check(hp::ReadLatest(b.get(), e) && e.index == 2 && !(e.flags & ef::position_valid) && e.position[0] == 0 && e.position[1] == 0,
          "publish: position flag and value cleared", static_cast<double>(e.flags), e.position[1]);
    check(b->writeCount == 2 && pub.frames() == 2, "publish: refused poses use no entry", static_cast<double>(b->writeCount), pub.frames());

    // from_views: views canted 3 deg outwards about a head turned 30 deg, eyes 64 mm apart; the head is between them.
    {
        const hp::Quat head = yaw(30 * kPi / 180);
        const hp::Quat ql = hp::Mul(head, yaw(3 * kPi / 180)), qr = hp::Mul(head, yaw(-3 * kPi / 180));
        const hp::Vec3 right = hp::Rotate(head, {1, 0, 0});
        const float fl[4]{float(ql.x), float(ql.y), float(ql.z), float(ql.w)};
        const float fr[4]{float(-qr.x), float(-qr.y), float(-qr.z), float(-qr.w)};  // the other hemisphere: same rotation
        const float pl[3]{float(0.1 - 0.032 * right.x), 1.2f, float(-0.032 * right.z)}, pr[3]{float(0.1 + 0.032 * right.x), 1.2f, float(0.032 * right.z)};
        head_pose::Sample v;
        check(head_pose::from_views(fl, pl, fr, pr, true, true, true, false, v), "from_views: usable");
        const hp::Quat got{v.orientation[0], v.orientation[1], v.orientation[2], v.orientation[3]};
        check(deg(angle_between(got, head)) < 1e-4, "from_views: the head's orientation", deg(angle_between(got, head)), 0);
        check(std::fabs(v.position[0] - 0.1f) < 1e-6f && std::fabs(v.position[1] - 1.2f) < 1e-6f && std::fabs(v.position[2]) < 1e-6f, "from_views: midpoint",
              v.position[0], v.position[2]);
        check(v.flags == (ef::orientation_valid | ef::orientation_tracked | ef::position_valid | ef::from_views), "from_views: flags", v.flags, 0);
        check(!head_pose::from_views(fl, pl, fr, pr, false, true, true, true, v), "from_views: no valid orientation, no pose");
        const float zero[4]{0, 0, 0, 0};
        check(!head_pose::from_views(zero, pl, fr, pr, true, true, true, true, v), "from_views: a zero quaternion, no pose");
        head_pose::Sample np;
        check(head_pose::from_views(fl, pl, fr, pr, true, false, true, true, np) && !(np.flags & ef::position_valid) && np.position[0] == 0,
              "from_views: no valid position, orientation only");
    }

    // A missing velocity is derived from the previous entry (90 deg/s yaw at 90 Hz), not over a base-space change or a
    // gap > 100 ms; a runtime velocity is kept as given.
    {
        head_pose::Publisher p2;
        auto b2 = new_block();
        const double rate = 90 * kPi / 180;
        const auto at = [&](double t) {
            head_pose::Sample x;
            const hp::Quat q = yaw(rate * t);
            x.orientation[0] = float(q.x); x.orientation[1] = float(q.y); x.orientation[2] = float(q.z); x.orientation[3] = float(q.w);
            x.position[0] = float(0.2 * t); x.position[1] = 1.2f; x.position[2] = 0;
            x.flags = ef::orientation_valid | ef::position_valid;
            return x;
        };
        const std::int64_t ns = 11'111'111;
        check(p2.publish(b2.get(), ns, 1, 0, 0x77, at(1 * 0.011111111)), "derive: first");
        hp::Entry e1;
        check(hp::ReadLatest(b2.get(), e1) && !(e1.flags & (ef::angular_velocity_valid | ef::linear_velocity_valid)), "derive: nothing to derive from on the first");
        check(p2.publish(b2.get(), 2 * ns, 2, 0, 0x77, at(2 * 0.011111111)), "derive: second");
        check(hp::ReadLatest(b2.get(), e1) && (e1.flags & ef::angular_velocity_valid) && (e1.flags & ef::linear_velocity_valid), "derive: both velocities derived");
        check(std::fabs(e1.angularVelocity[1] - rate) < 1e-3 && std::fabs(e1.angularVelocity[0]) < 1e-5 && std::fabs(e1.angularVelocity[2]) < 1e-5,
              "derive: angular velocity = 90 deg/s about +y", e1.angularVelocity[1], rate);
        check(std::fabs(e1.linearVelocity[0] - 0.2f) < 1e-3f && std::fabs(e1.linearVelocity[1]) < 1e-4f, "derive: linear velocity", e1.linearVelocity[0], 0.2);
        check(p2.publish(b2.get(), 3 * ns, 3, 0, 0x78, at(3 * 0.011111111)) && hp::ReadLatest(b2.get(), e1) && !(e1.flags & ef::angular_velocity_valid),
              "derive: not across a base-space change");
        check(p2.publish(b2.get(), 3 * ns + 150'000'000, 4, 0, 0x78, at(0.2)) && hp::ReadLatest(b2.get(), e1) && !(e1.flags & ef::angular_velocity_valid),
              "derive: not over a gap > 100 ms");
        head_pose::Sample given = at(0.21);
        given.angular_velocity[1] = 7.0f;
        given.flags |= ef::angular_velocity_valid;
        check(p2.publish(b2.get(), 3 * ns + 160'000'000, 5, 0, 0x78, given) && hp::ReadLatest(b2.get(), e1) && e1.angularVelocity[1] == 7.0f,
              "derive: the runtime's velocity is kept");
        // The other hemisphere of the same rotation must not read as a 360 deg turn.
        head_pose::Sample flipped = at(0.21 + 0.011111111);
        for (float& c : flipped.orientation) c = -c;
        check(p2.publish(b2.get(), 3 * ns + 160'000'000 + ns, 6, 0, 0x78, flipped) && hp::ReadLatest(b2.get(), e1) &&
                  std::fabs(e1.angularVelocity[1] - rate) < 1e-3,
              "derive: a sign-flipped quaternion is the same rotation", e1.angularVelocity[1], rate);
    }
}

// ---- Seqlock under concurrent writes -------------------------------------------------------------------------------
struct StressResult {
    std::uint64_t accepted{}, torn{}, order{}, busy{}, older{};
};
static void reader_loop(const hp::Block* b, std::atomic<bool>& stop, StressResult& r, unsigned seed) {
    hp::ReaderLimits limits;
    limits.attempts = 2;
    std::uint64_t last_index = 0;
    std::int64_t last_time = 0;
    std::uint32_t x = seed;
    while (!stop.load(std::memory_order_relaxed)) {
        hp::Entry e{};
        const hp::PoseStatus s = hp::ReadNewest(b, limits, e);
        if (s == hp::PoseStatus::kBusy) { ++r.busy; continue; }
        if (s != hp::PoseStatus::kOk) continue;
        ++r.accepted;
        if (!consistent(e)) ++r.torn;
        if (e.index < last_index || (e.index > last_index && e.displayTime <= last_time) || (e.index == last_index && e.displayTime != last_time)) ++r.order;
        last_index = e.index;
        last_time = e.displayTime;
        // An older entry still in the ring, by number: when accepted it must be complete and the one asked for.
        x = x * 1664525u + 1013904223u;
        const std::uint64_t back = (x >> 16) % hp::kRing;
        if (e.index > back) {
            hp::Entry o;
            if (hp::ReadIndex(b, e.index - back, o)) {
                ++r.older;
                if (!consistent(o) || o.index != e.index - back || (back && o.displayTime >= e.displayTime)) ++r.torn;
            }
        }
    }
}

static void stress_tests() {
    // The layer's path: two writer threads through one Publisher (its claim and lock), three readers.
    {
        auto b = new_block();
        head_pose::Publisher pub;
        std::atomic<bool> stop{false};
        std::atomic<std::int64_t> next_time{0};
        std::atomic<std::uint64_t> published{0}, refused{0};
        StressResult rs[3];
        std::vector<std::thread> threads;
        for (int i = 0; i < 3; ++i) threads.emplace_back(reader_loop, b.get(), std::ref(stop), std::ref(rs[i]), 17u + i);
        for (int w = 0; w < 2; ++w)
            threads.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    const std::int64_t t = next_time.fetch_add(1) + 1;
                    if (!pub.claim(t)) continue;
                    const Truth tr = truth(t);
                    if (pub.publish(b.get(), t, tr.qpc, tr.qpc + 5, 0x77, sample_of(tr))) published.fetch_add(1);
                    else refused.fetch_add(1);
                }
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        stop = true;
        for (auto& t : threads) t.join();
        StressResult total;
        for (const auto& r : rs) { total.accepted += r.accepted; total.torn += r.torn; total.order += r.order; total.busy += r.busy; total.older += r.older; }
        std::printf("stress (publisher, 2 writers, 3 readers, 1.5 s): %llu published, %llu refused (a later claim first), %llu newest reads, %llu older reads, %llu busy, %llu torn, %llu out of order\n",
                    static_cast<unsigned long long>(published.load()), static_cast<unsigned long long>(refused.load()), static_cast<unsigned long long>(total.accepted),
                    static_cast<unsigned long long>(total.older), static_cast<unsigned long long>(total.busy), static_cast<unsigned long long>(total.torn),
                    static_cast<unsigned long long>(total.order));
        check(published.load() > 10000 && total.accepted > 10000 && total.older > 1000, "stress: enough traffic", static_cast<double>(published.load()), static_cast<double>(total.accepted));
        check(total.torn == 0, "stress: no torn or mismatched copy accepted", static_cast<double>(total.torn), 0);
        check(total.order == 0, "stress: entry numbers and display times only go forward", static_cast<double>(total.order), 0);
        check(b->writeCount == published.load(), "stress: writeCount = entries published", static_cast<double>(b->writeCount), static_cast<double>(published.load()));
        // Every entry still in the ring after the run: complete, numbered n-15..n, display times increasing.
        hp::Entry prev{};
        bool ring_ok = true;
        for (std::uint64_t k = b->writeCount - hp::kRing + 1; k <= b->writeCount; ++k) {
            hp::Entry e{};
            ring_ok = ring_ok && hp::ReadIndex(b.get(), k, e) && consistent(e) && e.index == k && (k == b->writeCount - hp::kRing + 1 || e.displayTime > prev.displayTime);
            prev = e;
        }
        check(ring_ok, "stress: the final ring is complete and in order");
    }
    // The raw contract writer at full speed against the same readers (lapping on every few reads).
    {
        auto b = new_block();
        std::atomic<bool> stop{false};
        StressResult rs[3];
        std::vector<std::thread> threads;
        for (int i = 0; i < 3; ++i) threads.emplace_back(reader_loop, b.get(), std::ref(stop), std::ref(rs[i]), 91u + i);
        std::uint64_t written = 0;
        threads.emplace_back([&] {
            std::int64_t t = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const Truth tr = truth(++t);
                hp::Entry e{};
                e.displayTime = t; e.qpcLocate = tr.qpc; e.qpcDisplay = tr.qpc + 5; e.baseSpace = 0x77; e.flags = 63;
                std::copy(tr.q, tr.q + 4, e.orientation); std::copy(tr.p, tr.p + 3, e.position);
                std::copy(tr.w, tr.w + 3, e.angularVelocity); std::copy(tr.v, tr.v + 3, e.linearVelocity);
                hp::Publish(b.get(), e);
                ++written;
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        stop = true;
        for (auto& t : threads) t.join();
        StressResult total;
        for (const auto& r : rs) { total.accepted += r.accepted; total.torn += r.torn; total.order += r.order; total.busy += r.busy; total.older += r.older; }
        std::printf("stress (contract writer, 3 readers, 1 s): %llu written, %llu newest reads, %llu older reads, %llu busy, %llu torn, %llu out of order\n",
                    static_cast<unsigned long long>(written), static_cast<unsigned long long>(total.accepted), static_cast<unsigned long long>(total.older),
                    static_cast<unsigned long long>(total.busy), static_cast<unsigned long long>(total.torn), static_cast<unsigned long long>(total.order));
        check(written > 100000 && total.accepted > 10000, "raw stress: enough traffic", static_cast<double>(written), static_cast<double>(total.accepted));
        check(total.torn == 0 && total.order == 0, "raw stress: no torn copy, order kept", static_cast<double>(total.torn), static_cast<double>(total.order));
    }
    // Deterministic refusals: a slot being written (odd seq) and a slot already lapped (another index).
    {
        auto b = new_block();
        head_pose::Publisher pub;
        for (std::int64_t t = 1; t <= 20; ++t) { const Truth tr = truth(t); pub.publish(b.get(), t, tr.qpc, tr.qpc + 5, 0x77, sample_of(tr)); }
        hp::Entry e{};
        check(!hp::ReadIndex(b.get(), 3, e), "lapped: entry 3 was overwritten by entry 19");
        check(hp::ReadIndex(b.get(), 19, e) && e.displayTime == 19, "lapped: entry 19 reads");
        hp::AtomicU64(&b->ring[(20 - 1) % hp::kRing].seq).fetch_add(1);  // entry 20 "being written"
        check(!hp::ReadIndex(b.get(), 20, e), "odd seq: refused");
        check(hp::ReadLatest(b.get(), e) && e.index == 19, "odd seq: ReadLatest falls back to the previous entry", static_cast<double>(e.index), 19);
        hp::AtomicU64(&b->ring[(19 - 1) % hp::kRing].seq).fetch_add(1);
        hp::ReaderLimits limits;
        check(hp::ReadNewest(b.get(), limits, e) == hp::PoseStatus::kBusy, "both newest being written: busy, never a torn copy");
    }
}

// ---- Reader --------------------------------------------------------------------------------------------------------
static hp::Entry pose_entry(double yaw_rad, double rate, std::int64_t qpc_locate, std::int64_t qpc_display, std::int64_t display_time = 1000) {
    namespace ef = hp::entry_flags;
    hp::Entry e{};
    const hp::Quat q = yaw(yaw_rad);
    e.orientation[0] = float(q.x); e.orientation[1] = float(q.y); e.orientation[2] = float(q.z); e.orientation[3] = float(q.w);
    e.position[0] = 0.1f; e.position[1] = 1.2f; e.position[2] = -0.3f;
    e.angularVelocity[1] = float(rate);
    e.linearVelocity[0] = 0.5f;
    e.flags = ef::orientation_valid | ef::position_valid | ef::orientation_tracked | ef::position_tracked | ef::angular_velocity_valid | ef::linear_velocity_valid;
    e.qpcLocate = qpc_locate;
    e.qpcDisplay = qpc_display;
    e.displayTime = display_time;
    e.baseSpace = 0x77;
    return e;
}

static void reader_tests() {
    hp::ReaderLimits limits;
    hp::PredictedPose p;
    check(hp::ReadPredicted(nullptr, 0, 0, limits, p) == hp::PoseStatus::kNoBlock, "reader: no block");
    auto raw = std::make_unique<hp::Block>();
    std::memset(raw.get(), 0, sizeof(hp::Block));
    check(hp::ReadPredicted(raw.get(), 0, 0, limits, p) == hp::PoseStatus::kNotReady, "reader: all-zero block is not ready");
    auto b = new_block();
    check(hp::ReadPredicted(b.get(), 0, 0, limits, p) == hp::PoseStatus::kEmpty, "reader: ready but empty");
    hp::ReaderLimits other = limits;
    other.expectedPid = 1;
    check(hp::ReadPredicted(b.get(), 0, 0, other, p) == hp::PoseStatus::kWrongWriter, "reader: another process's writer refused");
    other.expectedPid = 4242;
    check(hp::ReadPredicted(b.get(), 0, 0, other, p) == hp::PoseStatus::kEmpty, "reader: the expected writer");

    const double rate = 90 * kPi / 180;  // 90 deg/s yaw
    const std::int64_t locate = 1'000'000'000, display = locate + 250'000;  // display 25 ms after the locate
    hp::Publish(b.get(), pose_entry(0.3, rate, locate, display));
    // Staleness by the locate time.
    check(hp::ReadPredicted(b.get(), locate + std::int64_t(0.149 * kFreq), display, limits, p) == hp::PoseStatus::kOk, "stale: 149 ms old is fresh");
    check(hp::ReadPredicted(b.get(), locate + std::int64_t(0.151 * kFreq), display, limits, p) == hp::PoseStatus::kStale, "stale: 151 ms old is stale");
    check(hp::ReadPredicted(b.get(), locate - std::int64_t(0.020 * kFreq), display, limits, p) == hp::PoseStatus::kInvalid, "stale: written 20 ms in the future: another clock");
    hp::ReaderLimits strict = limits;
    strict.staleS = 0.010;
    check(hp::ReadPredicted(b.get(), locate + std::int64_t(0.011 * kFreq), display, strict, p) == hp::PoseStatus::kStale, "stale: the limit is the reader's");

    // Extrapolation from the display time (qpcDisplay), exact for a constant rate within the horizon, either way.
    const std::int64_t now = locate + 100'000;
    for (double dt : {-0.050, -0.020, 0.0, 0.004, 0.020, 0.050}) {
        check(hp::ReadPredicted(b.get(), now, display + std::int64_t(dt * kFreq), limits, p) == hp::PoseStatus::kOk, "predict: ok");
        const double err = deg(angle_between(p.orientation, yaw(0.3 + rate * dt)));
        check(err < 1e-4 && std::fabs(p.aheadS - dt) < 1e-9, "predict: constant yaw rate exact within the horizon", err, dt);
        check(std::fabs(p.position.x - (0.1 + 0.5 * dt)) < 1e-6 && p.translated == (dt != 0), "predict: position by the linear velocity", p.position.x, 0.1 + 0.5 * dt);
    }
    check(p.rotated && std::fabs(p.ageS - 0.01) < 1e-9, "predict: rotated, age from the locate time", p.ageS, 0.01);
    for (double dt : {0.080, 1.0, -0.080, -1.0}) {
        hp::ReadPredicted(b.get(), now, display + std::int64_t(dt * kFreq), limits, p);
        const double capped = dt > 0 ? 0.050 : -0.050;
        check(std::fabs(p.aheadS - capped) < 1e-12 && deg(angle_between(p.orientation, yaw(0.3 + rate * capped))) < 1e-4, "predict: capped at 50 ms",
              p.aheadS, capped);
        check(std::fabs(p.position.x - (0.1 + 0.5 * capped)) < 1e-6, "predict: position capped too", p.position.x, 0.1 + 0.5 * capped);
    }
    hp::ReaderLimits short_horizon = limits;
    short_horizon.maxPredictionS = 0.020;
    hp::ReadPredicted(b.get(), now, display + std::int64_t(0.040 * kFreq), short_horizon, p);
    check(std::fabs(p.aheadS - 0.020) < 1e-12, "predict: the reader's shorter horizon", p.aheadS, 0.020);
    short_horizon.maxPredictionS = 1.0;
    hp::ReadPredicted(b.get(), now, display + std::int64_t(0.200 * kFreq), short_horizon, p);
    check(std::fabs(p.aheadS - hp::kMaxPredictionS) < 1e-12, "predict: never beyond kMaxPredictionS", p.aheadS, hp::kMaxPredictionS);

    // Without the display time in QPC the locate time is the entry's time.
    hp::Publish(b.get(), pose_entry(0.3, rate, locate + 10, 0, 2000));
    hp::ReadPredicted(b.get(), locate + 20, locate + 10 + std::int64_t(0.010 * kFreq), limits, p);
    check(std::fabs(p.aheadS - 0.010) < 1e-9 && p.entry.index == 2, "predict: qpcLocate when qpcDisplay is 0", p.aheadS, 0.010);

    // No velocity, a glitch velocity, a NaN velocity, no position.
    {
        hp::Entry e = pose_entry(0.3, rate, locate + 20, display + 20, 3000);
        e.flags &= ~hp::entry_flags::angular_velocity_valid;
        hp::Publish(b.get(), e);
        hp::ReadPredicted(b.get(), locate + 30, display + 20 + std::int64_t(0.030 * kFreq), limits, p);
        check(!p.rotated && deg(angle_between(p.orientation, yaw(0.3))) < 1e-4, "predict: no angular velocity, no rotation");
        e = pose_entry(0.3, 30.0, locate + 30, display + 30, 4000);
        hp::Publish(b.get(), e);
        hp::ReadPredicted(b.get(), locate + 40, display + 30 + std::int64_t(0.030 * kFreq), limits, p);
        check(!p.rotated && deg(angle_between(p.orientation, yaw(0.3))) < 1e-4, "predict: 30 rad/s is a glitch, no rotation");
        e = pose_entry(0.3, rate, locate + 40, display + 40, 5000);
        e.angularVelocity[0] = std::nanf("");
        e.linearVelocity[2] = 100.0f;
        hp::Publish(b.get(), e);
        hp::ReadPredicted(b.get(), locate + 50, display + 40 + std::int64_t(0.030 * kFreq), limits, p);
        check(!p.rotated && !p.translated && std::fabs(p.position.x - 0.1) < 1e-6, "predict: NaN angular / 100 m/s linear: neither extrapolated");
        e = pose_entry(0.3, rate, locate + 50, display + 50, 6000);
        e.flags &= ~hp::entry_flags::position_valid;
        hp::Publish(b.get(), e);
        hp::ReadPredicted(b.get(), locate + 60, display + 50 + std::int64_t(0.030 * kFreq), limits, p);
        check(p.position.x == 0 && p.position.y == 0 && !p.translated && p.rotated, "predict: no position, rotation only");
        e = pose_entry(0.3, rate, locate + 60, display + 60, 7000);
        e.orientation[3] = 3.0f;  // not a rotation (a writer that did not normalise)
        hp::Publish(b.get(), e);
        check(hp::ReadPredicted(b.get(), locate + 70, display, limits, p) == hp::PoseStatus::kInvalid, "reader: a non-unit quaternion is invalid");
        e.orientation[3] = std::nanf("");
        hp::Publish(b.get(), e);
        check(hp::ReadPredicted(b.get(), locate + 70, display, limits, p) == hp::PoseStatus::kInvalid, "reader: NaN orientation is invalid");
    }

    // The late correction: DCS rendered at yaw 0, the head is at yaw 10 deg (to the left) by the output time.
    {
        auto c = new_block();
        hp::Publish(c.get(), pose_entry(0.0, 0.0, locate, display, 1000));
        hp::Publish(c.get(), pose_entry(10 * kPi / 180, 0.0, locate + 110'000, display + 110'000, 2000));
        hp::LateCorrection lc{};
        hp::PredictedPose nowp;
        check(hp::LateCorrectionNow(c.get(), 1000, locate + 120'000, display + 110'000, limits, lc, &nowp) == hp::PoseStatus::kOk, "late: ok");
        const hp::Vec3 ahead = hp::Apply(lc, {10, 0, 0});  // 10 m straight ahead when DCS rendered (DCS: x forward, z right)
        const double az = deg(std::atan2(ahead.z, ahead.x));
        check(std::fabs(az - 10) < 1e-3, "late: a source straight ahead is now 10 deg to the right", az, 10);
        const hp::LateCorrection ref = hp::ComputeLateCorrection(yaw(0), {0.1, 1.2, -0.3}, nowp.orientation, nowp.position);
        check(deg(angle_between(ref.rotation, lc.rotation)) < 1e-9, "late: the contract's ComputeLateCorrection");
        check(hp::LateCorrectionNow(c.get(), 999, locate + 120'000, display, limits, lc) == hp::PoseStatus::kNotFound, "late: unknown display time");
        hp::Entry e = pose_entry(0.2, 0.0, locate + 120'000, display + 120'000, 3000);
        e.baseSpace = 0x99;
        hp::Publish(c.get(), e);
        check(hp::LateCorrectionNow(c.get(), 1000, locate + 130'000, display, limits, lc) == hp::PoseStatus::kOtherSpace, "late: another base space");
        check(hp::LateCorrectionNow(c.get(), 1000, locate + std::int64_t(0.5 * kFreq), display, limits, lc) == hp::PoseStatus::kStale, "late: stale");
        hp::Entry used;
        check(hp::ReadUsed(c.get(), 2000, limits, used) == hp::PoseStatus::kOk && used.index == 2, "used: by display time");
    }
    for (int s = 0; s <= static_cast<int>(hp::PoseStatus::kOtherSpace); ++s)
        check(std::strcmp(hp::PoseStatusName(static_cast<hp::PoseStatus>(s)), "?") != 0, "status names");
}

int main() {
    publisher_tests();
    reader_tests();
    stress_tests();
    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
