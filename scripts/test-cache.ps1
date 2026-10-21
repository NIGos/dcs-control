# Skips a native test run whose inputs have not changed since it last passed: the built test programs and libraries
# (Ninja relinks nothing when no source changed), the files under the given source folders, the environment the tests
# read (Test-NativeTestEnvironmentName), this script and its caller, and DCS's own files they read (a DCS update runs
# them again). A failed run leaves no stamp, so it runs again; DCSC_FORCE_TESTS=1 always runs.
#   . (Join-Path $PSScriptRoot 'test-cache.ps1')
#   $fingerprint = Get-NativeTestFingerprint $build @($testSource)   # before the run: an edit made during it runs them again
#   if (-not (Test-NativeTestsCurrent $build $fingerprint)) { ...ctest...; Save-NativeTestsPassed $build $fingerprint }
function Get-NativeTestFingerprint([string]$Build, [string[]]$Sources) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $files = @(Get-ChildItem -LiteralPath $Build -Recurse -File -Include '*.exe', '*.dll' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\(test-run|harness_out)\\' })
    foreach ($source in $Sources) { if (Test-Path -LiteralPath $source) { $files += @(Get-ChildItem -LiteralPath $source -Recurse -File) } }
    # This script and the build script that called it: a change to how the tests are run runs them again.
    $caller = Get-PSCallStack | Where-Object { $_.ScriptName -and $_.ScriptName -ne $PSCommandPath } | Select-Object -First 1
    foreach ($script in @($PSCommandPath, $caller.ScriptName)) {
        if ($script -and (Test-Path -LiteralPath $script)) { $files += Get-Item -LiteralPath $script }
    }
    $lines = @(foreach ($file in ($files | Sort-Object FullName -Unique)) {
        $stream = [IO.File]::OpenRead($file.FullName)
        try { $file.FullName.Substring($file.FullName.IndexOf('\', 3)) + ' ' + [Convert]::ToHexString($sha.ComputeHash($stream)) } finally { $stream.Dispose() }
    })
    $lines += @(Get-ChildItem env: | Where-Object { Test-NativeTestEnvironmentName $_.Name } | Sort-Object Name | ForEach-Object { "env $($_.Name)=$($_.Value)" })
    # DCS's files: size and time are enough to see an update (hashing them would take longer than some test runs).
    foreach ($bin in (Get-DcsBinsForTests)) {
        $lines += @(Get-ChildItem -LiteralPath $bin -File -Filter '*.dll' -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object { "dcs $($_.Name) $($_.Length) $($_.LastWriteTimeUtc.Ticks)" })
    }
    return [Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))))
}
# The environment the native tests read (grep getenv/GetEnvironmentVariable in native/*/test and the harnesses): DCS's
# location, the module's and layers' own switches (*_ONLY picks a subset), the OpenXR layer path one test inspects, and
# the profile folders the shadow-instance test and the module's settings lookup use.
function Test-NativeTestEnvironmentName([string]$Name) {
    if ($Name -in 'DCS_ROOT', 'DCS_BIN', 'XR_API_LAYER_PATH', 'USERPROFILE', 'LOCALAPPDATA') { return $true }
    foreach ($pattern in 'QV_*', 'DCSVR_*', 'DCSQV_*', 'PUPIL_*', '*_ONLY') { if ($Name -like $pattern) { return $true } }
    return $false
}
# Where the tests find DCS (native/dcsqvcull/test/dcs_install.h): DCS_BIN or DCS_ROOT, Steam libraries, the registry.
function Get-DcsBinsForTests {
    $bins = @()
    if ($env:DCS_BIN) { $bins += $env:DCS_BIN }
    if ($env:DCS_ROOT) { $bins += (Join-Path $env:DCS_ROOT 'bin') }
    $steam = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steam -and (Test-Path -LiteralPath (Join-Path $steam 'steamapps/libraryfolders.vdf'))) {
        foreach ($match in [regex]::Matches((Get-Content -LiteralPath (Join-Path $steam 'steamapps/libraryfolders.vdf') -Raw), '"path"\s+"([^"]+)"')) {
            $bins += (Join-Path ($match.Groups[1].Value -replace '\\\\', '\') 'steamapps/common/DCSWorld/bin')
        }
    }
    foreach ($key in 'HKCU:\Software\Eagle Dynamics\DCS World', 'HKCU:\Software\Eagle Dynamics\DCS World OpenBeta') {
        $path = (Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue).Path
        if ($path) { $bins += (Join-Path $path 'bin') }
    }
    return @($bins | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -Unique)
}
function Test-NativeTestsCurrent([string]$Build, [string]$Fingerprint) {
    if ($env:DCSC_FORCE_TESTS -eq '1') { return $false }
    $stamp = Join-Path $Build 'tests-passed.stamp'
    if (-not (Test-Path -LiteralPath $stamp)) { return $false }
    if ((Get-Content -LiteralPath $stamp -Raw).Trim() -ne $Fingerprint) { return $false }
    Write-Host "Tests unchanged since they last passed in $Build; skipped (DCSC_FORCE_TESTS=1 runs them)."
    return $true
}
# $Fingerprint is the one taken before the run (Get-NativeTestFingerprint): what the tests actually ran against.
function Save-NativeTestsPassed([string]$Build, [string]$Fingerprint) {
    Set-Content -LiteralPath (Join-Path $Build 'tests-passed.stamp') -Value $Fingerprint -Encoding ascii
}
