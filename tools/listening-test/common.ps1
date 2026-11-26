# Shared by the listening-test scripts: where DcsVr.Cli is, and the results folder of a session.
# Dot-source it: . "$PSScriptRoot\common.ps1"

$KitRoot = $PSScriptRoot
$RepoRoot = Split-Path -Parent (Split-Path -Parent $KitRoot)

# DcsVr.Cli.exe: the -Cli given, else this checkout's build output, else the installed app.
function Find-Cli([string]$Cli) {
  if ($Cli) {
    if (-not (Test-Path -LiteralPath $Cli)) { throw "DcsVr.Cli not found: $Cli" }
    return (Resolve-Path -LiteralPath $Cli).Path
  }
  $candidates = @()
  foreach ($config in 'Release', 'Debug') {
    $candidates += Get-ChildItem -Path (Join-Path $RepoRoot "src\DcsVr.Cli\bin\$config") -Filter DcsVr.Cli.exe -Recurse -ErrorAction SilentlyContinue |
      Sort-Object LastWriteTime -Descending | Select-Object -ExpandProperty FullName
  }
  $installed = Join-Path $env:LOCALAPPDATA 'Programs\DcsControl'
  $candidates += @((Join-Path $installed 'files\DcsVr.Cli.exe'), (Join-Path $installed 'DcsVr.Cli.exe'))
  foreach ($c in $candidates) { if ($c -and (Test-Path -LiteralPath $c)) { return $c } }
  throw 'DcsVr.Cli.exe was not found. Build the app, install DCS Control, or give it with -Cli.'
}

# The results folder of a session: the -Folder given, else artifacts\listening\<today> in this checkout.
function Get-ResultsFolder([string]$Folder) {
  if (-not $Folder) { $Folder = Join-Path $RepoRoot ("artifacts\listening\" + (Get-Date -Format 'yyyy-MM-dd')) }
  New-Item -ItemType Directory -Force -Path $Folder | Out-Null
  return (Resolve-Path -LiteralPath $Folder).Path
}

# Runs DcsVr.Cli with the arguments; stops the script on a non-zero exit.
function Invoke-Cli([string]$Exe, [string[]]$Arguments) {
  & $Exe @Arguments
  if ($LASTEXITCODE -ne 0) { throw "DcsVr.Cli $($Arguments[0]) $($Arguments[1]) failed (exit $LASTEXITCODE)." }
}
