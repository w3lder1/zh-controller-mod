param([string]$StepsA = '', [string]$StepsB = '', [int[]]$ShotsMs, [int]$DelayB = 4000, [string]$OutDir,
      [string]$GameArgs = '-win -xres 960 -yres 540 -quickstart', [switch]$OnlyA, [int]$KeepSeconds = 0,
      [string]$ExeB = '', [hashtable]$EnvB = @{}, [hashtable]$EnvA = @{}, [int]$KillBMs = 0,
      [string]$Game = (Join-Path $PSScriptRoot '..\..\..\..\TestGame\ZeroHour'))
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class CapMp {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hgt, bool repaint);
  public struct RECT { public int L, T, R, B; }
}
"@
# -Game: the test game folder (a copy of Zero Hour used only for tests), by default TestGame\ZeroHour
# next to the source folder.
$exe = Join-Path $game 'generalszh_mp.exe'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$dumpDir = Join-Path $PSScriptRoot '..\..\..\..\CrashDumps'
$dumpsBefore = @(Get-ChildItem $dumpDir -ErrorAction SilentlyContinue).Count

# Copy B can run another exe (-ExeB, e.g. a different build); each copy can get extra environment
# variables (-EnvA, -EnvB, e.g. CONTROLLERMOD_TEST_MOD_VERSION).
function Start-Instance([string]$steps, [string]$file = $exe, [hashtable]$extra = @{}) {
    if ($steps) { $env:CONTROLLERMOD_TEST_INPUT = $steps } else { $env:CONTROLLERMOD_TEST_INPUT = $null }
    $env:CONTROLLERMOD_TEST_STATE = '1'
    foreach ($k in $extra.Keys) { Set-Item -Path "env:$k" -Value $extra[$k] }
    $p = Start-Process -FilePath $file -WorkingDirectory $game -ArgumentList $GameArgs -PassThru
    $env:CONTROLLERMOD_TEST_INPUT = $null
    $env:CONTROLLERMOD_TEST_STATE = $null
    foreach ($k in $extra.Keys) { Remove-Item -Path "env:$k" -ErrorAction SilentlyContinue }
    return $p
}
function Shot($p, [string]$file) {
    $p.Refresh()
    if ($p.HasExited) { return "exited($($p.ExitCode))" }
    $h = $p.MainWindowHandle
    $r = New-Object CapMp+RECT
    [void][CapMp]::GetWindowRect($h, [ref]$r)
    if ($r.R - $r.L -le 0) { return 'no window' }
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc(); [void][CapMp]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save($file); $bmp.Dispose()
    return 'ok'
}

$start = Get-Date
$a = Start-Instance $StepsA $exe $EnvA
$b = $null
if (-not $OnlyA) { Start-Sleep -Milliseconds $DelayB; $b = Start-Instance $StepsB $(if ($ExeB) { $ExeB } else { $exe }) $EnvB }
$placed = $false
$i = 0
# -KillBMs: copy B is ended at that time (a player whose game or connection died). It needs a shot
# at or after that time.
$killed = $false
foreach ($ms in $ShotsMs) {
    $wait = $start.AddMilliseconds($ms) - (Get-Date)
    if ($wait.TotalMilliseconds -gt 0) { Start-Sleep -Milliseconds ([int]$wait.TotalMilliseconds) }
    if ($KillBMs -gt 0 -and -not $killed -and $ms -ge $KillBMs -and $b) {
        $b.Refresh(); if (-not $b.HasExited) { Stop-Process -Id $b.Id -Force }
        $killed = $true
        "B ended at $ms ms"
    }
    if (-not $placed) {
        # Side by side, so both can be seen (placement does not affect the test).
        # (A copy that has closed has no window; the screenshots then say so.)
        $a.Refresh(); if (-not $a.HasExited -and $a.MainWindowHandle -ne 0) { [void][CapMp]::MoveWindow($a.MainWindowHandle, 0, 40, 980, 590, $true) }
        if ($b) { $b.Refresh(); if (-not $b.HasExited -and $b.MainWindowHandle -ne 0) { [void][CapMp]::MoveWindow($b.MainWindowHandle, 985, 40, 980, 590, $true); $placed = $true } } else { $placed = $true }
    }
    $ra = Shot $a (Join-Path $OutDir ("A{0:D2}_{1}ms.png" -f $i, $ms))
    $rb = if ($b) { Shot $b (Join-Path $OutDir ("B{0:D2}_{1}ms.png" -f $i, $ms)) } else { '-' }
    "shot $i at $ms ms: A=$ra B=$rb"
    $i++
}
if ($KeepSeconds -gt 0) { Start-Sleep -Seconds $KeepSeconds }
foreach ($p in @($a, $b)) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
"crash dumps before=$dumpsBefore after=$(@(Get-ChildItem $dumpDir -ErrorAction SilentlyContinue).Count)"
