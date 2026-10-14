// DCS Control: Far depth engine export v1.1, the contract between DCS Control's engine module (DcsQvCull, which copies
// it verbatim) and the bundled Quad Views compositor (docs/FAR_DEPTH.md has the full specification).
//
// The engine copies each wanted HMD view's depth+stencil, whole subresource and raw, into its own texture at the end
// of that view's WaterSeaPass (D3D11 forbids a source box on depth-stencil resources), then pushes this descriptor to
// Quad Views on the render thread. Quad Views AddRefs the texture and reads it at xrEndFrame on the same immediate
// context, so D3D11 orders the copy before the read: no fence, mutex or query. The engine never changes DCS's image or
// bindings (CopySubresourceRegion touches no pipeline state).
//
// Self-contained: <cstdint> and a forward declaration only. #pragma pack(8), little-endian.
#ifndef DCSVR_DEPTH_EXPORT_H
#define DCSVR_DEPTH_EXPORT_H

#include <cstdint>

struct ID3D11Texture2D;

#pragma pack(push, 8)

enum : uint32_t {
    DCSVR_DEPTH_VALID = 1,
    DCSVR_DEPTH_REVERSE_Z = 2,
    DCSVR_DEPTH_INFINITE_FAR = 4,
    DCSVR_DEPTH_AFTER_WATER = 8,
    DCSVR_DEPTH_FALLBACK_CAPTURE = 16,        // flushed at GBufferLightingPass, the next view or the frame end
    DCSVR_DEPTH_HAS_STENCIL = 32,
    DCSVR_DEPTH_VIEWINV_VALID = 64,
    DCSVR_DEPTH_JITTER_KNOWN = 128,
    DCSVR_DEPTH_IDENTITY_BY_COPYTOHMD = 256,  // ordinal from the CopyToHMDPass order + MatchFov
    DCSVR_DEPTH_HAS_PRE_OVERLAY = 512,        // v1.1: preOverlay, preOverlayFormat and preOverlaySize are set
    DCSVR_DEPTH_PRE_POST_OVERLAY = 1024       // v1.1, with HAS_PRE_OVERLAY: preOverlay also holds the colour after them
};

struct DcsVrDepthView {
    uint32_t structSize;   // sizeof(DcsVrDepthView); Quad Views rejects smaller sizes and ignores the tail of larger ones
    uint32_t version;      // 1
    uint32_t hmdView;      // 0 L periphery, 1 R periphery, 2 L focus, 3 R focus (perception_shm.h order)
    uint32_t flags;
    uint64_t engineFrame;  // engine frame counter, the same for all views of one DCS frame
    int64_t displayTime;   // the displayTime of the xrLocateViews call whose FOV DCS rendered this view with, only when
                           // the engine knows that pairing; else 0 (never "the newest GazeOut record")
    uint32_t generation;   // an opaque id, changed on payload load, device change, texture recreation; need not grow
    uint32_t format;       // DXGI_FORMAT of texture: R32G8X24_TYPELESS (expected), R24G8_TYPELESS or R32_TYPELESS
    ID3D11Texture2D* texture;  // the engine's copy on DCS's device, ArraySize 1, MipLevels 1; Quad Views AddRefs it
    uint32_t texWidth, texHeight, arraySlice, sampleCount;  // arraySlice must be 0 (copy into subresource 0), sampleCount 1
    float viewport[6];     // x, y, w, h, MinDepth, MaxDepth of THIS view inside the texture (texels)
    float proj[16];        // gProj as cPerView holds it at byte 256, this frame (logical P[r][c] at [r * 4 + c])
    float viewInv[16];     // gViewInv this frame (rows 0-2 rotation, row 3 camera position), if VIEWINV_VALID
    float jitterPx[2];     // DLSS jitter in internal pixels (+x right, +y down) this frame, only if JITTER_KNOWN
    float fovTan[4];       // left, right, up, down tangents of DCS's frustum (DcsFov) for this view: from gProj, so
                           // they carry the jitter and cannot serve as the unjittered centre
    uint32_t outputWidth, outputHeight;  // this view's CopyToHMDPass target size if known, else 0
    // v1.1, with HAS_PRE_OVERLAY: this view's colour as it was before the first overlay-class pass (GUIPass or
    // OverlayLDRPass) drew into it, copied from that render target's viewport box (x, y, w, h) to (0, 0) of the engine's
    // own texture. Quad Views compares it with the focus image DCS submits: every pixel the overlays changed (HUD and
    // helmet symbology, gunsight, GUI) pins the warp. The texture: at least w x h, ArraySize 1, MipLevels 1, SampleCount
    // 1, SHADER_RESOURCE, the typeless family of the render target (else its own format, e.g. R11G11B10_FLOAT). Not
    // set when the target is MSAA or the copy could not be made: the depth is pushed alone.
    // With PRE_POST_OVERLAY the same texture also holds the colour after the overlay passes, copied from the same box at
    // the end of the last one (before CopyToHMDPass) to (w, 0): the texture is then at least 2w x h, and Quad Views
    // compares the two halves (one target, one format) instead of the submitted image.
    ID3D11Texture2D* preOverlay;
    uint32_t preOverlayFormat;  // DXGI_FORMAT of the render target view the overlays draw with (e.g. R8G8B8A8_UNORM_SRGB)
    uint32_t preOverlaySize;    // w | (h << 16): the copied box, in output pixels
};
static_assert(sizeof(DcsVrDepthView) == 264, "DcsVrDepthView layout (v1.1)");

#pragma pack(pop)

// Quad Views' exports (XR_APILAYER_MBUCCHIA_quad_views_foveated.dll), resolved with GetModuleHandle + GetProcAddress,
// noexcept, extern "C", render thread only:
//   uint32_t DcsVr_QuadViewsDepthApi();                         1, the interface version
//   uint32_t DcsVr_QuadViewsDepthWanted();                      bit mask of wanted hmdViews (0b1100 or 0), one atomic load
//   uint32_t DcsVr_QuadViewsDepth(const DcsVrDepthView* d);     1 accepted, else a DcsVrDepthPushResult code
//   void     DcsVr_QuadViewsDepthRelease(uint32_t generation);  drop every held texture of that generation
typedef uint32_t (*PFN_DcsVr_QuadViewsDepthApi)();
typedef uint32_t (*PFN_DcsVr_QuadViewsDepthWanted)();
typedef uint32_t (*PFN_DcsVr_QuadViewsDepth)(const DcsVrDepthView*);
typedef void (*PFN_DcsVr_QuadViewsDepthRelease)(uint32_t);

enum DcsVrDepthPushResult : uint32_t {
    DCSVR_DEPTH_PUSH_NULL_OR_SIZE = 0,
    DCSVR_DEPTH_PUSH_ACCEPTED = 1,
    DCSVR_DEPTH_PUSH_FOREIGN_DEVICE = 2,
    DCSVR_DEPTH_PUSH_FORMAT = 3,
    DCSVR_DEPTH_PUSH_MSAA = 4,
    DCSVR_DEPTH_PUSH_NOT_WANTED = 5,
    DCSVR_DEPTH_PUSH_NOT_RENDER_THREAD = 6,
    DCSVR_DEPTH_PUSH_LAYOUT = 7,  // arraySlice != 0, ArraySize or MipLevels != 1, a pre-overlay texture smaller than preOverlaySize
                                  // (2w wide with PRE_POST_OVERLAY), or PRE_POST_OVERLAY without HAS_PRE_OVERLAY
};

#endif // DCSVR_DEPTH_EXPORT_H
