/* Vtable slot indices of ID3D11DeviceContext1, taken from the SDK's C
   interface definitions so they never have to be hard-coded. */
#define CINTERFACE
#define COBJMACROS
#include <stddef.h>
#include <d3d11_1.h>

#define SLOT(name) (int)(offsetof(ID3D11DeviceContext1Vtbl, name) / sizeof(void*))

int DcsQv_ContextSlot(const char* name) {
  static const struct {
    const char* n;
    int s;
  } table[] = {
      {"VSSetConstantBuffers", SLOT(VSSetConstantBuffers)},
      {"PSSetShaderResources", SLOT(PSSetShaderResources)},
      {"PSSetShader", SLOT(PSSetShader)},
      {"PSSetSamplers", SLOT(PSSetSamplers)},
      {"VSSetShader", SLOT(VSSetShader)},
      {"DrawIndexed", SLOT(DrawIndexed)},
      {"Draw", SLOT(Draw)},
      {"PSSetConstantBuffers", SLOT(PSSetConstantBuffers)},
      {"IASetInputLayout", SLOT(IASetInputLayout)},
      {"IASetVertexBuffers", SLOT(IASetVertexBuffers)},
      {"IASetIndexBuffer", SLOT(IASetIndexBuffer)},
      {"DrawIndexedInstanced", SLOT(DrawIndexedInstanced)},
      {"DrawInstanced", SLOT(DrawInstanced)},
      {"IASetPrimitiveTopology", SLOT(IASetPrimitiveTopology)},
      {"VSSetShaderResources", SLOT(VSSetShaderResources)},
      {"VSSetSamplers", SLOT(VSSetSamplers)},
      {"GSSetShaderResources", SLOT(GSSetShaderResources)},
      {"HSSetShaderResources", SLOT(HSSetShaderResources)},
      {"DSSetShaderResources", SLOT(DSSetShaderResources)},
      {"CSSetShaderResources", SLOT(CSSetShaderResources)},
      {"OMSetRenderTargets", SLOT(OMSetRenderTargets)},
      {"OMSetRenderTargetsAndUnorderedAccessViews", SLOT(OMSetRenderTargetsAndUnorderedAccessViews)},
      {"OMSetBlendState", SLOT(OMSetBlendState)},
      {"OMSetDepthStencilState", SLOT(OMSetDepthStencilState)},
      {"RSSetState", SLOT(RSSetState)},
      {"CSSetUnorderedAccessViews", SLOT(CSSetUnorderedAccessViews)},
      {"ExecuteCommandList", SLOT(ExecuteCommandList)},
      {"ClearState", SLOT(ClearState)},
      {"VSSetConstantBuffers1", SLOT(VSSetConstantBuffers1)},
      {"PSSetConstantBuffers1", SLOT(PSSetConstantBuffers1)},
      {"SwapDeviceContextState", SLOT(SwapDeviceContextState)},
      {"SOSetTargets", SLOT(SOSetTargets)},
      {"FinishCommandList", SLOT(FinishCommandList)},
      {"CopyResource", SLOT(CopyResource)},
      {"CopySubresourceRegion", SLOT(CopySubresourceRegion)},
      {"ResolveSubresource", SLOT(ResolveSubresource)},
      {"UpdateSubresource", SLOT(UpdateSubresource)},
      {"ClearRenderTargetView", SLOT(ClearRenderTargetView)},
      {"ClearDepthStencilView", SLOT(ClearDepthStencilView)},
      {"ClearUnorderedAccessViewUint", SLOT(ClearUnorderedAccessViewUint)},
      {"ClearUnorderedAccessViewFloat", SLOT(ClearUnorderedAccessViewFloat)},
      {"GenerateMips", SLOT(GenerateMips)},
      {"Dispatch", SLOT(Dispatch)},
      {"DispatchIndirect", SLOT(DispatchIndirect)},
      /* gpu_dedup.h (R26 11) */
      {"Map", SLOT(Map)},
      {"Unmap", SLOT(Unmap)},
      {"DrawIndexedInstancedIndirect", SLOT(DrawIndexedInstancedIndirect)},
      {"DrawInstancedIndirect", SLOT(DrawInstancedIndirect)},
      {"CopyStructureCount", SLOT(CopyStructureCount)},
      {"UpdateSubresource1", SLOT(UpdateSubresource1)},
      {"CopySubresourceRegion1", SLOT(CopySubresourceRegion1)},
      {"ClearView", SLOT(ClearView)},
      {"DiscardResource", SLOT(DiscardResource)},
      {"DiscardView", SLOT(DiscardView)},
      {"DiscardView1", SLOT(DiscardView1)},
      {"CSSetShader", SLOT(CSSetShader)},
      /* fixation_depth.h: the viewport set with the G-buffer's depth target */
      {"RSSetViewports", SLOT(RSSetViewports)},
      /* fixation_depth.h: far depth pre-overlay copy point (draw entries) */
      {"DrawAuto", SLOT(DrawAuto)},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
    const char* a = table[i].n;
    const char* b = name;
    while (*a && *a == *b) ++a, ++b;
    if (*a == 0 && *b == 0) return table[i].s;
  }
  return -1;
}
