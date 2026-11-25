// Minimal pass-through OpenXR API layer that records its position in the chain.
// Built twice with different PROBE_NAME values to observe the loader's real layer order.
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <cstring>
#include <string>

#ifndef PROBE_NAME
#error PROBE_NAME is required
#endif

namespace {
PFN_xrGetInstanceProcAddr next_gipa{};

void record() {
    char current[1024]{};
    GetEnvironmentVariableA("DCSVR_PROBE_ORDER", current, sizeof(current));
    std::string order = current; order += PROBE_NAME; order += ';';
    SetEnvironmentVariableA("DCSVR_PROBE_ORDER", order.c_str());
}

XrResult XRAPI_CALL probe_gipa(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (std::strcmp(name, "xrGetInstanceProcAddr") == 0) { *function = reinterpret_cast<PFN_xrVoidFunction>(probe_gipa); return XR_SUCCESS; }
#ifdef PROBE_FAULT_NULL
    // As Ultraleap's hand tracking layer (GitHub #35): a null instance is dereferenced.
    if (instance == XR_NULL_HANDLE) { volatile int* nothing = nullptr; *nothing = 1; }
#endif
    return next_gipa ? next_gipa(instance, name, function) : XR_ERROR_HANDLE_INVALID;
}

XrResult XRAPI_CALL probe_create(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layer, XrInstance* instance) {
    if (!layer || !layer->nextInfo) return XR_ERROR_INITIALIZATION_FAILED;
    record();
    next_gipa = layer->nextInfo->nextGetInstanceProcAddr;
    XrApiLayerCreateInfo chained = *layer;
    chained.nextInfo = layer->nextInfo->next;
    return layer->nextInfo->nextCreateApiLayerInstance(info, &chained, instance);
}
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader, const char*, XrNegotiateApiLayerRequest* request) {
    if (!loader || !request || loader->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION) return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_CURRENT_API_VERSION;
    request->getInstanceProcAddr = probe_gipa;
    request->createApiLayerInstance = probe_create;
    return XR_SUCCESS;
}
