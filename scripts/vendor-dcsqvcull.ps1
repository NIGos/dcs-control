#Requires -Version 7
# Vendors a DcsQvCull round from the peer's repository into native/dcsqvcull (the manual steps, automated):
# 1. snapshots native/dcsqvcull/{src,test,lua,tools} to artifacts/dcsqvcull-snap-<Label>;
# 2. copies every peer source file (*.h *.hpp *.c *.cpp *.inc *.lua under src, test, lua) that is new or differs (line
#    endings and a UTF-8 BOM ignored), plus tools/vrfx_rig.h and tools/README_hook_sigs.md; build products, the stress
#    harness (stress_main.cpp) and the cockpit mask bench (cockpit_mask_bench.cpp) stay out;
# 3. lists files that exist only on our side (never deletes them);
# 4. runs the personal-data check on the copied files and the contract check on the native/ copies.
# -WhatIf lists what would be copied and checks the peer's files, changing nothing.
#   .\scripts\vendor-dcsqvcull.ps1 -Peer <peer repo>\DcsQvCull -Label r102 [-WhatIf]
param([Parameter(Mandatory)][string]$Peer, [Parameter(Mandatory)][string]$Label, [switch]$WhatIf)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ours = Join-Path $root 'native/dcsqvcull'
$Peer = (Resolve-Path -LiteralPath $Peer).Path
if (-not (Test-Path -LiteralPath (Join-Path $Peer 'src'))) { throw "$Peer has no src folder: pass the DcsQvCull folder of the peer's repository." }
if ($Label -notmatch '^[A-Za-z0-9._-]+$') { throw "Label '$Label': letters, digits, '.', '_' and '-' only." }

$sourceExtensions = '.h', '.hpp', '.c', '.cpp', '.inc', '.lua'
$excludedNames = 'stress_main.cpp', 'cockpit_mask_bench.cpp'
$extraTools = 'tools/vrfx_rig.h', 'tools/README_hook_sigs.md'
# Build products and caches anywhere under the scanned folders.
$excludedFolder = '[\\/](build[^\\/]*|out|bin|obj|x64|Release|Debug|__pycache__|\.vs)[\\/]'

function Get-Normalised([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $start = if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { 3 } else { 0 }
    $kept = [Collections.Generic.List[byte]]::new($bytes.Length)
    for ($i = $start; $i -lt $bytes.Length; $i++) { if ($bytes[$i] -ne 13) { $kept.Add($bytes[$i]) } }
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($kept.ToArray()))
}
function Get-Sources([string]$base) {
    $list = @()
    foreach ($folder in 'src', 'test', 'lua') {
        $path = Join-Path $base $folder
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $list += @(Get-ChildItem -LiteralPath $path -Recurse -File | Where-Object {
            $_.Extension.ToLowerInvariant() -in $sourceExtensions -and $_.Name -notin $excludedNames -and
            $_.FullName.Substring($base.Length) -notmatch $excludedFolder
        } | ForEach-Object { [IO.Path]::GetRelativePath($base, $_.FullName).Replace('\', '/') })
    }
    $list += @($extraTools | Where-Object { Test-Path -LiteralPath (Join-Path $base $_) })
    return @($list | Sort-Object -Unique)
}

$peerFiles = Get-Sources $Peer
$ourFiles = Get-Sources $ours
$added = [Collections.Generic.List[string]]::new(); $changed = [Collections.Generic.List[string]]::new()
foreach ($relative in $peerFiles) {
    $target = Join-Path $ours $relative
    if (-not (Test-Path -LiteralPath $target)) { $added.Add($relative) }
    elseif ((Get-Normalised (Join-Path $Peer $relative)) -ne (Get-Normalised $target)) { $changed.Add($relative) }
}
$onlyOurs = @($ourFiles | Where-Object { $_ -notin $peerFiles })

Write-Host "Peer: $Peer ($($peerFiles.Count) source files); ours: native/dcsqvcull ($($ourFiles.Count))."
foreach ($relative in $changed) { Write-Host "  changed  $relative" }
foreach ($relative in $added) { Write-Host "  new      $relative" }
foreach ($relative in $onlyOurs) { Write-Host "  ours only (kept, check by hand) $relative" }
$copy = @($changed) + @($added)
if (-not $copy.Count) { Write-Host 'Nothing to copy: native/dcsqvcull matches the peer.' }

if ($WhatIf) {
    if ($copy.Count) {
        & (Join-Path $PSScriptRoot 'check-personal-data.ps1') -Paths @($copy | ForEach-Object { Join-Path $Peer $_ })
        if ($LASTEXITCODE -ne 0) { Write-Host 'The peer files above hold personal data: fix them before vendoring.' }
    }
    Write-Host "WhatIf: nothing snapshotted or copied ($($copy.Count) file(s) would be copied)."
    return
}
if (-not $copy.Count) { & (Join-Path $PSScriptRoot 'check-contracts.ps1') -Only 'native/'; return }

$snapshot = Join-Path $root "artifacts/dcsqvcull-snap-$Label"
if (Test-Path -LiteralPath $snapshot) { throw "$snapshot exists already: pick another label." }
foreach ($folder in 'src', 'test', 'lua', 'tools') {
    if (Test-Path -LiteralPath (Join-Path $ours $folder)) { Copy-Item -LiteralPath (Join-Path $ours $folder) -Destination (Join-Path $snapshot $folder) -Recurse }
}
Write-Host "Snapshot: $snapshot"
foreach ($relative in $copy) {
    $target = Join-Path $ours $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $Peer $relative) -Destination $target -Force
}
Write-Host "Copied $($copy.Count) file(s) ($($changed.Count) changed, $($added.Count) new)."
& (Join-Path $PSScriptRoot 'check-personal-data.ps1') -Paths @($copy | ForEach-Object { Join-Path $ours $_ })
$personal = $LASTEXITCODE
& (Join-Path $PSScriptRoot 'check-contracts.ps1') -Only 'native/'
if ($personal -ne 0) { throw 'Vendored files hold personal data (listed above): fix them here and tell the peer.' }
