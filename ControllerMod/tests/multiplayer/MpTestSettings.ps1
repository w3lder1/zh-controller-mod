# Two game copies on one PC: the player settings they need, backed up first and always put back.
# Dot-source: . (Join-Path $PSScriptRoot 'MpTestSettings.ps1')
#
# Copy A (first started) reads options.ini / Network.ini, copy B (instance 2) reads
# Options_Instance02.ini / Network_Instance02.ini. Each needs its own address: LAN games use
# IPAddress, Direct Connect uses GameSpyIPAddress (the "Online IP" in Options). B remembers A's
# address for Direct Connect. Both play the lobby faction given (2 USA, 3 China, 4 GLA): the host
# uses Network.ini, a joiner sends its own file's faction when it joins.

$MpUserData = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Command and Conquer Generals Zero Hour Data'
$MpTouched = 'options.ini', 'Network.ini', 'ControllerMod.ini', 'Replays\00000000.rep'
$MpInstanceFiles = 'Options_Instance02.ini', 'Network_Instance02.ini', 'Skirmish_Instance02.ini', 'Replays\00000000_Instance02.rep'

function Backup-MpSettings([string]$Backup) {
    New-Item -ItemType Directory -Force (Join-Path $Backup 'Replays') | Out-Null
    foreach ($f in $MpInstanceFiles) {
        if (Test-Path -LiteralPath (Join-Path $MpUserData $f)) { throw "$f already exists in the user data folder; remove it or check what made it." }
    }
    foreach ($f in $MpTouched) {
        $src = Join-Path $MpUserData $f
        if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination (Join-Path $Backup $f) -Force }
    }
}

function Set-MpTwoCopySettings([int]$Faction = 2) {
    $options = @(Get-Content -LiteralPath (Join-Path $MpUserData 'options.ini') | Where-Object { $_ -notmatch '^(GameSpy)?IPAddress\s*=' })
    Set-Content -LiteralPath (Join-Path $MpUserData 'options.ini') -Value ($options + 'IPAddress = 127.0.0.1' + 'GameSpyIPAddress = 127.0.0.1') -Encoding ascii
    Set-Content -LiteralPath (Join-Path $MpUserData 'Options_Instance02.ini') -Value ($options + 'IPAddress = 127.0.0.2' + 'GameSpyIPAddress = 127.0.0.2') -Encoding ascii
    $network = @()
    if (Test-Path -LiteralPath (Join-Path $MpUserData 'Network.ini')) {
        $network = @(Get-Content -LiteralPath (Join-Path $MpUserData 'Network.ini') | Where-Object { $_ -notmatch '^PlayerTemplate\s*=' })
    }
    Set-Content -LiteralPath (Join-Path $MpUserData 'Network.ini') -Value ($network + "PlayerTemplate = $Faction") -Encoding ascii
    Set-Content -LiteralPath (Join-Path $MpUserData 'Network_Instance02.ini') -Value @('NumRemoteIPs = 1', 'RemoteIP0 = 127.0.0.1', "PlayerTemplate = $Faction") -Encoding ascii
    # Two windows would fight over one mouse cursor.
    $cm = Join-Path $MpUserData 'ControllerMod.ini'
    if (Test-Path -LiteralPath $cm) {
        (Get-Content -LiteralPath $cm) -replace '^LockMouseToReticle\s*=.*$', 'LockMouseToReticle = 0' | Set-Content -LiteralPath $cm -Encoding ascii
    }
}

function Restore-MpSettings([string]$Backup) {
    foreach ($f in $MpTouched) {
        $saved = Join-Path $Backup $f
        $live = Join-Path $MpUserData $f
        if (Test-Path -LiteralPath $saved) { Copy-Item -LiteralPath $saved -Destination $live -Force }
        elseif (Test-Path -LiteralPath $live) { Remove-Item -LiteralPath $live -Force }
    }
    foreach ($f in $MpInstanceFiles) {
        $live = Join-Path $MpUserData $f
        if (Test-Path -LiteralPath $live) { Remove-Item -LiteralPath $live -Force }
    }
}

# Test files the game copies write into the game folder.
function Clear-MpGameFiles([string]$Game) {
    foreach ($f in 'DebugLogFile.txt', 'DebugLogFile_Instance02.txt', 'DebugLogFilePrev.txt', 'DebugLogFilePrev_Instance02.txt',
        'controllermod_snaps.txt', 'controllermod_snaps_Instance02.txt', 'controllermod_state.txt', 'controllermod_state_Instance02.txt',
        'controllermod_test_start.txt') {
        $p = Join-Path $Game $f
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue }
    }
}
