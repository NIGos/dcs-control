// R94: [Perception] SymbologyTrace (dev only, every key default 0 / off; keys in sym_trace_cfg.h). The live HUD
// trace R89 (docs/research/R89_hud_layer_fg.md §5.3 P0) asked for: which pass draws the HUD, helmet and MFD
// symbology, which indicator type (record byte +0xc9) each replayed record has, the blend state of those draws, and
// copies of the views' targets around the overlay passes, so that the HDR alpha, the symbology bounding boxes, the
// mask's 128 coverage and the "tone map twice" delta can be measured offline. Measure only: no image changes.
//
// A session: one learn frame (the tone-map draw's source and target per view are learnt), then N traced frames.
// Per traced frame (render thread; all text goes to <dev folder>\sym_trace\<date_time>_s<session>\frame_<F>.txt,
// written by the config thread):
//   P B|E <hmd> <pass>           every pass boundary (fixation_depth.h OnBoundaryInner), HMD ordinal at the time
//   R <t> <pass> ...             every cockpit record replay (ovl_cpu.h ReplayHook): the overlay pass (H OverlayHDR,
//                                L OverlayLDR, G GUI, - none) and HMD view it runs in, DCS's view index, and the
//                                run-length list of the view's records as c9.c8 (S = a state record)
//   D <t> ...                    the records the replay drew (the render point, ovl_cpu.h RenderHook), c9.c8 runs,
//                                and the render target bound at its first draw
//   B <t> c9=<n> ...             the blend state (RT0) and render target of the first drawn record of each type
//   T <v> ...                    the view's tone-map draw: its PS SRV0 (the HDR source) and RTV (the LDR target)
//   C <v> <tag> ...              a target copy (raw file, listed in captures.txt)
// Copies (raw, tight rows; captures.txt names each with its frame, view, tag, size, DXGI format and bytes per texel):
//   hdr_pre     the view's HDR target at its OverlayHDRPass begin (the source learnt at the last tone-map draw)
//   hdr_post    the tone-map draw's PS SRV0 (the HDR as tone-mapped, after OverlayHDRPass)
//   ldr_tm      the tone-map draw's target at the OverlayLDRPass begin (or CopyToHMDPass begin without one)
//   ldr_post    the same target at the OverlayLDRPass end
//   segK_A_B    (SymbologyTraceSegments) the target bound when an overlay replay's drawn records change type from
//               c9 A to c9 B: everything of type A drawn, nothing of B yet
// The copies are mapped >= 2 frames later without waiting (waiting from 30 frames on); the bytes are handed to the
// config thread, which writes them. Dev only: a traced frame may stall the render thread for the copies' memcpy.
#pragma once

namespace symtr {

constexpr int kMaxFrames = 30;
constexpr size_t kMaxQueuedBytes = size_t(1536) << 20;  // copies beyond this are skipped (counted)

// ---- render-thread state ----
std::atomic<bool> g_on{false};  // this frame is traced (text, copies); read by the cockpit replay hooks
bool g_learn = false;           // this frame learns the tone-map draws' source / target (traced frames too)
int g_session = 0;
int g_learnLeft = 0, g_framesLeft = 0;
int g_views = 15, g_segs = 0;   // latched at the session start
bool g_tone = true;
uint64_t g_frameNo = 0;         // g_s.frame of the traced frame
std::wstring g_dir;             // this session's folder
std::string g_text;             // this frame's lines
int g_replays[4] = {};          // this frame's replays: HDR, LDR, GUI, other
int g_capsFrame = 0, g_skipped = 0;
struct Tgt {
  ID3D11Resource* res = nullptr;  // AddRef'd while the session runs
  UINT sub = 0;
  uint64_t frame = ~0ull;         // g_s.frame of the tone draw that saw it
};
Tgt g_hdr[4], g_ldr[4];
const void* g_preRes[4] = {};   // the resource copied as hdr_pre this frame (identity only)
bool g_preDone[4] = {}, g_tmDone[4] = {}, g_postDone[4] = {};
int g_segN[4] = {};
uint32_t g_blendLogged[4] = {};  // bit c9 (0-7): the type's first drawn record logged in this frame
// A replay in progress (render thread): its drawn records' runs.
struct Rep {
  bool open = false;
  int t = -1;
  std::string drawn;
  int lastKey = -1, run = 0;
  bool rtLogged = false;
  int lastC9 = -1;
};
Rep g_rep;

// A pending copy.
struct Cap {
  ID3D11Texture2D* st = nullptr;
  UINT w = 0, h = 0, bpp = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  uint64_t frame = 0, at = 0;  // the traced frame; g_s.frame at the copy
  int view = -1;
  std::string tag;
  std::wstring dir;
};
std::vector<Cap> g_caps;
size_t g_queuedBytes = 0;  // copies pending or written-out not yet (render thread's estimate)

// ---- config-thread output ----
struct Out {
  std::wstring dir, name;
  std::string text;           // a text file (the frame lines) or a captures.txt line
  std::vector<uint8_t> bytes;
  bool capture = false;
};
std::mutex g_outMutex;
std::vector<Out> g_out;                          // under g_outMutex
std::atomic<size_t> g_outBytes{0};               // bytes queued for the writer
std::atomic<uint64_t> g_filesWritten{0}, g_bytesWritten{0};

inline bool Busy() { return g_on.load(std::memory_order_relaxed) || g_learn || !g_caps.empty(); }

inline UINT BytesPerTexel(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_TYPELESS: return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return 16;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS: return 4;
    default: return 0;
  }
}

// Pure: a run-length list item appended ("1.0x12 "); key -1 flushes. The key is c9 * 256 + c8, or -2 for a state
// record ("S").
inline void RleAdd(std::string& s, int& lastKey, int& run, int key) {
  if (key == lastKey && key != -1) {
    ++run;
    return;
  }
  if (lastKey != -1 && run > 0) {
    char b[32];
    if (lastKey == -2)
      snprintf(b, sizeof(b), "Sx%d ", run);
    else
      snprintf(b, sizeof(b), "%d.%dx%d ", lastKey >> 8, lastKey & 255, run);
    s += b;
  }
  lastKey = key;
  run = key == -1 ? 0 : 1;
}

inline void Line(const char* fmt, ...) {
  char b[700];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  g_text.append(b, static_cast<size_t>(std::min(n, static_cast<int>(sizeof(b)) - 1)));
  g_text += '\n';
}

// The subresource of a view on a 2D texture (array slice, mip); false for other dimensions.
inline bool SrvSub(ID3D11ShaderResourceView* v, ID3D11Resource** res, UINT* sub) {
  D3D11_SHADER_RESOURCE_VIEW_DESC d{};
  v->GetDesc(&d);
  v->GetResource(res);
  if (!*res) return false;
  D3D11_RESOURCE_DIMENSION dim{};
  (*res)->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
  D3D11_TEXTURE2D_DESC td{};
  static_cast<ID3D11Texture2D*>(*res)->GetDesc(&td);
  if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
    *sub = d.Texture2D.MostDetailedMip;
  else if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
    *sub = D3D11CalcSubresource(d.Texture2DArray.MostDetailedMip, d.Texture2DArray.FirstArraySlice, td.MipLevels);
  else
    return false;
  return true;
}
inline bool RtvSub(ID3D11RenderTargetView* v, ID3D11Resource** res, UINT* sub) {
  D3D11_RENDER_TARGET_VIEW_DESC d{};
  v->GetDesc(&d);
  v->GetResource(res);
  if (!*res) return false;
  D3D11_RESOURCE_DIMENSION dim{};
  (*res)->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
  D3D11_TEXTURE2D_DESC td{};
  static_cast<ID3D11Texture2D*>(*res)->GetDesc(&td);
  if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D)
    *sub = d.Texture2D.MipSlice;
  else if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
    *sub = D3D11CalcSubresource(d.Texture2DArray.MipSlice, d.Texture2DArray.FirstArraySlice, td.MipLevels);
  else
    return false;
  return true;
}
// "<ptr> sub <n> <fmt> <w>x<h> (array <n>, samples <n>)" of a texture subresource.
inline std::string Describe(ID3D11Resource* r, UINT sub) {
  char b[160];
  if (!r) return "none";
  D3D11_RESOURCE_DIMENSION dim{};
  r->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
    snprintf(b, sizeof(b), "%p (dimension %d)", static_cast<void*>(r), static_cast<int>(dim));
    return b;
  }
  D3D11_TEXTURE2D_DESC td{};
  static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);
  const UINT mip = td.MipLevels ? sub % td.MipLevels : 0;
  snprintf(b, sizeof(b), "%p sub %u fmt %d %ux%u (array %u, mips %u, samples %u)", static_cast<void*>(r), sub,
           static_cast<int>(td.Format), std::max(1u, td.Width >> mip), std::max(1u, td.Height >> mip), td.ArraySize,
           td.MipLevels, td.SampleDesc.Count);
  return b;
}

inline void SetTgt(Tgt& t, ID3D11Resource* r, UINT sub) {
  if (t.res != r) {
    if (r) r->AddRef();
    if (t.res) t.res->Release();
    t.res = r;
  }
  t.sub = sub;
  t.frame = fixdepth::g_s.frame;
}
inline void ReleaseTargets() {
  for (int v = 0; v < 4; ++v)
    for (Tgt* t : {&g_hdr[v], &g_ldr[v]}) {
      if (t->res) t->res->Release();
      *t = Tgt{};
    }
}

// A copy of one texture subresource into a new staging texture (render thread). False: not copied (counted).
inline bool Capture(int v, const char* tag, ID3D11Resource* src, UINT sub) {
  State& s = fixdepth::g_s;
  if (!src || !s.dev || !s.ctx || v < 0 || v > 3 || !(g_views & (1 << v))) return false;
  D3D11_RESOURCE_DIMENSION dim{};
  src->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
    ++g_skipped;
    return false;
  }
  D3D11_TEXTURE2D_DESC td{};
  static_cast<ID3D11Texture2D*>(src)->GetDesc(&td);
  const UINT bpp = BytesPerTexel(td.Format);
  const UINT mip = td.MipLevels ? sub % td.MipLevels : 0;
  const UINT w = std::max(1u, td.Width >> mip), h = std::max(1u, td.Height >> mip);
  const size_t bytes = static_cast<size_t>(w) * h * bpp;
  if (!bpp || td.SampleDesc.Count != 1 || sub >= td.MipLevels * td.ArraySize ||
      g_queuedBytes + g_outBytes.load(std::memory_order_relaxed) + bytes > kMaxQueuedBytes) {
    ++g_skipped;
    Line("C %d %s SKIPPED (fmt %d samples %u bpp %u, queued %.0f MB)", v, tag, static_cast<int>(td.Format),
         td.SampleDesc.Count, bpp, (g_queuedBytes + g_outBytes.load()) / 1048576.0);
    return false;
  }
  D3D11_TEXTURE2D_DESC sd{};
  sd.Width = w;
  sd.Height = h;
  sd.MipLevels = sd.ArraySize = 1;
  sd.Format = td.Format;
  sd.SampleDesc.Count = 1;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* st = nullptr;
  if (FAILED(s.dev->CreateTexture2D(&sd, nullptr, &st)) || !st) {
    ++g_skipped;
    return false;
  }
  s.ctx->CopySubresourceRegion(st, 0, 0, 0, 0, src, sub, nullptr);
  Cap c;
  c.st = st;
  c.w = w, c.h = h, c.bpp = bpp, c.fmt = td.Format;
  c.frame = g_frameNo;
  c.at = s.frame;
  c.view = v;
  c.tag = tag;
  c.dir = g_dir;
  g_caps.push_back(std::move(c));
  g_queuedBytes += bytes;
  ++g_capsFrame;
  Line("C %d %s %ux%u fmt %d bpp %u from %p", v, tag, w, h, static_cast<int>(td.Format), bpp,
       static_cast<void*>(src));
  return true;
}

// The pending copies: mapped >= 2 frames later without waiting, waiting from 30 frames on (render thread).
inline void PollCaps(bool force) {
  State& s = fixdepth::g_s;
  if (g_caps.empty() || !s.ctx) return;
  for (size_t i = 0; i < g_caps.size();) {
    Cap& c = g_caps[i];
    const bool wait = force || s.frame >= c.at + 30;
    if (!wait && s.frame < c.at + 2) {
      ++i;
      continue;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = s.ctx->Map(c.st, 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
      ++i;
      continue;
    }
    const size_t row = static_cast<size_t>(c.w) * c.bpp;
    Out o;
    o.dir = c.dir;
    o.capture = true;
    if (SUCCEEDED(hr)) {
      o.bytes.resize(row * c.h);
      const uint8_t* p = static_cast<const uint8_t*>(m.pData);
      for (UINT y = 0; y < c.h; ++y) memcpy(o.bytes.data() + row * y, p + static_cast<size_t>(m.RowPitch) * y, row);
      s.ctx->Unmap(c.st, 0);
    }
    wchar_t name[160];
    swprintf_s(name, L"f%llu_v%d_%hs.raw", static_cast<unsigned long long>(c.frame), c.view, c.tag.c_str());
    o.name = name;
    char line[300];
    snprintf(line, sizeof(line), "f%llu_v%d_%s.raw frame %llu view %d tag %s w %u h %u fmt %d bpp %u bytes %zu%s",
             static_cast<unsigned long long>(c.frame), c.view, c.tag.c_str(), static_cast<unsigned long long>(c.frame),
             c.view, c.tag.c_str(), c.w, c.h, static_cast<int>(c.fmt), c.bpp, o.bytes.size(),
             SUCCEEDED(hr) ? "" : " MAP-FAILED");
    o.text = line;
    g_queuedBytes -= std::min(g_queuedBytes, row * c.h);
    c.st->Release();
    g_outBytes += o.bytes.size();
    {
      std::lock_guard<std::mutex> lock(g_outMutex);
      g_out.push_back(std::move(o));
    }
    g_caps.erase(g_caps.begin() + static_cast<ptrdiff_t>(i));
  }
}

// ---- hooks (render thread) ----

// Every pass boundary (fixation_depth.h OnBoundaryInner, before its own work for the boundary).
inline void OnBoundary(void* pass, bool begin, int k) {
  State& s = fixdepth::g_s;
  if (!g_on.load(std::memory_order_relaxed)) return;
  char n[48];
  fixdepth::PassName(pass, n, sizeof(n));
  Line("P %c %d %s", begin ? 'B' : 'E', s.hmdOrd, n);
  const int v = s.hmdOrd;
  if (v < 0 || v > 3) return;
  if (begin && strcmp(n, "OverlayHDRPass") == 0 && !g_preDone[v] && g_hdr[v].res &&
      s.frame - g_hdr[v].frame <= 2) {
    g_preDone[v] = true;
    g_preRes[v] = g_hdr[v].res;
    Capture(v, "hdr_pre", g_hdr[v].res, g_hdr[v].sub);
  }
  const bool ldrNow = g_ldr[v].res && g_ldr[v].frame == s.frame;  // this frame's tone-map target
  if (begin && (strcmp(n, "OverlayLDRPass") == 0 || k == plod::kHmd) && !g_tmDone[v] && ldrNow) {
    g_tmDone[v] = true;
    Capture(v, "ldr_tm", g_ldr[v].res, g_ldr[v].sub);
  }
  if (!begin && strcmp(n, "OverlayLDRPass") == 0 && !g_postDone[v] && ldrNow) {
    g_postDone[v] = true;
    Capture(v, "ldr_post", g_ldr[v].res, g_ldr[v].sub);
  }
}

// The tone-map draw of HMD view v (fixation_depth.h DrawPre, before any pixel shader swap): learn its PS SRV0 (the
// HDR source) and RTV 0 (the LDR target); in a traced frame copy the source (hdr_post).
inline void ToneDraw(int v, ID3D11DeviceContext* c, const D3D11_VIEWPORT& vp) {
  if (!c || v < 0 || v > 3) return;
  ID3D11ShaderResourceView* srv = nullptr;
  c->PSGetShaderResources(0, 1, &srv);
  ID3D11RenderTargetView* rtv = nullptr;
  c->OMGetRenderTargets(1, &rtv, nullptr);
  ID3D11Resource *hr = nullptr, *lr = nullptr;
  UINT hs = 0, ls = 0;
  const bool hok = srv && SrvSub(srv, &hr, &hs), lok = rtv && RtvSub(rtv, &lr, &ls);
  if (hok) SetTgt(g_hdr[v], hr, hs);
  if (lok) SetTgt(g_ldr[v], lr, ls);
  if (g_on.load(std::memory_order_relaxed)) {
    Line("T %d src %s | target %s | vp %.0f,%.0f %.0fx%.0f | pre_same %d", v, hok ? Describe(hr, hs).c_str() : "none",
         lok ? Describe(lr, ls).c_str() : "none", vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height,
         g_preRes[v] ? (g_preRes[v] == hr ? 1 : 0) : -1);
    if (hok) Capture(v, "hdr_post", hr, hs);
  }
  if (hr) hr->Release();
  if (lr) lr->Release();
  if (srv) srv->Release();
  if (rtv) rtv->Release();
}

// A cockpit record replay (ovl_cpu.h ReplayHook), render thread. t: the overlay pass's HMD view (-1 none); pk: 'H',
// 'L', 'G' or '-'; recs: the run-length list of the view's records.
inline void ReplayBegin(int t, char pk, int dcsView, uint32_t n, const std::string& recs) {
  if (g_rep.open) {  // (nested or an end not seen)
    Line("D %d drawn %s (not closed)", g_rep.t, g_rep.drawn.c_str());
  }
  g_rep = Rep{};
  g_rep.open = true;
  g_rep.t = t;
  ++g_replays[pk == 'H' ? 0 : pk == 'L' ? 1 : pk == 'G' ? 2 : 3];
  Line("R %d %c hmd %d dcsview %d recs %u: %s", t, pk, fixdepth::g_s.hmdOrd, dcsView, n, recs.c_str());
}
inline void ReplayEnd() {
  if (!g_rep.open) return;
  RleAdd(g_rep.drawn, g_rep.lastKey, g_rep.run, -1);
  Line("D %d drawn %s", g_rep.t, g_rep.drawn.empty() ? "-" : g_rep.drawn.c_str());
  g_rep = Rep{};
}
// The render target bound now (RTV 0) as text.
inline std::string BoundTarget(ID3D11DeviceContext* c, ID3D11Resource** resOut = nullptr, UINT* subOut = nullptr) {
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  c->OMGetRenderTargets(1, &rtv, &dsv);
  std::string d = "none";
  if (rtv) {
    ID3D11Resource* r = nullptr;
    UINT sub = 0;
    if (RtvSub(rtv, &r, &sub)) {
      d = Describe(r, sub);
      if (resOut) {
        *resOut = r;
        r->AddRef();
      }
      if (subOut) *subOut = sub;
    }
    if (r) r->Release();
    rtv->Release();
  }
  if (dsv) {
    d += " +dsv";
    dsv->Release();
  }
  return d;
}
// A record the replay draws (ovl_cpu.h RenderHook, before DCS's render call): its run; at a type change the bound
// target is copied (SymbologyTraceSegments).
inline void RenderPre(int c9, int c8) {
  if (!g_rep.open) return;
  RleAdd(g_rep.drawn, g_rep.lastKey, g_rep.run, c9 * 256 + c8);
  State& s = fixdepth::g_s;
  const int t = g_rep.t;
  if (g_rep.lastC9 >= 0 && c9 != g_rep.lastC9 && t >= 0 && t <= 3 && g_segN[t] < g_segs && s.ctx) {
    ID3D11Resource* r = nullptr;
    UINT sub = 0;
    BoundTarget(s.ctx, &r, &sub);
    char tag[32];
    snprintf(tag, sizeof(tag), "seg%d_%d_%d", g_segN[t], g_rep.lastC9, c9);
    ++g_segN[t];
    if (r) {
      Capture(t, tag, r, sub);
      r->Release();
    }
  }
  g_rep.lastC9 = c9;
}
// After DCS's render call of that record: the first drawn record of each type logs the blend state it drew with.
inline void RenderPost(int c9) {
  if (!g_rep.open) return;
  State& s = fixdepth::g_s;
  const int t = g_rep.t;
  if (!s.ctx) return;
  if (!g_rep.rtLogged) {
    g_rep.rtLogged = true;
    Line("D %d target %s", t, BoundTarget(s.ctx).c_str());
  }
  const int ti = t >= 0 && t <= 3 ? t : 0;
  if (c9 < 0 || c9 > 7 || (g_blendLogged[ti] & (1u << c9))) return;
  g_blendLogged[ti] |= 1u << c9;
  ID3D11BlendState* bs = nullptr;
  float f[4] = {};
  UINT mask = 0;
  s.ctx->OMGetBlendState(&bs, f, &mask);
  if (!bs) {
    Line("B %d c9 %d blend default (null state) target %s", t, c9, BoundTarget(s.ctx).c_str());
    return;
  }
  D3D11_BLEND_DESC d{};
  bs->GetDesc(&d);
  bs->Release();
  const D3D11_RENDER_TARGET_BLEND_DESC& r = d.RenderTarget[0];
  Line("B %d c9 %d blend en %d src %d dst %d op %d srcA %d dstA %d opA %d write 0x%x a2c %d indep %d factor %.3g %.3g "
       "%.3g %.3g mask 0x%x target %s",
       t, c9, r.BlendEnable, r.SrcBlend, r.DestBlend, r.BlendOp, r.SrcBlendAlpha, r.DestBlendAlpha, r.BlendOpAlpha,
       r.RenderTargetWriteMask, d.AlphaToCoverageEnable, d.IndependentBlendEnable, f[0], f[1], f[2], f[3], mask,
       BoundTarget(s.ctx).c_str());
}

// The frame end (fixation_depth.h EndFrame, after the frame counter advanced): the traced frame's text out, the
// pending copies polled, the next frame's role decided.
inline void EndFrame() {
  State& s = fixdepth::g_s;
  if (g_on.load(std::memory_order_relaxed)) {
    if (g_rep.open) ReplayEnd();
    Out o;
    o.dir = g_dir;
    wchar_t name[64];
    swprintf_s(name, L"frame_%llu.txt", static_cast<unsigned long long>(g_frameNo));
    o.name = name;
    o.text.swap(g_text);
    {
      std::lock_guard<std::mutex> lock(g_outMutex);
      g_out.push_back(std::move(o));
    }
    Log("symtrace: frame %llu traced: replays HDR %d, LDR %d, GUI %d, other %d; %d copies queued (%.1f MB pending), "
        "%d skipped",
        static_cast<unsigned long long>(g_frameNo), g_replays[0], g_replays[1], g_replays[2], g_replays[3],
        g_capsFrame, g_queuedBytes / 1048576.0, g_skipped);
  }
  g_text.clear();
  PollCaps(false);
  // The next frame's role.
  if (g_learnLeft == 0 && g_framesLeft == 0 && symtrcfg::g_req.load(std::memory_order_relaxed) > 0) {
    const int r = symtrcfg::g_req.exchange(0);
    if (r > 0) {
      ++g_session;
      g_views = symtrcfg::g_views.load();
      g_segs = symtrcfg::g_segs.load();
      g_tone = symtrcfg::g_tone.load() != 0;
      g_learnLeft = 1;
      g_framesLeft = std::min(r, kMaxFrames);
      g_skipped = 0;
      std::wstring base;
      {
        std::lock_guard<std::mutex> lock(symtrcfg::g_baseMutex);
        base = symtrcfg::g_base;
      }
      SYSTEMTIME st;
      GetLocalTime(&st);
      wchar_t sub[80];
      swprintf_s(sub, L"sym_trace\\%04u%02u%02u_%02u%02u%02u_s%d\\", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond, g_session);
      g_dir = base + sub;
      Log("symtrace: session %d: 1 learn frame + %d traced frames, views 0x%x, segments %d, tone probe %d -> %ls",
          g_session, g_framesLeft, g_views, g_segs, g_tone ? 1 : 0, g_dir.c_str());
    }
  }
  const bool wasActive = g_learn;
  g_on = false;
  g_learn = false;
  if (g_learnLeft > 0) {
    --g_learnLeft;
    g_learn = true;
  } else if (g_framesLeft > 0) {
    --g_framesLeft;
    g_learn = true;
    g_on = true;
  }
  if (g_on.load(std::memory_order_relaxed)) {
    g_frameNo = s.frame;
    for (int v = 0; v < 4; ++v) {
      g_preRes[v] = nullptr;
      g_preDone[v] = g_tmDone[v] = g_postDone[v] = false;
      g_segN[v] = 0;
      g_blendLogged[v] = 0;
    }
    for (int& r : g_replays) r = 0;
    g_capsFrame = 0;
    g_rep = Rep{};
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    Line("F %llu qpc %lld session %d", static_cast<unsigned long long>(s.frame), static_cast<long long>(q.QuadPart),
         g_session);
  }
  if (wasActive && !g_learn) {  // the session's last frame ended: its targets released
    ReleaseTargets();
    Log("symtrace: session %d frames done; %zu copies still pending", g_session, g_caps.size());
  }
}

// The uninstall (config thread, after the boundary callback drained): everything released, nothing mapped (the
// immediate context belongs to the render thread; a COM Release is thread-safe). Copies not written are lost.
inline void Reset() {
  for (Cap& c : g_caps)
    if (c.st) c.st->Release();
  g_caps.clear();
  g_queuedBytes = 0;
  ReleaseTargets();
  g_on = false;
  g_learn = false;
  g_learnLeft = g_framesLeft = 0;
  g_rep = Rep{};
  g_text.clear();
}

// Config thread (fixation_depth.h Tick): the queued files written.
inline void WriteFiles() {
  std::vector<Out> out;
  {
    std::lock_guard<std::mutex> lock(g_outMutex);
    out.swap(g_out);
  }
  for (Out& o : out) {
    // The folder: <base>sym_trace\<session>\ (both levels made here).
    const size_t at = o.dir.rfind(L"sym_trace\\");
    if (at != std::wstring::npos) CreateDirectoryW(o.dir.substr(0, at + 10).c_str(), nullptr);
    CreateDirectoryW(o.dir.c_str(), nullptr);
    FILE* f = nullptr;
    if (o.capture) {
      if (!o.bytes.empty() && _wfopen_s(&f, (o.dir + o.name).c_str(), L"wb") == 0 && f) {
        fwrite(o.bytes.data(), 1, o.bytes.size(), f);
        fclose(f);
        g_filesWritten++;
        g_bytesWritten += o.bytes.size();
      }
      f = nullptr;
      if (_wfopen_s(&f, (o.dir + L"captures.txt").c_str(), L"ab") == 0 && f) {
        fprintf(f, "%s\n", o.text.c_str());
        fclose(f);
      }
      g_outBytes -= std::min(g_outBytes.load(), o.bytes.size());
    } else if (_wfopen_s(&f, (o.dir + o.name).c_str(), L"wb") == 0 && f) {
      fwrite(o.text.data(), 1, o.text.size(), f);
      fclose(f);
      g_filesWritten++;
    }
  }
}

}  // namespace symtr
