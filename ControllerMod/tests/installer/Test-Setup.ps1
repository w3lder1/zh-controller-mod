<#
    Tests for the Setup program, silent and per user (/CURRENTUSER: no Windows permission prompt), on
    fake game folders under %TEMP%\ZHControllerSetupTests.

    By default it builds its own test copy of Setup from ControllerMod\release\ZHController.iss and
    the unzipped package: same program logic, but its own identity (Apps entry, Start menu group,
    shortcut names). A real install of the mod is never seen or touched, and nothing outside %TEMP%
    is changed.

    -RealIdentity tests the released Setup exe itself, with the real identity. It is needed for
    -PreviousSetup (the upgrade from an earlier release). It refuses to run while a per-user install,
    Start menu group or desktop shortcut of the mod exists.

    Usage:
      powershell -NoProfile -ExecutionPolicy Bypass -File Test-Setup.ps1 -Version 1.2.1
          [-ReleaseDir <work folder>\Release] [-RealIdentity [-PreviousSetup <older Setup exe>]] [-GameCopy <folder>]
    -ReleaseDir holds the unzipped ZHController-<Version> folder (and, for -RealIdentity,
      ZHController-Setup-<Version>.exe). Default: Release next to the source folder.
    -GameCopy: a disposable COPY of a real Zero Hour folder, for the byte-for-byte check on real
      game files (never the real installation).
    Exit code 0 when every check passes.
#>
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$ReleaseDir = '',
    [switch]$RealIdentity,
    [string]$PreviousSetup = '',
    [string]$GameCopy = ''
)
$ErrorActionPreference = 'Stop'
if (-not $ReleaseDir) { $ReleaseDir = Join-Path $PSScriptRoot '..\..\..\..\Release' }   # (a default here: $PSScriptRoot is empty in parameter defaults under -File)
if ($PreviousSetup -and -not $RealIdentity) { throw '-PreviousSetup needs -RealIdentity (an earlier release has the real identity).' }
$ReleaseDir = [System.IO.Path]::GetFullPath($ReleaseDir)
$zipDir = Join-Path $ReleaseDir "ZHController-$Version"
if (-not (Test-Path -LiteralPath "$zipDir\generalszh.exe")) { throw "missing $zipDir\generalszh.exe" }
$exeHash = (Get-FileHash "$zipDir\generalszh.exe").Hash
$work = Join-Path ([System.IO.Path]::GetTempPath()) 'ZHControllerSetupTests'
if (Test-Path -LiteralPath $work) { cmd /c rmdir /s /q "$work" }
New-Item -ItemType Directory $work | Out-Null

if ($RealIdentity) {
    $setup = Join-Path $ReleaseDir "ZHController-Setup-$Version.exe"
    if (-not (Test-Path -LiteralPath $setup)) { throw "missing $setup" }
    $appGuid = '6E0A3C52-8F1B-4C7D-9B2E-5A4D7C1E9F30'
    $appName = 'Zero Hour Controller Mod'
    $shortcutName = 'Zero Hour Controller'
} else {
    $appGuid = '0C7E5B19-3D2A-4F86-A1E4-5B9D2C7F3A61'
    $appName = 'ZHController Setup Test'
    $shortcutName = 'ZHController Setup Test'
    $iscc = @((Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'), (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $iscc) { throw 'Inno Setup 6 not found.' }
    $iss = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\release\ZHController.iss'))
    $out = Join-Path $work 'fixture'
    & $iscc /Q "/DAppVersion=$Version" "/DSourceDir=$zipDir" "/DOutputDir=$out" "/DExeHash=$exeHash" "/DAppGuid=$appGuid" "/DAppName=$appName" "/DShortcutName=$shortcutName" $iss | Out-Null
    $setup = Join-Path $out "ZHController-Setup-$Version.exe"
    if (-not (Test-Path -LiteralPath $setup)) { throw 'The test copy of Setup was not built.' }
}
$uninstKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{$appGuid}_is1"
$desktopLnk = Join-Path ([Environment]::GetFolderPath('Desktop')) "$shortcutName.lnk"
$startMenu = Join-Path ([Environment]::GetFolderPath('Programs')) $shortcutName
foreach ($existing in $uninstKey, $desktopLnk, $startMenu) {
    if (Test-Path -LiteralPath $existing) { throw "$existing already exists; these tests would change it. Remove that install first (or use the default test identity)." }
}
$script:pass = 0; $script:fail = 0

function Check([string]$name, [bool]$ok) {
    if ($ok) { $script:pass++; Write-Host "PASS $name" } else { $script:fail++; Write-Host "FAIL $name" -ForegroundColor Red }
}
function New-FakeGame([string]$path) {
    New-Item -ItemType Directory $path | Out-Null
    foreach ($f in 'INIZH.big', 'WindowZH.big', 'MapsZH.big', 'd3d8.dll', 'Generals.exe') { Set-Content -LiteralPath (Join-Path $path $f) -Value "x $f" }
}
function Fingerprint([string]$path) {
    (Get-ChildItem -LiteralPath $path -Recurse -Force | Sort-Object FullName |
        ForEach-Object { '{0}|{1}' -f $_.FullName.Substring($path.Length), $(if ($_.PSIsContainer) { 'D' } else { (Get-FileHash -LiteralPath $_.FullName).Hash }) }) -join "`n"
}
# Every process started by $root, and theirs (Setup runs its wizard as a child process).
function Get-ProcessTree([int]$root) {
    $all = @(Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId)
    $ids = New-Object System.Collections.Generic.List[int]
    $queue = New-Object System.Collections.Generic.Queue[int]
    $queue.Enqueue($root)
    while ($queue.Count) {
        $id = $queue.Dequeue()
        foreach ($c in $all | Where-Object { $_.ParentProcessId -eq $id }) { $ids.Add([int]$c.ProcessId); $queue.Enqueue([int]$c.ProcessId) }
    }
    return $ids
}
# Runs a Setup silently. A refusal shows its message box even when silent: close that Setup (only
# the processes it started) and return 99.
function Run-Setup([string]$exe, [string]$dir, [string]$tasks = '', [string]$tag = 'setup') {
    $log = Join-Path $work "$tag.log"
    if (Test-Path $log) { Remove-Item $log }
    $p = Start-Process -FilePath $exe -ArgumentList @('/CURRENTUSER', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/DIR=`"$dir`"", "/TASKS=`"$tasks`"", "/LOG=`"$log`"") -PassThru
    for ($i = 0; $i -lt 240 -and -not $p.HasExited; $i++) {
        Start-Sleep -Milliseconds 250
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'Message box \(OK\)' -Quiet)) {
            Start-Sleep -Milliseconds 500
            foreach ($id in Get-ProcessTree $p.Id) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
            $p.WaitForExit(10000) | Out-Null
            return 99
        }
    }
    $p.WaitForExit()
    return $p.ExitCode
}
function Run-Uninstall([string]$gameDir) {
    $ctrlDir = Join-Path $gameDir 'ZH Controller'
    $u = Join-Path $ctrlDir 'unins000.exe'
    $p = Start-Process -FilePath $u -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -PassThru
    $p.WaitForExit()
    # The uninstaller re-launches itself from TEMP; wait until its files are gone.
    for ($i = 0; $i -lt 120 -and ((Test-Path -LiteralPath $u) -or (Test-Path -LiteralPath "$ctrlDir\unins000.dat")); $i++) { Start-Sleep -Milliseconds 500 }
    for ($i = 0; $i -lt 40 -and (Test-Path -LiteralPath $ctrlDir); $i++) { Start-Sleep -Milliseconds 250 }
    Start-Sleep -Seconds 1
}
function Zip-Install([string]$gameDir) {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File "$zipDir\Install-ZHController.ps1" -GamePath $gameDir -NoShortcut 2>&1 | Out-String
    return @{ Code = $LASTEXITCODE; Out = $out }
}

try {
    $game = Join-Path $work 'Game'
    if ($GameCopy) {
        if (-not (Test-Path -LiteralPath "$GameCopy\INIZH.big")) { throw "-GameCopy is not a Zero Hour folder: $GameCopy" }
        if (Test-Path -LiteralPath "$GameCopy\generalszh.exe") { throw "-GameCopy already has a generalszh.exe" }
        $game = $GameCopy
    } else {
        New-FakeGame $game
    }
    $ctrl = Join-Path $game 'ZH Controller'
    $before = Fingerprint $game

    # Wrong folder: refused, nothing written.
    $wrong = Join-Path $work 'NotAGame'; New-Item -ItemType Directory $wrong | Out-Null
    Check 'wrong folder refused' ((Run-Setup $setup $wrong '' 'wrongdir') -ne 0)
    Check 'wrong folder: nothing written, no Apps entry' (@(Get-ChildItem $wrong -Force).Count -eq 0 -and -not (Test-Path $uninstKey))

    # EA layout: the folder one above the game installs into the inner game folder.
    $ea = Join-Path $work 'EALayout'
    $eaGame = Join-Path $ea 'Command and Conquer Generals Zero Hour'
    New-Item -ItemType Directory $ea | Out-Null
    New-FakeGame $eaGame
    Check 'EA layout: exit 0' ((Run-Setup $setup $ea '' 'eaparent') -eq 0)
    Check 'EA layout: installed in the inner game folder only' ((Test-Path "$eaGame\generalszh.exe") -and (Test-Path "$eaGame\ZH Controller\unins000.exe") -and @(Get-ChildItem $ea -Force).Count -eq 1)
    Run-Uninstall $eaGame
    Check 'EA layout: uninstalled' (-not (Test-Path "$eaGame\generalszh.exe") -and -not (Test-Path "$eaGame\ZH Controller"))

    # Normal install with a desktop shortcut.
    Check 'install exit 0' ((Run-Setup $setup $game 'desktopicon' 'install') -eq 0)
    Check 'exe added and identical' ((Get-FileHash "$game\generalszh.exe").Hash -eq $exeHash)
    foreach ($f in 'README.txt', 'CONTROLS.txt', 'RELEASE_NOTES.txt', 'LICENSE.txt', 'VERSION.txt', 'unins000.exe', 'unins000.dat', 'ZHController.setup.txt') {
        Check "ZH Controller\$f" (Test-Path (Join-Path $ctrl $f))
    }
    Check 'ownership file records the installed exe' ((Get-Content "$ctrl\ZHController.setup.txt") -contains "ExeSHA256=$exeHash")
    Check 'no zip-installer files' (-not (Test-Path "$ctrl\ZHController.install.txt") -and -not (Test-Path "$ctrl\Uninstall.cmd"))
    Check 'uninstall entry in Apps' ((Get-ItemProperty $uninstKey -ErrorAction SilentlyContinue).DisplayName -like "$appName*")   # "(Current user)" is added when an all-users install exists
    $lnkTarget = if (Test-Path $desktopLnk) { (New-Object -ComObject WScript.Shell).CreateShortcut($desktopLnk).TargetPath } else { '' }
    Check 'desktop shortcut -> game exe' ($lnkTarget -eq "$game\generalszh.exe")
    Check 'Start menu group' (Test-Path "$startMenu\$shortcutName.lnk")

    # The zip scripts refuse a Setup install.
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File "$zipDir\Install-ZHController.ps1" -GamePath $game -NoShortcut 2>&1 | Out-String
    Check 'zip Install refuses a Setup install' ($LASTEXITCODE -ne 0 -and $out -match 'Setup program')
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File "$zipDir\Uninstall-ZHController.ps1" -GamePath $game -Yes 2>&1 | Out-String
    Check 'zip Uninstall refuses a Setup install' ($LASTEXITCODE -ne 0 -and $out -match 'Settings > Apps')

    # Update over itself.
    Check 'reinstall exit 0' ((Run-Setup $setup $game '' 'reinstall') -eq 0)

    # An exe replaced by someone else after Setup is never overwritten by an update.
    Set-Content "$game\generalszh.exe" 'another community build'
    $foreign = (Get-FileHash "$game\generalszh.exe").Hash
    Check 'update refuses an independently replaced exe' ((Run-Setup $setup $game '' 'f12') -ne 0)
    Check '... which stays byte-for-byte unchanged' ((Get-FileHash "$game\generalszh.exe").Hash -eq $foreign)
    # A missing exe is simply installed again.
    Remove-Item "$game\generalszh.exe"
    Check 'update with the exe missing' ((Run-Setup $setup $game '' 'missing') -eq 0 -and (Get-FileHash "$game\generalszh.exe").Hash -eq $exeHash)

    Run-Uninstall $game
    Check 'uninstall: game identical to before' ((Fingerprint $game) -eq $before)
    Check 'uninstall: Apps entry, desktop shortcut and Start menu gone' (-not (Test-Path $uninstKey) -and -not (Test-Path $desktopLnk) -and -not (Test-Path $startMenu))

    # A different generalszh.exe or an unknown ZH Controller folder: refused, untouched.
    Set-Content "$game\generalszh.exe" 'another community build'
    Check 'foreign exe refused' ((Run-Setup $setup $game '' 'foreign') -ne 0)
    Check 'foreign exe unchanged, no ZH Controller folder' ((Get-FileHash "$game\generalszh.exe").Hash -eq $foreign -and -not (Test-Path $ctrl))
    Remove-Item "$game\generalszh.exe"
    New-Item -ItemType Directory $ctrl | Out-Null; Set-Content "$ctrl\mine.txt" 'someone else'
    Check 'foreign ZH Controller folder refused' ((Run-Setup $setup $game '' 'foreigndir') -ne 0)
    Check 'foreign folder untouched, no exe' ((Test-Path "$ctrl\mine.txt") -and -not (Test-Path "$game\generalszh.exe"))
    Remove-Item -Recurse $ctrl

    # A 'ZH Controller' folder that is a junction to somewhere else: refused, nothing written there.
    $far = Join-Path $work 'Elsewhere'
    New-Item -ItemType Directory $far | Out-Null
    Set-Content "$far\keep.txt" 'not the mod''s'
    cmd /c mklink /J "$ctrl" "$far" | Out-Null
    Check "a 'ZH Controller' junction is refused" ((Run-Setup $setup $game '' 'junction') -ne 0)
    Check '... and nothing is written through it' (@(Get-ChildItem $far -Force).Count -eq 1 -and -not (Test-Path "$game\generalszh.exe"))
    cmd /c rmdir "$ctrl"

    # Take over a zip install, also in a folder with non-ASCII characters.
    foreach ($name in @('', ('Caf' + [char]0xE9 + ' ' + [char]0x904A + [char]0x6232))) {
        $g = if ($name) { $x = Join-Path $work $name; New-FakeGame $x; $x } else { $game }
        $b = Fingerprint $g
        $label = if ($name) { " ($name)" } else { '' }
        $r = Zip-Install $g
        Check "take-over$label`: zip install first" ($r.Code -eq 0)
        Check "take-over$label`: Setup exit 0" ((Run-Setup $setup $g '' 'takeover') -eq 0)
        Check "take-over$label`: zip record and scripts gone" (-not (Test-Path -LiteralPath "$g\ZH Controller\ZHController.install.txt") -and -not (Test-Path -LiteralPath "$g\ZH Controller\Uninstall.cmd"))
        Check "take-over$label`: Setup install present" ((Test-Path -LiteralPath "$g\ZH Controller\unins000.exe") -and (Get-FileHash -LiteralPath "$g\generalszh.exe").Hash -eq $exeHash)
        Run-Uninstall $g
        Check "take-over$label`: uninstall restores the folder exactly" ((Fingerprint $g) -eq $b)
    }

    # A zip record that names a file the mod never installs (here the player's own file, with its
    # correct hash): Setup refuses and deletes nothing.
    $r = Zip-Install $game
    Check 'edited record: zip install first' ($r.Code -eq 0)
    Set-Content "$ctrl\my notes.txt" 'the player''s own file'
    $recordFile = "$ctrl\ZHController.install.txt"
    $lines = [System.IO.File]::ReadAllLines($recordFile, [System.Text.Encoding]::UTF8)
    $lines += "File=ZH Controller\my notes.txt|$((Get-FileHash "$ctrl\my notes.txt").Hash)"
    [System.IO.File]::WriteAllLines($recordFile, $lines, (New-Object System.Text.UTF8Encoding $true))
    $zipState = Fingerprint $game
    Check 'edited record: Setup refuses' ((Run-Setup $setup $game '' 'editedrecord') -ne 0)
    Check 'edited record: nothing deleted or changed' ((Fingerprint $game) -eq $zipState)
    [System.IO.File]::WriteAllLines($recordFile, $lines[0..($lines.Count - 2)], (New-Object System.Text.UTF8Encoding $true))
    Remove-Item "$ctrl\my notes.txt"
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File "$ctrl\Uninstall-ZHController.ps1" -GamePath $game -Yes 2>&1 | Out-String
    Check 'edited record: zip uninstall afterwards' ($LASTEXITCODE -eq 0 -and -not (Test-Path $ctrl))

    # A changed exe is kept by the uninstaller.
    Run-Setup $setup $game '' 'changed' | Out-Null
    Add-Content "$game\generalszh.exe" 'x'
    Run-Uninstall $game
    Check 'changed exe kept by uninstall, rest removed' ((Test-Path "$game\generalszh.exe") -and -not (Test-Path $ctrl))
    Remove-Item "$game\generalszh.exe"

    # Upgrade from an earlier Setup release (its exe is a known owned version).
    if ($PreviousSetup) {
        Check 'upgrade: previous Setup installs' ((Run-Setup $PreviousSetup $game '' 'previous') -eq 0)
        Check 'upgrade: new Setup replaces the previous exe' ((Run-Setup $setup $game '' 'upgrade') -eq 0 -and (Get-FileHash "$game\generalszh.exe").Hash -eq $exeHash)
        Run-Uninstall $game
        Check 'upgrade: uninstall afterwards' (-not (Test-Path "$game\generalszh.exe") -and -not (Test-Path $ctrl))
    }
    Check 'final: game identical to before' ((Fingerprint $game) -eq $before)
} finally {
    # Whatever a failed run left of this identity (never another one).
    foreach ($left in $desktopLnk, $startMenu) { if (Test-Path -LiteralPath $left) { cmd /c rmdir /s /q "$left" 2>$null; if (Test-Path -LiteralPath $left) { [System.IO.File]::Delete($left) } } }
    if (Test-Path -LiteralPath $uninstKey) { Write-Host "Left behind: $uninstKey (uninstall it in Settings > Apps: $appName)" -ForegroundColor Yellow }
}
if (-not $GameCopy -or $script:fail -eq 0) { cmd /c rmdir /s /q "$work" }
Write-Host "RESULT: $script:pass passed, $script:fail failed"
exit $(if ($script:fail) { 1 } else { 0 })
