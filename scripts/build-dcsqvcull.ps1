# Builds the DCS engine optimizations (native/dcsqvcull: loader and payload DLLs) and runs their offline tests.
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$wrapper = Join-Path $PSScriptRoot 'msvc.cmd'
$build = Join-Path $workspaceRoot 'artifacts/native/dcsqvcull'
& $wrapper cmake -S (Join-Path $workspaceRoot 'native/dcsqvcull') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'DcsQvCull configuration failed.' }
& $wrapper cmake --build $build --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'DcsQvCull build failed.' }
# The tests write reports and bench files into their working folder and read back the newest: they start from an
# empty one, so a report left by an earlier run is never read instead of this run's.
$testRun = Join-Path $build 'test-run'
if (Test-Path -LiteralPath $testRun) { Get-ChildItem -LiteralPath $testRun -Force | Remove-Item -Recurse -Force }
& $wrapper ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'DcsQvCull tests failed.' }
