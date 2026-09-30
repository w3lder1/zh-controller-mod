<#
    Fixture tests for the zip installer and uninstaller (ControllerMod/release/*.ps1).

    Everything runs on fake game folders under %TEMP%\ZHControllerInstallerTests, which is deleted
    and made again at the start. No real game, no Windows permission prompt, nothing outside that
    folder is touched (one check tries C:\Program Files\ZHControllerTest and expects a refusal).

    Usage: powershell -NoProfile -ExecutionPolicy Bypass -File Test-ZipInstaller.ps1
    Exit code 0 when every check passes.
#>
$ErrorActionPreference = 'Stop'
$rel = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\release'))
$base = Join-Path ([System.IO.Path]::GetTempPath()) 'ZHControllerInstallerTests'
if (-not $base.StartsWith([System.IO.Path]::GetTempPath(), [System.StringComparison]::OrdinalIgnoreCase)) { throw 'fixture root not in TEMP' }
if (Test-Path -LiteralPath $base) { cmd /c rmdir /s /q "$base" }   # rmdir does not follow junctions
New-Item -ItemType Directory $base | Out-Null

function New-FakeGame([string]$path) {
    New-Item -ItemType Directory $path | Out-Null
    foreach ($f in 'INIZH.big', 'WindowZH.big', 'MapsZH.big', 'd3d8.dll', 'Generals.exe') { Set-Content -LiteralPath (Join-Path $path $f) -Value "x $f" }
    New-Item -ItemType Directory (Join-Path $path 'Data') | Out-Null
    Set-Content -LiteralPath (Join-Path $path 'Data\a.txt') -Value 'x'
}
function Get-TreeFingerprint([string]$path, [string[]]$exclude) {
    $items = Get-ChildItem -LiteralPath $path -Recurse -File -Force | Where-Object {
        $r = $_.FullName.Substring($path.Length + 1); -not ($exclude | Where-Object { $r -eq $_ -or $r.StartsWith($_ + '\') })
    } | Sort-Object FullName | ForEach-Object { $_.FullName.Substring($path.Length + 1) + '|' + (Get-FileHash -LiteralPath $_.FullName).Hash }
    return ($items -join "`n")
}

$game = Join-Path $base 'Game'
New-FakeGame $game
$pkg = Join-Path $base 'Package'
New-Item -ItemType Directory $pkg | Out-Null
Copy-Item "$rel\*.ps1", "$rel\*.cmd", "$rel\*.txt" $pkg
Set-Content -LiteralPath (Join-Path $pkg 'generalszh.exe') -Value 'fake exe v1'

$ps64 = "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe"
$ps32 = "$env:windir\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
$results = @()
function Run([string]$ps, [string]$script, [string[]]$scriptArgs) {
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $script @scriptArgs 2>&1 | Out-String
    return @{ Code = $LASTEXITCODE; Out = $out }
}
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    $script:results += ('{0}  {1}{2}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name, $(if (-not $ok -and $detail) { "`n      $detail" } else { '' }))
}
$inst = "$pkg\Install-ZHController.ps1"
$uninst = "$pkg\Uninstall-ZHController.ps1"
$added = @('generalszh.exe', 'ZH Controller')

foreach ($s in $inst, $uninst) {
    $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile($s, [ref]$null, [ref]$errors) | Out-Null
    Check "parses: $(Split-Path -Leaf $s)" ($errors.Count -eq 0)
}

# ================= In place (default) ============================================================
$before = Get-TreeFingerprint $game $added
$r = Run $ps64 $inst @('-GamePath', $game, '-NoShortcut')
Check 'in-place: install' ($r.Code -eq 0 -and (Test-Path "$game\generalszh.exe") -and (Test-Path "$game\ZH Controller\Uninstall.cmd")) $r.Out
Check 'in-place: game files unchanged (incl. GenTool d3d8.dll)' ((Get-TreeFingerprint $game $added) -eq $before)
$rec = Get-Content "$game\ZH Controller\ZHController.install.txt"
Check 'in-place: record Complete, GameRoot, file list' ($rec -contains 'State=Complete' -and $rec -contains "GameRoot=$game" -and ($rec -match '^File=generalszh.exe\|').Count -eq 1 -and ($rec -match '^File=').Count -eq (1 + @('README.txt', 'CONTROLS.txt', 'RELEASE_NOTES.txt', 'LICENSE.txt', 'VERSION.txt', 'Uninstall-ZHController.ps1', 'Uninstall.cmd' | Where-Object { Test-Path (Join-Path $pkg $_) }).Count))
$r = Run $ps64 $inst @('-GamePath', $game, '-NoShortcut')
Check 'in-place: rerun = update' ($r.Code -eq 0 -and $r.Out -match 'Updated') $r.Out
# A new version: the exe is replaced, the record follows.
Set-Content -LiteralPath (Join-Path $pkg 'generalszh.exe') -Value 'fake exe v2'
$r = Run $ps64 $inst @('-GamePath', $game, '-NoShortcut')
Check 'in-place: update to a new exe' ($r.Code -eq 0 -and (Get-Content "$game\generalszh.exe") -eq 'fake exe v2') $r.Out

# A foreign generalszh.exe is never replaced.
$game2 = Join-Path $base 'Game2'
New-FakeGame $game2
Set-Content -LiteralPath "$game2\generalszh.exe" -Value 'someone elses build'
$r = Run $ps64 $inst @('-GamePath', $game2, '-NoShortcut')
Check 'in-place: refuses to replace a foreign generalszh.exe' ($r.Code -ne 0 -and (Get-Content "$game2\generalszh.exe") -eq 'someone elses build' -and -not (Test-Path "$game2\ZH Controller")) $r.Out
Remove-Item "$game2\generalszh.exe"
New-Item -ItemType Directory "$game2\ZH Controller" | Out-Null
Set-Content "$game2\ZH Controller\notes.txt" 'mine'
$r = Run $ps64 $inst @('-GamePath', $game2, '-NoShortcut')
Check "in-place: refuses a 'ZH Controller' folder it did not make" ($r.Code -ne 0 -and -not (Test-Path "$game2\generalszh.exe")) $r.Out
$r = Run $ps64 $uninst @('-GamePath', $game2, '-Yes')
Check 'in-place: uninstall refuses a folder without a record' ($r.Code -ne 0 -and (Test-Path "$game2\ZH Controller\notes.txt")) $r.Out

# Tampered records.
$game3 = Join-Path $base 'Game3'
New-FakeGame $game3
$r = Run $ps64 $inst @('-GamePath', $game3, '-NoShortcut')
$recFile = "$game3\ZH Controller\ZHController.install.txt"
$good = Get-Content $recFile
($good + 'File=Data\a.txt|0000') | Set-Content $recFile
$r = Run $ps64 $uninst @('-GamePath', $game3, '-Yes')
Check 'in-place: uninstall refuses a record naming a game file' ($r.Code -ne 0 -and (Test-Path "$game3\Data\a.txt") -and (Test-Path "$game3\generalszh.exe")) $r.Out
($good + 'File=ZH Controller\..\INIZH.big|0000') | Set-Content $recFile
$r = Run $ps64 $uninst @('-GamePath', $game3, '-Yes')
Check 'in-place: uninstall refuses a record with ..' ($r.Code -ne 0 -and (Test-Path "$game3\INIZH.big")) $r.Out
($good -replace '^GameRoot=.*', "GameRoot=$game2") | Set-Content $recFile
$r = Run $ps64 $uninst @('-GamePath', $game3, '-Yes')
Check "in-place: uninstall refuses another game's record" ($r.Code -ne 0 -and (Test-Path "$game3\generalszh.exe")) $r.Out
$good | Set-Content $recFile

# A changed file is kept.
Set-Content -LiteralPath "$game3\generalszh.exe" -Value 'changed by the user'
$r = Run $ps64 $uninst @('-GamePath', $game3, '-Yes')
Check 'in-place: uninstall keeps a changed exe, removes the rest' ($r.Code -eq 0 -and (Test-Path "$game3\generalszh.exe") -and -not (Test-Path "$game3\ZH Controller") -and $r.Out -match 'Kept generalszh.exe') $r.Out
Check 'in-place: game files still there after uninstall' ((Test-Path "$game3\INIZH.big") -and (Test-Path "$game3\d3d8.dll") -and (Test-Path "$game3\Data\a.txt"))

# Links.
$game4 = Join-Path $base 'Game4'
New-FakeGame $game4
$alias = Join-Path $base 'Alias4'
cmd /c mklink /J "$alias" "$game4" | Out-Null
$r = Run $ps64 $inst @('-GamePath', $alias, '-NoShortcut')
Check 'in-place: refuses a game path through a junction' ($r.Code -ne 0 -and -not (Test-Path "$game4\generalszh.exe")) $r.Out
$r = Run $ps64 $inst @('-GamePath', $game4, '-NoShortcut')
cmd /c mklink /J "$game4\ZH Controller\Link" "$game" | Out-Null
$r = Run $ps64 $uninst @('-GamePath', $game4, '-Yes')
Check 'in-place: uninstall refuses a junction in its folder' ($r.Code -ne 0 -and (Test-Path "$game\INIZH.big")) $r.Out
cmd /c rmdir "$game4\ZH Controller\Link"

# The 'ZH Controller' folder itself a junction to somewhere else: install, update and uninstall
# refuse, and nothing is written or removed at the far end.
$game5 = Join-Path $base 'Game5'
New-FakeGame $game5
$elsewhere = Join-Path $base 'Elsewhere5'
New-Item -ItemType Directory $elsewhere | Out-Null
Set-Content -LiteralPath "$elsewhere\keep.txt" -Value 'not the mod''s'
cmd /c mklink /J "$game5\ZH Controller" "$elsewhere" | Out-Null
$r = Run $ps64 $inst @('-GamePath', $game5, '-NoShortcut')
Check "in-place: install refuses a 'ZH Controller' junction" ($r.Code -ne 0 -and -not (Test-Path "$game5\generalszh.exe") -and @(Get-ChildItem -LiteralPath $elsewhere).Count -eq 1) $r.Out
cmd /c rmdir "$game5\ZH Controller"
$r = Run $ps64 $inst @('-GamePath', $game5, '-NoShortcut')
Check 'in-place: fixture install for the junction checks' ($r.Code -eq 0) $r.Out
Copy-Item -Recurse -LiteralPath "$game5\ZH Controller\*" -Destination $elsewhere
cmd /c rmdir /s /q "$game5\ZH Controller"
cmd /c mklink /J "$game5\ZH Controller" "$elsewhere" | Out-Null
$farBefore = Get-TreeFingerprint $elsewhere @()
$r = Run $ps64 $inst @('-GamePath', $game5, '-NoShortcut')
Check "in-place: update refuses a 'ZH Controller' junction" ($r.Code -ne 0 -and (Get-TreeFingerprint $elsewhere @()) -eq $farBefore) $r.Out
$r = Run $ps64 $uninst @('-GamePath', $game5, '-Yes')
Check "in-place: uninstall refuses a 'ZH Controller' junction" ($r.Code -ne 0 -and (Get-TreeFingerprint $elsewhere @()) -eq $farBefore -and (Test-Path "$game5\generalszh.exe")) $r.Out
cmd /c rmdir "$game5\ZH Controller"

# The installed Uninstall.cmd, double-clicked (one cmd started in its folder).
$before = Get-TreeFingerprint $game $added
$enter = Join-Path $base 'enter.txt'
Set-Content -LiteralPath $enter -Value ''
$log = Join-Path $base 'uninstall-cmd.log'
Start-Process -FilePath cmd.exe -ArgumentList "/c `"`"$game\ZH Controller\Uninstall.cmd`" -Yes`"" -WorkingDirectory "$game\ZH Controller" `
    -RedirectStandardInput $enter -RedirectStandardOutput $log -NoNewWindow -Wait
$cmdOut = Get-Content -Raw -LiteralPath $log
Check 'in-place: Uninstall.cmd from its folder removes exe and folder' (-not (Test-Path "$game\generalszh.exe") -and -not (Test-Path "$game\ZH Controller") -and $cmdOut -match 'back to normal') $cmdOut
Check 'in-place: game back to exactly how it was' ((Get-TreeFingerprint $game @()) -eq $before)

# EA App layout: the path given (or in the registry) is one level above the game.
$outer = Join-Path $base 'EA Games\Command and Conquer Generals Zero Hour'
New-Item -ItemType Directory $outer | Out-Null
$inner = Join-Path $outer 'Command and Conquer Generals Zero Hour'
New-FakeGame $inner
$r = Run $ps64 $inst @('-GamePath', $outer, '-NoShortcut')
Check 'EA layout: install finds the inner game folder' ($r.Code -eq 0 -and (Test-Path "$inner\generalszh.exe") -and -not (Test-Path "$outer\generalszh.exe")) $r.Out
$r = Run $ps64 $uninst @('-GamePath', $outer, '-Yes')
Check 'EA layout: uninstall with the outer path removes it' ($r.Code -eq 0 -and -not (Test-Path "$inner\generalszh.exe") -and -not (Test-Path "$inner\ZH Controller") -and (Test-Path "$inner\INIZH.big")) $r.Out

# ================= Review F01: every record entry is checked before anything changes ============
function New-InstalledGame([string]$name) {
    $g = Join-Path $base $name
    New-FakeGame $g
    $r = Run $ps64 $inst @('-GamePath', $g, '-NoShortcut')
    if ($r.Code -ne 0) { throw "fixture install failed: $($r.Out)" }
    return $g
}
function Get-Record([string]$g) { return [System.IO.File]::ReadAllLines("$g\ZH Controller\ZHController.install.txt", [System.Text.Encoding]::UTF8) }
function Set-Record([string]$g, [string[]]$lines) { [System.IO.File]::WriteAllLines("$g\ZH Controller\ZHController.install.txt", $lines, (New-Object System.Text.UTF8Encoding $true)) }

$g = New-InstalledGame 'F01'
$victim = Join-Path $base 'outside-victim.txt'
Set-Content -LiteralPath $victim -Value 'not the mod'
$good = Get-Record $g
$exeBefore = (Get-FileHash "$g\generalszh.exe").Hash
Set-Record $g ($good + "File=..\outside-victim.txt|$((Get-FileHash $victim).Hash)")
Set-Content -LiteralPath (Join-Path $pkg 'generalszh.exe') -Value 'fake exe v3'
$r = Run $ps64 $inst @('-GamePath', $g, '-NoShortcut')
Check 'F01: update refuses a record entry outside the game folder' ($r.Code -ne 0 -and (Test-Path $victim)) $r.Out
Check 'F01: ... and changes nothing (exe and record kept)' ((Get-FileHash "$g\generalszh.exe").Hash -eq $exeBefore -and ((Get-Record $g) -join "`n") -eq (($good + "File=..\outside-victim.txt|$((Get-FileHash $victim).Hash)") -join "`n"))
Set-Record $g ($good + "File=Data\a.txt|$((Get-FileHash "$g\Data\a.txt").Hash)")
$r = Run $ps64 $inst @('-GamePath', $g, '-NoShortcut')
Check 'F01: update refuses a record naming a game file' ($r.Code -ne 0 -and (Test-Path "$g\Data\a.txt") -and (Get-FileHash "$g\generalszh.exe").Hash -eq $exeBefore) $r.Out
foreach ($bad in @("File=ZH Controller\README.txt:hidden|$('A' * 64)", "File=C:\Windows\win.ini|$('A' * 64)", "File=ZH Controller\README.txt|1234", 'File=generalszh.exe')) {
    Set-Record $g ($good + $bad)
    $r = Run $ps64 $inst @('-GamePath', $g, '-NoShortcut')
    Check "F01: update refuses '$bad'" ($r.Code -ne 0 -and (Get-FileHash "$g\generalszh.exe").Hash -eq $exeBefore) $r.Out
    $r = Run $ps64 $uninst @('-GamePath', $g, '-Yes')
    Check "F01: uninstall refuses '$bad'" ($r.Code -ne 0 -and (Test-Path "$g\generalszh.exe")) $r.Out
}
Set-Record $g ($good + "File=..\outside-victim.txt|$((Get-FileHash $victim).Hash)")
$r = Run $ps64 $uninst @('-GamePath', $g, '-Yes')
Check 'F01: uninstall refuses a record entry outside the game folder' ($r.Code -ne 0 -and (Test-Path $victim) -and (Test-Path "$g\generalszh.exe")) $r.Out
Set-Record $g $good
# A file a newer version no longer ships is still cleaned up (legitimately retired file).
$pkgB = Join-Path $base 'PackageB'
Copy-Item -LiteralPath $pkg -Destination $pkgB -Recurse
Remove-Item -LiteralPath "$pkgB\RELEASE_NOTES.txt"
$r = Run $ps64 "$pkgB\Install-ZHController.ps1" @('-GamePath', $g, '-NoShortcut')
Check 'F01: a retired mod file is still removed on update' ($r.Code -eq 0 -and -not (Test-Path "$g\ZH Controller\RELEASE_NOTES.txt") -and (Test-Path "$g\ZH Controller\README.txt")) $r.Out
Check 'F01: ... and dropped from the record' (-not ((Get-Record $g) -match 'RELEASE_NOTES'))

# ================= Review F03: a failed update can be retried ====================================
$g = New-InstalledGame 'F03'
$v1 = (Get-FileHash "$g\generalszh.exe").Hash
Set-Content -LiteralPath (Join-Path $pkg 'generalszh.exe') -Value 'fake exe v4'
$lock = [System.IO.File]::Open("$g\generalszh.exe", 'Open', 'Read', 'Read')   # like a running game
try {
    $r = Run $ps64 $inst @('-GamePath', $g, '-NoShortcut')
    Check 'F03: update with the exe in use stops before changing anything' ($r.Code -ne 0 -and $r.Out -match 'in use' -and ((Get-Record $g) -contains 'State=Complete')) $r.Out
} finally { $lock.Dispose() }
Check 'F03: ... the old exe is still the recorded one' ((Get-FileHash "$g\generalszh.exe").Hash -eq $v1 -and ((Get-Record $g) -contains "File=generalszh.exe|$v1"))
$r = Run $ps64 $inst @('-GamePath', $g, '-NoShortcut')
Check 'F03: retry after closing it succeeds' ($r.Code -eq 0 -and (Get-Content "$g\generalszh.exe") -eq 'fake exe v4' -and ((Get-Record $g) -contains 'State=Complete')) $r.Out
# A failure between writes: the exe is copied, then a document cannot be written.
Set-Content -LiteralPath (Join-Path $pkg 'generalszh.exe') -Value 'fake exe v5'
Set-Content -LiteralPath (Join-Path $pkg 'README.txt') -Value 'readme v5'
$lock = [System.IO.File]::Open("$g\ZH Controller\README.txt", 'Open', 'Read', 'Read')
try {
    $r = Run $ps64 $inst @('-GamePath', $g, '-NoShortcut')
} finally { $lock.Dispose() }
$mid = Get-Record $g
Check 'F03: a failure half-way leaves an in-progress record owning old and new' ($r.Code -ne 0 -and $mid -contains 'State=InProgress' -and ($mid -match '^File=generalszh.exe\|').Count -eq 2 -and ($mid -match '^File=ZH Controller\\README.txt\|').Count -eq 2) $r.Out
$r = Run $ps64 $inst @('-GamePath', $g, '-NoShortcut')
Check 'F03: running it again finishes the update' ($r.Code -eq 0 -and (Get-Content "$g\ZH Controller\README.txt") -eq 'readme v5' -and ((Get-Record $g) -contains 'State=Complete') -and ((Get-Record $g) -match '^File=generalszh.exe\|').Count -eq 1) $r.Out
# Uninstall of a half-finished update removes everything the record owns.
Set-Content -LiteralPath (Join-Path $pkg 'generalszh.exe') -Value 'fake exe v6'
Set-Content -LiteralPath (Join-Path $pkg 'README.txt') -Value 'readme v6'
$lock = [System.IO.File]::Open("$g\ZH Controller\README.txt", 'Open', 'Read', 'Read')
try { $r = Run $ps64 $inst @('-GamePath', $g, '-NoShortcut') } finally { $lock.Dispose() }
$r = Run $ps64 $uninst @('-GamePath', $g, '-Yes')
Check 'F03: uninstall after a half-finished update removes it all' ($r.Code -eq 0 -and -not (Test-Path "$g\generalszh.exe") -and -not (Test-Path "$g\ZH Controller") -and (Test-Path "$g\INIZH.big")) $r.Out
Set-Content -LiteralPath (Join-Path $pkg 'README.txt') -Value (Get-Content -Raw "$rel\README.txt")
Copy-Item "$rel\README.txt" $pkg -Force

# ================= Review F04: any folder name survives the record ===============================
foreach ($name in @('Caf' + [char]0xE9, ([string][char]0x0418 + [char]0x0433 + [char]0x0440 + [char]0x0430), ([string][char]0x904A + [char]0x6232), 'Game [x] (y) & more')) {
    $g = Join-Path $base "U-$name"
    New-FakeGame $g
    $before = Get-TreeFingerprint $g @()
    $r = Run $ps64 $inst @('-GamePath', $g, '-NoShortcut')
    Check "F04: install into '$name'" ($r.Code -eq 0 -and (Test-Path -LiteralPath "$g\generalszh.exe")) $r.Out
    Check "F04: record stores '$name' exactly" ((Get-Record $g) -contains "GameRoot=$g")
    $r = Run $ps64 $inst @('-GamePath', $g, '-NoShortcut')
    Check "F04: update in '$name'" ($r.Code -eq 0 -and $r.Out -match 'Updated') $r.Out
    $r = Run $ps64 $uninst @('-GamePath', $g, '-Yes')
    Check "F04: uninstall from '$name' restores it exactly" ($r.Code -eq 0 -and (Get-TreeFingerprint $g @()) -eq $before) $r.Out
}
$zhU = Join-Path $base ('Copy ' + [char]0xE9 + [char]0x0418 + [char]0x904A)
$r = Run $ps64 $inst @('-Copy', '-GamePath', $game, '-InstallPath', $zhU, '-NoShortcut')
Check 'F04: copy install into a non-ASCII folder' ($r.Code -eq 0 -and ([System.IO.File]::ReadAllLines("$zhU\ZHController.install.txt", [System.Text.Encoding]::UTF8) -contains "InstallRoot=$zhU")) $r.Out
$r = Run $ps64 $inst @('-Copy', '-GamePath', $game, '-InstallPath', $zhU, '-NoShortcut')
Check 'F04: copy update in a non-ASCII folder' ($r.Code -eq 0 -and $r.Out -match 'Updated') $r.Out
$r = Run $ps64 $uninst @('-InstallPath', $zhU, '-Yes')
Check 'F04: copy uninstall from a non-ASCII folder' ($r.Code -eq 0 -and -not (Test-Path -LiteralPath $zhU)) $r.Out
# An old ASCII record (from 1.0.0-1.0.2) is still read.
$g = New-InstalledGame 'OldRecord'
[System.IO.File]::WriteAllLines("$g\ZH Controller\ZHController.install.txt", (Get-Record $g), [System.Text.Encoding]::ASCII)
$r = Run $ps64 $inst @('-GamePath', $g, '-NoShortcut')
Check 'F04: an old ASCII record still updates' ($r.Code -eq 0 -and $r.Out -match 'Updated') $r.Out
[System.IO.File]::WriteAllLines("$g\ZH Controller\ZHController.install.txt", (Get-Record $g), [System.Text.Encoding]::ASCII)
$r = Run $ps64 $uninst @('-GamePath', $g, '-Yes')
Check 'F04: an old ASCII record still uninstalls' ($r.Code -eq 0 -and -not (Test-Path "$g\ZH Controller")) $r.Out

# ================= -Copy ===========================================================================
$zh1 = Join-Path $base 'ZH1'
$r = Run $ps64 $inst @('-Copy', '-GamePath', $game, '-InstallPath', $zh1, '-NoShortcut')
Check 'copy: install to a new folder' ($r.Code -eq 0 -and (Test-Path "$zh1\generalszh.exe") -and (Test-Path "$zh1\INIZH.big")) $r.Out
Check 'copy: marker Complete with its own root' ((Get-Content "$zh1\ZHController.install.txt") -contains 'State=Complete' -and (Get-Content "$zh1\ZHController.install.txt") -contains "InstallRoot=$zh1")
$r = Run $ps64 $inst @('-Copy', '-GamePath', $game, '-InstallPath', $zh1, '-NoShortcut')
Check 'copy: second run = update' ($r.Code -eq 0 -and $r.Out -match 'Updated') $r.Out
$zh2 = Join-Path $base 'ZH2'
New-Item -ItemType Directory $zh2 | Out-Null
Set-Content "$zh2\keep.txt" 'keep'
$r = Run $ps64 $inst @('-Copy', '-GamePath', $game, '-InstallPath', $zh2, '-NoShortcut')
Check 'copy: refuses an unmarked non-empty folder' ($r.Code -ne 0 -and -not (Test-Path "$zh2\generalszh.exe")) $r.Out
$r = Run $ps64 $inst @('-Copy', '-GamePath', $game, '-InstallPath', "$game\Sub", '-NoShortcut')
Check 'copy: refuses a folder inside the game' ($r.Code -ne 0 -and -not (Test-Path "$game\Sub")) $r.Out
$alias = Join-Path $base 'Alias'
cmd /c mklink /J "$alias" "$game" | Out-Null
$r = Run $ps64 $inst @('-Copy', '-GamePath', $game, '-InstallPath', "$alias\ControllerCopy", '-NoShortcut')
Check 'copy: refuses a junction path into the game' ($r.Code -ne 0 -and -not (Test-Path "$game\ControllerCopy")) $r.Out
foreach ($bad in 'C:\', 'C:', '\\localhost\c$\ZHCTest') {
    $r = Run $ps64 $inst @('-Copy', '-GamePath', $game, '-InstallPath', $bad, '-NoShortcut')
    Check "copy: refuses $bad" ($r.Code -ne 0) $r.Out
}
$r = Run $ps32 $inst @('-Copy', '-GamePath', $game, '-InstallPath', 'C:\Program Files\ZHControllerTest', '-NoShortcut')
Check 'copy: 32-bit PowerShell refuses C:\Program Files' ($r.Code -ne 0 -and -not (Test-Path 'C:\Program Files\ZHControllerTest')) $r.Out
$r = Run $ps64 $uninst @('-InstallPath', (Join-Path $base 'ZH[12]'), '-Yes')
Check 'copy: uninstall treats ZH[12] literally' ($r.Code -ne 0 -and (Test-Path $zh1) -and (Test-Path $zh2)) $r.Out
foreach ($bad in 'C:\', 'C:') {
    $r = Run $ps64 $uninst @('-InstallPath', $bad, '-Yes')
    Check "copy: uninstall refuses $bad" ($r.Code -ne 0) $r.Out
}
New-Item -ItemType Directory "$zh2\ZHController.install.txt" | Out-Null
$r = Run $ps64 $uninst @('-InstallPath', $zh2, '-Yes')
Check 'copy: uninstall refuses a marker that is a folder' ($r.Code -ne 0 -and (Test-Path "$zh2\keep.txt")) $r.Out
Remove-Item -LiteralPath "$zh2\ZHController.install.txt"
Copy-Item -LiteralPath "$zh1\ZHController.install.txt" -Destination "$zh2\ZHController.install.txt"
$r = Run $ps64 $uninst @('-InstallPath', $zh2, '-Yes')
Check "copy: uninstall refuses another folder's marker" ($r.Code -ne 0 -and (Test-Path "$zh2\keep.txt")) $r.Out
$zh3 = Join-Path $base 'ZH3'
$r = Run $ps64 $inst @('-Copy', '-GamePath', $game, '-InstallPath', $zh3, '-NoShortcut')
(Get-Content "$zh3\ZHController.install.txt") -replace 'State=Complete', 'State=InProgress' | Set-Content "$zh3\ZHController.install.txt"
Remove-Item -LiteralPath "$zh3\Data" -Recurse
$r = Run $ps64 $inst @('-Copy', '-GamePath', $game, '-InstallPath', $zh3, '-NoShortcut')
Check 'copy: rerun finishes a stopped install' ($r.Code -eq 0 -and (Test-Path "$zh3\Data\a.txt")) $r.Out
$r = Run $ps64 "$zh1\ZH Controller docs\Uninstall-ZHController.ps1" @('-Yes')
Check 'copy: docs uninstaller removes its own copy' ($r.Code -eq 0 -and -not (Test-Path $zh1)) $r.Out
Check 'copy: other folders untouched' ((Test-Path "$zh2\keep.txt") -and (Test-Path $zh3) -and (Test-Path "$game\INIZH.big"))

$results
$failed = @($results | Where-Object { $_ -like 'FAIL*' }).Count
Write-Host ("RESULT: {0} passed, {1} failed" -f ($results.Count - $failed), $failed)
if (Test-Path -LiteralPath $base) { cmd /c rmdir /s /q "$base" }
exit $(if ($failed) { 1 } else { 0 })
