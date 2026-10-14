param(
    [ValidateSet('Install','Uninstall','Verify')][string]$Action = 'Install',
    [string]$Destination = (Join-Path $env:LOCALAPPDATA 'Programs/DcsControl'),
    [switch]$NoShortcut,
    [switch]$InstallPrerequisites,
    # The setup (DcsControl-<version>-Setup.exe) also puts DCS Control on the desktop, or with -NoDesktopShortcut removes
    # this installation's desktop shortcut; -NoStartMenuShortcut leaves it out of the Start menu (the pilot's choices).
    [switch]$DesktopShortcut,
    [switch]$NoDesktopShortcut,
    [switch]$NoStartMenuShortcut,
    # Where the shortcuts and the Apps & features entry go; the release tests point them at scratch folders and a scratch key.
    [string]$StartMenuFolder = [Environment]::GetFolderPath('Programs'),
    [string]$DesktopFolder = [Environment]::GetFolderPath('Desktop'),
    [string]$UninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\DcsControl'
)
$ErrorActionPreference = 'Stop'
# This script is files\scripts\installer.ps1; the release (with release-manifest.json) is the folder above files\.
$filesRoot = Split-Path -Parent $PSScriptRoot
$payloadRoot = Split-Path -Parent $filesRoot
$command = switch ($Action) { 'Install' {'app-install'} 'Uninstall' {'app-uninstall'} 'Verify' {'app-verify'} }
# files\Uninstall.cmd of an installation removes that installation, wherever it is.
if ($Action -eq 'Uninstall' -and -not $PSBoundParameters.ContainsKey('Destination') -and (Test-Path -LiteralPath (Join-Path $payloadRoot 'installation.json'))) { $Destination = $payloadRoot }
$installRoot = [IO.Path]::GetFullPath($Destination).TrimEnd('\','/')
function Get-WebViewVersion {
    foreach ($key in @('HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}','HKCU:\Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}')) {
        $value = Get-ItemPropertyValue -LiteralPath $key -Name pv -ErrorAction SilentlyContinue
        $parsedVersion = $null
        if ([Version]::TryParse($value,[ref]$parsedVersion) -and $parsedVersion -gt [Version]'0.0.0.0') { return $value }
    }
    return $null
}
function Install-MicrosoftPrerequisite([string]$Name,[string]$Url,[string]$Arguments) {
    $downloadRoot = Join-Path $installRoot '.prerequisite-downloads'
    New-Item -ItemType Directory -Path $downloadRoot -Force | Out-Null
    $installer = Join-Path $downloadRoot ($Name + '.exe')
    # The download folder goes in every case, also when the download or the installer fails.
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Write-Output "Downloading missing prerequisite from Microsoft: $Name"
        Invoke-WebRequest -Uri $Url -OutFile $installer -UseBasicParsing -TimeoutSec 120
        $signature = Get-AuthenticodeSignature -LiteralPath $installer
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation,') { throw "Prerequisite signature rejected: $Name. Nothing was executed." }
        $process = Start-Process -FilePath $installer -ArgumentList $Arguments -WindowStyle Hidden -Wait -PassThru
        if ($process.ExitCode -notin @(0,3010,1641)) { throw "Microsoft prerequisite installer failed: $Name ($($process.ExitCode))." }
    } finally { Remove-Item -LiteralPath $downloadRoot -Recurse -Force -ErrorAction SilentlyContinue }
    if ($process.ExitCode -in @(3010,1641)) { Write-Output 'Windows restart requested. Restart before the headset test.' }
}
$webViewVersion = Get-WebViewVersion
$cppLibraries = @('vcruntime140.dll','vcruntime140_1.dll','msvcp140.dll')
$missingCpp = @($cppLibraries | Where-Object { -not (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32/$_")) })
$vcFile = Join-Path $env:SystemRoot 'System32/vcruntime140.dll'
$vcVersion = if (Test-Path -LiteralPath $vcFile) { [Version](Get-Item -LiteralPath $vcFile).VersionInfo.FileVersion } else { [Version]'0.0' }
$needsCpp = $missingCpp.Count -gt 0 -or $vcVersion -lt [Version]'14.50'
if ($Action -eq 'Install') {
    # DCS keeps the CPU Boost helper and the engine optimizations of the installed version in use while it runs.
    if (Get-Process -Name DCS -ErrorAction SilentlyContinue) { throw 'Close DCS before installing or updating DCS Control.' }
    & (Join-Path $filesRoot 'DcsVr.Cli.exe') app-verify --source $payloadRoot
    if ($LASTEXITCODE -ne 0) { throw 'Package integrity failed. Prerequisite installation was not attempted.' }
}
if ($Action -eq 'Install' -and $InstallPrerequisites) {
    if (-not $webViewVersion) { Install-MicrosoftPrerequisite 'WebView2' 'https://go.microsoft.com/fwlink/p/?LinkId=2124703' '/silent /install' }
    if ($needsCpp) { Install-MicrosoftPrerequisite 'VisualCpp-x64' 'https://aka.ms/vc14/vc_redist.x64.exe' '/install /passive /norestart' }
}
Write-Output $(if (Get-WebViewVersion) { 'WebView2 Runtime detected.' } else { 'WebView2 Runtime missing. Install the Evergreen Runtime from https://developer.microsoft.com/microsoft-edge/webview2/ before opening the GUI.' })
if ($needsCpp -and -not $InstallPrerequisites) { Write-Output 'Visual C++ x64 runtime 14.50 or newer is required. Run Install DCS Control.cmd to obtain official prerequisites.' }
# Uninstalling from the installation itself: its own program files are open while it runs, so the uninstaller runs
# from a temporary copy of the program (the files\ root, without components or documents).
$cliRoot = $filesRoot
$temporaryCli = $null
if ($Action -eq 'Uninstall' -and ([IO.Path]::GetFullPath($filesRoot).TrimEnd('\','/') + '\').StartsWith($installRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    $temporaryCli = Join-Path ([IO.Path]::GetTempPath()) ('DcsControl-uninstall-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryCli -Force | Out-Null
    Get-ChildItem -LiteralPath $filesRoot -File | Copy-Item -Destination $temporaryCli
    $cliRoot = $temporaryCli
}
try {
    & (Join-Path $cliRoot 'DcsVr.Cli.exe') $command --source $payloadRoot --destination $installRoot
    if ($LASTEXITCODE -ne 0) { throw 'Application deployment failed. Review the error above; recovery backups are retained.' }
} finally {
    if ($temporaryCli) { Remove-Item -LiteralPath $temporaryCli -Recurse -Force -ErrorAction SilentlyContinue }
}
if ($NoShortcut -or $Action -eq 'Verify') { exit 0 }
$appPath = Join-Path $installRoot 'DcsControl.exe'
$shortcutPath = Join-Path $StartMenuFolder 'DCS Control.lnk'
$desktopPath = Join-Path $DesktopFolder 'DCS Control.lnk'
$shell = New-Object -ComObject WScript.Shell
if ($Action -eq 'Install') {
    $wanted = @(); $unwanted = @()
    if ($NoStartMenuShortcut) { $unwanted += $shortcutPath } else { $wanted += $shortcutPath }
    if ($DesktopShortcut) { $wanted += $desktopPath } elseif ($NoDesktopShortcut) { $unwanted += $desktopPath }
    foreach ($path in $wanted) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        $shortcut = $shell.CreateShortcut($path)
        $shortcut.TargetPath = $appPath
        $shortcut.IconLocation = $appPath + ',0'
        $shortcut.WorkingDirectory = $installRoot
        $shortcut.Save()
    }
    # Shortcuts the pilot no longer wants: only ones that open this installation.
    foreach ($path in $unwanted) {
        if ((Test-Path -LiteralPath $path) -and $shell.CreateShortcut($path).TargetPath -eq $appPath) { Remove-Item -LiteralPath $path }
    }
    # The Apps & features entry (per user): Uninstall runs the installation's own files\Uninstall.cmd, which removes the
    # owned files, the shortcuts and this entry. The setup then points it at its own uninstaller (Uninstall DCS Control.exe).
    $manifest = Get-Content -LiteralPath (Join-Path $payloadRoot 'release-manifest.json') -Raw | ConvertFrom-Json
    $bytes = [long](($manifest.files | Measure-Object -Property bytes -Sum).Sum)
    New-Item -Path $UninstallKey -Force | Out-Null
    $entry = [ordered]@{
        DisplayName = 'DCS Control'; DisplayVersion = [string]$manifest.version; Publisher = 'NIGos'; InstallLocation = $installRoot
        DisplayIcon = $appPath + ',0'; UninstallString = '"' + (Join-Path $env:SystemRoot 'System32\cmd.exe') + '" /c ""' + (Join-Path $installRoot 'files\Uninstall.cmd') + '""'
        URLInfoAbout = 'https://github.com/NIGos/dcs-control'; HelpLink = 'https://github.com/NIGos/dcs-control/issues'
    }
    foreach ($name in $entry.Keys) { New-ItemProperty -Path $UninstallKey -Name $name -Value $entry[$name] -PropertyType String -Force | Out-Null }
    foreach ($name in 'NoModify','NoRepair') { New-ItemProperty -Path $UninstallKey -Name $name -Value 1 -PropertyType DWord -Force | Out-Null }
    New-ItemProperty -Path $UninstallKey -Name EstimatedSize -Value ([int][Math]::Ceiling($bytes / 1KB)) -PropertyType DWord -Force | Out-Null
    Write-Output "Installed DCS Control in $installRoot. No game or VR driver settings changed."
    # DCS Control was called DCS VR Control before 0.5.0: its default install and Start menu entry are replaced by this
    # one when this one goes to the default folder. Only that app's own files go (its uninstaller checks each one); your
    # DCS backups and profiles are kept.
    $earlierRoot = Join-Path $env:LOCALAPPDATA 'Programs/DcsVrControl'
    $earlierRemoved = $false
    $defaultRoot = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Programs/DcsControl')).TrimEnd('\','/')
    if ($installRoot -eq $defaultRoot -and (Test-Path -LiteralPath (Join-Path $earlierRoot 'installation.json'))) {
        & (Join-Path $filesRoot 'DcsVr.Cli.exe') app-uninstall --destination $earlierRoot
        $earlierRemoved = $LASTEXITCODE -eq 0
        if ($earlierRemoved) { Write-Output "Removed the earlier DCS VR Control installation from $earlierRoot." } else { Write-Output "The earlier DCS VR Control installation in $earlierRoot was left in place; remove it from there if you no longer need it." }
    }
    # Its Start menu entry goes only with it: when that install was removed, or when the entry points nowhere.
    $earlierShortcut = Join-Path $StartMenuFolder 'DCS VR Control.lnk'
    if (Test-Path -LiteralPath $earlierShortcut) {
        $earlierTarget = $shell.CreateShortcut($earlierShortcut).TargetPath
        if ($earlierRemoved -or -not $earlierTarget -or -not (Test-Path -LiteralPath $earlierTarget)) { Remove-Item -LiteralPath $earlierShortcut }
    }
} else {
    # Only shortcuts and an Apps & features entry that belong to this installation.
    foreach ($path in $shortcutPath, $desktopPath) {
        if ((Test-Path -LiteralPath $path) -and $shell.CreateShortcut($path).TargetPath -eq $appPath) { Remove-Item -LiteralPath $path }
    }
    $location = if (Test-Path -LiteralPath $UninstallKey) { (Get-ItemProperty -LiteralPath $UninstallKey -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation }
    if ($location -and [IO.Path]::GetFullPath($location).TrimEnd('\','/') -eq $installRoot) { Remove-Item -LiteralPath $UninstallKey -Recurse }
    Write-Output 'Owned application files removed. Game-mod backup state is retained.'
}
