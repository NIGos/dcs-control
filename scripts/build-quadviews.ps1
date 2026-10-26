#Requires -Version 7
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$quadRoot = Join-Path $workspaceRoot 'external/quadviews'
if (-not (Test-Path -LiteralPath $quadRoot)) {
    & git clone --depth 1 --branch 1.1.3 https://github.com/mbucchia/Quad-Views-Foveated.git $quadRoot
    if ($LASTEXITCODE -ne 0) { throw 'Quad Views download failed.' }
}
if ((& git -C $quadRoot rev-parse HEAD) -ne '957ff0327185ea29acc41fc86c4cdc3caf428fb9') { throw 'Unexpected Quad Views revision.' }
& git -C $quadRoot submodule update --init --depth 1 external/OpenXR-SDK external/OpenXR-SDK-Source external/OpenXR-MixedReality external/FidelityFX-CAS external/fmt
if ($LASTEXITCODE -ne 0) { throw 'Quad Views dependencies failed.' }
# Every patch below is skipped when its marker is already present, so a checkout patched by an older version of this
# script would keep stale injected code. Start each build from the pinned upstream sources.
& git -C $quadRoot checkout -- openxr-api-layer
if ($LASTEXITCODE -ne 0) { throw 'Quad Views source reset failed.' }
& git -C $quadRoot clean -fdq openxr-api-layer
if ($LASTEXITCODE -ne 0) { throw 'Quad Views source reset failed.' }
$wilVersion = '1.0.220201.1'
$wilRoot = Join-Path $quadRoot "packages/Microsoft.Windows.ImplementationLibrary.$wilVersion"
if (-not (Test-Path -LiteralPath $wilRoot)) {
    $wilArchive = Join-Path $workspaceRoot '.cache/wil.nupkg'
    New-Item -ItemType Directory -Path (Split-Path -Parent $wilArchive) -Force | Out-Null
    $wilUrl = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.implementationlibrary/$wilVersion/microsoft.windows.implementationlibrary.$wilVersion.nupkg"
    if (-not (Test-Path -LiteralPath $wilArchive)) { Invoke-WebRequest -Uri $wilUrl -OutFile $wilArchive }
    # Pinned like every other build input; a different download is never extracted.
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $wilArchive).Hash -ne '924F1553BF191578261312BEDFA9E9A441AD9FCA59AAEAE4F0038AA3533CCD34') {
        Remove-Item -LiteralPath $wilArchive -Force
        throw 'WIL package digest mismatch.'
    }
    # Extract beside the target and move only a complete extraction, so an interrupted run is never trusted.
    $wilPartial = $wilRoot + '.partial'
    if (Test-Path -LiteralPath $wilPartial) { Remove-Item -LiteralPath $wilPartial -Recurse -Force }
    [IO.Compression.ZipFile]::ExtractToDirectory($wilArchive, $wilPartial)
    Move-Item -LiteralPath $wilPartial -Destination $wilRoot
}
$solutionDir = $quadRoot + '\'
# Per-process settings preserve global configuration for other applications.
$layerSource = Join-Path $quadRoot 'openxr-api-layer/layer.cpp'
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('DCSVR_QUAD_SETTINGS')) {
    $original = 'LoadConfiguration(localAppData / "settings.cfg");'
    $replacement = @'
const DWORD profileLength = GetEnvironmentVariableW(L"DCSVR_QUAD_SETTINGS", nullptr, 0);
                    if (profileLength > 0 && profileLength < 32768) {
                        std::vector<wchar_t> profilePath(profileLength);
                        if (GetEnvironmentVariableW(L"DCSVR_QUAD_SETTINGS", profilePath.data(), profileLength)) {
                            LoadConfiguration(std::filesystem::path(profilePath.data()));
                        }
                    } else {
                        LoadConfiguration(localAppData / "settings.cfg");
                    }
'@
    if (-not $layerText.Contains($original)) { throw 'Quad Views configuration source changed.' }
    [IO.File]::WriteAllText($layerSource, $layerText.Replace($original, $replacement))
}
# stereo_output_multiplier: resolution of the composited stereo image, as a multiple of the runtime's recommendation.
# Upstream composites at focus_multiplier, so focus 1.25 makes a 1.25x stereo image that the runtime then scales down;
# with framegen every frame-generation copy of that oversized image costs VRAM (about 10.7 GB at Crystal Super
# resolution). Unset keeps upstream behaviour.
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('Composited stereo resolution')) {
    $edits = @(
        @('        float m_focusPixelDensity{1.f};', "        float m_focusPixelDensity{1.f};`n        float m_stereoOutputMultiplier{0.f};"),
        @('                    } else if (name == "focus_multiplier") {', "                    } else if (name == `"stereo_output_multiplier`") {`n                        m_stereoOutputMultiplier = std::max(0.1f, std::stof(value));`n                        parsed = true;`n                    } else if (name == `"focus_multiplier`") {"),
        @('                    m_focusPixelDensity * stereoViews[xr::StereoView::Left].recommendedImageRectWidth;', '                    (m_stereoOutputMultiplier > 0.f ? m_stereoOutputMultiplier : m_focusPixelDensity) * stereoViews[xr::StereoView::Left].recommendedImageRectWidth;'),
        @('            m_needComputeBaseFov = false;', "            Log(fmt::format(`"Composited stereo resolution: {}x{}\n`", m_fullFovResolution.width, m_fullFovResolution.height));`n            m_needComputeBaseFov = false;")
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $($edit[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# Direct source views: upstream copies the stereo and focus images into flat textures every frame (its own TODO)
# and samples the copies for composition and sharpening. When the view's rectangle is the whole of a single-slice,
# single-sample, shader-readable image, a view of that image in the same format reads the very texels the copy
# would hold, so the copies and the flat textures are skipped. Anything else (sub-rectangles, texture arrays, MSAA,
# a format the image cannot be viewed in) keeps the copy. Pixel-identical by construction.
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('DCSVR direct source view')) {
    # Multi-line anchors follow the checkout's line endings.
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @(
            (('            // Copy to a flat texture for sampling.', '            {') -join $nl),
            (('            // DCSVR direct source view: what composition and sharpening sample, per source.',
             '            ComPtr<ID3D11ShaderResourceView> stereoSourceSrv;',
             '            ComPtr<ID3D11ShaderResourceView> focusSourceSrv;',
             '            // Copy to a flat texture for sampling.', '            {') -join $nl)
        ),
        @(
            (('                                                    uint32_t startSlot) {',
             '                    D3D11_TEXTURE2D_DESC desc{};') -join $nl),
            (('                                                    uint32_t startSlot) -> ComPtr<ID3D11ShaderResourceView> {',
             '                    ComPtr<ID3D11ShaderResourceView> sourceSrv;',
             '                    {',
             '                        D3D11_TEXTURE2D_DESC sourceDesc{};',
             '                        image->GetDesc(&sourceDesc);',
             '                        if (sourceDesc.ArraySize == 1 && view.subImage.imageArrayIndex == 0 &&',
             '                            sourceDesc.SampleDesc.Count == 1 && (sourceDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&',
             '                            view.subImage.imageRect.offset.x == 0 && view.subImage.imageRect.offset.y == 0 &&',
             '                            view.subImage.imageRect.extent.width == (int32_t)sourceDesc.Width &&',
             '                            view.subImage.imageRect.extent.height == (int32_t)sourceDesc.Height) {',
             '                            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};',
             '                            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
             '                            srvDesc.Format = (DXGI_FORMAT)swapchain.createInfo.format;',
             '                            srvDesc.Texture2D.MipLevels = 1;',
             '                            if (SUCCEEDED(m_applicationDevice->CreateShaderResourceView(',
             '                                    image, &srvDesc, sourceSrv.ReleaseAndGetAddressOf()))) {',
             '                                swapchain.flatImage[startSlot + viewIndex].Reset();',
             '                                return sourceSrv;',
             '                            }',
             '                        }',
             '                    }',
             '                    D3D11_TEXTURE2D_DESC desc{};') -join $nl)
        ),
        @(
            (('                                                           &box);', '                };') -join $nl),
            (('                                                           &box);',
             '                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};',
             '                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
             '                    srvDesc.Format = (DXGI_FORMAT)swapchain.createInfo.format;',
             '                    srvDesc.Texture2D.MipLevels = 1;',
             '                    CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(',
             '                        swapchain.flatImage[startSlot + viewIndex].Get(), &srvDesc, sourceSrv.ReleaseAndGetAddressOf()));',
             '                    return sourceSrv;',
             '                };') -join $nl)
        ),
        @(
            (('                flattenSourceImage(sourceImage, stereoView, swapchainForStereoView, 0);',
             '                flattenSourceImage(sourceFocusImage, focusView, swapchainForFocusView, xr::StereoView::Count);') -join $nl),
            (('                stereoSourceSrv = flattenSourceImage(sourceImage, stereoView, swapchainForStereoView, 0);',
             '                focusSourceSrv =',
             '                    flattenSourceImage(sourceFocusImage, focusView, swapchainForFocusView, xr::StereoView::Count);') -join $nl)
        ),
        @(
            (('                ComPtr<ID3D11ShaderResourceView> srv;',
             '                {',
             '                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};',
             '                    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
             '                    desc.Format = (DXGI_FORMAT)swapchainForFocusView.createInfo.format;',
             '                    desc.Texture2D.MipLevels = 1;',
             '                    CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(',
             '                        swapchainForFocusView.flatImage[xr::StereoView::Count + viewIndex].Get(),',
             '                        &desc,',
             '                        srv.ReleaseAndGetAddressOf()));',
             '                }') -join $nl),
            '                ComPtr<ID3D11ShaderResourceView> srv = focusSourceSrv;'
        ),
        @(
            (('                ComPtr<ID3D11ShaderResourceView> srvForStereoView;',
             '                {',
             '                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};',
             '                    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
             '                    desc.Format = (DXGI_FORMAT)swapchainForStereoView.createInfo.format;',
             '                    desc.Texture2D.MipLevels = 1;',
             '                    CHECK_HRCMD(',
             '                        m_applicationDevice->CreateShaderResourceView(swapchainForStereoView.flatImage[viewIndex].Get(),',
             '                                                                      &desc,',
             '                                                                      srvForStereoView.ReleaseAndGetAddressOf()));',
             '                }') -join $nl),
            '                ComPtr<ID3D11ShaderResourceView> srvForStereoView = stereoSourceSrv;'
        ),
        @(
            (('                ComPtr<ID3D11ShaderResourceView> srvForFocusView;',
             '                {',
             '                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};',
             '                    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
             '                    desc.Format = m_sharpenFocusView ? DXGI_FORMAT_R16G16B16A16_FLOAT',
             '                                                     : (DXGI_FORMAT)swapchainForFocusView.createInfo.format;',
             '                    desc.Texture2D.MipLevels = 1;',
             '                    CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(',
             '                        m_sharpenFocusView ? swapchainForFocusView.sharpenedImage[viewIndex].Get()',
             '                                           : swapchainForFocusView.flatImage[xr::StereoView::Count + viewIndex].Get(),',
             '                        &desc,',
             '                        srvForFocusView.ReleaseAndGetAddressOf()));',
             '                }') -join $nl),
            (('                ComPtr<ID3D11ShaderResourceView> srvForFocusView = focusSourceSrv;',
             '                if (m_sharpenFocusView) {',
             '                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};',
             '                    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
             '                    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;',
             '                    desc.Texture2D.MipLevels = 1;',
             '                    CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(',
             '                        swapchainForFocusView.sharpenedImage[viewIndex].Get(),',
             '                        &desc,',
             '                        srvForFocusView.ReleaseAndGetAddressOf()));',
             '                }') -join $nl)
        )
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# focus_view_shape: upstream blends the focus view in over a rectangular band whose alpha never drops below 0.5
# (max(0.5, s.x * s.y)), so the focus area always ends in a 50 % step that stays visible against the much softer
# periphery. focus_view_shape=2 makes the focus area the ellipse inscribed in the focus view (a circle for a square
# focus view) and fades it out radially with smoothstep over the same width smoothen_focus_view_edges gives the
# rectangle (2 x its value in normalized view coordinates), down to exactly 0 at the edge. Other values (or none)
# keep upstream behaviour. Larger exponents round a square instead (4: a rounded square; DCS VR Control writes 8 when
# the round focus area is off: a rectangle with slightly rounded corners that fades out to 0 like the circle).
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('m_focusViewShape')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @(
            (('        alignas(4) bool debugFocusView;', '    };') -join $nl),
            (('        alignas(4) bool debugFocusView;',
             '        alignas(4) float focusViewShape;',
             '        alignas(4) float padding[3];',
             '    };') -join $nl)
        ),
        @('        float m_smoothenFocusViewEdges{0.2f};', "        float m_smoothenFocusViewEdges{0.2f};$($nl)        float m_focusViewShape{0.f};"),
        @('                    } else if (name == "smoothen_focus_view_edges") {',
          (('                    } else if (name == "focus_view_shape") {',
            '                        m_focusViewShape = std::clamp(std::stof(value), 0.f, 16.f);',
            '                        parsed = true;',
            '                    } else if (name == "smoothen_focus_view_edges") {') -join $nl)),
        @('                drawing.smoothingArea = m_smoothenFocusViewEdges;',
          "                drawing.smoothingArea = m_smoothenFocusViewEdges;$($nl)                drawing.focusViewShape = m_focusViewShape;"),
        @('                            Log(fmt::format("Edge smoothing: {:.2f}\n", m_smoothenFocusViewEdges));',
          (('                            Log(fmt::format("Edge smoothing: {:.2f}\n", m_smoothenFocusViewEdges));',
            '                            if (m_focusViewShape >= 1.f) {',
            '                                Log(fmt::format("Focus view shape: rounded, exponent {:.1f}\n", m_focusViewShape));',
            '                            }') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
$pixelShaderSource = Join-Path $quadRoot 'openxr-api-layer/ProjectionPS.hlsl'
$pixelShaderText = [IO.File]::ReadAllText($pixelShaderSource)
if (-not $pixelShaderText.Contains('focusViewShape')) {
    $nl = if ($pixelShaderText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @('    bool debugFocusView;', "    bool debugFocusView;$($nl)    float focusViewShape;"),
        @('    if (smoothingArea) {',
          (('    if (focusViewShape >= 1) {',
            '        // DCS VR Control: a rounded focus area fading out radially to exactly 0 at its edge.',
            '        float2 d = abs(layer1ProjectedCoordNdc);',
            '        float r = pow(pow(d.x, focusViewShape) + pow(d.y, focusViewShape), 1 / focusViewShape);',
            '        float width = clamp(2 * smoothingArea, 0.01, 0.95);',
            '        color1.a = isInside * (1 - smoothstep(1 - width, 1, r));',
            '    } else if (smoothingArea) {') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($pixelShaderText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views shader changed: $(($edit[0] -split "`n")[0])" }
        $pixelShaderText = $pixelShaderText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($pixelShaderSource, $pixelShaderText)
}
# Early out: about nine pixels in ten of the composited image lie outside the focus area (a 0.34 x 0.34 focus section,
# smaller still inside the round shape). There the focus view's alpha is exactly 0 - outside the rectangle by
# isInside, outside the circle because smoothstep(1 - width, 1, r) is exactly 1 for r >= 1 - so the composition is
# the stereo colour through the same alpha handling. Those pixels now skip the focus sample and the mask maths. The
# debug view of the focus layer keeps the full path. The focus sample is taken at level 0 (the view has one mip
# level; the sampler is the same), as gradients are not available in the branch.
$pixelShaderText = [IO.File]::ReadAllText($pixelShaderSource)
if (-not $pixelShaderText.Contains('DCSVR early out')) {
    $nl = if ($pixelShaderText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        ,@('    float4 color1 = sourceFocusTexture.Sample(sourceSampler, layer1TexCoord);',
          (('    // DCSVR early out: no focus contribution here, the stereo colour as the full path would leave it.',
            '    bool focusVisible = all(abs(layer1ProjectedCoordNdc) < 1);',
            '    if (focusVisible && focusViewShape >= 1) {',
            '        float2 e = abs(layer1ProjectedCoordNdc);',
            '        focusVisible = pow(pow(e.x, focusViewShape) + pow(e.y, focusViewShape), 1 / focusViewShape) < 1;',
            '    }',
            '    [branch] if (!focusVisible && !debugFocusView) {',
            '        if (ignoreAlpha) {',
            '            color0.a = 1;',
            '        }',
            '        if (!isUnpremultipliedAlpha) {',
            '            color0 = unpremultiplyAlpha(color0);',
            '        }',
            '        color0 = premultiplyAlpha(color0);',
            '        return float4(color0.rgb, color0.a);',
            '    }',
            '    float4 color1 = sourceFocusTexture.SampleLevel(sourceSampler, layer1TexCoord, 0);') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($pixelShaderText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views shader changed: $(($edit[0] -split "`n")[0])" }
        $pixelShaderText = $pixelShaderText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($pixelShaderSource, $pixelShaderText)
}
# Cached views: upstream creates the composition's views every frame on the game's render thread (the direct source
# views, the sharpening UAV and SRV, the composition RTV: five per eye). A view of the same resource with the same
# description is the same view, so each is created once and kept with the swapchain whose images it views (they live
# exactly as long as it does) or with the sharpening texture it views (dropped when that is recreated).
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('dcsvrSourceSrv')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @('            ComPtr<ID3D11Texture2D> sharpenedImage[xr::StereoView::Count];',
          (('            ComPtr<ID3D11Texture2D> sharpenedImage[xr::StereoView::Count];',
            '            // DCSVR cached views of this swapchain''s images and of the sharpened textures.',
            '            std::map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> dcsvrSourceSrv;',
            '            std::map<ID3D11Texture2D*, ComPtr<ID3D11RenderTargetView>> dcsvrRtv;',
            '            ComPtr<ID3D11UnorderedAccessView> dcsvrSharpenedUav[xr::StereoView::Count];',
            '            ComPtr<ID3D11ShaderResourceView> dcsvrSharpenedSrv[xr::StereoView::Count];') -join $nl)),
        @((('                            if (SUCCEEDED(m_applicationDevice->CreateShaderResourceView(',
            '                                    image, &srvDesc, sourceSrv.ReleaseAndGetAddressOf()))) {',
            '                                swapchain.flatImage[startSlot + viewIndex].Reset();',
            '                                return sourceSrv;',
            '                            }') -join $nl),
          (('                            auto& cachedSrv = swapchain.dcsvrSourceSrv[image];',
            '                            if (cachedSrv || SUCCEEDED(m_applicationDevice->CreateShaderResourceView(',
            '                                    image, &srvDesc, cachedSrv.ReleaseAndGetAddressOf()))) {',
            '                                swapchain.flatImage[startSlot + viewIndex].Reset();',
            '                                return cachedSrv;',
            '                            }') -join $nl)),
        @((('                        CHECK_HRCMD(m_applicationDevice->CreateTexture2D(',
            '                            &desc, nullptr, swapchainForFocusView.sharpenedImage[viewIndex].ReleaseAndGetAddressOf()));') -join $nl),
          (('                        CHECK_HRCMD(m_applicationDevice->CreateTexture2D(',
            '                            &desc, nullptr, swapchainForFocusView.sharpenedImage[viewIndex].ReleaseAndGetAddressOf()));',
            '                        swapchainForFocusView.dcsvrSharpenedUav[viewIndex].Reset();',
            '                        swapchainForFocusView.dcsvrSharpenedSrv[viewIndex].Reset();') -join $nl)),
        @((('                ComPtr<ID3D11UnorderedAccessView> uav;',
            '                {',
            '                    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};',
            '                    desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;',
            '                    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;',
            '                    CHECK_HRCMD(m_applicationDevice->CreateUnorderedAccessView(',
            '                        swapchainForFocusView.sharpenedImage[viewIndex].Get(), &desc, uav.ReleaseAndGetAddressOf()));',
            '                }') -join $nl),
          (('                ComPtr<ID3D11UnorderedAccessView>& uav = swapchainForFocusView.dcsvrSharpenedUav[viewIndex];',
            '                if (!uav) {',
            '                    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};',
            '                    desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;',
            '                    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;',
            '                    CHECK_HRCMD(m_applicationDevice->CreateUnorderedAccessView(',
            '                        swapchainForFocusView.sharpenedImage[viewIndex].Get(), &desc, uav.ReleaseAndGetAddressOf()));',
            '                }') -join $nl)),
        @((('                if (m_sharpenFocusView) {',
            '                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};',
            '                    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
            '                    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;',
            '                    desc.Texture2D.MipLevels = 1;',
            '                    CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(',
            '                        swapchainForFocusView.sharpenedImage[viewIndex].Get(),',
            '                        &desc,',
            '                        srvForFocusView.ReleaseAndGetAddressOf()));',
            '                }') -join $nl),
          (('                if (m_sharpenFocusView) {',
            '                    ComPtr<ID3D11ShaderResourceView>& sharpenedSrv = swapchainForFocusView.dcsvrSharpenedSrv[viewIndex];',
            '                    if (!sharpenedSrv) {',
            '                        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};',
            '                        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
            '                        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;',
            '                        desc.Texture2D.MipLevels = 1;',
            '                        CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(',
            '                            swapchainForFocusView.sharpenedImage[viewIndex].Get(),',
            '                            &desc,',
            '                            sharpenedSrv.ReleaseAndGetAddressOf()));',
            '                    }',
            '                    srvForFocusView = sharpenedSrv;',
            '                }') -join $nl)),
        @((('                ComPtr<ID3D11RenderTargetView> rtv;',
            '                {',
            '                    D3D11_RENDER_TARGET_VIEW_DESC desc{};',
            '                    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;',
            '                    desc.Format = (DXGI_FORMAT)swapchainForStereoView.createInfo.format;',
            '                    CHECK_HRCMD(m_applicationDevice->CreateRenderTargetView(',
            '                        destinationImage, &desc, rtv.ReleaseAndGetAddressOf()));',
            '                }') -join $nl),
          (('                ComPtr<ID3D11RenderTargetView>& rtv = swapchainForStereoView.dcsvrRtv[destinationImage];',
            '                if (!rtv) {',
            '                    D3D11_RENDER_TARGET_VIEW_DESC desc{};',
            '                    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;',
            '                    desc.Format = (DXGI_FORMAT)swapchainForStereoView.createInfo.format;',
            '                    CHECK_HRCMD(m_applicationDevice->CreateRenderTargetView(',
            '                        destinationImage, &desc, rtv.ReleaseAndGetAddressOf()));',
            '                }') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# Sharpening only what is shown: with the round focus area the composition never reads the focus view outside the
# circle (alpha exactly 0, early out above), so the sharpening pass skips its 16x16 tiles that lie wholly outside
# the circle widened by two texels (the composition's bilinear reach). About a fifth of the focus view. The sharpened
# texture is cleared once when it is created, so a skipped texel holds zero. Off for the rectangle and while the
# focus layer debug view (which shows the whole focus view) is on.
$sharpenSource = Join-Path $quadRoot 'openxr-api-layer/SharpeningCS.hlsl'
$sharpenText = [IO.File]::ReadAllText($sharpenSource)
if (-not $sharpenText.Contains('dcsvrSkip')) {
    $nl = if ($sharpenText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @((('    uint4 const0;', '    uint4 const1;', '};') -join $nl),
          (('    uint4 const0;', '    uint4 const1;',
            '    // DCSVR: x the round shape exponent (0: off), y the margin in texels, zw the focus view size.',
            '    float4 dcsvrSkip;', '};') -join $nl)),
        @('    AU2 gxy = ARmp8x8(LocalThreadId.x) + AU2(WorkGroupId.x << 4u, WorkGroupId.y << 4u);',
          (('    AU2 gxy = ARmp8x8(LocalThreadId.x) + AU2(WorkGroupId.x << 4u, WorkGroupId.y << 4u);',
            '    if (dcsvrSkip.x >= 1) {',
            '        // The tile''s texel nearest the centre, in the composition''s normalized coordinates.',
            '        float2 size = dcsvrSkip.zw;',
            '        float2 tileMin = float2(WorkGroupId.xy) * 16.0;',
            '        float2 nearest = clamp(size * 0.5, tileMin, tileMin + 16.0);',
            '        float2 e = abs(nearest / size * 2.0 - 1.0);',
            '        float r = pow(pow(e.x, dcsvrSkip.x) + pow(e.y, dcsvrSkip.x), 1.0 / dcsvrSkip.x);',
            '        if (r >= 1.0 + 2.0 * dcsvrSkip.y / min(size.x, size.y)) {',
            '            return;',
            '        }',
            '    }') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($sharpenText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views sharpening shader changed: $(($edit[0] -split "`n")[0])" }
        $sharpenText = $sharpenText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($sharpenSource, $sharpenText)
}
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('DcsvrSkip')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @((('        alignas(4) uint32_t Const1[4];', '    };') -join $nl),
          (('        alignas(4) uint32_t Const1[4];', '        alignas(4) float DcsvrSkip[4];', '    };') -join $nl)),
        @((('                    CHECK_HRCMD(m_applicationDevice->CreateUnorderedAccessView(',
            '                        swapchainForFocusView.sharpenedImage[viewIndex].Get(), &desc, uav.ReleaseAndGetAddressOf()));',
            '                }') -join $nl),
          (('                    CHECK_HRCMD(m_applicationDevice->CreateUnorderedAccessView(',
            '                        swapchainForFocusView.sharpenedImage[viewIndex].Get(), &desc, uav.ReleaseAndGetAddressOf()));',
            '                    // A new sharpened texture: zero where the sharpening may skip.',
            '                    const float zero[4] = {0.f, 0.f, 0.f, 0.f};',
            '                    m_renderContext->ClearUnorderedAccessViewFloat(uav.Get(), zero);',
            '                }') -join $nl)),
        @((('                         (AF1)focusView.subImage.imageRect.extent.width,',
            '                         (AF1)focusView.subImage.imageRect.extent.height);',
            '                {') -join $nl),
          (('                         (AF1)focusView.subImage.imageRect.extent.width,',
            '                         (AF1)focusView.subImage.imageRect.extent.height);',
            '                sharpening.DcsvrSkip[0] = m_debugFocusView ? 0.f : m_focusViewShape;',
            '                sharpening.DcsvrSkip[1] = 2.f;',
            '                sharpening.DcsvrSkip[2] = (float)focusView.subImage.imageRect.extent.width;',
            '                sharpening.DcsvrSkip[3] = (float)focusView.subImage.imageRect.extent.height;',
            '                {') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# A gaze sample that is not finite (seen once on a Pimax Crystal Super as DCS's session became ready, the eye tracker
# still starting) gave the focus view a NaN projection; DecomposeProjectionMatrix then threw "Invalid projection
# matrix" and xrLocateViews failed, which DCS treats as fatal (black window). Such a frame now uses the fixed focus
# area, exactly as when the gaze is reported invalid.
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('DCSVR invalid gaze')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @((('                                        if (!isGazeValid ||',
            '                                            !ProjectPoint(viewForGazeProjection, gazeUnitVector, projectedGaze)) {') -join $nl),
          (('                                        // DCSVR invalid gaze: a gaze that is not finite counts as no gaze.',
            '                                        if (!isGazeValid ||',
            '                                            !ProjectPoint(viewForGazeProjection, gazeUnitVector, projectedGaze) ||',
            '                                            !std::isfinite(projectedGaze.x) || !std::isfinite(projectedGaze.y)) {') -join $nl)),
        @((('                                            views[i].fov =',
            '                                                xr::math::ComputeBoundingFov(m_cachedEyeFov[stereoViewIndex], min, max);') -join $nl),
          (('                                            try {',
            '                                                views[i].fov =',
            '                                                    xr::math::ComputeBoundingFov(m_cachedEyeFov[stereoViewIndex], min, max);',
            '                                            } catch (const std::exception&) {',
            '                                                // DCSVR invalid gaze: a degenerate focus area falls back to the fixed one.',
            '                                                views[i].fov = m_cachedEyeFov[i];',
            '                                            }') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# Eye-tracked focus with frame generation (native/quadviews/dcsvr_gaze.h, which explains it; tested by
# tests/quadviews-gaze): during a saccade the focus area reaches ahead along the eye's motion, at fixation a deadzone
# keeps it still. Both free on the GPU. A line every 30 s in the Quad Views log counts saccades, widened and held views.
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'native/quadviews/dcsvr_gaze.h') -Destination (Join-Path $quadRoot 'openxr-api-layer/dcsvr_gaze.h') -Force
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('DCSVR saccade')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $fix = { param($text) ($text -replace "`r`n", "`n").TrimEnd("`n") -replace "`n", $nl }
    $options = @'
                    } else if (name == "dcsvr_saccade_widening") {
                        m_dcsvrGaze.widening = std::stoi(value);
                        parsed = true;
                    } else if (name == "dcsvr_saccade_speed") {
                        m_dcsvrGaze.speed = std::clamp(std::stof(value), 30.f, 1000.f);
                        parsed = true;
                    } else if (name == "dcsvr_saccade_lead_ms") {
                        m_dcsvrGaze.leadMs = std::clamp(std::stof(value), 0.f, 200.f);
                        parsed = true;
                    } else if (name == "dcsvr_saccade_max_extend") {
                        m_dcsvrGaze.maxExtend = std::clamp(std::stof(value), 0.f, 1.f);
                        parsed = true;
                    } else if (name == "dcsvr_saccade_hold_ms") {
                        m_dcsvrGaze.holdMs = std::clamp(std::stof(value), 0.f, 200.f);
                        parsed = true;
                    } else if (name == "dcsvr_gaze_deadzone") {
                        m_dcsvrGaze.deadzoneDeg = std::clamp(std::stof(value), 0.f, 3.f);
                        parsed = true;
                    } else if (name == "focus_view_shape") {
'@
    $speed = @'
                                        isGazeValid = getEyeGaze(
                                            viewLocateInfo->displayTime, false /* getStateOnly */, gazeUnitVector);
                                    }
                                    // DCSVR saccade: gaze speed once per located frame, statistics every 30 s.
                                    if (isGazeValid)
                                        m_dcsvrFilter.UpdateSpeed(viewLocateInfo->displayTime, gazeUnitVector.x, gazeUnitVector.y, gazeUnitVector.z, m_dcsvrGaze);
                                    if (m_dcsvrGaze.widening || m_dcsvrGaze.deadzoneDeg > 0.f) {
                                        const auto statsNow = std::chrono::steady_clock::now();
                                        if (m_dcsvrStatsAt.time_since_epoch().count() == 0) m_dcsvrStatsAt = statsNow;
                                        const auto& stats = m_dcsvrFilter.Stats();
                                        if (statsNow - m_dcsvrStatsAt >= std::chrono::seconds(30) && stats.eyeFrames > 0) {
                                            Log(fmt::format("DCSVR gaze: {} eye-frames, {} saccades, widened {} ({:.1f}%%), held {} ({:.1f}%%), peak {:.0f} deg/s\n",
                                                            stats.eyeFrames, stats.saccades, stats.widened, 100.0 * stats.widened / stats.eyeFrames,
                                                            stats.held, 100.0 * stats.held / stats.eyeFrames, stats.peakSpeed));
                                            m_dcsvrFilter.ResetStats();
                                            m_dcsvrStatsAt = statsNow;
                                        }
                                    }
'@
    $gaze = @'
                                            {
                                                // DCSVR saccade: the eye's velocity in this view gives the lead; at
                                                // fixation, movements inside the deadzone keep the focus where it is.
                                                const XrFovf& eyeFov = m_cachedEyeFov[stereoViewIndex];
                                                const dcsvr::GazeVec2 gazed = m_dcsvrFilter.Eye((int)stereoViewIndex, viewLocateInfo->displayTime,
                                                    {projectedGaze.x, projectedGaze.y},
                                                    dcsvr::GazeFilter::NdcPerDegree(std::tan(eyeFov.angleLeft), std::tan(eyeFov.angleRight)),
                                                    dcsvr::GazeFilter::NdcPerDegree(std::tan(eyeFov.angleDown), std::tan(eyeFov.angleUp)), m_dcsvrGaze);
                                                projectedGaze = {gazed.x, gazed.y};
                                            }
                                            m_eyeGaze[stereoViewIndex] = projectedGaze;
'@
    $oldMinmax = @'
                                            const XrVector2f min{
                                                std::clamp(
                                                    m_eyeGaze[stereoViewIndex].x - horizontalFovSection, -1.f, 1.f),
                                                std::clamp(
                                                    m_eyeGaze[stereoViewIndex].y - verticalFovSection, -1.f, 1.f)};
                                            const XrVector2f max{
                                                std::clamp(
                                                    m_eyeGaze[stereoViewIndex].x + horizontalFovSection, -1.f, 1.f),
                                                std::clamp(
                                                    m_eyeGaze[stereoViewIndex].y + verticalFovSection, -1.f, 1.f)};
'@
    $minmax = @'
                                            XrVector2f min{
                                                std::clamp(
                                                    m_eyeGaze[stereoViewIndex].x - horizontalFovSection, -1.f, 1.f),
                                                std::clamp(
                                                    m_eyeGaze[stereoViewIndex].y - verticalFovSection, -1.f, 1.f)};
                                            XrVector2f max{
                                                std::clamp(
                                                    m_eyeGaze[stereoViewIndex].x + horizontalFovSection, -1.f, 1.f),
                                                std::clamp(
                                                    m_eyeGaze[stereoViewIndex].y + verticalFovSection, -1.f, 1.f)};
                                            {
                                                // DCSVR saccade: reach ahead along the eye's motion (same focus pixels).
                                                dcsvr::GazeVec2 lo{min.x, min.y}, hi{max.x, max.y};
                                                if (m_dcsvrFilter.Extend((int)stereoViewIndex, viewLocateInfo->displayTime, lo, hi, m_dcsvrGaze)) {
                                                    min = {lo.x, lo.y};
                                                    max = {hi.x, hi.y};
                                                }
                                            }
'@
    $edits = @(
        @((& $fix '#include "views.h"'), (& $fix "#include `"views.h`"`n#include `"dcsvr_gaze.h`"")),
        @((& $fix '        XrVector2f m_eyeGaze[xr::StereoView::Count]{};'), (& $fix "        XrVector2f m_eyeGaze[xr::StereoView::Count]{};`n        // DCSVR saccade: settings and state (native/quadviews/dcsvr_gaze.h).`n        dcsvr::GazeSettings m_dcsvrGaze{};`n        dcsvr::GazeFilter m_dcsvrFilter;`n        std::chrono::steady_clock::time_point m_dcsvrStatsAt{};")),
        @((& $fix '                    } else if (name == "focus_view_shape") {'), (& $fix $options)),
        @((& $fix "                                        isGazeValid = getEyeGaze(`n                                            viewLocateInfo->displayTime, false /* getStateOnly */, gazeUnitVector);`n                                    }"), (& $fix $speed)),
        @((& $fix '                                            m_eyeGaze[stereoViewIndex] = projectedGaze;'), (& $fix $gaze)),
        @((& $fix $oldMinmax), (& $fix $minmax))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# Sharpening taper (dcsvr_sharpen_taper=1): CAS sharpens the whole focus view at the same strength up to its edge,
# where the much softer periphery takes over, so the step in detail marks the focus edge. Inside the band where the
# composition fades the focus view out (2 x smoothen_focus_view_edges of the focus radius, for the round and the
# rounded-rectangle shape alike) the sharpened colour now fades back to the unsharpened one with the same smoothstep,
# so detail steps down gradually. One extra texel load per sharpened texel inside the band; off: identical output.
$sharpenText = [IO.File]::ReadAllText($sharpenSource)
if (-not $sharpenText.Contains('dcsvrTaper')) {
    $nl = if ($sharpenText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @('    float4 dcsvrSkip;',
          (('    float4 dcsvrSkip;',
            '    // DCSVR sharpen taper: x the edge band width in focus radii (0: off), y the shape exponent (0: rectangle).',
            '    float4 dcsvrTaper;') -join $nl)),
        @('[numthreads(WIDTH, HEIGHT, DEPTH)]',
          (('// DCSVR sharpen taper: inside the edge band the sharpening fades out as the composition fades the focus view out.',
            'float3 DcsvrTaper(float3 c, uint2 gxy) {',
            '    if (dcsvrTaper.x <= 0) {',
            '        return c;',
            '    }',
            '    float2 e = abs((float2(gxy) + 0.5) / dcsvrSkip.zw * 2.0 - 1.0);',
            '    float r = dcsvrTaper.y >= 1 ? pow(pow(e.x, dcsvrTaper.y) + pow(e.y, dcsvrTaper.y), 1.0 / dcsvrTaper.y) : max(e.x, e.y);',
            '    float w = 1 - smoothstep(1 - dcsvrTaper.x, 1, r);',
            '    [branch] if (w >= 1) {',
            '        return c;',
            '    }',
            '    return lerp(InputTexture.Load(int3(gxy, 0)).rgb, c, w);',
            '}',
            '',
            '[numthreads(WIDTH, HEIGHT, DEPTH)]') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($sharpenText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views sharpening shader changed: $(($edit[0] -split "`n")[0])" }
        $sharpenText = $sharpenText.Replace($edit[0], $edit[1])
    }
    $write = '    OutputTexture[ASU2(gxy)] = AF4(c, 1);'
    if (([regex]::Matches($sharpenText, [regex]::Escape($write))).Count -ne 4) { throw 'Quad Views sharpening shader changed: FP32 writes' }
    $sharpenText = $sharpenText.Replace($write, '    OutputTexture[ASU2(gxy)] = AF4(DcsvrTaper(c, gxy), 1);')
    [IO.File]::WriteAllText($sharpenSource, $sharpenText)
}
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('DcsvrTaper')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @('        alignas(4) float DcsvrSkip[4];',
          "        alignas(4) float DcsvrSkip[4];$($nl)        alignas(4) float DcsvrTaper[4];"),
        @('                sharpening.DcsvrSkip[3] = (float)focusView.subImage.imageRect.extent.height;',
          (('                sharpening.DcsvrSkip[3] = (float)focusView.subImage.imageRect.extent.height;',
            '                // DCSVR sharpen taper: the composition''s fade band (ProjectionPS: 2 x the edge smoothing).',
            '                sharpening.DcsvrTaper[0] = m_dcsvrSharpenTaper && !m_debugFocusView && m_smoothenFocusViewEdges > 0.f',
            '                                               ? std::clamp(2.f * m_smoothenFocusViewEdges, 0.01f, 0.95f) : 0.f;',
            '                sharpening.DcsvrTaper[1] = m_focusViewShape;') -join $nl)),
        @('                    } else if (name == "focus_view_shape") {',
          (('                    } else if (name == "dcsvr_sharpen_taper") {',
            '                        m_dcsvrSharpenTaper = std::stoi(value);',
            '                        parsed = true;',
            '                    } else if (name == "focus_view_shape") {') -join $nl)),
        @('        float m_focusViewShape{0.f};',
          "        float m_focusViewShape{0.f};$($nl)        bool m_dcsvrSharpenTaper{false};")
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# Periphery contrast (dcsvr_periphery_sharpen=S, 0: off): the periphery reaches the headset enlarged about five times
# with a bilinear filter, which costs it local contrast, and that loss (more than the missing detail) is what makes
# the periphery read as "blurred" next to the focus area (Patney et al. 2016, contrast-preserving foveation). The same
# CAS pass the focus view uses now also runs on each eye's periphery at its own resolution, before the enlargement,
# at strength S: about a quarter of the focus view's texels, no tile skip, no taper.
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('dcsvrPeripheryImage')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @('            ComPtr<ID3D11ShaderResourceView> dcsvrSharpenedSrv[xr::StereoView::Count];',
          (('            ComPtr<ID3D11ShaderResourceView> dcsvrSharpenedSrv[xr::StereoView::Count];',
            '            // DCSVR periphery contrast: the sharpened periphery of each eye and its views.',
            '            ComPtr<ID3D11Texture2D> dcsvrPeripheryImage[xr::StereoView::Count];',
            '            ComPtr<ID3D11UnorderedAccessView> dcsvrPeripheryUav[xr::StereoView::Count];',
            '            ComPtr<ID3D11ShaderResourceView> dcsvrPeripherySrv[xr::StereoView::Count];') -join $nl)),
        @((('                TraceLoggingWriteStop(local, "xrEndFrame_Sharpen");', '            }') -join $nl),
          (('                TraceLoggingWriteStop(local, "xrEndFrame_Sharpen");',
            '            }',
            '',
            '            // DCSVR periphery contrast: CAS on the periphery at its own resolution, before the enlargement.',
            '            ComPtr<ID3D11ShaderResourceView> dcsvrPeripherySource;',
            '            // Only for an opaque layer: the pass writes alpha 1.',
            '            if (m_dcsvrPeripherySharpen > 0.f && !m_debugFocusView && !(layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT)) {',
            '                const UINT width = (UINT)stereoView.subImage.imageRect.extent.width;',
            '                const UINT height = (UINT)stereoView.subImage.imageRect.extent.height;',
            '                ComPtr<ID3D11Texture2D>& image = swapchainForStereoView.dcsvrPeripheryImage[viewIndex];',
            '                D3D11_TEXTURE2D_DESC desc{};',
            '                if (image) {',
            '                    image->GetDesc(&desc);',
            '                }',
            '                if (!image || desc.Width != width || desc.Height != height) {',
            '                    desc = {};',
            '                    desc.ArraySize = 1;',
            '                    desc.Width = width;',
            '                    desc.Height = height;',
            '                    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;',
            '                    desc.MipLevels = 1;',
            '                    desc.SampleDesc.Count = 1;',
            '                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;',
            '                    CHECK_HRCMD(m_applicationDevice->CreateTexture2D(&desc, nullptr, image.ReleaseAndGetAddressOf()));',
            '                    swapchainForStereoView.dcsvrPeripheryUav[viewIndex].Reset();',
            '                    swapchainForStereoView.dcsvrPeripherySrv[viewIndex].Reset();',
            '                }',
            '                ComPtr<ID3D11UnorderedAccessView>& uav = swapchainForStereoView.dcsvrPeripheryUav[viewIndex];',
            '                if (!uav) {',
            '                    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};',
            '                    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;',
            '                    uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;',
            '                    CHECK_HRCMD(m_applicationDevice->CreateUnorderedAccessView(image.Get(), &uavDesc, uav.ReleaseAndGetAddressOf()));',
            '                }',
            '                ComPtr<ID3D11ShaderResourceView>& srv = swapchainForStereoView.dcsvrPeripherySrv[viewIndex];',
            '                if (!srv) {',
            '                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};',
            '                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
            '                    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;',
            '                    srvDesc.Texture2D.MipLevels = 1;',
            '                    CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(image.Get(), &srvDesc, srv.ReleaseAndGetAddressOf()));',
            '                }',
            '                SharpeningCSConstants periphery{};',
            '                CasSetup(periphery.Const0, periphery.Const1, std::clamp(m_dcsvrPeripherySharpen, 0.f, 1.f),',
            '                         (AF1)width, (AF1)height, (AF1)width, (AF1)height);',
            '                periphery.DcsvrSkip[2] = (float)width;',
            '                periphery.DcsvrSkip[3] = (float)height;',
            '                {',
            '                    D3D11_MAPPED_SUBRESOURCE mappedResources;',
            '                    CHECK_HRCMD(m_renderContext->Map(m_sharpeningCSConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResources));',
            '                    memcpy(mappedResources.pData, &periphery, sizeof(periphery));',
            '                    m_renderContext->Unmap(m_sharpeningCSConstants.Get(), 0);',
            '                }',
            '                ID3D11ShaderResourceView* nullSRV[] = {nullptr, nullptr};',
            '                m_renderContext->PSSetShaderResources(0, 2, nullSRV);',
            '                m_renderContext->CSSetConstantBuffers(0, 1, m_sharpeningCSConstants.GetAddressOf());',
            '                m_renderContext->CSSetShaderResources(0, 1, stereoSourceSrv.GetAddressOf());',
            '                m_renderContext->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);',
            '                m_renderContext->CSSetShader(m_sharpeningCS.Get(), nullptr, 0);',
            '                m_renderContext->Dispatch((width + 15) / 16, (height + 15) / 16, 1);',
            '                ID3D11UnorderedAccessView* nullUAV[] = {nullptr};',
            '                m_renderContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);',
            '                m_renderContext->CSSetShaderResources(0, 1, nullSRV);',
            '                dcsvrPeripherySource = srv;',
            '            }') -join $nl)),
        @('                ComPtr<ID3D11ShaderResourceView> srvForStereoView = stereoSourceSrv;',
          '                ComPtr<ID3D11ShaderResourceView> srvForStereoView = dcsvrPeripherySource ? dcsvrPeripherySource : stereoSourceSrv;'),
        @('                    } else if (name == "dcsvr_sharpen_taper") {',
          (('                    } else if (name == "dcsvr_periphery_sharpen") {',
            '                        m_dcsvrPeripherySharpen = std::clamp(std::stof(value), 0.f, 1.f);',
            '                        parsed = true;',
            '                    } else if (name == "dcsvr_sharpen_taper") {') -join $nl)),
        @('        bool m_dcsvrSharpenTaper{false};',
          "        bool m_dcsvrSharpenTaper{false};$($nl)        float m_dcsvrPeripherySharpen{0.f};")
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# Periphery stability (dcsvr_temporal=1; native/quadviews/dcsvr_perception.h and dcsvr_temporal_cs.hlsl, which explain
# it; tested by tests/perception; docs/PERCEPTION.md): a temporal low-pass on each eye's periphery at its own
# resolution, after the flatten and before the periphery contrast pass, against the shimmer the enlargement makes
# visible. The history is reprojected by the head's rotation and clipped to the current 3x3 neighbourhood; its weight
# is time-based (the same at 2X, 3X and Auto), zero inside the focus area and ramping up outside it; saccades, gaps,
# size changes and large head turns start it over, for both eyes at once. The gaze and saccade state of each located
# display time is kept for the submitted frame (one record for both eyes). Off: nothing runs, identical output.
foreach ($perceptionFile in @('dcsvr_perception.h', 'dcsvr_temporal_cs.hlsl')) {
    Copy-Item -LiteralPath (Join-Path $workspaceRoot "native/quadviews/$perceptionFile") -Destination (Join-Path $quadRoot "openxr-api-layer/$perceptionFile") -Force
}
$projectSource = Join-Path $quadRoot 'openxr-api-layer/openxr-api-layer.vcxproj'
$projectText = [IO.File]::ReadAllText($projectSource)
if (-not $projectText.Contains('dcsvr_temporal_cs.hlsl')) {
    $nl = if ($projectText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $anchor = (('  <ItemGroup>', '    <ResourceCompile Include="resource.rc" />') -join $nl)
    if (([regex]::Matches($projectText, [regex]::Escape($anchor))).Count -ne 1) { throw 'Quad Views project changed: resource item group' }
    $item = (('  <ItemGroup>',
              '    <FxCompile Include="dcsvr_temporal_cs.hlsl">',
              '      <ShaderType>Compute</ShaderType>',
              '      <ShaderModel>5.0</ShaderModel>',
              '      <VariableName>g_%(Filename)</VariableName>',
              '      <HeaderFileOutput>$(IntDir)%(Filename).h</HeaderFileOutput>',
              '      <ObjectFileOutput>',
              '      </ObjectFileOutput>',
              '    </FxCompile>',
              '    <ClInclude Include="dcsvr_perception.h" />',
              '  </ItemGroup>',
              $anchor) -join $nl)
    [IO.File]::WriteAllText($projectSource, $projectText.Replace($anchor, $item))
}
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('DCSVR periphery stability')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $fix = { param($text) ($text -replace "`r`n", "`n").TrimEnd("`n") -replace "`n", $nl }
    $members = @'
        float m_dcsvrPeripherySharpen{0.f};
        // DCSVR periphery stability: settings, the gaze of each located display time, the per-frame decision, each
        // eye's history (two textures written in turn) and the pass.
        dcsvr::TemporalSettings m_dcsvrTemporal{};
        dcsvr::GazeHistory m_dcsvrGazeHistory;
        dcsvr::TemporalController m_dcsvrTemporalControl;
        bool m_dcsvrTemporalActive{false};
        bool m_dcsvrTemporalLogged{false};
        bool m_dcsvrTemporalNoMemoryLogged{false};
        std::chrono::steady_clock::time_point m_dcsvrTemporalStatsAt{};
        struct DcsvrHistory {
            ComPtr<ID3D11Texture2D> image[2];
            ComPtr<ID3D11ShaderResourceView> srv[2];
            ComPtr<ID3D11UnorderedAccessView> uav[2];
            int next{0};
            bool fresh{true};
            int64_t time{0};  // display time of the frame this history was last written for
        };
        DcsvrHistory m_dcsvrHistory[xr::StereoView::Count];
        ComPtr<ID3D11Buffer> m_dcsvrTemporalConstants;
        ComPtr<ID3D11ComputeShader> m_dcsvrTemporalCS;
'@
    $options = @'
                    } else if (name == "dcsvr_temporal") {
                        m_dcsvrTemporal.enabled = std::stoi(value);
                        parsed = true;
                    } else if (name == "dcsvr_temporal_tau_ms") {
                        m_dcsvrTemporal.tauMs = std::clamp(std::stof(value), 0.f, 200.f);
                        parsed = true;
                    } else if (name == "dcsvr_temporal_ramp_deg") {
                        m_dcsvrTemporal.rampDeg = std::clamp(std::stof(value), 1.f, 30.f);
                        parsed = true;
                    } else if (name == "dcsvr_temporal_clip") {
                        m_dcsvrTemporal.clip = std::clamp(std::stof(value), 0.25f, 4.f);
                        parsed = true;
                    } else if (name == "dcsvr_temporal_debug") {
                        m_dcsvrTemporal.debug = std::stoi(value);
                        parsed = true;
                    } else if (name == "dcsvr_periphery_sharpen") {
'@
    $oldSpeed = @'
                                    if (isGazeValid)
                                        m_dcsvrFilter.UpdateSpeed(viewLocateInfo->displayTime, gazeUnitVector.x, gazeUnitVector.y, gazeUnitVector.z, m_dcsvrGaze);
'@
    $speed = @'
                                    if (isGazeValid)
                                        m_dcsvrFilter.UpdateSpeed(viewLocateInfo->displayTime, gazeUnitVector.x, gazeUnitVector.y, gazeUnitVector.z, m_dcsvrGaze);
                                    // DCSVR periphery stability: this display time's gaze and saccade state, one record
                                    // for both eyes, read when the frame is submitted.
                                    if (m_dcsvrTemporal.enabled) {
                                        dcsvr::GazeSample sample{viewLocateInfo->displayTime};
                                        sample.valid = isGazeValid && std::isfinite(gazeUnitVector.x) &&
                                                       std::isfinite(gazeUnitVector.y) && std::isfinite(gazeUnitVector.z);
                                        sample.inSaccade = sample.valid && m_dcsvrFilter.InSaccade();
                                        if (sample.valid) {
                                            sample.dir[0] = gazeUnitVector.x;
                                            sample.dir[1] = gazeUnitVector.y;
                                            sample.dir[2] = gazeUnitVector.z;
                                        }
                                        m_dcsvrGazeHistory.Record(sample);
                                    }
'@
    $oldFrame = @'
                            projectionViewAllocator.push_back(
                                {proj->views[xr::StereoView::Left], proj->views[xr::StereoView::Right]});
'@
    $frame = @'
                            projectionViewAllocator.push_back(
                                {proj->views[xr::StereoView::Left], proj->views[xr::StereoView::Right]});

                            // DCSVR periphery stability: one decision per frame for both eyes (a reset in one eye
                            // resets both), from the poses DCS rendered with and the gaze of this display time.
                            if (m_dcsvrTemporal.enabled) {
                                dcsvr::TemporalEye eyes[xr::StereoView::Count];
                                for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                                    const XrQuaternionf& pose = proj->views[eye].pose.orientation;
                                    const XrQuaternionf& inView = m_cachedEyePoses[eye].orientation;
                                    const XrFovf& fov = m_cachedEyeFov[eye];
                                    eyes[eye].pose = {pose.x, pose.y, pose.z, pose.w};
                                    eyes[eye].eyeInView = {inView.x, inView.y, inView.z, inView.w};
                                    eyes[eye].fov = {std::tan(fov.angleLeft), std::tan(fov.angleRight), std::tan(fov.angleUp), std::tan(fov.angleDown)};
                                    eyes[eye].width = (uint32_t)proj->views[eye].subImage.imageRect.extent.width;
                                    eyes[eye].height = (uint32_t)proj->views[eye].subImage.imageRect.extent.height;
                                }
                                // As the periphery contrast pass: only for an opaque layer, not with the focus debug view.
                                m_dcsvrTemporalActive = !m_debugFocusView && !(proj->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
                                m_dcsvrTemporalControl.BeginFrame(frameEndInfo->displayTime, eyes, (uint64_t)proj->space,
                                                                  m_dcsvrGazeHistory.At(frameEndInfo->displayTime), m_dcsvrTemporalActive, m_dcsvrTemporal);
                                if (!m_dcsvrTemporalLogged) {
                                    Log(fmt::format("DCSVR periphery stability: on, {:.0f} ms, ramp {:.0f} deg, clip {:.2f}{}\n", m_dcsvrTemporal.tauMs,
                                                    m_dcsvrTemporal.rampDeg, m_dcsvrTemporal.clip, m_dcsvrTemporal.debug ? ", debug view" : ""));
                                    m_dcsvrTemporalLogged = true;
                                }
                                const auto statsNow = std::chrono::steady_clock::now();
                                if (m_dcsvrTemporalStatsAt.time_since_epoch().count() == 0) m_dcsvrTemporalStatsAt = statsNow;
                                const auto& stats = m_dcsvrTemporalControl.Stats();
                                if (statsNow - m_dcsvrTemporalStatsAt >= std::chrono::seconds(30) && stats.frames > 0) {
                                    Log(fmt::format("DCSVR periphery stability: {} frames, {} filtered at {:.1f} FPS, {} resets (saccade {}, gap {}, turn {}, size {}, other {}), fades out {:.0f}-{:.0f} deg\n",
                                                    stats.frames, stats.filtered, stats.dtSum > 0.0 ? stats.filtered / stats.dtSum : 0.0, stats.resets,
                                                    stats.saccade, stats.gap, stats.turn, stats.size, stats.other, stats.falloff.startDeg, stats.falloff.endDeg));
                                    m_dcsvrTemporalControl.ResetStats();
                                    m_dcsvrTemporalStatsAt = statsNow;
                                }
                            }
'@
    $pass = @'
            // DCSVR periphery stability: the stabilised periphery replaces the source for what follows, so the periphery
            // contrast pass reads it and writes its own texture (sharpening never enters the history).
            // Only when the frame's decision runs the pass (strength 0 does not), and once per eye and frame: a second
            // projection layer of the same frame keeps its own image rather than blending with the first one's.
            if (m_dcsvrTemporal.enabled && m_dcsvrTemporalActive && m_dcsvrTemporalCS && m_dcsvrTemporalControl.Active() &&
                m_dcsvrHistory[viewIndex].time != m_dcsvrTemporalControl.Time()) {
                const UINT width = (UINT)stereoView.subImage.imageRect.extent.width;
                const UINT height = (UINT)stereoView.subImage.imageRect.extent.height;
                DcsvrHistory& history = m_dcsvrHistory[viewIndex];
                D3D11_TEXTURE2D_DESC desc{};
                if (history.image[0]) {
                    history.image[0]->GetDesc(&desc);
                }
                if (!history.image[0] || desc.Width != width || desc.Height != height) {
                    desc = {};
                    desc.ArraySize = 1;
                    desc.Width = width;
                    desc.Height = height;
                    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    desc.MipLevels = 1;
                    desc.SampleDesc.Count = 1;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                    // Without the memory for the history (VRAM at its limit) the frame goes on unfiltered: never an
                    // error returned to DCS from xrEndFrame for an optional pass.
                    bool created = true;
                    for (int i = 0; i < 2 && created; i++) {
                        created = SUCCEEDED(m_applicationDevice->CreateTexture2D(&desc, nullptr, history.image[i].ReleaseAndGetAddressOf())) &&
                                  SUCCEEDED(m_applicationDevice->CreateShaderResourceView(history.image[i].Get(), nullptr, history.srv[i].ReleaseAndGetAddressOf())) &&
                                  SUCCEEDED(m_applicationDevice->CreateUnorderedAccessView(history.image[i].Get(), nullptr, history.uav[i].ReleaseAndGetAddressOf()));
                    }
                    if (!created) {
                        history = {};
                        m_dcsvrTemporalControl.Invalidate();
                        if (!m_dcsvrTemporalNoMemoryLogged) {
                            Log(fmt::format("DCSVR periphery stability: no memory for the {}x{} history, this eye unfiltered\n", width, height));
                            m_dcsvrTemporalNoMemoryLogged = true;
                        }
                    }
                    history.next = 0;
                    history.fresh = true;
                }
                if (history.image[0] && history.image[1]) {
                // A frame decided for but not written (an early return in between) breaks the history's continuity.
                if (history.time != m_dcsvrTemporalControl.PreviousTime()) {
                    history.fresh = true;
                }
                history.time = m_dcsvrTemporalControl.Time();
                m_dcsvrTemporal.focusShape = m_focusViewShape;
                dcsvr::TemporalConstants temporal{};
                m_dcsvrTemporalControl.Constants((int)viewIndex,
                                                 {std::tan(focusView.fov.angleLeft), std::tan(focusView.fov.angleRight),
                                                  std::tan(focusView.fov.angleUp), std::tan(focusView.fov.angleDown)},
                                                 m_dcsvrTemporal, temporal);
                if (history.fresh) {
                    temporal.params[1] = 0.f;  // a new texture holds no history yet
                    history.fresh = false;
                }
                {
                    D3D11_MAPPED_SUBRESOURCE mappedResources;
                    CHECK_HRCMD(m_renderContext->Map(m_dcsvrTemporalConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResources));
                    memcpy(mappedResources.pData, &temporal, sizeof(temporal));
                    m_renderContext->Unmap(m_dcsvrTemporalConstants.Get(), 0);
                }
                const int write = history.next, read = 1 - history.next;
                ID3D11ShaderResourceView* nullSRV[] = {nullptr, nullptr};
                m_renderContext->PSSetShaderResources(0, 2, nullSRV);
                ID3D11ShaderResourceView* sources[] = {stereoSourceSrv.Get(), history.srv[read].Get()};
                m_renderContext->CSSetConstantBuffers(0, 1, m_dcsvrTemporalConstants.GetAddressOf());
                m_renderContext->CSSetShaderResources(0, 2, sources);
                m_renderContext->CSSetUnorderedAccessViews(0, 1, history.uav[write].GetAddressOf(), nullptr);
                m_renderContext->CSSetSamplers(0, 1, m_linearClampSampler.GetAddressOf());
                m_renderContext->CSSetShader(m_dcsvrTemporalCS.Get(), nullptr, 0);
                m_renderContext->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                ID3D11UnorderedAccessView* nullUAV[] = {nullptr};
                m_renderContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
                m_renderContext->CSSetShaderResources(0, 2, nullSRV);
                history.next = read;
                stereoSourceSrv = history.srv[write];
                }
            }

            // DCSVR periphery contrast: CAS on the periphery at its own resolution, before the enlargement.
'@
    $shader = @'
            CHECK_HRCMD(m_applicationDevice->CreateComputeShader(
                g_SharpeningCS, sizeof(g_SharpeningCS), nullptr, m_sharpeningCS.ReleaseAndGetAddressOf()));

            // DCSVR periphery stability.
            {
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = (UINT)sizeof(dcsvr::TemporalConstants);
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                CHECK_HRCMD(m_applicationDevice->CreateBuffer(&desc, nullptr, m_dcsvrTemporalConstants.ReleaseAndGetAddressOf()));
            }
            CHECK_HRCMD(m_applicationDevice->CreateComputeShader(
                g_dcsvr_temporal_cs, sizeof(g_dcsvr_temporal_cs), nullptr, m_dcsvrTemporalCS.ReleaseAndGetAddressOf()));
'@
    $release = @'
                    m_sharpeningCS.Reset();
                    // DCSVR periphery stability: the next session starts over.
                    m_dcsvrTemporalCS.Reset();
                    m_dcsvrTemporalConstants.Reset();
                    for (auto& history : m_dcsvrHistory) {
                        history = {};
                    }
                    m_dcsvrTemporalControl.Invalidate();
'@
    $edits = @(
        @((& $fix '#include "dcsvr_gaze.h"'), (& $fix "#include `"dcsvr_gaze.h`"`n#include `"dcsvr_perception.h`"")),
        @((& $fix '#include <SharpeningCS.h>'), (& $fix "#include <SharpeningCS.h>`n#include <dcsvr_temporal_cs.h>")),
        @((& $fix '        float m_dcsvrPeripherySharpen{0.f};'), (& $fix $members)),
        @((& $fix '                    } else if (name == "dcsvr_periphery_sharpen") {'), (& $fix $options)),
        @((& $fix $oldSpeed), (& $fix $speed)),
        @((& $fix $oldFrame), (& $fix $frame)),
        @((& $fix '            // DCSVR periphery contrast: CAS on the periphery at its own resolution, before the enlargement.'), (& $fix $pass)),
        @((& $fix "            CHECK_HRCMD(m_applicationDevice->CreateComputeShader(`n                g_SharpeningCS, sizeof(g_SharpeningCS), nullptr, m_sharpeningCS.ReleaseAndGetAddressOf()));"), (& $fix $shader)),
        @((& $fix '                    m_sharpeningCS.Reset();'), (& $fix $release))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# Gaze export for the pupil shift layer (DcsVr_QuadViewsGaze; native/quadviews/dcsvr_perception.h explains it; tested by
# tests/perception and native/pupil_shift's harness): without a gaze the focus views sit at a fixed spot (resting gaze
# plus a per-eye horizontal offset) that the pupil shift layer, reading the focus views, took for a real gaze of about
# 2.4 degrees outwards per eye. Each located display time's gaze record (the periphery stability's) is now kept always,
# in one history the export reads: flags (sample, valid, saccade) and the gaze in each eye's frame. Nothing else changes.
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('DCSVR gaze export')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $fix = { param($text) ($text -replace "`r`n", "`n").TrimEnd("`n") -replace "`n", $nl }
    $member = @'
        // DCSVR gaze export: the process's one gaze history, also read by DcsVr_QuadViewsGaze.
        dcsvr::GazeHistory& m_dcsvrGazeHistory{dcsvr::SharedGazeHistory()};
'@
    $oldRecord = @'
                                    if (m_dcsvrTemporal.enabled) {
                                        dcsvr::GazeSample sample{viewLocateInfo->displayTime};
'@
    $record = @'
                                    // DCSVR gaze export: recorded whether the periphery stability is on or not, for the
                                    // pupil shift layer (DcsVr_QuadViewsGaze).
                                    {
                                        dcsvr::GazeSample sample{viewLocateInfo->displayTime};
'@
    $oldStore = @'
                                        m_dcsvrGazeHistory.Record(sample);
'@
    $store = @'
                                        if (sample.valid) {
                                            const dcsvr::Quat eyeInView[xr::StereoView::Count] = {
                                                {m_cachedEyePoses[0].orientation.x, m_cachedEyePoses[0].orientation.y,
                                                 m_cachedEyePoses[0].orientation.z, m_cachedEyePoses[0].orientation.w},
                                                {m_cachedEyePoses[1].orientation.x, m_cachedEyePoses[1].orientation.y,
                                                 m_cachedEyePoses[1].orientation.z, m_cachedEyePoses[1].orientation.w}};
                                            sample.SetEyes(eyeInView);
                                        }
                                        m_dcsvrGazeHistory.Record(sample);
'@
    $export = @'

// DCSVR gaze export: the gaze Quad Views used for a display time, for DCS Control's pupil shift layer (found with
// GetModuleHandle/GetProcAddress). Flags: 1 a sample, 2 a real gaze (not the fixed focus), 4 a saccade; left and right
// receive the one combined gaze as a unit vector in each eye's view frame. See dcsvr_perception.h.
// The periphery stability's state for DCS Control's in-headset panel (OFXR): see dcsvr::PeripheryStatus.
extern "C" __declspec(dllexport) uint64_t DcsVr_QuadViewsPeripheryStatus() noexcept {
    return dcsvr::PeripheryStatus().load(std::memory_order_relaxed);
}

// A scene cut the head pose does not show (another view or camera, a respawn): the periphery stability's next frame
// starts over. Callable from any thread; does nothing with periphery stability off.
extern "C" __declspec(dllexport) void DcsVr_QuadViewsTemporalReset() noexcept {
    dcsvr::SceneCuts().fetch_add(1u, std::memory_order_relaxed);
}

// noexcept, with nothing thrown across the C boundary (the history's mutex can throw): 0, no sample, instead.
extern "C" __declspec(dllexport) uint32_t DcsVr_QuadViewsGaze(int64_t displayTime, float left[3], float right[3]) noexcept {
    try {
        return dcsvr::ExportGaze(dcsvr::SharedGazeHistory().At(displayTime), left, right);
    } catch (...) {
        return 0u;
    }
}
'@
    $edits = @(
        @((& $fix '        dcsvr::GazeHistory m_dcsvrGazeHistory;'), (& $fix $member)),
        @((& $fix $oldRecord), (& $fix $record)),
        @((& $fix $oldStore), (& $fix $store))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    $layerText = $layerText.TrimEnd() + $nl + (& $fix $export) + $nl
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# Far depth (dcsvr_far_depth=1; docs/FAR_DEPTH.md, native/quadviews/dcsvr_depthwarp.h, dcsvr_depthwarp_cs.hlsl,
# dcsvr_depthwarp_ps.hlsli, dcsvr_depth_import.h, dcsvr_depth_export.h; tested by tests/depthwarp): a depth-selective,
# infinity-anchored disparity warp from the engine module's depth of both focus views. The engine pushes each view's
# depth through DcsVr_QuadViewsDepth; once per frame the layer takes both views of one engine frame or neither, the
# controller decides the gain (changed only on saccades, blinks and scene cuts; held fields bridge up to two frames
# without depth), FieldMain builds each eye's displacement field, and the composition moves its focus sample and its
# periphery sample by that one field. Off, without the engine's depth or on an alpha-blended layer: nothing is bound
# and the composition's Far depth constants stay 0, which tests/quadviews-composition proves is the composition without
# Far depth, bit for bit, on the DLL's own shader bytecode.
foreach ($farDepthFile in @('dcsvr_depthwarp.h', 'dcsvr_depthwarp_cs.hlsl', 'dcsvr_coverage_cs.hlsl', 'dcsvr_depthwarp_ps.hlsli', 'dcsvr_depth_import.h', 'dcsvr_depth_export.h')) {
    Copy-Item -LiteralPath (Join-Path $workspaceRoot "native/quadviews/$farDepthFile") -Destination (Join-Path $quadRoot "openxr-api-layer/$farDepthFile") -Force
}
$projectText = [IO.File]::ReadAllText($projectSource)
if (-not $projectText.Contains('dcsvr_depthwarp_cs.hlsl')) {
    $nl = if ($projectText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $anchor = (('  <ItemGroup>', '    <ResourceCompile Include="resource.rc" />') -join $nl)
    if (([regex]::Matches($projectText, [regex]::Escape($anchor))).Count -ne 1) { throw 'Quad Views project changed: resource item group' }
    # FieldMain and CoverageMain are built into the layer: WarpMain, the compute fallback, stays in the offline test.
    $item = (('  <ItemGroup>',
              '    <FxCompile Include="dcsvr_depthwarp_cs.hlsl">',
              '      <ShaderType>Compute</ShaderType>',
              '      <ShaderModel>5.0</ShaderModel>',
              '      <EntryPointName>FieldMain</EntryPointName>',
              '      <VariableName>g_%(Filename)</VariableName>',
              '      <HeaderFileOutput>$(IntDir)%(Filename).h</HeaderFileOutput>',
              '      <ObjectFileOutput>',
              '      </ObjectFileOutput>',
              '    </FxCompile>',
              '    <FxCompile Include="dcsvr_coverage_cs.hlsl">',
              '      <ShaderType>Compute</ShaderType>',
              '      <ShaderModel>5.0</ShaderModel>',
              '      <EntryPointName>CoverageMain</EntryPointName>',
              '      <VariableName>g_%(Filename)</VariableName>',
              '      <HeaderFileOutput>$(IntDir)%(Filename).h</HeaderFileOutput>',
              '      <ObjectFileOutput>',
              '      </ObjectFileOutput>',
              '    </FxCompile>',
              '    <ClInclude Include="dcsvr_depthwarp.h" />',
              '    <ClInclude Include="dcsvr_depth_export.h" />',
              '    <ClInclude Include="dcsvr_depth_import.h" />',
              '    <None Include="dcsvr_depthwarp_ps.hlsli" />',
              '  </ItemGroup>',
              $anchor) -join $nl)
    [IO.File]::WriteAllText($projectSource, $projectText.Replace($anchor, $item))
}
# The composition, at the three anchors tests/depthwarp's InjectProjectionPS uses (the same text: the test checks the
# built shader equals its own injection into the copy kept here, ProjectionPS.nofardepth.hlsl, which the composition
# tests also take as the composition without Far depth).
$pixelShaderText = [IO.File]::ReadAllText($pixelShaderSource)
if (-not $pixelShaderText.Contains('dcsvrDisparityField')) {
    [IO.File]::WriteAllText((Join-Path $quadRoot 'openxr-api-layer/ProjectionPS.nofardepth.hlsl'), $pixelShaderText)
    $nl = if ($pixelShaderText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $fix = { param($text) ($text -replace "`r`n", "`n").TrimEnd("`n") -replace "`n", $nl }
    $edits = @(
        @((& $fix "    float focusViewShape;`n};"),
          (& $fix "    float focusViewShape;`n    float4 dcsvrWarp; // DCSVR Far depth: x on, yz focus uv -> periphery uv, w a held field`n    float4 dcsvrReuse[3]; // DCSVR Far depth: a held field's reprojection`n};")),
        @('Texture2D sourceFocusTexture : register(t1);',
          (& $fix "Texture2D sourceFocusTexture : register(t1);`nTexture2D<float2> dcsvrDisparityField : register(t2);`n#include `"dcsvr_depthwarp_ps.hlsli`"")),
        @((& $fix "    float4 color0 = sourceStereoTexture.Sample(sourceSampler, texcoord);`n`n    float2 layer1ProjectedCoordNdc = projectedFocusCoord.xy / projectedFocusCoord.z;"),
          (& $fix ("    float2 layer1ProjectedCoordNdc = projectedFocusCoord.xy / projectedFocusCoord.z;`n" +
                   "    // DCSVR Far depth: the focus and the periphery sample move by one displacement field.`n" +
                   "    float2 dcsvrTexcoord = texcoord;`n" +
                   "    DcsvrApplyDisparity(dcsvrWarp, dcsvrReuse, dcsvrDisparityField, sourceSampler, layer1ProjectedCoordNdc, dcsvrTexcoord);`n" +
                   "    float4 color0 = sourceStereoTexture.Sample(sourceSampler, dcsvrTexcoord);")))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($pixelShaderText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views shader changed: $(($edit[0] -split "`n")[0])" }
        $pixelShaderText = $pixelShaderText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($pixelShaderSource, $pixelShaderText)
}
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('DCSVR Far depth')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $fix = { param($text) ($text -replace "`r`n", "`n").TrimEnd("`n") -replace "`n", $nl }
    $constants = @'
        alignas(4) float focusViewShape;
        alignas(4) float padding[3];
        // DCSVR Far depth (dcsvr_depthwarp_ps.hlsli): x 1 while a field is bound, yz focus uv to periphery uv, w 1 for a
        // held field; the held field's reprojection. All 0 without Far depth.
        alignas(4) float dcsvrWarp[4];
        alignas(4) float dcsvrReuse[3][4];
    };
    static_assert(sizeof(ProjectionPSConstants) == 96, "ProjectionPSConstants must match ProjectionPS.hlsl's cbuffer");
'@
    $members = @'
        ComPtr<ID3D11ComputeShader> m_dcsvrTemporalCS;
        // DCSVR Far depth: the settings (dcsvr_far_depth*), the gain controller and this frame's decision, the jitter
        // centre of each eye, each eye's field (kept as the held field for a frame or two without depth), the views of
        // the engine's textures and the field pass. The engine's pushes land in dcsvr::SharedDepthImport().
        dcsvr::DisparitySettings m_dcsvrFarDepth{};
        float m_dcsvrFarZ0Override{0.f};  // an explicit Z0 or gamma wins over the preset, whichever line comes first
        float m_dcsvrFarGammaOverride{0.f};
        dcsvr::DisparityController m_dcsvrFarControl;
        dcsvr::DisparityDecision m_dcsvrFarDecision{};
        dcsvr::JitterCentreEstimator m_dcsvrFarJitter[xr::StereoView::Count];
        XrTime m_dcsvrFarTime{0};
        bool m_dcsvrFarFrameWarp{false};  // this display time's fields are valid for both eyes
        bool m_dcsvrFarWarp{false};       // and the projection layer being composited uses them
        bool m_dcsvrFarLogged{false};
        bool m_dcsvrFarNoMemoryLogged{false};
        uint64_t m_dcsvrFarFrames{0};
        std::chrono::steady_clock::time_point m_dcsvrFarStatsAt{};
        DcsvrField m_dcsvrField[xr::StereoView::Count];
        std::vector<DcsvrDepthViews> m_dcsvrDepthViews;
        ComPtr<ID3D11Buffer> m_dcsvrFarConstants;
        ComPtr<ID3D11ComputeShader> m_dcsvrFieldCS;
        // DCSVR Far depth overlay coverage (CoverageMain): its pass and constants, and its diagnostics: the changed field
        // texels of both eyes, sampled every m_dcsvrCoverageSampleFrames frames into a staging buffer read 6 or more
        // frames later. Never every frame: any Map of the immediate context synchronises a threaded driver with DCS's
        // render thread, even with DO_NOT_WAIT.
        ComPtr<ID3D11ComputeShader> m_dcsvrCoverageCS;
        ComPtr<ID3D11Buffer> m_dcsvrCoverageConstants;
        ComPtr<ID3D11Buffer> m_dcsvrCoverageCount;
        ComPtr<ID3D11UnorderedAccessView> m_dcsvrCoverageCountUav;
        ComPtr<ID3D11Buffer> m_dcsvrCoverageStaging;
        uint64_t m_dcsvrCoverageStagingTexels{0};  // the field texels of both eyes behind the pending copy, 0 when none
        uint64_t m_dcsvrCoverageCopyFrame{0};      // m_dcsvrFarFrames when it was copied (or last tried)
        bool m_dcsvrCoverageOn{true};              // dcsvr_far_depth_coverage
        int m_dcsvrCoverageSampleFrames{45};       // dcsvr_far_depth_coverage_sample: frames between samples, 0 none
        // How the copy is read: 0 in the submitted image's view format (DCS's target holds the bytes CopyToHMDPass copies
        // into the swapchain: the same bytes then read alike, whatever the two formats say), 1 in the render target's
        // own view format (a CopyToHMDPass that converts). Samples with nearly the whole view changed, three in a row,
        // flip it: the wrong reading changes every pixel.
        uint32_t m_dcsvrCoverageEncoding{0};
        uint32_t m_dcsvrCoverageSuspectRun{0};
        uint32_t m_dcsvrCoverageFlips{0};
        bool m_dcsvrCoverageDump{false};  // dcsvr_far_depth_coverage_dump: one-shot BMPs of the left eye's inputs and mask
        bool m_dcsvrCoverageDumpSetting{false};
        uint64_t m_dcsvrCoverageDumpAt{450};  // m_dcsvrFarFrames from which it fires on a frame with coverage (the A/B key re-arms)
        bool m_dcsvrCoverageThisFrame{false};
        uint32_t m_dcsvrCoveragePercent{255};  // the last sample's share x 100, 255 unknown
        uint64_t m_dcsvrCoverageLastFrame{0};  // m_dcsvrFarFrames of the last frame with coverage
        // DCSVR Far depth's in-flight A/B switch (dcsvr_far_depth_toggle=virtual-key:modifiers, 1 Ctrl, 2 Alt, 4 Shift;
        // 0:0 none, the format of DCS Control's other in-flight keys): off shows the unwarped image at once, on brings the
        // full effect back at once (counted as a scene cut). The engine keeps exporting either way.
        int m_dcsvrFarKeyVk{0};
        int m_dcsvrFarKeyMods{0};
        bool m_dcsvrFarKeyOff{false};
        bool m_dcsvrFarKeyDown{false};
        std::chrono::steady_clock::time_point m_dcsvrFarKeyAt{};
        uint32_t m_dcsvrFarToggles{0};
        // The extended panel status (DcsVr_QuadViewsDisparityStatus2): frames warped and held in the last whole second.
        std::chrono::steady_clock::time_point m_dcsvrFarSecondAt{};
        uint32_t m_dcsvrFarWarpedCount{0}, m_dcsvrFarHeldCount{0}, m_dcsvrFarWarpedLast{0}, m_dcsvrFarHeldLast{0};
        struct {
            uint64_t frames{0};    // frames with both eyes' coverage built
            uint64_t measured{0};  // of them, read back
            double share{0.0};     // the sum of their covered shares (before the dilation)
            uint64_t suspect{0};   // read back above overlaySuspectShare
            uint64_t missing{0};   // frames whose coverage could not be built (no submitted image, another pixel grid)
        } m_dcsvrCoverageStats;
        bool m_dcsvrCoverageGridLogged{false};
'@
    $options = @'
                    } else if (name == "dcsvr_far_depth") {
                        m_dcsvrFarDepth.enabled = std::stoi(value);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_preset") {
                        // subtle, default or strong (or 0-2): gamma and Z0 (docs/FAR_DEPTH.md section 1). An explicit
                        // dcsvr_far_depth_z0_m or dcsvr_far_depth_gamma keeps winning, whichever line comes first.
                        const std::string preset = dcsvrSettingWord(value);
                        if (preset == "subtle" || preset == "0") {
                            m_dcsvrFarDepth.ApplyPreset(dcsvr::FarDepthPreset::Subtle);
                        } else if (preset == "default" || preset == "1") {
                            m_dcsvrFarDepth.ApplyPreset(dcsvr::FarDepthPreset::Default);
                        } else if (preset == "strong" || preset == "2") {
                            m_dcsvrFarDepth.ApplyPreset(dcsvr::FarDepthPreset::Strong);
                        } else if (preset == "near" || preset == "3") {
                            m_dcsvrFarDepth.ApplyPreset(dcsvr::FarDepthPreset::Near);
                        } else {
                            throw std::invalid_argument(value);
                        }
                        if (m_dcsvrFarZ0Override > 0.f) {
                            m_dcsvrFarDepth.z0M = m_dcsvrFarZ0Override;
                        }
                        if (m_dcsvrFarGammaOverride > 0.f) {
                            m_dcsvrFarDepth.gamma = m_dcsvrFarGammaOverride;
                        }
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_world_ipd") {
                        // The Near preset's eye distance factor for the world (1 or less: the infinity-anchored curve).
                        m_dcsvrFarDepth.worldIpdScale = std::clamp(std::stof(value), 0.f, 2.f);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_z0_m") {
                        m_dcsvrFarDepth.z0M = m_dcsvrFarZ0Override = std::clamp(std::stof(value), 5.f, 2000.f);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_gamma") {
                        // Below 1, or the target disparity never rises above the natural one beyond Z0.
                        m_dcsvrFarDepth.gamma = m_dcsvrFarGammaOverride = std::clamp(std::stof(value), 0.05f, 0.95f);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_fade_start_m") {
                        m_dcsvrFarDepth.fadeStartM = std::clamp(std::stof(value), 100.f, 100000.f);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_fade_end_m") {
                        m_dcsvrFarDepth.fadeEndM = std::clamp(std::stof(value), 100.f, 200000.f);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_max_arcmin") {
                        m_dcsvrFarDepth.maxArcmin = std::clamp(std::stof(value), 0.f, 10.f);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_inner_deg") {
                        m_dcsvrFarDepth.innerDeg = std::clamp(std::stof(value), 0.f, 60.f);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_outer_deg") {
                        m_dcsvrFarDepth.outerDeg = std::clamp(std::stof(value), 0.f, 90.f);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_saccade_step") {
                        m_dcsvrFarDepth.saccadeStep = std::clamp(std::stof(value), 0.f, 1.f);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_drift_per_s") {
                        m_dcsvrFarDepth.driftPerS = std::clamp(std::stof(value), 0.f, 1.f);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_gating") {
                        // gated: the gain changes only where a change cannot be seen; immediate: live A/B only.
                        const std::string gating = dcsvrSettingWord(value);
                        if (gating == "gated" || gating == "1") {
                            m_dcsvrFarDepth.gated = true;
                        } else if (gating == "immediate" || gating == "0") {
                            m_dcsvrFarDepth.gated = false;
                        } else {
                            throw std::invalid_argument(value);
                        }
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_hold_frames") {
                        m_dcsvrFarDepth.holdFrames = std::clamp(std::stoi(value), 0, 8);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_allow_unmasked") {
                        // Live A/B only: depth without the engine's overlay coverage moves HUD symbology and glass.
                        m_dcsvrFarDepth.requireCoverage = !std::stoi(value);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_coverage") {
                        // Live A/B only: 0 ignores the engine's pre-overlay copy (no CoverageMain).
                        m_dcsvrCoverageOn = std::stoi(value);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_toggle") {
                        // virtual-key:modifiers in decimal (1 Ctrl, 2 Alt, 4 Shift), 0:0 none.
                        const auto colon = value.find(':');
                        m_dcsvrFarKeyVk = std::clamp(std::stoi(value.substr(0, colon)), 0, 255);
                        m_dcsvrFarKeyMods = colon == std::string::npos ? 0 : std::clamp(std::stoi(value.substr(colon + 1)), 0, 7);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_coverage_dump") {
                        // Diagnostics: once, about 10 s after the coverage starts, the left eye's pre-overlay copy, the
                        // submitted focus image and the coverage mask as BMPs next to settings.cfg (one stall, logged).
                        m_dcsvrCoverageDump = m_dcsvrCoverageDumpSetting = std::stoi(value);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_coverage_sample") {
                        // Frames between two diagnostic samples of the coverage (0: none, and no automatic encoding).
                        m_dcsvrCoverageSampleFrames = std::clamp(std::stoi(value), 0, 100000);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_mode") {
                        // Only the fused composition is built into the layer; the compute fallback (WarpMain) stays in
                        // the offline test until the fused route is rejected live.
                        const std::string mode = dcsvrSettingWord(value);
                        if (mode != "fused" && mode != "0" && mode != "compute" && mode != "1") {
                            throw std::invalid_argument(value);
                        }
                        if (mode == "compute" || mode == "1") {
                            Log("DCSVR Far depth: the compute fallback is not built into this layer, the fused composition is used\n");
                        }
                        m_dcsvrFarDepth.mode = dcsvr::FarDepthMode::Fused;
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_debug") {
                        m_dcsvrFarDepth.debug = std::stoi(value);
                        parsed = true;
                    } else if (name == "dcsvr_far_depth_debug_scale") {
                        m_dcsvrFarDepth.debugScale = std::clamp(std::stof(value), 1.f, 16.f);
                        parsed = true;
                    } else if (name == "dcsvr_temporal") {
'@
    $session = @'
                            initializeDeviceContext(d3dBindings->device);
                            // DCSVR Far depth: pushes must come from this device (or from the one its textures report
                            // behind a proxy such as ReShade's), and only with Far depth enabled is the engine asked to
                            // copy any depth. Off: the engine's DcsVr_QuadViewsDepthWanted() stays 0 and it copies nothing.
                            if (m_dcsvrFarDepth.enabled) {
                                dcsvr::SharedDepthImport().SetDevice(d3dBindings->device);
                                dcsvr::SharedDepthImport().SetWanted(dcsvr::DepthImport::WantedFocus);
                                Log("DCSVR Far depth: asking the engine for the depth of both focus views\n");
                            }
'@
    $frameCall = @'
                            // DCSVR Far depth: the engine's depth for both eyes or neither, the gain, both eyes' fields.
                            dcsvrFarDepthFrame(proj, frameEndInfo->displayTime);

                            for (uint32_t viewIndex = 0; viewIndex < xr::StereoView::Count; viewIndex++) {
'@
    $functions = @'
        // DCSVR Far depth: one eye's displacement field (R16G16_FLOAT, focus uv, the depth viewport's size), kept until
        // the next one is built so that a frame without depth can reuse it, with the eye orientation and focus frustum
        // it was built for.
        struct DcsvrField {
            ComPtr<ID3D11Texture2D> image;
            ComPtr<ID3D11ShaderResourceView> srv;
            ComPtr<ID3D11UnorderedAccessView> uav;
            // CoverageMain's output on the same grid (R8_UNORM), when the coverage pass exists.
            ComPtr<ID3D11Texture2D> coverage;
            ComPtr<ID3D11ShaderResourceView> coverageSrv;
            ComPtr<ID3D11UnorderedAccessView> coverageUav;
            bool held{false};
            dcsvr::Quat eye{};
            dcsvr::TanFov focus{};
            float reuse[3][4]{};  // this frame's focus NDC to the held field's (dcsvr::ReuseHomography)
        };

        // DCSVR Far depth: the views the field pass reads of one pushed texture. The engine alternates two slots per
        // eye, so they are kept rather than created every frame; a view holds its texture, so an entry unused for a few
        // frames is dropped (dcsvrFarDepthFrame) and a released generation does not stay alive.
        struct DcsvrDepthViews {
            ID3D11Texture2D* texture{nullptr};  // identity only: the views hold the references
            ID3D11Texture2D* preOverlayTexture{nullptr};
            uint32_t preOverlayFormat{0};
            ComPtr<ID3D11ShaderResourceView> depth, stencil, preOverlay;
            // The copy in the format CoverageMain reads it with (m_dcsvrCoverageEncoding), made on first use.
            DXGI_FORMAT coverageFormat{DXGI_FORMAT_UNKNOWN};
            ComPtr<ID3D11ShaderResourceView> coverageView;
            uint64_t used{0};
        };

        // A settings word without surrounding whitespace, in lower case.
        static std::string dcsvrSettingWord(std::string value) {
            while (!value.empty() && std::isspace((unsigned char)value.back())) {
                value.pop_back();
            }
            while (!value.empty() && std::isspace((unsigned char)value.front())) {
                value.erase(value.begin());
            }
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return value;
        }

        // DCSVR Far depth (native/quadviews/dcsvr_depthwarp.h, docs/FAR_DEPTH.md), once per submitted frame and before
        // either eye is composited; another projection layer of the same display time gets the same answer. The
        // engine's depth is taken for both eyes or not at all (dcsvr::DepthImport::Validate), the controller decides the
        // gain and whether the held fields bridge a frame without depth, and both eyes' fields are built here, so one
        // eye can never be warped alone: a field that cannot be built stops the warp for both. Sets m_dcsvrFarWarp for
        // compositeViewContent. Disabled, an alpha-blended layer or the focus debug view: nothing is bound, and the
        // composition's Far depth constants stay 0.
        void dcsvrFarDepthFrame(const XrCompositionLayerProjection* proj, XrTime displayTime) {
            m_dcsvrFarWarp = false;
            if (!m_dcsvrFarDepth.enabled || !m_applicationDevice) {
                return;
            }
            m_dcsvrCoverageThisFrame = false;
            if (displayTime != m_dcsvrFarTime) {
                dcsvrFarDepthKey();
            }
            const bool enabled = !m_debugFocusView && !(proj->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) && !m_dcsvrFarKeyOff;
            if (displayTime == m_dcsvrFarTime) {
                m_dcsvrFarWarp = m_dcsvrFarFrameWarp && enabled;
                return;
            }
            m_dcsvrFarTime = displayTime;
            m_dcsvrFarFrameWarp = false;
            m_dcsvrFarFrames++;
            dcsvr::DepthImport& depthImport = dcsvr::SharedDepthImport();
            // The thread that submits frames is DCS's render thread: pushes from any other thread are refused from now on.
            depthImport.SetRenderThread(GetCurrentThreadId());
            if (!m_projectionPS) {
                initializeCompositionResources(m_applicationDevice.Get());
            }
            m_dcsvrDepthViews.erase(std::remove_if(m_dcsvrDepthViews.begin(),
                                                   m_dcsvrDepthViews.end(),
                                                   [&](const DcsvrDepthViews& views) { return views.used + 8 < m_dcsvrFarFrames; }),
                                    m_dcsvrDepthViews.end());

            // This frame's eyes: the orientation DCS rendered with and the focus frustum Quad Views handed it (for DCS,
            // the quirk's record of this display time, as the composition uses).
            dcsvr::Quat eyes[xr::StereoView::Count];
            dcsvr::TanFov focus[xr::StereoView::Count];
            for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                const XrQuaternionf& orientation = proj->views[eye].pose.orientation;
                eyes[eye] = {orientation.x, orientation.y, orientation.z, orientation.w};
                XrFovf fov = proj->views[xr::StereoView::Count + eye].fov;
                if (m_needFocusFovCorrectionQuirk) {
                    std::unique_lock lock(m_focusFovMutex);
                    const auto it = m_focusFovForDisplayTime.find(displayTime);
                    if (it != m_focusFovForDisplayTime.cend()) {
                        fov = eye == xr::StereoView::Left ? it->second.first : it->second.second;
                    }
                }
                focus[eye] = {std::tan(fov.angleLeft), std::tan(fov.angleRight), std::tan(fov.angleUp), std::tan(fov.angleDown)};
            }

            dcsvr::DepthFrame depth{};
            if (enabled) {
                const XrExtent2Di& extent = proj->views[xr::QuadView::FocusLeft].subImage.imageRect.extent;
                depth = depthImport.Validate(displayTime,
                                             extent.height > 0 ? (float)extent.width / (float)extent.height : 0.f,
                                             m_dcsvrFarDepth.requireCoverage);
            }
            const bool depthValid = enabled && depth.reject == dcsvr::DepthReject::None && depth.view[0] && depth.view[1];
            if (depthValid) {
                // The unjittered centre follows every frame with depth, warped or not; Quad Views' focus frustum for
                // this frame is the reference (DCS renders exactly that frustum, plus DLSS's jitter).
                for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                    const DcsVrDepthView& view = depth.view[eye]->desc;
                    m_dcsvrFarJitter[eye].Add(depth.proj[eye],
                                              dcsvr::JitterCentreEstimator::Reference::FromTangents(
                                                  focus[eye].left, focus[eye].right, focus[eye].up, focus[eye].down),
                                              2.f / view.viewport[2],
                                              2.f / view.viewport[3]);
                }
            }
            // The held fields bridge this frame only if both eyes' changes since are small enough to reproject.
            bool reusable = true;
            for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                DcsvrField& field = m_dcsvrField[eye];
                reusable = reusable && field.held && field.srv &&
                           dcsvr::ReuseHomography(field.eye, field.focus, eyes[eye], focus[eye], field.reuse);
            }
            const dcsvr::GazeSample gaze = m_dcsvrGazeHistory.At(displayTime);
            dcsvr::DisparityFrameInput input;
            input.displayTime = displayTime;
            input.enabled = enabled;
            input.depthValid = depthValid;
            input.fieldReusable = reusable;
            input.tracker = m_trackerType != Tracker::None;
            input.gazeValid = gaze.valid;
            input.inSaccade = gaze.valid && gaze.inSaccade;
            const dcsvr::GazeEventCounters& events = dcsvr::GazeEvents();
            input.externalEvents = events.any.load(std::memory_order_relaxed) != 0;
            input.saccadeEvents = events.saccades.load(std::memory_order_relaxed);
            input.blinkEvents = events.blinks.load(std::memory_order_relaxed);
            // A switch back on counts as a scene cut: the full effect at once, as the A/B asks.
            input.sceneCuts = dcsvr::SceneCuts().load(std::memory_order_relaxed) + m_dcsvrFarToggles;
            m_dcsvrFarDecision = m_dcsvrFarControl.BeginFrame(input, m_dcsvrFarDepth);

            bool warp = m_dcsvrFarDecision.warp && m_dcsvrFieldCS && m_dcsvrFarConstants;
            if (warp && !m_dcsvrFarDecision.reuse) {
                // Both eyes' views and textures first, then both dispatches: all or nothing.
                DcsvrDepthViews views[xr::StereoView::Count];
                DcsvrCoverageInput coverage[xr::StereoView::Count];
                warp = depthValid;
                for (uint32_t eye = 0; eye < xr::StereoView::Count && warp; eye++) {
                    const DcsVrDepthView& view = depth.view[eye]->desc;
                    warp = dcsvrDepthViews(*depth.view[eye], views[eye]) &&
                           dcsvrFieldTexture(eye, (UINT)std::lround(view.viewport[2]), (UINT)std::lround(view.viewport[3]));
                    if (warp && views[eye].preOverlay) {
                        dcsvrCoverageInput(proj, eye, *depth.view[eye], coverage[eye]);
                    }
                }
                // Both eyes' coverage or neither; without it, Far depth only runs when allowed (allow_unmasked).
                const bool covered = warp && coverage[0].use && coverage[1].use;
                if (warp && depth.preOverlay && !covered) {
                    m_dcsvrCoverageStats.missing++;
                }
                if (!covered) {
                    coverage[0].use = coverage[1].use = false;
                    warp = warp && !m_dcsvrFarDepth.requireCoverage;
                }
                if (warp) {
                    dcsvrBuildFields(proj, displayTime, depth, views, coverage, eyes, focus, gaze);
                }
            } else if (warp) {
                // Reused: the held fields stay as built, the composition reprojects them (DcsvrField::reuse).
                warp = m_dcsvrField[xr::StereoView::Left].srv && m_dcsvrField[xr::StereoView::Right].srv;
            }
            if (!warp) {
                for (DcsvrField& field : m_dcsvrField) {
                    field.held = false;
                }
            }
            m_dcsvrFarFrameWarp = warp;
            m_dcsvrFarWarp = warp && enabled;
            dcsvrFarDepthStatus(warp, depthValid);

            if (!m_dcsvrFarLogged) {
                Log(fmt::format("DCSVR Far depth: on, {}, fade {:.0f}-{:.0f} m, at most {:.1f} arcmin per eye, {} gain, "
                                "hold {} frames, {}{}\n",
                                m_dcsvrFarDepth.worldIpdScale > 1.f
                                    ? fmt::format("near: the world at x{:.2f} the eye distance from {:.0f}-{:.0f} m, the cockpit untouched",
                                                  m_dcsvrFarDepth.worldIpdScale, m_dcsvrFarDepth.nearGuardM, 3.f * m_dcsvrFarDepth.nearGuardM)
                                    : fmt::format("gamma {:.2f}, Z0 {:.0f} m", m_dcsvrFarDepth.gamma, m_dcsvrFarDepth.z0M),
                                m_dcsvrFarDepth.fadeStartM, m_dcsvrFarDepth.fadeEndM,
                                m_dcsvrFarDepth.maxArcmin, m_dcsvrFarDepth.gated ? "gated" : "immediate", m_dcsvrFarDepth.holdFrames,
                                m_dcsvrFarDepth.requireCoverage ? "overlay coverage required" : "overlay coverage not required (A/B)",
                                m_dcsvrFarDepth.debug == 3 ? ", debug exaggeration" : ""));
                m_dcsvrFarLogged = true;
            }
            const auto statsNow = std::chrono::steady_clock::now();
            if (m_dcsvrFarStatsAt.time_since_epoch().count() == 0) m_dcsvrFarStatsAt = statsNow;
            const dcsvr::DisparityStats& stats = m_dcsvrFarControl.Stats();
            if (statsNow - m_dcsvrFarStatsAt >= std::chrono::seconds(30) && stats.frames > 0) {
                using Reject = dcsvr::DepthReject;
                Log(fmt::format("DCSVR Far depth: {} frames, {} warped ({} held), skipped {} without depth and {} at gain 0, gain {:.2f}, "
                                "steps saccade {} blink {} cut {}; engine since start: {} pushes accepted, frames rejected missing {} "
                                "frame {} generation {} flags {} projection {} aspect {} time {} swap {} viewport {} coverage {}; overlay coverage {} frames, "
                                "{:.2f}%% of the focus view drawn by overlays ({} samples, {} above {:.0f}%%), {} frames without it, copy read {}\n",
                                stats.frames, stats.warped, stats.reused, stats.skipDepth, stats.skipGain, m_dcsvrFarControl.Gain(),
                                stats.saccadeSteps, stats.blinkSteps, stats.cutSteps, depthImport.Accepted(),
                                depthImport.Rejects(Reject::Missing), depthImport.Rejects(Reject::Frame),
                                depthImport.Rejects(Reject::Generation), depthImport.Rejects(Reject::Flags),
                                depthImport.Rejects(Reject::Projection), depthImport.Rejects(Reject::Aspect),
                                depthImport.Rejects(Reject::DisplayTime), depthImport.Rejects(Reject::Swap),
                                depthImport.Rejects(Reject::Viewport), depthImport.Rejects(Reject::Coverage), m_dcsvrCoverageStats.frames,
                                m_dcsvrCoverageStats.measured ? 100.0 * m_dcsvrCoverageStats.share / (double)m_dcsvrCoverageStats.measured : 0.0,
                                m_dcsvrCoverageStats.measured, m_dcsvrCoverageStats.suspect, 100.0 * m_dcsvrFarDepth.overlaySuspectShare,
                                m_dcsvrCoverageStats.missing, m_dcsvrCoverageEncoding ? "as its target" : "as the submitted image"));
                m_dcsvrCoverageStats = {};
                m_dcsvrFarControl.ResetStats();
                m_dcsvrFarStatsAt = statsNow;
            }
        }

        // DCSVR Far depth: the in-flight switch, polled once per frame while Far depth is enabled: one low beep off, two
        // high on (Beep blocks: on its own thread, as the pupil shift layer does).
        void dcsvrFarDepthKey() {
            if (!m_dcsvrFarKeyVk) {
                m_dcsvrFarKeyDown = false;
                return;
            }
            const auto held = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
            const int mods = (held(VK_CONTROL) ? 1 : 0) | (held(VK_MENU) ? 2 : 0) | (held(VK_SHIFT) ? 4 : 0);
            // The key's own press, with the modifiers held at that moment, and at most one switch per 300 ms. Modifier
            // changes while the key is held do not count: Windows' Alt+Shift (input language) flickers the modifier
            // state, which used to read as a second press and switched straight back.
            const bool down = held(m_dcsvrFarKeyVk);
            const auto now = std::chrono::steady_clock::now();
            if (down && !m_dcsvrFarKeyDown && mods == m_dcsvrFarKeyMods && now - m_dcsvrFarKeyAt >= std::chrono::milliseconds(300)) {
                m_dcsvrFarKeyAt = now;
                m_dcsvrFarKeyOff = !m_dcsvrFarKeyOff;
                if (!m_dcsvrFarKeyOff) {
                    m_dcsvrFarToggles++;
                    // The diagnostic dump, when set, fires again on the first frame with coverage 90 frames after each switch
                    // back on (about 2 s).
                    if (m_dcsvrCoverageDumpSetting) {
                        m_dcsvrCoverageDump = true;
                        m_dcsvrCoverageDumpAt = m_dcsvrFarFrames + 90;
                    }
                }
                Log(fmt::format("DCSVR Far depth: switched {} by the key\n", m_dcsvrFarKeyOff ? "off" : "on"));
                const uintptr_t tone = m_dcsvrFarKeyOff ? (500u | (1u << 16)) : (1100u | (2u << 16));
                if (HANDLE thread = CreateThread(nullptr, 0, [](LPVOID parameter) -> DWORD {
                        const auto packed = reinterpret_cast<uintptr_t>(parameter);
                        for (uintptr_t i = 0; i < (packed >> 16); i++) {
                            if (i) Sleep(70);
                            Beep((DWORD)(packed & 0xFFFF), 80);
                        }
                        return 0;
                    }, reinterpret_cast<LPVOID>(tone), 0, nullptr)) {
                    CloseHandle(thread);
                }
            }
            m_dcsvrFarKeyDown = down;
        }

        // DCSVR Far depth: the extended panel status (DcsVr_QuadViewsDisparityStatus2), once per frame.
        void dcsvrFarDepthStatus(bool warped, bool depthValid) {
            const auto now = std::chrono::steady_clock::now();
            if (m_dcsvrFarSecondAt.time_since_epoch().count() == 0) m_dcsvrFarSecondAt = now;
            if (warped) {
                m_dcsvrFarWarpedCount++;
                if (m_dcsvrFarDecision.reuse) m_dcsvrFarHeldCount++;
            }
            if (now - m_dcsvrFarSecondAt >= std::chrono::seconds(1)) {
                m_dcsvrFarWarpedLast = m_dcsvrFarWarpedCount;
                m_dcsvrFarHeldLast = m_dcsvrFarHeldCount;
                m_dcsvrFarWarpedCount = m_dcsvrFarHeldCount = 0;
                m_dcsvrFarSecondAt = now;
            }
            if (m_dcsvrCoverageThisFrame) m_dcsvrCoverageLastFrame = m_dcsvrFarFrames;
            // The coverage share is known while coverage ran within the last 90 frames.
            const bool coverageRecent = m_dcsvrCoverageLastFrame && m_dcsvrFarFrames < m_dcsvrCoverageLastFrame + 90;
            dcsvr::DisparityStatusWord status;
            status.on = !m_dcsvrFarKeyOff;
            status.enabled = true;
            status.depthValid = depthValid;
            status.held = warped && m_dcsvrFarDecision.reuse;
            status.offByKey = m_dcsvrFarKeyOff;
            status.coverage = m_dcsvrCoverageThisFrame;
            status.gain = m_dcsvrFarDecision.gain;
            status.warpedPerSecond = m_dcsvrFarWarpedLast;
            status.heldPerSecond = m_dcsvrFarHeldLast;
            status.coveragePercent = coverageRecent ? m_dcsvrCoveragePercent : 255u;
            dcsvr::PublishDisparityStatus2(status);
        }

        // DCSVR Far depth: the views of one pushed view's textures (depth, stencil when the format has one, the
        // pre-overlay copy when pushed, in the render target's view format), from the cache or created. False when one
        // cannot be created: no warp this frame.
        bool dcsvrDepthViews(const dcsvr::DepthViewLatch& latch, DcsvrDepthViews& out) {
            ID3D11Texture2D* texture = latch.texture.Get();
            ID3D11Texture2D* preOverlay = latch.preOverlay.Get();
            const uint32_t preOverlayFormat = preOverlay ? latch.desc.preOverlayFormat : 0u;
            for (DcsvrDepthViews& cached : m_dcsvrDepthViews) {
                if (cached.texture == texture && cached.preOverlayTexture == preOverlay && cached.preOverlayFormat == preOverlayFormat) {
                    cached.used = m_dcsvrFarFrames;
                    out = cached;
                    return true;
                }
            }
            DcsvrDepthViews views;
            views.texture = texture;
            views.preOverlayTexture = preOverlay;
            views.preOverlayFormat = preOverlayFormat;
            views.used = m_dcsvrFarFrames;
            DXGI_FORMAT depthFormat, stencilFormat;
            dcsvr::DepthSrvFormats(latch.desc.format, depthFormat, stencilFormat);
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipLevels = 1;
            desc.Format = depthFormat;
            if (!texture || FAILED(m_applicationDevice->CreateShaderResourceView(texture, &desc, views.depth.ReleaseAndGetAddressOf()))) {
                return false;
            }
            if (stencilFormat != DXGI_FORMAT_UNKNOWN) {
                desc.Format = stencilFormat;
                if (FAILED(m_applicationDevice->CreateShaderResourceView(texture, &desc, views.stencil.ReleaseAndGetAddressOf()))) {
                    return false;
                }
            }
            if (preOverlay) {
                desc.Format = (DXGI_FORMAT)preOverlayFormat;
                if (FAILED(m_applicationDevice->CreateShaderResourceView(preOverlay, &desc, views.preOverlay.ReleaseAndGetAddressOf()))) {
                    return false;
                }
            }
            m_dcsvrDepthViews.push_back(views);
            out = views;
            return true;
        }

        // DCSVR Far depth: an eye's field texture at the depth viewport's size, recreated when that changes. Without the
        // memory for it (VRAM at its limit) the frame goes on without the warp: never an error returned to DCS.
        bool dcsvrFieldTexture(uint32_t eye, UINT width, UINT height) {
            DcsvrField& field = m_dcsvrField[eye];
            D3D11_TEXTURE2D_DESC desc{};
            if (field.image) {
                field.image->GetDesc(&desc);
                if (field.srv && field.uav && desc.Width == width && desc.Height == height &&
                    (!m_dcsvrCoverageCS || field.coverageUav)) {
                    return true;
                }
            }
            field = {};
            desc = {};
            desc.Width = width;
            desc.Height = height;
            desc.ArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R16G16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (width && height && SUCCEEDED(m_applicationDevice->CreateTexture2D(&desc, nullptr, field.image.ReleaseAndGetAddressOf())) &&
                SUCCEEDED(m_applicationDevice->CreateShaderResourceView(field.image.Get(), nullptr, field.srv.ReleaseAndGetAddressOf())) &&
                SUCCEEDED(m_applicationDevice->CreateUnorderedAccessView(field.image.Get(), nullptr, field.uav.ReleaseAndGetAddressOf()))) {
                if (!m_dcsvrCoverageCS) {
                    return true;
                }
                desc.Format = DXGI_FORMAT_R8_UNORM;
                if (SUCCEEDED(m_applicationDevice->CreateTexture2D(&desc, nullptr, field.coverage.ReleaseAndGetAddressOf())) &&
                    SUCCEEDED(m_applicationDevice->CreateShaderResourceView(field.coverage.Get(), nullptr, field.coverageSrv.ReleaseAndGetAddressOf())) &&
                    SUCCEEDED(m_applicationDevice->CreateUnorderedAccessView(field.coverage.Get(), nullptr, field.coverageUav.ReleaseAndGetAddressOf()))) {
                    return true;
                }
            }
            field = {};
            if (!m_dcsvrFarNoMemoryLogged) {
                Log(fmt::format("DCSVR Far depth: no memory for a {}x{} field, frames go on without the warp\n", width, height));
                m_dcsvrFarNoMemoryLogged = true;
            }
            return false;
        }

        // DCSVR Far depth: one eye's overlay coverage inputs: the engine's pre-overlay copy, the focus swapchain image
        // DCS submitted for this frame and CoverageMain's constants. use stays false when the two are not one pixel grid
        // (logged once) or the submitted image cannot be read as a Texture2D (an array slice, MSAA, another format).
        struct DcsvrCoverageInput {
            bool use{false};
            ComPtr<ID3D11ShaderResourceView> preOverlay, submitted;
            dcsvr::CoverageConstants constants{};
        };
        void dcsvrCoverageInput(const XrCompositionLayerProjection* proj, uint32_t eye, const dcsvr::DepthViewLatch& latch, DcsvrCoverageInput& out) {
            out = {};
            if (!m_dcsvrCoverageOn || !m_dcsvrCoverageCS || !m_dcsvrCoverageConstants || !m_dcsvrField[eye].coverageUav || !latch.preOverlay) {
                return;
            }
            // The engine's copy in one view format, cached with the pushed texture's other views.
            const auto copyView = [&](DXGI_FORMAT readAs) -> ComPtr<ID3D11ShaderResourceView> {
                for (DcsvrDepthViews& cached : m_dcsvrDepthViews) {
                    if (cached.texture == latch.texture.Get() && cached.preOverlayTexture == latch.preOverlay.Get() &&
                        cached.preOverlayFormat == latch.desc.preOverlayFormat) {
                        if (!cached.coverageView || cached.coverageFormat != readAs) {
                            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                            srvDesc.Format = readAs;
                            srvDesc.Texture2D.MipLevels = 1;
                            cached.coverageFormat = readAs;
                            if (FAILED(m_applicationDevice->CreateShaderResourceView(latch.preOverlay.Get(), &srvDesc,
                                                                                     cached.coverageView.ReleaseAndGetAddressOf()))) {
                                cached.coverageView.Reset();
                            }
                        }
                        return cached.coverageView;
                    }
                }
                return nullptr;
            };
            const uint32_t w = latch.desc.preOverlaySize & 0xffffu, h = latch.desc.preOverlaySize >> 16;
            D3D11_TEXTURE2D_DESC fieldDesc{};
            m_dcsvrField[eye].image->GetDesc(&fieldDesc);
            if (latch.desc.flags & DCSVR_DEPTH_PRE_POST_OVERLAY) {
                // Both copies in one texture (before at (0, 0), after at (w, 0)): one target, one format, and
                // CopyToHMDPass, whatever it converts, out of the comparison.
                out.preOverlay = out.submitted = copyView((DXGI_FORMAT)latch.desc.preOverlayFormat);
                out.use = out.preOverlay && dcsvr::MakeCoverageConstants(fieldDesc.Width, fieldDesc.Height, w, h, (int32_t)w, 0, (int32_t)w,
                                                                         (int32_t)h, m_dcsvrFarDepth.overlayThreshold, out.constants);
                if (dcsvr::CoverageFormatIsHdr(latch.desc.preOverlayFormat)) {
                    dcsvr::SetCoverageHdr(out.constants);
                }
                if (!m_dcsvrCoverageGridLogged) {
                    Log(fmt::format("DCSVR Far depth: overlay coverage {}: the engine's copies before and after the overlays, {}x{} each "
                                    "(target view {}, {}), field {}x{}\n",
                                    out.use ? "on" : "off", w, h, latch.desc.preOverlayFormat,
                                    dcsvr::CoverageFormatIsHdr(latch.desc.preOverlayFormat) ? "HDR, relative change" : "sRGB steps",
                                    fieldDesc.Width, fieldDesc.Height));
                    m_dcsvrCoverageGridLogged = true;
                }
                if (!out.use) {
                    out.preOverlay.Reset();
                    out.submitted.Reset();
                }
                return;
            }
            const XrCompositionLayerProjectionView& view = proj->views[xr::StereoView::Count + eye];
            ID3D11Texture2D* image = nullptr;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            {
                std::unique_lock lock(m_swapchainsMutex);
                const auto it = m_swapchains.find(view.subImage.swapchain);
                if (it == m_swapchains.end()) {
                    return;
                }
                Swapchain& swapchain = it->second;
                if (swapchain.images.empty()) {
                    uint32_t count = 0;
                    if (XR_FAILED(OpenXrApi::xrEnumerateSwapchainImages(view.subImage.swapchain, 0, &count, nullptr)) || !count) {
                        return;
                    }
                    std::vector<XrSwapchainImageD3D11KHR> d3d11Images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                    if (XR_FAILED(OpenXrApi::xrEnumerateSwapchainImages(
                            view.subImage.swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(d3d11Images.data())))) {
                        return;
                    }
                    for (uint32_t i = 0; i < count; i++) {
                        swapchain.images.push_back(d3d11Images[i].texture);
                    }
                }
                if (swapchain.lastReleasedIndex >= swapchain.images.size()) {
                    return;
                }
                image = swapchain.images[swapchain.lastReleasedIndex];
                format = (DXGI_FORMAT)swapchain.createInfo.format;
                D3D11_TEXTURE2D_DESC desc{};
                image->GetDesc(&desc);
                if (desc.ArraySize != 1 || view.subImage.imageArrayIndex != 0 || desc.SampleDesc.Count != 1 ||
                    !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) || !dcsvr::PreOverlayViewFormatSupported(format)) {
                    return;
                }
                // The same view the flattening caches for this image (a Texture2D in the swapchain's format).
                auto& cached = swapchain.dcsvrSourceSrv[image];
                if (!cached) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Format = format;
                    srvDesc.Texture2D.MipLevels = 1;
                    if (FAILED(m_applicationDevice->CreateShaderResourceView(image, &srvDesc, cached.ReleaseAndGetAddressOf()))) {
                        return;
                    }
                }
                out.submitted = cached;
            }
            // The copy read like the submitted image (encoding 0, when its texture can be viewed so) or as DCS's target.
            D3D11_TEXTURE2D_DESC preDesc{};
            latch.preOverlay->GetDesc(&preDesc);
            const DXGI_FORMAT readAs = m_dcsvrCoverageEncoding == 0 && dcsvr::PreOverlayViewCompatible(preDesc.Format, format)
                                           ? format
                                           : (DXGI_FORMAT)latch.desc.preOverlayFormat;
            out.preOverlay = copyView(readAs);
            if (!out.preOverlay) {
                out.submitted.Reset();
                return;
            }
            const XrRect2Di& rect = view.subImage.imageRect;
            out.use = dcsvr::MakeCoverageConstants(fieldDesc.Width, fieldDesc.Height, w, h, rect.offset.x, rect.offset.y, rect.extent.width,
                                                   rect.extent.height, m_dcsvrFarDepth.overlayThreshold, out.constants);
            if (!m_dcsvrCoverageGridLogged) {
                Log(fmt::format("DCSVR Far depth: overlay coverage {}: the engine's copy {}x{} (texture format {}, target view {}, read as {}), "
                                "the submitted focus image {}x{} at {},{} (format {}), field {}x{}\n",
                                out.use ? "on" : "off, not one pixel grid", w, h, (uint32_t)preDesc.Format, latch.desc.preOverlayFormat,
                                (uint32_t)readAs, rect.extent.width, rect.extent.height, rect.offset.x, rect.offset.y, (uint32_t)format,
                                fieldDesc.Width, fieldDesc.Height));
                m_dcsvrCoverageGridLogged = true;
            }
            if (!out.use) {
                out.submitted.Reset();
            }
        }

        // DCSVR Far depth: the coverage diagnostics, one sample at a time, read at least 6 frames after its copy without
        // waiting (not ready: tried again 6 frames later). Its share drives the statistics and the encoding flip.
        void dcsvrCoverageReadback() {
            if (!m_dcsvrCoverageStagingTexels || !m_dcsvrCoverageStaging || m_dcsvrFarFrames < m_dcsvrCoverageCopyFrame + 6) {
                return;
            }
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (m_renderContext->Map(m_dcsvrCoverageStaging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK) {
                m_dcsvrCoverageCopyFrame = m_dcsvrFarFrames;
                return;
            }
            const double share = (double)*static_cast<const uint32_t*>(mapped.pData) / (double)m_dcsvrCoverageStagingTexels;
            m_renderContext->Unmap(m_dcsvrCoverageStaging.Get(), 0);
            m_dcsvrCoverageStagingTexels = 0;
            m_dcsvrCoverageStats.measured++;
            m_dcsvrCoverageStats.share += share;
            m_dcsvrCoveragePercent = (uint32_t)std::lround(std::clamp(share, 0.0, 1.0) * 100.0);
            if (share > m_dcsvrFarDepth.overlaySuspectShare) {
                m_dcsvrCoverageStats.suspect++;
            }
            // Nearly every texel changed, three samples in a row: read the copy the other way. A view really covered by
            // overlays (a full-screen menu) pins everything either way, so flipping there costs nothing.
            m_dcsvrCoverageSuspectRun = share >= 0.9 ? m_dcsvrCoverageSuspectRun + 1 : 0;
            if (m_dcsvrCoverageSuspectRun >= 3) {
                m_dcsvrCoverageSuspectRun = 0;
                m_dcsvrCoverageEncoding ^= 1u;
                if (++m_dcsvrCoverageFlips <= 4) {
                    Log(fmt::format("DCSVR Far depth: the overlays changed {:.0f}%% of the focus view three samples in a row: the engine's copy is now "
                                    "read {}\n",
                                    100.0 * share,
                                    m_dcsvrCoverageEncoding ? "in its render target's format" : "in the submitted image's format"));
                }
            }
        }

        // DCSVR Far depth diagnostics: one texture region as a 24-bit BMP. 8-bit RGBA/BGRA families as colour, R8 as grey;
        // other formats are skipped (logged). Waits for the GPU: only for the one-shot dump.
        bool dcsvrDumpBmp(ID3D11Texture2D* texture, UINT x, UINT y, UINT w, UINT h, const std::filesystem::path& path) {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            const DXGI_FORMAT f = desc.Format;
            const bool rgba = f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            const bool bgra = f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                              f == DXGI_FORMAT_B8G8R8X8_TYPELESS || f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
            const bool grey = f == DXGI_FORMAT_R8_UNORM || f == DXGI_FORMAT_R8_TYPELESS;
            if ((!rgba && !bgra && !grey) || desc.SampleDesc.Count != 1 || x + w > desc.Width || y + h > desc.Height || !w || !h) {
                Log(fmt::format("DCSVR Far depth: coverage dump skipped {} (format {}, {}x{})\n", path.filename().string(), (uint32_t)f,
                                desc.Width, desc.Height));
                return false;
            }
            D3D11_TEXTURE2D_DESC sd = desc;
            sd.ArraySize = 1;
            sd.MipLevels = 1;
            sd.BindFlags = 0;
            sd.MiscFlags = 0;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(m_applicationDevice->CreateTexture2D(&sd, nullptr, staging.ReleaseAndGetAddressOf()))) {
                return false;
            }
            m_renderContext->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, 0, nullptr);
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (FAILED(m_renderContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                return false;
            }
            const UINT stride = (w * 3 + 3) & ~3u;
            std::vector<uint8_t> file(54 + (size_t)stride * h, 0);
            auto put32 = [&](size_t at, uint32_t v) { memcpy(&file[at], &v, 4); };
            file[0] = 'B';
            file[1] = 'M';
            put32(2, (uint32_t)file.size());
            put32(10, 54);
            put32(14, 40);
            put32(18, w);
            put32(22, h);
            file[26] = 1;
            file[28] = 24;
            put32(34, (uint32_t)(file.size() - 54));
            for (UINT row = 0; row < h; row++) {
                const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + (size_t)(y + row) * mapped.RowPitch;
                uint8_t* dst = &file[54 + (size_t)(h - 1 - row) * stride];
                for (UINT col = 0; col < w; col++) {
                    if (grey) {
                        const uint8_t v = src[x + col] ? 255 : 0;
                        dst[col * 3] = dst[col * 3 + 1] = dst[col * 3 + 2] = v;
                    } else {
                        const uint8_t* px = src + (size_t)(x + col) * 4;
                        dst[col * 3] = rgba ? px[2] : px[0];
                        dst[col * 3 + 1] = px[1];
                        dst[col * 3 + 2] = rgba ? px[0] : px[2];
                    }
                }
            }
            m_renderContext->Unmap(staging.Get(), 0);
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(file.data()), (std::streamsize)file.size());
            return out.good();
        }

        // DCSVR Far depth: FieldMain for both eyes (dcsvr_depthwarp_cs.hlsl) from this frame's projection and depth
        // viewport, the unjittered centre (the engine's jitterPx, else the estimate against Quad Views' focus frustum),
        // the rendered IPD and each eye's baseline, the gaze in each eye's frame (the focus centre without one) and the
        // gain.
        void dcsvrBuildFields(const XrCompositionLayerProjection* proj,
                              XrTime displayTime,
                              const dcsvr::DepthFrame& depth,
                              const DcsvrDepthViews (&views)[xr::StereoView::Count],
                              const DcsvrCoverageInput (&coverage)[xr::StereoView::Count],
                              const dcsvr::Quat (&eyes)[xr::StereoView::Count],
                              const dcsvr::TanFov (&focus)[xr::StereoView::Count],
                              const dcsvr::GazeSample& gaze) {
            // The rendered IPD in dcsvr::ChooseRenderedIpd's order: the pupil shift layer's push for this display time,
            // else the engine's camera separation, else the eye poses DCS submitted.
            const XrVector3f& left = proj->views[xr::StereoView::Left].pose.position;
            const XrVector3f& right = proj->views[xr::StereoView::Right].pose.position;
            const float between[3] = {right.x - left.x, right.y - left.y, right.z - left.z};
            const float located = std::sqrt(between[0] * between[0] + between[1] * between[1] + between[2] * between[2]);
            float camera = 0.f;
            const DcsVrDepthView& leftView = depth.view[xr::StereoView::Left]->desc;
            const DcsVrDepthView& rightView = depth.view[xr::StereoView::Right]->desc;
            if (leftView.flags & rightView.flags & DCSVR_DEPTH_VIEWINV_VALID) {
                const float dx = rightView.viewInv[12] - leftView.viewInv[12];
                const float dy = rightView.viewInv[13] - leftView.viewInv[13];
                const float dz = rightView.viewInv[14] - leftView.viewInv[14];
                camera = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            const dcsvr::RenderedEyes pushed = dcsvr::SharedRenderedEyes().Load();
            const bool pushedNow = pushed.displayTime != 0 && pushed.displayTime == displayTime;
            const float ipd = dcsvr::ChooseRenderedIpd(pushedNow ? pushed.ipdM : 0.f, camera, 1.f, located);

            // The overlay coverage of both eyes first (CoverageMain), counted into one buffer for the diagnostics.
            const bool covered = coverage[xr::StereoView::Left].use && coverage[xr::StereoView::Right].use;
            if (covered) {
                dcsvrCoverageReadback();
                const UINT zero[4] = {};
                if (m_dcsvrCoverageCountUav) {
                    m_renderContext->ClearUnorderedAccessViewUint(m_dcsvrCoverageCountUav.Get(), zero);
                }
                uint64_t texels = 0;
                for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                    const dcsvr::CoverageConstants& constants = coverage[eye].constants;
                    {
                        D3D11_MAPPED_SUBRESOURCE mappedResources;
                        CHECK_HRCMD(m_renderContext->Map(m_dcsvrCoverageConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResources));
                        memcpy(mappedResources.pData, &constants, sizeof(constants));
                        m_renderContext->Unmap(m_dcsvrCoverageConstants.Get(), 0);
                    }
                    ID3D11ShaderResourceView* nullPS[] = {nullptr, nullptr, nullptr};
                    m_renderContext->PSSetShaderResources(0, 3, nullPS);
                    ID3D11ShaderResourceView* sources[] = {coverage[eye].preOverlay.Get(), coverage[eye].submitted.Get()};
                    ID3D11UnorderedAccessView* targets[] = {m_dcsvrField[eye].coverageUav.Get(), m_dcsvrCoverageCountUav.Get()};
                    m_renderContext->CSSetConstantBuffers(2, 1, m_dcsvrCoverageConstants.GetAddressOf());
                    m_renderContext->CSSetShaderResources(3, 2, sources);
                    m_renderContext->CSSetUnorderedAccessViews(1, 2, targets, nullptr);
                    m_renderContext->CSSetShader(m_dcsvrCoverageCS.Get(), nullptr, 0);
                    m_renderContext->Dispatch(((UINT)constants.field[0] + 15) / 16, ((UINT)constants.field[1] + 15) / 16, 1);
                    ID3D11ShaderResourceView* nullSRV[] = {nullptr, nullptr};
                    ID3D11UnorderedAccessView* nullUAV[] = {nullptr, nullptr};
                    m_renderContext->CSSetUnorderedAccessViews(1, 2, nullUAV, nullptr);
                    m_renderContext->CSSetShaderResources(3, 2, nullSRV);
                    texels += (uint64_t)constants.field[0] * (uint64_t)constants.field[1];
                }
                m_dcsvrCoverageStats.frames++;
                // The diagnostic dump (dcsvr_far_depth_coverage_dump): 450 frames in, then 90 frames after each key-on.
                if (m_dcsvrCoverageDump && m_dcsvrFarFrames >= m_dcsvrCoverageDumpAt) {
                    m_dcsvrCoverageDump = false;
                    const dcsvr::CoverageConstants& k = coverage[xr::StereoView::Left].constants;
                    const UINT w = (UINT)k.image[2], h = (UINT)k.image[3];
                    ComPtr<ID3D11Resource> pre, submitted;
                    coverage[xr::StereoView::Left].preOverlay->GetResource(pre.ReleaseAndGetAddressOf());
                    coverage[xr::StereoView::Left].submitted->GetResource(submitted.ReleaseAndGetAddressOf());
                    ComPtr<ID3D11Texture2D> preTexture, submittedTexture;
                    pre.As(&preTexture);
                    submitted.As(&submittedTexture);
                    const bool a = preTexture && dcsvrDumpBmp(preTexture.Get(), 0, 0, w, h, localAppData / "dcsvr-coverage-pre-overlay.bmp");
                    const bool b = submittedTexture && dcsvrDumpBmp(submittedTexture.Get(), (UINT)k.image[0], (UINT)k.image[1], w, h,
                                                                    localAppData / "dcsvr-coverage-submitted.bmp");
                    const bool c = dcsvrDumpBmp(m_dcsvrField[xr::StereoView::Left].coverage.Get(), 0, 0, (UINT)k.field[0], (UINT)k.field[1],
                                                localAppData / "dcsvr-coverage-mask.bmp");
                    Log(fmt::format("DCSVR Far depth: coverage dump (pre-overlay {}, submitted {}, mask {}) in {}, read {}\n", a, b, c,
                                    localAppData.string(), m_dcsvrCoverageEncoding ? "as its target" : "as the submitted image"));
                }
                if (m_dcsvrCoverageSampleFrames > 0 && m_dcsvrCoverageCount && m_dcsvrCoverageStaging && !m_dcsvrCoverageStagingTexels &&
                    m_dcsvrFarFrames >= m_dcsvrCoverageCopyFrame + (uint64_t)m_dcsvrCoverageSampleFrames) {
                    m_renderContext->CopyResource(m_dcsvrCoverageStaging.Get(), m_dcsvrCoverageCount.Get());
                    m_dcsvrCoverageStagingTexels = texels;
                    m_dcsvrCoverageCopyFrame = m_dcsvrFarFrames;
                }
            }

            for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                const DcsVrDepthView& view = depth.view[eye]->desc;
                dcsvr::FieldInputs in;
                in.proj = depth.proj[eye];
                if (view.flags & DCSVR_DEPTH_JITTER_KNOWN) {
                    float dx, dy;
                    dcsvr::JitterNdc(view.jitterPx[0], view.jitterPx[1], view.viewport[2], view.viewport[3], dx, dy);
                    in.c0x = in.proj.cx - dx;
                    in.c0y = in.proj.cy - dy;
                } else {
                    m_dcsvrFarJitter[eye].Centre(in.c0x, in.c0y);
                }
                std::copy(view.viewport, view.viewport + 6, in.viewport);
                in.hasStencil = views[eye].stencil != nullptr;
                in.hasCoverage = covered;
                m_dcsvrCoverageThisFrame = covered;
                in.eye = (int)eye;
                in.ipdM = ipd;
                if (pushedNow) {
                    std::copy(pushed.baseline[eye], pushed.baseline[eye] + 3, in.baseline);
                } else {
                    // Toward the right eye, in this eye's own frame (canted displays turn each eye's frame).
                    dcsvr::Rotate(dcsvr::Conjugate(eyes[eye]), between, in.baseline);
                }
                in.gazeValid = gaze.valid;
                if (gaze.valid) {
                    std::copy(gaze.eye[eye], gaze.eye[eye] + 3, in.gaze);
                }
                in.gain = m_dcsvrFarDecision.gain;
                in.focusShape = m_focusViewShape;
                in.smoothing = m_smoothenFocusViewEdges;
                dcsvr::DisparityConstants constants;
                dcsvr::FieldConstants(in, m_dcsvrFarDepth, constants);
                {
                    D3D11_MAPPED_SUBRESOURCE mappedResources;
                    CHECK_HRCMD(m_renderContext->Map(m_dcsvrFarConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResources));
                    memcpy(mappedResources.pData, &constants, sizeof(constants));
                    m_renderContext->Unmap(m_dcsvrFarConstants.Get(), 0);
                }
                DcsvrField& field = m_dcsvrField[eye];
                ID3D11ShaderResourceView* nullSRV[] = {nullptr, nullptr, nullptr};
                m_renderContext->PSSetShaderResources(0, 3, nullSRV);
                ID3D11ShaderResourceView* sources[] = {views[eye].depth.Get(), views[eye].stencil.Get(),
                                                       covered ? m_dcsvrField[eye].coverageSrv.Get() : nullptr};
                m_renderContext->CSSetConstantBuffers(0, 1, m_dcsvrFarConstants.GetAddressOf());
                m_renderContext->CSSetShaderResources(0, 3, sources);
                m_renderContext->CSSetUnorderedAccessViews(0, 1, field.uav.GetAddressOf(), nullptr);
                m_renderContext->CSSetShader(m_dcsvrFieldCS.Get(), nullptr, 0);
                m_renderContext->Dispatch(((UINT)constants.field[0] + 15) / 16, ((UINT)constants.field[1] + 15) / 16, 1);
                ID3D11UnorderedAccessView* nullUAV[] = {nullptr};
                m_renderContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
                m_renderContext->CSSetShaderResources(0, 3, nullSRV);
                field.held = true;
                field.eye = eyes[eye];
                field.focus = focus[eye];
            }
        }

        void compositeViewContent(uint32_t viewIndex,
'@
    $drawing = @'
                drawing.debugFocusView = m_debugFocusView;
                // DCSVR Far depth: this eye's field moves the focus sample and the periphery sample alike
                // (dcsvr_depthwarp_ps.hlsli). Without it every Far depth constant stays 0 and nothing is bound at t2.
                if (m_dcsvrFarWarp && m_dcsvrField[viewIndex].srv) {
                    const XrFovf& eyeFov = m_cachedEyeFov[viewIndex];
                    drawing.dcsvrWarp[0] = 1.f;
                    // Focus uv to periphery uv: the focus view's tangent span over the composited view's (one orientation).
                    drawing.dcsvrWarp[1] = (std::tan(focusView.fov.angleRight) - std::tan(focusView.fov.angleLeft)) /
                                           (std::tan(eyeFov.angleRight) - std::tan(eyeFov.angleLeft));
                    drawing.dcsvrWarp[2] = (std::tan(focusView.fov.angleUp) - std::tan(focusView.fov.angleDown)) /
                                           (std::tan(eyeFov.angleUp) - std::tan(eyeFov.angleDown));
                    if (m_dcsvrFarDecision.reuse) {
                        drawing.dcsvrWarp[3] = 1.f;
                        memcpy(drawing.dcsvrReuse, m_dcsvrField[viewIndex].reuse, sizeof(drawing.dcsvrReuse));
                    }
                }
'@
    $oldBind = @'
                ID3D11ShaderResourceView* srvs[] = {srvForStereoView.Get(), srvForFocusView.Get()};
                m_renderContext->PSSetShaderResources(0, 2, srvs);
'@
    $bind = @'
                // DCSVR Far depth: t2 the field, only while the warp runs.
                const bool dcsvrFieldBound = m_dcsvrFarWarp && m_dcsvrField[viewIndex].srv;
                ID3D11ShaderResourceView* srvs[] = {srvForStereoView.Get(), srvForFocusView.Get(),
                                                    dcsvrFieldBound ? m_dcsvrField[viewIndex].srv.Get() : nullptr};
                m_renderContext->PSSetShaderResources(0, 3, srvs);
'@
    $draw = @'
                m_renderContext->Draw(3, 0);
                if (dcsvrFieldBound) {
                    // The next frame's FieldMain writes the field: it is not left bound as an input.
                    ID3D11ShaderResourceView* nullField[] = {nullptr};
                    m_renderContext->PSSetShaderResources(2, 1, nullField);
                }
'@
    $shader = @'
                g_dcsvr_temporal_cs, sizeof(g_dcsvr_temporal_cs), nullptr, m_dcsvrTemporalCS.ReleaseAndGetAddressOf()));

            // DCSVR Far depth: the field pass and its constants, only when enabled.
            if (m_dcsvrFarDepth.enabled) {
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = (UINT)sizeof(dcsvr::DisparityConstants);
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                CHECK_HRCMD(m_applicationDevice->CreateBuffer(&desc, nullptr, m_dcsvrFarConstants.ReleaseAndGetAddressOf()));
                CHECK_HRCMD(m_applicationDevice->CreateComputeShader(
                    g_dcsvr_depthwarp_cs, sizeof(g_dcsvr_depthwarp_cs), nullptr, m_dcsvrFieldCS.ReleaseAndGetAddressOf()));
                // The overlay coverage: its pass, constants and diagnostics. Any of them missing only means no coverage
                // (and no warp while it is required), never an error returned to DCS.
                desc.ByteWidth = (UINT)sizeof(dcsvr::CoverageConstants);
                if (FAILED(m_applicationDevice->CreateBuffer(&desc, nullptr, m_dcsvrCoverageConstants.ReleaseAndGetAddressOf())) ||
                    FAILED(m_applicationDevice->CreateComputeShader(
                        g_dcsvr_coverage_cs, sizeof(g_dcsvr_coverage_cs), nullptr, m_dcsvrCoverageCS.ReleaseAndGetAddressOf()))) {
                    m_dcsvrCoverageConstants.Reset();
                    m_dcsvrCoverageCS.Reset();
                }
                D3D11_BUFFER_DESC count{};
                count.ByteWidth = 16;
                count.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
                count.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
                D3D11_UNORDERED_ACCESS_VIEW_DESC countView{};
                countView.Format = DXGI_FORMAT_R32_TYPELESS;
                countView.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                countView.Buffer.NumElements = 4;
                countView.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
                if (SUCCEEDED(m_applicationDevice->CreateBuffer(&count, nullptr, m_dcsvrCoverageCount.ReleaseAndGetAddressOf())) &&
                    SUCCEEDED(m_applicationDevice->CreateUnorderedAccessView(m_dcsvrCoverageCount.Get(), &countView,
                                                                             m_dcsvrCoverageCountUav.ReleaseAndGetAddressOf()))) {
                    count.BindFlags = 0;
                    count.MiscFlags = 0;
                    count.Usage = D3D11_USAGE_STAGING;
                    count.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    if (FAILED(m_applicationDevice->CreateBuffer(&count, nullptr, m_dcsvrCoverageStaging.ReleaseAndGetAddressOf()))) {
                        m_dcsvrCoverageStaging.Reset();
                    }
                } else {
                    m_dcsvrCoverageCount.Reset();
                    m_dcsvrCoverageCountUav.Reset();
                }
            }
'@
    $oldRelease = @'
                    m_dcsvrTemporalControl.Invalidate();

                    m_applicationDevice.Reset();
'@
    $release = @'
                    m_dcsvrTemporalControl.Invalidate();
                    // DCSVR Far depth: the engine is told nothing is wanted, every held texture is released and the next
                    // session starts over at gain 0.
                    dcsvr::SharedDepthImport().SetWanted(0u);
                    dcsvr::SharedDepthImport().ReleaseAll();
                    dcsvr::SharedDepthImport().SetDevice(nullptr);
                    dcsvr::SharedDepthImport().SetRenderThread(0);
                    m_dcsvrDepthViews.clear();
                    for (DcsvrField& field : m_dcsvrField) {
                        field = {};
                    }
                    for (auto& jitter : m_dcsvrFarJitter) {
                        jitter.Reset();
                    }
                    m_dcsvrFarControl = dcsvr::DisparityController{};
                    m_dcsvrFarDecision = {};
                    m_dcsvrFarTime = 0;
                    m_dcsvrFarFrameWarp = m_dcsvrFarWarp = false;
                    m_dcsvrFieldCS.Reset();
                    m_dcsvrFarConstants.Reset();
                    m_dcsvrCoverageCS.Reset();
                    m_dcsvrCoverageConstants.Reset();
                    m_dcsvrCoverageCountUav.Reset();
                    m_dcsvrCoverageCount.Reset();
                    m_dcsvrCoverageStaging.Reset();
                    m_dcsvrCoverageStagingTexels = 0;
                    m_dcsvrCoverageCopyFrame = 0;
                    m_dcsvrCoverageEncoding = 0;
                    m_dcsvrCoverageSuspectRun = 0;
                    m_dcsvrCoverageFlips = 0;
                    m_dcsvrCoverageStats = {};
                    m_dcsvrCoverageGridLogged = false;
                    m_dcsvrCoveragePercent = 255;
                    m_dcsvrCoverageLastFrame = 0;
                    m_dcsvrFarKeyOff = m_dcsvrFarKeyDown = false;
                    m_dcsvrFarWarpedCount = m_dcsvrFarHeldCount = m_dcsvrFarWarpedLast = m_dcsvrFarHeldLast = 0;
                    m_dcsvrFarSecondAt = {};
                    dcsvr::DisparityStatus2().store(0u, std::memory_order_relaxed);
                    dcsvr::DisparityStatus().store(0u, std::memory_order_relaxed);

                    m_applicationDevice.Reset();
'@
    $export = @'

// DCSVR Far depth: the engine module's depth of both focus views (native/quadviews/dcsvr_depth_export.h, the contract,
// and docs/FAR_DEPTH.md section 5). Resolved with GetModuleHandle + GetProcAddress. noexcept, with nothing thrown across
// the C boundary (the import's mutex can throw): the answer is then "rejected" or nothing. The checks, the render
// thread among them, are dcsvr::DepthImport's.
extern "C" __declspec(dllexport) uint32_t DcsVr_QuadViewsDepthApi() noexcept {
    // A volatile read: the linker's identical-code folding cannot merge it with another function returning 1, so the
    // export keeps its own address (a debugger or a symbol lookup then names it).
    static volatile uint32_t version = 1u;
    return version;
}

// Which HMD views the engine should copy: both focus views (0b1100) while a session runs with Far depth enabled, else 0
// (the engine then copies nothing and frees its copies). One atomic load.
extern "C" __declspec(dllexport) uint32_t DcsVr_QuadViewsDepthWanted() noexcept {
    return dcsvr::SharedDepthImport().Wanted();
}

// One view's depth, pushed on DCS's render thread right after the engine recorded its copy: 1 accepted (Quad Views
// holds a reference), else a DcsVrDepthPushResult code.
extern "C" __declspec(dllexport) uint32_t DcsVr_QuadViewsDepth(const DcsVrDepthView* view) noexcept {
    try {
        return dcsvr::SharedDepthImport().Push(view);
    } catch (...) {
        return DCSVR_DEPTH_PUSH_NULL_OR_SIZE;
    }
}

// The engine drops a generation (uninstall, device change): Quad Views releases its references to those textures.
extern "C" __declspec(dllexport) void DcsVr_QuadViewsDepthRelease(uint32_t generation) noexcept {
    try {
        dcsvr::SharedDepthImport().Release(generation);
    } catch (...) {
    }
}

// Far depth's state for DCS Control's in-headset panel (dcsvr::DisparityStatus): bit 0 on (enabled, an opaque layer),
// bit 1 warped, bit 2 the engine's depth valid, bit 3 held fields bridged the frame, bits 8-15 the gain x 255, bits
// 16-23 the last skip reason (dcsvr::DisparitySkip), bits 24-31 an update counter. "On": bit 0 with bit 2 or 3;
// "waiting for engine": bit 0 without bits 2 and 3; "off": bit 0 clear (also 0, before the first frame or disabled).
extern "C" __declspec(dllexport) uint32_t DcsVr_QuadViewsDisparityStatus() noexcept {
    return dcsvr::DisparityStatus().load(std::memory_order_relaxed);
}

// Far depth's extended state for the in-headset panel (dcsvr::DisparityStatusWord, the bit layout there): on, enabled,
// depth valid, held, switched off by the key, coverage this frame, the gain x 100, frames warped and held in the last
// second, the coverage share (255 unknown) and an update counter. 0 while Far depth is disabled or before its first frame.
extern "C" __declspec(dllexport) uint64_t DcsVr_QuadViewsDisparityStatus2() noexcept {
    return dcsvr::DisparityStatus2().load(std::memory_order_relaxed);
}
'@
    $edits = @(
        @((& $fix '#include "dcsvr_perception.h"'), (& $fix "#include `"dcsvr_perception.h`"`n#include `"dcsvr_depthwarp.h`"`n#include `"dcsvr_depth_import.h`"")),
        @((& $fix '#include <dcsvr_temporal_cs.h>'), (& $fix "#include <dcsvr_temporal_cs.h>`n#include <dcsvr_depthwarp_cs.h>`n#include <dcsvr_coverage_cs.h>")),
        @((& $fix "        alignas(4) float focusViewShape;`n        alignas(4) float padding[3];`n    };"), (& $fix $constants)),
        @((& $fix '        ComPtr<ID3D11ComputeShader> m_dcsvrTemporalCS;'), (& $fix $members)),
        @((& $fix '                    } else if (name == "dcsvr_temporal") {'), (& $fix $options)),
        @((& $fix '                            initializeDeviceContext(d3dBindings->device);'), (& $fix $session)),
        @((& $fix '                            for (uint32_t viewIndex = 0; viewIndex < xr::StereoView::Count; viewIndex++) {'), (& $fix $frameCall)),
        @((& $fix '        void compositeViewContent(uint32_t viewIndex,'), (& $fix $functions)),
        @((& $fix '                drawing.debugFocusView = m_debugFocusView;'), (& $fix $drawing)),
        @((& $fix $oldBind), (& $fix $bind)),
        @((& $fix '                m_renderContext->Draw(3, 0);'), (& $fix $draw)),
        @((& $fix '                g_dcsvr_temporal_cs, sizeof(g_dcsvr_temporal_cs), nullptr, m_dcsvrTemporalCS.ReleaseAndGetAddressOf()));'), (& $fix $shader)),
        @((& $fix $oldRelease), (& $fix $release))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    $layerText = $layerText.TrimEnd() + $nl + (& $fix $export) + $nl
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# Live post-process settings (native/quadviews/dcsvr_live.h, which explains it; tested by tests/quadviews-live): upstream
# reads settings.cfg once per process. The layer now re-reads its files when their write time or size changes (checked
# at most once a second on the thread pool, never on the render thread) and applies only the keys that feed the
# per-frame post-process passes: sharpen_focus_view, smoothen_focus_view_edges, dcsvr_sharpen_taper,
# dcsvr_periphery_sharpen, dcsvr_temporal, dcsvr_temporal_tau_ms, dcsvr_temporal_ramp_deg, dcsvr_temporal_clip. Every
# other key keeps its start value (the log names the changed ones). One line in the Quad Views log per reload.
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'native/quadviews/dcsvr_live.h') -Destination (Join-Path $quadRoot 'openxr-api-layer/dcsvr_live.h') -Force
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('DCSVR live settings')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $fix = { param($text) ($text -replace "`r`n", "`n").TrimEnd("`n") -replace "`n", $nl }
    $load = @'
                    // DCSVR live settings: the files LoadConfiguration reads below, in its order, stamped and parsed
                    // first (a write in between is read at the first poll), from Quad Views' own defaults (the
                    // members before any file).
                    if (!m_dcsvrLiveDefaultsTaken) {
                        m_dcsvrLiveDefaults = dcsvrLiveCurrent();
                        m_dcsvrLiveDefaultsTaken = true;
                    }
                    {
                        std::vector<std::wstring> liveFiles{(dllHome / "settings.cfg").wstring()};
                        const DWORD liveLength = GetEnvironmentVariableW(L"DCSVR_QUAD_SETTINGS", nullptr, 0);
                        if (liveLength > 0 && liveLength < 32768) {
                            std::vector<wchar_t> livePath(liveLength);
                            if (GetEnvironmentVariableW(L"DCSVR_QUAD_SETTINGS", livePath.data(), liveLength)) {
                                liveFiles.push_back(livePath.data());
                            }
                        } else {
                            liveFiles.push_back((localAppData / "settings.cfg").wstring());
                        }
                        m_dcsvrLive = std::make_shared<dcsvr::live::Reloader>(
                            std::move(liveFiles),
                            dcsvr::live::Context{GetApplicationName(), GetApplicationExecutableName(), m_runtimeName, m_systemName},
                            m_dcsvrLiveDefaults);
                        m_dcsvrLive->Prime();
                    }
                    LoadConfiguration(dllHome / "settings.cfg");
'@
    $tick = @'
                handleDebugKeys();
                dcsvrLiveTick();
'@
    $functions = @'
        // DCSVR live settings (dcsvr_live.h): the live keys as the layer holds them now.
        dcsvr::live::Values dcsvrLiveCurrent() const {
            dcsvr::live::Values values;
            values.sharpen = m_sharpenFocusView;
            values.smoothing = m_smoothenFocusViewEdges;
            values.taper = m_dcsvrSharpenTaper;
            values.peripherySharpen = m_dcsvrPeripherySharpen;
            values.temporal = m_dcsvrTemporal.enabled;
            values.tauMs = m_dcsvrTemporal.tauMs;
            values.rampDeg = m_dcsvrTemporal.rampDeg;
            values.clip = m_dcsvrTemporal.clip;
            return values;
        }

        // DCSVR live settings: once per frame in xrEndFrame, where the debug keys change the same values and before
        // anything of the frame reads them. Hands the file check to the thread pool at most once a second and applies
        // a finished reload, all live keys at once (a try-lock: never waits). A reload sets every live key to its file
        // value, also one the debug keys changed since.
        void dcsvrLiveTick() {
            if (!m_dcsvrLive) {
                return;
            }
            m_dcsvrLive->Tick(std::chrono::steady_clock::now());
            dcsvr::live::Result reload;
            if (!m_dcsvrLive->Take(reload)) {
                return;
            }
            const dcsvr::live::Values before = dcsvrLiveCurrent();
            const dcsvr::live::Values& values = reload.values;
            m_sharpenFocusView = values.sharpen;
            m_smoothenFocusViewEdges = values.smoothing;
            m_dcsvrSharpenTaper = values.taper;
            m_dcsvrPeripherySharpen = values.peripherySharpen;
            m_dcsvrTemporal.enabled = values.temporal;
            m_dcsvrTemporal.tauMs = values.tauMs;
            m_dcsvrTemporal.rampDeg = values.rampDeg;
            m_dcsvrTemporal.clip = values.clip;
            if (before.temporal && !values.temporal) {
                // Periphery stability off: its history's memory goes back now; on again, it starts over.
                for (auto& history : m_dcsvrHistory) {
                    history = {};
                }
                m_dcsvrTemporalControl.Invalidate();
            }
            if (before.temporal != values.temporal || before.tauMs != values.tauMs || before.rampDeg != values.rampDeg ||
                before.clip != values.clip) {
                m_dcsvrTemporalLogged = false;  // its "on" line again, with the new values
            }
            const std::string changed = dcsvr::live::Changes(before, values);
            std::string ignored;
            for (const auto& name : reload.ignored) {
                ignored += (ignored.empty() ? "" : ", ") + name;
            }
            Log("DCSVR live settings: settings.cfg reloaded: %s%s%s\n",
                changed.empty() ? "no live key changed" : changed.c_str(),
                ignored.empty() ? "" : "; kept until the next start: ",
                ignored.c_str());
        }

        void handleDebugKeys() {
'@
    $members = @'
        bool m_debugKeys{false};
        // DCSVR live settings: the reloader of settings.cfg's post-process keys, and Quad Views' defaults for them.
        std::shared_ptr<dcsvr::live::Reloader> m_dcsvrLive;
        dcsvr::live::Values m_dcsvrLiveDefaults{};
        bool m_dcsvrLiveDefaultsTaken{false};
'@
    $edits = @(
        @((& $fix '#include "dcsvr_depth_import.h"'), (& $fix "#include `"dcsvr_depth_import.h`"`n#include `"dcsvr_live.h`"")),
        @((& $fix '                    LoadConfiguration(dllHome / "settings.cfg");'), (& $fix $load)),
        @((& $fix '                handleDebugKeys();'), (& $fix $tick)),
        @((& $fix '        void handleDebugKeys() {'), (& $fix $functions)),
        @((& $fix '        bool m_debugKeys{false};'), (& $fix $members))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# Log(std::string) passes the text to vsnprintf as the format: a lone '%' there ends DCS without a trace.
$unescapedPercent = [IO.File]::ReadAllLines($layerSource) | Where-Object { $_ -match 'Log\(fmt::format\(' -and ($_ -replace '%%', '') -match '%' }
if ($unescapedPercent) { throw "Quad Views Log text with an unescaped '%' (write '%%'): $($unescapedPercent -join ' | ')" }
# The error-check macros compile __FILE__ in: /d1trimfile keeps the build machine's folder (a user name) out of the DLL.
# The flag reaches every compile through a props file MSBuild imports (it ignores the CL/_CL_ variables' changes), and
# Rebuild: MSBuild does not see a changed option in objects built before, which would keep the path.
# /PDBALTPATH: the DLL names its symbols file without the folder.
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
$trimProps = Join-Path $workspaceRoot 'artifacts/native/quadviews-trimfile.props'
New-Item -ItemType Directory -Path (Split-Path -Parent $trimProps) -Force | Out-Null
[IO.File]::WriteAllText($trimProps, "<Project><ItemDefinitionGroup><ClCompile><AdditionalOptions>%(AdditionalOptions) &quot;/d1trimfile:$([Security.SecurityElement]::Escape($repoRoot))&quot;</AdditionalOptions></ClCompile><Link><AdditionalOptions>%(AdditionalOptions) /PDBALTPATH:%25_PDB%25</AdditionalOptions></Link></ItemDefinitionGroup></Project>")
& (Join-Path $PSScriptRoot 'msvc.cmd') msbuild "$quadRoot/openxr-api-layer/openxr-api-layer.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:SolutionName=XR_APILAYER_MBUCCHIA_quad_views_foveated "/p:SolutionDir=$solutionDir" "/p:ForceImportBeforeCppTargets=$trimProps" /m:4 /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw 'Quad Views build failed.' }
$builtDll = Join-Path $quadRoot 'bin/x64/Release/XR_APILAYER_MBUCCHIA_quad_views_foveated.dll'
if ((Test-Path $builtDll) -and [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($builtDll)).IndexOf($repoRoot, [StringComparison]::OrdinalIgnoreCase) -ge 0) { throw "The Quad Views DLL still contains $repoRoot." }
# The composition with our performance edits against the same shaders without them, on this machine's GPU (WARP
# without one): every pixel identical for the round and rectangular focus area, with and without sharpening. With Far
# depth built in, the composition with its constants at 0 (and a field bound that must not be read) against the shader
# without Far depth, both as source and as the DLL's own bytecode.
$compositionTest = Join-Path $workspaceRoot 'artifacts/native/qv-test/qv_composition_test.exe'
New-Item -ItemType Directory -Path (Split-Path -Parent $compositionTest) -Force | Out-Null
& (Join-Path $PSScriptRoot 'msvc.cmd') cl /nologo /std:c++20 /EHsc /O2 "/Fe:$compositionTest" "/Fo:$(Split-Path -Parent $compositionTest)/" (Join-Path $workspaceRoot 'tests/quadviews-composition/qv_composition_test.cpp') d3d11.lib d3dcompiler.lib
if ($LASTEXITCODE -ne 0) { throw 'Quad Views composition test build failed.' }
& $compositionTest (Join-Path $quadRoot 'openxr-api-layer') (Join-Path $quadRoot 'external/FidelityFX-CAS/ffx-cas') (Join-Path $quadRoot 'obj/x64/Release/ProjectionPS.h')
if ($LASTEXITCODE -ne 0) { throw 'Quad Views composition test failed.' }
# The saccade lead and the fixation deadzone on simulated gaze (2X and 3X frame times, tracker noise, blinks).
$gazeTest = Join-Path $workspaceRoot 'artifacts/native/qv-test/qv_gaze_test.exe'
& (Join-Path $PSScriptRoot 'msvc.cmd') cl /nologo /std:c++20 /EHsc /O2 /W4 "/Fe:$gazeTest" "/Fo:$(Split-Path -Parent $gazeTest)/" (Join-Path $workspaceRoot 'tests/quadviews-gaze/qv_gaze_test.cpp') "/I$(Join-Path $workspaceRoot 'native/quadviews')"
if ($LASTEXITCODE -ne 0) { throw 'Quad Views gaze test build failed.' }
& $gazeTest
if ($LASTEXITCODE -ne 0) { throw 'Quad Views gaze test failed.' }
# Live settings: the parser against Quad Views' section rules and file order, non-live keys never applied, no parse
# while the files' stamps stay, the non-blocking hand-over and the thread-pool poll (C++17, as the layer).
$liveTest = Join-Path $workspaceRoot 'artifacts/native/qv-test/qv_live_test.exe'
& (Join-Path $PSScriptRoot 'msvc.cmd') cl /nologo /std:c++17 /EHsc /O2 /W4 /WX "/Fe:$liveTest" "/Fo:$(Split-Path -Parent $liveTest)/" (Join-Path $workspaceRoot 'tests/quadviews-live/qv_live_test.cpp') "/I$(Join-Path $workspaceRoot 'native/quadviews')"
if ($LASTEXITCODE -ne 0) { throw 'Quad Views live settings test build failed.' }
& $liveTest
if ($LASTEXITCODE -ne 0) { throw 'Quad Views live settings test failed.' }
# Periphery stability: the shared maths, and the real shader on a D3D11 device (the GPU, else WARP) over synthetic
# periphery sequences (shimmer, head turn, unexplained motion, resets, 2X/3X/Auto timing) with a timing at 814x804.
$perceptionTest = Join-Path $workspaceRoot 'artifacts/native/qv-test/perception_test.exe'
& (Join-Path $PSScriptRoot 'msvc.cmd') cl /nologo /std:c++20 /EHsc /O2 /W4 "/Fe:$perceptionTest" "/Fo:$(Split-Path -Parent $perceptionTest)/" (Join-Path $workspaceRoot 'tests/perception/perception_test.cpp') "/I$(Join-Path $workspaceRoot 'native/quadviews')" d3d11.lib d3dcompiler.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { throw 'Perception test build failed.' }
& $perceptionTest (Join-Path $workspaceRoot 'native/quadviews')
if ($LASTEXITCODE -ne 0) { throw 'Perception test failed.' }
# Far depth: its maths, the import of the engine's export and the real field shader and composition (this build's
# ProjectionPS, checked to equal the test's own injection at the same anchors) on synthetic scenes.
# Half-size images and the GPU cost reported, not asserted: this build is also run with DCS flying, where timings say
# nothing about the code (scripts/build-depthwarp-test.ps1 -Perf asserts them on an idle GPU).
& (Join-Path $PSScriptRoot 'build-depthwarp-test.ps1') -Quick
# Built and tested: build-release.ps1 ships this DLL only while its sources match this stamp.
. (Join-Path $PSScriptRoot 'build-stamp.ps1'); Save-BuildStamp 'quadviews'
