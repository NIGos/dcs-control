// Minimal NVAPI declarations for the R139 measure-only VRS probe (vrs_probe.h): D3D11 variable pixel rate shading
// (the shading-rate surface, the per-viewport rate table, the capability query). Transcribed from NVIDIA's public
// NVAPI SDK (github.com/NVIDIA/nvapi, main branch fetched 2026-10-25, nvapi.h dated Sep 2, 2026; nvapi.h,
// nvapi_lite_common.h and nvapi_interface.h); only what we call is here, in the style of R53's nvapi_fgs_min.h
// (research_archive\R53_R38_focus_broadcast). Entry points are reached through nvapi64.dll's nvapi_QueryInterface
// with the ids below; nvapi64.lib is never linked. The four VRS ids are the ones R138 found 4x each in this PC's
// driver (nvapi64_impl.dll of driver 621.01, byte search): 0x99CA2DFF, 0x1B0C2F83, 0x34F7938F, 0x52B1499A, and they
// equal nvapi_interface.h's.
//
// SPDX-FileCopyrightText: Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
#pragma once

#include <d3d11.h>
#include <stdint.h>

namespace nvvrs {

// nvapi_interface.h (an enum rather than constexpr constants: tools/src_scan.py lists those as DCS locations).
// RSSetPixelShadingRateSampleOrder is 0xA942373A there (R138's recall 0x0A942373 was a shifted copy); not used.
enum InterfaceId : uint32_t {
  kId_Initialize = 0x0150e828,
  kId_D3D1x_GetGraphicsCapabilities = 0x52b1499a,
  kId_D3D11_RSSetViewportsPixelShadingRates = 0x34f7938f,
  kId_D3D11_CreateShadingRateResourceView = 0x99ca2dff,
  kId_D3D11_RSSetShadingRateResourceView = 0x1b0c2f83,
};

// nvapi_lite_common.h: MAKE_NVAPI_VERSION(type, ver) = sizeof(type) | ver << 16; NvAPI_Status values used here.
template <class T>
constexpr uint32_t MakeVersion(uint32_t ver) {
  return static_cast<uint32_t>(sizeof(T)) | (ver << 16);
}
enum Status : int {
  kOk = 0,                         // NVAPI_OK
  kError = -1,                     // NVAPI_ERROR
  kNoImplementation = -3,          // NVAPI_NO_IMPLEMENTATION
  kInvalidArgument = -5,           // NVAPI_INVALID_ARGUMENT
  kIncompatibleStructVersion = -9, // NVAPI_INCOMPATIBLE_STRUCT_VERSION
  kNotSupported = -104,            // NVAPI_NOT_SUPPORTED
};

// NV_D3D1x_GRAPHICS_CAPS_V1 / _V2 (nvapi.h). V2 is NV_D3D1x_GRAPHICS_CAPS_VER; V1 is the fallback for an older driver.
// The first word's bits: 0 bExclusiveScissorRectsSupported, 1 bVariablePixelRateShadingSupported,
// 2 bFastUAVClearSupported (V2 only).
struct GraphicsCapsV1 {
  uint32_t bits;
  uint32_t reserved[7];
};
struct GraphicsCapsV2 {
  uint32_t bits;
  uint16_t majorSMVersion, minorSMVersion, majorCudaSMVersion, minorCudaSMVersion;
  uint32_t reserved[13];
};
static_assert(sizeof(GraphicsCapsV1) == 32 && sizeof(GraphicsCapsV2) == 64, "NV_D3D1x_GRAPHICS_CAPS layouts");
constexpr uint32_t kCapsVer1 = MakeVersion<GraphicsCapsV1>(1);
constexpr uint32_t kCapsVer2 = MakeVersion<GraphicsCapsV2>(2);
constexpr uint32_t kCapVrsBit = 1u << 1;  // bVariablePixelRateShadingSupported

// NV_MAX_PIXEL_SHADING_RATES, NV_MAX_NUM_VIEWPORTS, NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH / _HEIGHT.
constexpr int kMaxRates = 16;
constexpr int kMaxViewports = 16;
constexpr int kTile = 16;

// NV_PIXEL_SHADING_RATE (an int-sized enum in the SDK).
enum PixelRate : int {
  kX0Cull = 0,  // NV_PIXEL_X0_CULL_RASTER_PIXELS
  kX16 = 1,     // NV_PIXEL_X16_PER_RASTER_PIXEL (supersampling rates 1-4: never used here)
  kX8 = 2,
  kX4 = 3,
  kX2 = 4,
  kX1 = 5,      // NV_PIXEL_X1_PER_RASTER_PIXEL: per-pixel shading
  k2x1 = 6,     // NV_PIXEL_X1_PER_2X1_RASTER_PIXELS
  k1x2 = 7,     // NV_PIXEL_X1_PER_1X2_RASTER_PIXELS
  k2x2 = 8,     // NV_PIXEL_X1_PER_2X2_RASTER_PIXELS
  k4x2 = 9,     // NV_PIXEL_X1_PER_4X2_RASTER_PIXELS
  k2x4 = 10,    // NV_PIXEL_X1_PER_2X4_RASTER_PIXELS
  k4x4 = 11,    // NV_PIXEL_X1_PER_4X4_RASTER_PIXELS
};

// NV_D3D11_VIEWPORT_SHADING_RATE_DESC_V1: a C++ bool, padded to the enum array.
struct ViewportRateDesc {
  bool enableVariablePixelShadingRate;
  PixelRate shadingRateTable[kMaxRates];
};
// NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_V1. numViewports 0 disables variable rate shading for every viewport.
struct ViewportsRateDesc {
  uint32_t version;
  uint32_t numViewports;
  ViewportRateDesc* pViewports;
};
static_assert(sizeof(ViewportRateDesc) == 68, "NV_D3D11_VIEWPORT_SHADING_RATE_DESC_V1 layout");
static_assert(sizeof(ViewportsRateDesc) == 16, "NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_V1 layout (x64)");
constexpr uint32_t kViewportsRateVer = MakeVersion<ViewportsRateDesc>(1);

// NV_SRRV_DIMENSION, NV_TEX2D_SRRV / NV_TEX2D_ARRAY_SRRV, NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_V1.
enum SrrvDimension : int { kSrrvTexture2D = 4, kSrrvTexture2DArray = 5 };
struct ShadingRateViewDesc {
  uint32_t version;
  DXGI_FORMAT Format;  // DXGI_FORMAT_R8_UINT or DXGI_FORMAT_R8_TYPELESS
  SrrvDimension ViewDimension;
  union {
    struct {
      UINT MipSlice;
    } Texture2D;
    struct {
      UINT MipSlice, FirstArraySlice, ArraySize;
    } Texture2DArray;
  };
};
static_assert(sizeof(ShadingRateViewDesc) == 24, "NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_V1 layout");
constexpr uint32_t kShadingRateViewVer = MakeVersion<ShadingRateViewDesc>(1);

// ID3D11NvShadingRateResourceView_V1 derives from ID3D11View (GetDesc after GetResource); we only bind and
// release it, so the base interface is enough.
using ShadingRateView = ID3D11View;

using QueryFn = void*(__cdecl*)(uint32_t);
using InitFn = int(__cdecl*)();
using GetCapsFn = int(__cdecl*)(IUnknown* dev, uint32_t structVersion, void* caps);
using SetRatesFn = int(__cdecl*)(IUnknown* ctx, ViewportsRateDesc* desc);
using CreateViewFn = int(__cdecl*)(ID3D11Device* dev, ID3D11Resource* res, ShadingRateViewDesc* desc,
                                   ShadingRateView** out);
using SetViewFn = int(__cdecl*)(IUnknown* ctx, ShadingRateView* view);

}  // namespace nvvrs
