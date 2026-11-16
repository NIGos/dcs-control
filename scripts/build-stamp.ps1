# Records which sources and patches a prebuilt component was built from, so build-release.ps1 refuses a DLL that is
# older than them (it copies Quad Views, OFXR and Cheeky as built, without rebuilding them).
#   . (Join-Path $PSScriptRoot 'build-stamp.ps1')
#   Save-BuildStamp 'quadviews'                       # at the end of a successful build
#   Assert-BuildStampCurrent 'quadviews'              # in build-release.ps1
#   Save-BuildStamp 'ofxr' -LocalOnly 'why'           # a build for this machine only (see below)
$BuildStampInputs = @{
    quadviews = @('scripts/build-quadviews.ps1', 'native/quadviews')
    ofxr      = @('scripts/build-ofxr-djules75.ps1', 'patches/ofxr-djules75')
    cheeky    = @('scripts/build-upstream.ps1', 'scripts/patch-cheeky.py', 'patches/cheeky')
}
$BuildStampScripts = @{ quadviews = 'scripts/build-quadviews.ps1'; ofxr = 'scripts/build-ofxr-djules75.ps1'; cheeky = 'scripts/build-upstream.ps1 -Component cheeky' }
function Get-BuildInputsFingerprint([string]$Name) {
    $root = Split-Path -Parent $PSScriptRoot
    $sha = [Security.Cryptography.SHA256]::Create()
    $files = foreach ($item in $BuildStampInputs[$Name]) {
        $path = Join-Path $root $item
        if (Test-Path -LiteralPath $path -PathType Leaf) { Get-Item -LiteralPath $path }
        elseif (Test-Path -LiteralPath $path) { Get-ChildItem -LiteralPath $path -Recurse -File | Where-Object { $_.FullName -notmatch '\\__pycache__\\' } }
    }
    # Line endings do not count: a checkout with core.autocrlf builds the same component.
    $lines = foreach ($file in ($files | Sort-Object FullName)) {
        $bytes = [IO.File]::ReadAllBytes($file.FullName)
        $text = [Text.Encoding]::UTF8.GetString($bytes).Replace("`r`n", "`n")
        $file.FullName.Substring($root.Length).Replace('\', '/') + ' ' + [Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))
    }
    return [Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))))
}
function Get-BuildStampPath([string]$Name) { Join-Path (Split-Path -Parent $PSScriptRoot) "artifacts/build-stamps/$Name.txt" }
# A stamp is the inputs' fingerprint on its first line. A LOCAL-ONLY stamp also has a line, "local-only <date> <why>":
# the component was built from these inputs but did not pass everything a release asks (a pacing test that failed, tests
# skipped while a game was running). It is the marked form of what was otherwise done by editing the stamp by hand:
# Assert-BuildStampCurrent refuses it unless the caller says -AllowLocalOnly, which a build to publish never does.
# Every stamp also has a line "toolset <versions>": the compiler the component was built with (Get-BuildToolset). It is
# a record, not a condition: two builds of the same inputs give the same bytes only with the same toolset, and the line
# says which one a binary came from. Nothing compares it.
function Get-BuildToolset {
    $wrapper = Join-Path $PSScriptRoot 'msvc.cmd'
    $tools = (& $wrapper cmd /c 'echo %VCToolsVersion%;%WindowsSDKVersion%' 2>$null | Select-Object -Last 1)
    $banner = (& $wrapper cl 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -match 'Version\s+[\d.]+' } | Select-Object -First 1)
    $vc, $sdk = "$tools".Trim() -split ';', 2
    $cl = if ("$banner" -match 'Version\s+([\d.]+)') { $Matches[1] } else { 'unknown' }
    if (-not $vc -or $vc -match '%') { $vc = 'unknown' }
    $sdk = if ($sdk -and $sdk -notmatch '%') { $sdk.Trim().TrimEnd('\') } else { 'unknown' }
    return "MSVC $vc, cl $cl, Windows SDK $sdk"
}
function Save-BuildStamp([string]$Name, [string]$LocalOnly) {
    $stamp = Get-BuildStampPath $Name
    New-Item -ItemType Directory -Path (Split-Path -Parent $stamp) -Force | Out-Null
    $lines = @(Get-BuildInputsFingerprint $Name)
    $lines += 'toolset ' + (Get-BuildToolset)
    if ($LocalOnly) {
        $why = ($LocalOnly -replace '\s+', ' ').Trim()
        $lines += 'local-only {0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm'), $why
        Write-Host "LOCAL-ONLY build stamp for ${Name}: $why. build-release.ps1 refuses it; not for publishing."
    }
    Set-Content -LiteralPath $stamp -Value $lines -Encoding ascii
}
# Fingerprint = the first line (null without a stamp); LocalOnly = the text after "local-only " (null for a full stamp);
# Toolset = the text after "toolset " (null in a stamp written before the line existed). Any other line makes the stamp
# unreadable, which counts as local-only.
function Get-BuildStamp([string]$Name) {
    $stamp = Get-BuildStampPath $Name
    $lines = @(if (Test-Path -LiteralPath $stamp) { Get-Content -LiteralPath $stamp | ForEach-Object { $_.Trim() } | Where-Object { $_ } })
    $rest = @($lines | Select-Object -Skip 1)
    $local = @($rest | Where-Object { $_ -like 'local-only*' }) | Select-Object -First 1
    $toolset = @($rest | Where-Object { $_ -like 'toolset *' }) | Select-Object -First 1
    $unknown = @($rest | Where-Object { $_ -notlike 'local-only*' -and $_ -notlike 'toolset *' })
    [pscustomobject]@{
        Fingerprint = if ($lines.Count) { $lines[0] } else { $null }
        LocalOnly   = if ($local) { ($local -replace '^local-only\s*', '') } elseif ($unknown.Count) { 'unreadable line in the stamp' } else { $null }
        Toolset     = if ($toolset) { ($toolset -replace '^toolset\s*', '') } else { $null }
    }
}
function Assert-BuildStampCurrent([string]$Name, [switch]$AllowLocalOnly) {
    $recorded = Get-BuildStamp $Name
    if ($recorded.Fingerprint -ne (Get-BuildInputsFingerprint $Name)) {
        throw "The prebuilt $Name component is older than its sources or patches (or was built before build stamps existed). Run $($BuildStampScripts[$Name]), then build the release again."
    }
    if ($recorded.LocalOnly) {
        if (-not $AllowLocalOnly) {
            throw "The prebuilt $Name component has a local-only build stamp ($($recorded.LocalOnly)): it did not pass everything a release asks. Run $($BuildStampScripts[$Name]) without -LocalOnly, then build the release again."
        }
        Write-Warning "The prebuilt $Name component has a LOCAL-ONLY build stamp ($($recorded.LocalOnly)): this package is not for publishing."
    }
}
# The OFXR fork's patch set as one hash: every patch in patches/ofxr-djules75 by name, with LF line endings (a checkout
# with core.autocrlf gives the same hash). scripts/build-ofxr-djules75.ps1 records it in the fork's .git folder when it
# has applied the patches (Get-OfxrPatchMarkerPath); scripts/assert-components-current.ps1 compares the two before the
# sources are packed. One function, so the two can never disagree about what "this patch set" is.
function Get-OfxrPatchSetHash {
    $patches = @(Get-ChildItem -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) 'patches/ofxr-djules75') -Filter '*.patch' -File | Sort-Object Name)
    $text = ($patches | ForEach-Object { $_.Name + "`n" + [IO.File]::ReadAllText($_.FullName).Replace("`r`n", "`n") }) -join "`n"
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($text)))
}
# Where the applied patch set is recorded: a file in the fork checkout's .git folder (never in its work tree).
function Get-OfxrPatchMarkerPath([string]$ForkRoot) {
    $gitDir = & git -C $ForkRoot rev-parse --absolute-git-dir
    if ($LASTEXITCODE -ne 0 -or -not $gitDir) { throw "$ForkRoot is not a git checkout." }
    return Join-Path $gitDir 'dcs-control-patches'
}
