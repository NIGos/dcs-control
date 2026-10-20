// R63 envinfo-layers-ignore-disable-env: the log's API-layer list marks an implicit layer that this process's
// environment turns off (disable_environment set / enable_environment unset) and logs XR_API_LAYER_PATH.
#pragma once

namespace r63env {

void Run() {
  const std::string qv =
      "{\n  \"file_format_version\": \"1.0.0\",\n  \"api_layer\": {\n    \"name\": "
      "\"XR_APILAYER_MBUCCHIA_quad_views_foveated\",\n    \"library_path\": \".\\QVFR.dll\",\n"
      "    \"disable_environment\":  \"DISABLE_XR_APILAYER_MBUCCHIA_quad_views_foveated\"\n  }\n}\n";
  auto none = [](const std::string&) { return false; };
  auto all = [](const std::string&) { return true; };
  Check(envinfo::ImplicitLayerNote(qv, all) == " (disabled by DISABLE_XR_APILAYER_MBUCCHIA_quad_views_foveated)" &&
            envinfo::ImplicitLayerNote(qv, none).empty(),
        "R63 env: an implicit layer whose disable_environment is set is marked disabled (not when unset)");
  const std::string gated = "{\"api_layer\": {\"name\": \"X\", \"enable_environment\": \"ENABLE_X\"}}";
  Check(envinfo::ImplicitLayerNote(gated, none) == " (off: ENABLE_X not set)" &&
            envinfo::ImplicitLayerNote(gated, all).empty(),
        "R63 env: an implicit layer whose enable_environment is unset is marked off");
  Check(envinfo::ImplicitLayerNote("", all).empty() && envinfo::ImplicitLayerNote("{\"name\": \"Y\"}", all).empty(),
        "R63 env: no manifest / no environment keys: no mark");
  wchar_t old[2048] = {};
  const DWORD had = GetEnvironmentVariableW(L"XR_API_LAYER_PATH", old, 2048);
  SetEnvironmentVariableW(L"XR_API_LAYER_PATH", L"C:\\qv\\layers");
  const std::string l = envinfo::XrLayers();
  Check(l.find("XR_API_LAYER_PATH=C:\\qv\\layers") != std::string::npos, "R63 env: XR_API_LAYER_PATH is logged");
  SetEnvironmentVariableW(L"XR_API_LAYER_PATH", had ? old : nullptr);
  SetEnvironmentVariableA("QV_R63_DISABLE_PROBE", "1");
  Check(envinfo::EnvSet("QV_R63_DISABLE_PROBE") && !envinfo::EnvSet("QV_R63_NOT_SET_PROBE") && !envinfo::EnvSet(""),
        "R63 env: EnvSet reads this process's environment");
  SetEnvironmentVariableA("QV_R63_DISABLE_PROBE", nullptr);
  printf("     (API layers here: %s)\n", envinfo::XrLayers().c_str());
}

}  // namespace r63env
