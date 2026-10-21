// R94: [Perception] SymbologyTrace (src/sym_trace.h, sym_trace_cfg.h; dev, measure only). The pure parts, the keys'
// defaults, and a session on WARP driven through the trace's own hooks (boundaries, the tone-map draw, the frame end,
// the writer): the right bytes in the right files, the session's roles per frame, nothing held afterwards. The
// integration with the real boundary callback and draw arm is checked in far_depth_test.h (the HDR sequence), the
// replay / render-point lines in ovl_cpu_test.h (the render-point scenario).
#pragma once

namespace symtr = fixdepth::symtr;  // (also used by far_depth_test.h and ovl_cpu_test.h)

namespace r94 {

using fdtest::FakePass;
using fdtest::MakePass;

inline std::vector<uint8_t> ReadAll(const std::wstring& p) {
  std::vector<uint8_t> b;
  FILE* f = nullptr;
  if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) return b;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n > 0) {
    b.resize(static_cast<size_t>(n));
    if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
  }
  fclose(f);
  return b;
}
inline std::string ReadText(const std::wstring& p) {
  const std::vector<uint8_t> b = ReadAll(p);
  return std::string(b.begin(), b.end());
}
inline int CountLines(const std::string& s, const char* prefix) {
  int n = 0;
  size_t at = 0;
  const size_t pl = strlen(prefix);
  while (at < s.size()) {
    const size_t e = s.find('\n', at);
    const size_t len = (e == std::string::npos ? s.size() : e) - at;
    if (len >= pl && s.compare(at, pl, prefix) == 0) ++n;
    if (e == std::string::npos) break;
    at = e + 1;
  }
  return n;
}
// Removes the files of dir and its subfolders (one level), then the folders.
inline void RemoveTree(const std::wstring& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    const std::wstring n = fd.cFileName;
    if (n == L"." || n == L"..") continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      RemoveTree(dir + n + L"\\");
    else
      DeleteFileW((dir + n).c_str());
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  RemoveDirectoryW(dir.c_str());
}
// The session folders under <base>sym_trace\ (sorted).
inline std::vector<std::wstring> Sessions(const std::wstring& base) {
  std::vector<std::wstring> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((base + L"sym_trace\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    const std::wstring n = fd.cFileName;
    if (n != L"." && n != L".." && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      out.push_back(base + L"sym_trace\\" + n + L"\\");
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end());
  return out;
}

inline void TestPure() {
  {
    std::string s;
    int last = -1, run = 0;
    for (int k : {256, 256, 256, -2, 2 * 256 + 3, 2 * 256 + 3, 0, -1}) symtr::RleAdd(s, last, run, k);
    Check(s == "1.0x3 Sx1 2.3x2 0.0x1 " && last == -1 && run == 0,
          "R94 SymbologyTrace: run-length list of c9.c8 keys (S = a state record), flushed by -1");
  }
  Check(symtr::BytesPerTexel(DXGI_FORMAT_R16G16B16A16_FLOAT) == 8 &&
            symtr::BytesPerTexel(DXGI_FORMAT_R8G8B8A8_TYPELESS) == 4 &&
            symtr::BytesPerTexel(DXGI_FORMAT_R11G11B10_FLOAT) == 4 && symtr::BytesPerTexel(DXGI_FORMAT_BC1_UNORM) == 0,
        "R94 SymbologyTrace: bytes per texel of the target formats (0: not copied)");
  // Keys: absent -> nothing requested (the ini read's defaults: 0 frames, views 15, segments 0, tone 1).
  {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring ini = std::wstring(tmp) + L"r94_keys_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
    WritePrivateProfileStringW(L"Perception", L"FixationDepth", L"1", ini.c_str());
    const int st = static_cast<int>(GetPrivateProfileIntW(L"Perception", L"SymbologyTrace", 0, ini.c_str()));
    const int vw = static_cast<int>(GetPrivateProfileIntW(L"Perception", L"SymbologyTraceViews", 15, ini.c_str()));
    const int sg = static_cast<int>(GetPrivateProfileIntW(L"Perception", L"SymbologyTraceSegments", 0, ini.c_str()));
    const int tn = static_cast<int>(GetPrivateProfileIntW(L"Perception", L"SymbologyTraceTone", 1, ini.c_str()));
    DeleteFileW(ini.c_str());
    symtrcfg::g_req = 0;
    symtrcfg::Configure(st, vw, sg, tn, L"C:\\x\\");
    Check(st == 0 && symtrcfg::g_req.load() == 0 && !symtr::Busy(),
          "R94 SymbologyTrace: no key -> no session requested, nothing busy (dev only, default off)");
  }
  symtrcfg::Configure(5, 0x13, 12, 0, L"C:\\x\\");
  const int r1 = symtrcfg::g_req.load();
  const bool cfg1 = symtrcfg::g_views.load() == 3 && symtrcfg::g_segs.load() == 8 && symtrcfg::g_tone.load() == 0;
  symtrcfg::g_req = 0;
  symtrcfg::Configure(5, 15, 0, 1, L"C:\\x\\");  // the same value again: no new session
  const int r2 = symtrcfg::g_req.load();
  symtrcfg::Configure(99, 15, 0, 1, L"C:\\x\\");
  const int r3 = symtrcfg::g_req.load();
  symtrcfg::Configure(0, 15, 0, 1, L"C:\\x\\");
  symtrcfg::g_req = 0;
  Check(r1 == 5 && cfg1 && r2 == 0 && r3 == 30,
        "R94 SymbologyTrace: a new value > 0 requests a session (clamped to 30 frames), the same value again does "
        "not; views masked to 4 bits, segments clamped to 8");
}

// A session on WARP through the trace's own hooks: 4 views, each frame DLSS output -> OverlayHDRPass (a HUD rectangle
// into the HDR target) -> ToneMappingPass (the draw reads the HDR, writes the LDR target) -> OverlayLDRPass (a
// rectangle into the LDR target) -> CopyToHMDPass. Views 2 / 3 share one 2-slice HDR texture (array slices).
inline void TestWarp() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* c = nullptr;
  const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &c))) {
    QvSkip("R94 SymbologyTrace WARP: no device\n");
    return;
  }
  fixdepth::State& s = fixdepth::g_s;
  ID3D11Device* oldDev = s.dev;
  ID3D11DeviceContext* oldCtx = s.ctx;
  const uint64_t oldFrame = s.frame;
  const int oldOrd = s.hmdOrd;
  ID3D11DeviceContext1* c1 = nullptr;
  c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1));
  if (!c1) {
    QvSkip("R94 SymbologyTrace WARP: no ID3D11DeviceContext1\n");
    c->Release();
    dev->Release();
    return;
  }
  s.dev = dev;
  s.ctx = c;
  const UINT W[4] = {64, 64, 96, 96}, H[4] = {48, 48, 80, 80};
  ID3D11Texture2D *hdr01[2] = {}, *hdrArr = nullptr, *ldr[4] = {};
  ID3D11RenderTargetView *hdrRtv[4] = {}, *ldrRtv[4] = {};
  ID3D11ShaderResourceView* hdrSrv[4] = {};
  {
    D3D11_TEXTURE2D_DESC td{};
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    for (int v = 0; v < 2; ++v) {
      td.Width = W[v], td.Height = H[v];
      dev->CreateTexture2D(&td, nullptr, &hdr01[v]);
      if (hdr01[v]) {
        dev->CreateRenderTargetView(hdr01[v], nullptr, &hdrRtv[v]);
        dev->CreateShaderResourceView(hdr01[v], nullptr, &hdrSrv[v]);
      }
    }
    td.Width = W[2], td.Height = H[2], td.ArraySize = 2;
    dev->CreateTexture2D(&td, nullptr, &hdrArr);
    for (int v = 2; v < 4 && hdrArr; ++v) {
      D3D11_RENDER_TARGET_VIEW_DESC rd{};
      rd.Format = td.Format;
      rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
      rd.Texture2DArray.FirstArraySlice = static_cast<UINT>(v - 2);
      rd.Texture2DArray.ArraySize = 1;
      dev->CreateRenderTargetView(hdrArr, &rd, &hdrRtv[v]);
      D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
      sd.Format = td.Format;
      sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
      sd.Texture2DArray.MipLevels = 1;
      sd.Texture2DArray.FirstArraySlice = static_cast<UINT>(v - 2);
      sd.Texture2DArray.ArraySize = 1;
      dev->CreateShaderResourceView(hdrArr, &sd, &hdrSrv[v]);
    }
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    for (int v = 0; v < 4; ++v) {
      td.Width = W[v], td.Height = H[v];
      dev->CreateTexture2D(&td, nullptr, &ldr[v]);
      D3D11_RENDER_TARGET_VIEW_DESC rd{};
      rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
      if (ldr[v]) dev->CreateRenderTargetView(ldr[v], &rd, &ldrRtv[v]);
    }
  }
  bool made = hdrArr != nullptr;
  for (int v = 0; v < 4; ++v) made = made && hdrRtv[v] && hdrSrv[v] && ldrRtv[v];
  Check(made, "R94 SymbologyTrace WARP: HDR targets (views 2 / 3 slices of one array) and LDR targets made");
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring base = std::wstring(tmp) + L"r94_symtrace_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  RemoveTree(base);
  CreateDirectoryW(base.c_str(), nullptr);
  FakePass ohdr = MakePass("OverlayHDRPass"), tone = MakePass("ToneMappingPass"), oldr = MakePass("OverlayLDRPass"),
           hmd = MakePass("CopyToHMDPass"), dlss = MakePass("DLSSPass");
  const D3D11_RECT hud{10, 8, 30, 20}, ldrHud{40, 30, 50, 40};
  const float hudC[4] = {0.0f, 4.0f, 0.0f, 0.25f}, ldrHudC[4] = {1, 0, 1, 1};
  int fi = 0;
  std::vector<uint64_t> tracedFrames;
  auto sceneOf = [&](int v, float o[4]) {
    o[0] = 0.1f * (fi % 7 + 1), o[1] = 0.01f * (v + 1), o[2] = 0.5f, o[3] = 1.0f;
  };
  auto runFrame = [&] {
    ++fi;
    const bool traced = symtr::g_on.load();
    if (traced) tracedFrames.push_back(s.frame);
    for (int v = 0; v < 4; ++v) {
      s.hmdOrd = v;
      float sc[4];
      sceneOf(v, sc);
      symtr::OnBoundary(&dlss, true, plod::kOther);
      c->ClearRenderTargetView(hdrRtv[v], sc);  // the DLSS output
      symtr::OnBoundary(&dlss, false, plod::kOther);
      symtr::OnBoundary(&ohdr, true, plod::kOther);
      c->OMSetRenderTargets(1, &hdrRtv[v], nullptr);
      c1->ClearView(hdrRtv[v], hudC, &hud, 1);  // the HUD into the HDR target
      ID3D11RenderTargetView* none = nullptr;
      c->OMSetRenderTargets(1, &none, nullptr);
      symtr::OnBoundary(&ohdr, false, plod::kOther);
      symtr::OnBoundary(&tone, true, plod::kOther);
      c->PSSetShaderResources(0, 1, &hdrSrv[v]);
      c->OMSetRenderTargets(1, &ldrRtv[v], nullptr);
      const D3D11_VIEWPORT vp{0, 0, static_cast<float>(W[v]), static_cast<float>(H[v]), 0, 1};
      if (symtr::g_learn) symtr::ToneDraw(v, c, vp);  // (the arm exists in learn and traced frames only)
      const float lc[4] = {sc[0], 0.25f, 0.75f, 1};
      c->ClearRenderTargetView(ldrRtv[v], lc);  // "the tone map"
      ID3D11ShaderResourceView* ns = nullptr;
      c->PSSetShaderResources(0, 1, &ns);
      c->OMSetRenderTargets(1, &none, nullptr);
      symtr::OnBoundary(&tone, false, plod::kOther);
      symtr::OnBoundary(&oldr, true, plod::kOther);
      c1->ClearView(ldrRtv[v], ldrHudC, &ldrHud, 1);
      symtr::OnBoundary(&oldr, false, plod::kOther);
      symtr::OnBoundary(&hmd, true, plod::kHmd);
      symtr::OnBoundary(&hmd, false, plod::kHmd);
    }
    s.hmdOrd = 0;
    ++s.frame;
    symtr::EndFrame();
  };
  // Idle: nothing runs, nothing is learnt.
  const int sess0 = symtr::g_session;
  for (int f = 0; f < 3; ++f) runFrame();
  const bool idleOk = !symtr::Busy() && tracedFrames.empty() && symtr::g_hdr[0].res == nullptr && symtr::g_out.empty();
  // A session of 2 traced frames (all views).
  symtrcfg::Configure(2, 15, 0, 0, base);
  runFrame();  // its end takes the request: the next frame learns
  const bool learnNext = symtr::g_learn && !symtr::g_on.load() && symtr::g_session == sess0 + 1;
  runFrame();  // learn
  const bool learnt = symtr::g_hdr[2].res == hdrArr && symtr::g_hdr[2].sub == 0 && symtr::g_hdr[3].sub == 1 &&
                      symtr::g_ldr[1].res == ldr[1] && tracedFrames.empty() && symtr::g_on.load();
  runFrame();  // traced 1
  runFrame();  // traced 2
  const bool roles = tracedFrames.size() == 2 && !symtr::g_on.load() && !symtr::g_learn &&
                     symtr::g_hdr[0].res == nullptr && symtr::g_ldr[3].res == nullptr;
  // The copies are mapped >= 2 frames later without waiting, waiting from 30 frames on (sym_trace.h). R107: frames
  // until they are (was a fixed 4: under CPU load WARP had not finished them, one failure in a parallel run), at
  // most 40 (by then the module waits for the copy itself).
  int drainFrames = 0;
  for (; drainFrames < 40 && !(symtr::g_caps.empty() && !symtr::Busy()); ++drainFrames) {
    if (drainFrames >= 4) Sleep(2);
    runFrame();
  }
  const bool drained = symtr::g_caps.empty() && !symtr::Busy();
  symtr::WriteFiles();
  Check(idleOk && learnNext && learnt && roles && drained,
        "R94 SymbologyTrace WARP: idle frames do nothing; a request starts one learn frame (the tone-map draw's "
        "source / target learnt, array slices as subresources), then exactly 2 traced frames; the targets are "
        "released at the session end and every copy is mapped within the next frames");
  const std::vector<std::wstring> ses = Sessions(base);
  bool files = ses.size() == 1, content = files, text = files;
  int capLines = 0;
  if (files) {
    const std::string caps = ReadText(ses[0] + L"captures.txt");
    capLines = CountLines(caps, "f");
    for (uint64_t F : tracedFrames) {
      const std::string ft = ReadText(ses[0] + L"frame_" + std::to_wstring(F) + L".txt");
      text = text && CountLines(ft, "F ") == 1 && CountLines(ft, "T ") == 4 && CountLines(ft, "P B 0 OverlayHDRPass") == 1 &&
             ft.find("pre_same 1") != std::string::npos && ft.find("pre_same 0") == std::string::npos &&
             CountLines(ft, "C ") == 16;
      for (int v = 0; v < 4; ++v) {
        auto f = [&](const char* tag) {
          return ReadAll(ses[0] + L"f" + std::to_wstring(F) + L"_v" + std::to_wstring(v) + L"_" +
                         std::wstring(tag, tag + strlen(tag)) + L".raw");
        };
        const std::vector<uint8_t> pre = f("hdr_pre"), post = f("hdr_post"), tm = f("ldr_tm"), lp = f("ldr_post");
        const size_t hb = static_cast<size_t>(W[v]) * H[v] * 8, lb = static_cast<size_t>(W[v]) * H[v] * 4;
        if (pre.size() != hb || post.size() != hb || tm.size() != lb || lp.size() != lb) {
          files = false;
          continue;
        }
        auto px8 = [&](const std::vector<uint8_t>& b, UINT x, UINT y) {
          uint16_t h4[4];
          memcpy(h4, &b[(static_cast<size_t>(y) * W[v] + x) * 8], 8);
          return std::array<float, 4>{cgrade::HalfToFloat(h4[0]), cgrade::HalfToFloat(h4[1]), cgrade::HalfToFloat(h4[2]),
                                      cgrade::HalfToFloat(h4[3])};
        };
        // The frame's own scene (the frame index fi at that frame: the scene's red channel names it).
        const auto sIn = px8(pre, 15, 12), sOut = px8(pre, 50, 40), hIn = px8(post, 15, 12), hOut = px8(post, 50, 40);
        const bool hdrOk = std::fabs(sIn[1] - 0.01f * (v + 1)) < 1e-3f && sIn[0] == sOut[0] && sIn[2] == 0.5f &&
                           std::fabs(hIn[1] - 4.0f) < 1e-3f && hIn[3] == 0.25f && hOut[0] == sOut[0] && hOut[3] == 1.0f;
        const uint8_t* t0 = &tm[(static_cast<size_t>(35) * W[v] + 45) * 4];
        const uint8_t* p0 = &lp[(static_cast<size_t>(35) * W[v] + 45) * 4];
        const uint8_t* p1 = &lp[(static_cast<size_t>(5) * W[v] + 5) * 4];
        const uint8_t* t1 = &tm[(static_cast<size_t>(5) * W[v] + 5) * 4];
        const bool ldrOk = t0[0] == t1[0] && t0[1] == 64 && p0[0] == 255 && p0[1] == 0 && p0[2] == 255 &&
                           !memcmp(p1, t1, 4);
        content = content && hdrOk && ldrOk;
      }
    }
  }
  printf("     (R94 trace: %zu session folder(s), %d capture lines, frames", ses.size(), capLines);
  for (uint64_t F : tracedFrames) printf(" %llu", static_cast<unsigned long long>(F));
  printf(")\n");
  Check(files && capLines == 32 && content,
        "R94 SymbologyTrace WARP: per traced frame and view hdr_pre (the DLSS output, no HUD), hdr_post (the HUD in "
        "the tone map's source, its alpha kept), ldr_tm (the tone map's output) and ldr_post (with the LDR overlay), "
        "each the view's own subresource at its full size; 32 lines in captures.txt");
  Check(text, "R94 SymbologyTrace WARP: each traced frame's text has its frame line, every pass boundary, one tone-map "
              "line per view (pre_same 1: the HDR copied at the overlay pass begin is the tone map's source) and 16 "
              "copy lines");
  // A view mask: only view 1 copied.
  symtrcfg::Configure(1, 0x2, 0, 0, base);
  for (int f = 0; f < 3; ++f) runFrame();  // the request, the learn frame, the traced frame
  for (int f = 0; f < 40 && symtr::Busy(); ++f) {  // R107: until its copies are mapped (was 4 more frames)
    if (f >= 4) Sleep(2);
    runFrame();
  }
  symtr::WriteFiles();
  const std::vector<std::wstring> ses2 = Sessions(base);
  int cap2 = -1;
  bool only1 = false;
  if (ses2.size() == 2) {
    const std::string caps = ReadText(ses2[1] + L"captures.txt");
    cap2 = CountLines(caps, "f");
    only1 = caps.find("_v1_") != std::string::npos && caps.find("_v0_") == std::string::npos &&
            caps.find("_v2_") == std::string::npos && caps.find("_v3_") == std::string::npos;
  }
  Check(cap2 == 4 && only1, "R94 SymbologyTrace WARP: SymbologyTraceViews=0x2 copies view 1's four targets only");
  // Uninstall in the middle of a session: everything released (no reference left on the device).
  symtrcfg::Configure(3, 15, 0, 0, base);
  runFrame();
  runFrame();  // learn
  runFrame();  // traced: copies pending
  const bool pendingBefore = !symtr::g_caps.empty();
  symtr::Reset();
  const bool resetOk = pendingBefore && symtr::g_caps.empty() && !symtr::Busy() && symtr::g_hdr[0].res == nullptr;
  symtrcfg::Configure(0, 15, 0, 1, base);
  {
    std::lock_guard<std::mutex> lock(symtr::g_outMutex);
    symtr::g_out.clear();
    symtr::g_outBytes = 0;
  }
  s.dev = oldDev;
  s.ctx = oldCtx;
  s.frame = oldFrame;
  s.hmdOrd = oldOrd;
  c->ClearState();
  auto rel = [](auto*& p) {
    if (p) p->Release();
    p = nullptr;
  };
  for (int v = 0; v < 4; ++v) rel(hdrRtv[v]), rel(hdrSrv[v]), rel(ldrRtv[v]), rel(ldr[v]);
  rel(hdr01[0]), rel(hdr01[1]), rel(hdrArr);
  rel(c1);
  rel(c);
  const ULONG last = dev->Release();
  RemoveTree(base);
  Check(resetOk && last == 0, "R94 SymbologyTrace WARP: an uninstall mid-session (Reset) releases the pending copies and "
                              "the learnt targets; no reference left on the device");
}

inline void Run() {
  printf("-- R94 SymbologyTrace (dev live HUD trace) --\n");
  TestPure();
  TestWarp();
}

}  // namespace r94
