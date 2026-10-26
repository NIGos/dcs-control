#include <iterator>
// DCS Control: live INI reload on the resident runtime (dcs_live_reload.hpp, dcs_live_reload_runtime.inc), on the
// generic host without graphics. DCS Control's writer starts the INI; the runtime saves it at start (its own write,
// never a reload); an atomic replace by DCS Control applies the live keys within about a second and stages the
// start-only ones; a later save by Cheeky keeps both; a DCS Control write right before a Cheeky save is not lost.
bool dcs_live_reload_mode{};

std::string dcs_read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
// As DCS Control's AtomicFile.Write: a temporary file moved over the INI.
void dcs_write_atomic(const std::filesystem::path& path, const std::string& text) {
    auto temporary = path; temporary += L".dcscontrol.tmp";
    { std::ofstream out(temporary, std::ios::binary | std::ios::trunc); out << text; }
    require(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
        "Replace the INI like DCS Control");
}
unsigned dcs_count(const std::string& text, const char* part) {
    unsigned count{};
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++count;
    return count;
}
// The INI as ConfigurationWriters.Cheeky writes it (integers for booleans, G9 numbers).
std::string dcs_writer_ini(const char* intensity, const char* nr_width, const char* color) {
    return std::string("[CheekyFoveatedDLSS]\nSchemaVersion=1\nEnabled=1\nD3D11D3D12Transport=1\nD3D12LowerHook=1\n"
        "PeripheralDlaa=0\nPeripheralDlaaScale=0.75\nWidth=1\nHeight=1\nCenterMode=0\nAutoStereoAlignment=0\n"
        "EyeCalibrationContinuous=0\nGazeSmoothingMs=20\nGazeHoldMs=400\nTransitionWidth=0.04\nNrEnabled=1\nNrFoveated=1\n"
        "NrUseSrFoveation=0\nNrWidth=") + nr_width + "\nNrHeight=0.6\nNrRoundness=1\nCenterPreset=0\nNrTransitionWidth=0.08\n"
        "NrProcessingOrder=1\nNrWorkingScale=1\nNrIntensity=" + intensity + "\nNrLocalToneStrength=1\n"
        "NrLocalStructureStrength=1\nNrSkinStructureStrength=1\nNrUiCorrection=0\nNrColorStrength=" + color + "\n"
        "NrHdrTransferStrength=1\nNrPaperWhiteScale=1\nNrDepthConvention=0\nNrMotionScaleXMultiplier=1\n"
        "NrMotionScaleYMultiplier=1\nNrStyle=0\nNrAutomaticMask=0\n";
}
template<class Condition> bool dcs_wait(Condition condition, unsigned ms = 6000) {
    for (unsigned waited = 0; waited < ms; waited += 50) { if (condition()) return true; Sleep(50); }
    return condition();
}

void verify_dcs_live_reload(CheekyRuntimeCommandFn command, CheekyRuntimeSnapshotFn get, std::uint64_t attachment,
    const std::filesystem::path& directory) {
    const auto ini = directory / "CheekyFoveatedDLSS.ini";
    const auto log = directory / "CheekyFoveatedDLSS-Standalone.log";
    const auto reloads = [&] { return dcs_count(dcs_read_text(log), "DCS Control live reload:"); };
    // Start saved the INI in Cheeky's own format (every key, learned calibration included).
    require(dcs_wait([&] { return contains(dcs_read_text(ini), "EyeCalibrationLearnedMethod="); }), "Start saves the INI");
    require(contains(snapshot(get), "\"NrIntensity\":1,") && contains(snapshot(get), "\"NrWidth\":0.600000024"),
        "Start reads DCS Control's INI");
    Sleep(2500);
    require(reloads() == 0, "Cheeky's own save at start is not a reload");

    // DCS Control changes a live key (intensity) and a start-only key (DLSS 5 area) while DCS runs.
    dcs_write_atomic(ini, dcs_writer_ini("0.5", "0.3", "1"));
    require(dcs_wait([&] { return contains(snapshot(get), "\"NrIntensity\":0.5,"); }), "Live key applied within seconds");
    require(contains(snapshot(get), "\"NrWidth\":0.600000024"), "Start-only key keeps its running value");
    require(dcs_wait([&] { return reloads() == 1; }), "One log line per reload");
    const auto text = dcs_read_text(log);
    require(contains(text, "DCS Control live reload: applied NrIntensity=0.5; next start: NrWidth=0.300000012"),
        "The log line names the applied and the staged keys");
    Sleep(2500);
    require(reloads() == 1, "A handled INI is not reloaded again");

    // An overlay change saves the INI: Cheeky's save keeps DCS Control's values and is no reload itself.
    require(command(attachment, "1\n900\nset\nNrColorStrength=0.75"), "Overlay set");
    require(dcs_wait([&] { return contains(dcs_read_text(ini), "NrColorStrength=0.75\n"); }), "Overlay change saved");
    auto saved = dcs_read_text(ini);
    require(contains(saved, "NrIntensity=0.5\n"), "Cheeky's save keeps DCS Control's live value");
    require(contains(saved, "NrWidth=0.300000012\n"), "Cheeky's save keeps DCS Control's start-only value");
    Sleep(2500);
    require(reloads() == 1, "Cheeky's own save is not a reload");

    // DCS Control writes right before Cheeky saves: the save first takes the new INI.
    dcs_write_atomic(ini, dcs_writer_ini("0.25", "0.3", "0.75"));
    require(command(attachment, "1\n901\nset\nNrPaperWhiteScale=1.5"), "Overlay set right after DCS Control's write");
    require(dcs_wait([&] { return contains(dcs_read_text(ini), "NrPaperWhiteScale=1.5\n"); }), "Second overlay change saved");
    saved = dcs_read_text(ini);
    require(contains(saved, "NrIntensity=0.25\n") && contains(snapshot(get), "\"NrIntensity\":0.25,"),
        "A DCS Control write just before a Cheeky save is applied, not overwritten");
    require(dcs_wait([&] { return reloads() == 2; }) && contains(dcs_read_text(log), "applied NrIntensity=0.25; next start: none"),
        "Second reload logged once");

    // A start-only key set back to its running value is no longer staged; an invalid INI is rejected whole.
    dcs_write_atomic(ini, dcs_writer_ini("0.25", "0.6", "0.75") + "NrPaperWhiteScale=1.5\n");
    require(dcs_wait([&] { return reloads() == 3; }), "Unstaging reload logged");
    dcs_write_atomic(ini, dcs_writer_ini("0.125", "0.6", "nope"));
    require(dcs_wait([&] { return contains(dcs_read_text(log), "Invalid setting: NrColorStrength; the INI is ignored"); }),
        "Malformed INI reported");
    require(contains(snapshot(get), "\"NrIntensity\":0.25,"), "A malformed INI applies nothing");
    require(command(attachment, "1\n902\nset\nNrStyle=1"), "Overlay set after the unstaging");
    require(dcs_wait([&] { return contains(dcs_read_text(ini), "NrStyle=1\n"); }), "Third overlay change saved");
    require(contains(dcs_read_text(ini), "NrWidth=0.600000024\n"), "An unstaged key saves its running value");
}
