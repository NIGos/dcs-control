#Requires -Version 7
# Frees space under artifacts/. Run by hand; by default it only lists what would go (and the size), -Apply does it.
# - artifacts/release: keeps the newest 3 releases (each one's folder, ZIPs, setup, checksums and signatures) and the
#   older releases the release checks read: 0.3.0 (test-release.ps1's upgrade fixture) and 0.2.3
#   (verify-web-release.py's unchanged-components baseline).
# - artifacts/dcsqvcull-snap-r*: keeps the newest 5 vendoring snapshots.
# - artifacts/web-ui: keeps the newest 5 interface render folders.
# - loose *.log files in artifacts/ move to artifacts/logs/ (nothing deleted).
param([switch]$Apply, [switch]$WhatIf, [int]$KeepReleases = 3, [int]$KeepSnapshots = 5, [int]$KeepRenders = 5)
$ErrorActionPreference = 'Stop'
$dryRun = $WhatIf -or -not $Apply
$artifacts = Join-Path (Split-Path -Parent $PSScriptRoot) 'artifacts'
$fixtureVersions = @('0.3.0-preview', '0.2.3-preview')

function Get-Size($item) {
    if ($item.PSIsContainer) { [long](Get-ChildItem -LiteralPath $item.FullName -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum }
    else { [long]$item.Length }
}
function Format-Size([long]$bytes) { if ($bytes -ge 1GB) { '{0:N2} GB' -f ($bytes / 1GB) } else { '{0:N1} MB' -f ($bytes / 1MB) } }

$remove = [Collections.Generic.List[object]]::new()
$sections = [ordered]@{}

# Releases: everything named <Product>-<version>-..., grouped by version, newest folder first.
$releaseRoot = Join-Path $artifacts 'release'
if (Test-Path -LiteralPath $releaseRoot) {
    $pattern = '^(?:DcsControl|DcsVrControl)-(?<version>\d+\.\d+\.\d+(?:-[a-z]+)?)-'
    $groups = Get-ChildItem -LiteralPath $releaseRoot -Force | Where-Object { $_.Name -match $pattern } |
        Group-Object { [regex]::Match($_.Name, $pattern).Groups['version'].Value }
    $ordered = @($groups | Sort-Object { ($_.Group | Measure-Object LastWriteTimeUtc -Maximum).Maximum } -Descending)
    $sections['release'] = @($ordered | Select-Object -Skip $KeepReleases | Where-Object { $_.Name -notin $fixtureVersions } | ForEach-Object { $_.Group })
}
$sections['dcsqvcull snapshots'] = @(Get-ChildItem -LiteralPath $artifacts -Directory -Filter 'dcsqvcull-snap-r*' |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -Skip $KeepSnapshots)
$webUi = Join-Path $artifacts 'web-ui'
if (Test-Path -LiteralPath $webUi) {
    $sections['web-ui renders'] = @(Get-ChildItem -LiteralPath $webUi -Directory | Sort-Object LastWriteTimeUtc -Descending | Select-Object -Skip $KeepRenders)
}

$total = 0L
foreach ($name in $sections.Keys) {
    $items = @($sections[$name]); $size = 0L
    foreach ($item in $items) { $itemSize = Get-Size $item; $size += $itemSize; Write-Host ("  {0,-10} {1}" -f (Format-Size $itemSize), $item.FullName.Substring($artifacts.Length + 1)) }
    Write-Host ("{0}: {1} item(s), {2}" -f $name, $items.Count, (Format-Size $size))
    $total += $size; $remove.AddRange([object[]]$items)
}
$logs = @(Get-ChildItem -LiteralPath $artifacts -File -Filter '*.log')
$logSize = [long]($logs | Measure-Object Length -Sum).Sum
Write-Host ("loose logs: {0} file(s), {1} -> artifacts/logs/" -f $logs.Count, (Format-Size $logSize))
Write-Host ("Total to delete: {0}" -f (Format-Size $total))

if ($dryRun) { Write-Host 'Nothing changed (listing only; -Apply deletes and moves).'; return }
foreach ($item in $remove) {
    $full = [IO.Path]::GetFullPath($item.FullName)
    if (-not $full.StartsWith([IO.Path]::GetFullPath($artifacts) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to delete outside artifacts: $full" }
    Remove-Item -LiteralPath $full -Recurse -Force
}
if ($logs.Count) {
    $logFolder = New-Item -ItemType Directory -Path (Join-Path $artifacts 'logs') -Force
    foreach ($log in $logs) { Move-Item -LiteralPath $log.FullName -Destination (Join-Path $logFolder.FullName $log.Name) -Force }
}
Write-Host ("Deleted {0}; moved {1} log(s)." -f (Format-Size $total), $logs.Count)
