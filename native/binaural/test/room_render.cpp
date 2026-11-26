// binaural_room_render: renders the cockpit space offline (docs/COCKPIT_SPACE.md 10, 11) for the listening-test kit
// and for checking the metrics by hand. No DCS, no audio device: WAV files and one JSON per run.
//
//   binaural_room_render --out <dir> [--name <base>] [--family <key>] [--strength 0..3] [--source x,y,z]...
//                        [--yaw <deg>] [--canopy 0..1] [--signal impulse|pink|pinkbursts|clickhum|dcs] [--seconds <s>]
//                        [--hrtf <file.dcshrtf>] [--rate <Hz>] [--gain-db <dB>] [--seed <n>]
//
//   --source: aircraft frame, metres, head at the origin (x forward, y up, z right); several sources play together.
//             Default 0.6,0,0. --yaw turns the head against the aircraft (positive: to the left).
//   --strength 0 renders Off (the direct path alone, today's binaural).
//   --hrtf: a .dcshrtf (default: native/binaural/data/sadie2_ku100.dcshrtf when present, else the synthetic sphere).
//   Writes <dir>/<base>.wav (16-bit PCM stereo, direct + room, x gain) and <dir>/<base>.json: the run's settings and,
//   for the first source, R1 (IACC early Off / on, late per octave), R2 (DRR), R3 (EDT, T20 per octave), R9 (ripple)
//   and R4 (K-weighted loudness change of pink noise against Off, at 48 kHz). With --signal impulse it also writes
//   <base>_direct.wav and <base>_room.wav.
// Exit code: 0 written, 1 bad arguments, 2 HRTF or write failure.
#include <windows.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "room_harness.h"
#include "synthetic_hrtf.h"

using namespace room_harness;
namespace cs = cockpit_space;

static std::vector<std::uint32_t> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamoff n = f.tellg();
    if (n <= 0 || n % 4) return {};
    std::vector<std::uint32_t> v(static_cast<std::size_t>(n / 4));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), n);
    return v;
}
static bool Parse3(const char* s, float out[3]) { return sscanf_s(s, "%f,%f,%f", &out[0], &out[1], &out[2]) == 3; }
static int Usage(const char* why) {
    std::fprintf(stderr, "binaural_room_render: %s\n", why);
    std::fprintf(stderr, "usage: binaural_room_render --out <dir> [--name <base>] [--family <key>] [--strength 0..3] [--source x,y,z]... "
                         "[--yaw <deg>] [--canopy 0..1] [--signal impulse|pink|pinkbursts|clickhum|dcs] [--seconds <s>] [--hrtf <file>] "
                         "[--rate <Hz>] [--gain-db <dB>] [--seed <n>]\n");
    return 1;
}

int main(int argc, char** argv) {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    std::string out, name = "render", family = "generic", signal = "impulse", hrtf;
    int strength = 1;
    double yaw = 0.0, seconds = 1.5, gainDb = 0.0;
    float canopy = 0.0f;
    std::uint32_t rate = 48000;
    unsigned seed = 1;
    std::vector<std::array<float, 3>> sources;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (!more) return Usage(("missing value after " + a).c_str());
        const char* v = argv[++i];
        if (a == "--out") out = v;
        else if (a == "--name") name = v;
        else if (a == "--family") family = v;
        else if (a == "--strength") strength = std::atoi(v);
        else if (a == "--yaw") yaw = std::atof(v);
        else if (a == "--canopy") canopy = static_cast<float>(std::atof(v));
        else if (a == "--signal") signal = v;
        else if (a == "--seconds") seconds = std::atof(v);
        else if (a == "--hrtf") hrtf = v;
        else if (a == "--rate") rate = static_cast<std::uint32_t>(std::atoi(v));
        else if (a == "--gain-db") gainDb = std::atof(v);
        else if (a == "--seed") seed = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        else if (a == "--source") {
            std::array<float, 3> p;
            if (!Parse3(v, p.data())) return Usage("--source wants x,y,z");
            sources.push_back(p);
        } else {
            return Usage(("unknown option " + a).c_str());
        }
    }
    if (out.empty()) return Usage("--out is required");
    if (strength < 0 || strength > 3) return Usage("--strength is 0..3");
    if (seconds <= 0.0 || seconds > 60.0) return Usage("--seconds is 0..60");
    if (rate < kMinSampleRate || rate > kMaxSampleRate) return Usage("--rate out of range");
    if (name.find_first_of("\\/:*?\"<>|") != std::string::npos) return Usage("--name must be a plain file name");
    if (sources.empty()) sources.push_back({kRoomRefDistanceM, 0.0f, 0.0f});
    cs::Family fam;
    bool isAuto = false;
    const std::wstring wf(family.begin(), family.end());
    if (!cs::ParseFamily(wf.c_str(), &fam, &isAuto) || isAuto) return Usage("--family is bubble|framed|helicopter|warbird|transport|generic");

    std::vector<std::uint32_t> file;
    std::string hrtfName;
    if (!hrtf.empty()) {
        file = ReadFile(hrtf), hrtfName = hrtf;
    } else {
        file = ReadFile(std::string(BINAURAL_DATA_DIR) + "/sadie2_ku100.dcshrtf"), hrtfName = "default (SADIE II KU100)";
        if (file.empty()) file = synth::Build(synth::Options{}), hrtfName = "synthetic sphere";
    }
    if (file.empty()) {
        std::fprintf(stderr, "binaural_room_render: cannot read the HRTF file\n");
        return 2;
    }
    RoomRig rig(file, rate, 64);
    if (!rig.ok) return 2;

    RoomParams p{};
    cs::Preset(fam, strength == 0 ? cs::kStrengthNormal : strength, &p);
    float q[4];
    YawQuat(-yaw, q);  // the head turned by +yaw against the aircraft: aircraft -> head is the opposite turn

    // The stimulus.
    const std::uint32_t warm = static_cast<std::uint32_t>(0.6 * rate), len = static_cast<std::uint32_t>(seconds * rate);
    Vec sig(std::size_t(warm) + len, 0.0f);
    {
        Vec s;
        if (signal == "impulse") s.assign(len, 0.0f), s[0] = 1.0f;
        else if (signal == "pink") s = Pink(len, seed, 0.1f);
        else if (signal == "pinkbursts") s = PinkBursts(len, rate, seed);
        else if (signal == "clickhum") s = ClickHum(len, rate);
        else if (signal == "dcs") s = DcsLike(len, rate, seed);
        else return Usage("--signal is impulse|pink|pinkbursts|clickhum|dcs");
        std::copy(s.begin(), s.end(), sig.begin() + warm);
    }
    SceneState st;
    st.params = p;
    std::memcpy(st.q, q, sizeof(q));
    st.canopy = canopy;
    if (strength == 0) st.directGain = false, st.renderRoom = false;
    std::vector<SceneSource> src(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        std::memcpy(src[i].pos, sources[i].data(), sizeof(float) * 3);
        src[i].signal = sig.data();
        // A source outside the box is a world sound heard through the canopy (as the module classes it in the cockpit).
        if (!room_model::InsideBox(p, src[i].pos)) src[i].flags = room_flags::kThroughCanopy;
    }
    const SceneOut o = RenderScene(rig, st, src, warm + len, 480);
    const Vec direct(o.direct.begin() + 2 * std::size_t(warm), o.direct.end());
    const Vec room(o.room.begin() + 2 * std::size_t(warm), o.room.end());
    const Vec total = Add(direct, room);
    const float gain = static_cast<float>(std::pow(10.0, gainDb / 20.0));
    CreateDirectoryA(out.c_str(), nullptr);
    const std::string base = out + "\\" + name;
    const std::size_t clipped = WriteWav16(base + ".wav", total, rate, gain);
    if (clipped == ~std::size_t(0)) {
        std::fprintf(stderr, "binaural_room_render: cannot write %s.wav\n", base.c_str());
        return 2;
    }
    if (signal == "impulse") {
        WriteWav16(base + "_direct.wav", direct, rate, gain);
        WriteWav16(base + "_room.wav", room, rate, gain);
    }

    // Metrics for the first source (impulse responses at this strength; Off reports the dry ones).
    const SourceMetrics m = MeasureSource(rig, p, src[0].pos, q);
    double r4 = 0.0;
    bool haveR4 = false;
    if (rate == 48000 && strength > 0) {
        const Vec pink = Pink(std::size_t(warm) + 2 * rate, seed + 7, 0.1f);
        SceneState s2 = st;
        std::vector<SceneSource> one(1);
        std::memcpy(one[0].pos, src[0].pos, sizeof(one[0].pos));
        one[0].signal = pink.data(), one[0].flags = src[0].flags;
        const SceneOut on = RenderScene(rig, s2, one, static_cast<std::uint32_t>(pink.size()), 480);
        s2.directGain = false, s2.renderRoom = false;
        const SceneOut off = RenderScene(rig, s2, one, static_cast<std::uint32_t>(pink.size()), 480);
        r4 = Loudness(on.Total(), warm) - Loudness(off.direct, warm);
        haveR4 = true;
    }
    FILE* j = nullptr;
    if (fopen_s(&j, (base + ".json").c_str(), "wb") != 0 || !j) {
        std::fprintf(stderr, "binaural_room_render: cannot write %s.json\n", base.c_str());
        return 2;
    }
    auto arr = [&](const char* key, const double* v, bool last = false) {
        std::fprintf(j, "    \"%s\": [%.4f, %.4f, %.4f, %.4f]%s\n", key, v[0], v[1], v[2], v[3], last ? "" : ",");
    };
    std::fprintf(j, "{\n  \"family\": \"%s\",\n  \"strength\": %d,\n  \"yawDeg\": %.2f,\n  \"canopy\": %.3f,\n  \"signal\": \"%s\",\n",
                 family.c_str(), strength, yaw, canopy, signal.c_str());
    std::fprintf(j, "  \"seconds\": %.3f,\n  \"rate\": %u,\n  \"gainDb\": %.2f,\n  \"clippedSamples\": %zu,\n", seconds, rate, gainDb, clipped);
    std::fprintf(j, "  \"hrtf\": \"%s\",\n  \"sources\": [", hrtf.empty() ? hrtfName.c_str() : "file");
    for (std::size_t i = 0; i < sources.size(); ++i)
        std::fprintf(j, "%s[%.3f, %.3f, %.3f]", i ? ", " : "", sources[i][0], sources[i][1], sources[i][2]);
    std::fprintf(j, "],\n  \"metrics\": {\n    \"octavesHz\": [500, 1000, 2000, 4000],\n");
    arr("iaccEarlyOff", m.iaccEarlyOff);
    arr("iaccEarly", m.iaccEarly);
    arr("iaccLate", m.iaccLate);
    arr("edtS", m.edt);
    arr("t20S", m.t20);
    std::fprintf(j, "    \"drrDb\": %.2f,\n    \"rippleDb\": %.2f,\n", strength ? m.drr : 0.0, m.ripple);
    if (haveR4) std::fprintf(j, "    \"loudnessChangeDb\": %.3f\n", r4);
    else std::fprintf(j, "    \"loudnessChangeDb\": null\n");
    std::fprintf(j, "  }\n}\n");
    std::fclose(j);
    std::printf("%s.wav written (%zu clipped samples)\n", base.c_str(), clipped);
    return 0;
}
