// DCS Control binaural audio: the cockpit space room (docs/COCKPIT_SPACE.md sections 4 and 8.1).
//
// Early reflections of every owned voice from an aircraft-fixed box around the head, mixed onto 14 aircraft-fixed buses
// that are rendered as far-field HRTF sources (a second core Renderer), plus one shared late tail (8-line feedback delay
// network, two-band decay, frequency-dependent interaural coherence, diffuse-field EQ of the HRTF set). The direct sound
// stays the voice's own Renderer call; the module multiplies its gain by RoomDirectGain so the total loudness holds.
//
// DSP summary (why: docs/COCKPIT_SPACE.md 4):
//   - Per voice: its weighted mono input (MonoInput x send x weight x wet, ramped) goes into a per-slot delay line
//     (12 ms at 96 kHz); 8 taps (6 walls + 2 canopy scatter, each a fixed 0-0.29 ms apart from the geometry) read it at
//     whole-sample delays and are panned (VBAP, energy-preserving, 3 buses) into per-(tap, bus) accumulators, which
//     Render runs through the tap's wall filters (first-order high-pass and low-pass) once for all voices. Gains ramp
//     linearly across the call; a changed delay crossfades between the old and the new read position. No single
//     reflection is louder than the early energy - 3 dB re the direct sound.
//   - Buses: fourth-order high-pass at sendHpHz, then each bus is one far-field HRTF source (kNoNearField) at
//     kRoomBusDistanceM in the direction C x bus, C = RoomFrame::aircraftToHead; the core's own filter crossfade makes a
//     head turn smooth.
//   - Late: the sum of the voices' late sends, pre-delayed and high-passed, feeds the FDN (Hadamard feedback, per-line
//     two-band absorption); two orthogonal outputs x1, x2 give L = x1 + H x2, R = x1 - H x2 with H rising from 0.23 to 1
//     around 650 Hz (interaural coherence about 0.9 low, about 0 above 1 kHz); then 3 biquads per ear fitted to the HRTF
//     set's diffuse-field response.
//   - Levels: tap gains normalised so their energy for a source 0.6 m ahead is earlyDb; the tail's energy for a unit send
//     is lateDb; both relative to the direct sound of the set's average direction (its diffuse-field energy), calibrated
//     for each set in SetHrtf (the late chain at five decay times, the buses against the incoherent model).
//   - Level match (Render with the direct sound): a slow three-band gain on direct + room, measured, so the space is
//     level-neutral for any voice set; RoomDirectGain's per-voice model stays the fast part.
//   - Smoothing: wet (RoomFrame::wet) slews at most 1 per kRoomWetSlewS; early reflections (frameValid && early) fade
//     in/out over kRoomEarlyFadeMs; canopy follows its target with a 0.3 s time constant; SetParams interpolates every
//     parameter from the current values to the new ones over kRoomParamsCrossfadeMs (delays crossfade, FDN gains ramp per
//     sample); an HRTF change crossfades the buses (core, 10 ms) and the diffuse EQ (one block).
//
// Threads: as binaural_core.h. Control thread: Init, SetHrtf, TakeRetiredHrtf, SetHeadRadius, SetParams, ResetStats.
// Audio thread: BeginBlock, AddSource, ResetSource, Render, EndBlock, Idle. BeginBlock sets FTZ|DAZ, EndBlock restores
// the caller's MXCSR. No allocation, lock, logging, I/O or wait on the audio thread; work bounded by
// frames x (sources x taps + buses + FDN lines).
#pragma once

#include <cstddef>
#include <cstdint>

#include "binaural_core.h"
#include "cockpit_space.h"   // Surface, RoomParams, Family, presets (pure)

namespace binaural {

using cockpit_space::RoomParams;

constexpr std::uint32_t kRoomBuses = 14;            // 6 axes + 8 cube corners, aircraft frame
constexpr std::uint32_t kRoomTaps = 8;              // 6 first-order walls + 2 canopy scatter (up-left, up-right)
constexpr std::uint32_t kRoomFdnLines = 8;
constexpr float kRoomMaxTapDelayMs = 12.0f;          // per-voice delay line: 12 ms at 96 kHz (next power of two)
constexpr float kRoomBusDistanceM = 2.0f;            // buses are far-field sources (kNoNearField)
constexpr float kRoomParamsCrossfadeMs = 100.0f;     // SetParams: old set -> new set
constexpr float kRoomRefDistanceM = 0.6f;
constexpr float kRoomWetSlewS = 0.05f;               // RoomFrame::wet 0 -> 1 takes at least this long
constexpr float kRoomEarlyFadeMs = 50.0f;            // early reflections on/off (frameValid, early)
constexpr float kRoomCanopyTauS = 0.3f;              // canopy smoothing time constant
constexpr float kRoomSpeedOfSound = 343.0f;
constexpr std::uint32_t kRoomBusTaps48 = 64;         // the buses' HRTF filters are cut to this (at 48 kHz; scaled)
// Voices with their own early reflections per block: the loudest kRoomOwnTaps (by the last block's send energy); the
// others, once 6 dB under the last of those (back at 3 dB), are summed into one shared tap line excited at their
// energy-weighted mean position (their tails stay their own sends). Each own voice costs a pass over the block per tap.
// Shipped off (every voice its own taps): a lossy option, measured in C17 with kRoomOwnTapsOption.
constexpr std::uint32_t kRoomOwnTaps = kMaxSources;
constexpr std::uint32_t kRoomOwnTapsOption = 16;
constexpr float kRoomTapMinShare = 0.0f;             // a voice's taps under this share of its early energy are left out (0: none; 0.04 measured +-0.74 dB, C17)

namespace room_flags {
constexpr std::uint32_t kReset = 1u;          // a new voice in this slot (tap lines and filter states cleared)
constexpr std::uint32_t kThroughCanopy = 2u;  // outside voice heard through the canopy: excitation on the box, throughDb
constexpr std::uint32_t kLateOnly = 4u;       // diffuse bed: no early taps, late send -3 dB
}  // namespace room_flags

// Per block, set by the audio module at BeginBlock (audio thread). Every value is the target at the block's END;
// the room ramps from the previous block's values across this block.
struct RoomFrame {
    float aircraftToHead[4];   // unit quaternion (x, y, z, w): aircraft frame -> head frame (x fwd, y up, z right)
    bool frameValid;           // false: early reflections fade out (late only), the rotation is ignored
    float wet;                 // 0..1: inside x (module on, kill switch off); strength is in RoomParams
    float canopyOpen;          // 0..1 (0 when unknown or CockpitSpaceCanopy=0)
    bool early;                // false: early reflections fade out (CPU back-off step 1)
};

// Per owned voice per block (audio thread), the target at the block's end.
struct RoomSource {
    float posAircraft[3];      // aircraft frame, head at origin, metres (module: conj(C) applied to the corrected pos)
    float send;                // linear: the voice's HRTF gain (u.gain without RoomDirectGain) x the distance law
    std::uint32_t flags;       // room_flags
    float direct;              // the direct gain d its Render call got (RoomDirectGainMix); 0 (not given) counts as 1.
                               // Weighs the level match (Render with directLR): what Off would have played is the
                               // direct sound divided by the voices' d.
};

// The level match (docs/COCKPIT_SPACE.md 6.2): RoomDirectGain's per-voice energy model is the fast part; the measured,
// slow part keeps direct + room as loud as the direct sound alone would be without d, in three bands, for any voice set
// (reflections that add in phase with the direct sound below 1 kHz, voices carrying the same signal, a low-heavy mix).
constexpr float kRoomMatchTauS = 1.5f;             // energy smoothing of the measure (one pole, per block)
constexpr float kRoomMatchSlewDbPerS = 1.0f;       // the gain moves at most this fast (no pumping)
constexpr float kRoomMatchMaxDb = 2.0f;            // and stays within +-this
constexpr float kRoomMatchLowHz = 200.0f;          // band edges: below, between (200-1000 Hz), above
constexpr float kRoomMatchHighHz = 1000.0f;
constexpr std::uint32_t kRoomMatchBands = 3;
constexpr std::uint32_t kRoomSections = 6;  // RoomStats::sectionUs

// What the direct gain d must follow in this block (Room::DirectState): the room's own values at the block's end, so
// d ramps with the room it makes room for. params: the parameters as crossfaded now (kRoomParamsCrossfadeMs); earlyMix:
// the early reflections' fade (0 off .. 1 on: frameValid, early and a rotation seen, over kRoomEarlyFadeMs).
struct RoomDirectState {
    const RoomParams* params;  // valid on the audio thread until the next BeginBlock; null before any parameters
    float wet;
    float canopyOpen;          // smoothed once, by the room (kRoomCanopyTauS)
    float earlyMix;
};

struct RoomStats {
    std::uint64_t blocks, sourcesAdded, earlyOffBlocks, frameInvalidBlocks, nonFinite, rejected, idleBlocks;
    std::uint64_t lastBlockNs, worstBlockNs;
    float lastWet, lastCanopy;
    float matchDb[kRoomMatchBands];  // the level match's gains (below 200 Hz, 200-1000 Hz, above 1 kHz), at full wet
    // Render's time per section since Init (microseconds): drain, wall filters, bus HRTF renders, early high-pass,
    // tail, output and level match.
    double sectionUs[kRoomSections];
};

class Room {
public:
    Room();
    ~Room();
    Room(const Room&) = delete;
    Room& operator=(const Room&) = delete;

    // ---- control thread ---------------------------------------------------------------------------------------
    // Allocates everything: the bus Renderer (kRoomBuses slots, near field off), 14 bus buffers, per-slot tap lines
    // (maxSources x next pow2 of 12 ms), the FDN, two parameter sets. sampleRate as binaural::Config.
    bool Init(std::uint32_t sampleRate, std::uint32_t maxSources, char* error, std::size_t errorCapacity);
    void SetHrtf(const HrtfSet* set);           // the voices' set; also recomputes the diffuse-field EQ (may allocate)
    const HrtfSet* TakeRetiredHrtf();           // as Renderer::TakeRetiredHrtf
    void SetHeadRadius(float metres);           // ITD radius for the buses, as Renderer::SetHeadRadius
    void SetParams(const RoomParams& p);        // published atomically; crossfaded over kRoomParamsCrossfadeMs

    // ---- audio thread (same rules as Renderer: no allocation, locks, logging, I/O; bounded) ---------------------
    void BeginBlock(std::uint32_t frames, const RoomFrame& f);
    // Adds one voice's sends. `in` is the same MonoInput the voice's Render call got; w0..w1 its HRTF weight
    // (ownership fade x kill switch), linear across the call. False (nothing added, counted) when rejected.
    bool AddSource(std::uint32_t slot, const RoomSource& s, const MonoInput& in, std::uint32_t frames, float w0, float w1);
    void ResetSource(std::uint32_t slot);
    // After every AddSource of the block: renders the buses (the internal Renderer) and the tail, ADDS to outLR.
    void Render(float* outLR, std::uint32_t frames);
    // The same with the level match. directLR (stereo, frames): this block's direct sound of the voices sent to the
    // room (their Render outputs with d applied, the kept side of wide voices included), NOT in outLR. Adds
    // M(direct + room) to outLR: M a three-band gain (kRoomMatch*) measured on direct + room against the direct alone
    // over the last seconds (at full wet), applied as gain^wet; it holds while the sound is quiet. directLR is added
    // to outLR even when the room cannot render (not ready, a rejected block). directLR null: Render(outLR, frames).
    void Render(float* outLR, std::uint32_t frames, const float* directLR);
    void EndBlock();
    bool Idle() const;                           // wet 0, every tap line and the tail silent: the module may skip the room
    RoomDirectState DirectState() const;         // after BeginBlock: the values d must use for this block

    // ---- any thread -------------------------------------------------------------------------------------------
    RoomStats GetStats() const;
    void ResetStats();

private:
    struct Impl;
    Impl* impl_;
    friend struct RoomAccess;  // the offline harness's read-only probes (src/room_model.h); not an API
};

// Pure (any thread): the direct gain d of section 6.2 for a voice, from the parameters, the wet weight and the
// voice's aircraft-frame position and flags. 1 when wet = 0.
float RoomDirectGain(const RoomParams& p, float wet, float canopyOpen, bool early, const float posAircraft[3],
                     std::uint32_t flags);
// Pure: the same with the early reflections' mix (RoomDirectState::earlyMix, 0..1) in place of the flag: their energy
// scales with its square (the taps' gain is scaled by it). RoomDirectGain(.., early, ..) is this with 1 or 0.
float RoomDirectGainMix(const RoomParams& p, float wet, float canopyOpen, float earlyMix, const float posAircraft[3],
                        std::uint32_t flags);
// Pure: the send's distance law of section 4.2 (clamp(r / kRoomRefDistanceM, 0.5, 2) inside the box, 1 outside).
float RoomSendDistanceGain(const RoomParams& p, const float posAircraft[3], std::uint32_t flags);

// CPU (harness C9, thread CPU time per 480-frame block at 48 kHz, the room alone, next to the voices' own renders):
// 48 voices AVX2/FMA p50 0.145 ms, p99 0.19 ms static, 0.21 ms with the head turning; 64 voices p99 0.20-0.24 ms; SSE2
// about 1.4x; early reflections off (back-off step 1) 0.043 ms. docs/COCKPIT_SPACE.md section 9.

}  // namespace binaural
