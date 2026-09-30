<#
.SYNOPSIS
  Tests the sync test's verdict (MpSyncCheck.ps1) with made-up logs: a good match passes, and each
  way a bad run could slip through fails. No game is started. Exit 0 = all as expected.
  -RealLogDir: a Run-SyncTest output folder whose logs must pass (optional, with -RealBattleMs).
#>
param([string]$RealLogDir = '', [int]$RealBattleMs = 0)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'MpSyncCheck.ps1')
$script:pass = 0; $script:fail = 0
function Expect([string]$name, [bool]$wantOk, $a, $b, [int]$battleMs) {
    $r = Test-MpSync $a $b $battleMs
    if ($r.Ok -eq $wantOk) { $script:pass++; Write-Host "PASS $name" }
    else { $script:fail++; Write-Host "FAIL $name (verdict $($r.Ok))" -ForegroundColor Red; $r.Report | Select-Object -First 6 | ForEach-Object { Write-Host "     $_" } }
}

# A good 2-minute match: a checksum every 100 frames, both players sending every required kind.
$battleMs = 120000
function New-Lines([int]$lastFrame, [switch]$NoCommands, [string[]]$extra = @()) {
    $lines = New-Object System.Collections.Generic.List[string]
    for ($f = 0; $f -le $lastFrame; $f += 100) { $lines.Add(("Appended CRC on frame {0}: {1:X8}" -f $f, (($f * 2654435761) -band 0x7FFFFFFF))) }
    if (-not $NoCommands) {
        $kinds = 'MSG_DOZER_CONSTRUCT', 'MSG_QUEUE_UNIT_CREATE', 'MSG_DO_MOVETO', 'MSG_CREATE_TEAM1', 'MSG_SELECT_TEAM1', 'MSG_DO_SPECIAL_POWER_AT_LOCATION'
        $frame = 50
        foreach ($k in $kinds) { foreach ($p in 2, 3) { $lines.Add("ZHC-CMD frame $frame player $p args 1 $k"); $frame += 40 } }
    }
    foreach ($x in $extra) { $lines.Add($x) }
    return Read-MpLogLines $lines.ToArray()
}
$full = [int]($battleMs / 1000 * 30)

Expect 'good match passes' $true (New-Lines $full) (New-Lines $full) $battleMs
Expect 'zero commands fails' $false (New-Lines $full -NoCommands) (New-Lines $full -NoCommands) $battleMs
Expect 'a match that ended early fails' $false (New-Lines ([int]($full * 0.5))) (New-Lines ([int]($full * 0.5))) $battleMs
$bad = New-Lines $full; $bad.Crc[1000] = 'DEADBEEF'
Expect 'a different checksum fails' $false $bad (New-Lines $full) $battleMs
Expect 'a logged mismatch fails' $false (New-Lines $full -extra 'CRC Mismatch on frame 900') (New-Lines $full) $battleMs
Expect 'extra command after the last shared checksum passes' $true (New-Lines $full -extra "ZHC-CMD frame $($full + 20) player 2 args 1 MSG_DO_MOVETO") (New-Lines $full) $battleMs
Expect 'extra command before the last shared checksum fails' $false (New-Lines $full -extra 'ZHC-CMD frame 1500 player 2 args 1 MSG_DO_MOVETO') (New-Lines $full) $battleMs
$one = Read-MpLogLines ((0..$full | Where-Object { $_ % 100 -eq 0 } | ForEach-Object { "Appended CRC on frame ${_}: 00000001" }) + 'ZHC-CMD frame 10 player 2 args 1 MSG_DOZER_CONSTRUCT')
Expect 'only one player building fails' $false $one $one $battleMs
Expect 'a missing log fails' $false (New-Lines $full) $null $battleMs

if ($RealLogDir) {
    Expect "real logs in $RealLogDir pass" $true (Read-MpLog (Join-Path $RealLogDir 'DebugLogFile.txt')) (Read-MpLog (Join-Path $RealLogDir 'DebugLogFile_Instance02.txt')) $RealBattleMs
}
Write-Host "RESULT: $script:pass passed, $script:fail failed"
exit $(if ($script:fail) { 1 } else { 0 })
