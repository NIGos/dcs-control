// Three views (native/quadviews/dcsvr_threeviews.h): the real fill pass on a D3D11 device (the GPU, else WARP) over a
// synthetic pair of parallel periphery views with DCS's frusta (left -56.8..+45.9 deg, right mirrored, 64 mm apart):
// a textured wall at 10 m and a cockpit panel at 0.6 m (stencil COCKPIT) seen by both eyes. Checks:
//   1. the matrices: a world point's left NDC maps to its right NDC (double-precision transform, world origin far away);
//   2. the strip and the right view's own cockpit pixels are DCS's, bit for bit;
//   3. the filled pixels match the right view rendered directly (the wall) within a small error;
//   4. the panel never appears in the right view's world away from its edge (the wall the right eye sees past
//      it, hidden from the left eye, takes the background);
//   5. without the pushes (no THREE_VIEWS flag) nothing is filled.
// Usage: threeviews_test.exe
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "dcsvr_threeviews.h"

using Microsoft::WRL::ComPtr;
using namespace dcsvr;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

struct Cam {
    double l, r, u, d, x;  // tangents, camera x (world)
};
static const double kNear = 0.05, kWallZ = 10.0, kOrigin = 2000.0;  // camera-relative origin (gViewInv rows are floats: 2 km keeps the 64 mm exact to 0.1 mm)
static double Wall(double X, double Y) { return 0.5 + 0.35 * std::sin(X * 2.0) * std::cos(Y * 1.5); }

static std::array<float, 16> Proj(const Cam& c) {
    std::array<float, 16> m{};
    m[0] = float(2 / (c.r - c.l));
    m[5] = float(2 / (c.u - c.d));
    m[8] = float(-(c.r + c.l) / (c.r - c.l));
    m[9] = float(-(c.u + c.d) / (c.u - c.d));
    m[11] = 1.f;
    m[14] = float(kNear);
    return m;
}

int main() {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        printf("FAIL no D3D11 device\n");
        return 1;
    }
    const double t1 = std::tan(56.8 * 3.14159265358979 / 180), t2 = std::tan(45.9 * 3.14159265358979 / 180),
                 tv = std::tan(50.0 * 3.14159265358979 / 180);
    const Cam camL{-t1, t2, tv, -tv, kOrigin - 0.032}, camR{-t2, t1, tv, -tv, kOrigin + 0.032};
    const UINT W = 538, H = 530, DW = 312, DH = 308;  // half of DCS's 1076x1060 colour and 624x616 depth
    const float strip = 0.55f;
    // The cockpit panel, in the world at 0.6 m: what the left eye sees at tangents x [-0.35, -0.05], y [-0.5, -0.2].
    auto panel = [&](const Cam& c, double tx, double ty) {
        const double X = c.x + tx * 0.6 - camL.x;
        return X > -0.35 * 0.6 && X < -0.05 * 0.6 && ty > -0.5 && ty < -0.2;
    };
    auto render = [&](const Cam& c, bool left, UINT w, UINT h, std::vector<float>* col, std::vector<uint8_t>* depth) {
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                const double u = (x + 0.5) / w, v = (y + 0.5) / h;
                const double tx = c.l + u * (c.r - c.l), ty = c.u + v * (c.d - c.u);
                bool ck = false;
                double z = kWallZ, val = Wall(c.x + tx * kWallZ - kOrigin, ty * kWallZ);
                if (panel(c, tx, ty)) ck = true, z = 0.6, val = 1.0;
                if (!left && tx > 0.6 && tx < 0.8 && ty > -0.6 && ty < -0.3) ck = true, z = 0.6, val = 0.0;  // its own cockpit
                if (col) {
                    float* p = &(*col)[(y * w + x) * 4];
                    p[0] = p[1] = p[2] = float(val), p[3] = 1.f;
                }
                if (depth) {
                    uint8_t* p = &(*depth)[(y * w + x) * 8];
                    const float d = float(kNear / z);
                    memcpy(p, &d, 4);
                    p[4] = ck ? 0x28 : 0x08, p[5] = p[6] = p[7] = 0;
                }
            }
    };
    std::vector<float> colL(W * H * 4), colR(W * H * 4), colRFill(W * H * 4);
    std::vector<uint8_t> depL(DW * DH * 8), depR(DW * DH * 8);
    render(camL, true, W, H, &colL, nullptr);
    render(camR, false, W, H, &colR, nullptr);
    render(camL, true, DW, DH, nullptr, &depL);
    render(camR, false, DW, DH, nullptr, &depR);
    // DCS's culled right view: outside the strip and its cockpit, sky (0.25).
    colRFill = colR;
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x) {
            const double u = (x + 0.5) / W, v = (y + 0.5) / H;
            const double tx = camR.l + u * (camR.r - camR.l), ty = camR.u + v * (camR.d - camR.u);
            const bool own = tx > 0.6 && tx < 0.8 && ty > -0.6 && ty < -0.3;
            if (u * 2 - 1 < strip && !own)
                for (int k = 0; k < 3; ++k) colRFill[(y * W + x) * 4 + k] = 0.25f;
        }
    auto texture = [&](DXGI_FORMAT f, UINT w, UINT h, const void* data, UINT pitch) {
        D3D11_TEXTURE2D_DESC d{w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA s{data, pitch, 0};
        ComPtr<ID3D11Texture2D> t;
        dev->CreateTexture2D(&d, &s, &t);
        return t;
    };
    std::vector<uint16_t> h16L(W * H * 4), h16R(W * H * 4);
    auto toHalf = [](float f) {
        uint32_t x;
        memcpy(&x, &f, 4);
        const uint32_t s = (x >> 16) & 0x8000, e = ((x >> 23) & 0xff), m = x & 0x7fffff;
        if (e < 113) return uint16_t(s);
        return uint16_t(s | ((e - 112) << 10) | (m >> 13));
    };
    for (size_t i = 0; i < h16L.size(); ++i) h16L[i] = toHalf(colL[i]), h16R[i] = toHalf(colRFill[i]);
    auto tL = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, h16L.data(), W * 8);
    auto tR = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, h16R.data(), W * 8);
    auto dL = texture(DXGI_FORMAT_R32G8X24_TYPELESS, DW, DH, depL.data(), DW * 8);
    auto dR = texture(DXGI_FORMAT_R32G8X24_TYPELESS, DW, DH, depR.data(), DW * 8);
    ComPtr<ID3D11ShaderResourceView> sL, sR;
    dev->CreateShaderResourceView(tL.Get(), nullptr, &sL);
    dev->CreateShaderResourceView(tR.Get(), nullptr, &sR);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> samp;
    dev->CreateSamplerState(&sd, &samp);

    auto desc = [&](uint32_t view, const Cam& c, ID3D11Texture2D* t, bool flag) {
        DcsVrDepthView d{};
        d.structSize = sizeof(d);
        d.version = 1;
        d.hmdView = view;
        d.flags = DCSVR_DEPTH_VALID | DCSVR_DEPTH_REVERSE_Z | DCSVR_DEPTH_INFINITE_FAR | DCSVR_DEPTH_HAS_STENCIL |
                  DCSVR_DEPTH_VIEWINV_VALID | (flag ? DCSVR_DEPTH_THREE_VIEWS : 0u);
        d.engineFrame = 7;
        d.generation = 3;
        d.format = DXGI_FORMAT_R32G8X24_TYPELESS;
        d.texture = t;
        d.texWidth = DW, d.texHeight = DH, d.sampleCount = 1;
        d.viewport[2] = float(DW), d.viewport[3] = float(DH), d.viewport[5] = 1.f;
        const auto p = Proj(c);
        memcpy(d.proj, p.data(), sizeof(d.proj));
        d.viewInv[0] = d.viewInv[5] = d.viewInv[10] = d.viewInv[15] = 1.f;
        d.viewInv[12] = float(c.x);
        d.stripNdcX = strip;
        return d;
    };
    // 1. The matrices.
    {
        const DcsVrDepthView a = desc(0, camL, dL.Get(), false), b = desc(1, camR, dR.Get(), true);
        float m[16];
        const bool ok = ThreeViewsTransform(a, b, m);
        // world point (kOrigin + 0.7, 0.4, 3): left view (0.732, 0.4, 3), right (0.668, 0.4, 3)
        auto ndc = [](const Cam& c, double x, double y, double z) {
            const double tx = x / z, ty = y / z;
            return std::array<double, 3>{(tx - c.l) / (c.r - c.l) * 2 - 1, (ty - c.d) / (c.u - c.d) * 2 - 1, kNear / z};
        };
        const auto l = ndc(camL, 0.732, 0.4, 3.0), r = ndc(camR, 0.668, 0.4, 3.0);
        double v[4] = {};
        for (int j = 0; j < 4; ++j) v[j] = l[0] * m[0 * 4 + j] + l[1] * m[1 * 4 + j] + l[2] * m[2 * 4 + j] + m[3 * 4 + j];
        const double e = std::fabs(v[0] / v[3] - r[0]) + std::fabs(v[1] / v[3] - r[1]) + std::fabs(v[2] / v[3] - r[2]);
        Check(ok && e < 1e-4, "matrices: a point's left NDC maps to its right NDC (cameras 2 km from the origin)");
    }
    DepthImport imp;
    imp.SetDevice(dev.Get());
    imp.SetWanted(DepthImport::WantedThreeViews);
    ThreeViews pass;
    auto run = [&](bool flag) -> ID3D11ShaderResourceView* {
        DcsVrDepthView a = desc(0, camL, dL.Get(), false), b = desc(1, camR, dR.Get(), flag);
        imp.Push(&a), imp.Push(&b);
        pass.BeginFrame(imp);
        return pass.Fill(dev.Get(), ctx.Get(), sL.Get(), sR.Get(), W, H, samp.Get());
    };
    ID3D11ShaderResourceView* out = run(true);
    Check(out != nullptr, "the frame with both pushes and the flag is filled");
    if (!out) return 1;
    // Read back the result.
    ComPtr<ID3D11Resource> res;
    out->GetResource(&res);
    D3D11_TEXTURE2D_DESC st{W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    ComPtr<ID3D11Texture2D> stage;
    dev->CreateTexture2D(&st, nullptr, &stage);
    ctx->CopyResource(stage.Get(), res.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m);
    auto half = [](uint16_t h) {
        const uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1f, mm = h & 0x3ff;
        uint32_t x = e ? (s | ((e + 112) << 23) | (mm << 13)) : s;
        float f;
        memcpy(&f, &x, 4);
        return f;
    };
    int ownBad = 0, ownN = 0, fillN = 0, panelLeak = 0, edgeN = 0, worstX = -1, worstY = -1, bigN = 0;
    double worstTx = 0, worstTy = 0;
    double errSum = 0, errMax = 0;
    for (UINT y = 4; y + 4 < H; ++y)
        for (UINT x = 4; x + 4 < W; ++x) {
            const uint16_t* p = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch) + x * 4;
            const float got = half(p[0]);
            const double u = (x + 0.5) / W, v = (y + 0.5) / H;
            const double tx = camR.l + u * (camR.r - camR.l), ty = camR.u + v * (camR.d - camR.u);
            const bool own = tx > 0.6 && tx < 0.8 && ty > -0.6 && ty < -0.3;
            const double ndcx = u * 2 - 1;
            const float dcs = colRFill[(y * W + x) * 4];
            if (own || ndcx >= strip + 0.06) {
                ++ownN;
                if (std::fabs(got - half(toHalf(dcs))) > 1e-6) ++ownBad;
                continue;
            }
            if (ndcx >= strip) continue;  // the cross-fade
            // The left eye sees this wall point? (outside its frustum: not checked; near the panel: the hole test)
            const double X = camR.x + tx * kWallZ - kOrigin, txL = (X + kOrigin - camL.x) / kWallZ;
            if (txL < camL.l + 0.02 || txL > camL.r - 0.02) continue;
            const bool nearPanel = std::fabs(ty + 0.35) < 0.2 && txL > -0.40 && txL < 0.0;
            const double txP = (camR.x + tx * 0.6 - camL.x) / 0.6;  // where the panel is, in left tangents at 0.6 m
            const bool panelEdge = std::fabs(ty + 0.35) < 0.17 && txP > -0.40 && txP < 0.0;  // 1-2 depth texels round it
            if (got > 0.95f && !panelEdge) ++panelLeak;  // the white panel (the wall stays within 0.15..0.85)
            if (nearPanel || panelEdge) {
                ++edgeN;
                continue;
            }
            const double e = std::fabs(got - Wall(X, ty * kWallZ));
            if (e > errMax) worstX = int(x), worstY = int(y), worstTx = tx, worstTy = ty;
            errSum += e, errMax = std::max(errMax, e), ++fillN;
            if (e > 0.08) ++bigN;
        }
    ctx->Unmap(stage.Get(), 0);
    char b[256];
    snprintf(b, sizeof(b), "strip and own cockpit are DCS's pixels: %d of %d differ", ownBad, ownN);
    Check(ownBad == 0 && ownN > 1000, b);
    snprintf(b, sizeof(b), "filled wall matches the direct render: mean error %.4f, max %.4f over %d px (worst at %d,%d tan %.3f,%.3f; %d px above 0.08)", fillN ? errSum / fillN : 1.0,
             errMax, fillN, worstX, worstY, worstTx, worstTy, bigN);
    // Up to one depth texel around the right view's own cockpit keeps DCS's colour (the stencil is at depth resolution):
    // a fringe, counted, not an error of the fill.
    Check(fillN > 10000 && errSum / fillN < 0.01 && bigN < 60, b);
    snprintf(b, sizeof(b), "the cockpit panel does not leak into the right view's world away from its edge: %d px (%d near it)", panelLeak, edgeN);
    Check(panelLeak == 0, b);
    Check(run(false) == nullptr, "without the THREE_VIEWS flag the right periphery stays DCS's own");
    {  // the engine filled the right periphery itself (before its glass): the layer leaves it as it is
        DcsVrDepthView a = desc(0, camL, dL.Get(), false), b = desc(1, camR, dR.Get(), true);
        b.flags |= DCSVR_DEPTH_FILLED_IN_ENGINE;
        imp.Push(&a), imp.Push(&b);
        pass.BeginFrame(imp);
        Check(pass.Active() && pass.Fill(dev.Get(), ctx.Get(), sL.Get(), sR.Get(), W, H, samp.Get()) == nullptr,
              "a frame the engine filled is taken but not filled again (the layer fill is the fallback)");
    }
    {  // the engine remade its copies (a resize): the layer's depth views of the old ones let them go
        auto refs = [](IUnknown* p) {
            p->AddRef();
            return p->Release();
        };
        run(true);
        const ULONG held = refs(dL.Get());
        auto dL2 = texture(DXGI_FORMAT_R32G8X24_TYPELESS, DW, DH, depL.data(), DW * 8);
        auto dR2 = texture(DXGI_FORMAT_R32G8X24_TYPELESS, DW, DH, depR.data(), DW * 8);
        int filled = 0;
        for (int i = 0; i < 130; ++i) {
            DcsVrDepthView a = desc(0, camL, dL2.Get(), false), b = desc(1, camR, dR2.Get(), true);
            imp.Push(&a), imp.Push(&b);
            pass.BeginFrame(imp);
            filled += pass.Fill(dev.Get(), ctx.Get(), sL.Get(), sR.Get(), W, H, samp.Get()) ? 1 : 0;
        }
        const ULONG after = refs(dL.Get()), afterR = refs(dR.Get());
        char m2[200];
        snprintf(m2, sizeof(m2), "old depth copies are let go by the layer's view cache (left refs %lu while used, %lu / right %lu after %d fills on new copies)",
                 held, after, afterR, filled);
        Check(filled == 130 && held > 1 && after == 1 && afterR == 1, m2);
    }
    printf("%d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
