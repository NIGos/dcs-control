$ErrorActionPreference='Stop'
$workspaceRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'build-jobs.ps1')  # Get-BuildJobs: the compile jobs at once
$wrapper=Join-Path $PSScriptRoot 'msvc.cmd'
$loaderRoot=Join-Path $workspaceRoot 'artifacts/native/openxr-loader'
& $wrapper cmake -S "$workspaceRoot/external/quadviews/external/OpenXR-SDK" -B $loaderRoot -G Ninja -DCMAKE_BUILD_TYPE=Release '-DCMAKE_POLICY_VERSION_MINIMUM=3.5.0' -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF
if($LASTEXITCODE -ne 0){throw 'Loader configuration failed.'}
& $wrapper cmake --build $loaderRoot --parallel (Get-BuildJobs)
if($LASTEXITCODE -ne 0){throw 'Loader build failed.'}
$smokeRoot=Join-Path $workspaceRoot 'artifacts/native/loader-smoke'
& $wrapper cmake -S "$workspaceRoot/tests/native" -B $smokeRoot -G Ninja -DCMAKE_BUILD_TYPE=Release
if($LASTEXITCODE -ne 0){throw 'Smoke configuration failed.'}
& $wrapper cmake --build $smokeRoot --parallel (Get-BuildJobs)
if($LASTEXITCODE -ne 0){throw 'Smoke build failed.'}
$fixtureRoot=Join-Path $smokeRoot 'fixture'
New-Item -ItemType Directory -Path "$fixtureRoot/layers","$fixtureRoot/appdata" -Force | Out-Null
@{file_format_version='1.0.0';runtime=@{library_path=(Join-Path $smokeRoot 'fixture_runtime.dll')}} | ConvertTo-Json | Set-Content -LiteralPath "$fixtureRoot/runtime.json" -Encoding ascii
$layerSpecs=@(
    @{name='XR_APILAYER_XRFrameBridge_diagnostic';dll="$workspaceRoot/.cache/packages/ofxr/ofxr/XR_APILAYER_XRFrameBridge_diagnostic.dll"},
    @{name='XR_APILAYER_CHEEKY_foveated_dlss';dll="$workspaceRoot/artifacts/native/cheeky/bin/Release/CheekyOpenXRLayer.dll"},
    @{name='XR_APILAYER_MBUCCHIA_quad_views_foveated';dll="$workspaceRoot/external/quadviews/bin/x64/Release/XR_APILAYER_MBUCCHIA_quad_views_foveated.dll"}
)
foreach($layer in $layerSpecs){
    $api=@{name=$layer.name;library_path=[IO.Path]::GetFullPath($layer.dll);api_version='1.0';implementation_version='1';description='Offline smoke'}
    if($layer.name -match 'quad_views'){$api.instance_extensions=@(@{name='XR_VARJO_quad_views';extension_version=1;entrypoints=@()},@{name='XR_VARJO_foveated_rendering';extension_version=3;entrypoints=@()})}
    @{file_format_version='1.0.0';api_layer=$api} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$fixtureRoot/layers/$($layer.name).json" -Encoding ascii
}
$modes=@(
    @{mode='stereo';layers='XR_APILAYER_XRFrameBridge_diagnostic'},
    @{mode='stereo';layers='XR_APILAYER_CHEEKY_foveated_dlss;XR_APILAYER_XRFrameBridge_diagnostic'},
    @{mode='quad';layers='XR_APILAYER_MBUCCHIA_quad_views_foveated;XR_APILAYER_XRFrameBridge_diagnostic'},
    @{mode='quad';layers='XR_APILAYER_CHEEKY_foveated_dlss;XR_APILAYER_MBUCCHIA_quad_views_foveated;XR_APILAYER_XRFrameBridge_diagnostic'}
)
foreach($mode in $modes){
    $start=[Diagnostics.ProcessStartInfo]::new((Join-Path $smokeRoot 'loader_smoke.exe'),$mode.mode)
    $start.UseShellExecute=$false; $start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
    $start.Environment['XR_RUNTIME_JSON']="$fixtureRoot/runtime.json"
    $start.Environment['XR_API_LAYER_PATH']="$fixtureRoot/layers"
    $start.Environment['XR_ENABLE_API_LAYERS']=$mode.layers
    $start.Environment['XRFG_DISABLE_OFXR_BRIDGE']='1'
    $start.Environment['CHEEKY_OPENXR_LAYER_DISABLE']='1'
    $start.Environment['DISABLE_XR_APILAYER_MBUCCHIA_quad_views_foveated']='1'
    $start.Environment['LOCALAPPDATA']="$fixtureRoot/appdata"
    $start.Environment['APPDATA']="$fixtureRoot/appdata"
    $start.Environment['XR_LOADER_DEBUG']='all'
    $start.Environment['DCSVR_QUAD_FOCUS']='1'
    $process=[Diagnostics.Process]::Start($start)
    $stdoutTask=$process.StandardOutput.ReadToEndAsync(); $stderrTask=$process.StandardError.ReadToEndAsync()
    if(-not $process.WaitForExit(30000)){$process.Kill();throw 'Headless layer smoke timed out.'}
    Write-Output $stdoutTask.GetAwaiter().GetResult(); Write-Output $stderrTask.GetAwaiter().GetResult()
    $code=$process.ExitCode; $process.Dispose(); if($code -ne 0){throw "Layer chain failed ($code): $($mode.layers)"}
}
# The same Quad Views chains with the source-built deferred OFXR layer that software Quad Views profiles deploy.
$deferredRoot=Join-Path $fixtureRoot 'deferred-ofxr'
New-Item -ItemType Directory -Path "$deferredRoot/layers" -Force | Out-Null
Copy-Item -LiteralPath "$workspaceRoot/artifacts/native/ofxr-full/XR_APILAYER_XRFrameBridge_diagnostic.dll" -Destination $deferredRoot -Force
"[ofxr]`r`nenabled=1`r`ndefer_until_submitted=1`r`n" | Set-Content -LiteralPath "$deferredRoot/ofxr_bridge.ini" -Encoding ascii
Get-ChildItem "$fixtureRoot/layers" -Filter *.json | Where-Object { $_.Name -notlike 'XR_APILAYER_XRFrameBridge*' } | Copy-Item -Destination "$deferredRoot/layers" -Force
@{file_format_version='1.0.0';api_layer=@{name='XR_APILAYER_XRFrameBridge_diagnostic';library_path=[IO.Path]::GetFullPath("$deferredRoot/XR_APILAYER_XRFrameBridge_diagnostic.dll");api_version='1.0';implementation_version='1';description='Offline smoke (deferred)'}} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$deferredRoot/layers/XR_APILAYER_XRFrameBridge_diagnostic.json" -Encoding ascii
foreach($layers in @('XR_APILAYER_MBUCCHIA_quad_views_foveated;XR_APILAYER_XRFrameBridge_diagnostic','XR_APILAYER_CHEEKY_foveated_dlss;XR_APILAYER_MBUCCHIA_quad_views_foveated;XR_APILAYER_XRFrameBridge_diagnostic')){
    $start=[Diagnostics.ProcessStartInfo]::new((Join-Path $smokeRoot 'loader_smoke.exe'),'quad')
    $start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
    $start.Environment['XR_RUNTIME_JSON']="$fixtureRoot/runtime.json"
    $start.Environment['XR_API_LAYER_PATH']="$deferredRoot/layers"
    $start.Environment['XR_ENABLE_API_LAYERS']=$layers
    foreach($name in 'XRFG_DISABLE_OFXR_BRIDGE','CHEEKY_OPENXR_LAYER_DISABLE','DISABLE_XR_APILAYER_MBUCCHIA_quad_views_foveated','DCSVR_QUAD_FOCUS'){ $start.Environment[$name]='1' }
    $start.Environment['LOCALAPPDATA']="$fixtureRoot/appdata"; $start.Environment['APPDATA']="$fixtureRoot/appdata"
    $process=[Diagnostics.Process]::Start($start)
    $stdoutTask=$process.StandardOutput.ReadToEndAsync(); $stderrTask=$process.StandardError.ReadToEndAsync()
    if(-not $process.WaitForExit(30000)){$process.Kill();throw 'Deferred OFXR layer smoke timed out.'}
    Write-Output ("deferred OFXR: " + $stdoutTask.GetAwaiter().GetResult().Trim().Split("`n")[-1]); [void]$stderrTask.GetAwaiter().GetResult()
    $code=$process.ExitCode; $process.Dispose(); if($code -ne 0){throw "Deferred OFXR layer chain failed ($code): $layers"}
}
& (Join-Path $smokeRoot 'quad_focus_policy.exe')
if($LASTEXITCODE -ne 0){throw 'Quad focus policy tests failed.'}

# Layer order for the Sboys gaze bridge. Implicit layers load above explicit ones, so an implicit bridge
# would sit above Quad Views. The profile disables it (disable_environment) and loads it explicitly, last.
$orderRoot=Join-Path $fixtureRoot 'gaze-order'
New-Item -ItemType Directory -Path "$orderRoot/layers","$orderRoot/implicit" -Force | Out-Null
foreach($probe in @('quad','gaze')){
    $api=@{name="XR_APILAYER_TEST_$($probe)_probe";library_path=[IO.Path]::GetFullPath((Join-Path $smokeRoot "$($probe)_probe.dll"));api_version='1.0';implementation_version='1';description='Order probe'}
    @{file_format_version='1.0.0';api_layer=$api} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$orderRoot/layers/$probe.json" -Encoding ascii
}
$implicitApi=@{name='XR_APILAYER_TEST_gaze_probe';library_path=[IO.Path]::GetFullPath((Join-Path $smokeRoot 'gaze_probe.dll'));api_version='1.0';implementation_version='1';description='Implicit order probe';disable_environment='DISABLE_XR_APILAYER_TEST_gaze_probe'}
$implicitManifest=[IO.Path]::GetFullPath("$orderRoot/implicit/gaze.json")
@{file_format_version='1.0.0';api_layer=$implicitApi} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $implicitManifest -Encoding ascii
$orderCases=@(
    @{name='implicit bridge without the profile fix';disable=$null;expected='gaze;quad;'},
    @{name='profile disables the implicit bridge and loads it last';disable='1';expected='quad;gaze;'}
)
foreach($case in $orderCases){
    $start=[Diagnostics.ProcessStartInfo]::new((Join-Path $smokeRoot 'gaze_order.exe'),"`"$implicitManifest`" $($case.expected)")
    $start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
    $start.Environment['XR_RUNTIME_JSON']="$fixtureRoot/runtime.json"
    $start.Environment['XR_API_LAYER_PATH']="$orderRoot/layers"
    $start.Environment['XR_ENABLE_API_LAYERS']='XR_APILAYER_TEST_quad_probe;XR_APILAYER_TEST_gaze_probe'
    if($case.disable){ $start.Environment['DISABLE_XR_APILAYER_TEST_gaze_probe']=$case.disable }
    $process=[Diagnostics.Process]::Start($start)
    $stdoutTask=$process.StandardOutput.ReadToEndAsync(); $stderrTask=$process.StandardError.ReadToEndAsync()
    if(-not $process.WaitForExit(30000)){$process.Kill();throw 'Gaze order test timed out.'}
    Write-Output "$($case.name): $($stdoutTask.GetAwaiter().GetResult().Trim()) $($stderrTask.GetAwaiter().GetResult().Trim())"
    $code=$process.ExitCode; $process.Dispose(); if($code -ne 0){throw "Gaze order case failed: $($case.name)"}
}
