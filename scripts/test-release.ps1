param([string]$NeuralRuntime)
$ErrorActionPreference='Stop'
$workspaceRoot=Split-Path -Parent $PSScriptRoot
$version = & (Join-Path $PSScriptRoot 'version.ps1')
$releaseRoot=Join-Path $workspaceRoot "artifacts/release/DcsControl-$version-win-x64"
# One fixture folder, emptied at the start of each run (a new folder per run once filled the disk).
$fixtureRoot=Join-Path $workspaceRoot 'artifacts/release-tests/latest'
if(Test-Path -LiteralPath $fixtureRoot){Remove-Item -LiteralPath $fixtureRoot -Recurse -Force}
$installRoot=Join-Path $fixtureRoot 'installed-app'
New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
$cli=Join-Path $releaseRoot 'files/DcsVr.Cli.exe'
& $cli app-verify --source $releaseRoot
if($LASTEXITCODE -ne 0){throw 'Payload verification failed.'}
& "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -NoProfile -ExecutionPolicy Bypass -File (Join-Path $releaseRoot 'files/scripts/installer.ps1') -Action Verify -Destination $installRoot -NoShortcut
if($LASTEXITCODE -ne 0){throw 'Windows PowerShell installer frontend failed.'}
$publishedPrevious=Join-Path $workspaceRoot 'artifacts/release/DcsVrControl-0.3.0-preview-win-x64'
# Earlier releases shipped Cheeky's version.dll fallback, which Microsoft Defender blocks. The upgrade fixture is a
# copy of 0.3.0 without that file and with a consistent manifest, so upgrading still removes obsolete owned files.
$previousRelease=Join-Path $fixtureRoot 'previous-0.3.0'
if(Test-Path -LiteralPath (Join-Path $publishedPrevious 'release-manifest.json')){
    $blocked=@('components/cheeky-focus/version.dll','components\cheeky-focus\version.dll')
    $manifest=Get-Content -LiteralPath (Join-Path $publishedPrevious 'release-manifest.json') -Raw | ConvertFrom-Json
    $manifest.files=@($manifest.files | Where-Object { $blocked -notcontains $_.path })
    foreach($file in $manifest.files){
        $target=Join-Path $previousRelease $file.path
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $publishedPrevious $file.path) -Destination $target
    }
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $previousRelease 'release-manifest.json') -Encoding utf8
}
$upgradeChecked=Test-Path -LiteralPath (Join-Path $previousRelease 'release-manifest.json')
if($upgradeChecked){
    & $cli app-install --source $previousRelease --destination $installRoot
    if($LASTEXITCODE -ne 0){throw 'Previous release installation fixture failed.'}
}
foreach($attempt in 1..2){
    & $cli app-install --source $releaseRoot --destination $installRoot
    if($LASTEXITCODE -ne 0){throw 'Application install/update fixture failed.'}
}
if(Test-Path -LiteralPath (Join-Path $installRoot 'files/packages/CustomHeadset-1.3.0-Pimax-UI.zip')){throw 'Upgrade retained the obsolete bundled Sboys archive.'}
$installedCli=Join-Path $installRoot 'files/DcsVr.Cli.exe'
& $installedCli inventory | Set-Content -LiteralPath (Join-Path $fixtureRoot 'inventory.json') -Encoding utf8
if($LASTEXITCODE -ne 0){throw 'Installed self-contained CLI failed.'}
$start=[Diagnostics.ProcessStartInfo]::new((Join-Path $installRoot 'DcsControl.exe'))
$start.UseShellExecute=$false; $start.CreateNoWindow=$true
$start.ArgumentList.Add('--web-smoke');$start.ArgumentList.Add((Join-Path $fixtureRoot 'gui'))
if ($NeuralRuntime) { $start.Environment['DCSVR_TEST_NEURAL_PATH'] = $NeuralRuntime }
$gui=[Diagnostics.Process]::Start($start)
if(-not $gui.WaitForExit(120000)){$gui.Kill($true);throw 'Offscreen GUI smoke timed out.'}
if($gui.ExitCode -ne 0){throw 'Installed GUI offscreen smoke failed.'}
$gui.Dispose()
# Every view the verification listed must exist (the list says how many).
$guiRenders=@(Get-Content -LiteralPath (Join-Path $fixtureRoot 'gui-renders.json') -Raw | ConvertFrom-Json)
if($guiRenders.Count -eq 0 -or @($guiRenders | Where-Object { -not (Test-Path -LiteralPath $_.path) -and -not (Test-Path -LiteralPath (Join-Path $fixtureRoot (Split-Path -Leaf $_.path))) }).Count){throw 'Missing offscreen GUI view.'}
$guiChecks=Get-Content -LiteralPath (Join-Path $fixtureRoot 'gui-checks.json') -Raw | ConvertFrom-Json
if(-not $guiChecks.passed -or $guiChecks.count -le 0){throw 'Installed WebView2 control and bridge verification failed.'}
# Exercise the actual packaged service against isolated game/runtime fixtures.
# Optional real user-supplied NR deployment is tested only in these game fixtures.
$presets = & $installedCli presets | ConvertFrom-Json
if($LASTEXITCODE -ne 0){throw 'Installed preset export failed.'}
$runtimeDll=Join-Path $fixtureRoot 'fixture-runtime.dll'
[IO.File]::WriteAllText($runtimeDll,'Offline manifest target; never loaded.')
$runtimeManifest=Join-Path $fixtureRoot 'fixture-runtime.json'
@{runtime=@{library_path=$runtimeDll}} | ConvertTo-Json | Set-Content -LiteralPath $runtimeManifest -Encoding utf8
$cases=foreach($presetId in @('pimax-combined','sboys-combined')) {
    @{id=$presetId;neural=$false}
    if($NeuralRuntime){@{id=$presetId;neural=$true}}
}
foreach($case in $cases){
    $presetId=$case.id
    $gameRoot=Join-Path $fixtureRoot ($presetId+$(if($case.neural){'-neural'}else{''}))
    New-Item -ItemType Directory -Path (Join-Path $gameRoot 'bin') -Force | Out-Null
    $dcsExe=Join-Path $gameRoot 'bin/DCS.exe'
    $options=Join-Path $gameRoot 'options.lua'
    $profileFile=Join-Path $gameRoot 'profile.json'
    $stateRoot=Join-Path $gameRoot 'state'
    [IO.File]::WriteAllText($dcsExe,'Offline DCS placeholder; never executed.')
    [IO.File]::WriteAllText($options,'options={graphics={Upscaling="DLSS"},VR={enable=false}}')
    $beforeOptions=(Get-FileHash -LiteralPath $options).Hash
    $profile=($presets | Where-Object id -eq $presetId | ConvertTo-Json -Depth 8) | ConvertFrom-Json
    $profile.neuralRendering=$case.neural; $profile.foveatedDlss=-not $case.neural
    $profile.foveaSource='Profile'; $profile.quadFocusScale=1.25; $profile.quadSharpening=.45; $profile.quadEdgeBlend=.15
    $profile.flowPreset='Slow'; $profile.bidirectionalFlow=$true; $profile.nvidiaFlowScale=100
    $profile.fpsLimit='MatchRefresh'; $profile.headsetRefreshHz=90; $profile | Add-Member -Force refreshFromHeadset $false; $profile.disableDcsVSync=$true
    if($case.neural){$profile.neuralRuntimePath=$NeuralRuntime; $profile.neuralLocalTone=1.1; $profile.neuralColorStrength=.8; $profile.neuralUiCorrection=$true}
    $profile.runtimeManifestPath=$runtimeManifest
    $profile | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $profileFile -Encoding utf8
    $cliOptions=@('--profile',$profileFile,'--state',$stateRoot,'--dcs',$dcsExe,'--options',$options)
    & $installedCli preview @cliOptions | Set-Content -LiteralPath (Join-Path $gameRoot 'preview.txt') -Encoding utf8
    if($LASTEXITCODE -ne 0){throw 'Packaged combined preview failed.'}
    & $installedCli apply @cliOptions | Set-Content -LiteralPath (Join-Path $gameRoot 'apply.txt') -Encoding utf8
    if($LASTEXITCODE -ne 0){throw 'Packaged combined apply failed.'}
    if($case.neural){
        $copiedNeural=Join-Path $gameRoot 'bin/CheekyFoveatedDLSS/nvngx_dlssnr.dll'
        if((Get-FileHash -LiteralPath $copiedNeural).Hash -ne (Get-FileHash -LiteralPath $NeuralRuntime).Hash){throw 'Neural runtime deployment hash differs.'}
        $tuning=Get-Content -LiteralPath (Join-Path $gameRoot 'bin/CheekyFoveatedDLSS/CheekyFoveatedDLSS.ini') -Raw
        foreach($value in @('NrEnabled=1','NrLocalToneStrength=1.1','NrColorStrength=0.8','NrUiCorrection=1')){if(!$tuning.Contains($value)){throw "Neural configuration missing: $value"}}
    }
    $status=& $installedCli status --state $stateRoot | ConvertFrom-Json
    if($LASTEXITCODE -ne 0){throw 'Packaged originals status failed.'}
    if($status.state -ne 'applied' -or $status.count -lt 5 -or -not ($status.files | Where-Object { $_.path -eq $options -and $_.action -eq 'settings' })){throw 'Packaged apply did not record the original files.'}
    $journal=$status.current
    $luaEntry=$journal.entries | Where-Object { $_.path -eq $options }
    if(-not ($luaEntry.luaChanges | Where-Object { $_.path -eq 'graphics.maxFPS' -and $_.installedRaw -eq '45' })){throw 'Combined pipeline did not apply the matched rendered cap.'}
    if(-not ($luaEntry.luaChanges | Where-Object { $_.path -eq 'graphics.sync' -and $_.installedRaw -eq 'false' })){throw 'Combined pipeline did not apply the explicit desktop VSync setting.'}
    $quadEntry=$journal.entries | Where-Object { $_.path.EndsWith([IO.Path]::Combine('quadviews','settings.cfg')) }
    if(-not $quadEntry){throw 'Managed Quad Views settings not found.'}
    $quadQuality=Get-Content -LiteralPath $quadEntry.path -Raw
    foreach($value in @('focus_multiplier=1.25','sharpen_focus_view=0.45','smoothen_focus_view_edges=0.15')){if(!$quadQuality.Contains($value)){throw "Custom Quad Views quality configuration missing: $value"}}
    $flowEntry=$journal.entries | Where-Object { $_.path.EndsWith('ofxr_bridge.ini') }
    if(-not $flowEntry){throw 'Managed optical flow settings not found.'}
    $flowQuality=Get-Content -LiteralPath $flowEntry.path -Raw
    foreach($value in @('nvidia_preset=slow','nvidia_input_scale=100','nvidia_bidirectional=1')){if(!$flowQuality.Contains($value)){throw "Custom optical flow quality configuration missing: $value"}}
    $launchEntry=$journal.entries | Where-Object { $_.path.EndsWith('launch.json') }
    $launch=Get-Content -LiteralPath $launchEntry.path -Raw | ConvertFrom-Json
    if($launch.environment.DCSVR_QUAD_FOCUS -ne $(if($case.neural){'1'}else{'0'})){throw 'Packaged adapter environment mismatch (Foveated SR alone is stereo-only, so Quad Views profiles without DLSS 5 run no adapter).'}
    & $installedCli launch-check --state $stateRoot
    if($LASTEXITCODE -ne 0){throw 'Packaged launch contract verification failed.'}
    [IO.File]::WriteAllText($runtimeDll,'Changed runtime fixture; never loaded.')
    $rejectedLaunch=& $installedCli launch-check --state $stateRoot 2>&1
    if($LASTEXITCODE -eq 0 -or ($rejectedLaunch -join "`n") -notmatch 'runtime, provider or pacing configuration changed'){throw 'Changed runtime was not rejected before launch.'}
    [IO.File]::WriteAllText($runtimeDll,'Offline manifest target; never loaded.')
    # Applying again over the applied profile overwrites it: no restore step, no conflict, same originals.
    & $installedCli apply @cliOptions | Set-Content -LiteralPath (Join-Path $gameRoot 'apply-again.txt') -Encoding utf8
    if($LASTEXITCODE -ne 0){throw 'Packaged apply over the applied profile failed.'}
    if((& $installedCli status --state $stateRoot | ConvertFrom-Json).count -ne $status.count){throw 'Applying again changed the recorded originals.'}
    & $installedCli restore --state $stateRoot
    if($LASTEXITCODE -ne 0){throw 'Packaged combined restoration failed.'}
    if((& $installedCli status --state $stateRoot | ConvertFrom-Json).count -ne 0){throw 'Back to stock DCS left recorded files.'}
    if((Get-FileHash -LiteralPath $options).Hash -ne $beforeOptions){throw 'Fixture DCS options did not restore.'}
    if(@(Get-ChildItem -LiteralPath (Join-Path $gameRoot 'bin') -Recurse -File).Count -ne 1){throw 'Fixture game mods were not removed.'}
    # The focus adapter only deploys with DLSS 5 (Foveated SR alone is stereo-only), so the tamper check needs the neural case.
    if($presetId -eq 'pimax-combined' -and $case.neural){
        $focusDll=Join-Path $installRoot 'files/components/cheeky-focus/CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll'
        $original=[IO.File]::ReadAllBytes($focusDll)
        try{
            $changed=$original.Clone(); $changed[0]=$changed[0] -bxor 1
            [IO.File]::WriteAllBytes($focusDll,$changed)
            $rejected=& $installedCli preview @cliOptions 2>&1
            if($LASTEXITCODE -eq 0 -or ($rejected -join "`n") -notmatch 'focus adapter is modified'){
                throw 'Tampered packaged focus adapter was not rejected.'
            }
        } finally {[IO.File]::WriteAllBytes($focusDll,$original)}
    }
}
$notes=Join-Path $installRoot 'user-notes.txt'
[IO.File]::WriteAllText($notes,'Retain this unowned file.')
# Uninstall exactly as a user does: files\Uninstall.cmd's script inside the installation, with no destination given
# (it removes its own installation, running the uninstaller from a temporary copy of the program).
& "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -NoProfile -ExecutionPolicy Bypass -File (Join-Path $installRoot 'files/scripts/installer.ps1') -Action Uninstall -NoShortcut
if($LASTEXITCODE -ne 0){throw 'Application uninstall fixture failed.'}
if((Test-Path -LiteralPath (Join-Path $installRoot 'DcsControl.exe')) -or (Test-Path -LiteralPath (Join-Path $installRoot 'files/DcsControl.exe'))){throw 'Owned application executable was not removed.'}
if([IO.File]::ReadAllText($notes) -ne 'Retain this unowned file.'){throw 'User file was changed.'}
$result=@{passed=$true;fixtureRoot=$fixtureRoot;previousReleaseUpgradeChecked=$upgradeChecked;webViewChecks=$guiChecks.count;webViewRenders=$guiRenders.Count;neuralDeploymentCases=@($cases | Where-Object neural).Count;checks=@('release hashes','Windows PowerShell frontend','self-contained install','repeat install/update','installed CLI','actual WebView2 interaction and native bridge checks','Pimax and Sboys combined apply, apply over, back to stock DCS','optional signed NR hash and tuning deployment','installed launch contract without executing DCS','changed external runtime rejection','tampered focus adapter rejection','owned-file uninstall','unowned-file retention')}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $fixtureRoot 'result.json') -Encoding utf8
Write-Output "PASS release fixture: $fixtureRoot"
