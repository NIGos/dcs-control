// DCS Control: the two shared-memory blocks between the pupil shift layer and the DCS engine module (native/dcsqvcull),
// for Depth boost's fixation depth (docs/FIXATION_DEPTH.md, "Interface").
//
//   Local\DcsVrPerceptionGaze_v1   written by the pupil shift layer at each xrLocateViews, read by the engine module:
//                                  the gaze of a display time and the FOV DCS renders each view with.
//   Local\DcsVrFixationDepth_v1    written by the engine module once per rendered frame, read by the layer: a summary
//                                  of DCS's depth buffer in a cone around that gaze.
//   Local\DcsVrDepthGrid_v1        written by the engine module (with [Perception] DepthGrid=1), read by the layer: a
//                                  coarse 32 x 32 grid of DCS's depth over a whole view, for Depth boost's predictive
//                                  transition (the landing point of a saccade, before the eyes get there).
//   Local\DcsVrDepthBoost_v1       written by the pupil shift layer at each xrLocateViews (Depth boost on), read by the
//                                  engine module: the eye distance DCS renders that display time with and the true one,
//                                  so the engine can render the cockpit at the true distance (its dual camera option).
//   Local\DcsVrCockpitStereo_v1    written by the engine module every rendered frame while its dual camera runs, read by
//                                  the layer: while fresh and active, the cockpit stays at the true eye distance whatever
//                                  the scale, so Depth boost keeps one world scale and changes it only slowly.
//
// All of them are named file mappings in the DCS process's session, little-endian, #pragma pack(8). Either side may create
// either mapping (CreateFileMapping opens an existing one); a block that was never written reads as all zero, which
// is "no data" (seq 0, flags 0). A seqlock guards each block against torn reads: seq is odd while the writer writes and
// even when it is done; a reader copies the block between two reads of seq and keeps the copy only when both are the
// same even value. There is one writer per block.
//
// View order (both blocks), as in XR_VARJO_quad_views: 0 left periphery (primary), 1 right periphery (primary),
// 2 left focus, 3 right focus. With two views (no quad views) only 0 and 1 exist.
//
// The names take an optional suffix from the environment variable DCSVR_PERCEPTION_SHM_SUFFIX (read once by the layer):
// the offline harness sets it so a test run never touches the blocks of a DCS that is running at the same time.
//
// This header is shared: the engine module may include it as it is (no dependency besides <atomic> and <cstring>; the
// mapping helper at the end needs <windows.h> included before it).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dcsvr_perception {

constexpr const wchar_t* gaze_block_name = L"Local\\DcsVrPerceptionGaze_v1";
constexpr const wchar_t* depth_block_name = L"Local\\DcsVrFixationDepth_v1";
constexpr const wchar_t* grid_block_name = L"Local\\DcsVrDepthGrid_v1";
constexpr const wchar_t* boost_block_name = L"Local\\DcsVrDepthBoost_v1";
constexpr const wchar_t* cockpit_stereo_block_name = L"Local\\DcsVrCockpitStereo_v1";
constexpr const char* name_suffix_variable = "DCSVR_PERCEPTION_SHM_SUFFIX";

#pragma pack(push, 8)
// Written by the pupil shift layer at each xrLocateViews (once per new display time).
struct GazeOut {
    std::uint64_t seq;            // even = stable
    std::int64_t displayTime;     // XrTime of the located views
    std::uint32_t flags;          // gaze_flags below
    std::uint32_t viewCount;      // 2 or 4
    float gazeTan[4][2];          // per view: gaze as tangents (x right, y up) in that view's own frame
    float fovTan[4][4];           // per view: left, right, up, down tangents of the FOV DCS renders with (after pupil shift)
    float vergenceM;              // binocular estimate, metres; <= 0 unknown
};

// Written by the engine module once per rendered frame.
struct DepthIn {
    std::uint64_t seq;            // even = stable
    std::int64_t displayTime;     // the GazeOut.displayTime the samples were taken for (or the newest one read)
    std::uint32_t flags;          // depth_flags below
    std::uint32_t view;           // which view was sampled
    float coneDeg;                // radius actually used
    float nearShare, farShare, edgeNearShare, edgeFarShare;  // 0..1, cone and 1.5 degree ring
    float medianM, nearestM, farMedianM;                     // metres
    std::uint16_t histogram[16];  // log-distance bins, 0.2 m .. 20 km
    float latencyFrames;          // frames between the depth read and its publication
};

// Written by the engine module (DepthGrid=1), about once per rendered frame, about 2 frames after the depth was read.
// Cell (col, row) covers the tangents x in [left + col * (right - left) / w, ...) from left to right and y from up
// (row 0) down to down (row h - 1), in the sampled view's own frame; cell index row * w + col.
struct DepthGrid {
    std::uint64_t seq;            // even = stable
    std::int64_t displayTime;     // the GazeOut.displayTime of the frame the depth was read from (or the newest one read)
    std::uint32_t flags;          // grid_flags below
    std::uint32_t view;           // which view was sampled (0: left periphery, the whole field)
    std::uint32_t w, h;           // 32 x 32 (w * h <= 1024)
    float fovTan[4];              // left, right, up, down tangents of the part of the view the grid covers
    float nearestM[1024];         // per cell: the nearest distance, metres
    float typicalM[1024];         // per cell: the log mean (geometric) of its distances, metres; 0: empty
    std::uint8_t nearShare[1024]; // per cell: the share of the cell nearer than 1.5 m, 0..255 = 0..1
};
// Written by the pupil shift layer at each xrLocateViews (once per new display time) while Depth boost is enabled.
// The layer moved each eye of the located views along the eye-to-eye axis, symmetrically about their midpoint, from
// baseIpdM apart to renderedIpdM apart (scale = renderedIpdM / baseIpdM): the left eye by (renderedIpdM - baseIpdM) / 2
// towards its own left, the right eye as much towards its right, in every view of that eye (periphery and focus). An
// engine that renders the cockpit with the true eye distance moves each eye's cockpit camera back by that much along
// its camera's right axis (the left eye's to the right, the right eye's to the left).
struct BoostOut {
    std::uint64_t seq;            // even = stable
    std::int64_t displayTime;     // XrTime of the located views the distance was applied to
    std::uint32_t flags;          // boost_flags below
    std::uint32_t frame;          // the layer's located-frame counter (increments per new display time)
    float scale;                  // renderedIpdM / baseIpdM (1: nothing applied)
    float baseIpdM;               // the true eye distance (DCS's custom IPD if set, else the runtime's)
    float renderedIpdM;           // the eye distance of the views returned to DCS (before any pupil shift)
    float reserved[5];            // 0
};
// Written by the engine module once per rendered frame while its dual camera option is on (CockpitTrueIpd). The layer
// takes it as acknowledged while qpc is less than 200 ms old (QueryPerformanceCounter, the same clock in both) and flags
// has active: then a one-frame mis-pairing of a BoostOut record must stay invisible, so Depth boost holds one world
// scale (its maximum) and moves towards it at DualCameraMmPerS, with no cockpit-return steps (the cockpit needs none).
struct CockpitStereo {
    std::uint64_t seq;            // even = stable
    std::uint32_t flags;          // cockpit_stereo_flags below
    std::uint32_t reserved;       // 0
    std::uint64_t engineFrame;    // the engine's frame counter
    std::int64_t qpc;             // QueryPerformanceCounter at the write
};
#pragma pack(pop)

static_assert(offsetof(CockpitStereo, flags) == 8 && offsetof(CockpitStereo, engineFrame) == 16 && offsetof(CockpitStereo, qpc) == 24 &&
              sizeof(CockpitStereo) == 32, "CockpitStereo layout");
static_assert(offsetof(BoostOut, displayTime) == 8 && offsetof(BoostOut, flags) == 16 && offsetof(BoostOut, frame) == 20 &&
              offsetof(BoostOut, scale) == 24 && offsetof(BoostOut, baseIpdM) == 28 && offsetof(BoostOut, renderedIpdM) == 32 &&
              sizeof(BoostOut) == 56, "BoostOut layout");
static_assert(offsetof(GazeOut, displayTime) == 8 && offsetof(GazeOut, flags) == 16 && offsetof(GazeOut, viewCount) == 20 &&
              offsetof(GazeOut, gazeTan) == 24 && offsetof(GazeOut, fovTan) == 56 && offsetof(GazeOut, vergenceM) == 120 &&
              sizeof(GazeOut) == 128, "GazeOut layout");
static_assert(offsetof(DepthIn, displayTime) == 8 && offsetof(DepthIn, flags) == 16 && offsetof(DepthIn, view) == 20 &&
              offsetof(DepthIn, coneDeg) == 24 && offsetof(DepthIn, nearShare) == 28 && offsetof(DepthIn, edgeFarShare) == 40 &&
              offsetof(DepthIn, medianM) == 44 && offsetof(DepthIn, farMedianM) == 52 && offsetof(DepthIn, histogram) == 56 &&
              offsetof(DepthIn, latencyFrames) == 88 && sizeof(DepthIn) == 96, "DepthIn layout");
static_assert(offsetof(DepthGrid, displayTime) == 8 && offsetof(DepthGrid, flags) == 16 && offsetof(DepthGrid, view) == 20 &&
              offsetof(DepthGrid, w) == 24 && offsetof(DepthGrid, h) == 28 && offsetof(DepthGrid, fovTan) == 32 &&
              offsetof(DepthGrid, nearestM) == 48 && offsetof(DepthGrid, typicalM) == 4144 && offsetof(DepthGrid, nearShare) == 8240 &&
              sizeof(DepthGrid) == 9264, "DepthGrid layout");
constexpr int grid_cells = 1024;

namespace gaze_flags {
constexpr std::uint32_t valid = 1u;      // a real gaze (or SimulateGaze); without it gazeTan is straight ahead
constexpr std::uint32_t saccade = 2u;    // the eyes are in a saccade
constexpr std::uint32_t per_eye = 4u;    // each eye's own gaze (vergence available, vergenceM may be set)
}  // namespace gaze_flags
namespace depth_flags {
constexpr std::uint32_t valid = 1u;      // the summary is usable
constexpr std::uint32_t reverse_z = 2u;  // reverse-Z detected
constexpr std::uint32_t focus_view = 4u; // the view sampled was a focus view
constexpr std::uint32_t water = 8u;       // water in the cone (from the stencil)
constexpr std::uint32_t after_water = 16u; // depth read after the water passes (water included in the distances)
constexpr std::uint32_t periphery = 32u;  // the gaze cone left the focus view: the same eye's periphery was sampled
}  // namespace depth_flags
namespace cockpit_stereo_flags {
constexpr std::uint32_t active = 1u;      // the dual camera renders the cockpit at the true eye distance (not paused)
constexpr std::uint32_t observing = 2u;   // observing only (CockpitTrueIpd=2): nothing corrected, the layer ignores it
}  // namespace cockpit_stereo_flags
namespace boost_flags {
constexpr std::uint32_t enabled = 1u;     // Depth boost enabled and not switched off in flight
constexpr std::uint32_t applied = 2u;     // the views of displayTime carry renderedIpdM (scale != 1)
}  // namespace boost_flags
namespace grid_flags {
constexpr std::uint32_t valid = 1u;       // the grid is usable
constexpr std::uint32_t reverse_z = 2u;   // reverse-Z detected
constexpr std::uint32_t water = 8u;       // water in the view (from the stencil)
constexpr std::uint32_t after_water = 16u; // depth read after the water passes
}  // namespace grid_flags

// Log-distance histogram bins: bin k covers [0.2 m * r^k, 0.2 m * r^(k+1)) with r = (20 km / 0.2 m)^(1/16).
constexpr float histogram_min_m = 0.2f, histogram_max_m = 20000.0f;
constexpr int histogram_bins = 16;

// ---- Seqlock ----------------------------------------------------------------------------------------------------------
// The block lives in shared memory; seq is accessed as a std::atomic<uint64_t> placed over it (lock-free, the same size
// and alignment as uint64_t on every Windows target).
static_assert(sizeof(std::atomic<std::uint64_t>) == 8 && std::atomic<std::uint64_t>::is_always_lock_free, "seq");

template <typename Block> std::atomic<std::uint64_t>& seq_of(Block* block) {
    return *reinterpret_cast<std::atomic<std::uint64_t>*>(&block->seq);
}

// The single writer: odd, data, even. value.seq is ignored.
template <typename Block> void seqlock_write(Block* block, const Block& value) {
    auto& seq = seq_of(block);
    const std::uint64_t start = seq.load(std::memory_order_relaxed) | 1u;  // odd (also repairs a writer that died mid-write)
    seq.store(start, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    constexpr std::size_t offset = sizeof(std::uint64_t);
    std::memcpy(reinterpret_cast<unsigned char*>(block) + offset, reinterpret_cast<const unsigned char*>(&value) + offset, sizeof(Block) - offset);
    std::atomic_thread_fence(std::memory_order_release);
    seq.store(start + 1, std::memory_order_release);
}

// A reader: true with a consistent copy (out.seq = its even sequence number), false while the writer is busy (after
// `attempts` tries) or when the block was never written (seq 0).
template <typename Block> bool seqlock_read(const Block* block, Block& out, int attempts = 4) {
    auto& seq = seq_of(const_cast<Block*>(block));
    for (int i = 0; i < attempts; ++i) {
        const std::uint64_t before = seq.load(std::memory_order_acquire);
        if (before == 0) return false;
        if (before & 1u) continue;
        std::memcpy(&out, block, sizeof(Block));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_relaxed) == before) { out.seq = before; return true; }
    }
    return false;
}

#ifdef _WINDOWS_
// A named mapping of one block (created if it does not exist yet). Never unmapped while the process runs: the other
// side may still read it.
template <typename Block> class SharedBlock {
public:
    bool open(const wchar_t* base_name, const wchar_t* suffix) {
        if (view_) return true;
        wchar_t name[160]{};
        wcscpy_s(name, base_name);
        if (suffix && suffix[0]) wcscat_s(name, suffix);
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(Block)), name);
        if (!mapping_) return false;
        view_ = static_cast<Block*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Block)));
        if (!view_) { CloseHandle(mapping_); mapping_ = nullptr; return false; }
        return true;
    }
    void close() {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        view_ = nullptr; mapping_ = nullptr;
    }
    Block* get() const { return view_; }
private:
    HANDLE mapping_{};
    Block* view_{};
};
#endif

}  // namespace dcsvr_perception
