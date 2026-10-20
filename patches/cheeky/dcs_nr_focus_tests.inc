// DCS Control: the DLSS 5 focus shape settings (settings.cpp clamps, dlss_nr_contract.cpp region parameters).
// Included by tests/openxr_calibration_tests.cpp; uses its require(). Run with CheekyTests --dcs-nr-focus.
#include <algorithm>
#include "dlss_nr_contract.hpp"
#include "settings.hpp"
void dcs_nr_focus_test() {
    using namespace cheeky::foveated_dlss;
    const auto saved = current_settings();
    auto requested = saved;
    requested.nr_enabled = requested.nr_foveated = true;
    requested.nr_use_sr_foveation = false;
    requested.nr_width = requested.nr_height = 0.8F;
    requested.nr_roundness = 1.0F;
    requested.nr_transition_width = 0.45F;
    requested.transition_width = 0.45F;
    update_settings(requested);
    const auto applied = current_settings();
    require(applied.nr_transition_width == 0.45F, "NrTransitionWidth keeps 0.45 (the Quad Views focus fade band)");
    require(applied.transition_width == 0.30F, "The SR TransitionWidth keeps its 0.30 cap");
    require(applied.nr_roundness == 1.0F && applied.nr_width == 0.8F && applied.nr_height == 0.8F,
        "A round central DLSS 5 area is kept as requested");
    requested.nr_transition_width = 0.8F;
    requested.nr_roundness = 3.0F;
    update_settings(requested);
    require(current_settings().nr_transition_width == 0.45F, "NrTransitionWidth is capped at 0.45");
    require(current_settings().nr_roundness == 1.0F, "NrRoundness is capped at 1 (round)");
    requested.nr_transition_width = -1.0F;
    update_settings(requested);
    require(current_settings().nr_transition_width == 0.0F, "A negative NrTransitionWidth is a hard edge");

    // The central area (NrWidth/NrHeight < 1, independent of the SR shape) carries NrRoundness and the wider fade
    // to the codec: the decode's distance is lerp(max(|x|,|y|), length(xy), roundness) over the NR region, so 1 is
    // the ellipse inscribed in it (a circle for a square focus view), and the fade runs from 1 - 0.45 / 0.8 to 1.
    const auto parameters = dlss_nr_foveation_parameters(applied, nullptr, 0U, 0U);
    require(parameters.width == 0.8F && parameters.height == 0.8F && parameters.roundness == 1.0F &&
        parameters.transition_width == 0.45F, "The central area's round shape and fade reach the NR region");
    const float normalized_feather = parameters.transition_width / (std::min)(parameters.width, parameters.height);
    require(normalized_feather > 0.56F && normalized_feather < 0.57F, "The fade starts at 44 % of the area radius");
    // A 0.45 fade on the whole focus view: full DLSS 5 inside 55 % of the radius, none at the edge.
    auto whole = applied;
    whole.nr_width = whole.nr_height = 1.0F;
    const auto whole_parameters = dlss_nr_foveation_parameters(whole, nullptr, 0U, 0U);
    require(whole_parameters.width == 1.0F && whole_parameters.transition_width == 0.45F &&
        whole_parameters.x_offset == 0.0F && whole_parameters.y_offset == 0.0F, "The whole-view fade stays centred");
    update_settings(saved);
}
