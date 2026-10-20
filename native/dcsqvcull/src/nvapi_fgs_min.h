// Minimal NVAPI declarations for the R38 rank 2 work (viewport broadcast:
// FastGS with a viewport mask, VS viewport-mask custom semantic, multi-view
// and single-pass-stereo support queries). Transcribed from NVIDIA's public
// NVAPI SDK (github.com/NVIDIA/nvapi, commit 70d337d, nvapi.h,
// nvapi_lite_common.h and nvapi_interface.h); only what we call is here.
// Entry points are reached through nvapi64.dll's nvapi_QueryInterface with
// the ids below (as uav_overlap.h does); nvapi64.lib is never linked.
//
// SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

namespace nvfgs {

// nvapi_interface.h (an enum rather than constexpr constants: tools/src_scan.py lists those as DCS locations)
enum InterfaceId : uint32_t {
  kId_Initialize = 0x0150e828,
  kId_D3D11_CreateFastGeometryShaderExplicit = 0x71ab7c9c,
  kId_D3D11_CreateGeometryShaderEx_2 = 0x99ed5c1c,
  kId_D3D11_CreateVertexShaderEx = 0x0beaa0b2,
  kId_D3D_QueryMultiViewSupport = 0xb6e0a41c,
  kId_D3D_QuerySinglePassStereoSupport = 0x6f5f0a6d,
  kId_D3D_QueryModifiedWSupport = 0xcbf9f4f5,
};

// nvapi_lite_common.h: MAKE_NVAPI_VERSION(type, ver) = sizeof(type) | ver << 16
template <class T>
constexpr uint32_t MakeVersion(uint32_t ver) {
  return static_cast<uint32_t>(sizeof(T)) | (ver << 16);
}
constexpr int kNvapiOk = 0;  // NVAPI_OK

// NV_FASTGS_FLAGS
enum FastGsFlags : uint32_t {
  kFastGsUseViewportMask = 1,         // SV_ViewportArrayIndex is a bitmask of viewports
  kFastGsOffsetRtIndexByVpIndex = 2,  // SV_RenderTargetArrayIndex += viewport index
  kFastGsStrictApiOrder = 4,          // broadcast primitives strictly in API order (slow)
};

// NvAPI_D3D11_CREATE_FASTGS_EXPLICIT_DESC_V1
struct FastGsExplicitDesc {
  uint32_t version;
  uint32_t flags;
  UINT* pCoordinateSwizzling;  // optional, 16 entries; NULL when unused
};
constexpr uint32_t kFastGsExplicitVer = MakeVersion<FastGsExplicitDesc>(1);

// NV_CUSTOM_SEMANTIC_TYPE (subset)
constexpr int kSemViewportMask = 2;  // NV_VIEWPORT_MASK_SEMANTIC
// NV_CUSTOM_SEMANTIC
struct CustomSemantic {
  UINT version;
  int type;        // NV_CUSTOM_SEMANTIC_TYPE
  char name[256];  // NvAPI_LongString
  BOOL registerSpecified;
  uint32_t registerNum;
  uint32_t registerMask;
  uint32_t reserved;
};
constexpr uint32_t kCustomSemanticVer = MakeVersion<CustomSemantic>(1);

// NvAPI_D3D11_CREATE_VERTEX_SHADER_EX_V3 (the SDK's VER_3 macro is sizeof(V2) | 3 << 16;
// V2 and V3 have the same size on x64 because of the pointer's alignment).
struct CreateVsExV3 {
  UINT version;
  uint32_t numCustomSemantics;
  CustomSemantic* pCustomSemantics;
  BOOL useWithFastGS;
  BOOL useSpecificShaderExt;
};
struct CreateVsExV2 {
  UINT version;
  uint32_t numCustomSemantics;
  CustomSemantic* pCustomSemantics;
  BOOL useWithFastGS;
};
constexpr uint32_t kCreateVsExVer = MakeVersion<CreateVsExV2>(3);

// NV_QUERY_MULTIVIEW_SUPPORT_PARAMS_V1 / NV_QUERY_SINGLE_PASS_STEREO_SUPPORT_PARAMS_V2 /
// NV_QUERY_MODIFIED_W_SUPPORT_PARAMS_V1: a version and one word of flags.
struct QueryBits {
  uint32_t version;
  uint32_t bits;  // multi-view: bit 0 MultiView, 1 SPS, 2 SPS XYZW; SPS: bit 0 SPS, 1 XYZW; modified W: != 0
};
constexpr uint32_t kQueryMultiViewVer = MakeVersion<QueryBits>(1);
constexpr uint32_t kQuerySpsVer = MakeVersion<QueryBits>(2);
constexpr uint32_t kQueryModifiedWVer = MakeVersion<QueryBits>(1);

using QueryFn = void*(__cdecl*)(uint32_t);
using InitFn = int(__cdecl*)();
using CreateFastGsFn = int(__cdecl*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                     const FastGsExplicitDesc*, ID3D11GeometryShader**);
using CreateVsExFn = int(__cdecl*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, const CreateVsExV3*,
                                   ID3D11VertexShader**);
using QueryBitsFn = int(__cdecl*)(IUnknown*, QueryBits*);

struct Api {
  CreateFastGsFn createFastGs = nullptr;
  CreateVsExFn createVsEx = nullptr;
  QueryBitsFn queryMultiView = nullptr;
  QueryBitsFn querySps = nullptr;
  QueryBitsFn queryModifiedW = nullptr;
  int initStatus = -1;
  bool loaded = false;
};

// nvapi64.dll from System32 (the display driver's). False: no NVIDIA driver,
// or NvAPI_Initialize failed (initStatus).
inline bool Load(Api& a) {
  if (a.loaded) return true;
  HMODULE m = LoadLibraryW(L"nvapi64.dll");
  if (!m) return false;
  auto q = reinterpret_cast<QueryFn>(GetProcAddress(m, "nvapi_QueryInterface"));
  if (!q) return false;
  auto init = reinterpret_cast<InitFn>(q(kId_Initialize));
  if (!init) return false;
  a.initStatus = init();
  if (a.initStatus != kNvapiOk) return false;
  a.createFastGs = reinterpret_cast<CreateFastGsFn>(q(kId_D3D11_CreateFastGeometryShaderExplicit));
  a.createVsEx = reinterpret_cast<CreateVsExFn>(q(kId_D3D11_CreateVertexShaderEx));
  a.queryMultiView = reinterpret_cast<QueryBitsFn>(q(kId_D3D_QueryMultiViewSupport));
  a.querySps = reinterpret_cast<QueryBitsFn>(q(kId_D3D_QuerySinglePassStereoSupport));
  a.queryModifiedW = reinterpret_cast<QueryBitsFn>(q(kId_D3D_QueryModifiedWSupport));
  a.loaded = true;
  return true;
}

}  // namespace nvfgs
