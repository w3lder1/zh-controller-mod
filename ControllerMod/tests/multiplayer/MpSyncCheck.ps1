# The sync test's verdict, separate so it can be tested with made-up logs (Test-MpSyncCheck.ps1).
# Dot-source: . (Join-Path $PSScriptRoot 'MpSyncCheck.ps1')
#
# Rules for a PASS:
# - every sync checksum both copies wrote matches, and neither logged a mismatch;
# - enough checksums for the match length asked for (one per 100 frames, 30 frames a second), so a
#   match that ended early cannot pass;
# - both copies ran the same commands in the same order; one copy may have run a few more only
#   after the last checksum both share (it was stopped a moment later), never before;
# - each of the two players sent the kinds of commands the routine is about.

$MpRequiredKinds = @('MSG_DOZER_CONSTRUCT', 'MSG_QUEUE_UNIT_CREATE', 'MSG_DO_MOVETO', 'MSG_CREATE_TEAM*', 'MSG_SELECT_TEAM*',
    'MSG_DO_SPECIAL_POWER_AT_LOCATION')

function Read-MpLogLines([string[]]$lines) {
    $crc = @{}; $cmds = New-Object System.Collections.Generic.List[string]; $mismatch = @()
    foreach ($line in $lines) {
        if ($line -match 'Appended CRC on frame (\d+): ([0-9A-F]{8})') { $crc[[int]$Matches[1]] = $Matches[2] }
        elseif ($line -match 'ZHC-CMD (frame \d+ player \d+ args \d+ \S+)') { $cmds.Add($Matches[1]) }
        elseif ($line -match 'CRC Mismatch|sawCRCMismatch|mismatch') { $mismatch += $line.Trim() }
    }
    return @{ Crc = $crc; Cmds = $cmds; Mismatch = $mismatch }
}

function Read-MpLog([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    return Read-MpLogLines ([System.IO.File]::ReadAllLines($path))
}

function Get-MpCmdFrame([string]$cmd) { if ($cmd -match '^frame (\d+) ') { return [int]$Matches[1] } return -1 }

# $a, $b: Read-MpLog results. $battleMs: how long the scripted match was meant to last.
# Returns @{ Ok; Report (lines) }.
function Test-MpSync($a, $b, [int]$battleMs, [int]$minCrcs = 20) {
    $report = New-Object System.Collections.Generic.List[string]
    $ok = $true
    if (-not $a -or -not $b) {
        $report.Add("Missing log: A=$([bool]$a) B=$([bool]$b)")
        return @{ Ok = $false; Report = $report }
    }
    $common = @($a.Crc.Keys | Where-Object { $b.Crc.ContainsKey($_) } | Sort-Object)
    $diff = @($common | Where-Object { $a.Crc[$_] -ne $b.Crc[$_] })
    $report.Add("Sync checksums: A wrote $($a.Crc.Count), B wrote $($b.Crc.Count), compared $($common.Count), different $($diff.Count)")
    if ($common.Count) { $report.Add("  frames $($common[0]) to $($common[-1]) ($([Math]::Round($common[-1] / 30 / 60, 1)) min of game time)") }
    if ($diff.Count) { $ok = $false; $report.Add("  first difference at frame $($diff[0]): A $($a.Crc[$diff[0]]) B $($b.Crc[$diff[0]])") }
    $needCrcs = [Math]::Max($minCrcs, [int][Math]::Floor($battleMs / 1000.0 * 30 / 100 * 0.85))
    if ($common.Count -lt $needCrcs) { $ok = $false; $report.Add("  too few checksums ($($common.Count), need $needCrcs for this match length): the match did not run long enough") }
    foreach ($side in @(@('A', $a), @('B', $b))) {
        if ($side[1].Mismatch.Count) { $ok = $false; $report.Add("Copy $($side[0]) logged a mismatch: $($side[1].Mismatch[0])") }
    }

    # The same commands in the same order.
    $n = [Math]::Min($a.Cmds.Count, $b.Cmds.Count)
    $firstDiff = -1
    for ($i = 0; $i -lt $n; $i++) { if ($a.Cmds[$i] -ne $b.Cmds[$i]) { $firstDiff = $i; break } }
    $report.Add("Commands: A ran $($a.Cmds.Count), B ran $($b.Cmds.Count); first $n compared, $(if ($firstDiff -lt 0) { 'identical' } else { "differ at #$firstDiff" })")
    if ($firstDiff -ge 0) { $ok = $false; $report.Add("  A: $($a.Cmds[$firstDiff])"); $report.Add("  B: $($b.Cmds[$firstDiff])") }
    # Commands only one copy ran must come after the last checksum both share.
    $longer = if ($a.Cmds.Count -ge $b.Cmds.Count) { $a } else { $b }
    $lastShared = if ($common.Count) { $common[-1] } else { -1 }
    for ($i = $n; $i -lt $longer.Cmds.Count; $i++) {
        if ((Get-MpCmdFrame $longer.Cmds[$i]) -le $lastShared) {
            $ok = $false
            $report.Add("  a command only one copy ran, before the last shared checksum (frame $lastShared): $($longer.Cmds[$i])")
            break
        }
    }

    # What the controller sent, per player (from A's log; the streams are the same).
    $byKind = @{}
    $byPlayer = @{}
    foreach ($c in $a.Cmds) {
        if ($c -match 'player (\d+) args \d+ (\S+)') {
            $key = $Matches[2]; $player = $Matches[1]
            if (-not $byKind.ContainsKey($key)) { $byKind[$key] = @{} }
            $byKind[$key][$player] = 1 + [int]$byKind[$key][$player]
            if (-not $byPlayer.ContainsKey($player)) { $byPlayer[$player] = @{} }
            $byPlayer[$player][$key] = $true
        }
    }
    # The two players are the ones that built something (the other entries are the game's own).
    $players = @($byPlayer.Keys | Where-Object { $byPlayer[$_].ContainsKey('MSG_DOZER_CONSTRUCT') } | Sort-Object)
    if ($players.Count -ne 2) { $ok = $false; $report.Add("  expected 2 players who built something, found $($players.Count)") }
    foreach ($p in $players) {
        $missing = @($MpRequiredKinds | Where-Object { $kind = $_; -not @($byPlayer[$p].Keys | Where-Object { $_ -like $kind }).Count })
        if ($missing.Count) { $ok = $false; $report.Add("  player $p never sent: $($missing -join ', ')") }
    }
    $report.Add('Commands by kind (player: count):')
    foreach ($k in ($byKind.Keys | Sort-Object)) {
        $per = ($byKind[$k].Keys | Sort-Object | ForEach-Object { "p$_ $($byKind[$k][$_])" }) -join ', '
        $report.Add(("  {0,-40} {1}" -f $k, $per))
    }
    return @{ Ok = $ok; Report = $report }
}
