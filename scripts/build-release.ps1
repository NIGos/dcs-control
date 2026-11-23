#Requires -Version 7
# -Sign: a release for publishing; it fails when the release signing key is missing (without it, a developer build
# warns and leaves the setup and ZIP unsigned).
# -LocalOnly: a package for this machine only. It accepts Quad Views and OFXR builds that carry a local-only build stamp
# (scripts/build-stamp.ps1: built with -LocalOnly, a test failed or was skipped), which a build without it refuses. The
# setup and the ZIP are then left unsigned, also when the signing key is here, so DCS Control's updater never installs
# them and they cannot be published by mistake. It cannot be combined with -Sign.
# -AllowDirty: with -Sign, build although the working tree has uncommitted changes or untracked files (see below).
param([switch]$NoRestore, [switch]$Sign, [switch]$LocalOnly, [switch]$AllowDirty)
$ErrorActionPreference = 'Stop'
if ($LocalOnly -and $Sign) { throw '-LocalOnly and -Sign cannot be used together: a local-only package is never signed or published.' }
$workspaceRoot = Split-Path -Parent $PSScriptRoot
# A release to publish is built from a clean tree, at the commit that is then tagged: the committed sources are the
# sources of the binaries, and the sources ZIP can be made from them. With -Sign, uncommitted changes or untracked
# files stop the build here, before anything is checked, built or deleted; -AllowDirty builds anyway. The commit goes
# into release-manifest.json ("commit"), with "-dirty" after it when the tree was not clean; a build outside a git
# checkout has none.
$sourceCommit = $null; $sourceChanges = @()
$sourceHead = try { "$(& git -C $workspaceRoot rev-parse HEAD 2>$null)".Trim() } catch { '' }
if ($sourceHead -match '^[0-9a-f]{40,64}$') {
    $sourceChanges = @(& git -C $workspaceRoot status --porcelain)
    if ($LASTEXITCODE -ne 0) { throw 'git status failed: the state of the working tree could not be read.' }
    $sourceCommit = if ($sourceChanges.Count) { "$sourceHead-dirty" } else { $sourceHead }
}
if ($Sign -and -not $AllowDirty) {
    if (-not $sourceCommit) { throw 'A signed release is built from a git checkout, and the commit of this folder could not be read. -AllowDirty builds anyway.' }
    if ($sourceChanges.Count) {
        throw ("A signed release is built from a clean tree, and this one has $($sourceChanges.Count) uncommitted or untracked path(s):`n" +
            (($sourceChanges | Select-Object -First 10) -join "`n") + "`nCommit them or move them out of the tree, then build again. -AllowDirty builds anyway and the manifest says so.")
    }
}
$env:DOTNET_CLI_TELEMETRY_OPTOUT='1'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE='false'
$env:NUGET_PACKAGES=Join-Path $workspaceRoot '.cache/nuget'
# DcsQvCull and pupil shift are built and tested by every release, side by side, each in a PowerShell process of its own
# (they set and read environment variables, so not two threads of this one), while the app is published here. Their
# outputs, build folders and checks are separate; each one's whole output goes to artifacts/native/<name>-release.log and
# is printed below once both have ended, one after the other. Either failing fails the release.
# A release to publish (-Sign) never trusts the native test cache (scripts/test-cache.ps1): DCSC_FORCE_TESTS=1 for those
# two builds' own runs only, the caller's session keeps its value. A build without -Sign skips a suite whose programs,
# sources and environment are exactly those of its last full pass, as a developer build does.
function Start-NativeBuild([string]$Name, [string]$Script) {
    $log = Join-Path $workspaceRoot "artifacts/native/$Name-release.log"
    New-Item -ItemType Directory -Path (Split-Path -Parent $log) -Force | Out-Null
    $previous = $env:DCSC_FORCE_TESTS
    if ($Sign) { $env:DCSC_FORCE_TESTS = '1' }
    try {
        $job = Start-Job -Name $Name -ArgumentList (Join-Path $PSScriptRoot $Script), $log -ScriptBlock {
            param([string]$Script, [string]$Log)
            $ErrorActionPreference = 'Stop'
            try { & $Script *>&1 | ForEach-Object { "$_" } | Out-File -LiteralPath $Log -Encoding utf8 }
            catch { "FAILED: $($_.Exception.Message)" | Out-File -LiteralPath $Log -Append -Encoding utf8; throw }
        }
    } finally { $env:DCSC_FORCE_TESTS = $previous }
    [pscustomobject]@{ Name = $Name; Job = $job; Log = $log }
}
function Wait-NativeBuilds([object[]]$Builds) {
    $null = $Builds.Job | Wait-Job
    $failed = @()
    foreach ($build in $Builds) {
        Write-Host "----- $($build.Name) ($($build.Log)) -----"
        if (Test-Path -LiteralPath $build.Log) { Get-Content -LiteralPath $build.Log | ForEach-Object { Write-Host $_ } }
        if ($build.Job.State -ne 'Completed') { $failed += $build.Name }
        Remove-Job -Job $build.Job -Force
    }
    if ($failed.Count) { throw "Native build failed: $($failed -join ', ') (full output in artifacts/native/<name>-release.log)." }
}
# A file's SHA-256 next to it as <file>.sha256 (lower case, one line): the components' own hashes, the ZIP's and the setup's.
function Write-Sha256Sidecar([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($Path + '.sha256') -Encoding ascii
}
# Every copy of the shared-memory contract headers, the OFXR tree's included, must match before anything is built.
& (Join-Path $PSScriptRoot 'check-contracts.ps1')
$dotnet = Join-Path $workspaceRoot '.tools/dotnet/dotnet.exe'
$version = & (Join-Path $PSScriptRoot 'version.ps1')
$releaseRoot = Join-Path $workspaceRoot "artifacts/release/DcsControl-$version-win-x64"
# The pilot's real DCS settings are hashed before the pipeline; test-release.ps1 reports a difference at its end (and
# fails on a change during its own run). A profile applied since, or a flight, changes them too.
$actualDcs = @{}; foreach ($name in 'options.lua','autoexec.cfg') { $file = Join-Path $env:USERPROFILE "Saved Games/DCS/Config/$name"; if (Test-Path -LiteralPath $file) { $actualDcs[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash } }
New-Item -ItemType Directory -Path (Join-Path $workspaceRoot 'artifacts/release') -Force | Out-Null
$actualDcs | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $workspaceRoot 'artifacts/fps-actual-before.json') -Encoding utf8
# An earlier build's signatures go first: a build that stops half way never leaves one next to new files. So does the
# sources ZIP of an earlier build of this version: it is made after the release (py -3 scripts/package-sources.py), and
# an old one next to the new files would fail test-release.ps1 at its end (verify-release-archives.ps1 compares it with
# this checkout) or, worse, be published with them.
foreach ($stale in "artifacts/release/DcsControl-$version-win-x64.zip.sig", "artifacts/release/DcsControl-$version-Setup.exe.sig",
    "artifacts/release/DcsControl-$version-sources.zip", "artifacts/release/DcsControl-$version-sources.zip.sha256") {
    Remove-Item -LiteralPath (Join-Path $workspaceRoot $stale) -Force -ErrorAction SilentlyContinue
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
# DCS engine optimizations (scripts/build-dcsqvcull.ps1), installed in Saved Games\DCS\Scripts when the profile enables
# them (the terrain prefetch fix included), and the pupil shift OpenXR layer (scripts/build-pupil-shift.ps1), deployed in
# the profile folder when the profile enables it. Built on every release so a stale DLL can never ship; collected below.
if ($env:DCSC_SKIP_TIMING_TESTS -eq '1') { throw 'DCSC_SKIP_TIMING_TESTS=1 is set: a release runs every test. Remove it and build again.' }
$nativeBuilds = @((Start-NativeBuild 'dcsqvcull' 'build-dcsqvcull.ps1'), (Start-NativeBuild 'pupil-shift' 'build-pupil-shift.ps1'))
# A release that stops before collecting them stops them too.
trap { if ($nativeBuilds) { $nativeBuilds.Job | Where-Object State -eq 'Running' | Stop-Job -ErrorAction SilentlyContinue }; break }
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
foreach ($component in @('quadviews','ofxr','cheeky')) { Assert-BuildStampCurrent $component -AllowLocalOnly:$LocalOnly }
foreach ($filename in @('dxgi.dll','CheekyFoveatedDLSS/CheekyFoveatedDLSSHost.dll','CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll')) {
    $target = Join-Path $payloadRoot "components/cheeky-focus/$filename"
    $source = if ($filename -eq 'dxgi.dll') { Join-Path $workspaceRoot ".cache/packages/cheeky/$filename" } else { Join-Path $workspaceRoot "artifacts/native/cheeky/bin/Release/$filename" }
    Copy-Item -LiteralPath $source -Destination $target -Force
    # The loader is the pinned Cheeky archive's own dxgi.dll (checked below against the archive's digest).
    if ($filename -eq 'dxgi.dll' -and (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne 'D9401877C6609106BD0D121EED5C29ECC5261E2C99378B1321EE16AD19AD3B0C') { throw 'Cheeky dxgi.dll is not the pinned one.' }
    Write-Sha256Sidecar $target
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
        Write-Sha256Sidecar $target
    }
} finally { $cheekyZip.Dispose() }
foreach ($archive in @('OFXR-Bridge-v0.2.1-V116.zip')) {
    Copy-Item -LiteralPath (Join-Path $workspaceRoot ".cache/packages/$archive") -Destination (Join-Path $payloadRoot 'packages') -Force
}
# OFXR layer built from the djules75 fork 0.2.9.1 (scripts/build-ofxr-djules75.ps1): the DCS-compatible build, used for every framegen profile.
New-Item -ItemType Directory -Path (Join-Path $payloadRoot 'components/ofxr') -Force | Out-Null
$deferredOfxr = Join-Path $payloadRoot 'components/ofxr/XR_APILAYER_XRFrameBridge_diagnostic.dll'
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'artifacts/native/ofxr-djules75/XR_APILAYER_XRFrameBridge_diagnostic.dll') -Destination $deferredOfxr -Force
Write-Sha256Sidecar $deferredOfxr
# DcsQvCull and pupil shift, started at the top.
Wait-NativeBuilds $nativeBuilds
New-Item -ItemType Directory -Path (Join-Path $payloadRoot 'components/dcsqvcull') -Force | Out-Null
foreach ($source in @('artifacts/native/dcsqvcull/DcsQvCull.dll','artifacts/native/dcsqvcull/DcsQvCullPayload.dll','native/dcsqvcull/lua/DcsQvCull.lua')) {
    $target = Join-Path $payloadRoot ('components/dcsqvcull/' + (Split-Path -Leaf $source))
    Copy-Item -LiteralPath (Join-Path $workspaceRoot $source) -Destination $target -Force
    Write-Sha256Sidecar $target
}
New-Item -ItemType Directory -Path (Join-Path $payloadRoot 'components/pupilshift') -Force | Out-Null
$pupilShift = Join-Path $payloadRoot 'components/pupilshift/XR_APILAYER_DCSVR_pupil_shift.dll'
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'artifacts/native/pupil-shift/XR_APILAYER_DCSVR_pupil_shift.dll') -Destination $pupilShift -Force
Write-Sha256Sidecar $pupilShift
$cheekyDll = Join-Path $payloadRoot 'components/CheekyOpenXRLayer.dll'
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'artifacts/native/cheeky/bin/Release/CheekyOpenXRLayer.dll') -Destination $cheekyDll -Force
Write-Sha256Sidecar $cheekyDll
foreach ($filename in @('XR_APILAYER_MBUCCHIA_quad_views_foveated.dll','openxr-api-layer.json','settings.cfg')) {
    $target = Join-Path $payloadRoot "components/quadviews/$filename"
    Copy-Item -LiteralPath (Join-Path $workspaceRoot "external/quadviews/bin/x64/Release/$filename") -Destination $target -Force
    Write-Sha256Sidecar $target
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
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'LICENSE') -Destination (Join-Path $payloadRoot 'licenses/Manager-GPL-3.0.txt') -Force
# The WebView2 SDK's licence files come from the package version the app references (one place: DcsVr.App.csproj).
$webView2 = ([xml](Get-Content -LiteralPath (Join-Path $workspaceRoot 'src/DcsVr.App/DcsVr.App.csproj') -Raw)).SelectSingleNode("//PackageReference[@Include='Microsoft.Web.WebView2']").Version
if (-not $webView2) { throw 'DcsVr.App.csproj does not reference Microsoft.Web.WebView2.' }
Copy-Item -LiteralPath (Join-Path $workspaceRoot ".cache/nuget/microsoft.web.webview2/$webView2/LICENSE.txt") -Destination (Join-Path $payloadRoot 'licenses/WebView2-SDK.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot ".cache/nuget/microsoft.web.webview2/$webView2/NOTICE.txt") -Destination (Join-Path $payloadRoot 'licenses/WebView2-NOTICE.txt') -Force
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
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\installer.ps1" -Action Uninstall -RestoreDcs & pause & exit /b
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
$manifest = @{ schemaVersion=1; product='DcsControl'; version=$version; files=@($files) }
if ($sourceCommit) { $manifest.commit = $sourceCommit }
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $releaseRoot 'release-manifest.json') -Encoding utf8
& (Join-Path $payloadRoot 'scripts/installer.ps1') -Action Verify -Destination (Join-Path $workspaceRoot 'artifacts/installer-verify') -NoShortcut
if ($LASTEXITCODE -ne 0) { throw 'Release integrity check failed.' }
# Every component of this release that can land in DCS's own folders joins src/DcsVr.Core/KnownComponents.txt, so later
# versions recognize its files in DCS even when the record of originals that listed them is gone (Leftovers.cs). New
# lines are for the next build and go into the release commit.
# Only cheeky/, cheeky-focus/ and dcsqvcull/ are recorded (Leftovers.RecordedComponentFolders, the tests compare the two
# lists). OFXR, Quad Views, pupil shift and CheekyOpenXRLayer.dll are installed only under DCS Control's own profile
# folder, where nothing consults this list: recording them added lines on every branch for no effect (and on every
# build, while their builds were not yet reproducible). Their earlier lines stay in the file (harmless).
$recordedComponents = @('cheeky/', 'cheeky-focus/', 'dcsqvcull/')
$knownList = Join-Path $workspaceRoot 'src/DcsVr.Core/KnownComponents.txt'
$knownText = [IO.File]::ReadAllText($knownList)
$known = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($line in $knownText -split "`n") { $token = ($line -split ' ', 2)[0].Trim(); if ($token -match '^[0-9a-fA-F]{64}$') { [void]$known.Add($token) } }
$componentsRoot = Join-Path $payloadRoot 'components'
$added = foreach ($sha in Get-ChildItem -LiteralPath $componentsRoot -Recurse -File -Filter '*.sha256' | Sort-Object FullName) {
    $component = [IO.Path]::GetRelativePath($componentsRoot, $sha.FullName).Replace('\', '/') -replace '\.sha256$', ''
    if (-not ($recordedComponents | Where-Object { $component.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) })) { continue }
    $hash = (Get-Content -LiteralPath $sha.FullName -Raw).Trim().Split(' ')[0].ToLowerInvariant()
    if ($hash -match '^[0-9a-f]{64}$' -and $known.Add($hash)) { "$hash $component ($version)" }
}
if ($added) {
    [IO.File]::WriteAllText($knownList, $knownText.TrimEnd("`r", "`n") + "`n" + (($added) -join "`n") + "`n")
    Write-Warning "$(@($added).Count) new component hash(es) added to src/DcsVr.Core/KnownComponents.txt: commit them with this release."
}
$releaseZip = $releaseRoot + '.zip'
if (Test-Path -LiteralPath $releaseZip) { Remove-Item -LiteralPath $releaseZip }
[IO.Compression.ZipFile]::CreateFromDirectory($releaseRoot, $releaseZip, [IO.Compression.CompressionLevel]::Optimal, $true)
Write-Sha256Sidecar $releaseZip
# The setup a pilot downloads: native/setup with this ZIP appended (scripts/build-setup.ps1). It and the ZIP are signed
# with the release key (scripts/sign-release.ps1); DCS Control's updater installs only a setup whose signature verifies.
$setup = Join-Path $workspaceRoot "artifacts/release/DcsControl-$version-Setup.exe"
& (Join-Path $PSScriptRoot 'build-setup.ps1') -Package $releaseZip -Output $setup | Out-Null
Write-Sha256Sidecar $setup
if ($LocalOnly) {
    # Signatures of an earlier build were removed at the start; none is made.
    Write-Warning 'LOCAL-ONLY package: the setup and the ZIP are not signed. Not for publishing.'
} else {
    & (Join-Path $PSScriptRoot 'sign-release.ps1') -Files $setup, $releaseZip -Required:$Sign
}
Write-Output $setup
Write-Output $releaseZip
