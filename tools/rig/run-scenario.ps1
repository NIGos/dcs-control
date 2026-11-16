# Scenario runner of the foveated frame-generation rig: one unattended DCS flight per scenario and arm. TEST-ONLY.
# Promoted from the scratch rig of 2026-11-15/16 (RIG.md is its record); README.md has the dependencies.
#
#   pwsh -File tools\rig\run-scenario.ps1 -Scenario hud [-Arm 2x] [-Both | -Twin] [-Prep] [-KeepPimax] [-Force] [-DryRun]
#
#   -Scenario   name of scenarios\<name>.json (or a path)
#   -Arm        one of the scenario's "arms" (profile overrides); default: the first
#   -Both       flies the option ON and then its twin with the option OFF (same scenario, same mission), one prep and
#               one restore around the two. -Twin flies only the twin. Default: only the option ON.
#   -Prep       quiet lock is always taken; -Prep also starts Pimax, sets the screen-off key to 0, restarts the headset
#   -KeepPimax  leaves PimaxClient running at the end even when this run started it
#   -Force      runs a scenario marked "blocked"
#   -DryRun     no profile is applied and nothing is started: prints the plan and the checks only
#
# Nothing of this machine is written in the script. Where things are comes from a parameter, else from an environment
# variable, else from the default given here:
#   -StateBackup   DCSC_RIG_STATE_BACKUP   a copy of the app's state files (draft.json, live.json, settings.json,
#                                          presets\active.txt, presets\Default.json) taken before any test: the profile
#                                          flown is that draft's. No default: required.
#   -MissionDir    DCSC_RIG_MISSIONS       the folder with the scenarios' .miz files. No default: required unless the
#                                          scenario names its mission by a full path.
#   -RunsDir       DCSC_RIG_RUNS           where runs go; default <repo>\artifacts\rig\runs (git-ignored)
#   -TapDir        DCSC_RIG_TAP_DIR        the built capture layer (XR_APILAYER_RIG_capture.dll and .json); default
#                                          <repo>\artifacts\rig\capture-layer\bin
#   -SavedGamesDcs DCSC_SAVED_GAMES_DCS    DCS's Saved Games folder; default %USERPROFILE%\Saved Games\DCS
#   -PimaxCtl      DCSC_PIMAX_CTL          pimax_ctl.exe (get / set / reboot); default <repo>\tools\runtime-probe\bin
#   -PimaxClient   DCSC_PIMAX_CLIENT       PimaxClient.exe; default under %ProgramFiles%
#   -PimaxReboot   DCSC_PIMAX_REBOOT       another program that restarts the headset; default: `pimax_ctl reboot`
# The installed DCS Control (CLI and state) is found under %LOCALAPPDATA%, as the app itself does.
#
# What it does per flight: profile = the backup draft + overrides, applied with the INSTALLED CLI; a COPY of the
# applied profile folder under <runs>\<run>\live with the scenario's ini / settings.cfg edits and with the capture
# layer appended LAST to XR_ENABLE_API_LAYERS / XR_API_LAYER_PATH (the app's own files are never edited); DCS started
# from the copy's launch.json into the mission; FLY clicked and confirmed in dcs.log; the timed schedule; logs
# collected; verdict.py on every capture (after DCS has closed).
# In `finally`: DCS closed, cursor clip released, stock restored with the installed CLI's `restore`, the screen-off key
# put back to the value read before -Prep changed it, PimaxClient closed only when it was not open before the run,
# user state compared by hash with the backup and with the copy taken at the start of this run, quiet lock removed.
# Exit code 1 when a flight or the run failed (the cleanup above still runs), 0 otherwise.
param([Parameter(Mandatory)][string]$Scenario, [string]$Arm, [switch]$Both, [switch]$Twin, [switch]$Prep, [switch]$KeepPimax, [switch]$Force, [switch]$DryRun,
  # -OfxrDll: the frame-generation layer DLL flown in the option-ON flight instead of the installed one; -TwinOfxrDll: the same
  # for the twin. Only the COPY under rig\runs\<run>\live\ofxr is replaced; the installed app is never touched.
  [string]$OfxrDll, [string]$TwinOfxrDll,
  # -FlyArms: several arms flown one after the other in one run (one prep, one restore); each arm may carry its own ofxr /
  # diagnostics / quadviews / tapEnv keys, its own schedule and seconds (four-points round, 2026-11-16).
  [string[]]$FlyArms,
  # -QvDll: the Quad Views layer DLL flown instead of the installed one (both flights); only the COPY is changed.
  [string]$QvDll,
  # Where things are on this machine (see the head of the file): parameter, else environment variable, else default.
  [string]$StateBackup = $env:DCSC_RIG_STATE_BACKUP, [string]$MissionDir = $env:DCSC_RIG_MISSIONS, [string]$RunsDir = $env:DCSC_RIG_RUNS,
  [string]$TapDir = $env:DCSC_RIG_TAP_DIR, [string]$SavedGamesDcs = $env:DCSC_SAVED_GAMES_DCS, [string]$PimaxCtl = $env:DCSC_PIMAX_CTL,
  [string]$PimaxClient = $env:DCSC_PIMAX_CLIENT, [string]$PimaxReboot = $env:DCSC_PIMAX_REBOOT)
$ErrorActionPreference = 'Stop'
if ($FlyArms) { $FlyArms = @($FlyArms | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }      # pwsh -File hands a list over as one string
$Rig     = [IO.Path]::GetFullPath($PSScriptRoot)
$Repo    = [IO.Path]::GetFullPath((Join-Path $Rig '..\..'))
$RigData = Join-Path $Repo 'artifacts\rig'      # what a run leaves behind: git-ignored, never under tools\
$Runs    = if ($RunsDir) { [IO.Path]::GetFullPath($RunsDir) } else { Join-Path $RigData 'runs' }
$Backup  = if ($StateBackup) { [IO.Path]::GetFullPath($StateBackup) } else { $null }
$Cli     = Join-Path $env:LOCALAPPDATA 'Programs\DcsControl\files\DcsVr.Cli.exe'
$State   = Join-Path $env:LOCALAPPDATA 'DcsControl'
if (-not $SavedGamesDcs) { $SavedGamesDcs = Join-Path $env:USERPROFILE 'Saved Games\DCS' }
$QvLog   = Join-Path $SavedGamesDcs 'Scripts\DcsQvCull\DcsQvCull.log'
$DcsLog  = Join-Path $SavedGamesDcs 'Logs\dcs.log'
$Ctl     = if ($PimaxCtl) { $PimaxCtl } else { Join-Path $Repo 'tools\runtime-probe\bin\pimax_ctl.exe' }
if (-not $PimaxClient) { $PimaxClient = Join-Path $env:ProgramFiles 'Pimax\PimaxClient\pimaxui\PimaxClient.exe' }
$Lock    = Join-Path $Repo 'artifacts\.lane\quiet.lock'
if (-not $TapDir) { $TapDir = Join-Path $RigData 'capture-layer\bin' }
$TapDir  = [IO.Path]::GetFullPath($TapDir)
$TapDll  = Join-Path $TapDir 'XR_APILAYER_RIG_capture.dll'
$TapName = 'XR_APILAYER_RIG_capture'
$Script:Failed = [Collections.Generic.List[string]]::new()      # what failed in this run: the exit code at the end
$OfxrLayerOn = $OfxrDll; $OfxrLayerTwin = $TwinOfxrDll      # the -OfxrDll / -TwinOfxrDll files; $OfxrDll below is the layer's file name
$OfxrDll = 'XR_APILAYER_XRFrameBridge_diagnostic.dll'
$StateFiles = 'draft.json', 'live.json', 'settings.json', 'presets\active.txt', 'presets\Default.json'
New-Item -ItemType Directory -Force $Runs | Out-Null
$Steps = Join-Path $Runs 'steps.log'

# ---- helpers (from a6.ps1 / a7.ps1 / win.ps1 / fly4.ps1) ----------------------------------------------------------
function Note([string]$s) { $l = "{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $s; Add-Content -LiteralPath $Steps -Value $l; Write-Host $l }
function Read-Shared([string]$p) { try { $fs = [IO.File]::Open($p, 'Open', 'Read', 'ReadWrite,Delete'); try { $r = New-Object IO.StreamReader($fs); $r.ReadToEnd() } finally { $fs.Dispose() } } catch { '' } }
function Sha([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant() }
function Write-Utf8([string]$p, [string]$t) { [IO.File]::WriteAllText($p, $t, (New-Object Text.UTF8Encoding($false))) }
function Applied {
  $b = (Read-Shared (Join-Path $State 'originals\baseline.json')) | ConvertFrom-Json
  $r = [pscustomobject]@{ State = [string]$b.state; Id = $null; App = $null; Folder = $null }
  if ($b.state -eq 'applied' -and $b.current) {
    $r.Id = [string]$b.current.profileId; $r.App = [string]$b.current.appVersion
    $l = $b.current.entries | Where-Object { (Split-Path ([string]$_.path) -Leaf) -ieq 'launch.json' } | Select-Object -First 1
    if ($l) { $r.Folder = Split-Path ([string]$l.path) -Parent }
  }
  $r
}
function Cli-Version { (& $Cli help 2>&1 | Select-Object -First 1) }
function Apply-Profile([string]$Profile, [string]$Why = '') {
  if (Get-Process DCS -ErrorAction SilentlyContinue) { throw 'DCS is running: no apply' }
  $d = Get-Content (Join-Path $State 'draft.json') -Raw | ConvertFrom-Json
  $out = & $Cli apply --profile $Profile --dcs $d.dcs --options $d.options 2>&1 | Out-String
  $code = $LASTEXITCODE; $a = Applied
  Note "apply $(Split-Path $Profile -Leaf) [$Why] with $(Cli-Version): exit $code; applied = $($a.Id) by $($a.App)"
  if ($code -ne 0) { throw "apply failed ($code): $out" }
  $a
}
function Restore-Stock {
  if (Get-Process DCS -ErrorAction SilentlyContinue) { throw 'DCS running: no restore' }
  $o = & $Cli restore 2>&1 | Out-String; $c = $LASTEXITCODE
  $s = (& $Cli status 2>&1 | Out-String | ConvertFrom-Json)
  Note "restore exit $c; status state=$($s.state) count=$($s.count)"
  if ($s.state -ne 'clean') { Note "WARNING: the app's state is '$($s.state)' after restore: open DCS Control and use Restore" }
}
# Sets key=value in the LAST section of that name (Quad Views' settings.cfg: a later section of the same name wins).
function Set-IniKey([string]$Text, [string]$Section, [string]$Key, [string]$Value) {
  $nl = if ($Text.Contains("`r`n")) { "`r`n" } else { "`n" }
  $lines = [Collections.Generic.List[string]]($Text -split "`r?`n"); $start = -1
  for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim() -ieq "[$Section]") { $start = $i } }
  if ($start -lt 0) { $lines.Add("[$Section]"); $lines.Add("$Key=$Value"); return ($lines -join $nl) }
  $end = $lines.Count
  for ($i = $start + 1; $i -lt $lines.Count; $i++) { if ($lines[$i].TrimStart().StartsWith('[')) { $end = $i; break } }
  $rx = '^\s*' + [regex]::Escape($Key) + '\s*='; $found = -1
  for ($i = $start + 1; $i -lt $end; $i++) { if ($lines[$i] -match $rx) { $found = $i } }
  if ($found -ge 0) { $lines[$found] = "$Key=$Value" } else { $at = $end; while ($at -gt $start + 1 -and $lines[$at - 1].Trim() -eq '') { $at-- }; $lines.Insert($at, "$Key=$Value") }
  $lines -join $nl
}
function Edit-IniFile([string]$Path, [string]$Section, $Pairs) {
  if (-not $Pairs) { return }
  $bytes = [IO.File]::ReadAllBytes($Path); $utf16 = ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE)
  $t = [IO.File]::ReadAllText($Path)
  foreach ($p in $Pairs.PSObject.Properties) { $t = Set-IniKey $t $Section $p.Name ([string]$p.Value) }
  if ($utf16) { [IO.File]::WriteAllText($Path, $t, [Text.Encoding]::Unicode) } else { Write-Utf8 $Path $t }
}
function Pimax-Ready([int]$StableSec = 40, [int]$TimeoutSec = 240) {
  $t0 = Get-Date
  while ($true) {
    $why = ''
    if (-not (Get-Process pi_server -ErrorAction SilentlyContinue)) { throw 'pi_server is not running (start Pimax Play, or use -Prep)' }
    $log = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Pimax\runtime\pvr_srv_log_*.txt') | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $m = [regex]::Matches((Read-Shared $log.FullName), '\[(\d\d-\d\d-\d\d \d\d:\d\d:\d\d)\.\d+\]\[[^\]]*\]\[info\]\[PSRV\] global error code:(\d+)')
    if ($m.Count) {
      $last = $m[$m.Count - 1]; $code = [int]$last.Groups[2].Value
      $age = ((Get-Date) - [datetime]::ParseExact($last.Groups[1].Value, 'yy-MM-dd HH:mm:ss', [Globalization.CultureInfo]::InvariantCulture)).TotalSeconds
      if ($code -eq 0 -and $age -ge $StableSec) { return "Pimax ready (code 0 for $([int]$age) s)" }
      $why = "code $code, $([int]$age) s"
    } else { $why = 'no global error code' }
    if (((Get-Date) - $t0).TotalSeconds -gt $TimeoutSec) { throw "Pimax runtime not ready within $TimeoutSec s ($why)" }
    Start-Sleep 2
  }
}
function Slam { $srv = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Pimax\runtime\pvr_srv_log_*.txt') -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1; if ($srv) { ((Read-Shared $srv.FullName) -split "`n" | Select-String 'slam warning flags' | Select-Object -Last 1).Line } }
function Dcs-LogHead { if (Test-Path -LiteralPath $DcsLog) { ((Read-Shared $DcsLog) -split "`n" | Select-Object -First 1) } else { '' } }
function State-Check([string]$Snapshot) {
  $bad = 0
  foreach ($n in $StateFiles) {
    $live = Join-Path $State $n; $bak = Join-Path $Backup $n; $snap = Join-Path $Snapshot $n
    $vsBak = if ((Test-Path $live) -and (Test-Path $bak) -and (Sha $live) -eq (Sha $bak)) { 'same as backup' } else { 'DIFFERENT from backup' }
    $vsSnap = if ((Test-Path $live) -and (Test-Path $snap) -and (Sha $live) -eq (Sha $snap)) { 'same as at the start of this run' } else { $bad++; 'DIFFERENT from the start of this run' }
    Note "state ${n}: $vsBak; $vsSnap"
  }
  if ($bad) { Note "WARNING: $bad user state file(s) changed during this run. The copies taken at the start are in $Snapshot (nothing was written back)." }
}
Add-Type -AssemblyName System.Drawing
if (-not ('Rig.W' -as [type])) {
Add-Type -Namespace Rig -Name W -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
[DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern bool ClipCursor(IntPtr rect);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern uint MapVirtualKeyW(uint code, uint type);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
[DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool altTab);
[DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern short GetKeyState(int vk);
'@
[Rig.W]::SetProcessDPIAware() | Out-Null
}
if (-not ('Rig.T' -as [type])) { Add-Type -Namespace Rig -Name T -MemberDefinition '[DllImport("winmm.dll")] public static extern uint timeBeginPeriod(uint ms); [DllImport("winmm.dll")] public static extern uint timeEndPeriod(uint ms);' }
# DCS key names -> virtual keys (the ones a binding of ours can meet). Events carry the scan code too: DCS reads those.
$DcsKeys = @{ 'Scroll' = 0x91; 'Pause' = 0x13; 'Num0' = 0x60; 'Num1' = 0x61; 'Num2' = 0x62; 'Num3' = 0x63; 'Num4' = 0x64; 'Num5' = 0x65; 'Num6' = 0x66; 'Num7' = 0x67; 'Num8' = 0x68; 'Num9' = 0x69
  'Home' = 0x24; 'End' = 0x23; 'Insert' = 0x2D; 'Delete' = 0x2E; 'PageUp' = 0x21; 'PageDown' = 0x22; 'Space' = 0x20; 'Enter' = 0x0D; 'Tab' = 0x09; 'Back' = 0x08
  'LCtrl' = 0xA2; 'RCtrl' = 0xA3; 'LShift' = 0xA0; 'RShift' = 0xA1; 'LAlt' = 0xA4; 'RAlt' = 0xA5; 'LWin' = 0x5B; 'RWin' = 0x5C }
1..12 | ForEach-Object { $DcsKeys["F$_"] = 0x6F + $_ }; 65..90 | ForEach-Object { $DcsKeys[[string][char]$_] = $_ }; 48..57 | ForEach-Object { $DcsKeys[[string][char]$_] = $_ }
function Send-Key([int]$Vk, [bool]$Up) { [Rig.W]::keybd_event([byte]$Vk, [byte]([Rig.W]::MapVirtualKeyW([uint32]$Vk, 0)), $(if ($Up) { 2 } else { 0 }), [UIntPtr]::Zero) }
# The keyboard binding of DCS's "recenter VR Headset" (iHeadTrackerPosReset, UiLayer). The installed DCS has NO default
# key for it (Config\Input\UiLayer\keyboard\default.lua lists the command without combos); the user's own binding is in
# <Saved Games DCS>\Config\Input\UiLayer\keyboard\Keyboard.diff.lua (read only here).
function Get-RecenterBinding {
  $f = Join-Path $SavedGamesDcs 'Config\Input\UiLayer\keyboard\Keyboard.diff.lua'
  if (-not (Test-Path -LiteralPath $f)) { return $null }
  $m = [regex]::Match((Get-Content -LiteralPath $f -Raw), '(?s)\["added"\]\s*=\s*\{(?<a>(?:(?!\["name"\]).)*?)\},\s*\["name"\]\s*=\s*"recenter VR Headset"')
  if (-not $m.Success) { return $null }
  $key = [regex]::Match($m.Groups['a'].Value, '\["key"\]\s*=\s*"([^"]+)"'); if (-not $key.Success) { return $null }
  $mods = @([regex]::Matches($m.Groups['a'].Value, '\[\d+\]\s*=\s*"([^"]+)"') | ForEach-Object { $_.Groups[1].Value })
  [pscustomobject]@{ Key = $key.Groups[1].Value; Mods = $mods; File = $f }
}
function Invoke-Recenter($Rec, $A) {
  $b = if ($A.key) { [pscustomobject]@{ Key = [string]$A.key; Mods = @($A.mods | Where-Object { $_ }); File = 'scenario' } } else { Get-RecenterBinding }
  if (-not $b) { throw "recenter: no keyboard binding of 'recenter VR Headset' found (DCS has none by default; bind a key in DCS: Options, Controls, UI Layer)" }
  $all = @($b.Mods) + $b.Key; foreach ($k in $all) { if (-not $DcsKeys.ContainsKey($k)) { throw "recenter: key '$k' of the binding is not in the runner's key table" } }
  $fg = Focus-Dcs; Start-Sleep -Milliseconds 200
  if (-not $fg) { Note "$($Rec.run) recenter: DCS is not in the foreground ($Script:FocusHow)" }
  $taps = if ($A.taps) { [int]$A.taps } elseif ($b.Key -in 'Scroll', 'CapsLock', 'NumLock') { 2 } else { 1 }      # a lock key is pressed twice so its lamp ends as it was (the second press recenters again: same pose)
  for ($i = 0; $i -lt $taps; $i++) {
    foreach ($k in $b.Mods) { Send-Key $DcsKeys[$k] $false }; Send-Key $DcsKeys[$b.Key] $false; Start-Sleep -Milliseconds 90; Send-Key $DcsKeys[$b.Key] $true; foreach ($k in $b.Mods) { Send-Key $DcsKeys[$k] $true }
    Start-Sleep -Milliseconds 350
  }
  Note "$($Rec.run) recenter VR: key $(($all) -join '+') sent $taps time(s), Scroll Lock state now $([Rig.W]::GetKeyState(0x91) -band 1) (binding from $($b.File); DCS in the foreground: $fg)"
}
function Get-DcsWin { $p = Get-Process DCS -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1; if ($p) { $p.MainWindowHandle } else { [IntPtr]::Zero } }
function Get-DcsClient { $h = Get-DcsWin; if ($h -eq [IntPtr]::Zero) { return $null }
  $r = New-Object Rig.W+RECT; [Rig.W]::GetClientRect($h, [ref]$r) | Out-Null; $p = New-Object Rig.W+POINT; [Rig.W]::ClientToScreen($h, [ref]$p) | Out-Null
  [pscustomobject]@{ H = $h; X = $p.X; Y = $p.Y; W = $r.R; Ht = $r.B; Fg = ([Rig.W]::GetForegroundWindow() -eq $h) } }
function Get-ForegroundInfo { $f = [Rig.W]::GetForegroundWindow(); $procId = 0; [Rig.W]::GetWindowThreadProcessId($f, [ref]$procId) | Out-Null; $sb = New-Object Text.StringBuilder 200; [Rig.W]::GetWindowTextW($f, $sb, 200) | Out-Null
  $p = Get-Process -Id $procId -ErrorAction SilentlyContinue; "'$($sb.ToString())' of $(if ($p) { $p.ProcessName } else { "pid $procId" })" }
# Brings a window to the foreground, by stronger means in turn; the DCS briefing ignores every click and key while
# its window is not the foreground one (flown 2026-11-15 18:26: six clicks on the FLY button did nothing).
# Returns the means that worked, or $null. $Script:FocusHow keeps it for the log.
function Focus-Window([IntPtr]$h) {
  if ($h -eq [IntPtr]::Zero) { return $null }
  $is = { [Rig.W]::GetForegroundWindow() -eq $h }
  if (& $is) { return 'already' }
  if ([Rig.W]::IsIconic($h)) { [Rig.W]::ShowWindow($h, 9) | Out-Null }
  [Rig.W]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [Rig.W]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)   # Alt tap: lets a background process take the foreground
  [Rig.W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 300; if (& $is) { return 'alt tap' }
  $fg = [Rig.W]::GetForegroundWindow(); $x = 0; $ft = [Rig.W]::GetWindowThreadProcessId($fg, [ref]$x); $me = [Rig.W]::GetCurrentThreadId()
  if ($ft -and $ft -ne $me) { [Rig.W]::AttachThreadInput($me, $ft, $true) | Out-Null; [Rig.W]::BringWindowToTop($h) | Out-Null; [Rig.W]::SetForegroundWindow($h) | Out-Null; [Rig.W]::AttachThreadInput($me, $ft, $false) | Out-Null; Start-Sleep -Milliseconds 300; if (& $is) { return 'attached input' } }
  [Rig.W]::SwitchToThisWindow($h, $true); Start-Sleep -Milliseconds 400; if (& $is) { return 'switch' }
  [Rig.W]::ShowWindow($h, 6) | Out-Null; Start-Sleep -Milliseconds 300; [Rig.W]::ShowWindow($h, 9) | Out-Null; [Rig.W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 600; if (& $is) { return 'minimise and restore' }
  # a click on the window's own title bar: the window manager activates what the user clicks
  $wr = New-Object Rig.W+RECT; [Rig.W]::GetWindowRect($h, [ref]$wr) | Out-Null
  [Rig.W]::SetCursorPos([int](($wr.L + $wr.R) / 2), $wr.T + 14) | Out-Null; Start-Sleep -Milliseconds 120; [Rig.W]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [Rig.W]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 400
  if (& $is) { return 'title bar click' }
  $null
}
function Focus-Dcs { $how = Focus-Window (Get-DcsWin); $Script:FocusHow = $(if ($how) { $how } else { "FAILED, foreground is $(Get-ForegroundInfo)" }); [bool]$how }
function Shot-Dcs([string]$Out) { $c = Get-DcsClient; if (-not $c -or $c.W -le 0) { return 'no DCS window' }
  $bmp = New-Object Drawing.Bitmap $c.W, $c.Ht; $g = [Drawing.Graphics]::FromImage($bmp); try { $g.CopyFromScreen($c.X, $c.Y, 0, 0, $bmp.Size); $bmp.Save($Out, [Drawing.Imaging.ImageFormat]::Png) } finally { $g.Dispose(); $bmp.Dispose() }; "$Out $($c.W)x$($c.Ht) at $($c.X),$($c.Y) fg=$($c.Fg)" }
function Click-Dcs([int]$Cx, [int]$Cy) { $c = Get-DcsClient; if (-not $c) { throw 'DCS has no window' }
  [Rig.W]::SetCursorPos($c.X + $Cx - 3, $c.Y + $Cy - 3) | Out-Null; Start-Sleep -Milliseconds 150
  [Rig.W]::mouse_event(1, 2, 2, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 150      # a nudge, so DCS sees motion
  [Rig.W]::SetCursorPos($c.X + $Cx, $c.Y + $Cy) | Out-Null; [Rig.W]::mouse_event(1, 1, 0, 0, [UIntPtr]::Zero)
  [Rig.W]::SetCursorPos($c.X + $Cx, $c.Y + $Cy) | Out-Null; Start-Sleep -Milliseconds 300
  [Rig.W]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 120; [Rig.W]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero) }
function Key-Tap([byte]$Vk, [byte[]]$Mods = @()) { foreach ($m in $Mods) { [Rig.W]::keybd_event($m, 0, 0, [UIntPtr]::Zero) }; [Rig.W]::keybd_event($Vk, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [Rig.W]::keybd_event($Vk, 0, 2, [UIntPtr]::Zero); foreach ($m in $Mods) { [Rig.W]::keybd_event($m, 0, 2, [UIntPtr]::Zero) } }

# Copy of the applied profile folder under <run>\live with the scenario's edits and the capture layer appended LAST.
function New-RigCopy([string]$RunDir, $Sc, [string]$SwapDll, $Rec) {
  if (Get-Process DCS -ErrorAction SilentlyContinue) { throw 'DCS is running: no copy' }
  $a = Applied; if (-not $a.Folder) { throw 'no applied profile' }
  $src = [IO.Path]::GetFullPath($a.Folder).TrimEnd('\'); $dst = Join-Path $RunDir 'live'
  if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force -Confirm:$false }
  $skip = '^(ofxr-bridge-flight-.*\.log|cursor-capture-.*|hud-.*\.bmp|.*\.log|.*\.dmp|.*\.bmp|.*\.tmp)$'; $n = 0
  foreach ($f in Get-ChildItem -LiteralPath $src -Recurse -File -Force) {
    if ($f.Name -match $skip) { continue }
    $to = Join-Path $dst ($f.FullName.Substring($src.Length).TrimStart('\')); [IO.Directory]::CreateDirectory((Split-Path $to -Parent)) | Out-Null
    Copy-Item -LiteralPath $f.FullName -Destination $to -Force; $n++
  }
  $from = [regex]::Escape($src.Replace('\', '\\'))
  foreach ($j in @(Get-ChildItem -LiteralPath (Join-Path $dst 'layers') -Filter '*.json' -File) + (Get-Item -LiteralPath (Join-Path $dst 'launch.json'))) {
    $t = [IO.File]::ReadAllText($j.FullName)
    Write-Utf8 $j.FullName ([regex]::Replace($t, $from + '(?=\\\\|")', { $dst.Replace('\', '\\') }, 'IgnoreCase'))
  }
  $layerDll = Join-Path $dst "ofxr\$OfxrDll"; $installedSha = Sha $layerDll
  if ($SwapDll) {
    if (-not (Test-Path -LiteralPath $SwapDll -PathType Leaf)) { throw "OFXR DLL to fly not found: $SwapDll" }
    Copy-Item -LiteralPath $SwapDll -Destination $layerDll -Force
    if ((Sha $layerDll) -ne (Sha $SwapDll)) { throw 'the OFXR DLL in the copy is not the one given' }
    if ((Sha (Join-Path $src "ofxr\$OfxrDll")) -ne $installedSha) { throw 'the applied profile folder changed during the copy' }
  }
  $flownSha = Sha $layerDll
  if ($QvDll) {
    $qvIn = Join-Path $dst 'quadviews\XR_APILAYER_MBUCCHIA_quad_views_foveated.dll'; $qvWas = Sha $qvIn
    if (-not (Test-Path -LiteralPath $QvDll -PathType Leaf)) { throw "Quad Views DLL to fly not found: $QvDll" }
    Copy-Item -LiteralPath $QvDll -Destination $qvIn -Force; if ((Sha $qvIn) -ne (Sha $QvDll)) { throw 'the Quad Views DLL in the copy is not the one given' }
    Note "Quad Views layer DLL: installed (applied profile) sha256 $qvWas; flown (the copy) sha256 $(Sha $qvIn) = $QvDll"
    if ($Rec) { $Rec['qvInstalledSha256'] = $qvWas; $Rec['qvFlownSha256'] = (Sha $qvIn) }
  }
  Note "OFXR layer DLL: installed (applied profile) sha256 $installedSha; flown (the copy) sha256 $flownSha$(if ($SwapDll) { " = $SwapDll" } else { ' (the installed one)' })"
  if ($Rec) { $Rec.ofxrInstalledSha256 = $installedSha; $Rec.ofxrFlownSha256 = $flownSha; $Rec.ofxrFlownFrom = $(if ($SwapDll) { $SwapDll } else { 'installed' }) }
  Edit-IniFile (Join-Path $dst 'ofxr\ofxr_bridge.ini') 'ofxr' $Sc.ofxr
  Edit-IniFile (Join-Path $dst 'ofxr\ofxr_bridge.ini') 'diagnostics' $Sc.diagnostics
  Edit-IniFile (Join-Path $dst 'quadviews\settings.cfg') 'exe:DCS' $Sc.quadviews
  # the capture layer: LAST in the list = closest to the runtime, below the frame-generation layer
  $lp = Join-Path $dst 'launch.json'; $launch = [IO.File]::ReadAllText($lp) | ConvertFrom-Json
  $names = @(([string]$launch.environment.XR_ENABLE_API_LAYERS -split ';') | Where-Object { $_ -and $_ -ne $TapName })
  if ($names.Count -eq 0 -or $names[-1] -ne 'XR_APILAYER_XRFrameBridge_diagnostic') { Note "NOTE: the last layer of the applied chain is '$($names[-1])', not the frame-generation layer: the capture layer then sees what THAT layer submits" }
  $launch.environment.XR_ENABLE_API_LAYERS = (($names + $TapName) -join ';')
  $launch.environment.XR_API_LAYER_PATH = (@(([string]$launch.environment.XR_API_LAYER_PATH -split ';') | Where-Object { $_ -and $_ -ne $TapDir }) + $TapDir) -join ';'
  $tap = Join-Path $RunDir 'tap'; New-Item -ItemType Directory -Force $tap | Out-Null
  $launch.environment | Add-Member -NotePropertyName RIGTAP_DIR -NotePropertyValue $tap -Force
  $launch.environment | Add-Member -NotePropertyName RIGTAP_MAX_MB -NotePropertyValue ([string]$(if ($Sc.maxDiskMb) { $Sc.maxDiskMb } else { 6144 })) -Force
  if ($Sc.tapEnv) { foreach ($p in $Sc.tapEnv.PSObject.Properties) { $launch.environment | Add-Member -NotePropertyName $p.Name -NotePropertyValue ([string]$p.Value) -Force }; Note "capture layer environment: $(($Sc.tapEnv.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' ')" }
  Write-Utf8 $lp ($launch | ConvertTo-Json -Depth 8)
  # checks: nothing points into the source; every layer library exists; the settings file is the copy's
  foreach ($p in $launch.environment.PSObject.Properties) { foreach ($part in ([string]$p.Value -split ';')) { if ($part -and $part.StartsWith($src + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "launch.json $($p.Name) still points into the source: $part" } } }
  foreach ($m in Get-ChildItem -LiteralPath (Join-Path $dst 'layers') -Filter '*.json' -File) {
    $lib = [string]([IO.File]::ReadAllText($m.FullName) | ConvertFrom-Json).api_layer.library_path
    if (-not $lib.StartsWith($dst + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "$($m.Name): library outside the copy: $lib" }
    if (-not (Test-Path -LiteralPath $lib)) { throw "$($m.Name): library missing: $lib" }
  }
  if (-not ([string]$launch.environment.DCSVR_QUAD_SETTINGS).StartsWith($dst + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "DCSVR_QUAD_SETTINGS is not in the copy: $($launch.environment.DCSVR_QUAD_SETTINGS)" }
  $ini = ([IO.File]::ReadAllText((Join-Path $dst 'ofxr\ofxr_bridge.ini')) -split "`r?`n" | Where-Object { $_ -match '^(foveated_\w+|deep_pipeline|pipelined_deep_hold|adaptive_frame_gen|adaptive_cadence|triple_frame_gen|smooth_cursor|hud_symbology|hud_capture|logging_enabled|frame_records)=' }) -join ' '
  $qv = ([IO.File]::ReadAllText((Join-Path $dst 'quadviews\settings.cfg')) -split "`r?`n" | Where-Object { $_ -match '^(dcsvr_two_layers|debug_\w+|force_no_eye_tracking)=' }) -join ' '
  Note "copy of $($a.Id) ($n files) -> $dst; layers: $($launch.environment.XR_ENABLE_API_LAYERS); ofxr: $ini; quad views: $qv"
  $dst
}
function Start-RigFlight([string]$RunDir, [string]$LaunchJson, [string]$Mission, $Rec) {
  if (Get-Process DCS -ErrorAction SilentlyContinue) { throw 'DCS already running' }
  if (Get-Process probe_run -ErrorAction SilentlyContinue) { throw 'probe_run is running (other session)' }
  $launch = [IO.File]::ReadAllText($LaunchJson) | ConvertFrom-Json
  foreach ($part in ([string]$launch.environment.XR_API_LAYER_PATH -split ';')) { if ($part -and -not (Test-Path -LiteralPath $part -PathType Container)) { throw "XR_API_LAYER_PATH folder missing: $part" } }
  Note (Pimax-Ready)
  $oldHead = Dcs-LogHead
  $psi = New-Object Diagnostics.ProcessStartInfo
  $psi.FileName = $launch.executable; $psi.Arguments = "--mission `"$Mission`""; $psi.WorkingDirectory = Split-Path $launch.executable; $psi.UseShellExecute = $false
  foreach ($p in $launch.environment.PSObject.Properties) { $psi.EnvironmentVariables[$p.Name] = [string]$p.Value }
  $Rec.started = (Get-Date).ToString('o')
  $proc = [Diagnostics.Process]::Start($psi); $Rec.pid = $proc.Id
  Note "$($Rec.run) DCS started pid $($proc.Id), environment from $LaunchJson"
  $layers = -2; $t0 = Get-Date
  while (((Get-Date) - $t0).TotalSeconds -lt 150) {
    if ($proc.HasExited) { $layers = -1; break }
    if ((Dcs-LogHead) -ne $oldHead) { $m = [regex]::Match((Read-Shared $DcsLog), 'OpenXR: Available Layers: \((\d+)\)'); if ($m.Success) { $layers = [int]$m.Groups[1].Value; break } }
    Start-Sleep 2
  }
  $Rec.layers = $layers
  if ($layers -le 0) { throw "VR start failed (layers $layers)" }
  Note "$($Rec.run) OpenXR: $layers API layer(s) available"
  $f = (Applied).Folder; $ap = Get-Content -LiteralPath (Join-Path $f 'profile.json') -Raw | ConvertFrom-Json
  if ($ap.usesBoostHelper -or $ap.cpuBoost) {      # what the app's own launch does for this profile (a2.ps1 / a6.ps1)
    $bp = New-Object Diagnostics.ProcessStartInfo $Cli; $bp.WorkingDirectory = Split-Path $Cli; $bp.UseShellExecute = $false; $bp.CreateNoWindow = $true
    foreach ($x in @('boost', '--state', $State, '--profile', (Join-Path $f 'profile.json'), '--dcs-pid', "$($proc.Id)", '--dcs-exe', $launch.executable, '--dcs-log', $DcsLog)) { $bp.ArgumentList.Add($x) }
    $b = [Diagnostics.Process]::Start($bp); Note "$($Rec.run) boost helper pid $($b.Id) for DCS pid $($proc.Id)"
  }
  $t0 = Get-Date; $passed = $false
  while (((Get-Date) - $t0).TotalSeconds -lt 400) {
    if ($proc.HasExited) { throw 'DCS exited during the mission load' }
    if ((Read-Shared $DcsLog) -match 'loadMission Done: Control passed to the player') { $passed = $true; break }
    Start-Sleep 3
  }
  if (-not $passed) { throw 'mission did not load within 400 s' }
  $Rec.briefing = (Get-Date).ToString('o'); Note "$($Rec.run) mission loaded (briefing)"
  $tapLog = Read-Shared (Join-Path $RunDir 'tap\rigtap.log')
  if ($tapLog -notmatch 'graphics binding D3D12') { throw "the capture layer did not see a D3D12 session (rigtap.log: $(($tapLog -split "`n" | Select-Object -Last 2) -join ' | ')): no flight" }
  Note "$($Rec.run) capture layer loaded: $((($tapLog -split "`n") | Select-String 'graphics binding' | Select-Object -Last 1).Line)"
}
# FLY: the real start is the line "WWT (Main): <aircraft>" in dcs.log (fly4.ps1; Pause/Break does not start these missions).
function Click-Fly([string]$RunDir, $Rec, [bool]$AtOrigin) {
  $t0 = Get-Date; $c = $null
  while (((Get-Date) - $t0).TotalSeconds -lt 40) { $c = Get-DcsClient; if ($c -and $c.W -ge 800) { break }; Start-Sleep 2 }
  Start-Sleep 4
  if (-not $c) { throw 'DCS has no window' }
  if ($AtOrigin) {   # debug_simulate_tracking confines the mouse to the desktop's top-left 1000 x 1000 px: the window has to be there
    $wr = New-Object Rig.W+RECT; [Rig.W]::GetWindowRect($c.H, [ref]$wr) | Out-Null
    [Rig.W]::SetWindowPos($c.H, [IntPtr]::Zero, $wr.L - $c.X, $wr.T - $c.Y, 0, 0, 0x0001 -bor 0x0004 -bor 0x0010) | Out-Null   # NOSIZE | NOZORDER | NOACTIVATE
    Start-Sleep 1; $c = Get-DcsClient; Note "$($Rec.run) DCS window moved: client origin now $($c.X),$($c.Y)"
  }
  $Rec.window = "$($c.W)x$($c.Ht)"; Note "$($Rec.run) DCS client window $($Rec.window) at $($c.X),$($c.Y)"
  Shot-Dcs (Join-Path $RunDir 'briefing.png') | Out-Null
  $flyPattern = 'WWT \(Main\): (?!Wwt export|Export )\S'; $ok = $false; $flyAt = $null
  # Which pose: the briefing's FLY button is seen in DCS's window only with the eye in the cockpit (flown 18:25 and
  # 18:29, untracked headset: at 929,700); with the tracked headset on its stand the panel is out of the picture.
  # The click that starts the mission is the SAME in both poses: 892,895 of 1076x1060. The mouse drives DCS's own
  # interface cursor in interface coordinates, not the place where the picture shows the button.
  $seen = (& py (Join-Path $Rig 'hudfind.py') --png (Join-Path $RunDir 'briefing.png') --fly 2>&1 | Out-String).Trim(); $Rec.flyButtonSeen = $seen
  $Rec.poseAtBriefing = $(if ($seen -match '\[\[') { 'eye in the cockpit (FLY button in the picture)' } else { 'eye not in the cockpit (no FLY button in the picture)' })
  Note "$($Rec.run) pose at the briefing: $($Rec.poseAtBriefing); tracking: $(Slam)"
  $tries = @(@(892, 895), @(896, 905), @(928, 700))
  if ($Rec.window -ne '1076x1060') { $tries = @(@([int](892 * $c.W / 1076), [int](895 * $c.Ht / 1060)), @([int](896 * $c.W / 1076), [int](905 * $c.Ht / 1060))) }
  $confirm = { param([int]$Sec) $tw = Get-Date; while (((Get-Date) - $tw).TotalSeconds -lt $Sec) { if ((Read-Shared $DcsLog) -match $flyPattern) { return $true }; Start-Sleep -Milliseconds 500 }; $false }
  for ($round = 1; $round -le 2 -and -not $ok; $round++) {
    foreach ($xy in $tries) {
      $fg = Focus-Dcs
      if (-not $fg) { Note "$($Rec.run) DCS is NOT in the foreground ($Script:FocusHow): a click would do nothing"; Start-Sleep 2; $fg = Focus-Dcs; if (-not $fg) { continue } }
      Click-Dcs $xy[0] $xy[1]; $flyAt = Get-Date; Note "$($Rec.run) click at $($xy[0]),$($xy[1]) (DCS in the foreground: $fg, by $Script:FocusHow)"
      if (& $confirm 8) { $ok = $true; Note "$($Rec.run) FLY clicked at $($xy[0]),$($xy[1]): confirmed in dcs.log"; break }
    }
    if (-not $ok -and (Focus-Dcs)) {      # the briefing's own key: Pause/Break, sent with its scan code (0x45 extended = DirectInput's PAUSE)
      [Rig.W]::keybd_event(0x13, 0x45, 1, [UIntPtr]::Zero); Start-Sleep -Milliseconds 120; [Rig.W]::keybd_event(0x13, 0x45, 3, [UIntPtr]::Zero); $flyAt = Get-Date
      Note "$($Rec.run) Pause/Break sent (DCS in the foreground by $Script:FocusHow)"
      if (& $confirm 8) { $ok = $true; Note "$($Rec.run) started with Pause/Break: confirmed in dcs.log" }
    }
  }
  if (-not $ok) { Note "$($Rec.run) FLY not confirmed; foreground now: $(Get-ForegroundInfo)" }
  if (-not $ok) { Shot-Dcs (Join-Path $RunDir 'after-fly.png') | Out-Null; throw 'FLY not confirmed in dcs.log (picture: after-fly.png)' }
  $Rec.fly = $flyAt.ToString('o'); $Rec.flyConfirmed = $true
  $flyAt
}
# ---- schedule actions --------------------------------------------------------------------------------------------
# The twin's capture plan from the option-on capture of the same tag: the same tangent window as the ON arm's focus
# layer plus a margin at full resolution (stream 1), and the whole image at the scale that brings it to the ON
# periphery's pixel size (stream 2). Without a reference: the whole image, the layer's budget picks the scale.
function Get-TwinPlan([string]$Scenario, [string]$ArmName, [string]$Tag, [double]$Margin = 0.15) {
  foreach ($d in @(Get-ChildItem -LiteralPath $Runs -Directory -Filter "${Scenario}_${ArmName}_*_on" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)) {
    $cap = Get-ChildItem -LiteralPath (Join-Path $d.FullName 'tap') -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match "^cap\d+_$([regex]::Escape($Tag))$" } | Select-Object -First 1
    if (-not $cap -or -not (Test-Path -LiteralPath (Join-Path $cap.FullName 'capture.jsonl'))) { continue }
    foreach ($line in (Get-Content -LiteralPath (Join-Path $cap.FullName 'capture.jsonl') -TotalCount 12 | Select-Object -Skip 1)) {
      $s = $line | ConvertFrom-Json; $proj = @($s.layers | Where-Object { $_.type -eq 'projection' }); if ($proj.Count -lt 2) { continue }
      $width = { param($l) [Math]::Tan($l.views[0].fov[1]) - [Math]::Tan($l.views[0].fov[0]) }
      $per = $proj | Sort-Object { & $width $_ } -Descending | Select-Object -First 1; $foc = $proj | Sort-Object { & $width $_ } | Select-Object -First 1
      $plan = [ordered]@{ from = $cap.FullName; peripheryWidth = [int]$per.views[0].rect[2] }
      for ($e = 0; $e -lt 2; $e++) { $t = @($foc.views[$e].fov | ForEach-Object { [Math]::Tan($_) }); $mx = ($t[1] - $t[0]) * $Margin; $my = ($t[2] - $t[3]) * $Margin
        $plan["window$e"] = ('{0:F6},{1:F6},{2:F6},{3:F6}' -f ($t[0] - $mx), ($t[1] + $mx), ($t[2] + $my), ($t[3] - $my)) }
      return [pscustomobject]$plan
    }
  }
  $null
}
# Where the focus image is in DCS's window, as fractions of the client area (centre and half sizes), ASSUMING the
# window shows the left eye's whole image: from the newest two-layer capture under rig\runs (the left eye's periphery
# and focus fields of view), else the values flown 2026-11-15 (fixed foveation, box centred on the eye's axis).
function Get-FocusBox {
  foreach ($cap in @(Get-ChildItem -LiteralPath $Runs -Recurse -Filter 'capture.jsonl' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match '_on\\tap\\' -and $_.FullName -notmatch '\\gaze_' } | Sort-Object LastWriteTime -Descending | Select-Object -First 6)) {
    foreach ($line in (Get-Content -LiteralPath $cap.FullName -TotalCount 4 | Select-Object -Skip 1)) {
      $s = $line | ConvertFrom-Json; $proj = @($s.layers | Where-Object { $_.type -eq 'projection' }); if ($proj.Count -lt 2) { continue }
      $tan = { param($v) @($v.fov | ForEach-Object { [Math]::Tan($_) }) }
      $per = $proj | Sort-Object { $t = & $tan $_.views[0]; $t[1] - $t[0] } -Descending | Select-Object -First 1; $foc = $proj | Sort-Object { $t = & $tan $_.views[0]; $t[1] - $t[0] } | Select-Object -First 1
      $p = & $tan $per.views[0]; $f = & $tan $foc.views[0]
      return [pscustomobject]@{ cx = (($f[0] + $f[1]) / 2 - $p[0]) / ($p[1] - $p[0]); cy = ($p[2] - ($f[2] + $f[3]) / 2) / ($p[2] - $p[3]); hw = ($f[1] - $f[0]) / 2 / ($p[1] - $p[0]); hh = ($f[2] - $f[3]) / 2 / ($p[2] - $p[3]); from = $cap.Directory.Name }
    }
  }
  [pscustomobject]@{ cx = 0.581; cy = 0.5; hw = 0.150; hh = 0.150; from = 'flown 2026-11-15' }
}
# Mouse log of a run (mouse.csv): time, QPC (the capture's clock: capture.jsonl "qpc"), label, screen x,y, window fraction x,y.
function Log-Mouse([string]$RunDir, [string]$Label, $c) {
  $pp = New-Object Rig.W+POINT; [Rig.W]::GetCursorPos([ref]$pp) | Out-Null; $f = Join-Path $RunDir 'mouse.csv'
  if (-not (Test-Path -LiteralPath $f)) { [IO.File]::WriteAllText($f, "time,qpc,label,screenX,screenY,windowFx,windowFy`n") }
  [IO.File]::AppendAllText($f, ("{0},{1},{2},{3},{4},{5:F5},{6:F5}`n" -f (Get-Date).ToString('o'), [Diagnostics.Stopwatch]::GetTimestamp(), $Label, $pp.X, $pp.Y, (($pp.X - $c.X) / $c.W), (($pp.Y - $c.Y) / $c.Ht)))
}
# Moves the mouse to a screen point at a given speed (pixels per second), in small steps with relative motion too
# (DCS reads raw input): never a jump.
function Move-MouseSlow([string]$RunDir, [string]$Label, [double]$Tx, [double]$Ty, [double]$Speed, $c) {
  $pp = New-Object Rig.W+POINT; [Rig.W]::GetCursorPos([ref]$pp) | Out-Null
  if ($pp.X -lt $c.X -or $pp.X -gt $c.X + $c.W -or $pp.Y -lt $c.Y -or $pp.Y -gt $c.Y + $c.Ht) { [Rig.W]::SetCursorPos([int]($c.X + $c.W / 2), [int]($c.Y + $c.Ht / 2)) | Out-Null; [Rig.W]::GetCursorPos([ref]$pp) | Out-Null }   # from outside the window: start at its centre
  $x = [double]$pp.X; $y = [double]$pp.Y; $sw = [Diagnostics.Stopwatch]::StartNew(); $last = 0.0
  while ($true) {
    $dx = $Tx - $x; $dy = $Ty - $y; $dist = [Math]::Sqrt($dx * $dx + $dy * $dy); if ($dist -lt 0.75) { break }
    $now = $sw.Elapsed.TotalSeconds; $step = [Math]::Min($dist, [Math]::Max(1.0, $Speed * ($now - $last))); $last = $now
    $nx = $x + $dx / $dist * $step; $ny = $y + $dy / $dist * $step
    [Rig.W]::mouse_event(1, ([int]$nx - [int]$x), ([int]$ny - [int]$y), 0, [UIntPtr]::Zero); [Rig.W]::SetCursorPos([int]$nx, [int]$ny) | Out-Null
    $x = $nx; $y = $ny; Log-Mouse $RunDir $Label $c; Start-Sleep -Milliseconds 8
  }
}
# artifacts\rig\cursor-calibration.json (made from a cursor-calib flight; it belongs to one headset and window size,
# so it is not tracked): the left eye's tangent of DCS's cockpit cursor = a * (window fraction x, y) + b.
function Get-CursorCalibration {
  $f = Join-Path $RigData 'cursor-calibration.json'; if (-not (Test-Path -LiteralPath $f)) { throw "no ${f}: fly -Scenario cursor-calib and derive the calibration first" }
  Get-Content -LiteralPath $f -Raw | ConvertFrom-Json
}
function Arm-Capture([string]$Tap, $A, $Twin) {
  $lines = @("frames=$(if ($A.frames) { $A.frames } else { 40 })", "budget_mb=$(if ($A.budget_mb) { $A.budget_mb } else { 2048 })")
  if ($Twin) {      # like with like: the ON arm's focus window at full resolution + the whole image near the ON periphery's size
    $lines += "window0=$($Twin.window0)"; $lines += "window1=$($Twin.window1)"; $lines += "scale2=$($Twin.scale2)"
    foreach ($k in 'tag', 'delay_ms') { if ($null -ne $A.$k) { $lines += "$k=$($A.$k)" } }
    $tmp = Join-Path $Tap 'arm.tmp'; [IO.File]::WriteAllText($tmp, ($lines -join "`n") + "`n", [Text.Encoding]::ASCII); Move-Item -LiteralPath $tmp -Destination (Join-Path $Tap 'arm.txt') -Force; return
  }
  foreach ($k in 'scale', 'crop', 'tag', 'delay_ms', 'scale2', 'crop2', 'window0', 'window1') { if ($null -ne $A.$k) { $lines += "$k=$($A.$k)" } }
  $tmp = Join-Path $Tap 'arm.tmp'; [IO.File]::WriteAllText($tmp, ($lines -join "`n") + "`n", [Text.Encoding]::ASCII)
  Move-Item -LiteralPath $tmp -Destination (Join-Path $Tap 'arm.txt') -Force
}
function Do-Action([string]$RunDir, $Rec, $A) {
  switch ([string]$A.do) {
    'capture' {
      $twin = $null
      if (-not $Rec.option -and -not $A.crop -and -not $A.scale -and -not $A.window0) {      # a scenario's own crop / scale is kept as it is
        $twin = Get-TwinPlan $Rec.scenario $Rec.arm ([string]$A.tag)
        if ($twin) { $twin | Add-Member -NotePropertyName scale2 -NotePropertyValue ([Math]::Max(1, [int][Math]::Round($Rec.composedWidth / [double]$twin.peripheryWidth))) -Force
          Note "$($Rec.run) twin capture '$($A.tag)': windows $($twin.window0) | $($twin.window1) at full resolution + whole image at scale $($twin.scale2) (from $($twin.from))" }
        else { Note "$($Rec.run) twin capture '$($A.tag)': no option-on capture of this scenario, arm and tag to take the window from: whole image, the layer's budget picks the scale (fly the ON arm first, or -Both)" }
      }
      Arm-Capture (Join-Path $RunDir 'tap') $A $twin
    }
    'recenter' { Invoke-Recenter $Rec $A }
    'mouse-path' {   # slow, logged mouse path through window fractions: { label, points: [[fx,fy],...], speedPx, settleMs }
      Focus-Dcs | Out-Null; $c = Get-DcsClient
      $plist = @($A.points); if ($plist.Count -eq 2 -and $plist[0] -isnot [array]) { $plist = @(, $plist) }      # one point given flat
      foreach ($pt in $plist) { Move-MouseSlow $RunDir ([string]$A.label) ($c.X + $c.W * [double]$pt[0]) ($c.Y + $c.Ht * [double]$pt[1]) ([double]$(if ($A.speedPx) { $A.speedPx } else { 120 })) $c }
      if ($A.settleMs) { Start-Sleep -Milliseconds ([int]$A.settleMs) }
    }
    'cursor-stretch' {   # one stretch of the cursor scenario, in the left eye's tangents (needs rig\cursor-calibration.json):
      # slow approach (or a marked jump) to the path's start, a capture window around the whole path in both layers, then the path
      Focus-Dcs | Out-Null; $c = Get-DcsClient; $cal = Get-CursorCalibration
      $toWin = { param($tx, $ty) $u = [double]$tx - $cal.b[0]; $v = [double]$ty - $cal.b[1]; $det = $cal.a[0][0] * $cal.a[1][1] - $cal.a[0][1] * $cal.a[1][0]
        @((($cal.a[1][1] * $u - $cal.a[0][1] * $v) / $det), ((-$cal.a[1][0] * $u + $cal.a[0][0] * $v) / $det)) }      # tangent -> window fraction
      $ctr = @([double]$A.centreTan[0], [double]$A.centreTan[1]); $rx = [double]$(if ($A.radiusTan) { $A.radiusTan[0] } else { 0 }); $ry = [double]$(if ($A.radiusTan) { $A.radiusTan[1] } else { 0 })
      $at = { param($ph) if ($A.shape -eq 'line') { @(($ctr[0] + $rx * [Math]::Sin($ph)), ($ctr[1] + $ry * [Math]::Sin($ph))) } else { @(($ctr[0] + $rx * [Math]::Cos($ph)), ($ctr[1] + $ry * [Math]::Sin($ph))) } }
      $speed = [double]$(if ($A.speedPx) { $A.speedPx } else { 60 })
      $p0 = & $at 0; $w0 = & $toWin $p0[0] $p0[1]
      foreach ($w in @($w0)) { if ($w -lt 0.03 -or $w -gt 0.97) { throw "cursor-stretch '$($A.label)': the path starts outside DCS's window (fraction $([Math]::Round($w, 3))): calibration or stretch wrong" } }
      if ($A.approach -eq 'jump') { [Rig.W]::SetCursorPos([int]($c.X + $c.W * $w0[0]), [int]($c.Y + $c.Ht * $w0[1])) | Out-Null; [Rig.W]::mouse_event(1, 1, 0, 0, [UIntPtr]::Zero); Log-Mouse $RunDir "$($A.label):jump" $c }
      else { Move-MouseSlow $RunDir "$($A.label):approach" ($c.X + $c.W * $w0[0]) ($c.Y + $c.Ht * $w0[1]) ([double]$(if ($A.approachSpeedPx) { $A.approachSpeedPx } else { 120 })) $c }
      Start-Sleep -Milliseconds 500
      # the capture window: the path's box plus a margin (the right eye sees a near cursor up to about 0.1 of a tangent to the left)
      $mx = [double]$(if ($A.marginTan) { $A.marginTan[0] } else { 0.13 }); $my = [double]$(if ($A.marginTan) { $A.marginTan[1] } else { 0.09 })
      $win = ('{0:F5},{1:F5},{2:F5},{3:F5}' -f ($ctr[0] - [Math]::Abs($rx) - $mx), ($ctr[0] + [Math]::Abs($rx) + $mx), ($ctr[1] + [Math]::Abs($ry) + $my), ($ctr[1] - [Math]::Abs($ry) - $my))
      $cap = [pscustomobject]@{ frames = $(if ($A.frames) { $A.frames } else { 56 }); window0 = $win; window1 = $win; tag = [string]$A.label; delay_ms = $(if ($null -ne $A.captureDelayMs) { $A.captureDelayMs } else { 2500 }) }
      Arm-Capture (Join-Path $RunDir 'tap') $cap $null
      $start = Get-Date; $sw = [Diagnostics.Stopwatch]::StartNew(); $rpx = [Math]::Abs($rx / $cal.a[0][0]) * $c.W; $rpy = [Math]::Abs($ry / $cal.a[1][1]) * $c.Ht      # the path's half sizes in window pixels
      $per = $(if ($A.shape -eq 'line') { 4 * [Math]::Sqrt($rpx * $rpx + $rpy * $rpy) } else { 2 * [Math]::PI * ($rpx + $rpy) / 2 }) / $speed      # seconds per turn at that speed
      if ($per -lt 2) { $per = 2 }
      while ($sw.Elapsed.TotalSeconds -lt [double]$A.seconds) {
        $pt = & $at (2 * [Math]::PI * $sw.Elapsed.TotalSeconds / $per); $w = & $toWin $pt[0] $pt[1]
        $x = [int]($c.X + $c.W * $w[0]); $y = [int]($c.Y + $c.Ht * $w[1]); $pp = New-Object Rig.W+POINT; [Rig.W]::GetCursorPos([ref]$pp) | Out-Null
        [Rig.W]::SetCursorPos($x, $y) | Out-Null; if ($x -ne $pp.X -or $y -ne $pp.Y) { [Rig.W]::mouse_event(1, ($x - $pp.X), ($y - $pp.Y), 0, [UIntPtr]::Zero); [Rig.W]::SetCursorPos($x, $y) | Out-Null }
        Log-Mouse $RunDir ([string]$A.label) $c; Start-Sleep -Milliseconds 8
      }
      $Rec.stretches += [ordered]@{ zone = [string]$A.label; start = $start.ToString('o'); end = (Get-Date).ToString('o'); box = "tangents centre $($ctr -join ','), radius $rx,$ry, $($A.shape), approach $(if ($A.approach) { $A.approach } else { 'slow' }), capture window $win, turn $([Math]::Round($per, 1)) s" }
    }
    'cursor-fast' {   # four-points round: the mouse on a circle (or to and fro on a line) at a set speed in WINDOW px/s, stepped on a clock
      # (tickHz, default 45 = one step per real frame at 2X on 90 Hz; the position of step k is the path at k / tickHz, so the
      # steps are speedPx / tickHz window px each, not 1 px). Same calibration, capture window and mouse.csv as cursor-stretch.
      Focus-Dcs | Out-Null; $c = Get-DcsClient; $cal = Get-CursorCalibration
      $toWin = { param($tx, $ty) $u = [double]$tx - $cal.b[0]; $v = [double]$ty - $cal.b[1]; $det = $cal.a[0][0] * $cal.a[1][1] - $cal.a[0][1] * $cal.a[1][0]
        @((($cal.a[1][1] * $u - $cal.a[0][1] * $v) / $det), ((-$cal.a[1][0] * $u + $cal.a[0][0] * $v) / $det)) }
      $ctr = @([double]$A.centreTan[0], [double]$A.centreTan[1]); $rx = [double]$A.radiusTan[0]; $ry = [double]$A.radiusTan[1]
      $speed = [double]$A.speedPx; $hz = [double]$(if ($A.tickHz) { $A.tickHz } else { 45 })
      $rpx = [Math]::Abs($rx / $cal.a[0][0]) * $c.W; $rpy = [Math]::Abs($ry / $cal.a[1][1]) * $c.Ht
      $line = ($A.shape -eq 'line'); $len = $(if ($line) { 4 * [Math]::Sqrt($rpx * $rpx + $rpy * $rpy) } else { 2 * [Math]::PI * ($rpx + $rpy) / 2 }); $per = $len / $speed      # seconds per turn, NOT clamped
      $at = { param($t) $u = ($t / $per) % 1.0
        if ($line) { $w = $(if ($u -lt 0.25) { 4 * $u } elseif ($u -lt 0.75) { 2 - 4 * $u } else { 4 * $u - 4 }); @(($ctr[0] + $rx * $w), ($ctr[1] + $ry * $w)) }      # triangle wave: constant speed
        else { @(($ctr[0] + $rx * [Math]::Cos(2 * [Math]::PI * $u)), ($ctr[1] + $ry * [Math]::Sin(2 * [Math]::PI * $u))) } }
      $p0 = & $at 0; $w0 = & $toWin $p0[0] $p0[1]
      foreach ($w in @($w0)) { if ($w -lt 0.02 -or $w -gt 0.985) { throw "cursor-fast '$($A.label)': the path starts outside DCS's window (fraction $([Math]::Round($w, 3)))" } }
      Move-MouseSlow $RunDir "$($A.label):approach" ($c.X + $c.W * $w0[0]) ($c.Y + $c.Ht * $w0[1]) ([double]$(if ($A.approachSpeedPx) { $A.approachSpeedPx } else { 160 })) $c
      Start-Sleep -Milliseconds 500
      $mx = [double]$(if ($A.marginTan) { $A.marginTan[0] } else { 0.13 }); $my = [double]$(if ($A.marginTan) { $A.marginTan[1] } else { 0.09 })
      $win = ('{0:F5},{1:F5},{2:F5},{3:F5}' -f ($ctr[0] - [Math]::Abs($rx) - $mx), ($ctr[0] + [Math]::Abs($rx) + $mx), ($ctr[1] + [Math]::Abs($ry) + $my), ($ctr[1] - [Math]::Abs($ry) - $my))
      $cap = [pscustomobject]@{ frames = $(if ($A.frames) { $A.frames } else { 56 }); window0 = $win; window1 = $win; tag = [string]$A.label; delay_ms = $(if ($null -ne $A.captureDelayMs) { $A.captureDelayMs } else { 2000 }) }
      Arm-Capture (Join-Path $RunDir 'tap') $cap $null
      $start = Get-Date; $sw = [Diagnostics.Stopwatch]::StartNew(); $k = 0; $late = 0; $f = Join-Path $RunDir 'mouse.csv'; $rows = New-Object Text.StringBuilder
      [Rig.T]::timeBeginPeriod(1) | Out-Null
      try {
        while ($true) {
          $tk = $k / $hz; if ($tk -ge [double]$A.seconds) { break }
          while ($sw.Elapsed.TotalSeconds -lt $tk) { [Threading.Thread]::SpinWait(20) }      # spin only: Sleep(1) lasts 15.6 ms in a process without a foreground window (first flight: steps 16 / 31 ms apart)
          if ($sw.Elapsed.TotalSeconds - $tk -gt 0.004) { $late++ }
          $pt = & $at $tk; $w = & $toWin $pt[0] $pt[1]
          $x = [int][Math]::Round($c.X + $c.W * $w[0]); $y = [int][Math]::Round($c.Y + $c.Ht * $w[1]); $pp = New-Object Rig.W+POINT; [Rig.W]::GetCursorPos([ref]$pp) | Out-Null
          [Rig.W]::SetCursorPos($x, $y) | Out-Null; if ($x -ne $pp.X -or $y -ne $pp.Y) { [Rig.W]::mouse_event(1, ($x - $pp.X), ($y - $pp.Y), 0, [UIntPtr]::Zero); [Rig.W]::SetCursorPos($x, $y) | Out-Null }
          [void]$rows.Append(("{0},{1},{2},{3},{4},{5:F5},{6:F5}`n" -f (Get-Date).ToString('o'), [Diagnostics.Stopwatch]::GetTimestamp(), [string]$A.label, $x, $y, (($x - $c.X) / $c.W), (($y - $c.Y) / $c.Ht)))
          $k++
        }
      } finally { [Rig.T]::timeEndPeriod(1) | Out-Null }
      if (-not (Test-Path -LiteralPath $f)) { [IO.File]::WriteAllText($f, "time,qpc,label,screenX,screenY,windowFx,windowFy`n") }
      [IO.File]::AppendAllText($f, $rows.ToString())
      Note "$($Rec.run) cursor-fast '$($A.label)': $k steps at $hz Hz, $([Math]::Round($speed / $hz, 2)) window px a step ($speed px/s), turn $([Math]::Round($per, 2)) s of $([Math]::Round($len)) px, $late steps more than 4 ms late"
      $Rec.stretches += [ordered]@{ zone = [string]$A.label; start = $start.ToString('o'); end = (Get-Date).ToString('o'); box = "tangents centre $($ctr -join ','), radius $rx,$ry, $(if ($line) { 'line' } else { 'circle' }), $speed window px/s in steps at $hz Hz, capture window $win, turn $([Math]::Round($per, 2)) s" }
    }
    'mouse-zone' {   # the mouse for N seconds in one zone: inside the focus image, in the periphery, or across the focus border
      Focus-Dcs | Out-Null; $c = Get-DcsClient; $fb = Get-FocusBox
      $cx = $c.X + $c.W * $fb.cx; $cy = $c.Y + $c.Ht * $fb.cy; $hw = $c.W * $fb.hw; $hh = $c.Ht * $fb.hh; $per = [double]$(if ($A.periodMs) { $A.periodMs } else { 3000 })
      $start = Get-Date; $sw = [Diagnostics.Stopwatch]::StartNew(); $px = $null
      while ($sw.Elapsed.TotalSeconds -lt [double]$A.seconds) {
        $ang = 2 * [Math]::PI * $sw.Elapsed.TotalMilliseconds / $per
        switch ([string]$A.zone) {
          'inside'    { $x = $cx + 0.4 * $hw * [Math]::Cos($ang); $y = $cy + 0.4 * $hh * [Math]::Sin($ang) }                    # a circle well inside the box
          'periphery' { $ox = if ($cx - 2.3 * $hw -gt $c.X + 0.08 * $c.W) { $cx - 2.3 * $hw } else { $cx + 2.3 * $hw }               # a circle beside the box, on the wider side
                        $x = $ox + 0.4 * $hw * [Math]::Cos($ang); $y = $cy + 0.4 * $hh * [Math]::Sin($ang) }
          'border'    { $x = $cx + $hw + 0.6 * $hw * [Math]::Sin($ang); $y = $cy + 0.3 * $hh * [Math]::Sin($ang / 3) }           # to and fro across the box's right edge
          default     { throw "mouse-zone: zone '$($A.zone)' (inside, periphery, border)" }
        }
        $x = [int]$x; $y = [int]$y; [Rig.W]::SetCursorPos($x, $y) | Out-Null
        if ($null -ne $px) { [Rig.W]::mouse_event(1, ($x - $px), ($y - $py), 0, [UIntPtr]::Zero); [Rig.W]::SetCursorPos($x, $y) | Out-Null }
        $px = $x; $py = $y; Start-Sleep -Milliseconds 8
      }
      $Rec.stretches += [ordered]@{ zone = [string]$A.zone; start = $start.ToString('o'); end = (Get-Date).ToString('o'); box = "centre $([Math]::Round($fb.cx, 3)),$([Math]::Round($fb.cy, 3)) half $([Math]::Round($fb.hw, 3)),$([Math]::Round($fb.hh, 3)) of DCS's window ($($fb.from))" }
    }
    'focus' { Focus-Dcs | Out-Null }
    'key' { Focus-Dcs | Out-Null; Key-Tap ([byte]$A.vk) ([byte[]]@($A.mods | Where-Object { $_ })) }
    'shot' {
      $png = Join-Path $RunDir "$($A.name).png"; Shot-Dcs $png | Out-Null
      if ($A.require -eq 'hud') {      # the bright-symbology test on DCS's own window: no HUD in view = no point in flying on
        $o = & py (Join-Path $Rig 'hudfind.py') --png $png --out (Join-Path $RunDir "$($A.name)-hud.png") 2>&1 | Out-String; $code = $LASTEXITCODE
        Note "$($Rec.run) HUD in view ($($A.name).png): exit $code $($o.Trim())"; $Rec.hudInView = ($code -eq 0)
        if ($code -ne 0) { throw "HUD not in view (picture: $png). Tracking: $(Slam)" }
      }
    }
    'mouse-move' { Focus-Dcs | Out-Null; $c = Get-DcsClient; [Rig.W]::SetCursorPos($c.X + [int]($A.x * $c.W), $c.Y + [int]($A.y * $c.Ht)) | Out-Null; [Rig.W]::mouse_event(1, 1, 0, 0, [UIntPtr]::Zero) }
    'mouse-circle' {   # a circle inside DCS's window (fractions of the client area), so the cursor stays inside the focus image
      Focus-Dcs | Out-Null; $c = Get-DcsClient; $cx = $c.X + $c.W * $(if ($null -ne $A.cx) { $A.cx } else { 0.5 }); $cy = $c.Y + $c.Ht * $(if ($null -ne $A.cy) { $A.cy } else { 0.5 }); $rad = [double]$(if ($A.radiusPx) { $A.radiusPx } else { 100 }); $per = [double]$(if ($A.periodMs) { $A.periodMs } else { 4000 })
      $sw = [Diagnostics.Stopwatch]::StartNew(); $px = $null
      while ($sw.Elapsed.TotalSeconds -lt [double]$A.seconds) {
        $ang = 2 * [Math]::PI * $sw.Elapsed.TotalMilliseconds / $per; $x = [int]($cx + $rad * [Math]::Cos($ang)); $y = [int]($cy + $rad * [Math]::Sin($ang))
        [Rig.W]::SetCursorPos($x, $y) | Out-Null
        if ($null -ne $px) { [Rig.W]::mouse_event(1, ($x - $px), ($y - $py), 0, [UIntPtr]::Zero); [Rig.W]::SetCursorPos($x, $y) | Out-Null }   # relative motion too: DCS reads raw input
        $px = $x; $py = $y; Start-Sleep -Milliseconds 8
      }
    }
    'gaze-alternate' {   # debug_simulate_tracking: gaze = normalize(cx/1000 - 0.5, 0.5 - cy/1000, -0.35) of the OS cursor (desktop pixels)
      $t = [Math]::Tan([double]$A.deg * [Math]::PI / 180); $xa = [int](500 - 350 * $t); $xb = [int](500 + 350 * $t); $y = [int](500 - 350 * [Math]::Tan([double]$(if ($A.pitchDeg) { $A.pitchDeg } else { 0 }) * [Math]::PI / 180))
      $steps = [int]$(if ($A.steps) { $A.steps } else { 3 }); $per = [int]$(if ($A.periodMs) { $A.periodMs } else { 700 }); $sw = [Diagnostics.Stopwatch]::StartNew(); $side = $false; $at = $xa
      [Rig.W]::SetCursorPos($xa, $y) | Out-Null
      while ($sw.Elapsed.TotalSeconds -lt [double]$A.seconds) {
        Start-Sleep -Milliseconds $per; $to = if ($side) { $xa } else { $xb }; $side = -not $side
        for ($i = 1; $i -le $steps; $i++) { [Rig.W]::SetCursorPos([int]($at + ($to - $at) * $i / $steps), $y) | Out-Null; if ($i -lt $steps) { Start-Sleep -Milliseconds 10 } }
        $at = $to
      }
      [Rig.W]::SetCursorPos(500, 500) | Out-Null
    }
    default { Note "$($Rec.run) unknown action '$($A.do)' skipped" }
  }
}
function Stop-RigFlight([string]$RunDir, $Rec, [string]$CopyDir) {
  $t0 = [datetime]$Rec.started
  $p = Get-Process DCS -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($p) { $p.CloseMainWindow() | Out-Null; if (-not $p.WaitForExit(90000)) { Note "$($Rec.run) DCS did not close in 90 s: killed"; $p | Stop-Process -Force -Confirm:$false } else { Note "$($Rec.run) DCS closed normally" } } else { Note "$($Rec.run) DCS was not running at stop" }
  Start-Sleep 8
  Copy-Item -LiteralPath $QvLog -Destination (Join-Path $RunDir 'DcsQvCull.log') -Force -ErrorAction SilentlyContinue
  Copy-Item -LiteralPath $DcsLog -Destination (Join-Path $RunDir 'dcs.log') -Force -ErrorAction SilentlyContinue
  foreach ($q in @(Get-ChildItem $env:LOCALAPPDATA -Filter '*quad*views*' -Directory -ErrorAction SilentlyContinue)) { Get-ChildItem $q.FullName -Filter *.log | ForEach-Object { Copy-Item $_.FullName (Join-Path $RunDir ('qv_' + $_.Name)) -Force } }
  Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Pimax\runtime\pvr_srv_log_*.txt') -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1 | ForEach-Object { Copy-Item $_.FullName (Join-Path $RunDir 'pvr_srv_log.txt') -Force }
  $ofxr = Join-Path $CopyDir 'ofxr'
  foreach ($x in @(Get-ChildItem -LiteralPath $ofxr -Filter 'ofxr-bridge-flight-*.log' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $t0 })) { Move-Item -LiteralPath $x.FullName -Destination (Join-Path $RunDir $x.Name) -Force; Note "$($Rec.run) flight log $($x.Name) $($x.Length) bytes moved to the run folder" }
  $extra = @(Get-ChildItem -LiteralPath $ofxr -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(cursor-capture-|hud-).*\.(bmp|txt)$' })
  if ($extra.Count) { $d = Join-Path $RunDir 'ofxr-captures'; New-Item -ItemType Directory -Force $d | Out-Null; $extra | Move-Item -Destination $d -Force; Note "$($Rec.run) $($extra.Count) OFXR capture files moved to ofxr-captures" }
}
function Flight([string]$RunName, $ScBase, $ArmDef, [bool]$Option) {
  # the arm's own ofxr / diagnostics / quadviews keys go on top of the scenario's
  $Sc = $ScBase | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  foreach ($sec in 'ofxr', 'diagnostics', 'quadviews', 'tapEnv') { if ($ArmDef.$sec) { if (-not $Sc.$sec) { $Sc | Add-Member -NotePropertyName $sec -NotePropertyValue ([pscustomobject]@{}) -Force }; foreach ($p in $ArmDef.$sec.PSObject.Properties) { $Sc.$sec | Add-Member -NotePropertyName $p.Name -NotePropertyValue $p.Value -Force } } }
  if ($ArmDef.schedule) { $Sc.schedule = $ArmDef.schedule }; if ($null -ne $ArmDef.seconds) { $Sc.seconds = $ArmDef.seconds }
  # an arm's "tracked" block (schedule, quadviews, ofxr) replaces / tops the above when the headset is tracked at this moment
  # (slam warning flags 0: pose pitched on the stand, the cockpit lower in the view) (B2 round, 2026-11-16)
  $trackedNow = -not ((Slam) -match 'slam warning flags: 1')
  if ($trackedNow -and $ArmDef.tracked) {
    if ($ArmDef.tracked.schedule) { $Sc.schedule = $ArmDef.tracked.schedule }
    foreach ($sec in 'ofxr', 'quadviews') { if ($ArmDef.tracked.$sec) { if (-not $Sc.$sec) { $Sc | Add-Member -NotePropertyName $sec -NotePropertyValue ([pscustomobject]@{}) -Force }; foreach ($p in $ArmDef.tracked.$sec.PSObject.Properties) { $Sc.$sec | Add-Member -NotePropertyName $p.Name -NotePropertyValue $p.Value -Force } } }
  }
  Note "$RunName pose before the launch: $(if ($trackedNow) { 'tracked' } else { 'untracked' })$(if ($ArmDef.tracked) { ' (the arm has a tracked variant)' })"
  $run = Join-Path $Runs $RunName; New-Item -ItemType Directory -Force $run | Out-Null
  $rec = [ordered]@{ run = $RunName; scenario = $Sc.name; arm = $ArmDef.name; option = $Option; composedWidth = 5424; hudInView = $null; poseAtBriefing = $null; flyButtonSeen = $null; ofxrInstalledSha256 = $null; ofxrFlownSha256 = $null; ofxrFlownFrom = $null; stretches = @(); pid = $null; started = $null; layers = $null; briefing = $null; fly = $null; flyConfirmed = $false; window = $null; captures = @(); applied = $null }
  $save = { Write-Utf8 (Join-Path $run 'run.json') ($rec | ConvertTo-Json -Depth 6) }
  try {
    # profile = the user's backup draft + scenario overrides + arm overrides + the option
    $prof = (Get-Content -LiteralPath (Join-Path $Backup 'draft.json') -Raw | ConvertFrom-Json).profile
    foreach ($set in @($Sc.profileOverrides, $ArmDef.profileOverrides)) { if ($set) { foreach ($p in $set.PSObject.Properties) { $prof | Add-Member -NotePropertyName $p.Name -NotePropertyValue $p.Value -Force } } }
    $prof | Add-Member -NotePropertyName frameGenFoveated -NotePropertyValue $Option -Force
    $pf = Join-Path $run 'profile.json'; Write-Utf8 $pf ($prof | ConvertTo-Json -Depth 8)
    $t0 = Get-Date; while ((Get-Process DcsVr.Cli -ErrorAction SilentlyContinue) -and ((Get-Date) - $t0).TotalSeconds -lt 90) { Start-Sleep 2 }   # the boost helper of a flight before ends by itself
    $a = Apply-Profile $pf "$($Sc.name) / $RunName"; $rec.applied = "$($a.Id) by $($a.App)"
    $copy = New-RigCopy $run $Sc $(if ($Option) { $OfxrLayerOn } else { $OfxrLayerTwin }) $rec
    $two = ((Get-Content (Join-Path $copy 'quadviews\settings.cfg')) | Select-String '^dcsvr_two_layers=(\d)' | Select-Object -Last 1)
    if ($Option -and (-not $two -or $two.Matches[0].Groups[1].Value -eq '0')) { throw 'the option is on in the profile but the applied settings.cfg has no dcsvr_two_layers: the installed app did not write the mode (wrong version?)' }
    if (-not [IO.Path]::IsPathRooted([string]$Sc.mission) -and -not $MissionDir) { throw "mission '$($Sc.mission)': no mission folder (-MissionDir or DCSC_RIG_MISSIONS)" }
    $mission = if ([IO.Path]::IsPathRooted([string]$Sc.mission)) { [string]$Sc.mission } else { Join-Path $MissionDir ([string]$Sc.mission) }
    if (-not (Test-Path -LiteralPath $mission)) { throw "mission not found: $mission" }
    Start-RigFlight $run (Join-Path $copy 'launch.json') $mission $rec; & $save
    $flyAt = Click-Fly $run $rec ([bool]$Sc.windowAtOrigin); & $save
    Note "$RunName flying since $($flyAt.ToString('HH:mm:ss')); tracking $(Slam)"
    # the composed image's width of this flight (the twin's capture plan needs it): the widest swapchain the capture layer logged
    $ws = @([regex]::Matches((Read-Shared (Join-Path $run 'tap\rigtap.log')), 'swapchain #\d+ \S+ (\d+)x(\d+) ') | ForEach-Object { [int]$_.Groups[1].Value }); if ($ws.Count) { $rec.composedWidth = ($ws | Measure-Object -Maximum).Maximum }
    if ($Sc.requireCockpit) { foreach ($act in @($Sc.schedule)) { if ($act.do -eq 'shot' -and $act.name -eq 'cockpit') { $act | Add-Member -NotePropertyName require -NotePropertyValue 'hud' -Force } } }   # no cockpit (HUD) in the picture after the recenter = the flight ends there
    foreach ($act in @($Sc.schedule | Sort-Object { [double]$_.at })) {
      $wait = [double]$act.at - ((Get-Date) - $flyAt).TotalSeconds; if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
      if (-not (Get-Process DCS -ErrorAction SilentlyContinue)) { throw 'DCS is gone during the schedule' }
      Note "$RunName FLY+$([Math]::Round(((Get-Date) - $flyAt).TotalSeconds, 1)) s: $($act.do) $(($act | ConvertTo-Json -Compress))"
      Do-Action $run $rec $act
    }
    $left = [double]$Sc.seconds - ((Get-Date) - $flyAt).TotalSeconds; if ($left -gt 0) { Start-Sleep ([int]$left) }
    Note ("$RunName perf lines: " + ((((Read-Shared $QvLog) -split "`n" | Select-String ' perf: fps' | Select-Object -Last 8 | ForEach-Object { ($_.Line -replace ' ft .*verdict=', ' ' -replace ' \(.*', '') }) -join ' | ')))
    Stop-RigFlight $run $rec $copy
    # after DCS has closed: every capture must be complete, then the verdicts
    $flight = Get-ChildItem -LiteralPath $run -Filter 'ofxr-bridge-flight-*.log' | Sort-Object Length -Descending | Select-Object -First 1
    foreach ($cap in @(Get-ChildItem -LiteralPath (Join-Path $run 'tap') -Directory -Filter 'cap*' -ErrorAction SilentlyContinue)) {
      $done = Test-Path -LiteralPath (Join-Path $cap.FullName 'done.txt'); $mb = [Math]::Round(((Get-ChildItem -LiteralPath $cap.FullName -File | Measure-Object Length -Sum).Sum) / 1MB)
      $rec.captures += [ordered]@{ name = $cap.Name; complete = $done; megabytes = $mb }
      Note "$RunName capture $($cap.Name): $(if ($done) { (Get-Content (Join-Path $cap.FullName 'done.txt') -TotalCount 1) } else { 'NOT complete' }), $mb MB"
      $args2 = @((Join-Path $Rig 'verdict.py'), $cap.FullName, '--quiet'); if ($flight) { $args2 += @('--flight-log', $flight.FullName) }
      foreach ($c in @($Sc.verdict.crops | Where-Object { $_ })) { $args2 += @('--crop', [string]$c) }
      if (-not $Option) { $ref = Get-ChildItem -LiteralPath $Runs -Directory -Filter "$($Sc.name)_*_on" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | ForEach-Object { Get-ChildItem (Join-Path $_.FullName 'tap') -Directory -Filter $cap.Name -ErrorAction SilentlyContinue } | Select-Object -First 1; if ($ref) { $args2 += @('--like', $ref.FullName) } }
      $o = & py @args2 2>&1 | Out-String; if ($LASTEXITCODE -ne 0) { Note "$RunName verdict.py on $($cap.Name) failed: $($o.Trim())" } elseif (Test-Path (Join-Path $cap.FullName 'verdict.txt')) { Get-Content (Join-Path $cap.FullName 'verdict.txt') | Write-Host }
    }
    if ($Sc.verdict.cursorLog -and $flight -and -not (Test-Path -LiteralPath (Join-Path $Rig 'cursor_verdict.py'))) { Note "$RunName cursor counters of the flight log not judged: cursor_verdict.py is not in tools\rig yet (README.md)" }
    elseif ($Sc.verdict.cursorLog -and $flight) {      # the smooth-cursor records of the flight log, per second and per mouse stretch
      $cv = @((Join-Path $Rig 'cursor_verdict.py'), $run); if (-not $Option) { $on = Get-ChildItem -LiteralPath $Runs -Directory -Filter "$($Sc.name)_$($ArmDef.name)_*_on" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1; if ($on) { $cv += @('--on', $on.FullName) } }
      & $save; $o = & py @cv 2>&1 | Out-String; Write-Host $o
    }
    Note "$RunName rigtap.log tail: $(((Read-Shared (Join-Path $run 'tap\rigtap.log')) -split "`n" | Where-Object { $_ } | Select-Object -Last 3) -join ' | ')"
  } catch { Note "$RunName FAILED: $_"; $rec['error'] = "$_"; $Script:Failed.Add("${RunName}: $_") }
  finally {
    $p = Get-Process DCS -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) { $p.CloseMainWindow() | Out-Null; if (-not $p.WaitForExit(90000)) { $p | Stop-Process -Force -Confirm:$false }; Start-Sleep 8; Note "$RunName DCS closed in the cleanup" }
    [Rig.W]::ClipCursor([IntPtr]::Zero) | Out-Null
    & $save
  }
}

# ---- main --------------------------------------------------------------------------------------------------------
$scPath = if (Test-Path -LiteralPath $Scenario -PathType Leaf) { $Scenario } else { Join-Path $Rig "scenarios\$Scenario.json" }
if (-not (Test-Path -LiteralPath $scPath)) { throw "scenario not found: $scPath (have: $((Get-ChildItem (Join-Path $Rig 'scenarios') -Filter *.json).BaseName -join ', '))" }
$sc = Get-Content -LiteralPath $scPath -Raw | ConvertFrom-Json
$arms = @($sc.arms | Where-Object { $_ }); if (-not $arms.Count) { $arms = @([pscustomobject]@{ name = 'default'; profileOverrides = $null }) }
$armDef = if ($Arm) { $arms | Where-Object { $_.name -eq $Arm } | Select-Object -First 1 } else { $arms[0] }
if (-not $armDef) { throw "arm '$Arm' not in the scenario (have: $($arms.name -join ', '))" }
$options = if ($Both) { @($true, $false) } elseif ($Twin) { @($false) } else { @($true) }
$stamp = Get-Date -Format 'MMdd-HHmm'
$caps = @($sc.schedule | Where-Object { $_.do -eq 'capture' })
Write-Host "scenario $($sc.name): $($sc.measures)"
Write-Host "status: $($sc.status)$(if ($sc.blockedWhy) { ' - ' + $sc.blockedWhy })"
Write-Host "mission $($sc.mission), $($sc.seconds) s after FLY, arm $(if ($FlyArms) { $FlyArms -join ', ' } else { $armDef.name }), flights: $(($options | ForEach-Object { if ($_) { 'option ON' } else { 'twin OFF' } }) -join ' then ')"
foreach ($x in @($sc.schedule | Sort-Object { [double]$_.at })) { Write-Host ("  FLY+{0,5} s  {1}" -f $x.at, ($x | ConvertTo-Json -Compress)) }
foreach ($x in @($sc.sideEffects | Where-Object { $_ })) { Write-Host "  side effect: $x" }
# checks that need nothing running
$problems = @()
if (-not (Test-Path -LiteralPath $Cli)) { $problems += "installed CLI missing: $Cli" }
if (-not (Test-Path -LiteralPath $TapDll) -or -not (Test-Path -LiteralPath (Join-Path $TapDir "$TapName.json"))) { $problems += "capture layer not found in $TapDir (-TapDir or DCSC_RIG_TAP_DIR; its sources are not in tools\rig yet, see README.md)" }
elseif ([string]((Get-Content (Join-Path $TapDir "$TapName.json") -Raw | ConvertFrom-Json).api_layer.library_path) -ne $TapDll) { $problems += 'capture layer manifest points elsewhere: rebuild' }
if (-not $Backup) { $problems += 'no user-state backup folder given (-StateBackup or DCSC_RIG_STATE_BACKUP)' }
else { foreach ($n in $StateFiles) { if (-not (Test-Path -LiteralPath (Join-Path $Backup $n))) { $problems += "user-state backup file missing: $n" } } }
if (Get-Process DCS -ErrorAction SilentlyContinue) { $problems += 'DCS is running' }
$installedRoot = Join-Path $env:LOCALAPPDATA 'Programs\DcsControl'
$dcOpen = @(Get-Process DcsControl -ErrorAction SilentlyContinue); $dcOurs = @($dcOpen | Where-Object { -not $_.Path -or $_.Path.StartsWith($installedRoot, [StringComparison]::OrdinalIgnoreCase) })
if ($dcOurs.Count) { $problems += 'DCS Control is open (close it: it holds the profile state)' } elseif ($dcOpen.Count) { Write-Host "note: a DCS Control from another folder is open (not the installed one, it does not hold the installed profile state): $(($dcOpen | ForEach-Object Path | Select-Object -Unique) -join '; ')" }
if ($sc.status -ne 'ready' -and -not $Force) { $problems += "scenario is '$($sc.status)': $($sc.blockedWhy) (-Force to run anyway)" }
$perSub = 39232128; $need = 0; foreach ($c in $caps) { $s = [double]$(if ($c.scale) { $c.scale } else { 1 }); $n = [Math]::Min(120, [int]$(if ($c.frames) { $c.frames } else { 40 })); $need += $n * $perSub / ($s * $s) }
$need *= $options.Count; $free = (Get-PSDrive -Name ($Runs.Substring(0, 1))).Free
Write-Host ("disk: about {0:N1} GB of captures at the flown sizes (39.2 MB per submission at scale 1, before any crop; up to 65.4 MB with the user's 3480x2048 focus), {1:N0} GB free" -f ($need / 1GB), ($free / 1GB))
if ($free -lt 2 * $need + 4GB) { $problems += 'not enough free disk for the captures (twice the estimate + 4 GB wanted)' }
if ($problems.Count) { $problems | ForEach-Object { Write-Host "PROBLEM: $_" }; if (-not $DryRun) { throw 'not started' } }
if ($DryRun) { Write-Host 'dry run: nothing applied, nothing started'; return }

$snapshot = Join-Path $Runs "state-before-$stamp"; $lockMine = $false
# The machine's state as it is before this run touches it: both are put back as found, not to fixed values.
$pimaxWasOpen = [bool](Get-Process PimaxClient -ErrorAction SilentlyContinue)
$screenOffBefore = $null      # read under -Prep, just before the key is set to 0
try {
  # quiet lock (artifacts\.lane\README.md): wait while another holds it, then take it
  $t0 = Get-Date; while (Test-Path -LiteralPath $Lock) { if (((Get-Date) - $t0).TotalMinutes -gt 60) { throw "quiet.lock still held after 60 min: $(Get-Content -LiteralPath $Lock -TotalCount 1)" }; Write-Host "quiet.lock held ($(Get-Content -LiteralPath $Lock -TotalCount 1)): waiting"; Start-Sleep 20 }
  while (Get-Process cl, link, MSBuild, ninja, ctest -ErrorAction SilentlyContinue) { Write-Host 'a build or test is running: waiting'; Start-Sleep 20 }
  Set-Content -LiteralPath $Lock -Value "rig scenario $($sc.name) (live flight, run-scenario.ps1), started $(Get-Date -Format HH:mm), expected $([int](($options.Count * $(if ($FlyArms) { $FlyArms.Count } else { 1 }) * ([double]$sc.seconds + 150) + 240) / 60) + 1) minutes"; $lockMine = $true
  Note "==== scenario $($sc.name), arm $($armDef.name), flights $($options.Count); quiet lock held"
  foreach ($n in $StateFiles) { $to = Join-Path $snapshot $n; [IO.Directory]::CreateDirectory((Split-Path $to -Parent)) | Out-Null; Copy-Item -LiteralPath (Join-Path $State $n) -Destination $to -Force }
  foreach ($n in $StateFiles) { if ((Sha (Join-Path $State $n)) -ne (Sha (Join-Path $Backup $n))) { Note "note: $n already differs from user-state-backup before this run (the profile used is the BACKUP draft)" } }
  $st = (& $Cli status 2>&1 | Out-String | ConvertFrom-Json); if ($st.state -ne 'clean') { Note "note: the app's state is '$($st.state)' before the run (profile $($st.profileId)); the run ends with stock restored" }
  if ($Prep) {
    if (-not (Get-Process PimaxClient -ErrorAction SilentlyContinue)) {
      if (-not (Test-Path -LiteralPath $PimaxClient)) { throw "PimaxClient not found: $PimaxClient (-PimaxClient or DCSC_PIMAX_CLIENT)" }
      Start-Process $PimaxClient; Note 'PimaxClient started'; Start-Sleep 30
    }
    # The value to put back at the end. A value that cannot be read is not changed: there would be nothing to restore.
    $s = (& $Ctl get runtime_screen_off_timeout_second 2>&1 | Select-Object -Last 1 | Out-String).Trim(); Note "screen-off key before: $s"
    if ($s -notmatch '^\d+$') { throw "the screen-off key could not be read ('$s'): not changing it" }
    $screenOffBefore = [int]$s
    $s = & $Ctl set runtime_screen_off_timeout_second 0 2>&1 | Out-String; Note "screen-off key set 0: $($s.Trim())"
    $r = if ($PimaxReboot) { & $PimaxReboot 2>&1 | Out-String } else { & $Ctl reboot 2>&1 | Out-String }; Note ("headset restart: " + ($r.Trim() -replace '\s+', ' '))
    Start-Sleep 90; Note (Pimax-Ready 40 300)
  }
  $armList = @($armDef); if ($FlyArms) { $armList = @(foreach ($n in $FlyArms) { $d = $arms | Where-Object { $_.name -eq $n } | Select-Object -First 1; if (-not $d) { throw "arm '$n' not in the scenario (have: $($arms.name -join ', '))" }; $d }) }
  # an arm may name its own flights ("options": [true, false] = option ON then its twin; [true] = ON only) (B2 round, 2026-11-16)
  foreach ($ad in $armList) { foreach ($opt in $(if ($null -ne $ad.options) { @($ad.options | ForEach-Object { [bool]$_ }) } else { $options })) { Flight ("{0}_{1}_{2}_{3}" -f $sc.name, $ad.name, $stamp, $(if ($opt) { 'on' } else { 'off' })) $sc $ad $opt } }
} catch { Note "run FAILED: $_"; $Script:Failed.Add("run: $_") }
finally {
  $p = Get-Process DCS -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($p) { $p.CloseMainWindow() | Out-Null; if (-not $p.WaitForExit(90000)) { $p | Stop-Process -Force -Confirm:$false }; Start-Sleep 8; Note 'DCS closed in the final cleanup' }
  [Rig.W]::ClipCursor([IntPtr]::Zero) | Out-Null
  $t0 = Get-Date; while ((Get-Process DcsVr.Cli -ErrorAction SilentlyContinue) -and ((Get-Date) - $t0).TotalSeconds -lt 90) { Start-Sleep 2 }
  try { Restore-Stock } catch { Note "restore FAILED: $_ (open DCS Control and use Restore)"; $Script:Failed.Add("restore: $_") }
  # The screen-off key goes back to what it was before -Prep set it to 0 (not to a fixed value); a run that did not
  # change it leaves it alone.
  if ($null -ne $screenOffBefore) {
    $byHand = "set runtime_screen_off_timeout_second to $screenOffBefore with pimax_ctl"
    if (Get-Process pi_server -ErrorAction SilentlyContinue) {
      try { $s = & $Ctl set runtime_screen_off_timeout_second $screenOffBefore 2>&1 | Out-String; Note "screen-off key restored to ${screenOffBefore}: $($s.Trim() -replace '\s+', ' ')" }
      catch { Note "screen-off key NOT restored: $_ ($byHand)"; $Script:Failed.Add("screen-off key not restored: $_") }
    } else { Note "screen-off key NOT restored: the Pimax service is not running ($byHand)"; $Script:Failed.Add('screen-off key not restored: the Pimax service is not running') }
  }
  # PimaxClient is closed only when this run found it closed: a client the user had open stays open.
  if ($KeepPimax -or $pimaxWasOpen) { Note "PimaxClient left as it is ($(if ($pimaxWasOpen) { 'it was open before the run' } else { '-KeepPimax' }))" }
  else { for ($i = 0; $i -lt 3; $i++) { Get-Process PimaxClient -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false; Start-Sleep 3 }; Note "PimaxClient closed (left: $(@(Get-Process PimaxClient -ErrorAction SilentlyContinue).Count))" }
  if (Test-Path -LiteralPath $snapshot) { State-Check $snapshot }
  Note "installed: $((Get-Content (Join-Path $env:LOCALAPPDATA 'Programs\DcsControl\installation.json') -Raw | ConvertFrom-Json).version); baseline: $((Applied).State); processes: $((Get-Process DCS, PimaxClient, DcsVr.Cli -ErrorAction SilentlyContinue | ForEach-Object Name) -join ' ')"
  if ($lockMine -and (Test-Path -LiteralPath $Lock)) { Remove-Item -LiteralPath $Lock -Force -Confirm:$false; Note 'quiet lock removed' }
}
# A failed flight or run is not swallowed: everything above was still cleaned up, and the caller gets the result.
if ($Script:Failed.Count) { Write-Host "RUN FAILED ($($Script:Failed.Count)):"; $Script:Failed | ForEach-Object { Write-Host "  $_" }; exit 1 }
exit 0
