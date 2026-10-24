param([Parameter(Mandatory=$true)][string]$NeuralRuntime, [string]$NgxCore)
$ErrorActionPreference='Stop'
$workspaceRoot=Split-Path -Parent $PSScriptRoot
$neuralPath=(Resolve-Path -LiteralPath $NeuralRuntime).Path
if ([IO.Path]::GetFileName($neuralPath) -ne 'nvngx_dlssnr.dll') { throw 'Select nvngx_dlssnr.dll.' }
$signature=Get-AuthenticodeSignature -LiteralPath $neuralPath
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'NVIDIA') { throw 'The neural runtime must have a valid NVIDIA signature.' }
$version=(Get-Item -LiteralPath $neuralPath).VersionInfo.FileVersion
if ($version -notmatch '^310[.,]\s*[89][.,]') { throw "The neural contract is 310.8 or 310.9; selected version: $version" }
if (!$NgxCore) {
    $NgxCore=Get-ChildItem -LiteralPath "$env:SystemRoot/System32/DriverStore/FileRepository" -Filter '_nvngx.dll' -File -Recurse | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (!$NgxCore) { throw 'No installed NVIDIA NGX core found.' }
$corePath=(Resolve-Path -LiteralPath $NgxCore).Path
$coreSignature=Get-AuthenticodeSignature -LiteralPath $corePath
$coreInfo=(Get-Item -LiteralPath $corePath).VersionInfo
$corePublisher=$coreSignature.SignerCertificate.Subject
if ($coreSignature.Status -ne 'Valid' -or $coreInfo.CompanyName -notmatch 'NVIDIA' -or $corePublisher -notmatch 'NVIDIA|Microsoft Windows Hardware Compatibility Publisher' -or [IO.Path]::GetFileName($corePath) -notin @('_nvngx.dll','nvngx.dll')) { throw 'Select a trusted NVIDIA NGX core from the installed driver.' }
$buildRoot=Join-Path $workspaceRoot 'artifacts/native/combined-gpu'
& (Join-Path $PSScriptRoot 'msvc.cmd') cmake -S (Join-Path $workspaceRoot 'tests/native') -B $buildRoot -G Ninja -DCMAKE_BUILD_TYPE=Release -DDCSVR_BUILD_GPU_SMOKE=ON
if ($LASTEXITCODE -ne 0) { throw 'GPU fixture configuration failed.' }
& (Join-Path $PSScriptRoot 'msvc.cmd') cmake --build $buildRoot --target neural_gpu -j 8
if ($LASTEXITCODE -ne 0) { throw 'GPU fixture build failed.' }
Copy-Item -LiteralPath $neuralPath -Destination (Join-Path $buildRoot 'nvngx_dlssnr.dll') -Force
$stdout=Join-Path $buildRoot 'combined.log'; $stderr=Join-Path $buildRoot 'errors.log'
$start=[Diagnostics.ProcessStartInfo]::new((Join-Path $buildRoot 'neural_gpu.exe'))
$start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
$start.ArgumentList.Add($corePath); $start.ArgumentList.Add($workspaceRoot)
$process=[Diagnostics.Process]::Start($start)
$outputTask=$process.StandardOutput.ReadToEndAsync(); $errorTask=$process.StandardError.ReadToEndAsync()
if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Combined GPU test timed out.' }
$output=$outputTask.GetAwaiter().GetResult(); $errors=$errorTask.GetAwaiter().GetResult()
$output | Set-Content -LiteralPath $stdout; $errors | Set-Content -LiteralPath $stderr
$passed=$process.ExitCode -eq 0 -and $output.Contains('PASS: real DLSS-NR -> exact Quad Views composition shaders -> NVIDIA stereo optical-flow synthesis on one GPU') -and $output.Contains('PASS: real DLSS-NR before upscaling in place on the SR input is bit-identical')
$report=[ordered]@{ passed=$passed; exitCode=$process.ExitCode; scope='Offscreen GPU stages; no DCS, OpenXR session, headset, eye tracking or DX11 hook validation'; neuralVersion=$version; neuralSha256=(Get-FileHash -LiteralPath $neuralPath).Hash.ToLowerInvariant(); ngxCore=$corePath; ngxCoreSha256=(Get-FileHash -LiteralPath $corePath).Hash.ToLowerInvariant(); log=$stdout }
$report | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $buildRoot 'result.json')
$process.Dispose()
if (!$passed) { Get-Content -LiteralPath $stderr; throw "Combined GPU test failed; inspect $stdout" }
Write-Output (Join-Path $buildRoot 'result.json')
