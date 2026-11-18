#Requires -Version 7
# Builds the offline equivalence harness (sx_equiv.exe) into -Out. Not part of the product build.
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')
$wrapper = Join-Path $root 'scripts\msvc.cmd'
New-Item -ItemType Directory -Force $Out | Out-Null
Push-Location $Out
try {
    & $wrapper cl /nologo /O2 /EHsc /std:c++17 /W3 (Join-Path $PSScriptRoot 'sx_equiv.cpp') /Fe:sx_equiv.exe
    if ($LASTEXITCODE -ne 0) { throw 'sx_equiv build failed.' }
} finally { Pop-Location }
