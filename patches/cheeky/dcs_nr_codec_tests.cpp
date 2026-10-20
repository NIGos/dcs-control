// DCS Control: the DLSS-NR codec (nr_codec_shader.hpp) as DCS Control ships it, on WARP and on the hardware adapter.
// 1. The embedded bytecode (d3d_shaders.hpp, which the build uses; it never compiles HLSL) is compiled from the
//    current, patched codec source: patch-cheeky.py regenerates it.
// 2. The shipped encode and decode (one load at working scale 1, where the bilinear weights are 1, 0, 0, 0) are
//    bit-identical to upstream's unconditional four-load lerp (the same source with the early-out removed) on random
//    and edge-case inputs: HDR and SDR, the default 0.75 (exact and inexact ratios), the 32-texel floor, with and
//    without the edge fade; at scale 1 within 1 fp16 ULP or 1/1023 (the texel, not upstream's near-zero blend).
// --sampled also measures the rejected alternative: the decode's proxy/neural reads as one filtered fetch each
// (SampleLevel through a linear clamp sampler). It prints its error against the loads and the decode's GPU time,
// and asserts nothing.
#include "nr_codec_shader.hpp"
#include "d3d_shaders.hpp"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using cheeky::foveated_dlss::CodecConstants;

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char text[256]{};
        std::snprintf(text, sizeof(text), "%s failed: 0x%08X", what, static_cast<unsigned>(hr));
        throw std::runtime_error(text);
    }
}
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

std::uint16_t to_half(float value) {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    const std::int32_t exponent = static_cast<std::int32_t>((bits >> 23U) & 0xFFU) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7FFFFFU;
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<std::uint16_t>(sign);
        mantissa |= 0x800000U;
        const auto shift = static_cast<std::uint32_t>(14 - exponent);
        const std::uint32_t half_mantissa = mantissa >> shift;
        const std::uint32_t rest = mantissa & ((1U << shift) - 1U), halfway = 1U << (shift - 1U);
        return static_cast<std::uint16_t>(sign | (half_mantissa + (rest > halfway || (rest == halfway && (half_mantissa & 1U)))));
    }
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7BFFU); // 65504, never inf
    std::uint32_t half = sign | (static_cast<std::uint32_t>(exponent) << 10U) | (mantissa >> 13U);
    const std::uint32_t rest = mantissa & 0x1FFFU;
    if (rest > 0x1000U || (rest == 0x1000U && (half & 1U))) ++half;
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(half, sign | 0x7BFFU));
}
// One fp16 ULP at |value|.
double half_ulp(double value) {
    const double magnitude = std::fabs(value);
    if (magnitude < std::ldexp(1.0, -14)) return std::ldexp(1.0, -24);
    return std::ldexp(1.0, static_cast<int>(std::floor(std::log2(magnitude))) - 10);
}

// Shader variants, from the shipped source.
enum class Variant { shipped, upstream, sampled };
std::string variant_source(Variant variant) {
    std::string text = cheeky::foveated_dlss::nr_codec_shader;
    const auto replace_all = [&](const std::string& from, const std::string& to, int expected) {
        int count{};
        for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
            text.replace(at, from.size(), to);
            ++count;
        }
        require(count == expected, "Codec source changed: anchor '" + from + "' found " + std::to_string(count) + " times");
    };
    if (variant == Variant::upstream) {
        // Upstream lerps all four loads unconditionally; the shipped code skips three at working scale 1, where the
        // weights are 1, 0, 0, 0.
        replace_all("[branch] if (any(ProxySize != SourceSize))", "if (true)", 1);
        replace_all("[branch] if (any(ProxySize != RegionSize))", "if (true)", 2);
    } else if (variant == Variant::sampled) {
        text = "SamplerState DcsLinearClamp : register(s0);\n" + text;
        for (const char* source : {"Source1", "Source2"}) {
            const std::string signature = std::string("float4 Load") + source + "Bilinear(float2 position, uint2 dimensions) {";
            replace_all(signature, signature + "\n    return " + source +
                ".SampleLevel(DcsLinearClamp, (position + 0.5) / float2(dimensions), 0.0);", 1);
        }
    }
    return text;
}

ComPtr<ID3DBlob> compile(Variant variant, const char* entry) {
    const auto text = variant_source(variant);
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(text.data(), text.size(), nullptr, nullptr, nullptr, entry, "cs_5_1",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0U, &code, &errors);
    if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    check(hr, "D3DCompile");
    return code;
}

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    std::uint64_t fence_value{};
    HANDLE event{};
    ComPtr<ID3D12RootSignature> root;
    // [variant][0 encode, 1 decode]
    ComPtr<ID3D12PipelineState> pipelines[3][2];
    std::string name;
    bool hardware{};

    void submit_and_wait() {
        check(list->Close(), "Close");
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1U, lists);
        check(queue->Signal(fence.Get(), ++fence_value), "Signal");
        check(fence->SetEventOnCompletion(fence_value, event), "SetEventOnCompletion");
        WaitForSingleObject(event, INFINITE);
        check(device->GetDeviceRemovedReason(), "Device removed");
        check(allocator->Reset(), "Allocator reset");
        check(list->Reset(allocator.Get(), nullptr), "List reset");
    }
};

// dlss_nr.cpp's root signature (SRV t0-t2, UAV u0-u1, 40 constants) plus s0 for the --sampled probe.
void create_pipelines(Gpu& gpu, bool sampled) {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3U, 0U, 0U, 0U};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2U, 0U, 0U, 0U};
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1U, &ranges[0]};
    parameters[1].DescriptorTable = {1U, &ranges[1]};
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[2].Constants.Num32BitValues = 40U;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc{3U, parameters, 1U, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized, errors;
    check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors), "Serialize root");
    check(gpu.device->CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&gpu.root)), "Root signature");
    for (const auto variant : {Variant::shipped, Variant::upstream, Variant::sampled}) {
        if (variant == Variant::sampled && !sampled) continue;
        for (int stage = 0; stage < 2; ++stage) {
            const auto code = compile(variant, stage == 0 ? "EncodeMain" : "DecodeMain");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = gpu.root.Get();
            pipeline.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            check(gpu.device->CreateComputePipelineState(&pipeline,
                IID_PPV_ARGS(&gpu.pipelines[static_cast<int>(variant)][stage])), "Pipeline");
        }
    }
}

ComPtr<ID3D12Resource> texture(Gpu& gpu, std::uint32_t width, std::uint32_t height, DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1U; desc.MipLevels = 1U;
    desc.Format = format; desc.SampleDesc.Count = 1U; desc.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
        IID_PPV_ARGS(&resource)), "Texture");
    return resource;
}
ComPtr<ID3D12Resource> buffer(Gpu& gpu, std::uint64_t size, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{type};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size; desc.Height = 1U; desc.DepthOrArraySize = 1U; desc.MipLevels = 1U;
    desc.SampleDesc.Count = 1U; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&resource)), "Buffer");
    return resource;
}
void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER value{};
    value.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    value.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1U, &value);
}

void upload(Gpu& gpu, ID3D12Resource* destination, const std::vector<std::uint16_t>& texels) {
    const auto desc = destination->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total{};
    gpu.device->GetCopyableFootprints(&desc, 0U, 1U, 0U, &footprint, nullptr, nullptr, &total);
    auto staging = buffer(gpu, total, D3D12_HEAP_TYPE_UPLOAD);
    std::uint8_t* mapped{};
    check(staging->Map(0U, nullptr, reinterpret_cast<void**>(&mapped)), "Map upload");
    const auto width = static_cast<std::size_t>(desc.Width);
    for (std::uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(mapped + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch,
            texels.data() + y * width * 4U, width * 8U);
    staging->Unmap(0U, nullptr);
    D3D12_TEXTURE_COPY_LOCATION to{destination, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION from{staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    from.PlacedFootprint = footprint;
    barrier(gpu.list.Get(), destination, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.list->CopyTextureRegion(&to, 0U, 0U, 0U, &from, nullptr);
    barrier(gpu.list.Get(), destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.submit_and_wait();
}

std::vector<float> readback(Gpu& gpu, ID3D12Resource* source) {
    const auto desc = source->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total{};
    gpu.device->GetCopyableFootprints(&desc, 0U, 1U, 0U, &footprint, nullptr, nullptr, &total);
    auto staging = buffer(gpu, total, D3D12_HEAP_TYPE_READBACK);
    D3D12_TEXTURE_COPY_LOCATION from{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION to{staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    to.PlacedFootprint = footprint;
    barrier(gpu.list.Get(), source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.list->CopyTextureRegion(&to, 0U, 0U, 0U, &from, nullptr);
    barrier(gpu.list.Get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu.submit_and_wait();
    const auto width = static_cast<std::size_t>(desc.Width);
    std::vector<float> values(width * desc.Height * 4U);
    std::uint8_t* mapped{};
    check(staging->Map(0U, nullptr, reinterpret_cast<void**>(&mapped)), "Map readback");
    for (std::uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(values.data() + y * width * 4U,
            mapped + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch, width * 16U);
    staging->Unmap(0U, nullptr);
    return values;
}

enum class Pattern { random, checker, stripes, gradient, tiny, saturated, count };
const char* pattern_name(Pattern pattern) {
    constexpr const char* names[]{"random", "checker", "stripes", "gradient", "tiny", "saturated"};
    return names[static_cast<int>(pattern)];
}
std::vector<std::uint16_t> fill(std::uint32_t width, std::uint32_t height, Pattern pattern, float scale,
    std::mt19937& random) {
    std::uniform_real_distribution<float> unit(0.0F, 1.0F);
    std::vector<std::uint16_t> texels(static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            for (std::uint32_t c = 0; c < 4U; ++c) {
                float value{};
                switch (pattern) {
                case Pattern::random: value = unit(random); break;
                case Pattern::checker: value = ((x + y + c) & 1U) ? 1.0F : 0.0F; break;
                case Pattern::stripes: value = ((x / 3U + c) & 1U) ? 1.0F : 0.0F; break;
                case Pattern::gradient: value = static_cast<float>(x * 4U + c) / static_cast<float>(width * 4U); break;
                case Pattern::tiny: value = unit(random) * 1.0e-4F; break;
                case Pattern::saturated: value = ((x ^ y) & 2U) ? 1.0F : unit(random) * 0.01F; break;
                case Pattern::count: break;
                }
                texels[(static_cast<std::size_t>(y) * width + x) * 4U + c] = to_half(value * scale);
            }
    return texels;
}

// One codec dispatch: t0-t2 and u0-u1 as in dlss_nr.cpp (encode: game colour -> original copy, proxy;
// decode: original copy, proxy, neural -> game colour).
struct Binding {
    ComPtr<ID3D12DescriptorHeap> heap, cpu_heap;
    std::vector<ID3D12Resource*> uavs;
};
Binding bind(Gpu& gpu, std::initializer_list<ID3D12Resource*> srvs, std::initializer_list<ID3D12Resource*> uavs) {
    Binding b;
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5U, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    check(gpu.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&b.heap)), "Descriptor heap");
    D3D12_DESCRIPTOR_HEAP_DESC cpu{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2U, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
    check(gpu.device->CreateDescriptorHeap(&cpu, IID_PPV_ARGS(&b.cpu_heap)), "CPU heap");
    const auto step = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto handle = b.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1U;
    for (auto* resource : srvs) { gpu.device->CreateShaderResourceView(resource, &srv, handle); handle.ptr += step; }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    auto cpu_handle = b.cpu_heap->GetCPUDescriptorHandleForHeapStart();
    for (auto* resource : uavs) {
        gpu.device->CreateUnorderedAccessView(resource, nullptr, &uav, handle);
        gpu.device->CreateUnorderedAccessView(resource, nullptr, &uav, cpu_handle);
        handle.ptr += step; cpu_handle.ptr += step;
        b.uavs.push_back(resource);
    }
    return b;
}
void record(Gpu& gpu, Binding& b, ID3D12PipelineState* pipeline, const CodecConstants& constants,
    std::uint32_t dispatch_width, std::uint32_t dispatch_height) {
    ID3D12DescriptorHeap* heaps[]{b.heap.Get()};
    gpu.list->SetDescriptorHeaps(1U, heaps);
    gpu.list->SetComputeRootSignature(gpu.root.Get());
    gpu.list->SetPipelineState(pipeline);
    const auto step = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto handle = b.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.list->SetComputeRootDescriptorTable(0U, handle);
    handle.ptr += 3U * step;
    gpu.list->SetComputeRootDescriptorTable(1U, handle);
    gpu.list->SetComputeRoot32BitConstants(2U, 40U, &constants, 0U);
    gpu.list->Dispatch((dispatch_width + 15U) / 16U, (dispatch_height + 15U) / 16U, 1U);
}
std::vector<std::vector<float>> run(Gpu& gpu, Binding& b, ID3D12PipelineState* pipeline, const CodecConstants& constants,
    std::uint32_t dispatch_width, std::uint32_t dispatch_height) {
    // A sentinel first, so a texel a variant skipped cannot match by accident.
    const float sentinel[4]{-7.0F, -7.0F, -7.0F, -7.0F};
    ID3D12DescriptorHeap* heaps[]{b.heap.Get()};
    gpu.list->SetDescriptorHeaps(1U, heaps);
    const auto step = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (std::size_t i = 0; i < b.uavs.size(); ++i) {
        auto shader_visible = b.heap->GetGPUDescriptorHandleForHeapStart();
        shader_visible.ptr += (3U + i) * step;
        auto cpu = b.cpu_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += i * step;
        gpu.list->ClearUnorderedAccessViewFloat(shader_visible, cpu, b.uavs[i], sentinel, 0U, nullptr);
        D3D12_RESOURCE_BARRIER uav_barrier{};
        uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barrier.UAV.pResource = b.uavs[i];
        gpu.list->ResourceBarrier(1U, &uav_barrier);
    }
    record(gpu, b, pipeline, constants, dispatch_width, dispatch_height);
    gpu.submit_and_wait();
    std::vector<std::vector<float>> outputs;
    for (auto* resource : b.uavs) outputs.push_back(readback(gpu, resource));
    return outputs;
}

struct Difference {
    double max_abs{}, max_ulps{}, max_abs_unit{};
    double max_tolerance{}; // per value min(fp16 ULPs, |d| x 1023): <= 1 is within 1 ULP or 1/1023
    std::size_t differing{};
};
Difference compare(const std::vector<float>& reference, const std::vector<float>& candidate) {
    Difference result;
    require(reference.size() == candidate.size(), "Output sizes differ");
    for (std::size_t i = 0; i < reference.size(); ++i) {
        if (std::memcmp(&reference[i], &candidate[i], sizeof(float)) == 0) continue;
        ++result.differing;
        require(std::isfinite(reference[i]) && std::isfinite(candidate[i]), "Non-finite decode output");
        const double difference = std::fabs(static_cast<double>(reference[i]) - candidate[i]);
        result.max_abs = std::max(result.max_abs, difference);
        result.max_ulps = std::max(result.max_ulps, difference / half_ulp(reference[i]));
        result.max_tolerance = std::max(result.max_tolerance,
            std::min(difference / half_ulp(reference[i]), difference * 1023.0));
        if (std::fabs(reference[i]) <= 1.0F) result.max_abs_unit = std::max(result.max_abs_unit, difference);
    }
    return result;
}

double time_ms(Gpu& gpu, Binding& b, ID3D12PipelineState* pipeline, const CodecConstants& constants,
    std::uint32_t dispatch_width, std::uint32_t dispatch_height) {
    D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2U, 0U};
    ComPtr<ID3D12QueryHeap> queries;
    check(gpu.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries)), "Query heap");
    auto results = buffer(gpu, 16U, D3D12_HEAP_TYPE_READBACK);
    std::uint64_t frequency{};
    check(gpu.queue->GetTimestampFrequency(&frequency), "Timestamp frequency");
    std::vector<double> samples;
    for (int iteration = 0; iteration < 61; ++iteration) {
        gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0U);
        record(gpu, b, pipeline, constants, dispatch_width, dispatch_height);
        gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1U);
        gpu.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0U, 2U, results.Get(), 0U);
        gpu.submit_and_wait();
        std::uint64_t* ticks{};
        check(results->Map(0U, nullptr, reinterpret_cast<void**>(&ticks)), "Map timestamps");
        if (iteration > 0) samples.push_back(static_cast<double>(ticks[1] - ticks[0]) * 1000.0 / static_cast<double>(frequency));
        results->Unmap(0U, nullptr);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2U];
}

Gpu make_gpu(IDXGIAdapter1* adapter, bool hardware, bool sampled) {
    Gpu gpu;
    gpu.hardware = hardware;
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    char name[128]{};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
    gpu.name = name;
    check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device)), "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC queue{};
    check(gpu.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&gpu.queue)), "Queue");
    check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator)), "Allocator");
    check(gpu.device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr,
        IID_PPV_ARGS(&gpu.list)), "List");
    check(gpu.device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)), "Fence");
    gpu.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    create_pipelines(gpu, sampled);
    return gpu;
}

int run_adapter(Gpu& gpu, bool sampled) {
    std::printf("Adapter: %s%s\n", gpu.name.c_str(), gpu.hardware ? "" : " (WARP)");
    std::mt19937 random(1234U);
    const auto srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const auto fp16 = DXGI_FORMAT_R16G16B16A16_FLOAT, fp32 = DXGI_FORMAT_R32G32B32A32_FLOAT;
    const auto writable = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // Region -> working size: scale 1, 0.75 exact (multiple of 4), 0.75 inexact, the 32-texel floor (upsampling),
    // a narrow region, and a full focus view on hardware.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes{{192U, 192U}, {256U, 192U}, {257U, 193U},
        {253U, 190U}, {40U, 32U}, {24U, 32U}, {100U, 33U}};
    if (gpu.hardware) sizes.push_back({1764U, 1323U});
    int failures{};
    Difference sampled_worst, sampled_worst_exact_ratio;
    for (const auto& [region, proxy] : sizes) {
        // The game's colour is larger than the region, which starts at an offset (the encode's sub-rectangle).
        const std::uint32_t base_x = 7U, base_y = 3U, game_width = region + 16U, game_height = region + 8U;
        auto game = texture(gpu, game_width, game_height, fp16, D3D12_RESOURCE_FLAG_NONE, srv);
        auto original = texture(gpu, region, region, fp16, D3D12_RESOURCE_FLAG_NONE, srv);
        auto proxy_texture = texture(gpu, proxy, proxy, fp16, D3D12_RESOURCE_FLAG_NONE, srv);
        auto neural = texture(gpu, proxy, proxy, fp16, D3D12_RESOURCE_FLAG_NONE, srv);
        auto copy_out = texture(gpu, region, region, fp32, writable, uav);
        auto proxy_out = texture(gpu, proxy, proxy, fp32, writable, uav);
        auto decoded = texture(gpu, region, region, fp32, writable, uav);
        auto encode = bind(gpu, {game.Get(), game.Get(), game.Get()}, {copy_out.Get(), proxy_out.Get()});
        auto decode = bind(gpu, {original.Get(), proxy_texture.Get(), neural.Get()}, {decoded.Get(), decoded.Get()});
        for (int p = 0; p < static_cast<int>(Pattern::count); ++p) {
            const auto pattern = static_cast<Pattern>(p);
            upload(gpu, game.Get(), fill(game_width, game_height, pattern, 64.0F, random));
            upload(gpu, proxy_texture.Get(), fill(proxy, proxy, pattern, 1.0F, random));
            upload(gpu, neural.Get(), fill(proxy, proxy, pattern == Pattern::checker ? Pattern::stripes : pattern, 1.0F, random));
            for (const bool hdr : {false, true}) {
                CodecConstants constants{};
                constants.size[0] = constants.size[1] = region;
                constants.source_size[0] = constants.source_size[1] = region;
                constants.source_base[0] = base_x; constants.source_base[1] = base_y;
                constants.proxy_size[0] = constants.proxy_size[1] = proxy;
                constants.paper_white_scale = hdr ? 2.5F : 1.0F;
                constants.transfer_strength = constants.color_strength = 1.0F;
                constants.hdr_mode = hdr ? 1U : 0U;
                constants.region_size[0] = constants.region_size[1] = region;
                constants.foveation_width = constants.foveation_height = 1.0F;
                constants.foveation_roundness = 1.0F;
                // Encode (it ignores the HDR flag for its copy, not for its proxy).
                const auto dispatch = std::max(region, proxy);
                const auto encode_shipped = run(gpu, encode, gpu.pipelines[0][0].Get(), constants, dispatch, dispatch);
                const auto encode_upstream = run(gpu, encode, gpu.pipelines[1][0].Get(), constants, dispatch, dispatch);
                for (std::size_t i = 0; i < 2U; ++i) {
                    const auto difference = compare(encode_upstream[i], encode_shipped[i]);
                    if (difference.differing) {
                        std::printf("  FAIL encode region=%u working=%u %s %s output%zu: %zu values differ (max|d|=%.3g)\n",
                            region, proxy, hdr ? "HDR" : "SDR", pattern_name(pattern), i, difference.differing, difference.max_abs);
                        ++failures;
                    }
                }
                // Decode, over an original from the same pattern family (scene-referred up to 64 in HDR).
                upload(gpu, original.Get(), fill(region, region, Pattern::random, hdr ? 64.0F : 1.0F, random));
                for (const float feather : {0.0F, 0.45F}) {
                    constants.source_base[0] = constants.source_base[1] = 0U;
                    constants.foveation_feather = feather;
                    const auto decode_shipped = run(gpu, decode, gpu.pipelines[0][1].Get(), constants, dispatch, dispatch);
                    const auto decode_upstream = run(gpu, decode, gpu.pipelines[1][1].Get(), constants, dispatch, dispatch);
                    const auto difference = compare(decode_upstream[0], decode_shipped[0]);
                    for (const float value : decode_shipped[0]) require(value != -7.0F, "Decode skipped a pixel");
                    // At working scale 1 the shipped decode reads the texel itself; upstream's position math
                    // ((x + 0.5) * P / R - 0.5 with the GPU's approximate division) can leave a fraction of a few
                    // 1e-7 there and blend that much of the neighbour. Each within 1 fp16 ULP or 1/1023; else bit-identical.
                    if (region == proxy && difference.differing && difference.max_tolerance <= 1.0) {
                        std::printf("  decode region=%u working=%u (scale 1) %s %s: %zu values differ by max|d|=%.3g "
                            "(%.4f/1023), each within 1 fp16 ULP or 1/1023 of upstream's near-zero-weight blend\n",
                            region, proxy, hdr ? "HDR" : "SDR", pattern_name(pattern), difference.differing,
                            difference.max_abs, difference.max_abs * 1023.0);
                    } else if (difference.differing) {
                        std::printf("  FAIL decode region=%u working=%u %s %s feather=%.2f: %zu values differ (max|d|=%.3g)\n",
                            region, proxy, hdr ? "HDR" : "SDR", pattern_name(pattern), feather, difference.differing,
                            difference.max_abs);
                        ++failures;
                    }
                    if (!sampled) continue;
                    const auto decode_sampled = run(gpu, decode, gpu.pipelines[2][1].Get(), constants, dispatch, dispatch);
                    const auto probe = compare(decode_upstream[0], decode_sampled[0]);
                    auto& worst = region * 3U == proxy * 4U ? sampled_worst_exact_ratio : sampled_worst;
                    worst.max_abs = std::max(worst.max_abs, probe.max_abs);
                    worst.max_ulps = std::max(worst.max_ulps, probe.max_ulps);
                    worst.max_abs_unit = std::max(worst.max_abs_unit, probe.max_abs_unit);
                    worst.differing += probe.differing;
                }
            }
        }
        if (gpu.hardware && region == 1764U) {
            CodecConstants constants{};
            constants.size[0] = constants.size[1] = constants.source_size[0] = constants.source_size[1] = region;
            constants.proxy_size[0] = constants.proxy_size[1] = proxy;
            constants.paper_white_scale = 2.5F; constants.transfer_strength = constants.color_strength = 1.0F;
            constants.hdr_mode = 1U;
            constants.region_size[0] = constants.region_size[1] = region;
            constants.foveation_width = constants.foveation_height = 1.0F;
            constants.foveation_roundness = 1.0F; constants.foveation_feather = 0.45F;
            const auto dispatch = std::max(region, proxy);
            // A warm-up, then alternating rounds, so clock changes show up instead of favouring one variant.
            static_cast<void>(time_ms(gpu, decode, gpu.pipelines[1][1].Get(), constants, dispatch, dispatch));
            for (int round = 0; round < 3; ++round) {
                const double encode_upstream = time_ms(gpu, encode, gpu.pipelines[1][0].Get(), constants, dispatch, dispatch);
                const double encode_shipped = time_ms(gpu, encode, gpu.pipelines[0][0].Get(), constants, dispatch, dispatch);
                const double decode_upstream = time_ms(gpu, decode, gpu.pipelines[1][1].Get(), constants, dispatch, dispatch);
                const double decode_shipped = time_ms(gpu, decode, gpu.pipelines[0][1].Get(), constants, dispatch, dispatch);
                const double decode_sampled = sampled
                    ? time_ms(gpu, decode, gpu.pipelines[2][1].Get(), constants, dispatch, dispatch) : 0.0;
                std::printf("  GPU time 1764x1764, working 1323x1323, round %d (median of 60): encode upstream %.4f ms, "
                    "shipped %.4f ms; decode upstream %.4f ms, shipped %.4f ms", round, encode_upstream, encode_shipped,
                    decode_upstream, decode_shipped);
                if (sampled) std::printf(", filtered fetch %.4f ms", decode_sampled);
                std::printf("\n");
            }
        }
    }
    if (sampled) {
        std::printf("  filtered-fetch decode vs four loads, 0.75 exact ratios: max|d|=%.3g, values<=1: %.3g (%.2f/1023), %.2f fp16 ULP\n",
            sampled_worst_exact_ratio.max_abs, sampled_worst_exact_ratio.max_abs_unit,
            sampled_worst_exact_ratio.max_abs_unit * 1023.0, sampled_worst_exact_ratio.max_ulps);
        std::printf("  filtered-fetch decode vs four loads, other ratios: max|d|=%.3g, values<=1: %.3g (%.2f/1023), %.2f fp16 ULP\n",
            sampled_worst.max_abs, sampled_worst.max_abs_unit, sampled_worst.max_abs_unit * 1023.0, sampled_worst.max_ulps);
    }
    return failures;
}

// The build embeds d3d_shaders.hpp; it must be the current codec source compiled (patch-cheeky.py regenerates it).
void check_embedded_bytecode() {
    using namespace cheeky::foveated_dlss;
    const auto decode = compile(Variant::shipped, "DecodeMain");
    const auto encode = compile(Variant::shipped, "EncodeMain");
    require(decode->GetBufferSize() == d3d_shaders::nr_decode.size &&
        std::memcmp(decode->GetBufferPointer(), d3d_shaders::nr_decode.data, d3d_shaders::nr_decode.size) == 0,
        "d3d_shaders.hpp nr_decode is not compiled from the current nr_codec_shader.hpp");
    require(encode->GetBufferSize() == d3d_shaders::nr_encode.size &&
        std::memcmp(encode->GetBufferPointer(), d3d_shaders::nr_encode.data, d3d_shaders::nr_encode.size) == 0,
        "d3d_shaders.hpp nr_encode is not compiled from the current nr_codec_shader.hpp");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const bool sampled = argc > 1 && std::strcmp(argv[1], "--sampled") == 0;
        check_embedded_bytecode();
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory2(0U, IID_PPV_ARGS(&factory)), "DXGI factory");
        int failures{};
        ComPtr<IDXGIAdapter1> warp;
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
        {
            auto gpu = make_gpu(warp.Get(), false, sampled);
            failures += run_adapter(gpu, sampled);
        }
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index, adapter.Reset()) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
                FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) continue;
            auto gpu = make_gpu(adapter.Get(), true, sampled);
            failures += run_adapter(gpu, sampled);
            break;
        }
        if (failures) {
            std::fprintf(stderr, "FAIL: %d codec cases differ from upstream's four-load lerp\n", failures);
            return 1;
        }
        std::puts("PASS: NR codec bytecode current, encode and decode bit-identical to upstream's four-load lerp");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "DCS NR codec test: %s\n", error.what());
        return 1;
    }
}
