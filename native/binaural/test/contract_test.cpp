// Contract test for the binaural audio interface headers (docs/BINAURAL_AUDIO.md): they compile together with
// windows.h, their layouts hold, and their pure helpers do what the headers say. Exit code 0 = pass.
#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "binaural_config.h"
#include "binaural_core.h"
#include "binaural_room.h"
#include "cockpit_space.h"
#include "head_pose_shm.h"
#include "hrtf_format.h"

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                 \
    } while (0)

using dcsvr_headpose::Vec3; using dcsvr_headpose::Quat; using dcsvr_headpose::LateCorrection; using dcsvr_headpose::Block; using dcsvr_headpose::Entry;
using dcsvr_headpose::OxrToDcs; using dcsvr_headpose::DcsToOxr; using dcsvr_headpose::Rotate; using dcsvr_headpose::Normalize;
using dcsvr_headpose::Mul; using dcsvr_headpose::Conj; using dcsvr_headpose::AxisAngle; using dcsvr_headpose::Apply;
using dcsvr_headpose::ComputeLateCorrection; using dcsvr_headpose::ReadLatest; using dcsvr_headpose::ReadIndex;
using dcsvr_headpose::ReadForDisplayTime; using dcsvr_headpose::InitWriter; using dcsvr_headpose::Publish;
using dcsvr_headpose::AtomicU64; using dcsvr_headpose::PredictOrientation; using dcsvr_headpose::kRing;
namespace entry_flags = dcsvr_headpose::entry_flags;

static bool Near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }
static bool NearV(const Vec3& a, const Vec3& b, double eps = 1e-9) {
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
}

static void TestAxes() {
    // OpenXR forward (-z) is DCS forward (+x); OpenXR right (+x) is DCS right (+z).
    CHECK(NearV(OxrToDcs(Vec3{0, 0, -1}), Vec3{1, 0, 0}));
    CHECK(NearV(OxrToDcs(Vec3{1, 0, 0}), Vec3{0, 0, 1}));
    CHECK(NearV(OxrToDcs(Vec3{0, 1, 0}), Vec3{0, 1, 0}));
    CHECK(NearV(DcsToOxr(OxrToDcs(Vec3{0.3, -0.2, 0.7})), Vec3{0.3, -0.2, 0.7}));
    // The quaternion mapping commutes with rotation for random rotations and vectors.
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1, 1);
    for (int i = 0; i < 1000; ++i) {
        const Quat q = Normalize({u(rng), u(rng), u(rng), u(rng)});
        const Vec3 v{u(rng), u(rng), u(rng)};
        CHECK(NearV(Rotate(OxrToDcs(q), OxrToDcs(v)), OxrToDcs(Rotate(q, v)), 1e-12));
        // Rotate agrees with the quaternion product q v q*.
        const Quat pv{v.x, v.y, v.z, 0};
        const Quat r = Mul(Mul(q, pv), Conj(q));
        CHECK(NearV(Rotate(q, v), Vec3{r.x, r.y, r.z}, 1e-12));
    }
}

static void TestLateCorrection() {
    const Quat id{0, 0, 0, 1};
    // No head motion: identity.
    LateCorrection c = ComputeLateCorrection(id, {0, 0, 0}, id, {0, 0, 0});
    CHECK(NearV(Apply(c, {1, 0, 0}), {1, 0, 0}));
    // The head turns 30 degrees to the LEFT (OpenXR: +30 degrees about +y turns -z towards -x). A source that was
    // straight ahead must now be to the right (DCS z > 0), still level, same distance.
    const double a = 30.0 * 3.14159265358979323846 / 180.0;
    c = ComputeLateCorrection(id, {0, 0, 0}, AxisAngle({0, 1, 0}, a), {0, 0, 0});
    const Vec3 p = Apply(c, {2, 0, 0});
    CHECK(p.z > 0.9 && Near(p.y, 0.0, 1e-12) && Near(std::sqrt(p.x * p.x + p.z * p.z), 2.0, 1e-12));
    CHECK(Near(p.x, 2 * std::cos(a), 1e-12) && Near(p.z, 2 * std::sin(a), 1e-12));
    // The head rolls 20 degrees to the right (OpenXR: -20 degrees about -z forward axis = +20 about +z? right ear down
    // is a rotation about the forward axis): a source on the right side (DCS z) moves up in the head frame.
    c = ComputeLateCorrection(id, {0, 0, 0}, AxisAngle({0, 0, -1}, 20.0 * 3.14159265358979323846 / 180.0), {0, 0, 0});
    const Vec3 r = Apply(c, {0, 0, 1});
    CHECK(r.y > 0.3);
    // The head moves 0.1 m to the right (OpenXR +x): a source 1 m ahead is now 0.1 m to the left (DCS z = -0.1).
    c = ComputeLateCorrection(id, {0, 0, 0}, id, {0.1, 0, 0});
    CHECK(NearV(Apply(c, {1, 0, 0}), {1, 0, -0.1}, 1e-12));
    // Composition with a non-identity used pose: only the relative motion matters.
    const Quat base = Normalize({0.1, 0.7, -0.2, 0.6});
    c = ComputeLateCorrection(base, {1, 2, 3}, base, {1, 2, 3});
    CHECK(NearV(Apply(c, {0.4, -0.3, 0.2}), {0.4, -0.3, 0.2}, 1e-12));
}

static void TestPoseRing() {
    Block* b = new Block();
    std::memset(b, 0, sizeof(Block));
    Entry e{};
    CHECK(!ReadLatest(b, e));  // never written
    InitWriter(b, 10000000, 1234, 0);
    for (int i = 1; i <= 40; ++i) {
        Entry w{};
        w.displayTime = 1000 + i;
        w.qpcLocate = 5000 + i;
        w.flags = entry_flags::orientation_valid;
        w.orientation[3] = 1.0f;
        w.frame = static_cast<std::uint32_t>(i);
        Publish(b, w);
    }
    CHECK(ReadLatest(b, e) && e.displayTime == 1040 && e.index == 40 && (e.seq & 1u) == 0);
    CHECK(ReadForDisplayTime(b, 1030, e) && e.frame == 30);
    CHECK(!ReadForDisplayTime(b, 1010, e));  // overwritten (ring of 16)
    // A slot in the middle of a write is refused.
    AtomicU64(&b->ring[(40 - 1) % kRing].seq).fetch_add(1);
    Entry f{};
    CHECK(!ReadIndex(b, 40, f));
    CHECK(ReadLatest(b, f) && f.index == 39);  // falls back to the previous one
    // Prediction: 90 deg/s about +y for 20 ms = 1.8 degrees; capped at 50 ms.
    Entry p{};
    p.flags = entry_flags::orientation_valid | entry_flags::angular_velocity_valid;
    p.orientation[3] = 1.0f;
    p.angularVelocity[1] = static_cast<float>(3.14159265358979323846 / 2);
    p.qpcLocate = 0;
    Quat q = PredictOrientation(p, 200000, 10000000);  // 20 ms
    CHECK(Near(2 * std::acos(q.w) * 180.0 / 3.14159265358979323846, 1.8, 1e-4));
    q = PredictOrientation(p, 10000000, 10000000);  // 1 s, capped to 50 ms = 4.5 degrees
    CHECK(Near(2 * std::acos(q.w) * 180.0 / 3.14159265358979323846, 4.5, 1e-4));
    delete b;
}

static std::vector<std::uint32_t> MakeOctahedronFile(std::uint16_t taps, bool corrupt) {
    using namespace hrtf_format;
    const Direction dirs[6] = {{1, 0, 0, 0}, {-1, 0, 0, 0}, {0, 1, 0, 0}, {0, -1, 0, 0}, {0, 0, 1, 0}, {0, 0, -1, 0}};
    const Triangle tris[8] = {{0, 2, 4}, {0, 5, 2}, {0, 3, 5}, {0, 4, 3}, {1, 4, 2}, {1, 2, 5}, {1, 5, 3}, {1, 3, 4}};
    const std::uint64_t payload = PayloadBytes(6, 8, taps);
    std::vector<std::uint32_t> words((sizeof(Header) + payload + 3) / 4, 0u);
    auto* bytes = reinterpret_cast<unsigned char*>(words.data());
    Header h{};
    h.magic = kMagic;
    h.version = kVersion;
    h.headerBytes = kHeaderBytes;
    h.flags = flags::kMinimumPhase | flags::kDiffuseFieldEq | flags::kFrontNormalised;
    h.sampleRate = 48000;
    h.directionCount = 6;
    h.triangleCount = 8;
    h.taps = taps;
    h.datasetHeadRadiusM = 0.0875f;
    h.measurementDistanceM = 1.2f;
    h.frontGain = 1.0f;
    h.payloadBytes = payload;
    strcpy_s(h.name, "test octahedron");
    strcpy_s(h.source, "contract test");
    unsigned char* p = bytes + sizeof(Header);
    std::memcpy(p, dirs, sizeof(dirs));
    auto* delays = reinterpret_cast<Delay*>(p + sizeof(dirs));
    for (int i = 0; i < 6; ++i) delays[i] = {10.0f + dirs[i].z * -20.0f + 20.0f, 10.0f + dirs[i].z * 20.0f + 20.0f};
    std::memcpy(p + sizeof(dirs) + 6 * sizeof(Delay), tris, sizeof(tris));
    auto* ir = reinterpret_cast<float*>(p + sizeof(dirs) + 6 * sizeof(Delay) + sizeof(tris));
    for (int d = 0; d < 6; ++d)
        for (int ear = 0; ear < 2; ++ear) ir[(d * 2 + ear) * taps] = 1.0f;  // unit impulses
    h.payloadFnv1a = Fnv1a(p, static_cast<std::size_t>(payload));
    h.headerFnv1a = HeaderChecksum(h);
    std::memcpy(bytes, &h, sizeof(h));
    if (corrupt) ir[5] = 0.5f;  // after the checksum: must be detected
    return words;
}

static void TestHrtfFormat() {
    using namespace hrtf_format;
    std::vector<std::uint32_t> f = MakeOctahedronFile(32, false);
    const std::size_t bytes = sizeof(Header) + PayloadBytes(6, 8, 32);
    View v;
    const char* why = nullptr;
    CHECK(Validate(f.data(), bytes, &v, &why));
    if (why) std::printf("  why: %s\n", why);
    CHECK(v.header->directionCount == 6 && v.triangles[7].c == 4 && v.Ir(5, 1)[0] == 1.0f && v.Ir(5, 1)[1] == 0.0f);
    CHECK(!Validate(f.data(), bytes - 4, &v, &why));  // truncated
    std::vector<std::uint32_t> bad = MakeOctahedronFile(32, true);
    CHECK(!Validate(bad.data(), bytes, &v, &why) && std::strcmp(why, "payload checksum") == 0);
    std::vector<std::uint32_t> g = f;
    reinterpret_cast<Header*>(g.data())->sampleRate = 44100;  // header checksum now wrong
    CHECK(!Validate(g.data(), bytes, &v, &why) && std::strcmp(why, "header checksum") == 0);
    CHECK(PayloadBytes(8802, 17600, 128) == 8802ull * 24 + 17600ull * 12 + 8802ull * 2 * 128 * 4);
}

static void TestConfig() {
    using namespace binaural_config;
    CHECK(kKeyCount == 13);
    for (int i = 0; i < kKeyCount; ++i) {
        const KeyInfo& k = kKeys[i];
        if (k.type != KeyType::Text) CHECK(k.defaultValue >= k.minValue && k.defaultValue <= k.maxValue);
    }
    AudioConfig def;
    CHECK(!def.binaural && def.maxObjects == 48 && def.cpuBudgetPercent == 25 && std::wcscmp(def.hrtf, kDefaultHrtfName) == 0);
    CHECK(std::fabs(HeadRadiusFromCircumferenceCm(57.0f) - 0.0907f) < 1e-3f);
    CHECK(HeadRadiusFromCircumferenceCm(10.0f) >= 0.06f && HeadRadiusFromCircumferenceCm(100.0f) <= 0.12f);
    // The ITD radius: the default circumference plays the set's own timing (scale exactly 1, whatever its fitted radius);
    // other sizes scale it by the circumference ratio; clamps as the key's range and the renderer's.
    CHECK(PilotItdRadiusM(0.0875f, 57.0f) == 0.0875f && PilotItdRadiusM(0.0912f, 57.0f) == 0.0912f);
    CHECK(std::fabs(PilotItdRadiusM(0.0875f, 60.0f) / 0.0875f - 60.0 / 57.0) < 1e-6);
    CHECK(std::fabs(PilotItdRadiusM(0.0875f, 30.0f) / 0.0875f - 48.0 / 57.0) < 1e-6);
    CHECK(PilotItdRadiusM(0.14f, 66.0f) == 0.15f && PilotItdRadiusM(0.0f, 57.0f) == 0.0875f);
    // The default set's fit (SADIE II KU100, 0.0943 m; 0.1194 m with the earlier onsets) scales over the whole head-size
    // range, unclamped.
    for (const float fit : {0.0943f, 0.1194f})
        CHECK(std::fabs(PilotItdRadiusM(fit, 66.0f) / fit - 66.0 / 57.0) < 1e-6 && std::fabs(PilotItdRadiusM(fit, 48.0f) / fit - 48.0 / 57.0) < 1e-6);
    CHECK(HeadScaleFromCircumferenceCm(std::nanf("")) == 1.0);
    std::uint32_t m = 99;
    CHECK(ParseExcludeContexts(L"", &m) && m == 0);
    CHECK(ParseExcludeContexts(L" 1, 3 ,5", &m) && m == ((1u << 1) | (1u << 3) | (1u << 5)));
    CHECK(!ParseExcludeContexts(L"8", &m));
    CHECK(!ParseExcludeContexts(L"1;2", &m));
    CHECK(!ParseExcludeContexts(L"a", &m));
    CHECK(IsValidHrtfName(L"default") && IsValidHrtfName(L"My head 2024-05"));
    CHECK(!IsValidHrtfName(L"..\\x") && !IsValidHrtfName(L"a/b") && !IsValidHrtfName(L"") && !IsValidHrtfName(L".hidden"));
    CHECK(std::fabs(TrimGain(-6.0f) - 0.5012f) < 1e-3f && std::fabs(TrimGain(100.0f) - 1.9953f) < 1e-3f);
}

// C1: the cockpit space headers (docs/COCKPIT_SPACE.md 8.1): keys, layouts, presets, the type table.
static void TestCockpitSpace() {
    using namespace binaural_config;
    using namespace cockpit_space;
    // Keys: the three new [Audio] keys with their defaults and ranges, and the AudioConfig defaults.
    auto key = [](const wchar_t* name) -> const KeyInfo* {
        for (const KeyInfo& k : kKeys)
            if (std::wcscmp(k.name, name) == 0) return &k;
        return nullptr;
    };
    const KeyInfo* cs = key(L"CockpitSpace");
    const KeyInfo* fam = key(L"CockpitSpaceFamily");
    const KeyInfo* can = key(L"CockpitSpaceCanopy");
    CHECK(cs && cs->type == KeyType::Int && cs->defaultValue == 1 && cs->minValue == 0 && cs->maxValue == 3 && std::strcmp(cs->ui, "main") == 0);
    CHECK(fam && fam->type == KeyType::Text && std::strcmp(fam->ui, "advanced") == 0);
    CHECK(can && can->type == KeyType::Bool && can->defaultValue == 1 && std::strcmp(can->ui, "advanced") == 0);
    AudioConfig def;
    CHECK(def.cockpitSpace == 1 && std::wcscmp(def.cockpitSpaceFamily, L"auto") == 0 && def.cockpitSpaceCanopy);
    CHECK(std::wcscmp(kTestSection, L"AudioTest") == 0 && sizeof(kTestKeys) / sizeof(kTestKeys[0]) == 3 &&
          sizeof(kTestHotkeys) / sizeof(kTestHotkeys[0]) == 8);
    CHECK(IsValidCockpitSpaceFamily(L"auto") && IsValidCockpitSpaceFamily(L" Bubble ") && IsValidCockpitSpaceFamily(L"transport"));
    CHECK(!IsValidCockpitSpaceFamily(L"") && !IsValidCockpitSpaceFamily(L"bubbles") && !IsValidCockpitSpaceFamily(nullptr) &&
          !IsValidCockpitSpaceFamily(L"generic generic generic"));

    // Layouts (the module and the harness share these structs).
    static_assert(sizeof(SurfaceParams) == 16, "SurfaceParams layout");
    static_assert(sizeof(RoomParams) == 140, "RoomParams layout");
    static_assert(offsetof(RoomParams, surface) == 8 && offsetof(RoomParams, earlyDb) == 104 &&
                  offsetof(RoomParams, neutralShare) == 136, "RoomParams offsets");
    static_assert(sizeof(binaural::RoomFrame) == 32 && offsetof(binaural::RoomFrame, frameValid) == 16 &&
                  offsetof(binaural::RoomFrame, wet) == 20 && offsetof(binaural::RoomFrame, canopyOpen) == 24 &&
                  offsetof(binaural::RoomFrame, early) == 28, "RoomFrame layout");
    static_assert(sizeof(binaural::RoomSource) == 20 && offsetof(binaural::RoomSource, send) == 12 &&
                  offsetof(binaural::RoomSource, flags) == 16, "RoomSource layout");
    CHECK(binaural::kRoomBuses == 14 && binaural::kRoomTaps == 8 && binaural::kRoomFdnLines == 8);
    CHECK(binaural::room_flags::kReset == 1u && binaural::room_flags::kThroughCanopy == 2u && binaural::room_flags::kLateOnly == 4u);

    // Presets: every family x strength is finite, every R < 1, RT60 > 0; strength steps as section 5.3.
    for (int f = 0; f < static_cast<int>(Family::Count); ++f) {
        RoomParams n{};
        CHECK(Preset(static_cast<Family>(f), kStrengthNormal, &n));
        for (int s = kStrengthSubtle; s <= kStrengthStrong; ++s) {
            RoomParams p{};
            CHECK(Preset(static_cast<Family>(f), s, &p));
            bool ok = p.strength == s && static_cast<int>(p.family) == f;
            for (const SurfaceParams& w : p.surface)
                ok = ok && std::isfinite(w.distanceM) && w.distanceM > 0.1f && w.reflection > 0.0f && w.reflection < 1.0f &&
                     w.lowpassHz >= 0.0f && w.highpassHz >= 0.0f;
            ok = ok && p.rt60LowS > 0.0f && p.rt60HighS > 0.0f && p.preDelayMs > 0.0f && p.sendHpHz == 200.0f &&
                 p.neutralShare == kNeutralShare && std::isfinite(p.earlyDb) && std::isfinite(p.lateDb);
            const float de[] = {0, -4, 0, 3}, dl[] = {0, -5, 0, 4}, rt[] = {0, 0.9f, 1.0f, 1.2f};
            ok = ok && p.earlyDb == n.earlyDb + de[s] && p.lateDb == n.lateDb + dl[s] && std::fabs(p.rt60LowS - n.rt60LowS * rt[s]) < 1e-6f;
            CHECK(ok);
            const RoomParams q = Sanitize(p);  // a preset is already in range
            bool same = q.family == p.family && q.strength == p.strength && q.earlyDb == p.earlyDb && q.lateDb == p.lateDb &&
                        q.rt60LowS == p.rt60LowS && q.rt60HighS == p.rt60HighS && q.crossoverHz == p.crossoverHz &&
                        q.preDelayMs == p.preDelayMs && q.sendHpHz == p.sendHpHz && q.throughDb == p.throughDb &&
                        q.neutralShare == p.neutralShare;
            for (int w = 0; w < kSurfaces; ++w)
                same = same && std::memcmp(&q.surface[w], &p.surface[w], sizeof(SurfaceParams)) == 0;
            CHECK(same);
        }
        CHECK(!Preset(static_cast<Family>(f), 0, &n) && !Preset(static_cast<Family>(f), 4, &n));
    }
    // Section 5.2 spot checks: bubble canopy and helicopter rows.
    RoomParams b{};
    Preset(Family::Bubble, kStrengthNormal, &b);
    CHECK(b.surface[kUp].distanceM == 0.25f && b.surface[kUp].reflection == 0.85f && b.surface[kUp].highpassHz == 300.0f &&
          b.surface[kLeft].lowpassHz == 8000.0f && b.surface[kLeft].highpassHz == 250.0f && b.earlyDb == -12.0f &&
          b.lateDb == -17.0f && b.rt60LowS == 0.10f && b.rt60HighS == 0.08f && b.preDelayMs == 2.0f);
    RoomParams h{};
    Preset(Family::Helicopter, kStrengthSubtle, &h);
    CHECK(h.surface[kRight].distanceM == 0.90f && h.surface[kBack].distanceM == 1.20f && h.earlyDb == -15.0f && h.lateDb == -19.0f);
    RoomParams bad = b;
    bad.surface[kUp].reflection = 3.0f, bad.rt60LowS = std::nanf(""), bad.strength = 9;
    const RoomParams cl = Sanitize(bad);
    CHECK(cl.surface[kUp].reflection == 0.95f && std::isfinite(cl.rt60LowS) && cl.strength == 3);

    // Families: keys round trip, the type table (exact, prefixes, unknown -> Generic).
    for (int f = 0; f < static_cast<int>(Family::Count); ++f) {
        Family g;
        bool isAuto = true;
        CHECK(ParseFamily(FamilyKey(static_cast<Family>(f)), &g, &isAuto) && g == static_cast<Family>(f) && !isAuto);
    }
    Family g;
    bool isAuto = false;
    CHECK(ParseFamily(L"AUTO", &g, &isAuto) && isAuto);
    CHECK(FamilyForType("F-16C_50") == Family::Bubble && FamilyForType("FA-18C_hornet") == Family::Bubble);
    CHECK(FamilyForType("F-14B") == Family::Framed && FamilyForType("Su-25T") == Family::Framed && FamilyForType("Su-27") == Family::Bubble);
    CHECK(FamilyForType("UH-1H") == Family::Helicopter && FamilyForType("Mi-8MT") == Family::Helicopter && FamilyForType("Mi-24P") == Family::Framed);
    CHECK(FamilyForType("P-51D-30-NA") == Family::Warbird && FamilyForType("SpitfireLFMkIXCW") == Family::Warbird);
    CHECK(FamilyForType("C-130J-30") == Family::Transport && FamilyForType("C-101CC") == Family::Framed);
    // Prefixes (case-insensitive, longest wins) for names not in the table.
    CHECK(FamilyForType("f-16c_block70") == Family::Bubble && FamilyForType("F-14D") == Family::Framed &&
          FamilyForType("Su-25TM") == Family::Framed && FamilyForType("FW-190A9") == Family::Warbird &&
          FamilyForType("SA342X") == Family::Helicopter && FamilyForType("Hawk T.1A") == Family::Framed);
    CHECK(FamilyForType("Yak-40") == Family::Generic && FamilyForType("") == Family::Generic && FamilyForType(nullptr) == Family::Generic);
    // Every exact name resolves to its own entry (no earlier duplicate with another family).
    for (const TypeEntry& e : kTypes) CHECK(FamilyForType(e.name) == e.family);
}

static void TestCoreDeclarations() {
    // The core is implemented in src/; here only its declarations are checked to be usable.
    binaural::Config c;
    CHECK(c.sampleRate == 48000 && c.maxSources <= binaural::kMaxSources && c.nearField);
    binaural::SourceUpdate s{{1, 0, 0}, 1.0f, binaural::source_flags::kReset};
    binaural::MonoInput in{nullptr, nullptr, 2, 1.0f};
    CHECK(s.flags == 1u && in.stride == 2);
    CHECK(sizeof(binaural::Stats::blockNsHistogram) / sizeof(std::uint32_t) == 16);
}

int main() {
    TestAxes();
    TestLateCorrection();
    TestPoseRing();
    TestHrtfFormat();
    TestConfig();
    TestCockpitSpace();
    TestCoreDeclarations();
    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
