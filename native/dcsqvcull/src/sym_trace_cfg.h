// R94: the keys of [Perception] SymbologyTrace (sym_trace.h), read by the config thread with the R41 probe keys
// (main.cpp, LoadConfig). Dev only: every key defaults to 0 / off, and nothing of the trace runs until a
// SymbologyTrace value > 0 is set (a new value starts a new session).
//   [Perception] SymbologyTrace=N          N consecutive traced frames (1-30) after one learn frame; a new value
//                                          starts a new session (0 = nothing)
//   [Perception] SymbologyTraceViews=M     HMD views whose targets are copied (bit v = view v; default 15)
//   [Perception] SymbologyTraceSegments=K  per view and traced frame, up to K copies of the bound render target at
//                                          the record-type changes of the overlay replays (0-8; default 0)
//   [Perception] SymbologyTraceTone=T      1 = the tone probe's lines (cb0, adapted luminance) at each traced view's
//                                          tone-map draw (default 1 while a session runs)
#pragma once

namespace symtrcfg {
inline std::atomic<int> g_req{0};       // frames of a new session (taken by the render thread at a frame end)
inline std::atomic<int> g_views{15};    // SymbologyTraceViews
inline std::atomic<int> g_segs{0};      // SymbologyTraceSegments
inline std::atomic<int> g_tone{1};      // SymbologyTraceTone
inline std::mutex g_baseMutex;
inline std::wstring g_base;             // the dev folder (DcsQvCull\dev\ in dev mode), else the log folder

// Config thread, each read of the ini: a new SymbologyTrace value > 0 requests a session.
inline void Configure(int frames, int views, int segs, int tone, const std::wstring& base) {
  static int last = 0;
  g_views = views & 15;
  g_segs = segs < 0 ? 0 : segs > 8 ? 8 : segs;
  g_tone = tone != 0 ? 1 : 0;
  if (frames == last) return;
  last = frames;
  if (frames > 0 && !base.empty()) {
    {
      std::lock_guard<std::mutex> lock(g_baseMutex);
      g_base = base;
    }
    g_req = frames > 30 ? 30 : frames;
  }
}
}  // namespace symtrcfg
