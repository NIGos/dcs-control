// Runtime probe: shared helpers (images, PNG through WIC, sRGB, key=value case files, the focus fade).
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> px; // RGBA8, row-major
  Image() {}
  Image(int w_, int h_) : w(w_), h(h_), px(size_t(w_) * h_ * 4) {}
  uint8_t* at(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
  const uint8_t* at(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }
};

inline double SrgbToLinear(double v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
inline double LinearToSrgb(double v) { v = v < 0 ? 0 : (v > 1 ? 1 : v); return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055; }
inline uint8_t Enc(double linear) { return uint8_t(LinearToSrgb(linear) * 255.0 + 0.5); }
inline double Dec(uint8_t b) { return SrgbToLinear(b / 255.0); }

// Quad Views' rounded focus fade (ProjectionPS.hlsl): superellipse radius, full inside 1 - 2*smoothing, 0 at r = 1.
constexpr double kFocusShape = 8.0;
constexpr double kSmoothing = 0.2;
inline double FocusRadius(double nx, double ny) { return pow(pow(fabs(nx), kFocusShape) + pow(fabs(ny), kFocusShape), 1.0 / kFocusShape); }
inline double FocusAlpha(double nx, double ny) {
  if (fabs(nx) >= 1 || fabs(ny) >= 1) return 0;
  double r = FocusRadius(nx, ny), width = 2 * kSmoothing, t = (r - (1 - width)) / width;
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  return 1 - t * t * (3 - 2 * t);
}

struct Fov { double l = 0, r = 0, u = 0, d = 0; }; // tangents: left, right, up, down (left and down negative)

inline bool SavePng(const std::wstring& path, const Image& im) {
  IWICImagingFactory* f = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
  IWICStream* s = nullptr; IWICBitmapEncoder* e = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* pb = nullptr; bool ok = false;
  if (SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
      SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) && SUCCEEDED(e->Initialize(s, WICBitmapEncoderNoCache)) &&
      SUCCEEDED(e->CreateNewFrame(&fr, &pb)) && SUCCEEDED(fr->Initialize(pb)) && SUCCEEDED(fr->SetSize(im.w, im.h))) {
    // The PNG encoder takes BGRA, not RGBA.
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    std::vector<BYTE> bgra(im.px.size());
    for (size_t i = 0; i < im.px.size(); i += 4) { bgra[i] = im.px[i + 2]; bgra[i + 1] = im.px[i + 1]; bgra[i + 2] = im.px[i]; bgra[i + 3] = im.px[i + 3]; }
    if (SUCCEEDED(fr->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
        SUCCEEDED(fr->WritePixels(im.h, im.w * 4, UINT(bgra.size()), bgra.data())) && SUCCEEDED(fr->Commit()) && SUCCEEDED(e->Commit()))
      ok = true;
  }
  if (pb) pb->Release(); if (fr) fr->Release(); if (e) e->Release(); if (s) s->Release(); f->Release();
  return ok;
}
inline bool LoadPng(const std::wstring& path, Image& im) {
  IWICImagingFactory* f = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
  IWICBitmapDecoder* d = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* c = nullptr; bool ok = false;
  if (SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) && SUCCEEDED(d->GetFrame(0, &fr)) &&
      SUCCEEDED(f->CreateFormatConverter(&c)) &&
      SUCCEEDED(c->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
    UINT w = 0, h = 0; c->GetSize(&w, &h); im = Image(int(w), int(h));
    ok = SUCCEEDED(c->CopyPixels(nullptr, w * 4, UINT(im.px.size()), im.px.data()));
  }
  if (c) c->Release(); if (fr) fr->Release(); if (d) d->Release(); f->Release();
  return ok;
}

using Case = std::map<std::string, std::string>;
inline bool SaveCase(const std::string& path, const Case& c) {
  FILE* f = fopen(path.c_str(), "w"); if (!f) return false;
  for (auto& kv : c) fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
  fclose(f); return true;
}
inline bool LoadCase(const std::string& path, Case& c) {
  FILE* f = fopen(path.c_str(), "r"); if (!f) return false;
  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    char* eq = strchr(line, '='); if (!eq) continue; *eq = 0;
    std::string v = eq + 1; while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
    c[line] = v;
  }
  fclose(f); return true;
}
inline double Num(const Case& c, const char* k, double def = 0) { auto i = c.find(k); return i == c.end() ? def : atof(i->second.c_str()); }
inline std::string Str(const Case& c, const char* k) { auto i = c.find(k); return i == c.end() ? std::string() : i->second; }
inline std::string F(double v) { char b[48]; snprintf(b, sizeof(b), "%.9g", v); return b; }
inline std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
