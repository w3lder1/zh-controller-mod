<#
.SYNOPSIS
  Multiplayer sync test: two copies of the controller mod play a LAN match on this PC, both driven by
  scripted controller input, and the test checks that they stayed in sync.

.DESCRIPTION
  Needs the multiplayer test build (build-mptest.cmd): debug logging, several copies per PC, scripted
  input. It is copied to the test game folder as generalszh_mp.exe.

  1. Backs up the player settings it touches (options.ini, Network.ini, ControllerMod.ini, the Last
     Replay) and gives each copy its own LAN address (127.0.0.1 host, 127.0.0.2 joiner).
  2. Copy A hosts (Alpine Assault), copy B joins by Direct Connect; both start the match with the
     controller, then run the same controller routine for -Minutes (see New-BattleSteps).
  3. Compares the logs: every sync checksum both copies wrote must match, neither may report a
     mismatch, and both must have run the same commands in the same order.
  4. Restores the settings and removes the second copy's files, also when something fails.

  The result (PASS/FAIL, checksum count, which kinds of commands were sent) is in summary.txt in
  -OutDir, together with the logs, the replay and screenshots. Exit code 0 = PASS.
#>
param(
    [int]$Minutes = 15,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$Game = '',
    [string]$Build = '',
    [int]$ShotEverySec = 60,
    [int]$MinCrcs = 20
)
$ErrorActionPreference = 'Stop'
if (-not $Game) { $Game = Join-Path $PSScriptRoot '..\..\..\..\TestGame\ZeroHour' }   # (a default here: $PSScriptRoot is empty in parameter defaults under -File)
if (-not $Build) { $Build = Join-Path $PSScriptRoot '..\..\..\build\mptest\GeneralsMD\Release\generalszh.exe' }   # (a default here: $PSScriptRoot is empty in parameter defaults under -File)

$userData = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Command and Conquer Generals Zero Hour Data'
$exe = Join-Path $Game 'generalszh_mp.exe'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$backup = Join-Path $OutDir 'settings-backup'

if (Get-Process -Name 'generalszh*', 'generals*' -ErrorAction SilentlyContinue) {
    throw 'Zero Hour is running. Close it first.'
}
if (-not (Test-Path -LiteralPath $Build)) { throw "No test build at $Build (run build-mptest.cmd)." }
Copy-Item -LiteralPath $Build -Destination $exe -Force

# --- Controller routine ----------------------------------------------------------------------------
# Menus by gadget name, then (after "battle;", timed from each copy's own first battle frame) a
# build-up and a 40 s cycle repeated for the whole match. The routine touches everything the
# controller can send: production and cancelling from a building's wheel, building placement,
# whole army and army on screen, moves, guard, line move, type cycling, control groups (assign,
# select), the selection's command wheel and its Orders page (force attack, waypoints), promotions
# and generals' powers, unload. What the controller really sent is counted from the logs; the
# routine does not need every step to land.
# Test build steps used here: LOOK:Name centres the camera on the player's oldest object of that
# type (the normal A press then selects it), AIM:Name points the open wheel's stick at the slice
# with that name (so no step depends on slice positions), SNAP:label records what the controller
# sees in controllermod_snaps*.txt. Names are USA's (both copies play USA); Sell is never aimed at.
$lobbyA = 'shell;7000:@ButtonMultiplayer;9000:@ButtonNetwork;11500:@ButtonHost;13000:@ButtonSelectMap;14500:@RadioButtonSystemMaps;16000:@ListboxMap;17500:@ButtonOK;30000:@ButtonStart;battle;'
$lobbyB = 'shell;7000:@ButtonMultiplayer;9000:@ButtonNetwork;18000:@ButtonDirectConnect;20000:@ButtonJoin;25000:@ButtonStart;battle;'
$buildUpMs = 72000
$cycleMs = 40000

function New-BattleSteps([int]$cycles, [string]$pan1, [string]$pan2) {
    $dirs = 'N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'
    $s = New-Object System.Collections.Generic.List[string]
    $c = 0
    $add = { param($from, $to, $what) if ($to) { $s.Add("$($c + $from)-$($c + $to):$what") } else { $s.Add("$($c + $from):$what") } }

    # Build-up: a second worker, a Barracks placed a short camera move away from the worker (and
    # again a little further if that spot is blocked), five infantry once it stands (on a Barracks
    # still being built the only slice is "Cancel Build"), and a Power Plant once the worker is free.
    & $add 1000 $null 'LEFT'; & $add 2000 $null 'A'; & $add 3000 3500 'AIM:Dozer|Worker'; & $add 3600 $null 'A'; & $add 4200 $null 'B'
    & $add 5000 $null 'LOOK:Dozer|Worker'; & $add 5500 $null 'A'; & $add 6500 $null 'Y'; & $add 7500 8000 'AIM:Barracks'; & $add 8100 $null 'A'
    # tries: a short camera nudge, then longer ones in other directions (once placed, a further A
    # only selects what is under the reticle)
    & $add 9000 9300 "LS$pan2"; & $add 9600 $null 'A'; & $add 10100 10500 'LSS'; & $add 10800 $null 'A'; & $add 11300 11800 'LSW'
    & $add 12100 $null 'A'; & $add 12600 13300 'LSN'; & $add 13600 $null 'A'; & $add 14100 15000 'LSE'; & $add 15300 $null 'A'
    & $add 15800 $null 'SNAP:barracks-placed'; & $add 16300 $null 'B'; & $add 16800 $null 'B'
    & $add 46000 $null 'LOOK:Dozer|Worker'; & $add 46500 $null 'A'; & $add 47500 $null 'Y'; & $add 48500 49000 'AIM:Reactor|Power Plant'; & $add 49100 $null 'A'
    & $add 50000 50300 "LS$pan1"; & $add 50600 $null 'A'; & $add 51100 51500 'LSN'; & $add 51800 $null 'A'; & $add 52300 52800 'LSE'
    & $add 53100 $null 'A'; & $add 53600 54300 'LSS'; & $add 54600 $null 'A'
    & $add 55100 $null 'SNAP:power-placed'; & $add 55600 $null 'B'; & $add 56100 $null 'B'
    & $add 62000 $null 'LOOK:Barracks'; & $add 62500 $null 'A'; & $add 63500 64000 'AIM:Ranger|Red Guard|Rebel'
    foreach ($t in 64100, 64400, 64700, 65000, 65300) { & $add $t $null 'A' }
    & $add 65800 $null 'SNAP:infantry-queued'; & $add 66300 $null 'B'; & $add 66800 $null 'B'

    for ($k = 0; $k -lt $cycles; $k++) {
        $c = $buildUpMs + $k * $cycleMs
        $d = $dirs[$k % 8]
        $unit = ('Ranger|Red Guard|Rebel', 'Missile Defender|Tank Hunter|RPG')[$k % 2]
        $cc = ('Dozer|Worker', 'Spy Satellite|Radar', 'Rally Point')[$k % 3]
        $group = "Group $(($k % 9) + 1)"
        # more units from the Barracks, then the Command Center's wheel (a worker, sometimes taken
        # out of the queue again; Spy Satellite and rally point, each confirmed at the reticle)
        & $add 0 $null 'LOOK:Barracks'; & $add 500 $null 'A'; & $add 1200 1700 "AIM:$unit"; & $add 1800 $null 'A'; & $add 2100 $null 'A'; & $add 2800 $null 'B'
        & $add 3500 $null 'LEFT'; & $add 4000 $null 'A'; & $add 4700 5200 "AIM:$cc"; & $add 5300 $null 'A'
        if ($k % 3 -eq 0) { if ($k % 2 -eq 1) { & $add 5700 $null 'X' } } else { & $add 5900 $null 'A' }
        & $add 6300 $null 'B'
        # whole army, move; control group: assign, deselect, select it again
        & $add 7000 $null 'LB'; & $add 7600 8600 "LS$pan1"; & $add 9000 $null 'X'
        & $add 10000 $null 'DOWN'; & $add 10500 11000 "AIM:$group"; & $add 11200 $null 'Y'; & $add 11800 $null 'B'
        & $add 12400 $null 'B'; & $add 13000 $null 'DOWN'; & $add 13500 14000 "AIM:$group"; & $add 14200 $null 'A'; & $add 14800 $null 'B'
        # army on screen: move, guard (double tap: the second X within the 260 ms window), line move,
        # type cycle
        & $add 15500 $null 'LEFT'; & $add 16200 $null 'RB'; & $add 17000 18500 "LS$pan2"; & $add 19000 19100 'X'; & $add 19220 19320 'X'
        & $add 21000 23000 'X'; & $add 21400 22800 'LSS'
        for ($r = 0; $r -le ($k % 3); $r++) { & $add (24000 + $r * 300) $null 'RT' }
        # the selection's command wheel: an order, or every other cycle the Orders page (force
        # attack, waypoints), confirmed at the reticle
        & $add 25500 $null 'Y'
        if ($k % 2 -eq 1) {
            & $add 26000 $null 'UP'; & $add 26500 26900 ('AIM:' + ('Force attack', 'Waypoints')[($k -shr 1) % 2])
        } else {
            & $add 26500 26900 ('AIM:' + ('Guard', 'Attack Move', 'Stop')[($k -shr 1) % 3])
        }
        & $add 27100 $null 'A'; & $add 27900 28400 "LS$d"; & $add 28600 $null 'A'
        & $add 29200 $null 'B'; & $add 29600 $null 'B'
        # D-pad up opens Promotions while a point is unspent (buy one), otherwise the generals'
        # powers (use the Spy Satellite at the reticle)
        & $add 30300 $null 'UP'; & $add 30900 31400 'AIM:Paladin|Stealth|Spy Drone|Spy Satellite'; & $add 31500 $null 'A'; & $add 32300 $null 'A'
        & $add 33000 $null 'B'; & $add 33400 $null 'B'
        # unload whatever is selected
        & $add 37800 $null 'LB+RB'; & $add 38700 $null 'B'
        if ($k % 5 -eq 0) { & $add 39300 $null "SNAP:cycle-$k" }
    }
    return ($s -join ';')
}

$battleMs = $Minutes * 60000
$cycles = [Math]::Max(1, [int][Math]::Floor(($battleMs - $buildUpMs) / $cycleMs))
$stepsA = $lobbyA + (New-BattleSteps $cycles 'E' 'SW')
$stepsB = $lobbyB + (New-BattleSteps $cycles 'W' 'NE')
Set-Content -LiteralPath (Join-Path $OutDir 'steps-A.txt') -Value $stepsA -Encoding ascii
Set-Content -LiteralPath (Join-Path $OutDir 'steps-B.txt') -Value $stepsB -Encoding ascii

# --- Settings: back up, then one LAN address per copy (MpTestSettings.ps1) ------------------------
. (Join-Path $PSScriptRoot 'MpTestSettings.ps1')
. (Join-Path $PSScriptRoot 'MpSyncCheck.ps1')
Backup-MpSettings -Backup $backup

$logA = Join-Path $Game 'DebugLogFile.txt'
$logB = Join-Path $Game 'DebugLogFile_Instance02.txt'
$result = 'FAIL'
try {
    # Both play USA: the routine's wheel names are USA's.
    Set-MpTwoCopySettings -Faction 2
    foreach ($l in $logA, $logB) { if (Test-Path -LiteralPath $l) { Remove-Item -LiteralPath $l -Force } }

    # --- Play ------------------------------------------------------------------------------------------
    $endMs = 65000 + $buildUpMs + $cycles * $cycleMs + 5000
    $shots = @()
    for ($t = 20000; $t -lt $endMs; $t += $ShotEverySec * 1000) { $shots += $t }
    $shots += $endMs
    Write-Host "Playing $cycles cycles ($([int]($endMs / 60000)) min in all)..."
    & (Join-Path $PSScriptRoot 'Run-TwoInstances.ps1') -Game $Game -StepsA $stepsA -StepsB $stepsB -ShotsMs $shots `
        -OutDir (Join-Path $OutDir 'shots') | Tee-Object -FilePath (Join-Path $OutDir 'run.txt')
    Start-Sleep -Seconds 2

    # --- Compare -------------------------------------------------------------------------------------
    foreach ($l in $logA, $logB, (Join-Path $Game 'controllermod_snaps.txt'), (Join-Path $Game 'controllermod_snaps_Instance02.txt')) {
        if (Test-Path -LiteralPath $l) { Copy-Item -LiteralPath $l -Destination $OutDir -Force }
    }
    $rep = Join-Path $userData 'Replays\00000000.rep'
    if (Test-Path -LiteralPath $rep) { Copy-Item -LiteralPath $rep -Destination (Join-Path $OutDir 'match-A.rep') -Force }

    $check = Test-MpSync (Read-MpLog $logA) (Read-MpLog $logB) ($buildUpMs + $cycles * $cycleMs) $MinCrcs
    $report = $check.Report
    $ok = $check.Ok
    $crashLines = @(Get-Content -LiteralPath (Join-Path $OutDir 'run.txt') | Where-Object { $_ -match 'exited\(' })
    if ($crashLines.Count) { $ok = $false; $report.Add("A copy closed early: $($crashLines[0])") }
    if ($ok) { $result = 'PASS' }
    $report.Insert(0, "MP sync test: $result ($cycles cycles, $(Get-Date -Format 'yyyy-MM-dd HH:mm'))")
    Set-Content -LiteralPath (Join-Path $OutDir 'summary.txt') -Value $report -Encoding ascii
    $report
}
finally {
    Stop-MpOwnedCopies -Dir $OutDir -Game $Game
    Start-Sleep -Seconds 1
    Restore-MpSettings -Backup $backup
    Clear-MpGameFiles -Game $Game
    Write-Host "Settings restored. Result: $result (details in $OutDir\summary.txt)"
}
if ($result -ne 'PASS') { exit 1 }
exit 0
