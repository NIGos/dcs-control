#Requires -Version 7
# -LocalOnly '<why>': a build for this machine only. When the second test phase does not pass (a pacing test fails 3
# times, or DCSC_SKIP_TIMING_TESTS=1 skips the phase) the build ends with a local-only stamp carrying <why> and the
# failed tests, instead of stopping without one. build-release.ps1 refuses a local-only stamp (scripts/build-stamp.ps1),
# so nothing built this way can be published, and no stamp has to be written by hand. The first phase must still pass.
# When everything passes the stamp is a full one, -LocalOnly or not.
# -BuildOnly: configure and build, then stop: no test, no stamp (the stamp of an earlier build is left as it is).
param([string]$LocalOnly, [switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$sdkRoot = Join-Path $workspaceRoot 'external/ofxr/external/FidelityFX-SDK-v1.1.4'
$wrapper = Join-Path $PSScriptRoot 'msvc.cmd'
# Every key the app writes for this component must be read by it (scripts/written-keys.txt).
& (Join-Path $PSScriptRoot 'check-written-keys.ps1')
if ($LASTEXITCODE -ne 0) { throw 'The app writes a key this component never reads (check-written-keys.ps1).' }
if (-not (Test-Path -LiteralPath $sdkRoot)) {
    & git clone --depth 1 --filter=blob:none --sparse --branch v1.1.4 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git $sdkRoot
    if ($LASTEXITCODE -ne 0) { throw 'FidelityFX download failed.' }
    & git -C $sdkRoot sparse-checkout set sdk tools
    if ($LASTEXITCODE -ne 0) { throw 'FidelityFX sparse checkout failed.' }
}
$revision = & git -C $sdkRoot rev-parse HEAD
if ($revision -ne 'c6efa6bf7f2027b3ec94f28578bb5965eabb9e55') { throw 'Unexpected FidelityFX revision.' }
# The pinned SDK forces a Visual Studio platform even with Ninja. Keep its
# target architecture but avoid a generator platform that Ninja rejects.
$toolchainPath = Join-Path $sdkRoot 'sdk/toolchain.cmake'
$toolchainText = [IO.File]::ReadAllText($toolchainPath)
$originalPlatform = 'set(CMAKE_GENERATOR_PLATFORM "x64" CACHE STRING "" FORCE)'
# Add /machine:x64 only when missing and without a leading space: FidelityFX's platform check wants exactly
# "/machine:x64" (GitHub #30). Folders patched by an earlier run carry the old appending line and are upgraded.
$ninjaLinker = @'
if(NOT CMAKE_EXE_LINKER_FLAGS MATCHES "/machine:x64")
            string(STRIP "${CMAKE_EXE_LINKER_FLAGS} /machine:x64" CMAKE_EXE_LINKER_FLAGS)
        endif()
'@
$ninjaLinkerMark = 'string(STRIP "${CMAKE_EXE_LINKER_FLAGS} /machine:x64" CMAKE_EXE_LINKER_FLAGS)'
$oldNinjaLinker = 'set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /machine:x64")'
$ninjaPlatform = @'
if(CMAKE_GENERATOR STREQUAL "Ninja")
        set(CMAKE_GENERATOR_PLATFORM "" CACHE STRING "" FORCE)
        @LINKER@
    else()
        set(CMAKE_GENERATOR_PLATFORM "x64" CACHE STRING "" FORCE)
    endif()
'@
$ninjaPlatform = $ninjaPlatform.Replace('@LINKER@', $ninjaLinker)
if ($toolchainText.Contains($oldNinjaLinker)) {
    [IO.File]::WriteAllText($toolchainPath, $toolchainText.Replace($oldNinjaLinker, $ninjaLinker))
} elseif (-not $toolchainText.Contains($ninjaLinkerMark)) {
    if (-not $toolchainText.Contains($originalPlatform)) { throw 'Pinned SDK toolchain layout changed.' }
    [IO.File]::WriteAllText($toolchainPath, $toolchainText.Replace($originalPlatform, $ninjaPlatform))
}
# A string patch to a pinned SDK file: the anchor is replaced, or the replacement is already there (an earlier run).
# Neither means the file is not the one pinned, and the build stops instead of building it unpatched.
function Update-PinnedText([string]$text, [string]$anchor, [string]$replacement, [string]$file) {
    if ($text.Contains($anchor)) { return $text.Replace($anchor, $replacement) }
    if ($text.Contains($replacement)) { return $text }
    throw "Pinned SDK file $file changed: '$anchor' not found."
}
$pixCmakePath = Join-Path $sdkRoot 'sdk/libs/pix/CMakeLists.txt'
$pixCmake = [IO.File]::ReadAllText($pixCmakePath)
[IO.File]::WriteAllText($pixCmakePath, (Update-PinnedText $pixCmake '${CMAKE_GENERATOR_PLATFORM}' '${CMAKE_VS_PLATFORM_NAME}' $pixCmakePath))
$sdkBuild = Join-Path $workspaceRoot 'artifacts/native/fidelityfx'
# SDK 1.1.4 appends output paths to the output-variable name itself. Ninja
# correctly rejects the resulting literal dependency. Preserve the name and
# accumulate paths separately (no shader or runtime behavior changes).
$compilePath = Join-Path $sdkRoot 'sdk/include/FidelityFX/gpu/CMakeCompileShaders.txt'
$compileText = [IO.File]::ReadAllText($compilePath)
$compileText = Update-PinnedText $compileText 'list(APPEND PERMUTATION_OUTPUTS ' 'list(APPEND LOCAL_PERMUTATION_OUTPUTS ' $compilePath
$compileText = Update-PinnedText $compileText 'set(${PERMUTATION_OUTPUTS} ${PERMUTATION_OUTPUTS} PARENT_SCOPE)' 'set(${PERMUTATION_OUTPUTS} ${LOCAL_PERMUTATION_OUTPUTS} PARENT_SCOPE)' $compilePath
[IO.File]::WriteAllText($compilePath, $compileText)
# Reproducible binaries: /Brepro on every compile and link, so two builds of the same sources give the same bytes (the
# PE time stamp becomes a hash of the content) and a rebuild of unchanged sources adds no line to the list of known
# components. The flags are given to the configuration, not put in a patch: the patch set stays what the fork's licence
# covers. CMake takes *_FLAGS_INIT only when it creates a build folder, so a folder configured without the flag is
# deleted first and the cache is checked after the configuration.
$reproFlags = @('-DCMAKE_C_FLAGS_INIT=/Brepro', '-DCMAKE_CXX_FLAGS_INIT=/Brepro', '-DCMAKE_SHARED_LINKER_FLAGS_INIT=/Brepro',
    '-DCMAKE_MODULE_LINKER_FLAGS_INIT=/Brepro', '-DCMAKE_EXE_LINKER_FLAGS_INIT=/Brepro')
$reproCacheKeys = @('CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS', 'CMAKE_SHARED_LINKER_FLAGS', 'CMAKE_EXE_LINKER_FLAGS')
function Test-ReproCache([string]$Build) {
    $cache = Join-Path $Build 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cache)) { return $false }
    # A project without C sources has no CMAKE_C_FLAGS: a key that is absent is not a fault, one without the flag is.
    $lines = Get-Content -LiteralPath $cache
    foreach ($key in $reproCacheKeys) {
        $entry = @($lines -match "^${key}:[A-Z]+=")
        if ($entry.Count -and $entry[0] -notmatch '/Brepro') { return $false }
        if (-not $entry.Count -and $key -ne 'CMAKE_C_FLAGS') { return $false }
    }
    return $true
}
function Reset-BuildFolderWithoutRepro([string]$Build) {
    if ((Test-Path -LiteralPath (Join-Path $Build 'CMakeCache.txt')) -and -not (Test-ReproCache $Build)) {
        Write-Host "$Build was configured without /Brepro: deleted, the build starts from an empty folder."
        Remove-Item -LiteralPath $Build -Recurse -Force
    }
}
Reset-BuildFolderWithoutRepro $sdkBuild
& $wrapper cmake -S "$sdkRoot/sdk" -B $sdkBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFX_ALL=OFF -DFFX_OF=ON -DFFX_API_BACKEND=DX12_X64 -DFFX_BUILD_AS_DLL=OFF @reproFlags
if ($LASTEXITCODE -ne 0) { throw 'FidelityFX configuration failed.' }
if (-not (Test-ReproCache $sdkBuild)) { throw "FidelityFX is not configured with /Brepro (see CMakeCache.txt in $sdkBuild)." }
& $wrapper cmake --build $sdkBuild --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'FidelityFX build failed.' }
# --- djules75 fork of OFXR Bridge (DCS-capable since 0.2.6), pinned to tag 0.2.9.1 ---
$forkRoot = Join-Path $workspaceRoot 'external/ofxr-djules75'
$forkTag = '0.2.9.1'
$forkCommit = '8cdee48ec97afc41cff0a48c68a44566643c9943'
if (-not (Test-Path -LiteralPath $forkRoot)) {
    & git clone --branch $forkTag --depth 1 https://github.com/djules75/OFXR-Bridge.git $forkRoot
    if ($LASTEXITCODE -ne 0) { throw 'OFXR fork download failed.' }
}
$forkRevision = & git -C $forkRoot rev-parse HEAD
if ($forkRevision -ne $forkCommit) { throw "Unexpected OFXR fork revision $forkRevision (expected $forkCommit for $forkTag)." }
# The fork's layer needs openvr.h (FPS overlay reads SteamVR delivery stats). Same pin as its CI.
$openvrHeader = Join-Path $forkRoot 'external/openvr/openvr.h'
$openvrSha256 = '94E5545370159C85F87CD6E15DD3739F7C919FC7A6E869F5E4ED463533A07ED0'
if (-not (Test-Path -LiteralPath $openvrHeader)) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $openvrHeader) | Out-Null
    & curl.exe -sSL -o $openvrHeader 'https://raw.githubusercontent.com/ValveSoftware/openvr/v2.5.1/headers/openvr.h'
    if ($LASTEXITCODE -ne 0) { throw 'openvr.h download failed.' }
}
if ((Get-FileHash -LiteralPath $openvrHeader -Algorithm SHA256).Hash -ne $openvrSha256) { throw 'openvr.h hash mismatch.' }
# The fork fixes the DCS second-thread xrWaitFrame deadlock itself, and defer_until_submitted is not
# ported. DCS Control's own changes (the in-headset diagnostic panel) are kept as source patches in
# patches/ofxr-djules75 (LGPL: the corresponding-source package carries them) and applied here in name
# order to the pinned checkout. A patch that is already applied is left as it is, so re-running is safe;
# a checkout that matches neither state stops the build. Restore a clean checkout with
# `git -C external/ofxr-djules75 checkout -- .` plus `git -C external/ofxr-djules75 clean -n` (review, then -f).
$forkPatches = @(Get-ChildItem -LiteralPath (Join-Path $workspaceRoot 'patches/ofxr-djules75') -Filter '*.patch' -File -ErrorAction SilentlyContinue | Sort-Object Name)
if ($forkPatches.Count -eq 0) { throw 'No OFXR fork patches found in patches/ofxr-djules75: the build would ship the fork without DCS Control''s changes.' }
function Test-ForkPatch([string]$patchFile, [switch]$Reverse) {
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $arguments = @('-C', $forkRoot, 'apply', '--check', '--whitespace=nowarn')
        if ($Reverse) { $arguments += '--reverse' }
        & git @arguments $patchFile 2>&1 | Out-Null
        return $LASTEXITCODE -eq 0
    } finally { $ErrorActionPreference = $savedPreference }
}
# Patches are LF; a checkout with core.autocrlf may have turned them into CRLF.
$normalizedPatches = @($forkPatches | ForEach-Object {
    $temp = [IO.Path]::GetTempFileName()
    [IO.File]::WriteAllText($temp, [IO.File]::ReadAllText($_.FullName).Replace("`r`n", "`n"))
    [pscustomobject]@{ Name = $_.Name; FullName = $temp }
})
# The patch set applied last is recorded in the fork's .git folder. When any patch changed since (an earlier one edited,
# for example), the checkout goes back to the pinned sources and every patch is applied again: finding only the newest
# applied patch would take an edited earlier one as applied and build stale code. Ignored files (external/openvr) stay.
# The hash and the marker's place are shared with scripts/assert-components-current.ps1 (scripts/build-stamp.ps1).
. (Join-Path $PSScriptRoot 'build-stamp.ps1')
$patchSet = Get-OfxrPatchSetHash
$patchMarker = Get-OfxrPatchMarkerPath $forkRoot
$recordedSet = if (Test-Path -LiteralPath $patchMarker) { (Get-Content -LiteralPath $patchMarker -Raw).Trim() } else { $null }
if ($recordedSet -and $recordedSet -ne $patchSet) {
    Write-Host 'OFXR fork patches changed since they were applied: starting again from the pinned sources.'
    & git -C $forkRoot checkout -- .
    if ($LASTEXITCODE -ne 0) { throw 'OFXR fork source reset failed.' }
    & git -C $forkRoot clean -fdq
    if ($LASTEXITCODE -ne 0) { throw 'OFXR fork source reset failed.' }
}
try {
    # A later patch may edit files an earlier one created, so earlier patches no longer reverse-apply on a fully
    # patched tree. Find the newest applied patch first; everything before it counts as applied.
    $appliedThrough = -1
    for ($i = $normalizedPatches.Count - 1; $i -ge 0; $i--) {
        if (Test-ForkPatch $normalizedPatches[$i].FullName -Reverse) { $appliedThrough = $i; break }
    }
    for ($i = 0; $i -lt $normalizedPatches.Count; $i++) {
        $forkPatch = $normalizedPatches[$i]
        if ($i -le $appliedThrough) { Write-Host "OFXR fork patch $($forkPatch.Name) already applied."; continue }
        if (-not (Test-ForkPatch $forkPatch.FullName)) { throw "OFXR fork patch $($forkPatch.Name) neither applies to nor is applied in $forkRoot; restore a clean checkout." }
        & git -C $forkRoot apply --whitespace=nowarn $forkPatch.FullName
        if ($LASTEXITCODE -ne 0) { throw "OFXR fork patch $($forkPatch.Name) failed to apply." }
        Write-Host "Applied OFXR fork patch $($forkPatch.Name)."
    }
    Set-Content -LiteralPath $patchMarker -Value $patchSet -Encoding ascii
} finally { $normalizedPatches | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue } }
# The shared-memory contract headers the patches put in the fork must match their canonical copies: a drifted copy
# stops the build here rather than shipping a layer that reads the engine module's blocks differently.
& (Join-Path $PSScriptRoot 'check-contracts.ps1')
$forkBuild = Join-Path $workspaceRoot 'artifacts/native/ofxr-djules75'
Reset-BuildFolderWithoutRepro $forkBuild
& $wrapper cmake -S $forkRoot -B $forkBuild -G Ninja -DCMAKE_BUILD_TYPE=Release "-DXRFG_FIDELITYFX_SDK_ROOT=$sdkRoot/sdk" @reproFlags
if ($LASTEXITCODE -ne 0) { throw 'OFXR fork configuration failed.' }
if (-not (Test-ReproCache $forkBuild)) { throw "The OFXR fork is not configured with /Brepro (see CMakeCache.txt in $forkBuild)." }
& $wrapper cmake --build $forkBuild --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'OFXR fork build failed.' }
# -BuildOnly: the DLL without its tests and without a stamp (a second build to compare hashes with, for example).
if ($BuildOnly) {
    Write-Host 'OFXR fork built (-BuildOnly): no test was run and no build stamp written.'
    return
}
# Which tests run where is said by two ctest labels the patch set gives them (0091, the end of the fork's
# CMakeLists.txt), not by names in this script:
#   alone  - runs in the second phase, one at a time with no neighbour;
#   pacing - as alone, and its verdict is a pacing measure: it may be run again (see the second phase).
# The labels are read from the configured build; a build whose patch set has no such label stops here.
$testInfo = (& $wrapper ctest --test-dir $forkBuild --show-only=json-v1 | Out-String | ConvertFrom-Json).tests
$testLabels = @{}
foreach ($test in $testInfo) {
    $labels = @($test.properties | Where-Object { $_.name -eq 'LABELS' } | ForEach-Object { $_.value })
    $testLabels[[string]$test.name] = $labels
}
$aloneTests = @($testLabels.Keys | Where-Object { $testLabels[$_] -contains 'alone' })
$pacingLabelled = @($testLabels.Keys | Where-Object { $testLabels[$_] -contains 'pacing' })
if (-not $aloneTests.Count -or -not $pacingLabelled.Count) { throw 'The OFXR fork build has no test labelled alone / pacing: the patch that labels them (0091) is missing.' }
$unpaired = @($pacingLabelled | Where-Object { $testLabels[$_] -notcontains 'alone' })
if ($unpaired.Count) { throw "OFXR fork tests labelled pacing but not alone: $($unpaired -join ', ')." }
# The first phase: every test once, except the ones labelled alone, the tray tests (they create a notification icon)
# and the benches (label bench: GPU times printed, nothing required; `ctest -L bench` runs them).
# The quotes keep the '|' from cmd: msvc.cmd passes its arguments on through %*.
& $wrapper ctest --test-dir $forkBuild --output-on-failure --parallel 2 --timeout 300 -LE "`"bench|^alone$`"" -E tray_lifecycle
if ($LASTEXITCODE -ne 0) { throw 'OFXR fork tests failed.' }
. (Join-Path $PSScriptRoot 'build-stamp.ps1')
if ($env:DCSC_SKIP_TIMING_TESTS -eq '1') {
    # An intermediate build while a game is running (the pacing tests need a quiet CPU): no stamp, so
    # build-release.ps1 refuses this DLL until a build with the pacing tests has passed.
    Write-Host 'Pacing tests skipped (DCSC_SKIP_TIMING_TESTS=1): this build is not stamped for a release.'
    if ($LocalOnly) { Save-BuildStamp 'ofxr' -LocalOnly "$LocalOnly (pacing tests skipped)" }
    else { Remove-Item -LiteralPath (Get-BuildStampPath 'ofxr') -Force -ErrorAction SilentlyContinue }
    return
}
# The second phase. These tests run the layer on the WARP software device against a fake runtime with a 10 ms frame
# period (tests/layer_call_chain.cpp), so what makes one miss a deadline is CPU contention on this machine (another
# build, a game's own threads), not GPU load. A test of the retry set that fails runs again, up to 3 times in all; a
# real regression fails every time.
# The retry set is the tests labelled pacing (0091): only tests whose verdict is a pacing measure (deadlines, missed
# slots, the time of a 408) and that the retry log has shown to be load-sensitive. Everything else that runs in this
# phase (labelled alone only: the two-layer tests, warm_private_images, the release offset cases) fails the build at
# its first failure: an intermittent failure there is a race in the product, and a retry would pass it one run in three.
function Test-PacingRetryAllowed([string]$Name) { return $pacingLabelled -contains $Name }
# The line that says why a test failed, from ctest's output (--output-on-failure): the message of the first
# "CMake Error" after the test's result line, else the first line of its output, else the result line itself.
function Get-CtestFailureLine([string[]]$Output, [string]$Name) {
    $escaped = [regex]::Escape($Name)
    $resultAt = -1
    for ($i = 0; $i -lt $Output.Count; $i++) {
        if ($Output[$i] -match "Test\s+#\d+:\s+$escaped\s" -and $Output[$i] -notmatch '\sPassed\s') { $resultAt = $i; break }
    }
    if ($resultAt -lt 0) { return 'no result line in the ctest output' }
    $own = @(for ($i = $resultAt + 1; $i -lt $Output.Count; $i++) {
        if ($Output[$i] -match '^\s*(Start\s+\d+:|\d+/\d+\s+Test\s+#\d+:|\d+% tests passed)') { break }
        $Output[$i]
    })
    for ($i = 0; $i -lt $own.Count - 1; $i++) {
        if ($own[$i] -match '^CMake Error') {
            $message = @($own[($i + 1)..($own.Count - 1)] | Where-Object { $_ -and $_.Trim() }) | Select-Object -First 1
            if ($message) { return $message.Trim() }
        }
    }
    $first = @($own | Where-Object { $_ -and $_.Trim() }) | Select-Object -First 1
    if ($first) { return $first.Trim() }
    return ($Output[$resultAt] -replace '^\s*\d+/\d+\s+', '').Trim()
}
# Attempts 2 and 3 for the tests that failed attempt 1. Every attempt, the first included, is logged (date, test,
# attempt, result, the failure line) in $LogPath. $RunOne runs one test by name and returns ExitCode and Output.
# Returns one record per failed test: Name, Retryable, PassedAt (0 = never), Attempts.
function Invoke-PacingRetries([string[]]$FailedTests, [string[]]$FirstOutput, [scriptblock]$RunOne, [string]$LogPath) {
    $log = {
        param([string]$Test, [string]$Text)
        Add-Content -LiteralPath $LogPath -Encoding utf8 -Value ('{0} {1} {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Test, $Text)
    }
    foreach ($failedTest in $FailedTests) {
        $reason = Get-CtestFailureLine $FirstOutput $failedTest
        if (-not (Test-PacingRetryAllowed $failedTest)) {
            & $log $failedTest "attempt 1 failed, not retried (outside the retry set): $reason"
            [pscustomobject]@{ Name = $failedTest; Retryable = $false; PassedAt = 0; Attempts = 1 }
            continue
        }
        & $log $failedTest "attempt 1 failed: $reason"
        $passedAt = 0
        $attempt = 1
        while ($attempt -lt 3 -and -not $passedAt) {
            $attempt++
            $run = & $RunOne $failedTest
            if ($run.ExitCode -eq 0) {
                $passedAt = $attempt
                & $log $failedTest "attempt $attempt passed"
            } else {
                & $log $failedTest "attempt $attempt failed: $(Get-CtestFailureLine @($run.Output) $failedTest)"
            }
        }
        [pscustomobject]@{ Name = $failedTest; Retryable = $true; PassedAt = $passedAt; Attempts = $attempt }
    }
}
$firstOutput = @(& $wrapper ctest --test-dir $forkBuild --output-on-failure --parallel 1 --timeout 300 -LE bench -L "`"^alone$`"" | ForEach-Object { Write-Host $_; $_ })
if ($LASTEXITCODE -ne 0) {
    $failedList = Join-Path $forkBuild 'Testing/Temporary/LastTestsFailed.log'
    $failedTests = @(if (Test-Path -LiteralPath $failedList) {
        Get-Content -LiteralPath $failedList | ForEach-Object { ($_ -split ':', 2)[-1].Trim() } | Where-Object { $_ }
    })
    if ($failedTests.Count -eq 0) { throw 'OFXR fork pacing tests failed (no failed test listed).' }
    $retryLog = Join-Path $workspaceRoot 'artifacts/native/ofxr-djules75-retries.log'
    # --no-tests=error: a name that matches no test is a failure, not an empty run that exits 0.
    $runOne = {
        param([string]$name)
        $lines = @(& $wrapper ctest --test-dir $forkBuild --output-on-failure --timeout 300 --no-tests=error -R "`"^$name$`"" | ForEach-Object { Write-Host $_; $_ })
        [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $lines }
    }
    $retried = @(Invoke-PacingRetries $failedTests $firstOutput $runOne $retryLog)
    Write-Host ''
    Write-Host "OFXR fork pacing tests, second phase: $($retried.Count) failed their first attempt (log: artifacts/native/ofxr-djules75-retries.log)."
    foreach ($entry in $retried) {
        $outcome = if (-not $entry.Retryable) { 'NOT RETRIED (outside the retry set): failed' }
            elseif ($entry.PassedAt) { "needed a retry: passed at attempt $($entry.PassedAt) of 3" }
            else { 'FAILED 3 times' }
        Write-Host "  $($entry.Name): $outcome"
    }
    $broken = @($retried | Where-Object { -not $_.PassedAt })
    if ($broken.Count) {
        $brokenNames = ($broken | ForEach-Object Name) -join ', '
        if (-not $LocalOnly) { throw "OFXR fork tests failed in the second phase: $brokenNames." }
        Add-Content -LiteralPath $retryLog -Encoding utf8 -Value ('{0} local-only stamp written by the build script: {1} (failed: {2}); not for publishing' -f
            (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $LocalOnly, $brokenNames)
        Save-BuildStamp 'ofxr' -LocalOnly "$LocalOnly (failed: $brokenNames)"
        return
    }
} else {
    Write-Host 'OFXR fork pacing tests, second phase: all passed at the first attempt, nothing retried.'
}
# Built and tested: build-release.ps1 ships this DLL only while its sources and patches match this stamp.
Save-BuildStamp 'ofxr'
