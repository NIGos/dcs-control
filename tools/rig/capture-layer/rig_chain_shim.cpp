// Test-only: lets a program that loads ONE layer DLL (OFXR's tests\layer_call_chain.cpp: xrfg_layer_call_chain.exe
// <layer dll> <log> <scenario>, with its fake runtime) run a chain of two: the upper layer (OFXR) and, below it,
// the lower layer (the rig capture layer). The test loads this DLL instead of OFXR's.
//   RIGSHIM_UPPER = full path of the upper layer's DLL, RIGSHIM_LOWER = full path of the lower layer's DLL.
#include <windows.h>
#include <cstring>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
static XrNegotiateApiLayerRequest g_up{}, g_low{}; static char g_name[XR_MAX_API_LAYER_NAME_SIZE];
static XrResult XRAPI_CALL ShimCreate(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* li, XrInstance* inst) {
  if (!li || !li->nextInfo) return XR_ERROR_INITIALIZATION_FAILED;
  XrApiLayerNextInfo low = *li->nextInfo;                       // same header fields
  strcpy_s(low.layerName, g_name); low.nextGetInstanceProcAddr = g_low.getInstanceProcAddr; low.nextCreateApiLayerInstance = g_low.createApiLayerInstance; low.next = li->nextInfo;
  XrApiLayerCreateInfo up = *li; up.nextInfo = &low;
  return g_up.createApiLayerInstance(info, &up, inst);          // OFXR -> capture layer -> the caller's next (the fake runtime)
}
extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* li, const char* name, XrNegotiateApiLayerRequest* req) {
  if (!li || !req) return XR_ERROR_INITIALIZATION_FAILED;
  char up[1024], low[1024]; if (!GetEnvironmentVariableA("RIGSHIM_UPPER", up, sizeof(up)) || !GetEnvironmentVariableA("RIGSHIM_LOWER", low, sizeof(low))) return XR_ERROR_INITIALIZATION_FAILED;
  HMODULE mu = LoadLibraryA(up), ml = LoadLibraryA(low); if (!mu || !ml) return XR_ERROR_INITIALIZATION_FAILED;
  auto nu = (PFN_xrNegotiateLoaderApiLayerInterface)GetProcAddress(mu, "xrNegotiateLoaderApiLayerInterface"), nl = (PFN_xrNegotiateLoaderApiLayerInterface)GetProcAddress(ml, "xrNegotiateLoaderApiLayerInterface");
  if (!nu || !nl) return XR_ERROR_INITIALIZATION_FAILED;
  strncpy(g_name, name ? name : "", sizeof(g_name) - 1);
  g_up = *req; g_low = *req;
  if (XR_FAILED(nu(li, name, &g_up)) || XR_FAILED(nl(li, "XR_APILAYER_RIG_capture", &g_low))) return XR_ERROR_INITIALIZATION_FAILED;
  *req = g_up; req->createApiLayerInstance = ShimCreate; return XR_SUCCESS;
}
