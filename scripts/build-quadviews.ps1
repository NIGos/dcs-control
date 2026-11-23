#Requires -Version 7
# -LocalOnly '<why>': a build for this machine only. When one of the tests that follow the build fails, the build ends
# with a local-only stamp carrying <why> and the failure, instead of stopping without one. build-release.ps1 refuses a
# local-only stamp (scripts/build-stamp.ps1), so nothing built this way can be published. The build itself and the
# check of the DLL's content must still pass. When everything passes the stamp is a full one, -LocalOnly or not.
# -PatchOnly: the pinned sources reset and patched, then stop: nothing is built, tested or stamped. With -Checkout the
# checkout patched is that folder instead of external/quadviews (a scratch copy, to compare two patched trees).
param([string]$LocalOnly, [switch]$PatchOnly, [string]$Checkout)
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'build-jobs.ps1')  # Get-BuildJobs: the compile jobs at once
$quadRoot = Join-Path $workspaceRoot 'external/quadviews'
# Every key the app writes for this component must be read by it (scripts/written-keys.txt).
& (Join-Path $PSScriptRoot 'check-written-keys.ps1')
if ($LASTEXITCODE -ne 0) { throw 'The app writes a key this component never reads (check-written-keys.ps1).' }
if ($Checkout) { $quadRoot = [IO.Path]::GetFullPath($Checkout) }
if (-not (Test-Path -LiteralPath $quadRoot)) {
    & git clone --depth 1 --branch 1.1.3 https://github.com/mbucchia/Quad-Views-Foveated.git $quadRoot
    if ($LASTEXITCODE -ne 0) { throw 'Quad Views download failed.' }
}
if ((& git -C $quadRoot rev-parse HEAD) -ne '957ff0327185ea29acc41fc86c4cdc3caf428fb9') { throw 'Unexpected Quad Views revision.' }
& git -C $quadRoot submodule update --init --depth 1 external/OpenXR-SDK external/OpenXR-SDK-Source external/OpenXR-MixedReality external/FidelityFX-CAS external/fmt
if ($LASTEXITCODE -ne 0) { throw 'Quad Views dependencies failed.' }
. (Join-Path $PSScriptRoot 'build-stamp.ps1')
# DCS Control's changes to Quad Views are source patches in patches/quadviews, applied in name order to the pinned
# sources, plus whole files of its own from native/quadviews (see below).
$quadPatches = @(Get-ChildItem -LiteralPath (Join-Path $workspaceRoot 'patches/quadviews') -Filter '*.patch' -File -ErrorAction SilentlyContinue | Sort-Object Name)
if ($quadPatches.Count -eq 0) { throw 'No Quad Views patches found in patches/quadviews: the build would ship Quad Views without DCS Control''s changes.' }
$ownSources = @('dcsvr_gaze.h', 'dcsvr_perception.h', 'dcsvr_temporal_cs.hlsl', 'dcsvr_depthwarp.h', 'dcsvr_depthwarp_cs.hlsl', 'dcsvr_coverage_cs.hlsl',
    'dcsvr_depthwarp_ps.hlsli', 'dcsvr_depth_import.h', 'dcsvr_depth_export.h', 'dcsvr_live.h', 'dcsvr_twolayer_ps.hlsl',
    'dcsvr_threeviews.h')
# What the patched tree is made from: every patch (name and LF text) and every own source, as one hash.
function Get-QuadPatchInputs {
    $parts = @($quadPatches | ForEach-Object { 'patch ' + $_.Name + "`n" + [IO.File]::ReadAllText($_.FullName).Replace("`r`n", "`n") })
    $parts += @($ownSources | ForEach-Object { 'own ' + $_ + "`n" + [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([IO.File]::ReadAllBytes((Join-Path $workspaceRoot "native/quadviews/$_")))) })
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($parts -join "`n"))))
}
# The state of openxr-api-layer against the pin: the diff of its tracked files and every untracked file with its hash.
function Get-QuadTreeState {
    $diff = @(& git -C $quadRoot diff HEAD --binary --no-color --no-ext-diff -- openxr-api-layer)
    if ($LASTEXITCODE -ne 0) { throw 'Quad Views tree state could not be read (git diff).' }
    $untracked = @(& git -C $quadRoot ls-files --others --exclude-standard -- openxr-api-layer)
    if ($LASTEXITCODE -ne 0) { throw 'Quad Views tree state could not be read (git ls-files).' }
    $lines = $diff + @($untracked | Sort-Object | ForEach-Object { 'untracked ' + $_ + ' ' + [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([IO.File]::ReadAllBytes((Join-Path $quadRoot $_)))) })
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))))
}
# The patches are made against the pinned sources: a build starts from them, whatever an earlier build (or an older
# version of this script) left in the checkout. The one exception: the checkout is exactly what applying these same
# patches and sources gave last time (a record in its .git folder holds both hashes). Then nothing is reset or copied,
# its files keep their times and the build below is incremental. Any other state, a hand edit in the checkout included,
# resets it. -PatchOnly and -Checkout always reset.
$patchInputs = Get-QuadPatchInputs
$patchMarker = Join-Path "$(& git -C $quadRoot rev-parse --absolute-git-dir)".Trim() 'dcs-control-patches'
$recordedPatch = if (Test-Path -LiteralPath $patchMarker) { (Get-Content -LiteralPath $patchMarker -Raw).Trim() } else { $null }
$keepTree = -not $PatchOnly -and -not $Checkout -and $recordedPatch -and $recordedPatch -eq "$patchInputs $(Get-QuadTreeState)"
if ($keepTree) {
    Write-Host 'Quad Views patches and own sources unchanged since they were applied, and the checkout as they left it: kept.'
} else {
    Remove-Item -LiteralPath $patchMarker -Force -ErrorAction SilentlyContinue
    & git -C $quadRoot checkout -- openxr-api-layer
    if ($LASTEXITCODE -ne 0) { throw 'Quad Views source reset failed.' }
    & git -C $quadRoot clean -fdq openxr-api-layer
    if ($LASTEXITCODE -ne 0) { throw 'Quad Views source reset failed.' }
}
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
# The patches are applied here in name order to the pinned sources reset above. What each one does and why is written
# at the head of its file. Each was made against the pin plus the ones before it (later ones anchor on lines earlier ones
# add): a patch that does not apply means upstream's sources or an earlier patch changed, and the build stops instead of
# building without it.
# Until 2026-11 this script made the same changes itself, as about 110 text replacements; the patches give the same
# tree byte for byte (compared with -PatchOnly before the replacements were removed).
if (-not $keepTree) {
    foreach ($quadPatch in $quadPatches) {
        # Patches are LF; a checkout with core.autocrlf may have turned them into CRLF.
        $normalizedPatch = [IO.Path]::GetTempFileName()
        try {
            [IO.File]::WriteAllText($normalizedPatch, [IO.File]::ReadAllText($quadPatch.FullName).Replace("`r`n", "`n"))
            & git -C $quadRoot apply --whitespace=nowarn $normalizedPatch
            if ($LASTEXITCODE -ne 0) { throw "Quad Views patch $($quadPatch.Name) does not apply to the pinned sources in $quadRoot." }
        } finally { Remove-Item -LiteralPath $normalizedPatch -Force -ErrorAction SilentlyContinue }
        Write-Host "Applied Quad Views patch $($quadPatch.Name)."
    }
    # The sources DCS Control adds are whole files in native/quadviews, copied beside the patched ones: the tests below
    # compile them from there, so a patch would only hold a second copy.
    foreach ($ownSource in $ownSources) {
        Copy-Item -LiteralPath (Join-Path $workspaceRoot "native/quadviews/$ownSource") -Destination (Join-Path $quadRoot "openxr-api-layer/$ownSource") -Force
    }
    if (-not $Checkout) { Set-Content -LiteralPath $patchMarker -Value "$patchInputs $(Get-QuadTreeState)" -Encoding ascii }
}
$layerSource = Join-Path $quadRoot 'openxr-api-layer/layer.cpp'
# Log(std::string) passes the text to vsnprintf as the format: a lone '%' there ends DCS without a trace.
$unescapedPercent = [IO.File]::ReadAllLines($layerSource) | Where-Object { $_ -match 'Log\(fmt::format\(' -and ($_ -replace '%%', '') -match '%' }
if ($unescapedPercent) { throw "Quad Views Log text with an unescaped '%' (write '%%'): $($unescapedPercent -join ' | ')" }
if ($PatchOnly) {
    Write-Host "Quad Views sources patched in $quadRoot (-PatchOnly): nothing built."
    return
}
if ($Checkout) { throw '-Checkout is for -PatchOnly: a build always uses external/quadviews.' }
# The error-check macros compile __FILE__ in: /d1trimfile keeps the build machine's folder (a user name) out of the DLL.
# The flag reaches every compile through a props file MSBuild imports (it ignores the CL/_CL_ variables' changes).
# /PDBALTPATH: the DLL names its symbols file without the folder.
# /Brepro on compile and link, and whole-program code generation in full instead of upstream's incremental one (it keeps
# state from the link before): two builds of the same sources give the same DLL, so a rebuild adds no known-component line.
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
$trimProps = Join-Path $workspaceRoot 'artifacts/native/quadviews-trimfile.props'
New-Item -ItemType Directory -Path (Split-Path -Parent $trimProps) -Force | Out-Null
$trimText = "<Project><ItemDefinitionGroup><ClCompile><AdditionalOptions>%(AdditionalOptions) &quot;/d1trimfile:$([Security.SecurityElement]::Escape($repoRoot))&quot; /Brepro</AdditionalOptions></ClCompile><Link><AdditionalOptions>%(AdditionalOptions) /PDBALTPATH:%25_PDB%25 /Brepro</AdditionalOptions><LinkTimeCodeGeneration>UseLinkTimeCodeGeneration</LinkTimeCodeGeneration></Link></ItemDefinitionGroup></Project>"
if (-not (Test-Path -LiteralPath $trimProps) -or [IO.File]::ReadAllText($trimProps) -ne $trimText) { [IO.File]::WriteAllText($trimProps, $trimText) }
$msbuildArguments = @("$quadRoot/openxr-api-layer/openxr-api-layer.vcxproj", '/t:Build', '/p:Configuration=Release', '/p:Platform=x64', '/p:PlatformToolset=v145',
    '/p:SolutionName=XR_APILAYER_MBUCCHIA_quad_views_foveated', "/p:SolutionDir=$solutionDir", "/p:ForceImportBeforeCppTargets=$trimProps")
# The DLL is built again only when something it is made from changed; then the build is a full one. A record in obj
# names what the last successful build was made from (this checkout's folder, the patches and own sources, the props
# file, the MSBuild arguments, the compiler) and the hash of every file it left in bin/x64/Release and of the shader
# header the tests read. When all of that is still so, MSBuild is not started: the outputs are the ones that build made.
# Anything else deletes obj and bin/x64/Release first (by this script, never by MSBuild's Clean) and builds everything:
# - An incremental MSBuild run is no shortcut here. The project's pre-build step rewrites version.h and resource.rc on
#   every build, so it always recompiles and relinks, and a relink on top of the last one does not give the same bytes
#   as a full link (measured: 73 bytes differ, the time stamp and debug ids; full builds are byte for byte the same).
#   MSBuild also does not see a changed option in objects built before (the /d1trimfile path, a new compiler).
# - MSBuild's Clean deletes the files its state in obj (the *.write.1.tlog lists) names by ABSOLUTE path. In a checkout
#   copied from another place with its obj folder, those are the other checkout's outputs: on 2026-11-16 a build in a copy
#   deleted the original's DLL and objects that way. With obj gone there is no such list; the folder is in the record.
# The record goes before MSBuild starts and is written only after it succeeded: a failed build is followed by a full one.
$quadIntermediate = Join-Path $quadRoot 'obj'
$quadOutput = Join-Path $quadRoot 'bin/x64/Release'
$buildRecordPath = Join-Path $quadIntermediate 'dcs-control-build.txt'
$buildInputs = @("folder $quadRoot", "inputs $patchInputs", "props $trimText", "msbuild $($msbuildArguments -join ' ')", "toolset $(Get-BuildToolset)")
function Get-QuadBuildOutputs {
    $outputs = @(if (Test-Path -LiteralPath $quadOutput) { Get-ChildItem -LiteralPath $quadOutput -Recurse -File | Where-Object Name -ne 'tests-passed.stamp' })
    $shaderHeader = Join-Path $quadIntermediate 'x64/Release/ProjectionPS.h'
    if (Test-Path -LiteralPath $shaderHeader) { $outputs += Get-Item -LiteralPath $shaderHeader }
    return @($outputs | Sort-Object FullName | ForEach-Object { 'output ' + $_.FullName.Substring($quadRoot.Length) + ' ' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash })
}
$recordedBuild = if (Test-Path -LiteralPath $buildRecordPath) { [IO.File]::ReadAllText($buildRecordPath) } else { $null }
$currentOutputs = @(Get-QuadBuildOutputs)
if ($currentOutputs.Count -and $recordedBuild -eq ((@($buildInputs) + $currentOutputs) -join "`n")) {
    Write-Host 'Quad Views: inputs, folder and compiler unchanged since the last successful build, its outputs as it left them: not built again.'
} else {
    foreach ($folder in $quadIntermediate, $quadOutput) { if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force } }
    Write-Host 'Quad Views: full build.'
    & (Join-Path $PSScriptRoot 'msvc.cmd') msbuild @msbuildArguments "/m:$(Get-BuildJobs)" /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Quad Views build failed.' }
    [IO.File]::WriteAllText($buildRecordPath, ((@($buildInputs) + @(Get-QuadBuildOutputs)) -join "`n"))
}
$builtDll = Join-Path $quadRoot 'bin/x64/Release/XR_APILAYER_MBUCCHIA_quad_views_foveated.dll'
if (-not (Test-Path -LiteralPath $builtDll)) { throw "The Quad Views build left no DLL at ${builtDll}: its content cannot be checked." }
# As 8-bit text and as UTF-16 (at both byte alignments): a wide string would hold the folder as well.
$builtBytes = [IO.File]::ReadAllBytes($builtDll)
foreach ($builtText in @([Text.Encoding]::ASCII.GetString($builtBytes), [Text.Encoding]::Unicode.GetString($builtBytes), [Text.Encoding]::Unicode.GetString($builtBytes, 1, $builtBytes.Length - 1))) {
    if ($builtText.IndexOf($repoRoot, [StringComparison]::OrdinalIgnoreCase) -ge 0) { throw "The Quad Views DLL still contains $repoRoot." }
}
# The tests below are skipped when nothing they read changed since they last passed in full (scripts/test-cache.ps1):
# the DLL, the patched layer sources and shaders the composition and Far depth tests compile, the shader header the
# build made, the tests' own sources and the scripts. DCSC_FORCE_TESTS=1 runs them. The record is kept next to the DLL,
# so a full build (which deletes that folder) always runs them.
. (Join-Path $PSScriptRoot 'test-cache.ps1')
$testSources = @('tests/quadviews-composition', 'tests/quadviews-gaze', 'tests/quadviews-live', 'tests/perception', 'tests/threeviews', 'tests/depthwarp',
    'scripts/build-depthwarp-test.ps1', 'native/quadviews' | ForEach-Object { Join-Path $workspaceRoot $_ })
$testSources += @((Join-Path $quadRoot 'openxr-api-layer'), (Join-Path $quadRoot 'external/FidelityFX-CAS/ffx-cas'), (Join-Path $quadRoot 'obj/x64/Release/ProjectionPS.h'))
$testFingerprint = Get-NativeTestFingerprint $quadOutput $testSources
if (Test-NativeTestsCurrent $quadOutput $testFingerprint) {
    Save-BuildStamp 'quadviews'
    return
}
try {
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
# Three views (0.7): the real fill pass on a synthetic pair of periphery views (matrices, DCS's pixels kept, the fill
# against a direct render, no left-cockpit leak, nothing without the engine's flag).
$threeViewsTest = Join-Path $workspaceRoot 'artifacts/native/qv-test/threeviews_test.exe'
& (Join-Path $PSScriptRoot 'msvc.cmd') cl /nologo /std:c++20 /EHsc /O2 /W4 "/Fe:$threeViewsTest" "/Fo:$(Split-Path -Parent $threeViewsTest)/" (Join-Path $workspaceRoot 'tests/threeviews/threeviews_test.cpp') "/I$(Join-Path $workspaceRoot 'native/quadviews')" d3d11.lib
if ($LASTEXITCODE -ne 0) { throw 'Three views test build failed.' }
& $threeViewsTest
if ($LASTEXITCODE -ne 0) { throw 'Three views test failed.' }
# Far depth: its maths, the import of the engine's export and the real field shader and composition (this build's
# ProjectionPS, checked to equal the test's own injection at the same anchors) on synthetic scenes.
# Half-size images and the GPU cost reported, not asserted: this build is also run with DCS flying, where timings say
# nothing about the code (scripts/build-depthwarp-test.ps1 -Perf asserts them on an idle GPU).
& (Join-Path $PSScriptRoot 'build-depthwarp-test.ps1') -Quick
} catch {
    # Without -LocalOnly a failed test stops the build as before. With it: the first failure ends the tests (the ones
    # after it did not run) and the stamp says so.
    if (-not $LocalOnly) { throw }
    Write-Host "Quad Views test failed: $($_.Exception.Message)"
    Save-BuildStamp 'quadviews' -LocalOnly "$LocalOnly (failed: $($_.Exception.Message))"
    return
}
# Built and tested: build-release.ps1 ships this DLL only while its sources match this stamp.
Save-NativeTestsPassed $quadOutput $testFingerprint
Save-BuildStamp 'quadviews'
