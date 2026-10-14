// DCS Control: Quad Views' side of the Far depth engine export (dcsvr_depth_export.h): the pushed focus views,
// latched with a reference of their own, and the per-frame validation that decides whether both eyes may be warped
// (never one eye alone, never stale depth). C++ only; tested by tests/depthwarp.
#ifndef DCSVR_DEPTH_IMPORT_H
#define DCSVR_DEPTH_IMPORT_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "dcsvr_depth_export.h"
#include "dcsvr_depthwarp.h"

namespace dcsvr {

// Why a frame's depth was not used (statistics, one counter each).
enum class DepthReject : uint8_t {
    None,
    Missing,       // a focus view was not pushed since the previous frame
    Frame,         // the two views are from different engine frames
    Generation,    // the two views of different generations (one from before a reload)
    Flags,         // not VALID, not REVERSE_Z
    Projection,    // gProj not a reverse-Z perspective
    Aspect,        // the viewport's aspect is not the focus image's
    DisplayTime,   // the engine's display time is not this frame's
    Swap,          // left and right exchanged (camera positions against camera right)
    Viewport,      // the two eyes' viewports of different sizes
    Coverage,      // no pre-overlay copy while Far depth requires it (the HUD would move)
    Count
};

// No 16-bit depth: raw reverse-Z D16 with a 0.02 m near plane has a single step for all of 0.9-2.6 km and reads 0 (the
// sky) beyond, exactly the band Far depth is for.
inline bool DepthFormatSupported(uint32_t f) {
    return f == DXGI_FORMAT_R32G8X24_TYPELESS || f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_R32_TYPELESS;
}
// The pre-overlay copy (HAS_PRE_OVERLAY): the colour formats an overlay render target can have, and whether a texture
// of format t can be viewed as v (the same format, or v in t's typeless family).
inline bool PreOverlayViewFormatSupported(uint32_t v) {
    switch (v) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT: return true;
    default: return false;
    }
}
inline bool PreOverlayViewCompatible(uint32_t t, uint32_t v) {
    if (!PreOverlayViewFormatSupported(v)) return false;
    if (t == v) return true;
    switch (t) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return v == DXGI_FORMAT_R8G8B8A8_UNORM || v == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return v == DXGI_FORMAT_B8G8R8A8_UNORM || v == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return v == DXGI_FORMAT_B8G8R8X8_UNORM || v == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return v == DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return v == DXGI_FORMAT_R16G16B16A16_FLOAT || v == DXGI_FORMAT_R16G16B16A16_UNORM;
    default: return false;
    }
}

// The SRV formats of a typeless depth format: depth (red) and stencil (green, UNKNOWN when there is none).
inline void DepthSrvFormats(uint32_t f, DXGI_FORMAT& depth, DXGI_FORMAT& stencil) {
    stencil = DXGI_FORMAT_UNKNOWN;
    switch (f) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        depth = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        stencil = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        break;
    case DXGI_FORMAT_R24G8_TYPELESS:
        depth = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        stencil = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        break;
    case DXGI_FORMAT_R32_TYPELESS: depth = DXGI_FORMAT_R32_FLOAT; break;
    default: depth = DXGI_FORMAT_UNKNOWN; break;
    }
}

// One latched view: the descriptor (pointer fields aside) and Quad Views' own references to the textures.
struct DepthViewLatch {
    DcsVrDepthView desc{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, preOverlay;
    bool fresh{false};  // pushed since the last frame consumed it
};

struct DepthFrame {
    DepthReject reject{DepthReject::Missing};
    const DepthViewLatch* view[2]{};  // left and right focus
    ProjectionParams proj[2]{};
    bool preOverlay{false};           // both views carry a pre-overlay copy
};

class DepthImport {
  public:
    static constexpr uint32_t WantedFocus = 0b1100u;

    // The device and thread pushes must come from (Quad Views' application device, DCS's render thread: the thread
    // of xrEndFrame). Thread 0: not known yet, any thread. When the application's device is a proxy (ReShade wraps
    // the device, not the resources), a texture made on it reports the device underneath: a probe texture created
    // here tells which one, and both are accepted.
    void SetDevice(ID3D11Device* device) {
        ID3D11Device* resourceDevice = nullptr;  // identity only: no reference held
        if (device) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = td.Height = td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8_UNORM;
            td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> probe;
            Microsoft::WRL::ComPtr<ID3D11Device> reported;
            if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &probe))) probe->GetDevice(&reported);
            resourceDevice = reported.Get();
        }
        std::lock_guard lock(m_mutex);
        m_device = device;
        m_resourceDevice = resourceDevice;
    }
    void SetRenderThread(DWORD thread) { m_thread.store(thread, std::memory_order_relaxed); }
    void SetWanted(uint32_t mask) { m_wanted.store(mask, std::memory_order_relaxed); }
    uint32_t Wanted() const { return m_wanted.load(std::memory_order_relaxed); }

    // DcsVr_QuadViewsDepth: checks, AddRefs the textures and drops the view's previous ones. No GPU work.
    uint32_t Push(const DcsVrDepthView* d) {
        if (!d || d->structSize < sizeof(DcsVrDepthView) || d->version < 1 || !d->texture || d->hmdView > 3)
            return DCSVR_DEPTH_PUSH_NULL_OR_SIZE;
        const DWORD thread = m_thread.load(std::memory_order_relaxed);
        if (thread && thread != GetCurrentThreadId()) return DCSVR_DEPTH_PUSH_NOT_RENDER_THREAD;
        if (!(Wanted() & (1u << d->hmdView))) return DCSVR_DEPTH_PUSH_NOT_WANTED;
        if (d->sampleCount != 1) return DCSVR_DEPTH_PUSH_MSAA;
        if (!DepthFormatSupported(d->format)) return DCSVR_DEPTH_PUSH_FORMAT;
        D3D11_TEXTURE2D_DESC td{};
        d->texture->GetDesc(&td);
        if (td.SampleDesc.Count != 1) return DCSVR_DEPTH_PUSH_MSAA;
        if (td.Format != (DXGI_FORMAT)d->format || !(td.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return DCSVR_DEPTH_PUSH_FORMAT;
        // One slice, one mip: the field shader reads a Texture2D (an array would need another view dimension).
        if (d->arraySlice != 0 || td.ArraySize != 1 || td.MipLevels != 1) return DCSVR_DEPTH_PUSH_LAYOUT;
        ID3D11Texture2D* preOverlay = nullptr;
        if (d->flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) {
            preOverlay = d->preOverlay;
            if (!preOverlay) return DCSVR_DEPTH_PUSH_NULL_OR_SIZE;
            D3D11_TEXTURE2D_DESC cd{};
            preOverlay->GetDesc(&cd);
            if (cd.SampleDesc.Count != 1) return DCSVR_DEPTH_PUSH_MSAA;
            if (!PreOverlayViewCompatible(cd.Format, d->preOverlayFormat) || !(cd.BindFlags & D3D11_BIND_SHADER_RESOURCE))
                return DCSVR_DEPTH_PUSH_FORMAT;
            const uint32_t w = d->preOverlaySize & 0xffffu, h = d->preOverlaySize >> 16;
            const uint32_t needW = (d->flags & DCSVR_DEPTH_PRE_POST_OVERLAY) ? 2 * w : w;
            if (!w || !h || cd.Width < needW || cd.Height < h || cd.ArraySize != 1 || cd.MipLevels != 1) return DCSVR_DEPTH_PUSH_LAYOUT;
        } else if (d->flags & DCSVR_DEPTH_PRE_POST_OVERLAY) {
            return DCSVR_DEPTH_PUSH_LAYOUT;  // the pair needs the copy it is part of
        }
        std::lock_guard lock(m_mutex);
        if (!OnDevice(d->texture) || (preOverlay && !OnDevice(preOverlay))) return DCSVR_DEPTH_PUSH_FOREIGN_DEVICE;
        DepthViewLatch& latch = m_views[d->hmdView];
        std::memcpy(&latch.desc, d, sizeof(DcsVrDepthView));
        latch.desc.texture = latch.desc.preOverlay = nullptr;  // only the ComPtrs own them
        if (!preOverlay) latch.desc.flags &= ~(uint32_t)(DCSVR_DEPTH_HAS_PRE_OVERLAY | DCSVR_DEPTH_PRE_POST_OVERLAY);
        latch.texture = d->texture;                            // AddRef; the previous texture is released
        latch.preOverlay = preOverlay;
        latch.fresh = true;
        ++m_accepted;
        return DCSVR_DEPTH_PUSH_ACCEPTED;
    }

    // DcsVr_QuadViewsDepthRelease: drops the held references of that generation (an opaque id, not an order).
    void Release(uint32_t generation) {
        std::lock_guard lock(m_mutex);
        for (auto& v : m_views)
            if (v.texture && v.desc.generation == generation) Drop(v);
    }
    // The session end.
    void ReleaseAll() {
        std::lock_guard lock(m_mutex);
        for (auto& v : m_views) Drop(v);
        m_lastTime = 0;
        m_last = {};
    }

    // Once per xrEndFrame, before any dispatch: both focus views fresh, of one engine frame and one generation,
    // flagged valid reverse-Z, a reverse-Z perspective, viewports of one size and of the focus image's aspect
    // (focusAspect = width / height), this display time when the engine knows it, left left of right, and both with
    // a pre-overlay copy when requireCoverage. Consumes the pushes: the next frame needs new ones. Another call for the
    // same (non-zero) display time, a second projection layer, returns the same frame and consumes nothing.
    DepthFrame Validate(int64_t displayTime, float focusAspect, bool requireCoverage = false) {
        std::lock_guard lock(m_mutex);
        if (displayTime != 0 && displayTime == m_lastTime) return m_last;
        DepthFrame f;
        f.reject = Check(displayTime, focusAspect, requireCoverage, f);
        for (auto& v : m_views) v.fresh = false;
        m_rejects[(int)f.reject]++;
        m_lastTime = displayTime;
        m_last = f;
        return f;
    }

    uint64_t Rejects(DepthReject r) const { return m_rejects[(int)r]; }
    uint64_t Accepted() const { return m_accepted; }
    const DepthViewLatch& View(int hmdView) const { return m_views[hmdView]; }

  private:
    // Made on Quad Views' device: the one the application handed over, or the one its resources report (a proxy).
    bool OnDevice(ID3D11Texture2D* t) const {
        Microsoft::WRL::ComPtr<ID3D11Device> owner;
        t->GetDevice(&owner);
        return owner && m_device && (owner.Get() == m_device || owner.Get() == m_resourceDevice);
    }
    static void Drop(DepthViewLatch& v) {
        v.texture.Reset();
        v.preOverlay.Reset();
        v.fresh = false;
    }

    DepthReject Check(int64_t displayTime, float focusAspect, bool requireCoverage, DepthFrame& f) const {
        const DepthViewLatch& l = m_views[2];
        const DepthViewLatch& r = m_views[3];
        if (!l.fresh || !r.fresh || !l.texture || !r.texture) return DepthReject::Missing;
        if (l.desc.engineFrame != r.desc.engineFrame) return DepthReject::Frame;
        // Freshness and the engine frame already tie both views to this frame; their generations only have to agree
        // (a counter that a reload restarts lower is as good as a higher one).
        if (l.desc.generation != r.desc.generation) return DepthReject::Generation;
        for (const auto* v : {&l, &r}) {
            const uint32_t need = DCSVR_DEPTH_VALID | DCSVR_DEPTH_REVERSE_Z;
            if ((v->desc.flags & need) != need) return DepthReject::Flags;
        }
        for (int e = 0; e < 2; ++e) {
            const DepthViewLatch& v = e ? r : l;
            f.proj[e] = ProjectionParams::Parse(v.desc.proj);
            if (!f.proj[e].ok || !f.proj[e].reverse) return DepthReject::Projection;
            const float w = v.desc.viewport[2], h = v.desc.viewport[3];
            if (!(w >= 16.f && h >= 16.f) || !(focusAspect > 0.f) || std::fabs((w / h) / focusAspect - 1.f) > 0.01f)
                return DepthReject::Aspect;
            if (v.desc.displayTime != 0 && displayTime != 0 && v.desc.displayTime != displayTime) return DepthReject::DisplayTime;
        }
        // The two eyes' fields are built and composed alike: one size.
        if (std::lround(l.desc.viewport[2]) != std::lround(r.desc.viewport[2]) || std::lround(l.desc.viewport[3]) != std::lround(r.desc.viewport[3]))
            return DepthReject::Viewport;
        if ((l.desc.flags & r.desc.flags & DCSVR_DEPTH_VIEWINV_VALID) != 0) {
            // Camera right (gViewInv row 0) of the left view; the right camera must be on its positive side.
            const float* a = l.desc.viewInv;
            const float* b = r.desc.viewInv;
            const float dx = b[12] - a[12], dy = b[13] - a[13], dz = b[14] - a[14];
            if (dx * a[0] + dy * a[1] + dz * a[2] <= 0.f) return DepthReject::Swap;
        }
        f.preOverlay = l.preOverlay && r.preOverlay;
        if (requireCoverage && !f.preOverlay) return DepthReject::Coverage;
        f.view[0] = &l;
        f.view[1] = &r;
        return DepthReject::None;
    }

    mutable std::mutex m_mutex;
    ID3D11Device* m_device{nullptr};
    ID3D11Device* m_resourceDevice{nullptr};
    std::atomic<DWORD> m_thread{0};
    std::atomic<uint32_t> m_wanted{0};
    DepthViewLatch m_views[4]{};
    int64_t m_lastTime{0};
    DepthFrame m_last{};
    uint64_t m_accepted{0};
    uint64_t m_rejects[(int)DepthReject::Count]{};
};

// One import for the layer and its exports (the layer is a single instance per process, and the engine reaches the
// exports with GetModuleHandle + GetProcAddress, without an instance to name).
inline DepthImport& SharedDepthImport() {
    static DepthImport import;
    return import;
}

} // namespace dcsvr

#endif // DCSVR_DEPTH_IMPORT_H
