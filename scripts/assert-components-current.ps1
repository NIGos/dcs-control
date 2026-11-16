#Requires -Version 7
# Fails unless the patched upstream trees in external\ are the ones the checked-out scripts and patches produce:
# Quad Views, OFXR and Cheeky were each built from them (scripts/build-stamp.ps1), and the OFXR fork carries exactly
# this patch set (the marker scripts/build-ofxr-djules75.ps1 leaves in the fork's .git folder).
# scripts/package-sources.py runs it before it packs external\*: the sources archive must match what ships, and
# external\ holds whatever was built last, on any branch.
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'build-stamp.ps1')
foreach ($component in @('quadviews', 'ofxr', 'cheeky')) { Assert-BuildStampCurrent $component }
# The patch set, computed as build-ofxr-djules75.ps1 computes it: every patch by name, LF line endings.
$forkRoot = Join-Path $workspaceRoot 'external/ofxr-djules75'
$patches = @(Get-ChildItem -LiteralPath (Join-Path $workspaceRoot 'patches/ofxr-djules75') -Filter '*.patch' -File | Sort-Object Name)
$sha = [Security.Cryptography.SHA256]::Create()
$text = ($patches | ForEach-Object { $_.Name + "`n" + [IO.File]::ReadAllText($_.FullName).Replace("`r`n", "`n") }) -join "`n"
$patchSet = [Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))
$gitDir = & git -C $forkRoot rev-parse --absolute-git-dir
if ($LASTEXITCODE -ne 0) { throw 'external/ofxr-djules75 is not a git checkout.' }
$marker = Join-Path $gitDir 'dcs-control-patches'
$recorded = if (Test-Path -LiteralPath $marker) { (Get-Content -LiteralPath $marker -Raw).Trim() } else { $null }
if ($recorded -ne $patchSet) {
    throw "external/ofxr-djules75 does not carry the patches checked out here ($($patches.Count) in patches/ofxr-djules75). Run scripts/build-ofxr-djules75.ps1, then try again."
}
Write-Output "Components current: Quad Views, OFXR ($($patches.Count) patches) and Cheeky were built from this checkout."
