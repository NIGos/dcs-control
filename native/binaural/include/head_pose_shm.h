// DCS Control: Local\DcsVrHeadPose_v1, the head pose at audio rate for binaural audio (docs/BINAURAL_AUDIO.md).
//
//   Writer: the pupil shift OpenXR layer (native/pupil_shift, task C), at each xrLocateViews with a new display time:
//           xrLocateSpace(VIEW space, info->space, displayTime) with XrSpaceVelocity chained. One entry per display
//           time. Only while PupilShift.ini [HeadPose] Publish=1 (binaural_config.h kPosePublishSection/Key).
//   Reader: the DCS audio module (task B): on the simulation thread once per SND_process (the pose DCS used for the
//           listener, by display time) and on the render thread once per device block (the newest pose, predicted to
//           the block's output time). Both only read.
//
// Same conventions as perception_shm.h: a named file mapping in the DCS process's session, little-endian,
// #pragma pack(8), all-zero means "never written"; the optional name suffix comes from DCSVR_PERCEPTION_SHM_SUFFIX
// (offline tests). Either side may create the mapping. The layer never unmaps it while the process runs.
//
// Layout: a header and a ring of kRing entries. Each entry has its own seqlock (seq odd while written). The writer
// fills ring[(n - 1) % kRing] for its n-th entry (n from 1), sets entry.index = n, then publishes writeCount = n with
// release order. A reader loads writeCount (acquire), copies the entry, and keeps it when its seq was even and unchanged
// and its index is the one it expected (else the writer lapped it: retry or take the next newer one).
//
// Poses are OpenXR: x right, y up, -z forward, metres, in the base space DCS passed to xrLocateViews (baseSpace; a
// change of that handle means a new space: the reader drops older entries). The helpers at the end map them to DCS's
// listener frame (x forward, y up, z right; docs/BINAURAL_AUDIO_HOOKS.md section 7) and compute the late correction.
//
// Header-only, C++17; the mapping helper needs <windows.h> included before this header.
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dcsvr_headpose {

constexpr const wchar_t* block_name = L"Local\\DcsVrHeadPose_v1";
constexpr const char* name_suffix_variable = "DCSVR_PERCEPTION_SHM_SUFFIX";
constexpr std::uint32_t kMagic = 0x50484344u;  // bytes "DCHP"
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kRing = 16;
constexpr double kMaxPredictionS = 0.050;      // readers never extrapolate further than this

namespace entry_flags {
constexpr std::uint32_t orientation_valid = 1u;    // XR_SPACE_LOCATION_ORIENTATION_VALID_BIT
constexpr std::uint32_t position_valid = 2u;       // XR_SPACE_LOCATION_POSITION_VALID_BIT
constexpr std::uint32_t orientation_tracked = 4u;
constexpr std::uint32_t position_tracked = 8u;
constexpr std::uint32_t angular_velocity_valid = 16u;
constexpr std::uint32_t linear_velocity_valid = 32u;
constexpr std::uint32_t from_views = 64u;          // fallback: averaged from the two eye views (VIEW space failed)
}  // namespace entry_flags
namespace writer_flags {
constexpr std::uint32_t qpc_display = 1u;          // entries carry qpcDisplay (XR_KHR_win32_convert_performance_counter_time)
}  // namespace writer_flags

#pragma pack(push, 8)
struct Entry {
    std::uint64_t seq;                 // even = stable
    std::uint64_t index;               // the writer's entry number (1, 2, ...)
    std::int64_t displayTime;          // XrTime located
    std::int64_t qpcLocate;            // QueryPerformanceCounter when the layer located it
    std::int64_t qpcDisplay;           // QPC equivalent of displayTime; 0 when unknown
    std::uint64_t baseSpace;           // XrSpace handle value of the base space
    std::uint32_t flags;               // entry_flags
    std::uint32_t frame;               // the layer's located-frame counter
    float orientation[4];              // x, y, z, w: VIEW in the base space
    float position[3];                 // metres
    float angularVelocity[3];          // rad/s, axes of the base space
    float linearVelocity[3];           // m/s, base space
    float reserved[5];                 // 0
};
struct Block {
    std::uint32_t magic;               // kMagic once the writer initialised it
    std::uint32_t version;             // kVersion
    std::uint64_t writeCount;          // entries published (atomic)
    std::int64_t qpcFrequency;         // QueryPerformanceFrequency
    std::uint32_t writerPid;
    std::uint32_t writerFlags;         // writer_flags
    std::uint64_t reserved[4];         // 0
    Entry ring[kRing];
};
#pragma pack(pop)

static_assert(offsetof(Entry, index) == 8 && offsetof(Entry, displayTime) == 16 && offsetof(Entry, qpcLocate) == 24 &&
                  offsetof(Entry, qpcDisplay) == 32 && offsetof(Entry, baseSpace) == 40 && offsetof(Entry, flags) == 48 &&
                  offsetof(Entry, frame) == 52 && offsetof(Entry, orientation) == 56 && offsetof(Entry, position) == 72 &&
                  offsetof(Entry, angularVelocity) == 84 && offsetof(Entry, linearVelocity) == 96 && sizeof(Entry) == 128,
              "Entry layout");
static_assert(offsetof(Block, writeCount) == 8 && offsetof(Block, qpcFrequency) == 16 && offsetof(Block, writerPid) == 24 &&
                  offsetof(Block, writerFlags) == 28 && offsetof(Block, ring) == 64 && sizeof(Block) == 64 + kRing * 128,
              "Block layout");
static_assert(sizeof(std::atomic<std::uint64_t>) == 8 && std::atomic<std::uint64_t>::is_always_lock_free, "atomics");

inline std::atomic<std::uint64_t>& AtomicU64(const std::uint64_t* p) {
    return *reinterpret_cast<std::atomic<std::uint64_t>*>(const_cast<std::uint64_t*>(p));
}

// ---- writer (one per process) ----------------------------------------------------------------------------------------
inline void InitWriter(Block* b, std::int64_t qpcFrequency, std::uint32_t pid, std::uint32_t writerFlags) {
    b->qpcFrequency = qpcFrequency;
    b->writerPid = pid;
    b->writerFlags = writerFlags;
    b->version = kVersion;
    std::atomic_thread_fence(std::memory_order_release);
    reinterpret_cast<std::atomic<std::uint32_t>*>(&b->magic)->store(kMagic, std::memory_order_release);
}

// Publishes `e` (its seq and index are ignored) as the next entry.
inline void Publish(Block* b, const Entry& e) {
    const std::uint64_t n = AtomicU64(&b->writeCount).load(std::memory_order_relaxed) + 1;
    Entry& slot = b->ring[(n - 1) % kRing];
    auto& seq = AtomicU64(&slot.seq);
    const std::uint64_t start = seq.load(std::memory_order_relaxed) | 1u;
    seq.store(start, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    constexpr std::size_t skip = offsetof(Entry, index);
    std::memcpy(reinterpret_cast<unsigned char*>(&slot) + skip, reinterpret_cast<const unsigned char*>(&e) + skip, sizeof(Entry) - skip);
    slot.index = n;
    std::atomic_thread_fence(std::memory_order_release);
    seq.store(start + 1, std::memory_order_release);
    AtomicU64(&b->writeCount).store(n, std::memory_order_release);
}

// ---- readers ----------------------------------------------------------------------------------------------------------
inline bool Ready(const Block* b) {
    return reinterpret_cast<const std::atomic<std::uint32_t>*>(&b->magic)->load(std::memory_order_acquire) == kMagic &&
           b->version == kVersion;
}

// Copies entry number n (1-based) if it is still in the ring and was not being rewritten.
inline bool ReadIndex(const Block* b, std::uint64_t n, Entry& out) {
    if (n == 0) return false;
    const Entry& slot = b->ring[(n - 1) % kRing];
    auto& seq = AtomicU64(&slot.seq);
    const std::uint64_t before = seq.load(std::memory_order_acquire);
    if (before == 0 || (before & 1u)) return false;
    std::memcpy(&out, &slot, sizeof(Entry));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq.load(std::memory_order_relaxed) != before || out.index != n) return false;
    out.seq = before;
    return true;
}

// The newest complete entry (tries the two newest).
inline bool ReadLatest(const Block* b, Entry& out) {
    if (!Ready(b)) return false;
    const std::uint64_t n = AtomicU64(&b->writeCount).load(std::memory_order_acquire);
    return ReadIndex(b, n, out) || (n > 1 && ReadIndex(b, n - 1, out));
}

// The entry located for `displayTime` (exact match), searching the ring from the newest.
inline bool ReadForDisplayTime(const Block* b, std::int64_t displayTime, Entry& out) {
    if (!Ready(b)) return false;
    const std::uint64_t n = AtomicU64(&b->writeCount).load(std::memory_order_acquire);
    for (std::uint64_t k = 0; k < kRing && k < n; ++k) {
        Entry e;
        if (ReadIndex(b, n - k, e) && e.displayTime == displayTime) {
            out = e;
            return true;
        }
    }
    return false;
}

// ---- math (double precision, plain structs) ---------------------------------------------------------------------------
struct Vec3 {
    double x, y, z;
};
struct Quat {
    double x, y, z, w;  // rotation local -> parent; identity (0, 0, 0, 1)
};

inline Quat Mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat Conj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat Normalize(const Quat& q) {
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-12)) return {0, 0, 0, 1};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}
// v' = q v q* (the same formula as ed_sound's sub_1c60).
inline Vec3 Rotate(const Quat& q, const Vec3& v) {
    const Vec3 c{q.y * v.z - q.z * v.y, q.z * v.x - q.x * v.z, q.x * v.y - q.y * v.x};
    const Vec3 cc{q.y * c.z - q.z * c.y, q.z * c.x - q.x * c.z, q.x * c.y - q.y * c.x};
    return {v.x + 2 * (q.w * c.x + cc.x), v.y + 2 * (q.w * c.y + cc.y), v.z + 2 * (q.w * c.z + cc.z)};
}
inline Quat AxisAngle(const Vec3& axisUnit, double radians) {
    const double s = std::sin(radians * 0.5);
    return {axisUnit.x * s, axisUnit.y * s, axisUnit.z * s, std::cos(radians * 0.5)};
}

// OpenXR (x right, y up, -z forward) -> DCS listener frame (x forward, y up, z right): dcs = M oxr,
// M = [[0,0,-1],[0,1,0],[1,0,0]] (a proper rotation, det +1).
inline Vec3 OxrToDcs(const Vec3& v) { return {-v.z, v.y, v.x}; }
inline Quat OxrToDcs(const Quat& q) { return {-q.z, q.y, q.x, q.w}; }
inline Vec3 DcsToOxr(const Vec3& v) { return {v.z, v.y, -v.x}; }

inline Quat OrientationOf(const Entry& e) {
    return Normalize({e.orientation[0], e.orientation[1], e.orientation[2], e.orientation[3]});
}
inline Vec3 PositionOf(const Entry& e) { return {e.position[0], e.position[1], e.position[2]}; }

// The entry's time in QPC ticks (qpcDisplay when known, else qpcLocate).
inline std::int64_t QpcOf(const Entry& e) { return e.qpcDisplay ? e.qpcDisplay : e.qpcLocate; }

// Orientation extrapolated to `qpcTarget` with the entry's angular velocity (base-space axes, so it multiplies on the
// left), by at most kMaxPredictionS either way. Without a valid angular velocity: the entry's orientation.
inline Quat PredictOrientation(const Entry& e, std::int64_t qpcTarget, std::int64_t qpcFrequency) {
    const Quat q = OrientationOf(e);
    if (!(e.flags & entry_flags::angular_velocity_valid) || qpcFrequency <= 0) return q;
    double dt = static_cast<double>(qpcTarget - QpcOf(e)) / static_cast<double>(qpcFrequency);
    if (dt > kMaxPredictionS) dt = kMaxPredictionS;
    if (dt < -kMaxPredictionS) dt = -kMaxPredictionS;
    const Vec3 w{e.angularVelocity[0], e.angularVelocity[1], e.angularVelocity[2]};
    const double speed = std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z);
    if (!(speed > 1e-9) || !std::isfinite(speed)) return q;
    const Quat dq = AxisAngle({w.x / speed, w.y / speed, w.z / speed}, speed * dt);
    return Normalize(Mul(dq, q));
}

// The late correction for audio, in DCS's listener frame: a point the listener saw at pUsed (DCS frame, when the head
// pose was qUsed/tUsed) is at Rotate(rotation, pUsed) + translation now (head pose qNow/tNow). Poses are OpenXR, in one
// base space. The aircraft's own motion between DCS frames is not included (v1).
struct LateCorrection {
    Quat rotation;
    Vec3 translation;
};
inline LateCorrection ComputeLateCorrection(const Quat& qUsed, const Vec3& tUsed, const Quat& qNow, const Vec3& tNow) {
    const Quat inv = Conj(Normalize(qNow));
    const Quat r = Normalize(Mul(inv, Normalize(qUsed)));
    const Vec3 d{tUsed.x - tNow.x, tUsed.y - tNow.y, tUsed.z - tNow.z};
    return {OxrToDcs(r), OxrToDcs(Rotate(inv, d))};
}
inline Vec3 Apply(const LateCorrection& c, const Vec3& p) {
    const Vec3 r = Rotate(c.rotation, p);
    return {r.x + c.translation.x, r.y + c.translation.y, r.z + c.translation.z};
}

#ifdef _WINDOWS_
// A named mapping of the block (created if missing). Never unmapped while the process runs.
class SharedBlock {
public:
    bool open(const wchar_t* suffix) {
        if (view_) return true;
        wchar_t name[160]{};
        wcscpy_s(name, block_name);
        if (suffix && suffix[0]) wcscat_s(name, suffix);
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(Block)), name);
        if (!mapping_) return false;
        view_ = static_cast<Block*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Block)));
        if (!view_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }
        return true;
    }
    Block* get() const { return view_; }

private:
    HANDLE mapping_{};
    Block* view_{};
};
#endif

}  // namespace dcsvr_headpose
