// Minimal NVAPI declarations for single-pass stereo with a full second position (route S of
// PER_EYE_PASS_DESIGN.md): the support query, the stereo mode switch and the vertex shader creation with custom
// semantics. Transcribed from NVIDIA's public NVAPI SDK (github.com/NVIDIA/nvapi, main branch fetched 2026-11-11;
// nvapi.h and nvapi_interface.h); only what we call is here, in the style of nvapi_vrs_min.h. Entry points are
// reached through nvapi64.dll's nvapi_QueryInterface with the ids below; nvapi64.lib is never linked. Nothing here
// touches driver settings (no DRS call).
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
#include <string.h>

namespace nvsps {

// nvapi_interface.h (an enum rather than constexpr constants: tools/src_scan.py lists those as DCS locations).
enum InterfaceId : uint32_t {
  kId_Initialize = 0x0150e828,
  kId_D3D11_CreateVertexShaderEx = 0x0beaa0b2,
  kId_D3D_QuerySinglePassStereoSupport = 0x6f5f0a6d,
  kId_D3D_SetSinglePassStereoMode = 0xa39e6e6e,
};

enum Status : int { kOk = 0 };

// NV_CUSTOM_SEMANTIC_TYPE.
enum SemanticType : int {
  kSemXRight = 1,          // NV_X_RIGHT_SEMANTIC
  kSemViewportMask = 2,    // NV_VIEWPORT_MASK_SEMANTIC
  kSemXyzwRight = 3,       // NV_XYZW_RIGHT_SEMANTIC
  kSemPackedEyeIndex = 17  // NV_PACKED_EYE_INDEX_SEMANTIC
};

// NV_CUSTOM_SEMANTIC (NvAPI_LongString is char[256]).
struct CustomSemantic {
  uint32_t version;
  SemanticType type;
  char name[256];
  BOOL registerSpecified;
  uint32_t registerNum, registerMask, reserved;
};
static_assert(sizeof(CustomSemantic) == 280, "NV_CUSTOM_SEMANTIC layout");
enum : uint32_t { kCustomSemanticVer = 280u | (1u << 16) };

// NvAPI_D3D11_CREATE_VERTEX_SHADER_EX_V3; the SDK builds NVAPI_D3D11_CREATEVERTEXSHADEREX_VER_3 from the V2 size
// (the two sizes are equal on x64).
struct CreateVsEx {
  uint32_t version;
  uint32_t numCustomSemantics;
  CustomSemantic* pCustomSemantics;
  BOOL useWithFastGS;
  BOOL useSpecificShaderExt;
};
static_assert(sizeof(CreateVsEx) == 24, "NvAPI_D3D11_CREATE_VERTEX_SHADER_EX layout (x64)");
enum : uint32_t { kCreateVsExVer = 24u | (3u << 16) };

// NV_QUERY_SINGLE_PASS_STEREO_SUPPORT_PARAMS_V2: bit 0 single-pass stereo, bit 1 with a full second position.
struct QuerySps {
  uint32_t version;
  uint32_t bits;
};
enum : uint32_t { kQuerySpsVer = 8u | (2u << 16), kSpsBit = 1u, kSpsXyzwBit = 2u };

using QueryFn = void*(__cdecl*)(uint32_t);
using InitFn = int(__cdecl*)();
using QuerySpsFn = int(__cdecl*)(IUnknown* dev, QuerySps* q);
using SetSpsFn = int(__cdecl*)(IUnknown* devOrCtx, uint32_t numViews, uint32_t rtIndexOffset, uint8_t independentMask);
using CreateVsExFn = int(__cdecl*)(ID3D11Device* dev, const void* code, SIZE_T size, ID3D11ClassLinkage* link,
                                   const CreateVsEx* args, ID3D11VertexShader** out);

struct Api {
  QuerySpsFn query = nullptr;
  SetSpsFn setMode = nullptr;
  CreateVsExFn createVsEx = nullptr;
  bool ok() const { return query && setMode && createVsEx; }
};

// Binds the entry points from an already loaded nvapi64.dll (the caller decides whether to load it). Returns 0, or
// a negative step number.
inline int Bind(HMODULE nvapi, Api& a) {
  a = Api();
  if (!nvapi) return -1;
  const QueryFn q = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(GetProcAddress(nvapi, "nvapi_QueryInterface")));
  if (!q) return -2;
  const InitFn init = reinterpret_cast<InitFn>(q(kId_Initialize));
  if (!init || init() != kOk) return -3;
  a.query = reinterpret_cast<QuerySpsFn>(q(kId_D3D_QuerySinglePassStereoSupport));
  a.setMode = reinterpret_cast<SetSpsFn>(q(kId_D3D_SetSinglePassStereoMode));
  a.createVsEx = reinterpret_cast<CreateVsExFn>(q(kId_D3D11_CreateVertexShaderEx));
  return a.ok() ? 0 : -4;
}

inline CustomSemantic MakeSemantic(SemanticType type, const char* name) {
  CustomSemantic s = {};
  s.version = kCustomSemanticVer;
  s.type = type;
  strncpy_s(s.name, name, _TRUNCATE);
  return s;
}

}  // namespace nvsps
