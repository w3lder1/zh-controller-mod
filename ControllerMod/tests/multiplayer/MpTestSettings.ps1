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

# The backup is a new folder for each run, with a list (backup.txt) of every file: there or not,
# and its SHA-256. A folder that already exists is refused, so an old backup is never put back.
function Backup-MpSettings([string]$Backup) {
    if (Test-Path -LiteralPath $Backup) { throw "The backup folder $Backup already exists (an earlier run?). Use a new -OutDir." }
    foreach ($f in $MpInstanceFiles) {
        if (Test-Path -LiteralPath (Join-Path $MpUserData $f)) { throw "$f already exists in the user data folder; remove it or check what made it." }
    }
    New-Item -ItemType Directory -Force (Join-Path $Backup 'Replays') | Out-Null
    $list = New-Object System.Collections.Generic.List[string]
    foreach ($f in $MpTouched) {
        $src = Join-Path $MpUserData $f
        if (Test-Path -LiteralPath $src -PathType Leaf) {
            $hash = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
            Copy-Item -LiteralPath $src -Destination (Join-Path $Backup $f) -Force
            if ((Get-FileHash -LiteralPath (Join-Path $Backup $f) -Algorithm SHA256).Hash -ne $hash) { throw "Backing up $f failed." }
            $list.Add("present|$hash|$f")
        } else {
            $list.Add("absent||$f")
        }
    }
    Set-Content -LiteralPath (Join-Path $Backup 'backup.txt') -Value $list -Encoding ascii
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

# Puts back exactly what backup.txt lists and checks the hashes; without that list nothing is
# touched. The second copy's files are removed.
function Restore-MpSettings([string]$Backup) {
    $listFile = Join-Path $Backup 'backup.txt'
    if (-not (Test-Path -LiteralPath $listFile)) { throw "No backup list in $Backup; nothing was restored. Check the settings by hand." }
    $problems = @()
    foreach ($entry in Get-Content -LiteralPath $listFile) {
        $state, $hash, $f = $entry -split '\|', 3
        $live = Join-Path $MpUserData $f
        if ($state -eq 'present') {
            Copy-Item -LiteralPath (Join-Path $Backup $f) -Destination $live -Force
            if ((Get-FileHash -LiteralPath $live -Algorithm SHA256).Hash -ne $hash) { $problems += $f }
        } elseif (Test-Path -LiteralPath $live) {
            Remove-Item -LiteralPath $live -Force
        }
    }
    foreach ($f in $MpInstanceFiles) {
        $live = Join-Path $MpUserData $f
        if (Test-Path -LiteralPath $live) { Remove-Item -LiteralPath $live -Force }
    }
    if ($problems.Count) { throw "Restored with different content: $($problems -join ', '). The backup is in $Backup." }
}

# Ends only the game copies Run-TwoInstances.ps1 started (their process ids are in pids.txt files
# under $Dir), and only while they still run the test program of the test game folder.
function Stop-MpOwnedCopies([string]$Dir, [string]$Game) {
    $exe = [System.IO.Path]::GetFullPath((Join-Path $Game 'generalszh_mp.exe'))
    foreach ($file in @(Get-ChildItem -LiteralPath $Dir -Recurse -Filter 'pids.txt' -ErrorAction SilentlyContinue)) {
        foreach ($id in Get-Content -LiteralPath $file.FullName) {
            $proc = Get-Process -Id ([int]$id) -ErrorAction SilentlyContinue
            if ($proc -and $proc.Path -and [System.IO.Path]::GetFullPath($proc.Path) -eq $exe) { Stop-Process -Id $proc.Id -Force }
        }
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
