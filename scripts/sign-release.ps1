# Signs release files with the release key: <file>.sig next to each, the base64 of an ECDSA P-256 / SHA-256 signature of
# the whole file in IEEE P1363 form (r and s, 32 bytes each). DCS Control's updater (src/DcsVr.Core/Updates.cs) and
# scripts/verify-web-release.py check it with the public key embedded in the app. The private key stays in
# %USERPROFILE%\.dcs-control-signing and is only read here; it is never copied anywhere.
param([Parameter(Mandatory)][string[]]$Files, [switch]$Required, [string]$KeyFile = (Join-Path $env:USERPROFILE '.dcs-control-signing\release-signing-key.pem'))
$ErrorActionPreference = 'Stop'
# A signature from an earlier build of the same name would no longer match.
foreach ($file in $Files) { Remove-Item -LiteralPath ($file + '.sig') -ErrorAction SilentlyContinue }
if (-not (Test-Path -LiteralPath $KeyFile)) {
    if ($Required) { throw "Release signing key not found: $KeyFile. A release built with -Sign must be signed (developer builds leave -Sign out)." }
    Write-Warning "Release signing key not found ($KeyFile): the setup and the ZIP are not signed. DCS Control's updater refuses an unsigned setup."
    return
}
# ECDsa.ImportFromPem needs .NET 5 or later, so PowerShell 7; Windows PowerShell hands each file over to it.
if ($PSVersionTable.PSVersion.Major -lt 7) {
    $pwsh = Get-Command pwsh.exe -ErrorAction SilentlyContinue
    if (-not $pwsh) { throw 'Signing needs PowerShell 7 (pwsh.exe).' }
    foreach ($file in $Files) {
        & $pwsh.Source -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Files $file -KeyFile $KeyFile -Required
        if ($LASTEXITCODE -ne 0) { throw "Signing failed: $file" }
    }
    return
}
# Every signature is checked with the public key the app embeds: one it would refuse never ships.
$source = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) 'src/DcsVr.Core/Updates.cs') -Raw
$publicPem = [regex]::Match($source, '-----BEGIN PUBLIC KEY-----[\s\S]+?-----END PUBLIC KEY-----').Value -replace '(?m)^[ \t]+', ''
$sha256 = [Security.Cryptography.HashAlgorithmName]::SHA256
$p1363 = [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation
$key = [Security.Cryptography.ECDsa]::Create(); $verifier = [Security.Cryptography.ECDsa]::Create()
try {
    $key.ImportFromPem([IO.File]::ReadAllText($KeyFile)); $verifier.ImportFromPem($publicPem)
    if ($key.KeySize -ne 256) { throw 'The release key is not an ECDSA P-256 key.' }
    foreach ($file in $Files) {
        $stream = [IO.File]::OpenRead($file)
        try { $signature = $key.SignData($stream, $sha256, $p1363) } finally { $stream.Dispose() }
        $stream = [IO.File]::OpenRead($file)
        try { $valid = $verifier.VerifyData($stream, $signature, $sha256, $p1363) } finally { $stream.Dispose() }
        if (-not $valid) { throw "The signing key does not belong to the public key embedded in DCS Control (src/DcsVr.Core/Updates.cs): $file" }
        [IO.File]::WriteAllText($file + '.sig', [Convert]::ToBase64String($signature), [Text.Encoding]::ASCII)
        Write-Output "Signed $file"
    }
} finally { $key.Dispose(); $verifier.Dispose() }
