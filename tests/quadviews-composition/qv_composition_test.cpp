// DCS Control: the bundled Quad Views composition, as built by
// scripts/build-quadviews.ps1, against the same shaders without our
// performance edits (the early out of the composition and the sharpening's
// skipped tiles). Every pixel of the composited image must be identical, for
// the round and the rectangular focus area, with and without sharpening, with
// the focus area centred and off centre. Exit code 0 on success.
//
// With Far depth built in (dcsvr_depthwarp_ps.hlsli injected into ProjectionPS.hlsl), the reference is the shader the
// build kept from before that injection (ProjectionPS.nofardepth.hlsl): the composition with every Far depth constant
// at 0 must equal it pixel for pixel, with a non-zero field bound at t2 that it must not read. Given the DLL's generated
// shader header, the same holds for the DLL's own bytecode against the reference compiled as fxc compiles it.
//
//   qv_composition_test.exe <quad views openxr-api-layer dir> <FidelityFX-CAS ffx-cas dir> [<built ProjectionPS.h>]

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

void replace_once(std::string& text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    if (at == std::string::npos) throw std::runtime_error("anchor not found: " + from);
    text.replace(at, from.size(), to);
}

class Includes final : public ID3DInclude {
public:
    explicit Includes(std::vector<std::filesystem::path> directories) : directories_(std::move(directories)) {}
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
        for (const auto& directory : directories_) {
            const auto path = directory / name;
            if (std::filesystem::exists(path)) {
                auto* text = new std::string(read_text(path));
                *data = text->data();
                *bytes = static_cast<UINT>(text->size());
                held_.push_back(text);
                return S_OK;
            }
        }
        return E_FAIL;
    }
    HRESULT __stdcall Close(LPCVOID) override { return S_OK; }
    ~Includes() { for (auto* text : held_) delete text; }

private:
    std::vector<std::filesystem::path> directories_;
    std::vector<std::string*> held_;
};

ComPtr<ID3DBlob> compile(const std::string& source, const char* entry, const char* target, Includes& includes,
    const std::vector<D3D_SHADER_MACRO>& macros = {}, UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3) {
    std::vector<D3D_SHADER_MACRO> defined = macros;
    defined.push_back({nullptr, nullptr});
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source.data(), source.size(), "shader", defined.data(), &includes, entry, target,
            flags, 0, &code, &errors))) {
        throw std::runtime_error(std::string("compile failed: ") +
            (errors ? static_cast<const char*>(errors->GetBufferPointer()) : entry));
    }
    return code;
}

void check(HRESULT result, const char* what) {
    if (FAILED(result)) throw std::runtime_error(what);
}

// The bytecode fxc wrote into the layer's generated header (const BYTE g_ProjectionPS[] = { 68, 88, ... };).
std::vector<std::uint8_t> header_bytecode(const std::filesystem::path& path) {
    const std::string text = read_text(path);
    const auto start = text.find("g_ProjectionPS[]");
    const auto open = start == std::string::npos ? start : text.find('{', start);
    const auto close = open == std::string::npos ? open : text.find('}', open);
    if (close == std::string::npos) throw std::runtime_error("no g_ProjectionPS in " + path.string());
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = open + 1; i < close;) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            ++i;
            continue;
        }
        std::size_t used = 0;
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 4), &used)));
        i += used;
    }
    return bytes;
}

struct Case {
    float shape;
    float smoothing;
    float offset_x, offset_y;
    bool sharpen;
    bool unpremultiplied;
    float taper = 0.f;  // dcsvr_sharpen_taper band (0: off)
    float edge = 0.f;   // dcsvr_edge_match (0: off)
};

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("usage: qv_composition_test <openxr-api-layer dir> <ffx-cas dir>");
        const std::filesystem::path layer_dir = argv[1];
        const std::filesystem::path cas_dir = argv[2];
        Includes includes({layer_dir, cas_dir});

        const std::string projection_new = read_text(layer_dir / "ProjectionPS.hlsl");
        // Far depth built in: the reference is the shader from before its injection, kept by the build.
        const bool far_depth = projection_new.find("dcsvrDisparityField") != std::string::npos;
        const std::string projection_base = far_depth ? read_text(layer_dir / "ProjectionPS.nofardepth.hlsl") : projection_new;
        if (projection_base.empty()) throw std::runtime_error("ProjectionPS.nofardepth.hlsl missing: run scripts/build-quadviews.ps1");
        // Edge match's scale comes from the coordinates as the views project them, never from the ones Far depth
        // displaced (the displacement jumps across depth edges: its derivative read as a huge scale, a blur halo), and
        // with edge match off none of it runs.
        for (const std::string* ps : {&projection_new, &projection_base}) {
            if (ps->find("ddx(layer1TexCoord)") != std::string::npos || ps->find("ddy(layer1TexCoord)") != std::string::npos ||
                ps->find("ddx(layer1ProjectedCoordNdc)") != std::string::npos)
                throw std::runtime_error("edge match takes its derivatives from the displaced focus coordinate");
            if (ps->find("projectedFocusCoord.xy / projectedFocusCoord.z) * float2(0.5f, -0.5f)") == std::string::npos)
                throw std::runtime_error("edge match: no derivative of the undisplaced focus coordinate");
            if (ps->find("[branch] if (dcsvrEdgeMatch > 0) {") == std::string::npos)
                throw std::runtime_error("edge match off still computes its scale");
        }
        {
            // layer.cpp's per-frame paths (no device needed): the submission log (0022, 0027) compares a fixed signature
            // before it formats anything and forgets destroyed swapchains; two layers with three views (0029) asks for
            // the periphery pass, so the filled right periphery is what the layer sends.
            const std::string layer = read_text(layer_dir / "layer.cpp");
            const auto body = [&](const std::string& head) {
                const auto at = layer.find(head);
                if (at == std::string::npos) throw std::runtime_error("layer.cpp: no " + head);
                const auto end = layer.find("\n        }\n", at) == std::string::npos ? layer.find("\r\n        }\r\n", at)
                                                                                     : layer.find("\n        }\n", at);
                return layer.substr(at, end - at);
            };
            const std::string log = body("void dcsvrLogSubmission(const XrFrameEndInfo& info) {");
            const auto compare = log.find("if (sig == m_dcsvrSubmitSig)");
            if (compare == std::string::npos || log.find("fmt::format(\"", 0) < compare)
                throw std::runtime_error("submission log formats text before it compares the frame's signature");
            if (body("XrResult xrDestroySwapchain(XrSwapchain swapchain) override {").find("dcsvrForgetSwapchain(swapchain)") == std::string::npos)
                throw std::runtime_error("submission log: a destroyed swapchain stays in the format map");
            if (body("bool dcsvrTwoLayerPeripheryPass() const {").find("m_dcsvrThreeViews.Active()") == std::string::npos)
                throw std::runtime_error("two layers: three views does not ask for the periphery pass");
            if (layer.find("focus.dcsvrEdge[2] = m_dcsvrTwoLayersAlpha == 3 ? 0.f") == std::string::npos)
                throw std::runtime_error("opaque focus without a periphery source takes mode 2's sRGB encoding");
        }
        std::string projection_old = projection_base;
        replace_once(projection_old, "[branch] if (!focusVisible && !debugFocusView) {", "[branch] if (false) {");
        replace_once(projection_old, "sourceFocusTexture.SampleLevel(sourceSampler, layer1TexCoord, 0)",
            "sourceFocusTexture.Sample(sourceSampler, layer1TexCoord)");
        // The reference also has no edge match (0019-edge-match.patch): with dcsvr_edge_match 0 every case below must be
        // pixel-identical to the composition without it.
        if (projection_old.find("[branch] if (dcsvrEdgeMatch > 0 &&") != std::string::npos)
            replace_once(projection_old, "[branch] if (dcsvrEdgeMatch > 0 &&", "[branch] if (false &&");
        const std::string vertex = read_text(layer_dir / "ProjectionVS.hlsl");
        const std::string sharpening = read_text(layer_dir / "SharpeningCS.hlsl");

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL level{};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                &device, &level, &context))) {
            check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                      &device, &level, &context), "device");
        }

        const auto vs_code = compile(vertex, "main", "vs_5_0", includes);
        const auto ps_new_code = compile(projection_new, "main", "ps_5_0", includes);
        const auto ps_old_code = compile(projection_old, "main", "ps_5_0", includes);
        const auto cs_code = compile(sharpening, "main", "cs_5_0", includes,
            {{"CAS_SAMPLE_FP16", "0"}, {"CAS_SAMPLE_SHARPEN_ONLY", "1"}, {"WIDTH", "64"}, {"HEIGHT", "1"}, {"DEPTH", "1"}});
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps_new, ps_old, ps_dll, ps_base_fxc;
        ComPtr<ID3D11ComputeShader> cs;
        check(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs), "vs");
        check(device->CreatePixelShader(ps_new_code->GetBufferPointer(), ps_new_code->GetBufferSize(), nullptr, &ps_new), "ps new");
        check(device->CreatePixelShader(ps_old_code->GetBufferPointer(), ps_old_code->GetBufferSize(), nullptr, &ps_old), "ps old");
        check(device->CreateComputeShader(cs_code->GetBufferPointer(), cs_code->GetBufferSize(), nullptr, &cs), "cs");
        // The DLL's own composition shader and the reference compiled with fxc's defaults (no flags: level 1).
        if (far_depth && argc > 3 && std::filesystem::exists(argv[3])) {
            const auto dll_code = header_bytecode(argv[3]);
            check(device->CreatePixelShader(dll_code.data(), dll_code.size(), nullptr, &ps_dll), "ps dll");
            const auto base_code = compile(projection_base, "main", "ps_5_0", includes, {}, 0);
            check(device->CreatePixelShader(base_code->GetBufferPointer(), base_code->GetBufferSize(), nullptr, &ps_base_fxc), "ps base");
        }

        // QV_BENCH=1: the composition's GPU time at a Crystal Super's size instead of the equality test.
        const bool bench = GetEnvironmentVariableA("QV_BENCH", nullptr, 0) != 0;
        const UINT kOut = bench ? 5400U : 1024U;
        const UINT kFocus = bench ? 2076U : 600U;
        std::mt19937 random(1234);
        const auto make_texture = [&](UINT size, DXGI_FORMAT format, UINT bind, bool fill) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = size;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = bind;
            std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size) * size);
            for (auto& p : pixels) p = random();
            D3D11_SUBRESOURCE_DATA data{pixels.data(), size * 4U, 0};
            ComPtr<ID3D11Texture2D> texture;
            check(device->CreateTexture2D(&desc, fill ? &data : nullptr, &texture), "texture");
            return texture;
        };
        const auto stereo = make_texture(kOut, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, true);
        const auto focus = make_texture(kFocus, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, true);
        D3D11_TEXTURE2D_DESC sharpened_desc{};
        sharpened_desc.Width = sharpened_desc.Height = kFocus;
        sharpened_desc.MipLevels = sharpened_desc.ArraySize = 1;
        sharpened_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sharpened_desc.SampleDesc.Count = 1;
        sharpened_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> sharpened;
        check(device->CreateTexture2D(&sharpened_desc, nullptr, &sharpened), "sharpened");
        D3D11_TEXTURE2D_DESC target_desc{};
        target_desc.Width = target_desc.Height = kOut;
        target_desc.MipLevels = target_desc.ArraySize = 1;
        target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        target_desc.SampleDesc.Count = 1;
        target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target, staging;
        check(device->CreateTexture2D(&target_desc, nullptr, &target), "target");
        target_desc.BindFlags = 0;
        target_desc.Usage = D3D11_USAGE_STAGING;
        target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&target_desc, nullptr, &staging), "staging");

        ComPtr<ID3D11ShaderResourceView> stereo_srv, focus_srv, sharpened_srv;
        ComPtr<ID3D11UnorderedAccessView> sharpened_uav;
        ComPtr<ID3D11RenderTargetView> rtv;
        check(device->CreateShaderResourceView(stereo.Get(), nullptr, &stereo_srv), "srv");
        check(device->CreateShaderResourceView(focus.Get(), nullptr, &focus_srv), "srv");
        check(device->CreateShaderResourceView(sharpened.Get(), nullptr, &sharpened_srv), "srv");
        check(device->CreateUnorderedAccessView(sharpened.Get(), nullptr, &sharpened_uav), "uav");
        check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "rtv");

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        check(device->CreateSamplerState(&sampler_desc, &sampler), "sampler");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        ComPtr<ID3D11RasterizerState> rasterizer;
        check(device->CreateRasterizerState(&raster, &rasterizer), "raster");

        const auto buffer = [&](UINT size) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = size;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            ComPtr<ID3D11Buffer> created;
            check(device->CreateBuffer(&desc, nullptr, &created), "buffer");
            return created;
        };
        const auto vs_constants = buffer(64), ps_constants = buffer(96), cs_constants = buffer(64);

        // A Far depth field full of non-zero displacements, bound at t2 in every run: with the Far depth constants at 0
        // the composition must not read it.
        ComPtr<ID3D11ShaderResourceView> field_srv;
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = 256;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16G16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            std::vector<std::uint16_t> halves(256 * 256 * 2, 0x2A00);  // 0.047 focus uv, about 28 px of a 600 px view
            D3D11_SUBRESOURCE_DATA data{halves.data(), 256 * 4U, 0};
            ComPtr<ID3D11Texture2D> field;
            check(device->CreateTexture2D(&desc, &data, &field), "field");
            check(device->CreateShaderResourceView(field.Get(), nullptr, &field_srv), "field srv");
        }

        // The opaque focus check (below) composes with a focus area of another size over another periphery texture.
        float focus_area = 0.34f;
        ID3D11ShaderResourceView* stereo_view = nullptr;
        const auto run = [&](const Case& c, bool optimized, ID3D11PixelShader* shader = nullptr) {
            // Sharpening (skipping tiles only in the optimized chain).
            ID3D11ShaderResourceView* focus_view = focus_srv.Get();
            if (c.sharpen) {
                const float zero[4]{};
                context->ClearUnorderedAccessViewFloat(sharpened_uav.Get(), zero);
                // CAS sharpen-only setup at 0.7 (CasSetup's output, precomputed for 600x600 at sharpness 0.7).
                struct { std::uint32_t c0[4]; std::uint32_t c1[4]; float skip[4]; float taper[4]; } k{};
                const float sharp = -1.0f / (8.0f + (5.0f - 8.0f) * 0.7f);
                const float one = 1.0f;
                std::memcpy(&k.c0[0], &one, 4); std::memcpy(&k.c0[1], &one, 4);
                const float zerof = 0.0f;
                std::memcpy(&k.c0[2], &zerof, 4); std::memcpy(&k.c0[3], &zerof, 4);
                std::memcpy(&k.c1[0], &sharp, 4);
                k.c1[1] = 0;  // half packing unused at FP32
                const float eight = 8.0f * sharp;
                std::memcpy(&k.c1[2], &eight, 4);
                k.c1[3] = 0;
                k.skip[0] = optimized ? c.shape : 0.f;
                k.skip[1] = 2.f;
                k.skip[2] = k.skip[3] = static_cast<float>(kFocus);
                k.taper[0] = c.taper;
                k.taper[1] = c.shape;
                context->UpdateSubresource(cs_constants.Get(), 0, nullptr, &k, 0, 0);
                context->CSSetConstantBuffers(0, 1, cs_constants.GetAddressOf());
                context->CSSetShaderResources(0, 1, focus_srv.GetAddressOf());
                context->CSSetUnorderedAccessViews(0, 1, sharpened_uav.GetAddressOf(), nullptr);
                context->CSSetShader(cs.Get(), nullptr, 0);
                context->Dispatch((kFocus + 15) / 16, (kFocus + 15) / 16, 1);
                ID3D11UnorderedAccessView* none[1]{};
                context->CSSetUnorderedAccessViews(0, 1, none, nullptr);
                focus_view = sharpened_srv.Get();
            }
            // Focus area: 0.34 of the view (focus_area), possibly off centre.
            const float sx = 1.f / focus_area, sy = 1.f / focus_area;
            const float m[4][4] = {{sx, 0, 0, 0}, {0, sy, 0, 0}, {0, 0, 1, 0}, {-c.offset_x * sx, -c.offset_y * sy, 0, 1}};
            float column_major[16];
            for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) column_major[j * 4 + i] = m[i][j];
            context->UpdateSubresource(vs_constants.Get(), 0, nullptr, column_major, 0, 0);
            // The upstream constants with edge match before the shape (0019-edge-match.patch), then Far depth's (warp, reuse), all 0: Far depth off.
            struct { float smoothing; std::uint32_t ignore_alpha, unpremultiplied, debug; float edge; float shape; float pad[2]; float warp[4]; float reuse[12]; } p{};
            static_assert(sizeof(p) == 96);
            p.smoothing = c.smoothing;
            p.ignore_alpha = 0xFFFFFFFEu;
            p.unpremultiplied = c.unpremultiplied ? 1u : 0u;
            p.shape = c.shape;
            p.edge = c.edge;
            context->UpdateSubresource(ps_constants.Get(), 0, nullptr, &p, 0, 0);
            const float clear[4]{0.25f, 0.5f, 0.75f, 1.f};
            context->ClearRenderTargetView(rtv.Get(), clear);
            context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            D3D11_VIEWPORT viewport{0, 0, static_cast<float>(kOut), static_cast<float>(kOut), 0, 1};
            context->RSSetViewports(1, &viewport);
            context->RSSetState(rasterizer.Get());
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context->VSSetShader(vs.Get(), nullptr, 0);
            context->VSSetConstantBuffers(0, 1, vs_constants.GetAddressOf());
            context->PSSetShader(shader ? shader : optimized ? ps_new.Get() : ps_old.Get(), nullptr, 0);
            context->PSSetConstantBuffers(0, 1, ps_constants.GetAddressOf());
            ID3D11ShaderResourceView* views[3] = {stereo_view ? stereo_view : stereo_srv.Get(), focus_view, field_srv.Get()};
            context->PSSetShaderResources(0, 3, views);
            context->PSSetSamplers(0, 1, sampler.GetAddressOf());
            context->Draw(3, 0);
            ID3D11ShaderResourceView* none[3]{};
            context->PSSetShaderResources(0, 3, none);
            if (bench) return std::vector<std::uint32_t>{};
            context->CopyResource(staging.Get(), target.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map");
            std::vector<std::uint32_t> out(static_cast<std::size_t>(kOut) * kOut);
            for (UINT y = 0; y < kOut; ++y)
                std::memcpy(&out[static_cast<std::size_t>(y) * kOut],
                    static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, kOut * 4);
            context->Unmap(staging.Get(), 0);
            return out;
        };

        if (bench) {
            D3D11_QUERY_DESC disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp_desc{D3D11_QUERY_TIMESTAMP, 0};
            ComPtr<ID3D11Query> disjoint, begin, end;
            check(device->CreateQuery(&disjoint_desc, &disjoint), "query");
            check(device->CreateQuery(&stamp_desc, &begin), "query");
            check(device->CreateQuery(&stamp_desc, &end), "query");
            const auto time = [&](const Case& c, bool optimized) {
                std::vector<double> samples;
                for (int i = 0; i < 40; ++i) {
                    context->Begin(disjoint.Get());
                    context->End(begin.Get());
                    static_cast<void>(run(c, optimized));
                    context->End(end.Get());
                    context->End(disjoint.Get());
                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
                    UINT64 b = 0, e = 0;
                    while (context->GetData(disjoint.Get(), &d, sizeof(d), 0) != S_OK) {}
                    while (context->GetData(begin.Get(), &b, sizeof(b), 0) != S_OK) {}
                    while (context->GetData(end.Get(), &e, sizeof(e), 0) != S_OK) {}
                    if (!d.Disjoint && i >= 5) samples.push_back(double(e - b) * 1000.0 / double(d.Frequency));
                }
                std::sort(samples.begin(), samples.end());
                return samples[samples.size() / 2];
            };
            for (const Case c : {Case{2.f, 0.2f, 0.f, 0.f, true, false}, Case{2.f, 0.2f, 0.f, 0.f, false, false}}) {
                const double before = time(c, false), after = time(c, true);
                std::printf("bench %ux%u focus %u sharpen=%d: composition median before %.3f ms, after %.3f ms%s",
                    kOut, kOut, kFocus, c.sharpen ? 1 : 0, before, after, "\n");
            }
            return 0;
        }
        const std::array<Case, 8> cases{{
            {2.f, 0.2f, 0.f, 0.f, false, false}, {2.f, 0.2f, 0.15f, -0.1f, false, false},
            {2.f, 0.05f, -0.2f, 0.2f, true, false}, {2.f, 0.2f, 0.f, 0.f, true, false},
            {0.f, 0.2f, 0.f, 0.f, false, false}, {0.f, 0.f, 0.1f, 0.1f, true, false},
            {2.f, 0.2f, 0.1f, 0.f, true, true}, {4.f, 0.1f, 0.f, 0.f, true, false}}};
        bool all_identical = true;
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const auto before = run(cases[i], false);
            const auto after = run(cases[i], true);
            std::size_t differing = 0;
            for (std::size_t p = 0; p < before.size(); ++p) differing += before[p] != after[p] ? 1U : 0U;
            std::printf("case %zu shape=%.0f smoothing=%.2f offset=(%.2f,%.2f) sharpen=%d unpremultiplied=%d: %zu differing pixels of %zu\n",
                i, cases[i].shape, cases[i].smoothing, cases[i].offset_x, cases[i].offset_y, cases[i].sharpen ? 1 : 0,
                cases[i].unpremultiplied ? 1 : 0, differing, before.size());
            all_identical = all_identical && differing == 0;
            // The test means something only where the focus view shows: count the pixels the focus changed
            // against the same composition with the focus area pushed out of view.
            Case away = cases[i];
            away.offset_x = 50.f;
            const auto periphery_only = run(away, false);
            std::size_t focus_pixels = 0;
            for (std::size_t p = 0; p < before.size(); ++p) focus_pixels += before[p] != periphery_only[p] ? 1U : 0U;
            std::printf("    focus area pixels %zu%s", focus_pixels, "\n");
            all_identical = all_identical && focus_pixels > before.size() / 20U;
        }
        std::printf(all_identical ? "Quad Views composition identical\n" : "Quad Views composition DIFFERS\n");

        // Far depth off in the DLL itself: its bytecode, every Far depth constant 0 and the field bound, against the
        // shader without Far depth compiled as fxc compiles it.
        bool far_depth_off = true;
        if (far_depth) {
            if (!ps_dll) {
                std::printf("Far depth off: the DLL's ProjectionPS.h not given or not found, source check only\n");
            } else {
                for (std::size_t i = 0; i < cases.size(); ++i) {
                    const auto dll = run(cases[i], true, ps_dll.Get());
                    const auto reference = run(cases[i], true, ps_base_fxc.Get());
                    std::size_t differing = 0;
                    for (std::size_t p = 0; p < dll.size(); ++p) differing += dll[p] != reference[p] ? 1U : 0U;
                    std::printf("far depth off, DLL bytecode, case %zu: %zu differing pixels of %zu\n", i, differing, dll.size());
                    far_depth_off = far_depth_off && differing == 0;
                }
                std::printf(far_depth_off ? "Quad Views composition with Far depth off identical to the composition without it (DLL bytecode)\n"
                                          : "Quad Views composition with Far depth off DIFFERS from the composition without it\n");
            }
        }

        // Sharpening taper (focus radius in the shape's own norm): the same composition in the inner half of the focus area, a softer one in its edge band.
        bool taper_ok = true;
        for (const float shape : {2.f, 8.f}) {
            Case on{shape, 0.2f, 0.f, 0.f, true, false, 0.4f};
            Case off = on;
            off.taper = 0.f;
            const auto plain = run(off, true), tapered = run(on, true);
            const float mid = kOut * 0.5f, half = kOut * 0.5f * 0.34f;
            std::size_t centre = 0, band = 0;
            for (UINT y = 0; y < kOut; ++y)
                for (UINT x = 0; x < kOut; ++x) {
                    const std::size_t p = static_cast<std::size_t>(y) * kOut + x;
                    if (plain[p] == tapered[p]) continue;
                    const float ax = std::fabs(x + 0.5f - mid) / half, ay = std::fabs(y + 0.5f - mid) / half;
                    const float d = std::pow(std::pow(ax, shape) + std::pow(ay, shape), 1.f / shape);
                    if (d < 0.5f) ++centre; else ++band;
                }
            std::printf("taper shape=%.0f: %zu changed pixels in the inner half, %zu in the edge band%s", shape, centre, band, "\n");
            taper_ok = taper_ok && centre == 0 && band > 1000U;
        }
        std::printf(taper_ok ? "Quad Views sharpening taper only softens the edge band%s" : "Quad Views sharpening taper FAILED%s", "\n");

        // Edge match: the focus view is 600 texels over 0.34 of a 1024-texel periphery (1.7 focus texels per periphery
        // texel), so inside the fade band its detail is tapered. Nothing changes inside the band's inner edge or outside
        // the focus area, and the band changes.
        bool edge_ok = true;
        for (const float shape : {2.f, 8.f}) {
            Case on{shape, 0.2f, 0.f, 0.f, false, false, 0.f, 0.6f};
            Case off = on;
            off.edge = 0.f;
            const auto plain = run(off, true), matched = run(on, true);
            const float mid = kOut * 0.5f, half = kOut * 0.5f * 0.34f;
            std::size_t inner = 0, band = 0, outside = 0;
            for (UINT y = 0; y < kOut; ++y)
                for (UINT x = 0; x < kOut; ++x) {
                    const std::size_t p = static_cast<std::size_t>(y) * kOut + x;
                    if (plain[p] == matched[p]) continue;
                    const float ax = std::fabs(x + 0.5f - mid) / half, ay = std::fabs(y + 0.5f - mid) / half;
                    const float d = std::pow(std::pow(ax, shape) + std::pow(ay, shape), 1.f / shape);
                    if (d < 0.6f - 0.01f) ++inner; else if (d > 1.f + 0.01f) ++outside; else ++band;
                }
            std::printf("edge match shape=%.0f: %zu changed pixels inside the band, %zu in it, %zu outside the focus%s",
                shape, inner, band, outside, "\n");
            edge_ok = edge_ok && inner == 0 && outside == 0 && band > 1000U;
        }
        std::printf(edge_ok ? "Quad Views edge match only changes the fade band%s" : "Quad Views edge match FAILED%s", "\n");

        // Two layers (dcsvr_twolayer_ps.hlsl), the focus layer's pass: with edge match off (dcsvrEdge 0, or k <= 1) the
        // output equals the pass without edge match (the same source with its branch removed) for every shape, band,
        // sRGB encoding and fill; with it on (0021: k from the CPU) only the fade band changes.
        bool two_layer_ok = true;
        const auto two_layer_path = layer_dir / "dcsvr_twolayer_ps.hlsl";
        if (!std::filesystem::exists(two_layer_path)) {
            std::printf("two layers: dcsvr_twolayer_ps.hlsl not in the layer folder%s", "\n");
            two_layer_ok = false;
        } else {
            const std::string two_new = read_text(two_layer_path);
            std::string two_old = two_new;
            replace_once(two_old, "[branch] if (dcsvrEdge.x > 0 &&", "[branch] if (false &&");
            // Without 0023's alpha conventions either: mode 0 (dcsvrEdge.z 0) must give the bytes of the pass before it.
            replace_once(two_old, "[branch] if (dcsvrEdge.z >= 2) {", "[branch] if (false) {");
            replace_once(two_old, "} else if (dcsvrEdge.z >= 1) {", "} else if (false) {");
            // Nor 0024's opaque focus (dcsvrEdge.w): with it 0 the pass must give the bytes of the pass before it.
            replace_once(two_old, "[branch] if (dcsvrEdge.w != 0) {", "[branch] if (false) {");
            const auto two_new_code = compile(two_new, "main", "ps_5_0", includes);
            const auto two_old_code = compile(two_old, "main", "ps_5_0", includes);
            ComPtr<ID3D11PixelShader> two_new_ps, two_old_ps;
            check(device->CreatePixelShader(two_new_code->GetBufferPointer(), two_new_code->GetBufferSize(), nullptr, &two_new_ps), "ps two new");
            check(device->CreatePixelShader(two_old_code->GetBufferPointer(), two_old_code->GetBufferSize(), nullptr, &two_old_ps), "ps two old");
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = kFocus;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> layer_target, layer_staging;
            check(device->CreateTexture2D(&desc, nullptr, &layer_target), "layer target");
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check(device->CreateTexture2D(&desc, nullptr, &layer_staging), "layer staging");
            ComPtr<ID3D11RenderTargetView> layer_rtv;
            check(device->CreateRenderTargetView(layer_target.Get(), nullptr, &layer_rtv), "layer rtv");
            // dcsvr_two_layers_alpha 2: the focus swapchain is R8G8B8A8_UNORM_SRGB; its raw bytes are read back.
            ComPtr<ID3D11Texture2D> srgb_target, srgb_staging;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.CPUAccessFlags = 0;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            check(device->CreateTexture2D(&desc, nullptr, &srgb_target), "srgb target");
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check(device->CreateTexture2D(&desc, nullptr, &srgb_staging), "srgb staging");
            ComPtr<ID3D11RenderTargetView> srgb_rtv;
            check(device->CreateRenderTargetView(srgb_target.Get(), nullptr, &srgb_rtv), "srgb rtv");
            const auto layer_constants = buffer(128);
            struct TwoLayer { float smoothing, shape, layer, encode; float warp[4]; float reuse[12]; float debug[4]; float edge[4]; float band[4]; };
            static_assert(sizeof(TwoLayer) == 128);
            const auto run_layer = [&](const TwoLayer& k, ID3D11PixelShader* shader, bool srgb = false) {
                const float identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                context->UpdateSubresource(vs_constants.Get(), 0, nullptr, identity, 0, 0);
                context->UpdateSubresource(layer_constants.Get(), 0, nullptr, &k, 0, 0);
                const float clear[4]{0.25f, 0.5f, 0.75f, 1.f};
                context->ClearRenderTargetView(srgb ? srgb_rtv.Get() : layer_rtv.Get(), clear);
                context->OMSetRenderTargets(1, srgb ? srgb_rtv.GetAddressOf() : layer_rtv.GetAddressOf(), nullptr);
                D3D11_VIEWPORT viewport{0, 0, static_cast<float>(kFocus), static_cast<float>(kFocus), 0, 1};
                context->RSSetViewports(1, &viewport);
                context->RSSetState(rasterizer.Get());
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                context->VSSetShader(vs.Get(), nullptr, 0);
                context->VSSetConstantBuffers(0, 1, vs_constants.GetAddressOf());
                context->PSSetShader(shader, nullptr, 0);
                context->PSSetConstantBuffers(0, 1, layer_constants.GetAddressOf());
                ID3D11ShaderResourceView* views[3] = {focus_srv.Get(), focus_srv.Get(), field_srv.Get()};
                context->PSSetShaderResources(0, 3, views);
                context->PSSetSamplers(0, 1, sampler.GetAddressOf());
                context->Draw(3, 0);
                ID3D11ShaderResourceView* none[3]{};
                context->PSSetShaderResources(0, 3, none);
                ID3D11Texture2D* const staging = srgb ? srgb_staging.Get() : layer_staging.Get();
                context->CopyResource(staging, srgb ? srgb_target.Get() : layer_target.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                check(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped), "map layer");
                std::vector<std::uint32_t> out(static_cast<std::size_t>(kFocus) * kFocus);
                for (UINT y = 0; y < kFocus; ++y)
                    std::memcpy(&out[static_cast<std::size_t>(y) * kFocus],
                        static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, kFocus * 4);
                context->Unmap(staging, 0);
                return out;
            };
            std::size_t off_cases = 0, off_differing = 0;
            for (const float shape : {0.f, 2.f, 8.f})
                for (const float smoothing : {0.f, 0.05f, 0.2f})
                    for (const float encode : {0.f, 1.f})
                        for (const float fill : {0.f, 1.f})
                            for (const float k : {0.f, 1.f, 3.f}) {
                                TwoLayer c{};
                                c.smoothing = smoothing;
                                c.shape = shape;
                                c.layer = 1.f;
                                c.encode = encode;
                                c.debug[1] = fill;
                                c.edge[0] = k == 3.f ? 0.f : 0.6f;  // off by the share, or by k <= 1
                                c.edge[1] = k;
                                const auto before = run_layer(c, two_old_ps.Get()), after = run_layer(c, two_new_ps.Get());
                                ++off_cases;
                                for (std::size_t p = 0; p < before.size(); ++p) off_differing += before[p] != after[p] ? 1U : 0U;
                            }
            std::printf("two layers, edge match off: %zu cases, %zu differing pixels%s", off_cases, off_differing, "\n");
            two_layer_ok = off_differing == 0;
            for (const float shape : {2.f, 8.f}) {
                TwoLayer on{};
                on.smoothing = 0.2f;
                on.shape = shape;
                on.layer = 1.f;
                on.debug[1] = 1.f;
                on.edge[0] = 0.6f;
                on.edge[1] = 3.f;
                TwoLayer off = on;
                off.edge[0] = 0.f;
                const auto plain = run_layer(off, two_new_ps.Get()), matched = run_layer(on, two_new_ps.Get());
                const float mid = kFocus * 0.5f;
                std::size_t inner = 0, band = 0, outside = 0;
                for (UINT y = 0; y < kFocus; ++y)
                    for (UINT x = 0; x < kFocus; ++x) {
                        const std::size_t p = static_cast<std::size_t>(y) * kFocus + x;
                        if (plain[p] == matched[p]) continue;
                        const float ax = std::fabs(x + 0.5f - mid) / mid, ay = std::fabs(y + 0.5f - mid) / mid;
                        const float d = std::pow(std::pow(ax, shape) + std::pow(ay, shape), 1.f / shape);
                        if (d < 0.6f - 0.01f) ++inner; else if (d > 1.f + 0.01f) ++outside; else ++band;
                    }
                std::printf("two layers, edge match shape=%.0f: %zu changed pixels inside the band, %zu in it, %zu outside%s",
                    shape, inner, band, outside, "\n");
                two_layer_ok = two_layer_ok && inner == 0 && outside == 0 && band > 1000U;
            }
            // Two-layer alpha conventions (0023, dcsvrEdge.z): 1 and 2 give the same image as 0 once a compositor blends
            // them by the rule the runtime probe measured on the Pimax runtime, on the stored (encoded) bytes:
            // 0 unpremultiplied out = c * a + P * (1 - a); 1 and 2 premultiplied out = p + P * (1 - a). The alpha bytes are
            // equal in 0 and 1 (within one code in 2's sRGB target), 2's raw bytes equal 1's within one code; the blends agree
            // within 8-bit quantisation: colour, alpha and the product each rounded (1.5 codes; 2.5 with the sRGB store).
            bool alpha_ok = true;
            if (alpha_ok) {
                double worst_blend = 0.0, worst_blend2 = 0.0;
                int worst_bytes = 0, worst_alpha2 = 0;
                std::size_t alpha_differing = 0, band_pixels = 0, cases = 0;
                for (const float shape : {0.f, 2.f, 8.f})
                    for (const float encode : {0.f, 1.f})
                        for (const float fill : {0.f, 1.f}) {
                            TwoLayer c{};
                            c.smoothing = 0.2f;
                            c.shape = shape;
                            c.layer = 1.f;
                            c.encode = encode;
                            c.debug[1] = fill;
                            TwoLayer m1 = c, m2 = c;
                            m1.edge[2] = 1.f;
                            m2.edge[2] = 2.f;
                            const auto u = run_layer(c, two_new_ps.Get()), p1 = run_layer(m1, two_new_ps.Get()), p2 = run_layer(m2, two_new_ps.Get(), true);
                            ++cases;
                            for (std::size_t px = 0; px < u.size(); ++px) {
                                const int a = static_cast<int>(u[px] >> 24);
                                const int a1 = static_cast<int>(p1[px] >> 24), a2 = static_cast<int>(p2[px] >> 24);
                                if (a1 != a) ++alpha_differing;
                                worst_alpha2 = (std::max)(worst_alpha2, std::abs(a2 - a));
                                if (a > 0 && a < 255) ++band_pixels;
                                for (int ch = 0; ch < 3; ++ch) {
                                    const int cu = static_cast<int>((u[px] >> (8 * ch)) & 255U), c1 = static_cast<int>((p1[px] >> (8 * ch)) & 255U),
                                              c2 = static_cast<int>((p2[px] >> (8 * ch)) & 255U);
                                    worst_bytes = (std::max)(worst_bytes, std::abs(c1 - c2));
                                    for (const int periphery : {0, 64, 128, 255}) {
                                        const double ref = cu * a / 255.0 + periphery * (1.0 - a / 255.0);
                                        const double got1 = c1 + periphery * (1.0 - a / 255.0);
                                        const double got2 = c2 + periphery * (1.0 - a2 / 255.0);
                                        worst_blend = (std::max)(worst_blend, std::abs(got1 - ref));
                                        worst_blend2 = (std::max)(worst_blend2, std::abs(got2 - ref));
                                    }
                                }
                            }
                        }
                std::printf("two-layer alpha: %zu cases, %zu fade pixels, alpha differing %zu (sRGB target worst %d), blend worst %.2f codes (sRGB target %.2f), sRGB vs UNORM bytes worst %d%s",
                    cases, band_pixels, alpha_differing, worst_alpha2, worst_blend, worst_blend2, worst_bytes, "\n");
                alpha_ok = alpha_differing == 0 && band_pixels > 1000U && worst_alpha2 <= 1 && worst_blend <= 1.5 && worst_blend2 <= 2.5 && worst_bytes <= 1;
            }
            std::printf(alpha_ok ? "Quad Views two-layer alpha conventions: premultiplied equals unpremultiplied after the blend%s"
                                 : "Quad Views two-layer alpha conventions FAILED%s", "\n");
            two_layer_ok = two_layer_ok && alpha_ok;

            // Opaque focus (0024, dcsvrEdge.w = 1): the focus pass writes the composed result over the whole focus
            // rectangle. Checks:
            //  1. The pass with w 0 is the pass before 0024 (two_old above has the branch removed): modes 0-2 unchanged.
            //  2. Against the composition (the built ProjectionPS.hlsl, ignoreAlpha): the focus area is 600 of 1024 output
            //     pixels and starts on a whole pixel, so each focus texel is one output pixel, and the periphery texture
            //     is 512 (the composition enlarges it, k = 2 for the edge match). Every focus pixel must equal the
            //     composed pixel at the same place (fade band, sharp area, corners outside the shape), for every shape,
            //     band, offset, with sharpening and with edge match.
            //  3. Where mode 0's fade is 1 and the layer's alpha is 1, the colour is mode 0's exactly (the sharp area).
            //  4. The alpha (0031): the composition's fade at the texel's own place, over the composition's band but never
            //     a narrower one than edge blend 0.2's (the outer 0.4 of the radius; the largest of |x| and |y| for the
            //     rectangle of shape 0), so every texel of the outer border is transparent. Where alpha is 0 or 1 the
            //     colour is the composed one (check 2, so a display path that ignores the alpha shows the right image
            //     there); where it is between 0 and 1 the runtime's blend of the stored bytes over the periphery
            //     (bilinear, as the runtime's periphery layer shows it here) gives the composed colour, wherever the
            //     colour did not have to be clamped (round and rounded areas; upstream's rectangle, shape 0, does not
            //     fade to 0 at its edge, so its ramp trades the composed colour for no line). Where the ramp is the
            //     composition's own band (edge blend 0.2, round or rounded) the alpha is the fade to 8 bits, so every
            //     texel there, clamped or not, must blend to the composed colour within 2 codes.
            //  5. Chromatic aberration (0031): the Pimax runtime samples each colour channel at its own place (red a few
            //     texels towards the lens centre, blue a few outwards) but blends with green's alpha, so a channel gets
            //     the background times a(x_c) - a(x_G) too much or too little (the warm line of the 2026-11-22 flights:
            //     green unchanged, red +35 %, blue -41 % of the background along all four edges with 0025's rim). With
            //     offsets of 2 texels of 600 radially (the flights' 5 of 1928, rounded up) the stored alpha must never
            //     differ by more than 0.04 between a texel and its offset places (10 codes over a white background);
            //     0025's rim (alpha 0 to 1 over 0.75 % of the side) gives about 0.44, a hard edge 1.
            // Before 0025 the layer had alpha 1 up to its edge and went out with no blending: the Pimax runtime drew a
            // line along its bottom edge (rig 2026-11-22). Checks 4 and 5 fail on that pass, and 5 on 0025's.
            bool opaque_ok = true;
            {
                const auto stereo_half = make_texture(kOut / 2, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, true);
                ComPtr<ID3D11ShaderResourceView> stereo_half_srv;
                check(device->CreateShaderResourceView(stereo_half.Get(), nullptr, &stereo_half_srv), "srv half");
                // The periphery's texels on the CPU, for the rim's blend check.
                const UINT half = kOut / 2;
                std::vector<std::uint32_t> periphery_texels(static_cast<std::size_t>(half) * half);
                {
                    D3D11_TEXTURE2D_DESC desc{};
                    stereo_half->GetDesc(&desc);
                    desc.BindFlags = 0;
                    desc.Usage = D3D11_USAGE_STAGING;
                    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    ComPtr<ID3D11Texture2D> read;
                    check(device->CreateTexture2D(&desc, nullptr, &read), "periphery read");
                    context->CopyResource(read.Get(), stereo_half.Get());
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    check(context->Map(read.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map periphery");
                    for (UINT y = 0; y < half; ++y)
                        std::memcpy(&periphery_texels[static_cast<std::size_t>(y) * half],
                            static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, half * 4);
                    context->Unmap(read.Get(), 0);
                }
                // Bilinear, border black (the test's sampler), channel ch, at uv.
                const auto periphery_at = [&](float u, float v, int ch) {
                    const float x = u * half - 0.5f, y = v * half - 0.5f;
                    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
                    const float fx = x - x0, fy = y - y0;
                    const auto texel = [&](int tx, int ty) {
                        if (tx < 0 || ty < 0 || tx >= static_cast<int>(half) || ty >= static_cast<int>(half)) return 0.f;
                        return static_cast<float>((periphery_texels[static_cast<std::size_t>(ty) * half + tx] >> (8 * ch)) & 255U);
                    };
                    return (texel(x0, y0) * (1 - fx) + texel(x0 + 1, y0) * fx) * (1 - fy) +
                           (texel(x0, y0 + 1) * (1 - fx) + texel(x0 + 1, y0 + 1) * fx) * fy;
                };
                // The layer's alpha byte as the shader computes it (0034: the ramp as set, 0.4 by default; 0 none).
                const auto smooth_step = [](float e0, float e1, float v) {
                    const float s = (std::clamp)((v - e0) / (e1 - e0), 0.f, 1.f);
                    return s * s * (3 - 2 * s);
                };
                const auto layer_alpha = [&](UINT x, UINT y, float shape, float smoothing, float ramp = 0.4f) {
                    (void)smoothing;  // 0031 widened the ramp to the composition's band; 0034 takes it as set
                    if (ramp <= 0) return 255;
                    const float ox = std::abs((x + 0.5f) / kFocus * 2 - 1), oy = std::abs((y + 0.5f) / kFocus * 2 - 1);
                    if (ox >= 1 || oy >= 1) return 0;
                    const float r = shape >= 1 ? std::pow(std::pow(ox, shape) + std::pow(oy, shape), 1 / shape) : (std::max)(ox, oy);
                    return static_cast<int>(std::lround((1 - smooth_step(1 - ramp, 1, r)) * 255));
                };
                // 0025's rim, for check 5's reference only.
                const auto rim_alpha_0025 = [&](float u, float v) {
                    const float d = (std::min)((std::min)(u, 1 - u), (std::min)(v, 1 - v)) / 0.015f;
                    return std::lround((std::clamp)(2 * d - 1, 0.f, 1.f) * 255) / 255.f;
                };
                // Check 5: the stored alpha bilinear at texel coordinates (outside the layer 0, as the runtime reads it).
                const auto alpha_at = [&](const std::vector<std::uint32_t>& img, float fx, float fy) {
                    const float x = fx - 0.5f, y = fy - 0.5f;
                    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
                    const float wx = x - x0, wy = y - y0;
                    const auto texel = [&](int tx, int ty) {
                        if (tx < 0 || ty < 0 || tx >= static_cast<int>(kFocus) || ty >= static_cast<int>(kFocus)) return 0.f;
                        return static_cast<float>(img[static_cast<std::size_t>(ty) * kFocus + tx] >> 24) / 255.f;
                    };
                    return (texel(x0, y0) * (1 - wx) + texel(x0 + 1, y0) * wx) * (1 - wy) +
                           (texel(x0, y0 + 1) * (1 - wx) + texel(x0 + 1, y0 + 1) * wx) * wy;
                };
                // The worst |a(x_c) - a(x_G)| over the layer and a margin around it, red and blue kAberration texels
                // inwards and outwards along the ray from the layer's centre.
                const float kAberration = 2.f;
                const auto aberration_error = [&](const auto& alpha_of) {
                    double worst = 0;
                    const int margin = 8;
                    for (int y = -margin; y < static_cast<int>(kFocus) + margin; ++y)
                        for (int x = -margin; x < static_cast<int>(kFocus) + margin; ++x) {
                            const float gx = x + 0.5f, gy = y + 0.5f;
                            const float dx = gx - kFocus * 0.5f, dy = gy - kFocus * 0.5f, len = std::sqrt(dx * dx + dy * dy);
                            if (len < 1) continue;
                            const float ux = dx / len * kAberration, uy = dy / len * kAberration;
                            const float a_green = alpha_of(gx, gy);
                            worst = (std::max)(worst, static_cast<double>(std::abs(alpha_of(gx - ux, gy - uy) - a_green)));
                            worst = (std::max)(worst, static_cast<double>(std::abs(alpha_of(gx + ux, gy + uy) - a_green)));
                        }
                    return worst;
                };
                const float area = static_cast<float>(kFocus) / static_cast<float>(kOut);
                const auto run_opaque = [&](const TwoLayer& k, ID3D11ShaderResourceView* focus_view, ID3D11ShaderResourceView* plain) {
                    const float identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                    context->UpdateSubresource(vs_constants.Get(), 0, nullptr, identity, 0, 0);
                    context->UpdateSubresource(layer_constants.Get(), 0, nullptr, &k, 0, 0);
                    const float clear[4]{0.25f, 0.5f, 0.75f, 1.f};
                    context->ClearRenderTargetView(layer_rtv.Get(), clear);
                    context->OMSetRenderTargets(1, layer_rtv.GetAddressOf(), nullptr);
                    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(kFocus), static_cast<float>(kFocus), 0, 1};
                    context->RSSetViewports(1, &viewport);
                    context->RSSetState(rasterizer.Get());
                    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                    context->VSSetShader(vs.Get(), nullptr, 0);
                    context->VSSetConstantBuffers(0, 1, vs_constants.GetAddressOf());
                    context->PSSetShader(two_new_ps.Get(), nullptr, 0);
                    context->PSSetConstantBuffers(0, 1, layer_constants.GetAddressOf());
                    ID3D11ShaderResourceView* views[4] = {focus_view, plain, field_srv.Get(), stereo_half_srv.Get()};
                    context->PSSetShaderResources(0, 4, views);
                    context->PSSetSamplers(0, 1, sampler.GetAddressOf());
                    context->Draw(3, 0);
                    ID3D11ShaderResourceView* none[4]{};
                    context->PSSetShaderResources(0, 4, none);
                    context->CopyResource(layer_staging.Get(), layer_target.Get());
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    check(context->Map(layer_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map opaque");
                    std::vector<std::uint32_t> out(static_cast<std::size_t>(kFocus) * kFocus);
                    for (UINT y = 0; y < kFocus; ++y)
                        std::memcpy(&out[static_cast<std::size_t>(y) * kFocus],
                            static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, kFocus * 4);
                    context->Unmap(layer_staging.Get(), 0);
                    return out;
                };
                // One periphery texel in focus texels, as layer.cpp computes it: 600 focus texels over 512 periphery
                // texels' 600 / 1024 of the view.
                const float k_cpu = static_cast<float>(kFocus) / static_cast<float>(kOut / 2) / area;
                std::size_t cases = 0, blended = 0, exact_off = 0, total_off = 0, alpha_not_one = 0, sharp = 0, sharp_differing = 0;
                std::size_t rim_alpha_wrong = 0, border_opaque = 0, rim_ramp = 0, rim_clamped = 0, band_ramp = 0, band_clamped = 0;
                double worst_aberration = 0;
                int worst_off = 0, worst_edge = 0, worst_sharp = 0;
                double worst_rim_blend = 0, worst_band_blend = 0;
                focus_area = area;
                stereo_view = stereo_half_srv.Get();
                for (const float shape : {0.f, 2.f, 8.f})
                    for (const float smoothing : {0.f, 0.05f, 0.2f})
                        // Offsets of whole output pixels (NDC 0.125 = 64 px, -0.0625 = 32 px).
                        for (const auto& offset : {std::array<float, 2>{0.f, 0.f}, std::array<float, 2>{0.125f, -0.0625f}})
                            for (const bool sharpen : {false, true})
                                for (const float edge : {0.f, 0.6f}) {
                                    if (edge > 0.f && (shape < 1.f || smoothing <= 0.f)) continue;
                                    const Case c{shape, smoothing, offset[0], offset[1], sharpen, false, 0.f, edge};
                                    const auto reference = run(c, true);
                                    // run() left the sharpened focus in `sharpened` when it sharpened.
                                    ID3D11ShaderResourceView* const focus_view = sharpen ? sharpened_srv.Get() : focus_srv.Get();
                                    const bool fill = sharpen && shape >= 1.f;
                                    TwoLayer k{};
                                    k.smoothing = smoothing;
                                    k.shape = shape;
                                    k.layer = 1.f;
                                    k.debug[1] = fill ? 1.f : 0.f;
                                    k.edge[0] = edge;
                                    k.edge[1] = edge > 0.f ? k_cpu : 0.f;
                                    TwoLayer o = k;
                                    o.edge[3] = 1.4f;  // 0034: 1 + the ramp (dcsvr_two_layers_opaque_ramp's default)
                                    o.band[0] = o.band[1] = area;
                                    o.band[2] = (1.f - area + offset[0]) * 0.5f;
                                    o.band[3] = (1.f - area - offset[1]) * 0.5f;
                                    // Mode 0's pass on the same inputs (the sharpened image at t0 when sharpened, as
                                    // layer.cpp binds it).
                                    const auto standard_sharp = run_opaque(k, focus_view, focus_srv.Get());
                                    const auto opaque = run_opaque(o, focus_view, focus_srv.Get());
                                    ++cases;
                                    worst_aberration = (std::max)(worst_aberration, aberration_error([&](float fx, float fy) { return alpha_at(opaque, fx, fy); }));
                                    const bool own_band = shape >= 1.f && smoothing >= 0.2f;
                                    const int x0 = static_cast<int>(std::lround(o.band[2] * kOut)), y0 = static_cast<int>(std::lround(o.band[3] * kOut));
                                    for (UINT y = 0; y < kFocus; ++y)
                                        for (UINT x = 0; x < kFocus; ++x) {
                                            const std::size_t p = static_cast<std::size_t>(y) * kFocus + x;
                                            const std::uint32_t r = reference[static_cast<std::size_t>(y0 + y) * kOut + x0 + x];
                                            const int a_out = static_cast<int>(opaque[p] >> 24), a_rim = layer_alpha(x, y, shape, smoothing);
                                            if (std::abs(a_out - a_rim) > 1) ++rim_alpha_wrong;
                                            if ((x == 0 || y == 0 || x + 1 == kFocus || y + 1 == kFocus) && a_out != 0) ++border_opaque;
                                            if (a_rim > 0 && a_rim < 255) {
                                                // The runtime's blend of the stored bytes over the periphery must give the
                                                // composed colour (unless the colour was clamped).
                                                ++rim_ramp;
                                                const float a = a_out / 255.f;
                                                const float u = (x + 0.5f) / kFocus * o.band[0] + o.band[2];
                                                const float v = (y + 0.5f) / kFocus * o.band[1] + o.band[3];
                                                bool clamped = false;
                                                double worst_here = 0;
                                                for (int ch = 0; ch < 3; ++ch) {
                                                    const int stored = static_cast<int>((opaque[p] >> (8 * ch)) & 255U);
                                                    clamped = clamped || stored == 0 || stored == 255;
                                                    const double shown = stored * a + periphery_at(u, v, ch) * (1 - a);
                                                    worst_here = (std::max)(worst_here, std::abs(shown - static_cast<double>((r >> (8 * ch)) & 255U)));
                                                }
                                                if (clamped) ++rim_clamped;
                                                // Upstream's rectangle (shape 0) does not fade to 0 at its edge: the ramp
                                                // there trades the composed colour for no line, so it is not compared.
                                                else if (edge <= 0.f && shape >= 1.f) worst_rim_blend = (std::max)(worst_rim_blend, worst_here);
                                                // In the composition's own band the alpha is the fade to 8 bits, so even
                                                // a clamped colour is shown within the rounding: every texel compared.
                                                if (own_band && edge <= 0.f) {
                                                    ++band_ramp;
                                                    band_clamped += clamped ? 1U : 0U;
                                                    worst_band_blend = (std::max)(worst_band_blend, worst_here);
                                                }
                                                continue;
                                            }
                                            int worst = 0;
                                            for (int ch = 0; ch < 3; ++ch)
                                                worst = (std::max)(worst, std::abs(static_cast<int>((opaque[p] >> (8 * ch)) & 255U) -
                                                                                   static_cast<int>((r >> (8 * ch)) & 255U)));
                                            if (edge > 0.f) {
                                                worst_edge = (std::max)(worst_edge, worst);
                                            } else {
                                                worst_off = (std::max)(worst_off, worst);
                                                ++total_off;
                                                exact_off += worst == 0 ? 1U : 0U;
                                            }
                                            if (a_rim == 255 && (opaque[p] >> 24) != 255U) ++alpha_not_one;
                                            const std::uint32_t a0 = standard_sharp[p] >> 24;
                                            if (a0 == 255U && a_rim == 255) {
                                                ++sharp;
                                                // A fade byte of 255 is a fade of 1 within rounding (0.998 or more): the
                                                // periphery may add a code there.
                                                int d = 0;
                                                for (int ch = 0; ch < 3; ++ch)
                                                    d = (std::max)(d, std::abs(static_cast<int>((standard_sharp[p] >> (8 * ch)) & 255U) -
                                                                               static_cast<int>((opaque[p] >> (8 * ch)) & 255U)));
                                                if (d != 0) ++sharp_differing;
                                                worst_sharp = (std::max)(worst_sharp, d);
                                            } else {
                                                ++blended;
                                            }
                                        }
                                }
                // 0034, dcsvr_two_layers_opaque_ramp: a narrower ramp (0.1) and none (0: alpha 1, the layer not blended).
                // Round shape (2), edge blend 0.2 and 0.05, both offsets, sharpened or not, edge match off. Where the
                // stored alpha is 1 the colour must be the composed one (the runtime's periphery is not seen there:
                // with 0.1 that is everything inside r 0.9, the composition's band included); in the ramp the runtime's
                // blend over the periphery must give the composed colour (unclamped texels); with 0 every texel is
                // alpha 1 and the composed colour. The chromatic aberration residual of 0.1 is reported, not limited.
                std::size_t narrow_cases = 0, narrow_alpha_wrong = 0, narrow_border_opaque = 0, narrow_ramp = 0, narrow_opaque = 0;
                std::size_t none_cases = 0, none_alpha_not_one = 0;
                int narrow_worst_off = 0, none_worst = 0;
                double narrow_worst_blend = 0, narrow_aberration = 0;
                for (const float ramp : {0.1f, 0.f})
                    for (const float smoothing : {0.05f, 0.2f})
                        for (const auto& offset : {std::array<float, 2>{0.f, 0.f}, std::array<float, 2>{0.125f, -0.0625f}})
                            for (const bool sharpen : {false, true}) {
                                const float shape = 2.f;
                                const Case c{shape, smoothing, offset[0], offset[1], sharpen, false, 0.f, 0.f};
                                const auto reference = run(c, true);
                                ID3D11ShaderResourceView* const focus_view = sharpen ? sharpened_srv.Get() : focus_srv.Get();
                                TwoLayer o{};
                                o.smoothing = smoothing;
                                o.shape = shape;
                                o.layer = 1.f;
                                o.debug[1] = sharpen ? 1.f : 0.f;
                                o.edge[3] = 1.f + ramp;
                                o.band[0] = o.band[1] = area;
                                o.band[2] = (1.f - area + offset[0]) * 0.5f;
                                o.band[3] = (1.f - area - offset[1]) * 0.5f;
                                const auto opaque = run_opaque(o, focus_view, focus_srv.Get());
                                (ramp > 0 ? narrow_cases : none_cases)++;
                                if (ramp > 0)
                                    narrow_aberration = (std::max)(narrow_aberration, aberration_error([&](float fx, float fy) { return alpha_at(opaque, fx, fy); }));
                                const int x0 = static_cast<int>(std::lround(o.band[2] * kOut)), y0 = static_cast<int>(std::lround(o.band[3] * kOut));
                                for (UINT y = 0; y < kFocus; ++y)
                                    for (UINT x = 0; x < kFocus; ++x) {
                                        const std::size_t p = static_cast<std::size_t>(y) * kFocus + x;
                                        const std::uint32_t r = reference[static_cast<std::size_t>(y0 + y) * kOut + x0 + x];
                                        const int a_out = static_cast<int>(opaque[p] >> 24), a_set = layer_alpha(x, y, shape, smoothing, ramp);
                                        int worst = 0;
                                        for (int ch = 0; ch < 3; ++ch)
                                            worst = (std::max)(worst, std::abs(static_cast<int>((opaque[p] >> (8 * ch)) & 255U) -
                                                                               static_cast<int>((r >> (8 * ch)) & 255U)));
                                        if (ramp <= 0) {
                                            if (a_out != 255) ++none_alpha_not_one;
                                            none_worst = (std::max)(none_worst, worst);
                                            continue;
                                        }
                                        if (std::abs(a_out - a_set) > 1) ++narrow_alpha_wrong;
                                        if ((x == 0 || y == 0 || x + 1 == kFocus || y + 1 == kFocus) && a_out != 0) ++narrow_border_opaque;
                                        if (a_set > 0 && a_set < 255) {
                                            ++narrow_ramp;
                                            const float a = a_out / 255.f;
                                            const float u = (x + 0.5f) / kFocus * o.band[0] + o.band[2];
                                            const float v = (y + 0.5f) / kFocus * o.band[1] + o.band[3];
                                            bool clamped = false;
                                            double worst_here = 0;
                                            for (int ch = 0; ch < 3; ++ch) {
                                                const int stored = static_cast<int>((opaque[p] >> (8 * ch)) & 255U);
                                                clamped = clamped || stored == 0 || stored == 255;
                                                const double shown = stored * a + periphery_at(u, v, ch) * (1 - a);
                                                worst_here = (std::max)(worst_here, std::abs(shown - static_cast<double>((r >> (8 * ch)) & 255U)));
                                            }
                                            if (!clamped) narrow_worst_blend = (std::max)(narrow_worst_blend, worst_here);
                                            continue;
                                        }
                                        if (a_set == 255) ++narrow_opaque;
                                        narrow_worst_off = (std::max)(narrow_worst_off, worst);
                                    }
                            }
                std::printf("opaque focus ramp 0.1: %zu cases, alpha off the ramp's %zu, opaque border texels %zu, %zu opaque texels (composed worst %d codes), %zu ramp texels (runtime blend vs composed worst %.2f codes), chromatic aberration worst alpha mismatch %.3f; ramp 0: %zu cases, alpha not 1 %zu, composed worst %d codes%s",
                    narrow_cases, narrow_alpha_wrong, narrow_border_opaque, narrow_opaque, narrow_worst_off, narrow_ramp, narrow_worst_blend, narrow_aberration,
                    none_cases, none_alpha_not_one, none_worst, "\n");
                const bool narrow_ok = narrow_cases == 8 && none_cases == 8 && narrow_alpha_wrong == 0 && narrow_border_opaque == 0 &&
                                       narrow_opaque > 1000000U && narrow_worst_off <= 1 && narrow_ramp > 10000U && narrow_worst_blend <= 2.0 &&
                                       none_alpha_not_one == 0 && none_worst <= 1;
                focus_area = 0.34f;
                stereo_view = nullptr;
                std::printf("opaque focus vs composition: %zu cases, %zu sharp pixels (%zu differing from mode 0, worst %d codes), %zu blended or periphery pixels, edge match off worst %d codes (%zu of %zu exact), on worst %d codes, alpha not 1 %zu%s",
                    cases, sharp, sharp_differing, worst_sharp, blended, worst_off, exact_off, total_off, worst_edge, alpha_not_one, "\n");
                // Check 5's reference: 0025's rim (and a hard edge) on the same metric.
                const double rim_aberration = aberration_error([&](float fx, float fy) {
                    return fx < 0 || fy < 0 || fx >= kFocus || fy >= kFocus ? 0.f : rim_alpha_0025(fx / kFocus, fy / kFocus);
                });
                const double hard_aberration = aberration_error([&](float fx, float fy) {
                    return fx < 0 || fy < 0 || fx >= kFocus || fy >= kFocus ? 0.f : 1.f;
                });
                std::printf("opaque focus ramp: alpha off the ramp's %zu, opaque border texels %zu, ramp texels %zu (%zu clamped; in the composition's own band %zu, %zu clamped), runtime blend vs composed worst %.2f codes (own band, clamped too: %.2f)%s",
                    rim_alpha_wrong, border_opaque, rim_ramp, rim_clamped, band_ramp, band_clamped, worst_rim_blend, worst_band_blend, "\n");
                std::printf("opaque focus chromatic aberration (red and blue %.0f texels off green, blend with green's alpha): worst alpha mismatch %.3f (%.1f codes on white); 0025's rim %.3f, a hard edge %.3f%s",
                    kAberration, worst_aberration, worst_aberration * 255, rim_aberration, hard_aberration, "\n");
                opaque_ok = worst_off <= 1 && worst_edge <= 2 && alpha_not_one == 0 && worst_sharp <= 1 && sharp > 1000000U && blended > 1000000U &&
                            rim_alpha_wrong == 0 && border_opaque == 0 && rim_ramp > 10000U && worst_rim_blend <= 2.0 && band_ramp > 10000U &&
                            worst_band_blend <= 2.0 && worst_aberration <= 0.04 && rim_aberration > 0.3 && hard_aberration > 0.9 &&
                            narrow_ok;
            }
            std::printf(opaque_ok ? "Quad Views opaque focus: the focus layer holds the composed image over its rectangle%s"
                                  : "Quad Views opaque focus FAILED%s", "\n");            two_layer_ok = two_layer_ok && opaque_ok;
        }
        std::printf(two_layer_ok ? "Quad Views two-layer edge match: off identical, on only the fade band%s"
                                 : "Quad Views two-layer edge match FAILED%s", "\n");
        return all_identical && taper_ok && edge_ok && far_depth_off && two_layer_ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::printf("error: %s\n", error.what());
        return 2;
    }
}
