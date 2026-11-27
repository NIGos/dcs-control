#Requires -Version 7
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$target = 'x86_64-pc-windows-msvc'
$previous = $env:CARGO_TARGET_DIR
$env:CARGO_TARGET_DIR = Join-Path $root 'artifacts/native/dcs-cpu-opt'
Push-Location (Join-Path $root 'native/dcs-cpu-opt')
try {
    cargo test --release --locked --target $target --lib
    if ($LASTEXITCODE -ne 0) { throw 'DCS CPU Opt tests failed.' }
    cargo build --release --locked --target $target
    if ($LASTEXITCODE -ne 0) { throw 'DCS CPU Opt build failed.' }
} finally {
    Pop-Location
    $env:CARGO_TARGET_DIR = $previous
}
