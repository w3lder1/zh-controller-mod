<#
    Tests for the Setup program (ZHController-Setup-<version>.exe), silent and per user
    (/CURRENTUSER: no Windows permission prompt), on fake game folders under
    %TEMP%\ZHControllerSetupTests. Nothing else is touched, except that the desktop shortcut check
    moves an existing "Zero Hour Controller.lnk" aside and puts it back.

    Usage:
      powershell -NoProfile -ExecutionPolicy Bypass -File Test-Setup.ps1 -Version 1.0.3
          [-ReleaseDir C:\dev\ZHController\Release] [-PreviousSetup <older Setup exe>] [-GameCopy <folder>]
    -ReleaseDir must hold ZHController-Setup-<Version>.exe and the unzipped ZHController-<Version> folder.
    -PreviousSetup: also test the upgrade from that earlier Setup release.
    -GameCopy: a disposable COPY of a real Zero Hour folder, for the byte-for-byte check on real
      game files (never the real installation).
    Exit code 0 when every check passes.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$ReleaseDir = 'C:\dev\ZHController\Release',
    [string]$PreviousSetup = '',
    [string]$GameCopy = ''
)
$ErrorActionPreference = 'Stop'
$setup = Join-Path $ReleaseDir "ZHController-Setup-$Version.exe"
$zipDir = Join-Path $ReleaseDir "ZHController-$Version"
foreach ($p in $setup, "$zipDir\generalszh.exe") { if (-not (Test-Path -LiteralPath $p)) { throw "missing $p" } }
$exeHash = (Get-FileHash "$zipDir\generalszh.exe").Hash
$work = Join-Path ([System.IO.Path]::GetTempPath()) 'ZHControllerSetupTests'
if (Test-Path -LiteralPath $work) { cmd /c rmdir /s /q "$work" }
New-Item -ItemType Directory $work | Out-Null
$uninstKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{6E0A3C52-8F1B-4C7D-9B2E-5A4D7C1E9F30}_is1'
$desktopLnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Zero Hour Controller.lnk'
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
# Runs a Setup silently. A refusal shows its message box even when silent: close it ourselves and
# return 99.
function Run-Setup([string]$exe, [string]$dir, [string]$tasks = '', [string]$tag = 'setup') {
    $log = Join-Path $work "$tag.log"
    if (Test-Path $log) { Remove-Item $log }
    $p = Start-Process -FilePath $exe -ArgumentList @('/CURRENTUSER', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/DIR=`"$dir`"", "/TASKS=`"$tasks`"", "/LOG=`"$log`"") -PassThru
    for ($i = 0; $i -lt 240 -and -not $p.HasExited; $i++) {
        Start-Sleep -Milliseconds 250
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'Message box \(OK\)' -Quiet)) {
            Start-Sleep -Milliseconds 500
            Get-Process | Where-Object { $_.Path -like '*\is-*\*.tmp' } | Stop-Process -Force
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
    Start-Process -FilePath $u -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait | Out-Null
    # The uninstaller re-launches itself from TEMP; wait until it is done.
    for ($i = 0; $i -lt 120 -and ((Test-Path -LiteralPath $u) -or (Test-Path -LiteralPath "$ctrlDir\unins000.dat") -or (Get-Process | Where-Object { $_.Name -like '_iu*' })); $i++) { Start-Sleep -Milliseconds 500 }
    for ($i = 0; $i -lt 40 -and (Test-Path -LiteralPath $ctrlDir); $i++) { Start-Sleep -Milliseconds 250 }
    Start-Sleep -Seconds 1
}
function Zip-Install([string]$gameDir) {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File "$zipDir\Install-ZHController.ps1" -GamePath $gameDir -NoShortcut 2>&1 | Out-String
    return @{ Code = $LASTEXITCODE; Out = $out }
}

$lnkBackup = $null
if (Test-Path $desktopLnk) { $lnkBackup = Join-Path $work 'owner-shortcut.lnk'; Move-Item $desktopLnk $lnkBackup -Force }
try {
    $games = @()
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
    Check 'ownership file records the installed exe (F12)' ((Get-Content "$ctrl\ZHController.setup.txt") -contains "ExeSHA256=$exeHash")
    Check 'no zip-installer files' (-not (Test-Path "$ctrl\ZHController.install.txt") -and -not (Test-Path "$ctrl\Uninstall.cmd"))
    Check 'uninstall entry in Apps' ((Get-ItemProperty $uninstKey -ErrorAction SilentlyContinue).DisplayName -like 'Zero Hour Controller Mod*')   # "(Current user)" is added when an all-users install exists
    $lnkTarget = if (Test-Path $desktopLnk) { (New-Object -ComObject WScript.Shell).CreateShortcut($desktopLnk).TargetPath } else { '' }
    Check 'desktop shortcut -> game exe' ($lnkTarget -eq "$game\generalszh.exe")
    $startMenu = Join-Path ([Environment]::GetFolderPath('Programs')) 'Zero Hour Controller'
    Check 'Start menu group' (Test-Path "$startMenu\Zero Hour Controller.lnk")

    # The zip scripts refuse a Setup install.
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File "$zipDir\Install-ZHController.ps1" -GamePath $game -NoShortcut 2>&1 | Out-String
    Check 'zip Install refuses a Setup install' ($LASTEXITCODE -ne 0 -and $out -match 'Setup program')
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File "$zipDir\Uninstall-ZHController.ps1" -GamePath $game -Yes 2>&1 | Out-String
    Check 'zip Uninstall refuses a Setup install' ($LASTEXITCODE -ne 0 -and $out -match 'Settings > Apps')

    # Update over itself.
    Check 'reinstall exit 0' ((Run-Setup $setup $game '' 'reinstall') -eq 0)

    # F12: an exe replaced by someone else after Setup is never overwritten by an update.
    Set-Content "$game\generalszh.exe" 'another community build'
    $foreign = (Get-FileHash "$game\generalszh.exe").Hash
    Check 'F12: update refuses an independently replaced exe' ((Run-Setup $setup $game '' 'f12') -ne 0)
    Check 'F12: ... which stays byte-for-byte unchanged' ((Get-FileHash "$game\generalszh.exe").Hash -eq $foreign)
    # F12: a missing exe is simply installed again.
    Remove-Item "$game\generalszh.exe"
    Check 'F12: update with the exe missing' ((Run-Setup $setup $game '' 'missing') -eq 0 -and (Get-FileHash "$game\generalszh.exe").Hash -eq $exeHash)

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

    # Take over a zip install, also in a folder with non-ASCII characters (F04 across the two).
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

    # A changed exe is kept by the uninstaller.
    Run-Setup $setup $game '' 'changed' | Out-Null
    Add-Content "$game\generalszh.exe" 'x'
    Run-Uninstall $game
    Check 'changed exe kept by uninstall, rest removed' ((Test-Path "$game\generalszh.exe") -and -not (Test-Path $ctrl))
    Remove-Item "$game\generalszh.exe"

    # F12: upgrade from an earlier Setup release (its exe is a known owned version).
    if ($PreviousSetup) {
        Check 'upgrade: previous Setup installs' ((Run-Setup $PreviousSetup $game '' 'previous') -eq 0)
        Check 'upgrade: new Setup replaces the previous exe' ((Run-Setup $setup $game '' 'upgrade') -eq 0 -and (Get-FileHash "$game\generalszh.exe").Hash -eq $exeHash)
        Run-Uninstall $game
        Check 'upgrade: uninstall afterwards' (-not (Test-Path "$game\generalszh.exe") -and -not (Test-Path $ctrl))
    }
    Check 'final: game identical to before' ((Fingerprint $game) -eq $before)
} finally {
    if ($lnkBackup -and (Test-Path $lnkBackup)) { Move-Item $lnkBackup $desktopLnk -Force }
}
if (-not $GameCopy -or $script:fail -eq 0) { cmd /c rmdir /s /q "$work" }
Write-Host "RESULT: $script:pass passed, $script:fail failed"
exit $(if ($script:fail) { 1 } else { 0 })
