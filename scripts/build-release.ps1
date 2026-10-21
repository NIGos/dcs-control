#Requires -Version 7
# -Sign: a release for publishing; it fails when the release signing key is missing (without it, a developer build
# warns and leaves the setup and ZIP unsigned).
param([switch]$NoRestore, [switch]$Sign)
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$env:DOTNET_CLI_TELEMETRY_OPTOUT='1'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE='false'
$env:NUGET_PACKAGES=Join-Path $workspaceRoot '.cache/nuget'
# A release never trusts the native test cache (scripts/test-cache.ps1): DCSC_FORCE_TESTS=1 for the native builds'
# own runs only; the caller's session keeps its value.
function Invoke-WithFreshTests([scriptblock]$Build) {
    $previous = $env:DCSC_FORCE_TESTS
    $env:DCSC_FORCE_TESTS = '1'
    try { & $Build } finally { $env:DCSC_FORCE_TESTS = $previous }
}
# Every copy of the shared-memory contract headers, the OFXR tree's included, must match before anything is built.
& (Join-Path $PSScriptRoot 'check-contracts.ps1')
$dotnet = Join-Path $workspaceRoot '.tools/dotnet/dotnet.exe'
$version = & (Join-Path $PSScriptRoot 'version.ps1')
$releaseRoot = Join-Path $workspaceRoot "artifacts/release/DcsControl-$version-win-x64"
# The user's real DCS settings are hashed before the pipeline; verify-web-release.py checks that nothing in
# building, testing or verifying the release changed them. A profile the user applied earlier is fine.
$actualDcs = @{}; foreach ($name in 'options.lua','autoexec.cfg') { $file = Join-Path $env:USERPROFILE "Saved Games/DCS/Config/$name"; if (Test-Path -LiteralPath $file) { $actualDcs[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash } }
New-Item -ItemType Directory -Path (Join-Path $workspaceRoot 'artifacts/release') -Force | Out-Null
$actualDcs | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $workspaceRoot 'artifacts/fps-actual-before.json') -Encoding utf8
# An earlier build's signatures go first: a build that stops half way never leaves one next to new files.
foreach ($signed in "artifacts/release/DcsControl-$version-win-x64.zip.sig", "artifacts/release/DcsControl-$version-Setup.exe.sig") {
    Remove-Item -LiteralPath (Join-Path $workspaceRoot $signed) -Force -ErrorAction SilentlyContinue
}
if (Test-Path -LiteralPath $releaseRoot) {
    $resolvedRelease = (Resolve-Path -LiteralPath $releaseRoot).Path
    $allowedRelease = [IO.Path]::GetFullPath((Join-Path $workspaceRoot "artifacts/release/DcsControl-$version-win-x64"))
    if ($resolvedRelease -ne $allowedRelease -or -not $resolvedRelease.StartsWith([IO.Path]::GetFullPath($workspaceRoot) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing to clean an unexpected release destination.' }
    Remove-Item -LiteralPath $resolvedRelease -Recurse -Force
}
New-Item -ItemType Directory -Path $releaseRoot -Force | Out-Null
$payloadRoot = Join-Path $releaseRoot 'files'
New-Item -ItemType Directory -Path $payloadRoot -Force | Out-Null
# The launcher at the top of the release folder (starts files\DcsControl.exe); the interface render below uses it too.
$launcher = & (Join-Path $PSScriptRoot 'build-launcher.ps1') | Select-Object -Last 1
Copy-Item -LiteralPath $launcher -Destination (Join-Path $releaseRoot 'DcsControl.exe') -Force
foreach ($project in @('src/DcsVr.App','src/DcsVr.Cli')) {
    $publishOptions = @('-c','Release','-r','win-x64','--self-contained','true','-o',$payloadRoot,'-p:DebugType=None','-p:DebugSymbols=false','--nologo')
    if ($NoRestore) { $publishOptions += '--no-restore' }
    & $dotnet publish (Join-Path $workspaceRoot $project) @publishOptions
    if ($LASTEXITCODE -ne 0) { throw 'Publish failed.' }
}
foreach ($dir in @('packages','components/quadviews','components/cheeky-focus/CheekyFoveatedDLSS','licenses','scripts','profiles','docs')) { New-Item -ItemType Directory -Path (Join-Path $payloadRoot $dir) -Force | Out-Null }
# Quad Views, OFXR and Cheeky are copied as built: each must have been built from the sources and patches here now
# (the source package carries those, so they must match what ships).
. (Join-Path $PSScriptRoot 'build-stamp.ps1')
if ($env:DCSC_SKIP_TIMING_TESTS -eq '1') { throw 'DCSC_SKIP_TIMING_TESTS=1 is set: a release runs every test. Remove it and build again.' }
foreach ($component in @('quadviews','ofxr','cheeky')) { Assert-BuildStampCurrent $component }
foreach ($filename in @('dxgi.dll','CheekyFoveatedDLSS/CheekyFoveatedDLSSHost.dll','CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll')) {
    $target = Join-Path $payloadRoot "components/cheeky-focus/$filename"
    $source = if ($filename -eq 'dxgi.dll') { Join-Path $workspaceRoot ".cache/packages/cheeky/$filename" } else { Join-Path $workspaceRoot "artifacts/native/cheeky/bin/Release/$filename" }
    Copy-Item -LiteralPath $source -Destination $target -Force
    # The loader is the pinned Cheeky archive's own dxgi.dll (checked below against the archive's digest).
    if ($filename -eq 'dxgi.dll' -and (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne 'D9401877C6609106BD0D121EED5C29ECC5261E2C99378B1321EE16AD19AD3B0C') { throw 'Cheeky dxgi.dll is not the pinned one.' }
    (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($target + '.sha256') -Encoding ascii
}
# Cheeky stereo routes ship the three upstream binaries they use, taken from the pinned archive. The archive itself
# and its version.dll fallback loader (flagged by Microsoft Defender) are not distributed.
$cheekyArchive = Join-Path $workspaceRoot '.cache/packages/CheekyFoveatedDLSS-0.5.4-Standalone-release.zip'
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $cheekyArchive).Hash -ne 'D119A8EDAACF0924FA748F6B7A2D13486BAA39313E965A3EED6E385234F5116C') { throw 'Cheeky archive digest mismatch.' }
$cheekyZip = [IO.Compression.ZipFile]::OpenRead($cheekyArchive)
try {
    foreach ($entryName in @('dxgi.dll','CheekyFoveatedDLSS/CheekyFoveatedDLSSHost.dll','CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll')) {
        $entry = $cheekyZip.Entries | Where-Object { $_.FullName.Replace('\','/') -eq $entryName } | Select-Object -First 1
        if (-not $entry) { throw "Cheeky archive is missing $entryName." }
        $target = Join-Path $payloadRoot "components/cheeky/$entryName"
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($target + '.sha256') -Encoding ascii
    }
} finally { $cheekyZip.Dispose() }
foreach ($archive in @('OFXR-Bridge-v0.2.1-V116.zip')) {
    Copy-Item -LiteralPath (Join-Path $workspaceRoot ".cache/packages/$archive") -Destination (Join-Path $payloadRoot 'packages') -Force
}
# OFXR layer built from the djules75 fork 0.2.9.1 (scripts/build-ofxr-djules75.ps1): the DCS-compatible build, used for every framegen profile.
New-Item -ItemType Directory -Path (Join-Path $payloadRoot 'components/ofxr') -Force | Out-Null
$deferredOfxr = Join-Path $payloadRoot 'components/ofxr/XR_APILAYER_XRFrameBridge_diagnostic.dll'
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'artifacts/native/ofxr-djules75/XR_APILAYER_XRFrameBridge_diagnostic.dll') -Destination $deferredOfxr -Force
(Get-FileHash -LiteralPath $deferredOfxr -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($deferredOfxr + '.sha256') -Encoding ascii
# DCS engine optimizations (scripts/build-dcsqvcull.ps1), installed in Saved Games\DCS\Scripts when the profile enables
# them (the terrain prefetch fix included). Rebuilt and tested on every release so a stale DLL can never ship.
Invoke-WithFreshTests { & (Join-Path $PSScriptRoot 'build-dcsqvcull.ps1') }
New-Item -ItemType Directory -Path (Join-Path $payloadRoot 'components/dcsqvcull') -Force | Out-Null
foreach ($source in @('artifacts/native/dcsqvcull/DcsQvCull.dll','artifacts/native/dcsqvcull/DcsQvCullPayload.dll','native/dcsqvcull/lua/DcsQvCull.lua')) {
    $target = Join-Path $payloadRoot ('components/dcsqvcull/' + (Split-Path -Leaf $source))
    Copy-Item -LiteralPath (Join-Path $workspaceRoot $source) -Destination $target -Force
    (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($target + '.sha256') -Encoding ascii
}
# Pupil shift OpenXR layer (scripts/build-pupil-shift.ps1), deployed in the profile folder when the profile enables it.
Invoke-WithFreshTests { & (Join-Path $PSScriptRoot 'build-pupil-shift.ps1') }
New-Item -ItemType Directory -Path (Join-Path $payloadRoot 'components/pupilshift') -Force | Out-Null
$pupilShift = Join-Path $payloadRoot 'components/pupilshift/XR_APILAYER_DCSVR_pupil_shift.dll'
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'artifacts/native/pupil-shift/XR_APILAYER_DCSVR_pupil_shift.dll') -Destination $pupilShift -Force
(Get-FileHash -LiteralPath $pupilShift -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($pupilShift + '.sha256') -Encoding ascii
$cheekyDll = Join-Path $payloadRoot 'components/CheekyOpenXRLayer.dll'
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'artifacts/native/cheeky/bin/Release/CheekyOpenXRLayer.dll') -Destination $cheekyDll -Force
(Get-FileHash -LiteralPath $cheekyDll -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($cheekyDll + '.sha256') -Encoding ascii
foreach ($filename in @('XR_APILAYER_MBUCCHIA_quad_views_foveated.dll','openxr-api-layer.json','settings.cfg')) {
    $target = Join-Path $payloadRoot "components/quadviews/$filename"
    Copy-Item -LiteralPath (Join-Path $workspaceRoot "external/quadviews/bin/x64/Release/$filename") -Destination $target -Force
    (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($target + '.sha256') -Encoding ascii
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'installer.ps1') -Destination (Join-Path $payloadRoot 'scripts/installer.ps1') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'README.md') -Destination (Join-Path $payloadRoot 'README.md') -Force
# The policy's version and corresponding-source name always match this build (GPL source offer).
$policy = Get-Content -LiteralPath (Join-Path $workspaceRoot 'distribution-policy.json') -Raw | ConvertFrom-Json
$policy.version = $version; $policy.correspondingSource = "DcsControl-$version-sources.zip"
$policy | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $payloadRoot 'distribution-policy.json') -Encoding utf8
Get-ChildItem -LiteralPath (Join-Path $workspaceRoot 'docs') -File | Where-Object { $_.Extension -in '.md', '.json', '.png' } |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $payloadRoot 'docs') -Force }
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'docs/screenshots') -Destination (Join-Path $payloadRoot 'docs') -Recurse -Force
$renderStart = [Diagnostics.ProcessStartInfo]::new((Join-Path $releaseRoot 'DcsControl.exe'))
$renderStart.UseShellExecute = $false; $renderStart.CreateNoWindow = $true
$renderRoot = Join-Path $workspaceRoot ('artifacts/web-ui/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $renderRoot -Force | Out-Null
# The shipped interface previews are rendered from a neutral demo layout, so no path shows the build machine's user.
$demoRoot = Join-Path ([IO.Path]::GetPathRoot($workspaceRoot)) 'DcsVrDemo'
if (Test-Path -LiteralPath $demoRoot) { Remove-Item -LiteralPath $demoRoot -Recurse -Force }
New-Item -ItemType Directory -Path $demoRoot -Force | Out-Null
$renderStart.Environment['DCSVR_SMOKE_DEMO_ROOT'] = $demoRoot
$renderStart.ArgumentList.Add('--web-smoke'); $renderStart.ArgumentList.Add((Join-Path $renderRoot 'interface'))
# The demo folder at the drive root goes in every case; a failed render keeps its folder (interface-checks.json says why).
try {
    $renderProcess = [Diagnostics.Process]::Start($renderStart)
    if (-not $renderProcess.WaitForExit(120000)) { $renderProcess.Kill($true); throw "Release interface render timed out (see $renderRoot)." }
    if ($renderProcess.ExitCode -ne 0) { throw "Release interface render failed (see $renderRoot)." }
    $renderProcess.Dispose()
} finally { Remove-Item -LiteralPath $demoRoot -Recurse -Force -ErrorAction SilentlyContinue }
Get-ChildItem -LiteralPath $renderRoot -File | Where-Object { $_.Name -like 'interface-*.png' -or $_.Name -eq 'interface-checks.json' -or $_.Name -eq 'interface-renders.json' } | Copy-Item -Destination (Join-Path $payloadRoot 'docs') -Force
Remove-Item -LiteralPath $renderRoot -Recurse -Force -ErrorAction SilentlyContinue # copied into the payload; nothing else reads it
# The render list names each image by its file name only: the build folder (and the build machine's user name) never ships.
$rendersFile = Join-Path $payloadRoot 'docs/interface-renders.json'
$renders = @(Get-Content -LiteralPath $rendersFile -Raw | ConvertFrom-Json)
foreach ($render in $renders) { if ($render.PSObject.Properties['path']) { $render.path = Split-Path -Leaf $render.path } }
ConvertTo-Json -InputObject $renders -Depth 8 | Set-Content -LiteralPath $rendersFile -Encoding utf8
if ($renders.Count -eq 0 -or @($renders | Where-Object { -not (Test-Path -LiteralPath (Join-Path $payloadRoot ('docs/' + $_.path))) }).Count) { throw 'Release interface preview is incomplete.' }
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'LICENSE') -Destination (Join-Path $payloadRoot 'licenses/Manager-MIT.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot '.cache/nuget/microsoft.web.webview2/1.0.4258.31/LICENSE.txt') -Destination (Join-Path $payloadRoot 'licenses/WebView2-SDK.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot '.cache/nuget/microsoft.web.webview2/1.0.4258.31/NOTICE.txt') -Destination (Join-Path $payloadRoot 'licenses/WebView2-NOTICE.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/cheeky/LICENSE') -Destination (Join-Path $payloadRoot 'licenses/Cheeky-GPL-3.0.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot '.cache/packages/ofxr/LICENSE') -Destination (Join-Path $payloadRoot 'licenses/OFXR-LGPL-3.0.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/ofxr-djules75/LICENSE') -Destination (Join-Path $payloadRoot 'licenses/OFXR-djules75-LGPL-3.0.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/ofxr-djules75/THIRD_PARTY.md') -Destination (Join-Path $payloadRoot 'licenses/OFXR-djules75-THIRD_PARTY.md') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/quadviews/LICENSE') -Destination (Join-Path $payloadRoot 'licenses/QuadViews-MIT.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/quadviews/THIRD_PARTY') -Destination (Join-Path $payloadRoot 'licenses/QuadViews-THIRD-PARTY.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/quadviews/external/OpenXR-MixedReality/NOTICE') -Destination (Join-Path $payloadRoot 'licenses/OpenXR-MixedReality-NOTICE.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/quadviews/packages/Microsoft.Windows.ImplementationLibrary.1.0.220201.1/LICENSE') -Destination (Join-Path $payloadRoot 'licenses/WIL-MIT.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/quadviews/packages/Microsoft.Windows.ImplementationLibrary.1.0.220201.1/ThirdPartyNotices.txt') -Destination (Join-Path $payloadRoot 'licenses/WIL-ThirdPartyNotices.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot '.cache/packages/cheeky/licenses') -Destination (Join-Path $payloadRoot 'licenses/Cheeky-dependencies') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/sboys/LICENCE.md') -Destination (Join-Path $payloadRoot 'licenses/Sboys-GPL-2.0.md') -Force
$presets = & $dotnet (Join-Path $payloadRoot 'DcsVr.Cli.dll') presets
if ($LASTEXITCODE -ne 0 -or -not $presets) { throw 'Preset export failed.' }
$presets | Set-Content -LiteralPath (Join-Path $payloadRoot 'profiles/presets.json') -Encoding utf8
@'
@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\scripts\installer.ps1" -Action Install -InstallPrerequisites
pause
'@ | Set-Content -LiteralPath (Join-Path $releaseRoot 'Install DCS Control.cmd') -Encoding ascii
# One line ending in exit: the uninstaller removes this file, and cmd must not read it again afterwards.
@'
@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\installer.ps1" -Action Uninstall & pause & exit /b
'@ | Set-Content -LiteralPath (Join-Path $payloadRoot 'Uninstall.cmd') -Encoding ascii
# The top of the release folder also holds a short README next to the launcher and the installer command.
@"
DCS Control $version
====================

Start: double-click DcsControl.exe (it can stay in this folder, anywhere you like).
Install with a Start menu entry: double-click "Install DCS Control.cmd".
(DcsControl-$version-Setup.exe on the Releases page installs it in one step, with Start menu and desktop shortcuts.)

The files folder holds the rest of the app: leave it next to DcsControl.exe.

Guide: https://github.com/NIGos/dcs-control/blob/main/docs/USER_GUIDE.md
Problems and results: https://github.com/NIGos/dcs-control/issues
"@ | Set-Content -LiteralPath (Join-Path $releaseRoot 'README.txt') -Encoding utf8
$files = Get-ChildItem -LiteralPath $releaseRoot -Recurse -File | Where-Object { $_.Name -ne 'release-manifest.json' } | ForEach-Object {
    @{ path=[IO.Path]::GetRelativePath($releaseRoot,$_.FullName); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); bytes=$_.Length }
}
@{ schemaVersion=1; product='DcsControl'; version=$version; files=@($files) } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $releaseRoot 'release-manifest.json') -Encoding utf8
& (Join-Path $payloadRoot 'scripts/installer.ps1') -Action Verify -Destination (Join-Path $workspaceRoot 'artifacts/installer-verify') -NoShortcut
if ($LASTEXITCODE -ne 0) { throw 'Release integrity check failed.' }
$releaseZip = $releaseRoot + '.zip'
if (Test-Path -LiteralPath $releaseZip) { Remove-Item -LiteralPath $releaseZip }
[IO.Compression.ZipFile]::CreateFromDirectory($releaseRoot, $releaseZip, [IO.Compression.CompressionLevel]::Optimal, $true)
(Get-FileHash -LiteralPath $releaseZip -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($releaseZip + '.sha256') -Encoding ascii
# The setup a pilot downloads: native/setup with this ZIP appended (scripts/build-setup.ps1). It and the ZIP are signed
# with the release key (scripts/sign-release.ps1); DCS Control's updater installs only a setup whose signature verifies.
$setup = Join-Path $workspaceRoot "artifacts/release/DcsControl-$version-Setup.exe"
& (Join-Path $PSScriptRoot 'build-setup.ps1') -Package $releaseZip -Output $setup | Out-Null
(Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($setup + '.sha256') -Encoding ascii
& (Join-Path $PSScriptRoot 'sign-release.ps1') -Files $setup, $releaseZip -Required:$Sign
Write-Output $setup
Write-Output $releaseZip
