// DCS Control binaural audio: the compact HRTF file format (".dcshrtf"), version 1.
//
// One format for every HRTF the audio module can load: the bundled generic set (SADIE II KU100, embedded in the engine
// payload) and the pilot's own SOFA files, converted by the app (docs/BINAURAL_AUDIO.md, "HRTF data"). The DCS
// process never parses SOFA/HDF5: it reads only this format, validates it with Validate() below and resamples it to
// the device rate on a control thread.
//
// Content (all little-endian, no padding between sections):
//   Header         256 bytes
//   Direction[n]   unit vector of each measurement, in the head frame (below), 16 bytes each
//   Delay[n]       onset delay of each ear, in samples at Header.sampleRate (fractional), 8 bytes each
//   Triangle[t]    the measurement sphere's triangulation (convex hull of the directions), indices into Direction,
//                  counter-clockwise seen from outside, 12 bytes each
//   ir             float[n][2][taps]: minimum-phase impulse responses, ear 0 = left, 1 = right, onset delay removed
//
// Head frame = DCS's listener frame (docs/BINAURAL_AUDIO_HOOKS.md section 7): x forward, y up, z right, metres.
// From SOFA spherical coordinates (azimuth degrees counter-clockwise from the front, i.e. positive to the left;
// elevation degrees up):  x = cos(el) cos(az),  y = sin(el),  z = -cos(el) sin(az).
//
// The converter (C#, src/DcsVr.Core, task A) produces:
//   - minimum-phase IRs (real cepstrum), truncated to `taps` with a half-Hann tail window;
//   - onset delays from the original HRIRs: the lag that best aligns each HRIR with its own minimum-phase version
//     (cross-correlation upsampled 8x; Nam, Abel and Smith 2008), plus SOFA Data.Delay, the common minimum removed; the
//     interaural time difference is delayRight - delayLeft. (A -20 dB level threshold was the first plan: band-limited
//     onsets ring before the threshold and shadowed ears rise late: 38 us of ITD error on a synthetic set vs < 1 us.)
//   - diffuse-field equalisation (flag kDiffuseFieldEq): every IR divided by the power average over all directions
//     (weighted by each direction's share of the sphere), smoothed to 1/3 octave, centred on its 200 Hz-10 kHz mean,
//     limited to +-15 dB;
//   - triangulation: the convex hull of the directions (repeats within 0.05 degrees merged), t = 2n - 4;
//   - taps: 128 at 48 kHz, scaled with the rate (120 at 44.1 kHz, 256 at 96 kHz);
//   - front normalisation (flag kFrontNormalised): one gain for the whole set so that white noise from the front
//     (x = 1) comes out at 0 dB per ear (energy of the two front IRs averaged = 1). Header.frontGain records it.
//   - datasetHeadRadiusM: least-squares fit of Woodworth's ITD(phi) = (r / c)(phi + sin phi), c = 343 m/s, phi the
//     lateral angle, over all directions; the renderer scales ITDs by (pilot radius / this radius).
//
// Checksums: FNV-1a 64 of the payload (everything after the header) and of the header with headerFnv1a set to zero.
// Header-only, C++17, no dependencies besides the standard library.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hrtf_format {

constexpr std::uint32_t kMagic = 0x54524844u;  // bytes "DHRT"
constexpr std::uint16_t kVersion = 1;
constexpr std::uint16_t kHeaderBytes = 256;
constexpr const char* kFileExtension = ".dcshrtf";

constexpr std::uint32_t kMinSampleRate = 22050, kMaxSampleRate = 192000;
constexpr std::uint32_t kMinDirections = 6, kMaxDirections = 16384;
constexpr std::uint16_t kMinTaps = 16, kMaxTaps = 512;
constexpr std::uint64_t kMaxFileBytes = 64ull << 20;

namespace flags {
constexpr std::uint32_t kMinimumPhase = 1u;     // required in version 1
constexpr std::uint32_t kDiffuseFieldEq = 2u;
constexpr std::uint32_t kFrontNormalised = 4u;
constexpr std::uint32_t kPersonal = 8u;         // converted from the pilot's own SOFA file (informational)
constexpr std::uint32_t kKnown = kMinimumPhase | kDiffuseFieldEq | kFrontNormalised | kPersonal;
}  // namespace flags

#pragma pack(push, 8)
struct Header {
    std::uint32_t magic;               // kMagic
    std::uint16_t version;             // kVersion
    std::uint16_t headerBytes;         // kHeaderBytes
    std::uint32_t flags;               // flags:: above
    std::uint32_t sampleRate;          // of the IRs and the delays, Hz
    std::uint32_t directionCount;      // n
    std::uint32_t triangleCount;       // t (a closed convex hull: t = 2n - 4)
    std::uint16_t taps;                // IR length
    std::uint16_t reserved0;           // 0
    float datasetHeadRadiusM;          // Woodworth fit of the delays, metres (0.06 .. 0.12)
    float measurementDistanceM;        // source distance of the measurements, metres (SOFA SourcePosition r)
    float frontGain;                   // the front normalisation gain applied (1 when none), informational
    std::uint64_t payloadBytes;        // bytes after the header = PayloadBytes(n, t, taps)
    std::uint64_t payloadFnv1a;        // FNV-1a 64 of those bytes
    std::uint64_t headerFnv1a;         // FNV-1a 64 of this header with this field zero
    char name[64];                     // UTF-8, NUL-terminated, shown in the app ("SADIE II KU100")
    char source[96];                   // UTF-8, NUL-terminated: dataset, version, licence ("SADIE II v2-2, Apache-2.0")
    std::uint8_t reserved[32];         // 0
};
struct Direction {
    float x, y, z;                     // unit vector, head frame
    float reserved;                    // 0
};
struct Delay {
    float left, right;                 // onset delay in samples at sampleRate, >= 0
};
struct Triangle {
    std::uint32_t a, b, c;             // indices into Direction, counter-clockwise seen from outside
};
#pragma pack(pop)

static_assert(sizeof(Header) == 256, "Header size");
static_assert(offsetof(Header, flags) == 8 && offsetof(Header, sampleRate) == 12 && offsetof(Header, directionCount) == 16 &&
                  offsetof(Header, triangleCount) == 20 && offsetof(Header, taps) == 24 &&
                  offsetof(Header, datasetHeadRadiusM) == 28 && offsetof(Header, measurementDistanceM) == 32 &&
                  offsetof(Header, frontGain) == 36 && offsetof(Header, payloadBytes) == 40 &&
                  offsetof(Header, payloadFnv1a) == 48 && offsetof(Header, headerFnv1a) == 56 && offsetof(Header, name) == 64 &&
                  offsetof(Header, source) == 128 && offsetof(Header, reserved) == 224,
              "Header layout");
static_assert(sizeof(Direction) == 16 && sizeof(Delay) == 8 && sizeof(Triangle) == 12, "record sizes");

inline std::uint64_t Fnv1a(const void* data, std::size_t bytes, std::uint64_t h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

inline std::uint64_t PayloadBytes(std::uint32_t directions, std::uint32_t triangles, std::uint32_t taps) {
    return std::uint64_t(directions) * (sizeof(Direction) + sizeof(Delay)) + std::uint64_t(triangles) * sizeof(Triangle) +
           std::uint64_t(directions) * 2u * taps * sizeof(float);
}

inline std::uint64_t HeaderChecksum(const Header& h) {
    Header copy;
    std::memcpy(&copy, &h, sizeof(copy));
    copy.headerFnv1a = 0;
    return Fnv1a(&copy, sizeof(copy));
}

// Pointers into a validated file (valid as long as its memory is).
struct View {
    const Header* header = nullptr;
    const Direction* directions = nullptr;
    const Delay* delays = nullptr;
    const Triangle* triangles = nullptr;
    const float* ir = nullptr;  // ir[(d * 2 + ear) * taps + k]
    const float* Ir(std::uint32_t direction, int ear) const {
        return ir + (std::size_t(direction) * 2u + static_cast<std::size_t>(ear)) * header->taps;
    }
};

namespace detail {
inline bool Finite(float v) { return std::isfinite(v); }
inline bool HasNul(const char* s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (s[i] == 0) return true;
    return false;
}
}  // namespace detail

// Checks everything a reader relies on. `data` must be 4-byte aligned (any heap allocation or resource is). On
// failure returns false and `why` names the first problem (a static string, for the log). Cost: one pass over the file.
inline bool Validate(const void* data, std::size_t bytes, View* out, const char** why) {
    const char* dummy = nullptr;
    if (!why) why = &dummy;
    *why = nullptr;
    if (!data || bytes < sizeof(Header)) return *why = "too small", false;
    if ((reinterpret_cast<std::uintptr_t>(data) & 3u) != 0) return *why = "not 4-byte aligned", false;
    if (bytes > kMaxFileBytes) return *why = "too large", false;
    Header h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic) return *why = "not an HRTF file (magic)", false;
    if (h.version != kVersion) return *why = "unsupported version", false;
    if (h.headerBytes != kHeaderBytes) return *why = "header size", false;
    if (HeaderChecksum(h) != h.headerFnv1a) return *why = "header checksum", false;
    if ((h.flags & ~flags::kKnown) != 0) return *why = "unknown flags", false;
    if (!(h.flags & flags::kMinimumPhase)) return *why = "not minimum phase", false;
    if (h.sampleRate < kMinSampleRate || h.sampleRate > kMaxSampleRate) return *why = "sample rate", false;
    if (h.directionCount < kMinDirections || h.directionCount > kMaxDirections) return *why = "direction count", false;
    if (h.triangleCount < 4 || h.triangleCount > 2u * h.directionCount) return *why = "triangle count", false;
    if (h.taps < kMinTaps || h.taps > kMaxTaps) return *why = "tap count", false;
    if (!(h.datasetHeadRadiusM >= 0.04f && h.datasetHeadRadiusM <= 0.15f)) return *why = "head radius", false;
    if (!(h.measurementDistanceM >= 0.1f && h.measurementDistanceM <= 10.0f)) return *why = "measurement distance", false;
    if (!(h.frontGain > 0.0f) || !detail::Finite(h.frontGain)) return *why = "front gain", false;
    if (!detail::HasNul(h.name, sizeof(h.name)) || !detail::HasNul(h.source, sizeof(h.source))) return *why = "strings", false;
    if (h.reserved0 != 0) return *why = "reserved field", false;
    if (h.payloadBytes != PayloadBytes(h.directionCount, h.triangleCount, h.taps)) return *why = "payload size field", false;
    if (bytes != sizeof(Header) + h.payloadBytes) return *why = "file size", false;
    const auto* base = static_cast<const unsigned char*>(data);
    if (Fnv1a(base + sizeof(Header), static_cast<std::size_t>(h.payloadBytes)) != h.payloadFnv1a) return *why = "payload checksum", false;

    View v;
    v.header = static_cast<const Header*>(data);
    v.directions = reinterpret_cast<const Direction*>(base + sizeof(Header));
    v.delays = reinterpret_cast<const Delay*>(v.directions + h.directionCount);
    v.triangles = reinterpret_cast<const Triangle*>(v.delays + h.directionCount);
    v.ir = reinterpret_cast<const float*>(v.triangles + h.triangleCount);

    const float maxDelay = 0.01f * static_cast<float>(h.sampleRate);  // 10 ms
    for (std::uint32_t i = 0; i < h.directionCount; ++i) {
        const Direction& d = v.directions[i];
        if (!detail::Finite(d.x) || !detail::Finite(d.y) || !detail::Finite(d.z)) return *why = "direction not finite", false;
        const float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
        if (std::fabs(len2 - 1.0f) > 1e-3f) return *why = "direction not unit", false;
        const Delay& dl = v.delays[i];
        if (!(dl.left >= 0.0f && dl.left <= maxDelay && dl.right >= 0.0f && dl.right <= maxDelay)) return *why = "delay", false;
    }
    for (std::uint32_t i = 0; i < h.triangleCount; ++i) {
        const Triangle& t = v.triangles[i];
        if (t.a >= h.directionCount || t.b >= h.directionCount || t.c >= h.directionCount || t.a == t.b || t.b == t.c ||
            t.a == t.c)
            return *why = "triangle index", false;
    }
    const std::size_t irCount = std::size_t(h.directionCount) * 2u * h.taps;
    for (std::size_t i = 0; i < irCount; ++i)
        if (!detail::Finite(v.ir[i]) || std::fabs(v.ir[i]) > 64.0f) return *why = "impulse response value", false;
    if (out) *out = v;
    return true;
}

}  // namespace hrtf_format
