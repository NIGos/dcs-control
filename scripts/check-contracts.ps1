#Requires -Version 7
# The shared-memory contract headers (scripts/contracts.json) must be identical in every copy: the canonical one, each
# copy, and the hash recorded in the manifest, compared without CR and a UTF-8 BOM. A copy that does not exist yet (the
# OFXR tree before its first build) is skipped and listed. -Update records the canonical copies' hashes, after both
# sides agreed a change. -Root checks another checkout (the engine module's build reads ours read-only). -Only checks
# only the copies whose path starts with one of the given prefixes (e.g. -Only native/: the native builds leave the OFXR
# tree's copies to the OFXR build).
param([switch]$Update, [string]$Root = (Split-Path -Parent $PSScriptRoot), [string[]]$Only)
$ErrorActionPreference = 'Stop'
$manifestPath = Join-Path $PSScriptRoot 'contracts.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
function Get-NormalisedHash([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $start = if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { 3 } else { 0 }
    $kept = [Collections.Generic.List[byte]]::new($bytes.Length)
    for ($i = $start; $i -lt $bytes.Length; $i++) { if ($bytes[$i] -ne 13) { $kept.Add($bytes[$i]) } }
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($kept.ToArray()))
}
function Test-Selected([string]$path) { -not $Only -or @($Only | Where-Object { $path.Replace('\', '/').StartsWith($_.Replace('\', '/'), [StringComparison]::OrdinalIgnoreCase) }).Count -gt 0 }
$problems = [Collections.Generic.List[string]]::new(); $skipped = [Collections.Generic.List[string]]::new()
foreach ($contract in $manifest.contracts) {
    $canonical = Join-Path $Root $contract.canonical
    if (-not (Test-Path -LiteralPath $canonical)) { $problems.Add("$($contract.name): canonical copy missing ($($contract.canonical))"); continue }
    $hash = Get-NormalisedHash $canonical
    if ($Update) { $contract.sha256 = $hash }
    elseif ((Test-Selected $contract.canonical) -and $contract.sha256 -ne $hash) { $problems.Add("$($contract.name): $($contract.canonical) differs from the manifest (agree the change, then run with -Update)") }
    foreach ($copy in $contract.copies) {
        if (-not (Test-Selected $copy)) { continue }
        $path = Join-Path $Root $copy
        if (-not (Test-Path -LiteralPath $path)) { $skipped.Add("$($contract.name): $copy (not present)"); continue }
        if ((Get-NormalisedHash $path) -ne $hash) { $problems.Add("$($contract.name): $copy differs from $($contract.canonical)") }
    }
}
if ($Update) {
    ($manifest | ConvertTo-Json -Depth 5) + "`n" | Set-Content -LiteralPath $manifestPath -NoNewline -Encoding utf8NoBOM
    Write-Host "Contract hashes recorded from the canonical copies."
}
foreach ($line in $skipped) { Write-Host "SKIP contract copy $line" }
if ($problems.Count) { foreach ($line in $problems) { Write-Host "FAIL contract $line" }; throw "Contract headers differ ($($problems.Count))." }
Write-Host "Contract headers identical ($(@($manifest.contracts).Count) contracts)."
