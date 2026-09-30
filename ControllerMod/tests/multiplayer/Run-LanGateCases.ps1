<#
.SYNOPSIS
  Who may join whom on LAN (see Core\GameEngine\Include\GameNetwork\ControllerModLan.h): two game
  copies on this PC, A hosts, B looks at the LAN list and joins by Direct Connect.

.DESCRIPTION
  Cases (-Cases, default all):
    same       B runs the same program             -> B joins (B reaches the game setup screen)
    build      B runs a different build            -> B refuses: "different game files"; A greyed in B's list
    version    B is another Controller Mod version -> B refuses: "Controller Mod <A's>, you have <B's>"
    hostcheck  B acts like normal Zero Hour (no tags, no checks; CONTROLLERMOD_TEST_ACT_RETAIL)
                                                   -> A refuses the join; B gets the game's own mismatch message
    retailhost A acts like normal Zero Hour        -> B refuses from the LAN list: "not running the Controller Mod"
    retailaccept  A acts like normal Zero Hour, B only checks the host's acceptance (as when a big
               game's announcement has no room for the tag) -> A accepts, B leaves at once and says why
  "A different build" is the same exe with 16 bytes appended: it runs the same, but its program
  checksum differs, as between two real releases. Uses the multiplayer test build.
  Output: per case, both copies' snapshots (menu screen after each step) and screenshots, and a
  summary with the verdicts. Exit 0 = every case as expected.
#>
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string[]]$Cases = @('same', 'build', 'version', 'hostcheck', 'retailhost', 'retailaccept'),
    [string]$Game = '',
    [string]$Build = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Game) { $Game = Join-Path $PSScriptRoot '..\..\..\..\TestGame\ZeroHour' }   # (a default here: $PSScriptRoot is empty in parameter defaults under -File)
if (-not $Build) { $Build = Join-Path $PSScriptRoot '..\..\..\build\mptest\GeneralsMD\Release\generalszh.exe' }   # (a default here: $PSScriptRoot is empty in parameter defaults under -File)
. (Join-Path $PSScriptRoot 'MpTestSettings.ps1')
if (Get-Process -Name 'generalszh*', 'generals' -ErrorAction SilentlyContinue) { throw 'Zero Hour is running. Close it first.' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$exeA = Join-Path $Game 'generalszh_mp.exe'
$exeOther = Join-Path $Game 'generalszh_mp_other.exe'
Copy-Item -LiteralPath $Build -Destination $exeA -Force
Copy-Item -LiteralPath $Build -Destination $exeOther -Force
$fs = [System.IO.File]::Open($exeOther, 'Append')
$fs.Write([byte[]](1..16), 0, 16)
$fs.Close()

# B's clock starts 4 s after A's. A hosts at 11.5 s and announces its game every 10 s; B is in the
# LAN lobby from 9 s (A's clock 13 s), so A's game is in B's list by B's 17 s.
$stepsA = 'shell;7000:@ButtonMultiplayer;9000:@ButtonNetwork;11500:@ButtonHost;14000:SNAP:hosting;34000:SNAP:end'
$stepsB = 'shell;7000:@ButtonMultiplayer;9000:@ButtonNetwork;17000:SNAP:lan-list;18000:@ButtonDirectConnect;20000:@ButtonJoin;25000:SNAP:after-join'

$expect = @{
    same      = @{ Exe = $exeA; Env = @{}; Layer = 'LanGameOptionsMenu'; Note = 'joined: game setup screen' }
    build     = @{ Exe = $exeOther; Env = @{}; Layer = 'MessageBox'; Note = 'refused by B itself' }
    version   = @{ Exe = $exeOther; Env = @{ CONTROLLERMOD_TEST_MOD_VERSION = '1.0.9' }; Layer = 'MessageBox'; Note = 'refused by B itself' }
    hostcheck = @{ Exe = $exeOther; Env = @{ CONTROLLERMOD_TEST_ACT_RETAIL = '1' }; Layer = 'MessageBox'; Note = 'refused by host A' }
    retailhost = @{ Exe = $exeA; Env = @{}; EnvA = @{ CONTROLLERMOD_TEST_ACT_RETAIL = '1' }; Layer = 'MessageBox'; Note = 'refused by B itself' }
    retailaccept = @{ Exe = $exeA; Env = @{ CONTROLLERMOD_TEST_JOIN_CHECK = 'accept' }; EnvA = @{ CONTROLLERMOD_TEST_ACT_RETAIL = '1' }; Layer = 'MessageBox'; Note = 'accepted, then B left' }
}

$summary = New-Object System.Collections.Generic.List[string]
$allOk = $true
$backup = Join-Path $OutDir 'settings-backup'
Backup-MpSettings -Backup $backup
try {
    foreach ($case in $Cases) {
        $e = $expect[$case]
        $dir = Join-Path $OutDir $case
        New-Item -ItemType Directory -Force $dir | Out-Null
        Set-MpTwoCopySettings -Faction 2
        Clear-MpGameFiles -Game $Game
        & (Join-Path $PSScriptRoot 'Run-TwoInstances.ps1') -Game $Game -StepsA $stepsA -StepsB $stepsB -ShotsMs 21000, 29500, 38500 `
            -OutDir (Join-Path $dir 'shots') -ExeB $e.Exe -EnvB $e.Env -EnvA $(if ($e.EnvA) { $e.EnvA } else { @{} }) | Out-File -FilePath (Join-Path $dir 'run.txt') -Encoding ascii
        Start-Sleep -Seconds 2
        foreach ($f in 'controllermod_snaps.txt', 'controllermod_snaps_Instance02.txt', 'DebugLogFile.txt', 'DebugLogFile_Instance02.txt') {
            $src = Join-Path $Game $f
            if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $dir -Force }
        }
        # What B's screen was after the join: the menu layer in its last snapshot.
        $snapsB = Join-Path $dir 'controllermod_snaps_Instance02.txt'
        $layer = '(no snapshot)'
        if (Test-Path -LiteralPath $snapsB) {
            $text = Get-Content -LiteralPath $snapsB -Raw
            $after = $text.Substring([Math]::Max(0, $text.IndexOf('SNAP after-join')))
            if ($after -match 'menu: layer=(\S+)') { $layer = $Matches[1] }
        }
        $ok = $layer -like "*$($e.Layer)*"
        # The host's log shows whether it refused a join.
        $hostDenied = $false
        $logA = Join-Path $dir 'DebugLogFile.txt'
        if (Test-Path -LiteralPath $logA) { $hostDenied = [bool](Select-String -LiteralPath $logA -Pattern 'join denied because of CRC mismatch' -Quiet) }
        if ($case -eq 'hostcheck' -and -not $hostDenied) { $ok = $false }
        if ($case -ne 'hostcheck' -and $hostDenied) { $ok = $false }
        # retailaccept: B must have left again after the acceptance.
        $logB = Join-Path $dir 'DebugLogFile_Instance02.txt'
        $leftAgain = (Test-Path -LiteralPath $logB) -and [bool](Select-String -LiteralPath $logB -Pattern 'left again: the host is not the same Controller Mod' -Quiet)
        if (($case -eq 'retailaccept') -ne $leftAgain) { $ok = $false }
        # A refusal is shown once: the host's later announcements must not try again.
        $refusals = 0
        if (Test-Path -LiteralPath $logB) { $refusals = @(Select-String -LiteralPath $logB -Pattern 'showJoinRefusal - join refused').Count }
        $refusedByB = $case -in 'build', 'version', 'retailhost', 'retailaccept'
        if (($refusedByB -and $refusals -ne 1) -or (-not $refusedByB -and $refusals -ne 0)) { $ok = $false }
        if (-not $ok) { $allOk = $false }
        $summary.Add(("{0,-12} {1}  B after join: {2}; host refused: {3}; B left after acceptance: {4}; refusals shown: {5}  (expected: {6})" -f $case, $(if ($ok) { 'OK  ' } else { 'FAIL' }), $layer, $hostDenied, $leftAgain, $refusals, $e.Note))
        Clear-MpGameFiles -Game $Game
    }
}
finally {
    Stop-MpOwnedCopies -Dir $OutDir -Game $Game
    Start-Sleep -Seconds 1
    Restore-MpSettings -Backup $backup
    Clear-MpGameFiles -Game $Game
    if (Test-Path -LiteralPath $exeOther) { Remove-Item -LiteralPath $exeOther -Force }
}
$summary.Insert(0, "LAN join cases: $(if ($allOk) { 'ALL AS EXPECTED' } else { 'SOME NOT AS EXPECTED' })")
Set-Content -LiteralPath (Join-Path $OutDir 'summary.txt') -Value $summary -Encoding ascii
$summary
if (-not $allOk) { exit 1 }
exit 0
