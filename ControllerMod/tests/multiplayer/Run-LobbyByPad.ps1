<#
.SYNOPSIS
  A LAN game set up and started with the controller only: two copies on this PC, A hosts, B joins
  from the LAN list, B types a chat line with the on-screen keyboard and accepts, A starts with
  Start then A. Both must reach the battle.

.DESCRIPTION
  Uses the multiplayer test build. Everything is pad input: the main menu to Multiplayer and
  Network, A's Create Game (LEFT, DOWN from the games list), B's join from the LAN list (A picks the
  game, A joins), the on-screen keyboard, Start (jump to Accept / Play Game) and A. The one
  shortcut: @TextEntryChat reaches B's chat box.
  Output: both copies' snapshots, screenshots and a summary. Exit 0 = all checks passed.
#>
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
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
Copy-Item -LiteralPath $Build -Destination (Join-Path $Game 'generalszh_mp.exe') -Force

# Both start with LB: it only makes the menu focus appear (it can already be showing on one copy
# and not the other), so the same presses land the same way on both. B's clock starts 4 s after
# A's. B is in A's game by B 21 s. In the setup screen B opens the chat box's keyboard (@ reaches
# the box; the typing is pad input): Down to "a", Right x4 to "g", A A, Start = done (sends).
# Then Start jumps to Accept, A accepts. A: Start jumps to Play Game, A.
$stepsA = 'shell;8000:LB;8500:SNAP:host-main;8900:DOWN;9200:SNAP:host-down;9500:A;10500:SNAP:host-mp;10900:DOWN;11500:A;13000:SNAP:host-lobby;13500:LEFT;13900:DOWN;14300:SNAP:host-on-create;14700:A;16000:SNAP:host-setup;' +
    '36000:MENU;36600:SNAP:host-start-focus;37000:A;38000:SNAP:host-started;60000:SNAP:host-end'
$stepsB = 'shell;8000:LB;8500:SNAP:joiner-main;8900:DOWN;9200:SNAP:joiner-down;9500:A;10500:SNAP:joiner-mp;10900:DOWN;11500:A;17500:SNAP:joiner-lobby;18000:A;18600:SNAP:game-picked;19000:A;23000:SNAP:joined;' +
    '24000:@TextEntryChat;24600:SNAP:keyboard;25000:DOWN;25400:RIGHT;25700:RIGHT;26000:RIGHT;26300:RIGHT;26700:A;27000:A;' +
    '27400:SNAP:typed;27800:MENU;28400:SNAP:chat-sent;29000:MENU;29600:SNAP:accept-focus;30000:A;30800:SNAP:accepted;56000:SNAP:joiner-end'

$summary = New-Object System.Collections.Generic.List[string]
$ok = $true
$backup = Join-Path $OutDir 'settings-backup'
Backup-MpSettings -Backup $backup
try {
    Set-MpTwoCopySettings -Faction 2
    Clear-MpGameFiles -Game $Game
    & (Join-Path $PSScriptRoot 'Run-TwoInstances.ps1') -Game $Game -StepsA $stepsA -StepsB $stepsB -ShotsMs 28000, 31500, 35000, 42000, 62000, 68000 `
        -OutDir (Join-Path $OutDir 'shots') | Out-File -FilePath (Join-Path $OutDir 'run.txt') -Encoding ascii
    Start-Sleep -Seconds 2
    foreach ($f in 'controllermod_snaps.txt', 'controllermod_snaps_Instance02.txt', 'DebugLogFile.txt', 'DebugLogFile_Instance02.txt') {
        $src = Join-Path $Game $f
        if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $OutDir -Force }
    }
}
finally {
    Stop-MpOwnedCopies -Dir $OutDir -Game $Game
    Start-Sleep -Seconds 1
    Restore-MpSettings -Backup $backup
    Clear-MpGameFiles -Game $Game
}

function Get-Snap([string]$file, [string]$label) {
    $path = Join-Path $OutDir $file
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    $text = Get-Content -LiteralPath $path -Raw
    $at = $text.IndexOf("SNAP $label ")
    if ($at -lt 0) { return '' }
    $end = $text.IndexOf('== SNAP', $at + 5)
    if ($end -lt 0) { $end = $text.Length }
    return $text.Substring($at, $end - $at)
}
function Check([string]$what, [bool]$pass) {
    $script:summary.Add(("{0} {1}" -f $(if ($pass) { 'OK  ' } else { 'FAIL' }), $what))
    if (-not $pass) { $script:ok = $false }
}
$logA = Join-Path $OutDir 'DebugLogFile.txt'
Check 'A: LEFT, DOWN from the games list reached Create Game' ((Get-Snap 'controllermod_snaps.txt' 'host-on-create') -match 'focus=LanLobbyMenu.wnd:ButtonHost')
Check 'B: the LAN lobby starts on the games list' ((Get-Snap 'controllermod_snaps_Instance02.txt' 'joiner-lobby') -match 'focus=LanLobbyMenu.wnd:ListboxGames')
Check 'B joined the game setup screen' ((Get-Snap 'controllermod_snaps_Instance02.txt' 'joined') -match 'layer=LanGameOptionsMenu')
Check 'B: the chat box opened the on-screen keyboard' ((Get-Snap 'controllermod_snaps_Instance02.txt' 'keyboard') -match 'keyboard: letters .*TextEntryChat')
Check 'B: typed "gg" with the pad' ((Get-Snap 'controllermod_snaps_Instance02.txt' 'typed') -match 'text="gg"')
Check 'B: Start sent the chat line (keyboard closed)' ((Get-Snap 'controllermod_snaps_Instance02.txt' 'chat-sent') -notmatch 'keyboard:')
Check 'A received the chat line' ((Get-Snap 'controllermod_snaps.txt' 'host-start-focus') -match 'chat: \S+ "\[Instance02\] gg"')
Check 'B: Start jumped to Accept' ((Get-Snap 'controllermod_snaps_Instance02.txt' 'accept-focus') -match 'focus=LanGameOptionsMenu.wnd:ButtonStart')
Check 'A: Start jumped to Play Game' ((Get-Snap 'controllermod_snaps.txt' 'host-start-focus') -match 'focus=LanGameOptionsMenu.wnd:ButtonStart')
Check 'A reached the battle' ((Get-Snap 'controllermod_snaps.txt' 'host-end') -match 'context: battle')
# (A can only start once B has accepted, so both in battle also proves B's Accept.)
Check 'B reached the battle' ((Get-Snap 'controllermod_snaps_Instance02.txt' 'joiner-end') -match 'context: battle')
$summary.Insert(0, "Lobby by pad: $(if ($ok) { 'PASS' } else { 'FAIL' })")
Set-Content -LiteralPath (Join-Path $OutDir 'summary.txt') -Value $summary -Encoding ascii
$summary
if (-not $ok) { exit 1 }
exit 0
