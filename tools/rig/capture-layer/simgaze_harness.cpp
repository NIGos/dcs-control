// Headset-free test of the rig capture layer's simulated eye gaze (simgaze.h, RIGTAP_SIMGAZE in rigtap.cpp).
// This program is the loader, a fake runtime WITHOUT eye tracking, and the layer above (it plays Quad Views' / Cheeky's
// part: an eye-gaze action, its space, the eye-gaze profile suggestion, xrGetActionStatePose, xrLocateSpace).
//   simgaze_harness <layer dll> <out dir>
// Exit 0 = every check passed.
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include "simgaze.h"

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* what) { if (ok) ++g_pass; else { ++g_fail; std::printf("FAIL: %s\n", what); } }
static bool approx(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

// ---- the fake runtime --------------------------------------------------------------------------------------------
static bool g_offered = false;                         // the runtime lists XR_EXT_eye_gaze_interaction
static std::vector<std::string> g_rtExt;               // extensions of the last successful instance
static unsigned g_rtCreates = 0, g_rtSuggests = 0, g_rtActionLocates = 0, g_rtPoseCalls = 0;
static std::map<std::string, XrPath> g_paths; static std::map<XrSpace, int> g_refType; static std::map<XrSpace, XrAction> g_actSpace;
static uint64_t g_h = 0x5000; static double g_headYawDeg = 30; static bool g_headLost = false;
static XrPath Path(const char* s) { auto it = g_paths.find(s); if (it != g_paths.end()) return it->second; XrPath p = (XrPath)(g_paths.size() + 100); g_paths[s] = p; return p; }
static XrResult XRAPI_CALL RtStringToPath(XrInstance, const char* s, XrPath* p) { *p = Path(s); return XR_SUCCESS; }
static XrResult XRAPI_CALL RtCreateSession(XrInstance, const XrSessionCreateInfo*, XrSession* s) { *s = (XrSession)0x51; return XR_SUCCESS; }
static XrResult XRAPI_CALL RtDestroySession(XrSession) { return XR_SUCCESS; }
static XrResult XRAPI_CALL RtCreateRef(XrSession, const XrReferenceSpaceCreateInfo* ci, XrSpace* sp) { *sp = (XrSpace)++g_h; g_refType[*sp] = (int)ci->referenceSpaceType; return XR_SUCCESS; }
static XrResult XRAPI_CALL RtCreateActionSpace(XrSession, const XrActionSpaceCreateInfo* ci, XrSpace* sp) { *sp = (XrSpace)++g_h; g_actSpace[*sp] = ci->action; return XR_SUCCESS; }
static XrResult XRAPI_CALL RtDestroySpace(XrSpace sp) { g_refType.erase(sp); g_actSpace.erase(sp); return XR_SUCCESS; }
static XrResult XRAPI_CALL RtGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* p) {
  for (auto* n = (XrBaseOutStructure*)p->next; n; n = n->next) if (n->type == XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT) ((XrSystemEyeGazeInteractionPropertiesEXT*)n)->supportsEyeGazeInteraction = XR_FALSE;
  return XR_SUCCESS;
}
static XrResult XRAPI_CALL RtSuggest(XrInstance, const XrInteractionProfileSuggestedBinding* s) {
  ++g_rtSuggests; return s->interactionProfile == Path("/interaction_profiles/ext/eye_gaze_interaction") && !g_offered ? XR_ERROR_PATH_UNSUPPORTED : XR_SUCCESS;
}
static XrResult XRAPI_CALL RtGetPose(XrSession, const XrActionStateGetInfo*, XrActionStatePose* st) { ++g_rtPoseCalls; st->isActive = XR_FALSE; return XR_SUCCESS; }
static XrResult XRAPI_CALL RtCurProfile(XrSession, XrPath, XrInteractionProfileState* st) { st->interactionProfile = XR_NULL_PATH; return XR_SUCCESS; }
// The head: yaw g_headYawDeg about +Y at (0, 1.6, 0) in LOCAL; a VIEW space located in itself is the identity.
static XrResult XRAPI_CALL RtLocateSpace(XrSpace sp, XrSpace base, XrTime, XrSpaceLocation* loc) {
  if (g_actSpace.count(sp)) { ++g_rtActionLocates; loc->locationFlags = 0; return XR_SUCCESS; }  // no eye tracker
  const bool viewInLocal = g_refType[sp] == XR_REFERENCE_SPACE_TYPE_VIEW && g_refType[base] == XR_REFERENCE_SPACE_TYPE_LOCAL;
  const double h = viewInLocal ? g_headYawDeg * 3.14159265358979 / 360 : 0;
  loc->pose = XrPosef{{0, (float)std::sin(h), 0, (float)std::cos(h)}, {0, viewInLocal ? 1.6f : 0.0f, 0}};
  loc->locationFlags = g_headLost ? 0 : 0xF;
  return XR_SUCCESS;
}
static XrResult XRAPI_CALL RtGipa(XrInstance, const char* name, PFN_xrVoidFunction* fn) {
  static const std::map<std::string, PFN_xrVoidFunction> table = {
    {"xrStringToPath", (PFN_xrVoidFunction)RtStringToPath}, {"xrCreateSession", (PFN_xrVoidFunction)RtCreateSession},
    {"xrDestroySession", (PFN_xrVoidFunction)RtDestroySession}, {"xrCreateReferenceSpace", (PFN_xrVoidFunction)RtCreateRef},
    {"xrCreateActionSpace", (PFN_xrVoidFunction)RtCreateActionSpace}, {"xrDestroySpace", (PFN_xrVoidFunction)RtDestroySpace},
    {"xrGetSystemProperties", (PFN_xrVoidFunction)RtGetSystemProperties}, {"xrSuggestInteractionProfileBindings", (PFN_xrVoidFunction)RtSuggest},
    {"xrGetActionStatePose", (PFN_xrVoidFunction)RtGetPose}, {"xrGetCurrentInteractionProfile", (PFN_xrVoidFunction)RtCurProfile},
    {"xrLocateSpace", (PFN_xrVoidFunction)RtLocateSpace}};
  auto it = table.find(name); *fn = it == table.end() ? nullptr : it->second; return *fn ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
}
static XrResult XRAPI_CALL RtCreateApiLayerInstance(const XrInstanceCreateInfo* ci, const XrApiLayerCreateInfo*, XrInstance* inst) {
  ++g_rtCreates; std::vector<std::string> ext;
  for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) { if (!g_offered && !strcmp(ci->enabledExtensionNames[i], XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) return XR_ERROR_EXTENSION_NOT_PRESENT; ext.push_back(ci->enabledExtensionNames[i]); }
  g_rtExt = ext; *inst = (XrInstance)0x11; return XR_SUCCESS;
}

// ---- the model alone -----------------------------------------------------------------------------------------------
static void ModelTests(const std::string& out) {
  simgaze::Program p; std::string err;
  check(!simgaze::parse("x=3", p, err), "parse: mode= is required");
  check(!simgaze::parse("mode=fixed;bogus=1", p, err), "parse: an unknown key is an error");
  check(!simgaze::parse("mode=targets;targets=1,2|3", p, err), "parse: a malformed target is an error");
  check(!simgaze::parse("mode=fixed;x=99", p, err), "parse: x beyond 60 deg is an error");
  check(simgaze::parse("mode=fixed;x=4;y=-2", p, err) && simgaze::evaluate(p, 5).x == 4 && simgaze::evaluate(p, 5).y == -2 && simgaze::evaluate(p, 5).valid, "fixed gaze");
  check(simgaze::parse("mode=off", p, err) && !simgaze::evaluate(p, 1).simulated, "off: not simulated");
  check(simgaze::parse("mode=lost", p, err) && simgaze::evaluate(p, 1).simulated && !simgaze::evaluate(p, 1).valid, "lost: simulated, never valid");
  // A 10 deg saccade: 43 ms, minimum-jerk, peak about 436 deg/s, lands exactly; dwell 300 ms each side.
  check(simgaze::parse("mode=targets\ntargets=0,0|10,0\ndwell_ms=300\nloop=0", p, err), "parse targets");
  const double d = simgaze::saccade_duration_s(10);
  check(approx(d, 0.043, 1e-9), "main sequence: 10 deg lasts 43 ms");
  double peak = 0, prev = 0; bool monotonic = true, flagged = true;
  for (int i = 0; i <= 60; ++i) {
    const double t = 0.3 + i * 0.001; const auto s = simgaze::evaluate(p, t);
    if (i) { const double v = (s.x - prev) / 0.001; peak = std::max(peak, v); if (v < -1e-6) monotonic = false; }
    if (t > 0.3005 && t < 0.3 + d - 0.0005 && !s.saccade) flagged = false;
    prev = s.x;
  }
  check(peak > 380 && peak < 480, "saccade peak speed between 380 and 480 deg/s (main sequence)");
  check(monotonic, "saccade position never moves back");
  check(flagged, "saccade samples are flagged");
  check(simgaze::evaluate(p, 0.2).x == 0 && simgaze::evaluate(p, 0.3 + d + 0.01).x == 10 && simgaze::evaluate(p, 30).x == 10, "dwell before, landing after, holds at the end without loop");
  check(simgaze::parse("mode=targets;targets=0,0|10,0;dwell_ms=300", p, err), "parse looping targets");
  const double cycle = 2 * (d + 0.3);
  check(approx(simgaze::evaluate(p, 0.3 + cycle + 0.02).x, simgaze::evaluate(p, 0.3 + 0.02).x, 1e-4), "loop repeats the same saccade");
  check(simgaze::evaluate(p, 0.3 + d + 0.3 + d + 0.1).x == 0, "loop returns to the first target");
  check(simgaze::parse("mode=sweep;amp_x=10;amp_y=5;period_ms=2000", p, err) && approx(simgaze::evaluate(p, 0.5).x, 10, 1e-4) && approx(simgaze::evaluate(p, 0.5).y, 0, 1e-4) && approx(simgaze::evaluate(p, 0).y, 5, 1e-4), "sweep (x sine, y 90 deg ahead)");
  check(simgaze::parse("mode=fixed;blink_every_ms=1000;blink_ms=150", p, err) && simgaze::evaluate(p, 0.5).valid && !simgaze::evaluate(p, 1.05).valid && simgaze::evaluate(p, 1.2).valid && !simgaze::evaluate(p, 2.1).valid, "blinks every second for 150 ms");
  { simgaze::parse("mode=fixed;noise_deg=0.2;seed=7", p, err); double sx = 0, sxx = 0; const int n = 6000;
    for (int i = 0; i < n; ++i) { const double v = simgaze::evaluate(p, i * 0.01).x; sx += v; sxx += v * v; }
    const double mean = sx / n, sd = std::sqrt(sxx / n - mean * mean);
    check(std::fabs(mean) < 0.05 && sd > 0.1 && sd < 0.3, "fixational noise: mean ~0, sd about noise_deg");
    check(simgaze::evaluate(p, 1.234).x == simgaze::evaluate(p, 1.234).x, "noise is deterministic"); }
  { const std::string csv = out + "\\replay.csv"; FILE* f = std::fopen(csv.c_str(), "wb"); std::fputs("t_s,x_deg,y_deg,valid\n0,0,0,1\n1,10,-4,1\n2,10,-4,0\n3,0,0,1\n", f); std::fclose(f);
    check(simgaze::parse("mode=replay;file=" + csv, p, err), "parse replay");
    const auto a = simgaze::evaluate(p, 0.5), b = simgaze::evaluate(p, 2.5), c = simgaze::evaluate(p, 9);
    check(approx(a.x, 5, 1e-4) && approx(a.y, -2, 1e-4) && a.valid, "replay interpolates");
    check(!b.valid, "replay: invalid where a neighbour is invalid");
    check(c.x == 0 && c.valid, "replay holds the last sample past the end");
    check(simgaze::parse("mode=replay;loop=1;file=" + csv, p, err) && approx(simgaze::evaluate(p, 3.5).x, 5, 1e-4), "replay loops"); }
  { float fwd[3] = {0, 0, -1}, o[3];
    simgaze::rotate(simgaze::gaze_orientation(10, 0), fwd, o); check(approx(o[0], std::sin(10 * 3.14159265 / 180), 1e-5) && approx(o[1], 0, 1e-6), "x>0 looks right");
    simgaze::rotate(simgaze::gaze_orientation(0, 10), fwd, o); check(approx(o[1], std::sin(10 * 3.14159265 / 180), 1e-5) && approx(o[0], 0, 1e-6), "y>0 looks up"); }
}

// ---- the layer -----------------------------------------------------------------------------------------------------
struct Layer { XrNegotiateApiLayerRequest req{}; XrInstance inst{}; };
template <class F> static F Get(Layer& l, const char* n) { PFN_xrVoidFunction p = nullptr; l.req.getInstanceProcAddr(l.inst, n, &p); return (F)p; }
static XrResult Create(HMODULE mod, Layer& l, bool askGaze) {
  auto negotiate = (PFN_xrNegotiateLoaderApiLayerInterface)GetProcAddress(mod, "xrNegotiateLoaderApiLayerInterface");
  XrNegotiateLoaderInfo li{}; li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO; li.structVersion = XR_LOADER_INFO_STRUCT_VERSION; li.structSize = sizeof(li);
  li.minInterfaceVersion = 1; li.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION; li.minApiVersion = XR_MAKE_VERSION(1, 0, 0); li.maxApiVersion = XR_CURRENT_API_VERSION;
  l.req = {}; l.req.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST; l.req.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION; l.req.structSize = sizeof(l.req);
  if (!negotiate || XR_FAILED(negotiate(&li, "XR_APILAYER_RIG_capture", &l.req))) return XR_ERROR_INITIALIZATION_FAILED;
  XrApiLayerNextInfo ni{}; ni.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO; ni.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION; ni.structSize = sizeof(ni);
  strcpy_s(ni.layerName, "XR_APILAYER_RIG_capture"); ni.nextGetInstanceProcAddr = RtGipa; ni.nextCreateApiLayerInstance = RtCreateApiLayerInstance;
  XrApiLayerCreateInfo lci{}; lci.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO; lci.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION; lci.structSize = sizeof(lci); lci.nextInfo = &ni;
  const char* ext[] = {"XR_KHR_D3D11_enable", XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME};
  XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO}; strcpy_s(ici.applicationInfo.applicationName, "simgaze_harness"); ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ici.enabledExtensionCount = askGaze ? 2 : 1; ici.enabledExtensionNames = ext;
  return l.req.createApiLayerInstance(&ici, &lci, &l.inst);
}
static bool HasExt(const char* n) { for (auto& e : g_rtExt) if (e == n) return true; return false; }
static std::string ReadAll(const std::string& p) { std::string s; if (FILE* f = std::fopen(p.c_str(), "rb")) { char b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n); std::fclose(f); } return s; }
static void WriteGaze(const std::string& dir, const char* text) { const std::string tmp = dir + "\\gaze.tmp"; FILE* f = std::fopen(tmp.c_str(), "wb"); std::fputs(text, f); std::fclose(f); MoveFileExA(tmp.c_str(), (dir + "\\gaze.txt").c_str(), MOVEFILE_REPLACE_EXISTING); }
// The direction (-Z axis) of a located pose.
static void Dir(const XrPosef& p, float o[3]) { const float f[3] = {0, 0, -1}; simgaze::rotate({p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, f, o); }

int main(int argc, char** argv) {
  if (argc < 3) { std::printf("usage: simgaze_harness <layer dll> <out dir>\n"); return 2; }
  const std::string out = argv[2]; CreateDirectoryA(out.c_str(), nullptr);
  ModelTests(out);
  HMODULE mod = LoadLibraryA(argv[1]); if (!mod) { std::printf("cannot load %s\n", argv[1]); return 2; }
  SetEnvironmentVariableA("RIGTAP_DIR", out.c_str()); SetEnvironmentVariableA("RIGTAP_EXTENSIONS", "XR_KHR_D3D11_enable");
  auto active = (int (*)())GetProcAddress(mod, "RigTap_SimGazeActive");
  check(active != nullptr, "RigTap_SimGazeActive exported");

  // -- 1. Off (no RIGTAP_SIMGAZE): nothing of the gaze is touched.
  SetEnvironmentVariableA("RIGTAP_SIMGAZE", nullptr);
  { Layer l; PFN_xrVoidFunction e = nullptr; XrNegotiateApiLayerRequest dummy{};
    (void)dummy;
    auto negotiate = (PFN_xrNegotiateLoaderApiLayerInterface)GetProcAddress(mod, "xrNegotiateLoaderApiLayerInterface"); (void)negotiate;
    check(Create(mod, l, true) == XR_ERROR_EXTENSION_NOT_PRESENT, "off: the runtime's refusal of the eye-gaze extension reaches the application");
    check(XR_SUCCEEDED(Create(mod, l, false)), "off: instance without the extension");
    check(Get<PFN_xrLocateSpace>(l, "xrLocateSpace") == RtLocateSpace && Get<PFN_xrGetSystemProperties>(l, "xrGetSystemProperties") == RtGetSystemProperties &&
          Get<PFN_xrGetActionStatePose>(l, "xrGetActionStatePose") == RtGetPose && Get<PFN_xrSuggestInteractionProfileBindings>(l, "xrSuggestInteractionProfileBindings") == RtSuggest,
          "off: the gaze functions are the runtime's own (no hook handed out)");
    l.req.getInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &e); uint32_t n = 0; XrExtensionProperties props[8]; for (auto& p : props) p = {XR_TYPE_EXTENSION_PROPERTIES};
    auto en = (PFN_xrEnumerateInstanceExtensionProperties)e; check(en && XR_SUCCEEDED(en(nullptr, 8, &n, props)) && n == 1, "off: null-instance extension list is the runtime's");
    check(active() == 0, "off: RigTap_SimGazeActive 0"); }

  // -- 2. On, runtime without eye tracking.
  SetEnvironmentVariableA("RIGTAP_SIMGAZE", "1"); SetEnvironmentVariableA("RIGTAP_SIMGAZE_PROGRAM", "mode=fixed;x=10;y=-5");
  { PFN_xrVoidFunction e = nullptr; Layer probe; probe.req = {}; HMODULE m = mod; (void)m;
    auto negotiate = (PFN_xrNegotiateLoaderApiLayerInterface)GetProcAddress(mod, "xrNegotiateLoaderApiLayerInterface");
    XrNegotiateLoaderInfo li{}; li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO; li.structVersion = XR_LOADER_INFO_STRUCT_VERSION; li.structSize = sizeof(li); li.minInterfaceVersion = 1; li.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION; li.minApiVersion = XR_MAKE_VERSION(1, 0, 0); li.maxApiVersion = XR_CURRENT_API_VERSION;
    probe.req.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST; probe.req.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION; probe.req.structSize = sizeof(probe.req); negotiate(&li, "XR_APILAYER_RIG_capture", &probe.req);
    probe.req.getInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &e); uint32_t n = 0; XrExtensionProperties props[8]; for (auto& p : props) p = {XR_TYPE_EXTENSION_PROPERTIES};
    auto en = (PFN_xrEnumerateInstanceExtensionProperties)e; bool has = false;
    check(en && XR_SUCCEEDED(en(nullptr, 8, &n, props)) && n == 2, "on: null-instance extension list gains one entry");
    for (uint32_t i = 0; i < n; ++i) if (!strcmp(props[i].extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) has = true;
    check(has, "on: ... XR_EXT_eye_gaze_interaction"); }
  Layer l; g_rtCreates = 0;
  check(XR_SUCCEEDED(Create(mod, l, true)), "on: instance with the eye-gaze extension succeeds although the runtime has none");
  check(g_rtCreates == 2 && !HasExt(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) && HasExt("XR_KHR_D3D11_enable"), "on: retried at the runtime without it, the rest kept");
  check(active() == 1, "on: RigTap_SimGazeActive 1");
  auto sysProps = Get<PFN_xrGetSystemProperties>(l, "xrGetSystemProperties"); auto strToPath = Get<PFN_xrStringToPath>(l, "xrStringToPath");
  auto createSession = Get<PFN_xrCreateSession>(l, "xrCreateSession"); auto createRef = Get<PFN_xrCreateReferenceSpace>(l, "xrCreateReferenceSpace");
  auto createActionSpace = Get<PFN_xrCreateActionSpace>(l, "xrCreateActionSpace"); auto suggest = Get<PFN_xrSuggestInteractionProfileBindings>(l, "xrSuggestInteractionProfileBindings");
  auto getPose = Get<PFN_xrGetActionStatePose>(l, "xrGetActionStatePose"); auto locate = Get<PFN_xrLocateSpace>(l, "xrLocateSpace");
  auto curProfile = Get<PFN_xrGetCurrentInteractionProfile>(l, "xrGetCurrentInteractionProfile"); auto destroySession = Get<PFN_xrDestroySession>(l, "xrDestroySession");
  check(sysProps && strToPath && createSession && createRef && createActionSpace && suggest && getPose && locate && curProfile && destroySession, "on: every function resolved");
  check(locate != RtLocateSpace && getPose != RtGetPose && sysProps != RtGetSystemProperties, "on: the gaze functions are hooked");
  XrSystemEyeGazeInteractionPropertiesEXT eg{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT}; XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES}; sp.next = &eg;
  check(XR_SUCCEEDED(sysProps(l.inst, 1, &sp)) && eg.supportsEyeGazeInteraction == XR_TRUE, "on: supportsEyeGazeInteraction TRUE");
  XrSession s{}; XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO}; check(XR_SUCCEEDED(createSession(l.inst, &sci, &s)), "on: session");
  XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rci.poseInReferenceSpace = XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
  XrSpace local{}; createRef(s, &rci, &local);
  // Like Quad Views: the action space exists before the binding is suggested (at attach).
  const XrAction gazeAction = (XrAction)0x7001, handAction = (XrAction)0x7002;
  XrActionSpaceCreateInfo aci{XR_TYPE_ACTION_SPACE_CREATE_INFO}; aci.action = gazeAction; aci.poseInActionSpace = XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
  XrSpace gazeSpace{}, handSpace{}; createActionSpace(s, &aci, &gazeSpace); aci.action = handAction; createActionSpace(s, &aci, &handSpace);
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION}; g_rtActionLocates = 0;
  locate(gazeSpace, local, 1000, &loc); check(g_rtActionLocates == 1, "on: before the suggestion the gaze space is not known yet: forwarded");
  XrPath profile{}, gazePath{}, eyes{}, hand{}; strToPath(l.inst, "/interaction_profiles/ext/eye_gaze_interaction", &profile); strToPath(l.inst, "/user/eyes_ext/input/gaze_ext/pose", &gazePath);
  strToPath(l.inst, "/user/eyes_ext", &eyes); strToPath(l.inst, "/user/hand/left/input/grip/pose", &hand);
  XrActionSuggestedBinding b{gazeAction, gazePath}; XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING}; sb.interactionProfile = profile; sb.countSuggestedBindings = 1; sb.suggestedBindings = &b;
  g_rtSuggests = 0; check(XR_SUCCEEDED(suggest(l.inst, &sb)) && g_rtSuggests == 0, "on: eye-gaze suggestion succeeds, not forwarded to a runtime without the extension");
  XrActionSuggestedBinding hb{handAction, hand}; XrInteractionProfileSuggestedBinding hsb = sb; strToPath(l.inst, "/interaction_profiles/khr/simple_controller", &hsb.interactionProfile); hsb.suggestedBindings = &hb;
  check(XR_SUCCEEDED(suggest(l.inst, &hsb)) && g_rtSuggests == 1, "on: other profiles' suggestions are forwarded");
  XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = gazeAction; XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE}; g_rtPoseCalls = 0;
  check(XR_SUCCEEDED(getPose(s, &gi, &ps)) && ps.isActive == XR_TRUE && g_rtPoseCalls == 0, "on: the gaze action is active");
  gi.action = handAction; ps.isActive = XR_TRUE; check(XR_SUCCEEDED(getPose(s, &gi, &ps)) && ps.isActive == XR_FALSE && g_rtPoseCalls == 1, "on: other actions are the runtime's");
  XrInteractionProfileState ips{XR_TYPE_INTERACTION_PROFILE_STATE}; check(XR_SUCCEEDED(curProfile(s, eyes, &ips)) && ips.interactionProfile == profile, "on: /user/eyes_ext reports the eye-gaze profile");
  // Fixed x=10 y=-5 with the head yawed 30 deg left (+Y): world direction = head * gaze.
  XrEyeGazeSampleTimeEXT st{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT}; loc = {XR_TYPE_SPACE_LOCATION}; loc.next = &st; g_rtActionLocates = 0;
  check(XR_SUCCEEDED(locate(gazeSpace, local, 2'000'000'000, &loc)) && g_rtActionLocates == 0, "on: the gaze space is answered by the layer");
  check(loc.locationFlags == (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT), "on: valid and tracked");
  check(st.time == 2'000'000'000, "on: sample time = located time (latency 0)");
  check(approx(loc.pose.position.y, 1.6, 1e-6), "on: gaze origin = head position");
  { float o[3]; Dir(loc.pose, o); const double r = 3.14159265358979 / 180, gx = std::sin(10 * r) * std::cos(-5 * r), gy = std::sin(-5 * r), gz = -std::cos(10 * r) * std::cos(-5 * r);
    const double c = std::cos(30 * r), sn = std::sin(30 * r);  // yaw +30 about +Y: x' = c x + s z, z' = -s x + c z
    check(approx(o[0], c * gx + sn * gz, 1e-5) && approx(o[1], gy, 1e-5) && approx(o[2], -sn * gx + c * gz, 1e-5), "on: direction = head yaw x scripted gaze"); }
  loc = {XR_TYPE_SPACE_LOCATION}; g_rtActionLocates = 0; locate(handSpace, local, 2'000'000'000, &loc); check(g_rtActionLocates == 1, "on: other action spaces are forwarded");
  // Command file: a saccade program, picked up within a quarter second; its clock starts at the first locate.
  Sleep(300); WriteGaze(out, "mode=targets\ntargets=0,0|10,0\ndwell_ms=100\nloop=0\nlatency_ms=8\n"); Sleep(300);
  const XrTime T0 = 5'000'000'000; loc = {XR_TYPE_SPACE_LOCATION}; loc.next = &st; locate(gazeSpace, local, T0, &loc);
  check(GetFileAttributesA((out + "\\gaze.taken-1.txt").c_str()) != INVALID_FILE_ATTRIBUTES, "gaze.txt taken and renamed");
  check(st.time == T0 - 8'000'000, "latency_ms moves the sample time back");
  g_headYawDeg = 0; auto xAt = [&](XrTime t) { XrSpaceLocation q{XR_TYPE_SPACE_LOCATION}; locate(gazeSpace, local, t, &q); float o[3]; Dir(q.pose, o); return std::atan2(o[0], -o[2]) * 180 / 3.14159265358979; };
  check(approx(xAt(T0 + 50'000'000), 0, 1e-3) && approx(xAt(T0 + 100'000'000 + 21'500'000), 5, 0.01) && approx(xAt(T0 + 200'000'000), 10, 1e-3), "saccade through the layer: dwell, midpoint, landing");
  Sleep(300); WriteGaze(out, "mode=bogus\n"); Sleep(300); xAt(T0 + 300'000'000);
  check(GetFileAttributesA((out + "\\gaze.rejected-2.txt").c_str()) != INVALID_FILE_ATTRIBUTES && approx(xAt(T0 + 310'000'000), 10, 1e-3), "a bad gaze.txt is rejected, the program stays");
  Sleep(300); WriteGaze(out, "mode=lost\n"); Sleep(300);
  loc = {XR_TYPE_SPACE_LOCATION}; st.time = 1; loc.next = &st; locate(gazeSpace, local, T0 + 400'000'000, &loc);
  gi.action = gazeAction; ps.isActive = XR_FALSE; getPose(s, &gi, &ps);
  check(loc.locationFlags == 0 && st.time == 0 && ps.isActive == XR_TRUE, "lost: action active, location without flags (like nobody wearing the headset)");
  Sleep(300); WriteGaze(out, "mode=fixed\n"); Sleep(300); g_headLost = true;
  loc = {XR_TYPE_SPACE_LOCATION}; locate(gazeSpace, local, T0 + 500'000'000, &loc); check(loc.locationFlags == 0, "head not tracked: no gaze");
  g_headLost = false; loc = {XR_TYPE_SPACE_LOCATION}; locate(gazeSpace, local, T0 + 510'000'000, &loc); check(loc.locationFlags != 0, "head tracked again: gaze");
  Sleep(300); WriteGaze(out, "mode=off\n"); Sleep(300); g_rtActionLocates = 0; g_rtPoseCalls = 0;
  loc = {XR_TYPE_SPACE_LOCATION}; locate(gazeSpace, local, T0 + 600'000'000, &loc); getPose(s, &gi, &ps);
  check(g_rtActionLocates == 1 && g_rtPoseCalls == 1 && loc.locationFlags == 0 && active() == 0, "mode=off: the runtime's own answers pass through");
  destroySession(s);

  // -- 3. On, runtime WITH the extension (a tracker that is merely absent): forwarded, still overridden.
  g_offered = true; Layer l2; g_rtCreates = 0; SetEnvironmentVariableA("RIGTAP_SIMGAZE_PROGRAM", "mode=fixed;x=-7");
  check(XR_SUCCEEDED(Create(mod, l2, true)) && g_rtCreates == 1 && HasExt(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME), "offered: the extension reaches the runtime");
  { auto cs = Get<PFN_xrCreateSession>(l2, "xrCreateSession"); auto cr = Get<PFN_xrCreateReferenceSpace>(l2, "xrCreateReferenceSpace"); auto cas = Get<PFN_xrCreateActionSpace>(l2, "xrCreateActionSpace");
    auto sg = Get<PFN_xrSuggestInteractionProfileBindings>(l2, "xrSuggestInteractionProfileBindings"); auto lo = Get<PFN_xrLocateSpace>(l2, "xrLocateSpace");
    XrSession s2{}; cs(l2.inst, &sci, &s2); XrSpace loc2{}; cr(s2, &rci, &loc2); aci.action = gazeAction; XrSpace g2{}; cas(s2, &aci, &g2);
    g_rtSuggests = 0; check(XR_SUCCEEDED(sg(l2.inst, &sb)) && g_rtSuggests == 1, "offered: the eye-gaze suggestion is forwarded");
    g_headYawDeg = 0; XrSpaceLocation q{XR_TYPE_SPACE_LOCATION}; g_rtActionLocates = 0; lo(g2, loc2, 9'000'000'000, &q); float o[3]; Dir(q.pose, o);
    check(g_rtActionLocates == 0 && q.locationFlags != 0 && approx(std::atan2(o[0], -o[2]) * 180 / 3.14159265358979, -7, 1e-3), "offered: the scripted gaze replaces the runtime's"); }

  const std::string log = ReadAll(out + "\\rigtap.log"), trace = ReadAll(out + "\\simgaze_trace.csv");
  check(log.find("SIMULATED EYE GAZE ON") != std::string::npos && log.find("runtime says supportsEyeGazeInteraction=0") != std::string::npos, "the log says it loudly");
  check(log.find("rejected (unknown mode: bogus)") != std::string::npos, "the log names a rejected program");
  check(std::count(trace.begin(), trace.end(), '\n') > 8, "the trace holds the served samples");
  std::printf("simgaze_harness: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
