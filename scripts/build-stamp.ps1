# Records which sources and patches a prebuilt component was built from, so build-release.ps1 refuses a DLL that is
# older than them (it copies Quad Views, OFXR and Cheeky as built, without rebuilding them).
#   . (Join-Path $PSScriptRoot 'build-stamp.ps1')
#   Save-BuildStamp 'quadviews'                       # at the end of a successful build
#   Assert-BuildStampCurrent 'quadviews'              # in build-release.ps1
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
function Save-BuildStamp([string]$Name) {
    $stamp = Get-BuildStampPath $Name
    New-Item -ItemType Directory -Path (Split-Path -Parent $stamp) -Force | Out-Null
    Set-Content -LiteralPath $stamp -Value (Get-BuildInputsFingerprint $Name) -Encoding ascii
}
function Assert-BuildStampCurrent([string]$Name) {
    $stamp = Get-BuildStampPath $Name
    $recorded = if (Test-Path -LiteralPath $stamp) { (Get-Content -LiteralPath $stamp -Raw).Trim() } else { $null }
    if ($recorded -ne (Get-BuildInputsFingerprint $Name)) {
        throw "The prebuilt $Name component is older than its sources or patches (or was built before build stamps existed). Run $($BuildStampScripts[$Name]), then build the release again."
    }
}
