<#
.SYNOPSIS
  A multiplayer match used with the controller only: chat, the pause menu, surrender, and a
  player who drops out. Two copies on this PC; everything by pad from the main menu.

.DESCRIPTION
  Uses the multiplayer test build. Both copies go through the lobby by pad (as Run-LobbyByPad.ps1),
  then in the battle:
  -Case chat (default):
    B: Menu opens the pause menu (the match goes on: multiplayer never pauses), B closes it;
       View (help), A = chat to everyone, types "hi" on the on-screen keyboard, Start sends it;
       View, X = chat to allies, types a letter, B cancels (nothing is sent);
       Menu, Down to Surrender, A, Left to Yes, A.
    A must receive "[Instance02] hi" and not the cancelled line. Both get the score screen, where
    Start jumps to OK and A goes back to the LAN lobby.
    (The pause menu is B's: the game opens it only in the window in front.)
  -Case drop:
    B's game is ended in the battle (a player whose game or connection died). A gets the "waiting
    for players" screen, where the pad works: B does nothing, the chat box opens the keyboard,
    Start sends, and A on the vote button drops B at once. The match goes on for A.
  Output: both copies' snapshots, screenshots, debug logs and a summary. Exit 0 = all checks passed.
#>
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [ValidateSet('chat', 'drop')][string]$Case = 'chat',
    [string]$Game = (Join-Path $PSScriptRoot '..\..\..\..\TestGame\ZeroHour'),
    [string]$Build = (Join-Path $PSScriptRoot '..\..\..\build\mptest\GeneralsMD\Release\generalszh.exe'),
    [switch]$MenuDump
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'MpTestSettings.ps1')
if (Get-Process -Name 'generalszh*', 'generals' -ErrorAction SilentlyContinue) { throw 'Zero Hour is running. Close it first.' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
Copy-Item -LiteralPath $Build -Destination (Join-Path $Game 'generalszh_mp.exe') -Force

# The lobby by pad (see Run-LobbyByPad.ps1): A creates the game, B joins from the LAN list and
# accepts, A starts. Then battle; steps count from each copy's first battle frame.
$lobbyA = 'shell;8000:LB;8900:DOWN;9500:A;10900:DOWN;11500:A;13500:LEFT;13900:DOWN;14700:A;36000:MENU;37000:A;'
$lobbyB = 'shell;8000:LB;8900:DOWN;9500:A;10900:DOWN;11500:A;18000:A;19000:A;29000:MENU;30000:A;'

if ($Case -eq 'chat') {
    # B does the pause menu (the game opens it only in the window in front, which is B's).
    # On-screen keyboard: it opens on "q" (row 1, column 0). "h": Down to "a", Right x5. "i": Up to
    # "y", Right x2. Pause menu, top to bottom: Options (the focus starts there), Surrender, Exit,
    # Return. "Are you sure?": the focus starts on No, Left is Yes.
    # Score screen: Start jumps to OK, A goes back to the LAN lobby.
    $stepsA = $lobbyA + 'battle;2000:SNAP:a-battle;18500:SNAP:a-after-chat;27000:SNAP:a-after-surrender;36000:SNAP:a-score;' +
        '42000:MENU;42600:SNAP:a-score-focus;43000:A;45500:SNAP:a-after-score'
    $stepsB = $lobbyB + 'battle;2000:SNAP:b-battle;2500:MENU;3300:SNAP:b-quit-menu;5300:SNAP:b-quit-menu-later;5700:B;6300:SNAP:b-quit-closed;' +
        '7000:VIEW;7400:SNAP:help;7800:A;8600:SNAP:chat-open;' +
        '9000:DOWN;9300:RIGHT;9600:RIGHT;9900:RIGHT;10200:RIGHT;10500:RIGHT;10900:A;11300:UP;11600:RIGHT;11900:RIGHT;12300:A;12700:SNAP:chat-typed;' +
        '13100:MENU;13700:SNAP:chat-sent;' +
        '15000:VIEW;15500:X;16300:SNAP:allies-open;16700:A;17000:SNAP:allies-typed;17400:B;18000:SNAP:allies-cancelled;' +
        '20000:MENU;20800:SNAP:b-quit-menu-2;21200:DOWN;21600:SNAP:on-surrender;22000:A;23000:SNAP:surrender-box;23400:LEFT;23800:SNAP:on-yes;24200:A;25200:SNAP:b-after-surrender;' +
        '36000:SNAP:b-score;40000:MENU;40600:SNAP:b-score-focus;41000:A;43500:SNAP:b-after-score'
    $shots = 70000, 80000, 90000, 100000, 110000
    $kill = 0
}
else {
    # B's game ends 72 s after A's launch, about 27 s into the battle; A's screen comes about 5 s
    # later (the game waits 5 s for a player) and would drop B by itself after 60 s more.
    # The screen: vote button top left (the focus starts there), chat box below, Quit Game bottom right.
    $stepsA = $lobbyA + 'battle;2000:SNAP:a-battle;20000:SNAP:a-before-drop;' +
        '36000:SNAP:d-screen;36500:B;37000:SNAP:d-after-b;37500:DOWN;38000:SNAP:d-on-entry;38400:A;39000:SNAP:d-keyboard;' +
        '39400:A;39800:SNAP:d-typed;40200:MENU;40800:SNAP:d-chat-sent;41200:UP;41600:SNAP:d-on-vote;42000:A;' +
        '47000:SNAP:d-after-vote;52000:SNAP:d-later'
    $stepsB = $lobbyB + 'battle;2000:SNAP:b-battle'
    $shots = 70000, 72000, 90000, 105000, 112000
    $kill = 72000
}

$envA = @{}; $envB = @{}
if ($MenuDump) { $envA['CONTROLLERMOD_MENU_DUMP'] = '1'; $envB['CONTROLLERMOD_MENU_DUMP'] = '1' }

$summary = New-Object System.Collections.Generic.List[string]
$ok = $true
$backup = Join-Path $OutDir 'settings-backup'
Backup-MpSettings -Backup $backup
try {
    Set-MpTwoCopySettings -Faction 2
    Clear-MpGameFiles -Game $Game
    & (Join-Path $PSScriptRoot 'Run-TwoInstances.ps1') -Game $Game -StepsA $stepsA -StepsB $stepsB -ShotsMs $shots -KillBMs $kill -EnvA $envA -EnvB $envB `
        -OutDir (Join-Path $OutDir 'shots') | Out-File -FilePath (Join-Path $OutDir 'run.txt') -Encoding ascii
    Start-Sleep -Seconds 2
    foreach ($f in 'controllermod_snaps.txt', 'controllermod_snaps_Instance02.txt', 'DebugLogFile.txt', 'DebugLogFile_Instance02.txt',
        'controllermod_menu_dump.txt', 'controllermod_menu_dump_Instance02.txt') {
        $src = Join-Path $Game $f
        if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $OutDir -Force }
    }
}
finally {
    foreach ($p in Get-Process -Name 'generalszh_mp*' -ErrorAction SilentlyContinue) { Stop-Process -Id $p.Id -Force }
    Start-Sleep -Seconds 1
    Restore-MpSettings -Backup $backup
    Clear-MpGameFiles -Game $Game
    foreach ($f in 'controllermod_menu_dump.txt', 'controllermod_menu_dump_Instance02.txt') {
        Remove-Item -LiteralPath (Join-Path $Game $f) -ErrorAction SilentlyContinue
    }
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
function Get-Frame([string]$snap) { if ($snap -match 'frame=(\d+)') { return [int]$Matches[1] } return -1 }
function Check([string]$what, [bool]$pass) {
    $script:summary.Add(("{0} {1}" -f $(if ($pass) { 'OK  ' } else { 'FAIL' }), $what))
    if (-not $pass) { $script:ok = $false }
}
$snapsA = 'controllermod_snaps.txt'
$snapsB = 'controllermod_snaps_Instance02.txt'
$logA = Join-Path $OutDir 'DebugLogFile.txt'
$logTextA = if (Test-Path -LiteralPath $logA) { Get-Content -LiteralPath $logA -Raw } else { '' }

Check 'A reached the battle' ((Get-Snap $snapsA 'a-battle') -match 'context: battle')
Check 'B reached the battle' ((Get-Snap $snapsB 'b-battle') -match 'context: battle')
if ($Case -eq 'chat') {
    Check 'B: View shows the help' ((Get-Snap $snapsB 'help') -match 'context: battle help')
    Check 'B: A in the help opened the chat box with the keyboard on it' ((Get-Snap $snapsB 'chat-open') -match 'match-chat' -and (Get-Snap $snapsB 'chat-open') -match 'keyboard: letters .*TextEntryChat')
    Check 'B: typed "hi" with the pad' ((Get-Snap $snapsB 'chat-typed') -match 'keyboard: .*text="hi"')
    Check 'B: Start sent it and closed the chat box' ((Get-Snap $snapsB 'chat-sent') -match 'context: battle\r?\n' -and (Get-Snap $snapsB 'chat-sent') -notmatch 'keyboard:')
    Check 'A received "[Instance02] hi"' ($logTextA -match 'ZHC-MSG \[Instance02\] hi')
    Check 'B: X in the help opened the chat box (allies)' ((Get-Snap $snapsB 'allies-open') -match 'keyboard: letters .*TextEntryChat')
    Check 'B: typed "q"' ((Get-Snap $snapsB 'allies-typed') -match 'text="q"')
    Check 'B: B cancelled the chat (box closed, back in the battle)' ((Get-Snap $snapsB 'allies-cancelled') -match 'context: battle\r?\n')
    Check 'A did not receive the cancelled line' ($logTextA -notmatch 'ZHC-MSG \[Instance02\] q')
    $qm1 = Get-Snap $snapsB 'b-quit-menu'; $qm2 = Get-Snap $snapsB 'b-quit-menu-later'
    Check 'B: Menu opened the pause menu' ($qm1 -match 'quit-menu' -and $qm1 -match 'menu: layer=QuitNoSave')
    Check 'B: the match went on under the pause menu (multiplayer)' ((Get-Frame $qm2) -gt (Get-Frame $qm1) + 30)
    Check 'B: B closed the pause menu' ((Get-Snap $snapsB 'b-quit-closed') -match 'context: battle\r?\n')
    Check 'B: Down reached Surrender' ((Get-Snap $snapsB 'on-surrender') -match 'focus=QuitNoSave.wnd:ButtonRestart')
    Check 'B: "Are you sure?" starts on No' ((Get-Snap $snapsB 'surrender-box') -match 'focus=MessageBox.wnd:ButtonNo')
    Check 'B: Left reached Yes' ((Get-Snap $snapsB 'on-yes') -match 'focus=MessageBox.wnd:ButtonYes')
    Check 'B surrendered (money gone, back on the battlefield)' ((Get-Snap $snapsB 'b-after-surrender') -match 'context: battle\r?\n' -and (Get-Snap $snapsB 'b-after-surrender') -match 'money=0 ')
    Check 'A: the score screen after the match' ((Get-Snap $snapsA 'a-score') -match 'layer=ScoreScreen')
    Check 'B: the score screen after the match' ((Get-Snap $snapsB 'b-score') -match 'layer=ScoreScreen')
    Check 'A: Start jumped to OK on the score screen' ((Get-Snap $snapsA 'a-score-focus') -match 'focus=ScoreScreen.wnd:ButtonOk')
    Check 'B: Start jumped to OK on the score screen' ((Get-Snap $snapsB 'b-score-focus') -match 'focus=ScoreScreen.wnd:ButtonOk')
    Check 'A: OK left the score screen' ((Get-Snap $snapsA 'a-after-score') -match 'context: shell' -and (Get-Snap $snapsA 'a-after-score') -notmatch 'ScoreScreen')
    Check 'B: OK left the score screen' ((Get-Snap $snapsB 'b-after-score') -match 'context: shell' -and (Get-Snap $snapsB 'b-after-score') -notmatch 'ScoreScreen')
}
else {
    Check 'A: the battle ran before the drop' ((Get-Snap $snapsA 'a-before-drop') -match 'context: battle\r?\n')
    Check 'A: the "waiting for players" screen appeared' ((Get-Snap $snapsA 'd-screen') -match 'disconnect-screen')
    # The first press only shows the focus frame (the pad was idle), on the vote button.
    $afterB = Get-Snap $snapsA 'd-after-b'
    Check 'A: B did nothing there (screen still up, focus on the vote button)' ($afterB -match 'disconnect-screen' -and $afterB -match 'focus=DisconnectScreen.wnd:ButtonKickPlayer1')
    Check 'A: Down reached the chat box' ((Get-Snap $snapsA 'd-on-entry') -match 'focus=DisconnectScreen.wnd:TextEntry')
    Check 'A: A opened the on-screen keyboard' ((Get-Snap $snapsA 'd-keyboard') -match 'keyboard: letters .*DisconnectScreen.wnd:TextEntry')
    Check 'A: typed "q"' ((Get-Snap $snapsA 'd-typed') -match 'text="q"')
    $sent = Get-Snap $snapsA 'd-chat-sent'
    Check 'A: Start sent it (keyboard closed, no pause menu, screen still up)' ($sent -match 'disconnect-screen' -and $sent -notmatch 'keyboard:' -and $sent -notmatch 'quit-menu')
    Check 'A: Up reached the vote button' ((Get-Snap $snapsA 'd-on-vote') -match 'focus=DisconnectScreen.wnd:ButtonKickPlayer1')
    $after = Get-Snap $snapsA 'd-after-vote'
    Check 'A: the vote dropped B at once (screen gone, back in the battle)' ($after -match 'context: battle\r?\n')
    Check 'A: the match went on' ((Get-Frame (Get-Snap $snapsA 'd-later')) -gt (Get-Frame $after) + 60)
    Check 'A was told B left' ($logTextA -match 'ZHC-MSG Instance02 has left the game')
}
$summary.Insert(0, "Match by pad ($Case): $(if ($ok) { 'PASS' } else { 'FAIL' })")
Set-Content -LiteralPath (Join-Path $OutDir 'summary.txt') -Value $summary -Encoding ascii
$summary
if (-not $ok) { exit 1 }
