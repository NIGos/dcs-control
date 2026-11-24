// DCS Control binaural audio: BinauralCore, the HRTF renderer (task A in docs/BINAURAL_AUDIO.md).
//
// A static library with no DCS dependency. The DCS audio module (task B) feeds it one mono source at a time from inside
// ed_sound's render callback (docs/BINAURAL_AUDIO_HOOKS.md section 5.2); the offline harness feeds it the same way.
//
// DSP (why: docs/BINAURAL_AUDIO.md, "DSP approach"):
//   - HRTF set: minimum-phase FIRs + per-ear onset delays (hrtf_format.h), resampled to the device rate at load.
//   - Direction lookup: barycentric interpolation on the measurement sphere's triangulation. FIRs are interpolated
//     linearly (minimum phase makes that safe, no comb filtering), delays separately.
//   - ITD: per-ear fractional delay line after the FIR (24-tap Kaiser-windowed sinc, 256 phases interpolated; flat
//     within 0.02 dB to 0.42 x the rate; 12 samples of look-ahead = the renderer's only added latency), the delay
//     changing per sample; delays scaled about their mean by headRadius / datasetHeadRadius, so head size changes the
//     ITD only. The FIR runs first so both ears (and both filters of a crossfade) share one pass over the mono input.
//   - Near field (NearField on, source < 1 m): per-ear parallax (direction from each ear, ears at +-r on the z axis) for
//     sources under 2 m (fading out between 1.5 and 2 m; the ear offset shrinks to 0 between 2 and 1 head radius, so
//     a source at the head never flips the near ear's filter), plus a first-order low shelf per ear (1 kHz corner,
//     Brungart-style) that adds up to 12 dB of ILD at 0.25 m, 90 degrees (+8 dB near ear, -4 dB far ear).
//   - FIR: direct form, AVX2/FMA with an SSE2 fallback (chosen at Init); any block length, no FFT framing.
//   - Distance gain, doppler, propagation delay, air and canopy filtering: never applied here (DCS already did them).
//
// Units: positions in metres in the head frame = DCS's listener frame (x forward, y up, z right); gains linear.
//
// Threads:
//   - Control thread(s): Init, LoadHrtf, ReleaseHrtf, the Set* calls, TakeRetiredHrtf, GetStats. They may allocate.
//   - Audio thread (one at a time per Renderer): BeginBlock, Render, ResetSource, EndBlock. These never allocate,
//     lock, log, touch files or make system calls besides QueryPerformanceCounter; their work is bounded by
//     frames x sources.
//   - Set* calls publish atomically; the audio thread picks them up at the next BeginBlock and ramps to them inside
//     that block (no step, no click).
// Real-time guards (audio thread): BeginBlock saves MXCSR and sets FTZ|DAZ, EndBlock restores it (ED's kernels keep
// their own mode). A non-finite value in a source's input or state resets that source and adds nothing for that call
// (counted in Stats::nonFinite). Output is never clamped here: DCS's limiter runs after us.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace binaural {

constexpr std::uint32_t kMinSampleRate = 22050;
constexpr std::uint32_t kMaxSampleRate = 96000;
constexpr std::uint32_t kMaxSources = 64;            // render slots per Renderer
constexpr std::uint32_t kMaxFramesPerCall = 8192;    // longer calls are rejected (Render returns false, adds nothing)
constexpr std::uint32_t kSubBlockFrames = 256;       // parameter interpolation step inside a call
constexpr float kMaxDelayMs = 4.0f;                  // onset + ITD + near-field parallax, per ear
constexpr float kNearFieldM = 1.0f;                  // ILD correction below this distance
constexpr float kParallaxM = 2.0f;                   // per-ear directions below this distance
constexpr float kMinDistanceM = 0.05f;               // positions are clamped to this distance (head-locked below)
constexpr float kDefaultHeadRadiusM = 0.0875f;       // used when Config.headRadiusM is 0

// An HRTF set validated (hrtf_format::Validate) and resampled to one device rate. Immutable once built; shared by
// any number of Renderers at that rate.
struct HrtfSet;

struct HrtfInfo {
    std::uint32_t directions;
    std::uint32_t taps;                 // at the device rate
    std::uint32_t sampleRate;           // the device rate it was built for
    float datasetHeadRadiusM;
    char name[64];
};

// Control thread. Validates `data` (a .dcshrtf image in memory, 4-byte aligned), resamples it to `deviceRate` and
// builds the lookup structures. Returns null on failure with a short reason in `error` (NUL-terminated).
HrtfSet* LoadHrtf(const void* data, std::size_t bytes, std::uint32_t deviceRate, char* error, std::size_t errorCapacity);
// Control thread. Frees a set that no Renderer references any more (see Renderer::TakeRetiredHrtf).
void ReleaseHrtf(HrtfSet* set);
HrtfInfo DescribeHrtf(const HrtfSet* set);

struct Config {
    std::uint32_t sampleRate = 48000;    // the device rate (kMinSampleRate .. kMaxSampleRate)
    std::uint32_t maxSources = 48;       // 1 .. kMaxSources; slots 0 .. maxSources-1
    float headRadiusM = 0.0f;            // 0: kDefaultHeadRadiusM
    bool nearField = true;
};

// Per call, per source: the state at the END of this call. The renderer interpolates from the previous call's values
// (kept per slot) across the call, every kSubBlockFrames, so a direction change inside a block is a smooth sweep.
struct SourceUpdate {
    float position[3];                   // head frame, metres (x forward, y up, z right)
    float gain;                          // linear, ramped across the call (level trim and late-translation correction)
    std::uint32_t flags;                 // source_flags below
};
namespace source_flags {
constexpr std::uint32_t kReset = 1u;     // a new sound in this slot: no interpolation from the previous state,
                                         // delay lines and filter history cleared
constexpr std::uint32_t kNoNearField = 2u;  // skip parallax and the ILD shelf for this source
}  // namespace source_flags

// The mono signal of one source: mono[i] = scale * (a[i * stride] + (b ? b[i * stride] : 0)).
// The audio module passes its interleaved stereo scratch: a = &scratch[0], stride 2, b null (left channel), or for the
// ownership fade-in/out a = &scratch[0], b = &scratch[1], scale 0.5.
struct MonoInput {
    const float* a;
    const float* b;
    std::uint32_t stride;
    float scale;
};

// Lock-free counters, read by the control thread (GetStats); written only by the audio thread.
struct Stats {
    std::uint64_t blocks;
    std::uint64_t renders;               // Render calls that added output
    std::uint64_t filterCrossfades;      // Render calls that ran two filter sets (direction moved past the threshold)
    std::uint64_t hrtfSwaps;             // per-source crossfades to a new HRTF set
    std::uint64_t nonFinite;             // sources reset for a NaN/Inf
    std::uint64_t rejected;              // Render calls refused (frames > kMaxFramesPerCall, slot out of range, no HRTF)
    std::uint64_t lastBlockNs;           // time between BeginBlock and EndBlock of the last block
    std::uint64_t worstBlockNs;          // since the last ResetStats
    std::uint32_t blockNsHistogram[16];  // log2 buckets of block time in microseconds: [0] < 2 us ... [15] >= 32768 us
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Control thread. Allocates every buffer the audio thread will use (delay lines, filter histories, two filter
    // sets per slot for crossfades, scratch for kMaxFramesPerCall). False with a reason on bad input or no memory.
    bool Init(const Config& config, char* error, std::size_t errorCapacity);

    // Control thread, any time after Init. The new set must be built for this Renderer's sampleRate. Each active
    // source crossfades from the old set to the new one over its next Render call. The old set becomes "retired"
    // once the audio thread no longer uses it; the control thread frees it after TakeRetiredHrtf.
    void SetHrtf(const HrtfSet* set);
    const HrtfSet* TakeRetiredHrtf();    // null when nothing is retired yet

    void SetHeadRadius(float metres);    // 0.06 .. 0.12; ITDs ramp to the new scale over the next call per source
    void SetNearField(bool on);

    // ---- audio thread ---------------------------------------------------------------------------------------------
    // Opens a device block of `frames` frames (bookkeeping, FTZ/DAZ, timing). Render calls must be inside a block.
    void BeginBlock(std::uint32_t frames);
    // Renders one source into `outLR` (interleaved stereo, `frames` frames), ADDING to it:
    //   outLR[2i]   += w(i) * left(i),  outLR[2i+1] += w(i) * right(i),
    // with w(i) linear from w0 at i = 0 to w1 at i = frames (the ownership fade; 1, 1 for a plain call).
    // Crossfade rules:
    //   - direction/position: interpolated per kSubBlockFrames from the previous call's state; when an ear's filter
    //     direction moves more than 0.5 degrees of arc from the one its current filter was built for, the new filter
    //     runs over the sub-block and the old one over its first 64 frames, crossfaded linearly over those 64 frames
    //     (barycentric weights are continuous across triangle edges, so a triangle change alone needs no crossfade);
    //   - ITD: the fractional delays change per sample (no step), at most 1 sample per 16 output samples (faster
    //     changes are spread over later calls); a change of at most 0.1 sample within a sub-block is rendered as a
    //     per-sample blend of the delays at its two ends (within 0.05 dB of exact at 16 kHz);
    //   - gain and the near-field shelf: linear ramps across the call;
    //   - a new HRTF set: full-call crossfade old -> new;
    //   - kReset: no interpolation (a new sound starts where it is).
    // Returns false and adds nothing when the call is rejected (see Stats::rejected).
    bool Render(std::uint32_t slot, const SourceUpdate& update, const MonoInput& input, std::uint32_t frames, float w0,
                float w1, float* outLR);
    // Clears a slot (the voice ended). Bounded, no allocation.
    void ResetSource(std::uint32_t slot);
    void EndBlock();

    // ---- any thread -----------------------------------------------------------------------------------------------
    Stats GetStats() const;              // a consistent-enough copy (each counter read atomically)
    void ResetStats();                   // control thread
    std::uint32_t SampleRate() const;

private:
    struct Impl;
    Impl* impl_;
    friend struct RendererAccess;  // the offline harness's read-only probes (src/internal.h); not an API
};

// CPU budget and back-off (implemented by the audio module, measured with Stats):
//   - budget = CpuBudgetPercent (binaural_config.h, default 25 %) of the block duration, measured by the module around
//     its own work only (the HRTF voices' Render calls and their scratch clears, audio_render.h Block::dspTicks)
//     against frames / sampleRate. Stats::lastBlockNs is not that measure: it runs from BeginBlock to EndBlock, which
//     in DCS spans ED's whole mix including ED's own voices; it is informational;
//   - over budget in more than 10 % of the blocks of the last second: MaxObjects in use drops by 8 (not below 8), the
//     quietest owned voices fade back to DCS's own panning; it climbs back by 4 after 10 s under half the budget;
//   - ED's own overrun counter (docs/BINAURAL_AUDIO_HOOKS.md section 6, item 7) is the hard stop: 3 overruns in 10 s
//     turn binaural off for the session.
// Measured cost (harness A12, AVX2, 48 kHz, 128 taps, 480 frames, thread CPU time): 64 static sources p50 0.21 ms;
// 64 sources with the head turning 90 deg/s p50 0.27 ms, p99 0.31 ms; 48 sources (the default MaxObjects) p50 0.21 ms;
// 64 sources all sweeping ~190 deg/s p99 0.40 ms; SSE2 about 2x. CMake target: binaural_core (include/ is its public
// include path).

}  // namespace binaural
