#pragma once
// DCS Control: DLSS 5 (DLSS-NR feature 18) stops for the rest of the session once its evaluation keeps failing.
// Upstream retries a failed evaluation on every frame, each time with a history reset, so a broken NR runtime costs
// the encode, the guides and a failing NGX call per focus view per frame for the whole flight. After
// dcs_nr_failure_limit consecutive failures on one view, or at once when the result or the device says the device
// was removed or hung, the latch closes: evaluate_dlss_nr returns false before touching anything and the D3D11
// transport runs SR only (Settings::dcs_nr_suppressed), so DCS's image is the plain DLSS upscale it would be with
// DLSS 5 off. Nothing reopens it until the process restarts. Pure policy, no Windows dependency, unit-tested.
#include <atomic>
#include <cstdint>

namespace cheeky::foveated_dlss {

inline constexpr std::uint32_t dcs_nr_failure_limit = 30U;
inline constexpr const char* dcs_nr_failure_latch_reason =
    "DLSS 5 stopped for this session after repeated evaluation failures";

// DXGI_ERROR_DEVICE_REMOVED, _HUNG, _RESET and _DRIVER_INTERNAL_ERROR, as an NGX result or a removed reason.
[[nodiscard]] constexpr bool dcs_nr_device_lost(const std::uint32_t code) noexcept {
    return code == 0x887A0005U || code == 0x887A0006U || code == 0x887A0007U || code == 0x887A0020U;
}

class DcsNrFailureLatch {
public:
    // One failed evaluation; `run` is that view's consecutive failure count including this one.
    // True exactly once: on the failure that closes the latch.
    [[nodiscard]] bool failed(const std::uint32_t run, const std::uint32_t result,
        const std::uint32_t device_removed_reason) noexcept {
        if (run < dcs_nr_failure_limit && !dcs_nr_device_lost(result) && !dcs_nr_device_lost(device_removed_reason))
            return false;
        return !latched_.exchange(true, std::memory_order_acq_rel);
    }
    [[nodiscard]] bool active() const noexcept { return latched_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> latched_{};
};

// The process-wide latch (one per loaded runtime DLL).
[[nodiscard]] inline DcsNrFailureLatch& dcs_nr_failure_latch() noexcept {
    static DcsNrFailureLatch latch;
    return latch;
}

}  // namespace cheeky::foveated_dlss
