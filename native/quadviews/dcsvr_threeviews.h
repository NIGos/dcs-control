// DCS Control 0.7 three views, Quad Views' side (a first, rough version to judge in the headset). When the engine
// (DcsQvCull [Views] ThreeViews) renders the right periphery only as its outer strip (NDC x >= stripNdcX) plus the
// cockpit, it pushes both periphery views' depth + stencil (dcsvr_depth_export.h v1.2, hmdView 0 / 1, the right one
// flagged DCSVR_DEPTH_THREE_VIEWS). At xrEndFrame, before the right eye's periphery is used (stability, contrast,
// composition), this pass builds the right periphery the composition samples instead of DCS's image:
//   - DCS's own pixels where its stencil says cockpit (composition COCKPIT, (s & 0x38) == 0x28, copied before the
//     transparent passes) and in the strip (a short cross-fade at its inner edge);
//   - everywhere else the left periphery's colour, reprojected: the left depth (cockpit texels left out) is splatted
//     into the right view at depth resolution (nearest wins), then each right pixel maps back into the left image with
//     that depth (bilinear colour). A pixel no left texel reached (the right eye sees what the cockpit hides from the
//     left one) takes the colour of the farther of its nearest reached neighbours on its row (background stretch);
//     none within the search: the direction at infinity.
// The matrices come from the pushed gProj / gViewInv (row vectors), the relative transform in double precision. All
// work is two compute dispatches on the application's context; GPU time is measured with timestamp queries and logged.
// DCSVR_THREE_VIEWS_DIR (environment, optional): a folder for this pass's own log (three_views.log) and for dumps: a
// file dump.flag there makes the next frame write the right periphery as composed (filled or DCS's own), the left
// periphery and the pixel classes (tv_<n>_*.bin + .json), then the flag is deleted.
#ifndef DCSVR_THREEVIEWS_H
#define DCSVR_THREEVIEWS_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "dcsvr_depth_export.h"
#include "dcsvr_depth_import.h"

namespace dcsvr {

// 4x4 in double, row vectors (v' = v * M), logical M[r][c] at [r * 4 + c] as the descriptor holds it.
struct Mat4d {
    double m[16]{};
    static Mat4d From(const float f[16]) {
        Mat4d r;
        for (int i = 0; i < 16; ++i) r.m[i] = f[i];
        return r;
    }
    Mat4d operator*(const Mat4d& b) const {
        Mat4d r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                double s = 0;
                for (int k = 0; k < 4; ++k) s += m[i * 4 + k] * b.m[k * 4 + j];
                r.m[i * 4 + j] = s;
            }
        return r;
    }
    bool Inverse(Mat4d& out) const {
        double a[4][8];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
        for (int c = 0; c < 4; ++c) {
            int p = c;
            for (int r = c + 1; r < 4; ++r)
                if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
            if (std::fabs(a[p][c]) < 1e-300) return false;
            if (p != c)
                for (int j = 0; j < 8; ++j) std::swap(a[p][j], a[c][j]);
            const double d = a[c][c];
            for (int j = 0; j < 8; ++j) a[c][j] /= d;
            for (int r = 0; r < 4; ++r) {
                if (r == c) continue;
                const double f = a[r][c];
                if (f != 0)
                    for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
            }
        }
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) out.m[i * 4 + j] = a[i][j + 4];
        return true;
    }
};

// The NDC (post-divide, reverse-Z depth) of view a to the homogeneous NDC of view b: inv(Pa) * VIa * inv(VIb) * Pb,
// with VIa * inv(VIb) built in double from the two camera transforms (large world coordinates cancel there).
inline bool ThreeViewsTransform(const DcsVrDepthView& a, const DcsVrDepthView& b, float out[16]) {
    const Mat4d Pa = Mat4d::From(a.proj), Pb = Mat4d::From(b.proj), VIa = Mat4d::From(a.viewInv),
                VIb = Mat4d::From(b.viewInv);
    Mat4d iPa, iVIb;
    if (!Pa.Inverse(iPa) || !VIb.Inverse(iVIb)) return false;
    const Mat4d M = iPa * (VIa * iVIb) * Pb;
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(M.m[i])) return false;
        out[i] = (float)M.m[i];
    }
    return true;
}

constexpr const char kThreeViewsCs[] = R"(
cbuffer C : register(b0) {
  row_major float4x4 M01;  // left NDC -> right homogeneous NDC
  row_major float4x4 M10;  // right NDC -> left homogeneous NDC
  float4 vpL;              // left depth viewport in its texture (x, y, w, h)
  float4 vpR;              // right depth viewport
  float4 outSize;          // colour w, h, 1/w, 1/h
  float4 strip;            // x: strip NDC, y: blend start NDC, z: blend end NDC, w: search texels
};
Texture2D<float4> colL : register(t0);
Texture2D<float4> colR : register(t1);
Texture2D<float> depL : register(t2);
Texture2D<uint2> stL : register(t3);
Texture2D<float> depR : register(t4);
Texture2D<uint2> stR : register(t5);
RWTexture2D<uint> splat : register(u0);
RWTexture2D<float4> outC : register(u1);
RWTexture2D<uint> cls : register(u2);
SamplerState lin : register(s0);
bool Cockpit(uint s) { return (s & 0x38) == 0x28; }

[numthreads(8, 8, 1)] void Splat(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)vpL.z || id.y >= (uint)vpL.w) return;
  int2 t = int2(vpL.xy) + int2(id.xy);
  if (Cockpit(stL.Load(int3(t, 0)).g)) return;
  float d = max(depL.Load(int3(t, 0)), 1e-7);
  float2 uv = (float2(id.xy) + 0.5) / vpL.zw;
  float4 c = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1), M01);
  if (!(c.w > 0)) return;
  float3 p = c.xyz / c.w;
  float2 uv1 = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
  float2 f = uv1 * vpR.zw - 0.5;
  int y = (int)floor(f.y + 0.5);
  int x0 = (int)floor(f.x);
  if (y < 0 || y >= (int)vpR.w || !(p.z > 0)) return;
  uint v = asuint(p.z);  // reverse Z: larger = nearer, and a positive float's bits order like it
  [unroll] for (int k = 0; k < 2; ++k) {
    int x = x0 + k;
    if (x >= 0 && x < (int)vpR.z) InterlockedMax(splat[int2(x, y)], v);
  }
}

float2 ToLeft(float2 ndc, float d) {
  float4 c = mul(float4(ndc, d, 1), M10);
  float2 p = c.xy / max(c.w, 1e-12);
  return float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
}
// The left sample's bilinear footprint touches the left eye's cockpit: that colour is not this world point's (the
// depth is splatted at depth resolution, one texel covers about 1.7 output pixels at an occlusion edge).
bool LeftCockpit(float2 uvL) {
  float2 t = uvL * vpL.zw;
  float2 h = 0.5 * vpL.zw * outSize.zw + 0.5;
  int2 a = int2(clamp(floor(t - h), 0, vpL.zw - 1)) + int2(vpL.xy), b = int2(clamp(floor(t + h), 0, vpL.zw - 1)) + int2(vpL.xy);
  return Cockpit(stL.Load(int3(a.x, a.y, 0)).g) || Cockpit(stL.Load(int3(b.x, a.y, 0)).g) ||
         Cockpit(stL.Load(int3(a.x, b.y, 0)).g) || Cockpit(stL.Load(int3(b.x, b.y, 0)).g);
}

[numthreads(8, 8, 1)] void Fill(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)outSize.x || id.y >= (uint)outSize.y) return;
  float2 uv = (float2(id.xy) + 0.5) * outSize.zw;
  float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
  float4 own = colR.Load(int3(id.xy, 0));
  int2 rt = int2(clamp(uv * vpR.zw, 0, vpR.zw - 1));
  if (Cockpit(stR.Load(int3(int2(vpR.xy) + rt, 0)).g)) {
    outC[id.xy] = own;
    cls[id.xy] = 1;
    return;
  }
  float w = saturate((ndc.x - strip.y) / max(strip.z - strip.y, 1e-4));
  if (w >= 1) {
    outC[id.xy] = own;
    cls[id.xy] = 0;
    return;
  }
  uint k = 2;
  float2 uvL = 0;
  uint s = splat[rt];
  bool found = false;
  if (s != 0) {
    uvL = ToLeft(ndc, asfloat(s));
    found = !LeftCockpit(uvL);
  }
  if (!found) {
    // A hole (or a sample on the left cockpit): the farther of the nearest usable texels left and right on this row,
    // each with its own mapping (background stretch).
    int n = (int)strip.w;
    bool fl = false, fr = false;
    float dl = 0, dr = 0;
    float2 ul = 0, ur = 0;
    for (int i = 1; i <= n && !(fl && fr); ++i) {
      [unroll] for (int side = 0; side < 2; ++side) {
        if (side == 0 ? fl : fr) continue;
        int x = side == 0 ? rt.x - i : rt.x + i;
        if (x < 0 || x >= (int)vpR.z) continue;
        uint a = splat[int2(x, rt.y)];
        if (a == 0) continue;
        float2 u = ToLeft(float2(((x + 0.5) / vpR.z) * 2 - 1, ndc.y), asfloat(a));
        if (LeftCockpit(u)) continue;
        if (side == 0) { fl = true; dl = asfloat(a); ul = u; } else { fr = true; dr = asfloat(a); ur = u; }
      }
    }
    if (!fl && !fr) {
      uvL = ToLeft(ndc, 1e-7);
      k = 4;
    } else {
      uvL = (!fr || (fl && dl <= dr)) ? ul : ur;
      k = 3;
    }
  }
  if (any(uvL < 0) || any(uvL > 1)) k = 5;
  float4 rep = colL.SampleLevel(lin, saturate(uvL), 0);
  rep.a = 1;
  outC[id.xy] = lerp(rep, own, w);
  cls[id.xy] = k;
}
)";

class ThreeViews {
  public:
    using LogFn = void (*)(const std::string&);

    void SetLog(LogFn f) {
        m_log = f;
        char dir[MAX_PATH];
        const DWORD n = GetEnvironmentVariableA("DCSVR_THREE_VIEWS_DIR", dir, MAX_PATH);
        if (n > 0 && n < MAX_PATH) m_dir = dir;
    }
    void Reset() { *this = ThreeViews{}; }

    // Once per xrEndFrame (before the eyes): this frame's pushes, or none.
    void BeginFrame(DepthImport& import) {
        m_ok = import.TakeThreeViews(m_latch);
        if (m_ok) ++m_framesTaken;
        if (!m_dir.empty() && (++m_flagPoll % 45) == 0 && !m_dumpPending) {
            const std::string flag = m_dir + "\\dump.flag";
            if (GetFileAttributesA(flag.c_str()) != INVALID_FILE_ATTRIBUTES) {
                DeleteFileA(flag.c_str());
                m_dumpPending = true;
            }
        }
    }
    bool Active() const { return m_ok; }

    // The right eye's periphery (viewIndex 1), after the flat copies: left / right are the sources the composition
    // would sample, w x h their size (one size for both eyes). Returns the SRV to sample instead, or null (DCS's own).
    // dumpRight: called for the dump with whatever the composition will sample.
    ID3D11ShaderResourceView* Fill(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* left,
                                   ID3D11ShaderResourceView* right, UINT w, UINT h, ID3D11SamplerState* sampler) {
        if (!m_ok || !left || !right || !dev || !ctx) return nullptr;
        if (!Ensure(dev)) return nullptr;
        const DcsVrDepthView& L = m_latch[0].desc;
        const DcsVrDepthView& R = m_latch[1].desc;
        Constants k{};
        if (!ThreeViewsTransform(L, R, k.M01) || !ThreeViewsTransform(R, L, k.M10)) {
            ++m_badMatrix;
            return nullptr;
        }
        for (int i = 0; i < 4; ++i) k.vpL[i] = L.viewport[i], k.vpR[i] = R.viewport[i];
        const UINT dw = (UINT)std::lround(R.viewport[2]), dh = (UINT)std::lround(R.viewport[3]);
        const UINT lw = (UINT)std::lround(L.viewport[2]), lh = (UINT)std::lround(L.viewport[3]);
        if (dw < 16 || dh < 16 || lw < 16 || lh < 16 || w < 16 || h < 16) return nullptr;
        k.outSize[0] = (float)w, k.outSize[1] = (float)h, k.outSize[2] = 1.f / w, k.outSize[3] = 1.f / h;
        k.strip[0] = R.stripNdcX;
        k.strip[1] = R.stripNdcX + 0.01f;
        k.strip[2] = R.stripNdcX + 0.06f;
        k.strip[3] = 24.f;
        ID3D11ShaderResourceView *dL = nullptr, *sL = nullptr, *dR = nullptr, *sR = nullptr;
        if (!DepthViews(dev, m_latch[0], dL, sL) || !DepthViews(dev, m_latch[1], dR, sR)) {
            ++m_badDepth;
            return nullptr;
        }
        if (!EnsureTargets(dev, dw, dh, w, h)) return nullptr;
        {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return nullptr;
            memcpy(m.pData, &k, sizeof(k));
            ctx->Unmap(m_cb.Get(), 0);
        }
        Timer& t = m_timers[m_timerNext];
        const bool timed = !t.pending && t.dj;
        if (timed) {
            ctx->Begin(t.dj.Get());
            ctx->End(t.t0.Get());
        }
        const UINT zero[4] = {};
        ctx->ClearUnorderedAccessViewUint(m_splatUav.Get(), zero);
        ID3D11ShaderResourceView* nullSrv[6] = {};
        ctx->PSSetShaderResources(0, 3, nullSrv);
        ID3D11ShaderResourceView* srvs[6] = {left, right, dL, sL, dR, sR};
        ID3D11UnorderedAccessView* uavs[3] = {m_splatUav.Get(), m_outUav.Get(), m_clsUav.Get()};
        ctx->CSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
        ctx->CSSetShaderResources(0, 6, srvs);
        ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        ctx->CSSetSamplers(0, 1, &sampler);
        ctx->CSSetShader(m_splatCs.Get(), nullptr, 0);
        ctx->Dispatch((lw + 7) / 8, (lh + 7) / 8, 1);
        ctx->CSSetShader(m_fillCs.Get(), nullptr, 0);
        ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        ID3D11UnorderedAccessView* nullUav[3] = {};
        ctx->CSSetUnorderedAccessViews(0, 3, nullUav, nullptr);
        ctx->CSSetShaderResources(0, 6, nullSrv);
        if (timed) {
            ctx->End(t.t1.Get());
            ctx->End(t.dj.Get());
            t.pending = true;
            m_timerNext = (m_timerNext + 1) % kTimers;
        }
        PollTimers(ctx);
        ++m_filled;
        if (m_filled == 1)
            Say("DCSVR three views: first fill: periphery " + std::to_string(w) + "x" + std::to_string(h) + ", depth " +
                std::to_string(lw) + "x" + std::to_string(lh) + " / " + std::to_string(dw) + "x" + std::to_string(dh) +
                ", strip from NDC x " + std::to_string(R.stripNdcX) + ", engine frame " + std::to_string(R.engineFrame));
        return m_outSrv.Get();
    }

    // The dump (after Fill, or with DCS's own image when the frame was not filled). Waits for the GPU: dumps only.
    // fov: the right periphery's and the right focus view's tangents (left, right, up, down), for the comparison's
    // "outside the focus area".
    void MaybeDump(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* left,
                   ID3D11ShaderResourceView* rightUsed, bool filled, const float periphTan[4], const float focusTan[4]) {
        if (!m_dumpPending) return;
        m_dumpPending = false;
        const std::string base = m_dir + "\\tv_" + std::to_string(++m_dumpN) + "_" + (filled ? "on" : "off");
        {
            FILE* f = nullptr;
            if (fopen_s(&f, (base + "_fov.json").c_str(), "wb") == 0 && f) {
                fprintf(f, "{\"periphery\": [%.6f, %.6f, %.6f, %.6f], \"focus\": [%.6f, %.6f, %.6f, %.6f], \"filled\": %d, "
                           "\"stripNdcX\": %.4f}\n",
                        periphTan[0], periphTan[1], periphTan[2], periphTan[3], focusTan[0], focusTan[1], focusTan[2],
                        focusTan[3], filled ? 1 : 0, m_latch[1].desc.stripNdcX);
                fclose(f);
            }
        }
        int ok = 0;
        ok += DumpSrv(dev, ctx, rightUsed, base + "_right") ? 1 : 0;
        ok += DumpSrv(dev, ctx, left, base + "_left") ? 1 : 0;
        if (filled && m_clsSrv) ok += DumpSrv(dev, ctx, m_clsSrv.Get(), base + "_class") ? 1 : 0;
        Say("DCSVR three views: dump " + base + " (" + std::to_string(ok) + " files, " + (filled ? "filled" : "DCS's own") +
            ")");
    }

    // Statistics, every few hundred filled frames.
    void Report() {
        if (m_filled == 0 || m_filled == m_reported || (m_filled - m_reported) < 900) return;
        m_reported = m_filled;
        char b[512];
        snprintf(b, sizeof(b),
                 "DCSVR three views: %llu frames filled (%llu taken), GPU mean %.3f ms max %.3f ms over %llu timed; "
                 "matrix failures %llu, depth view failures %llu",
                 (unsigned long long)m_filled, (unsigned long long)m_framesTaken, m_gpuN ? m_gpuSum / m_gpuN : -1.0,
                 m_gpuMax, (unsigned long long)m_gpuN, (unsigned long long)m_badMatrix, (unsigned long long)m_badDepth);
        Say(b);
        m_gpuSum = 0, m_gpuMax = 0, m_gpuN = 0;
    }

  private:
    struct Constants {
        float M01[16], M10[16];
        float vpL[4], vpR[4], outSize[4], strip[4];
    };
    struct Timer {
        Microsoft::WRL::ComPtr<ID3D11Query> dj, t0, t1;
        bool pending = false;
    };
    static constexpr int kTimers = 6;

    void Say(const std::string& s) {
        if (m_log) m_log(s + "\n");
        if (!m_dir.empty()) {
            FILE* f = nullptr;
            if (fopen_s(&f, (m_dir + "\\three_views.log").c_str(), "ab") == 0 && f) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                fprintf(f, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, s.c_str());
                fclose(f);
            }
        }
    }

    bool Ensure(ID3D11Device* dev) {
        if (m_failed) return false;
        if (m_splatCs) return true;
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        using CompileFn = decltype(&D3DCompile);
        const auto compile = dc ? reinterpret_cast<CompileFn>(GetProcAddress(dc, "D3DCompile")) : nullptr;
        bool ok = compile != nullptr;
        for (int i = 0; i < 2 && ok; ++i) {
            Microsoft::WRL::ComPtr<ID3DBlob> code, err;
            ok = SUCCEEDED(compile(kThreeViewsCs, sizeof(kThreeViewsCs) - 1, "dcsvr_threeviews", nullptr, nullptr,
                                   i ? "Fill" : "Splat", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err));
            if (!ok) {
                Say(std::string("DCSVR three views: shader did not compile: ") +
                    (err ? (const char*)err->GetBufferPointer() : "no compiler"));
                break;
            }
            ok = SUCCEEDED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                    (i ? m_fillCs : m_splatCs).ReleaseAndGetAddressOf()));
        }
        if (ok) {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = sizeof(Constants);
            d.Usage = D3D11_USAGE_DYNAMIC;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ok = SUCCEEDED(dev->CreateBuffer(&d, nullptr, m_cb.ReleaseAndGetAddressOf()));
        }
        for (Timer& t : m_timers) {
            if (!ok) break;
            D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            dev->CreateQuery(&q, t.dj.ReleaseAndGetAddressOf());
            q.Query = D3D11_QUERY_TIMESTAMP;
            dev->CreateQuery(&q, t.t0.ReleaseAndGetAddressOf());
            dev->CreateQuery(&q, t.t1.ReleaseAndGetAddressOf());
            if (!t.t0 || !t.t1) t.dj.Reset();
        }
        if (!ok) {
            m_failed = true;
            m_splatCs.Reset();
            Say("DCSVR three views: resources failed; the right periphery stays DCS's own");
        }
        return ok;
    }

    bool EnsureTargets(ID3D11Device* dev, UINT dw, UINT dh, UINT w, UINT h) {
        if (m_splat && m_dw == dw && m_dh == dh && m_w == w && m_h == h) return true;
        m_splat.Reset(), m_splatUav.Reset(), m_out.Reset(), m_outUav.Reset(), m_outSrv.Reset(), m_cls.Reset(),
            m_clsUav.Reset(), m_clsSrv.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = dw;
        d.Height = dh;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32_UINT;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &m_splat)) &&
                  SUCCEEDED(dev->CreateUnorderedAccessView(m_splat.Get(), nullptr, &m_splatUav));
        d.Width = w;
        d.Height = h;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &m_out)) &&
             SUCCEEDED(dev->CreateUnorderedAccessView(m_out.Get(), nullptr, &m_outUav)) &&
             SUCCEEDED(dev->CreateShaderResourceView(m_out.Get(), nullptr, &m_outSrv));
        d.Format = DXGI_FORMAT_R8_UINT;
        ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &m_cls)) &&
             SUCCEEDED(dev->CreateUnorderedAccessView(m_cls.Get(), nullptr, &m_clsUav)) &&
             SUCCEEDED(dev->CreateShaderResourceView(m_cls.Get(), nullptr, &m_clsSrv));
        if (!ok) {
            m_splat.Reset();
            Say("DCSVR three views: no memory for the fill targets; this frame stays DCS's own");
            return false;
        }
        m_dw = dw, m_dh = dh, m_w = w, m_h = h;
        return true;
    }

    // Depth and stencil SRVs of a latched texture (cached by texture; the latches hold the textures).
    bool DepthViews(ID3D11Device* dev, const DepthViewLatch& l, ID3D11ShaderResourceView*& depth,
                    ID3D11ShaderResourceView*& stencil) {
        ID3D11Texture2D* t = l.texture.Get();
        auto it = m_views.find(t);
        if (it == m_views.end()) {
            if (m_views.size() > 16) m_views.clear();
            DXGI_FORMAT df, sf;
            DepthSrvFormats(l.desc.format, df, sf);
            if (df == DXGI_FORMAT_UNKNOWN || sf == DXGI_FORMAT_UNKNOWN) return false;
            Views v;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            sd.Format = df;
            if (FAILED(dev->CreateShaderResourceView(t, &sd, &v.depth))) return false;
            sd.Format = sf;
            if (FAILED(dev->CreateShaderResourceView(t, &sd, &v.stencil))) return false;
            v.tex = l.texture;
            it = m_views.emplace(t, v).first;
        }
        depth = it->second.depth.Get();
        stencil = it->second.stencil.Get();
        return true;
    }

    void PollTimers(ID3D11DeviceContext* ctx) {
        for (Timer& t : m_timers) {
            if (!t.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            if (ctx->GetData(t.dj.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            UINT64 a = 0, b = 0;
            if (ctx->GetData(t.t0.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                ctx->GetData(t.t1.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                continue;
            t.pending = false;
            if (dj.Disjoint || !dj.Frequency || b < a) continue;
            const double ms = (double)(b - a) * 1000.0 / (double)dj.Frequency;
            m_gpuSum += ms, m_gpuMax = (std::max)(m_gpuMax, ms), ++m_gpuN;
        }
    }

    bool DumpSrv(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv, const std::string& base) {
        if (!srv) return false;
        Microsoft::WRL::ComPtr<ID3D11Resource> res;
        srv->GetResource(&res);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        if (FAILED(res.As(&tex))) return false;
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        srv->GetDesc(&sd);
        const UINT slice = sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY ? sd.Texture2DArray.FirstArraySlice : 0;
        D3D11_TEXTURE2D_DESC s = d;
        s.ArraySize = 1;
        s.MipLevels = 1;
        s.SampleDesc.Count = 1;
        s.Usage = D3D11_USAGE_STAGING;
        s.BindFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        s.MiscFlags = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> st;
        if (d.SampleDesc.Count != 1 || FAILED(dev->CreateTexture2D(&s, nullptr, &st))) return false;
        ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, tex.Get(), D3D11CalcSubresource(0, slice, d.MipLevels), nullptr);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        FILE* f = nullptr;
        bool ok = fopen_s(&f, (base + ".bin").c_str(), "wb") == 0 && f;
        if (ok) {
            fwrite(m.pData, 1, (size_t)m.RowPitch * d.Height, f);
            fclose(f);
        }
        ctx->Unmap(st.Get(), 0);
        if (ok && fopen_s(&f, (base + ".json").c_str(), "wb") == 0 && f) {
            fprintf(f, "{\"width\": %u, \"height\": %u, \"rowPitch\": %u, \"format\": %u, \"viewFormat\": %u}\n", d.Width,
                    d.Height, m.RowPitch, (unsigned)d.Format, (unsigned)sd.Format);
            fclose(f);
        }
        return ok;
    }

    struct Views {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depth, stencil;
    };

    LogFn m_log = nullptr;
    std::string m_dir;
    bool m_ok = false, m_failed = false, m_dumpPending = false;
    DepthViewLatch m_latch[2];
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_splatCs, m_fillCs;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_splat, m_out, m_cls;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_splatUav, m_outUav, m_clsUav;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_outSrv, m_clsSrv;
    std::map<ID3D11Texture2D*, Views> m_views;
    UINT m_dw = 0, m_dh = 0, m_w = 0, m_h = 0;
    Timer m_timers[kTimers];
    int m_timerNext = 0;
    double m_gpuSum = 0, m_gpuMax = 0;
    uint64_t m_gpuN = 0, m_filled = 0, m_reported = 0, m_framesTaken = 0, m_badMatrix = 0, m_badDepth = 0, m_flagPoll = 0;
    int m_dumpN = 0;
};

} // namespace dcsvr

#endif // DCSVR_THREEVIEWS_H
