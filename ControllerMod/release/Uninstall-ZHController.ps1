<#
    Zero Hour Controller Mod - uninstaller.

    Removes what Install-ZHController.ps1 added, and its desktop shortcut. Your game files, save
    games and game options are never touched.

    Which install:
      - run from "<game folder>\ZH Controller" (its Uninstall.cmd): the controller added to that
        game folder: exactly the files listed in its record, if unchanged, then that folder;
      - run from a copy's "ZH Controller docs" folder: that copy (the whole copy folder);
      - otherwise: the controller added to the game folder found in the registry, or with
        -GamePath <folder>; or a separate copy with -InstallPath <folder>
        (default copy folder: %USERPROFILE%\Games\ZH Controller).
    Add -RemoveControllerSettings to also delete ControllerMod.ini.

    Safety: literal paths, full drive-letter paths only; a record must name that exact folder;
    nothing is removed through a link or junction; a changed or unknown file is kept.
#>
[CmdletBinding()]
param(
    [string]$InstallPath = '',
    [string]$GamePath = '',
    [switch]$RemoveControllerSettings,
    [switch]$Yes,
    [switch]$ElevatedChild   # internal: the removal in the game folder, run with permission
)

$ErrorActionPreference = 'Stop'
$MarkerName = 'ZHController.install.txt'
$ShortcutName = 'Zero Hour Controller.lnk'
$DocsName = 'ZH Controller docs'
$InPlaceDirName = 'ZH Controller'

function Fail([string]$message) {
    Write-Host ''
    Write-Host "ERROR: $message" -ForegroundColor Red
    Write-Host 'Nothing was deleted.'
    if ($ElevatedChild) { Read-Host 'Press Enter to close this window' | Out-Null }
    exit 1
}

# --- Shared path safety (same rules as the installer) --------------------------------------------
function Get-SafeFullPath([string]$path, [string]$what) {
    if ([string]::IsNullOrWhiteSpace($path)) { Fail "No $what was given." }
    if ($path.StartsWith('\\') -or $path.StartsWith('//')) { Fail "The $what must be on a drive letter, not a network path: $path" }
    # Only full paths: "C:" or "C:folder" would silently mean the current folder of drive C.
    if ($path -notmatch '^[A-Za-z]:[\\/]') { Fail "Please give the $what as a full path starting with a drive letter (like C:\Games\ZHC), not: $path" }
    try { $full = [System.IO.Path]::GetFullPath($path) } catch { Fail "The $what is not a valid path: $path" }
    $root = [System.IO.Path]::GetPathRoot($full)
    if ($root -notmatch '^[A-Za-z]:\\$') { Fail "The $what must be on a drive letter: $full" }
    $trimmed = $full.TrimEnd('\')
    if ($trimmed.Length -le 2) { Fail "The $what cannot be a whole drive ($root)." }
    return $trimmed
}

function Test-Inside([string]$inner, [string]$outer) {
    return ($inner + '\').StartsWith($outer + '\', [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-NoLinkOnPath([string]$path, [string]$what) {
    $p = $path
    while ($p) {
        if (Test-Path -LiteralPath $p) {
            if ([System.IO.File]::GetAttributes($p) -band [System.IO.FileAttributes]::ReparsePoint) {
                Fail "The $what goes through a link or junction ($p)."
            }
        }
        $p = [System.IO.Path]::GetDirectoryName($p)
    }
}

function Find-LinkInside([string]$root) {
    $stack = New-Object System.Collections.Stack
    $stack.Push($root)
    while ($stack.Count -gt 0) {
        $dir = $stack.Pop()
        foreach ($entry in [System.IO.Directory]::GetFileSystemEntries($dir)) {
            $attributes = [System.IO.File]::GetAttributes($entry)
            if ($attributes -band [System.IO.FileAttributes]::ReparsePoint) { return $entry }
            if ($attributes -band [System.IO.FileAttributes]::Directory) { $stack.Push($entry) }
        }
    }
    return $null
}

function Get-ProtectedFolders {
    $list = @()
    foreach ($p in @($env:ProgramW6432, $env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:windir)) {
        if ($p) { $list += [System.IO.Path]::GetFullPath($p).TrimEnd('\') }
    }
    return $list | Select-Object -Unique
}

# Every file this mod has ever added to a game folder (all versions). A record entry that is not
# exactly one of these is refused before anything is changed.
$KnownInPlaceFiles = @('generalszh.exe') + (@('README.txt', 'CONTROLS.txt', 'RELEASE_NOTES.txt', 'LICENSE.txt', 'VERSION.txt',
    'Uninstall-ZHController.ps1', 'Uninstall.cmd') | ForEach-Object { "ZH Controller\$_" })

# Records are UTF-8 (with BOM) so any folder name survives; older ASCII records read
# the same way.
function Read-RecordLines([string]$file) {
    return [System.IO.File]::ReadAllLines($file, [System.Text.Encoding]::UTF8)
}

# Writes the record in one step: a new file next to it, then swapped in.
function Write-Record([string]$file, [string[]]$lines) {
    $temp = "$file.new"
    [System.IO.File]::WriteAllLines($temp, $lines, (New-Object System.Text.UTF8Encoding $true))
    if (Test-Path -LiteralPath $file -PathType Leaf) {
        [System.IO.File]::Replace($temp, $file, [NullString]::Value)
    } else {
        [System.IO.File]::Move($temp, $file)
    }
}

# File= entries as relative path -> list of accepted SHA-256 values (an update in progress lists
# both the old and the new one). Fails on anything that is not a known file with a proper hash.
function Get-RecordedFiles($record) {
    $recorded = @{}
    if (-not $record) { return $recorded }
    foreach ($entry in $record.Files) {
        $parts = $entry.Split('|')
        $known = $parts.Count -eq 2 -and ($KnownInPlaceFiles -contains $parts[0]) -and $parts[1] -match '^[0-9A-Fa-f]{64}$'
        if (-not $known) { Fail "The install record lists something this mod never installs: $entry" }
        $rel = ($KnownInPlaceFiles | Where-Object { $_ -eq $parts[0] } | Select-Object -First 1)
        if (-not $recorded.ContainsKey($rel)) { $recorded[$rel] = @() }
        if ($recorded[$rel] -notcontains $parts[1].ToUpperInvariant()) { $recorded[$rel] += $parts[1].ToUpperInvariant() }
    }
    return $recorded
}

function Read-Marker([string]$markerDir, [string]$expectedRoot, [string]$rootKey) {
    $file = Join-Path $markerDir $MarkerName
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { Fail "$file is not a file, so this was not made by this mod." }
    $values = @{ Files = @() }
    foreach ($line in (Read-RecordLines $file)) {
        $i = $line.IndexOf('=')
        if ($i -le 0) { continue }
        $key = $line.Substring(0, $i).Trim()
        $value = $line.Substring($i + 1).Trim()
        if ($key -eq 'File') { $values.Files += $value } else { $values[$key] = $value }
    }
    if ($values['ZHControllerInstall'] -ne '1' -or -not $values[$rootKey] -or
        -not [string]::Equals($values[$rootKey], $expectedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail "The record in $markerDir does not belong to this folder."
    }
    return $values
}

function Get-RegistryGamePath {
    foreach ($key in @('HKLM:\SOFTWARE\WOW6432Node\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour',
                       'HKLM:\SOFTWARE\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour')) {
        try {
            $p = (Get-ItemProperty -LiteralPath $key -Name InstallPath -ErrorAction Stop).InstallPath
            if ($p) { return $p }
        } catch { }
    }
    return ''
}

function Assert-GameNotRunningFrom([string]$folder) {
    foreach ($process in (Get-Process -Name generalszh -ErrorAction SilentlyContinue)) {
        try { $path = $process.Path } catch { $path = $null }
        if ($path -and (Test-Inside ([System.IO.Path]::GetFullPath($path)) $folder)) { Fail 'The controller game is running. Close it first.' }
    }
}

function Remove-ShortcutTo([string]$exePath) {
    $shortcut = Join-Path ([Environment]::GetFolderPath('Desktop')) $ShortcutName
    if (Test-Path -LiteralPath $shortcut -PathType Leaf) {
        try {
            $target = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut).TargetPath
            if ($target -and [string]::Equals([System.IO.Path]::GetFullPath($target), $exePath, [System.StringComparison]::OrdinalIgnoreCase)) {
                Remove-Item -LiteralPath $shortcut -Force
                Write-Host 'Desktop shortcut removed.'
            }
        } catch { }
    }
}

function Remove-Settings {
    if ($RemoveControllerSettings) {
        $ini = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Command and Conquer Generals Zero Hour Data\ControllerMod.ini'
        if (Test-Path -LiteralPath $ini -PathType Leaf) { Remove-Item -LiteralPath $ini -Force; Write-Host 'ControllerMod.ini removed.' }
    }
}

function Confirm-Removal([string]$what) {
    Write-Host "This removes: $what"
    Write-Host 'Kept: your game files, your saves and options (Documents\...\Zero Hour Data).'
    if (-not $Yes) {
        $answer = Read-Host 'Type YES to uninstall'
        if ($answer -ne 'YES') { Write-Host 'Cancelled. Nothing was deleted.'; exit 0 }
    }
}

if (-not $ElevatedChild) { Write-Host 'Zero Hour Controller Mod - uninstall' -ForegroundColor Cyan }

# --- Which install -------------------------------------------------------------------------------
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$mode = ''
if ($PSBoundParameters.ContainsKey('InstallPath')) {
    $mode = 'Copy'
} elseif ($PSBoundParameters.ContainsKey('GamePath')) {
    $mode = 'InPlace'
} elseif ((Split-Path -Leaf $here) -eq $InPlaceDirName -and (Test-Path -LiteralPath (Join-Path $here $MarkerName) -PathType Leaf)) {
    $mode = 'InPlace'; $GamePath = Split-Path -Parent $here
} elseif ((Split-Path -Leaf $here) -eq $DocsName -and (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $here) $MarkerName) -PathType Leaf)) {
    $mode = 'Copy'; $InstallPath = Split-Path -Parent $here
} else {
    $registryGame = Get-RegistryGamePath
    $mode = 'Copy'; $InstallPath = Join-Path $env:USERPROFILE 'Games\ZH Controller'
    if ($registryGame -and $registryGame -match '^[A-Za-z]:[\\/]') {
        # The registry may point at the game folder or one level above it (EA App layout).
        foreach ($candidate in @($registryGame, (Join-Path $registryGame 'Command and Conquer Generals Zero Hour'))) {
            if (Test-Path -LiteralPath (Join-Path (Join-Path $candidate $InPlaceDirName) $MarkerName) -PathType Leaf) {
                $mode = 'InPlace'; $GamePath = $candidate; break
            }
        }
    }
}
# -GamePath one level above the game folder (EA App layout): use the subfolder with the install.
if ($mode -eq 'InPlace' -and $GamePath -match '^[A-Za-z]:[\\/]' -and
    -not (Test-Path -LiteralPath (Join-Path $GamePath $InPlaceDirName)) -and
    (Test-Path -LiteralPath (Join-Path (Join-Path (Join-Path $GamePath 'Command and Conquer Generals Zero Hour') $InPlaceDirName) $MarkerName) -PathType Leaf)) {
    $GamePath = Join-Path $GamePath 'Command and Conquer Generals Zero Hour'
}

# =================================================================================================
# Added to the game folder
# =================================================================================================
if ($mode -eq 'InPlace') {
    $GamePath = Get-SafeFullPath $GamePath 'game folder'
    Assert-NoLinkOnPath $GamePath 'game folder'
    $ctrlDir = Join-Path $GamePath $InPlaceDirName
    if (-not (Test-Path -LiteralPath $ctrlDir -PathType Container)) { Fail "The controller is not installed in $GamePath ($InPlaceDirName is missing)." }
    $link = Find-LinkInside $ctrlDir
    if ($link) { Fail "There is a link or junction inside $ctrlDir ($link)." }
    if (Test-Path -LiteralPath (Join-Path $ctrlDir 'unins000.exe')) {
        Fail "The controller mod was installed here with the Setup program. Uninstall it in Windows Settings > Apps (Zero Hour Controller Mod) instead."
    }
    $record = Read-Marker $ctrlDir $GamePath 'GameRoot'
    if (-not $record -or $record['Mode'] -ne 'InPlace') { Fail "$ctrlDir has no install record, so nothing is removed." }

    # Only files this mod ever adds (the whole record is checked before anything is deleted).
    $recorded = Get-RecordedFiles $record
    $files = @()
    foreach ($rel in $recorded.Keys) {
        $files += @{ Path = (Join-Path $GamePath $rel); Hashes = $recorded[$rel]; Rel = $rel }
    }
    $exePath = Join-Path $GamePath 'generalszh.exe'

    if (-not $ElevatedChild) {
        Assert-GameNotRunningFrom $GamePath
        Confirm-Removal "generalszh.exe and the folder '$InPlaceDirName' from $GamePath"
    }

    $writable = $true
    try {
        $probe = Join-Path $ctrlDir ('zhcontroller-write-check-' + [guid]::NewGuid().ToString('N') + '.tmp')
        [System.IO.File]::WriteAllText($probe, '')
        [System.IO.File]::Delete($probe)
    } catch { $writable = $false }

    if (-not $writable) {
        if ($ElevatedChild) { Fail "Even with permission the game folder cannot be changed: $GamePath" }
        Write-Host 'Windows will now ask for permission to remove the files from the game folder.'
        Set-Location -LiteralPath $env:TEMP
        $shellExe = (Get-Process -Id $PID).Path
        $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -GamePath `"$GamePath`" -Yes -ElevatedChild"
        try {
            $child = Start-Process -FilePath $shellExe -ArgumentList $arguments -Verb RunAs -Wait -PassThru -WorkingDirectory $env:TEMP
        } catch {
            Fail 'Windows permission was not given. Nothing was deleted.'
        }
        if ($child.ExitCode -ne 0) { Write-Host 'The uninstall did not finish (see the other window).' -ForegroundColor Red; exit 1 }
    } else {
        Set-Location -LiteralPath $env:TEMP
        [System.IO.Directory]::SetCurrentDirectory($env:TEMP)
        $kept = @()
        foreach ($f in $files) {
            if (-not (Test-Path -LiteralPath $f.Path)) { continue }
            if (-not (Test-Path -LiteralPath $f.Path -PathType Leaf)) { $kept += $f.Rel; continue }
            if (@($f.Hashes) -notcontains (Get-FileHash -LiteralPath $f.Path -Algorithm SHA256).Hash) { $kept += $f.Rel; continue }
            [System.IO.File]::SetAttributes($f.Path, [System.IO.FileAttributes]::Normal)
            try { [System.IO.File]::Delete($f.Path) } catch { Fail "Could not remove $($f.Rel): $($_.Exception.Message). Close the game and run the uninstaller again." }
        }
        # The record last, then the folder if nothing else is in it.
        [System.IO.File]::Delete((Join-Path $ctrlDir $MarkerName))
        if ([System.IO.Directory]::GetFileSystemEntries($ctrlDir).Length -eq 0) {
            [System.IO.Directory]::Delete($ctrlDir, $false)
        } else {
            Write-Host "Kept $ctrlDir because it has other files in it." -ForegroundColor Yellow
        }
        foreach ($k in $kept) { Write-Host "Kept $k (it was changed after the install)." -ForegroundColor Yellow }
        if ($ElevatedChild) {
            Write-Host 'Files removed from the game folder.' -ForegroundColor Green
            Start-Sleep -Seconds 2
            exit 0
        }
    }

    Remove-ShortcutTo $exePath
    Remove-Settings
    Write-Host 'Uninstalled. Your game is back to normal.' -ForegroundColor Green
    exit 0
}

# =================================================================================================
# A separate copy
# =================================================================================================
$InstallPath = Get-SafeFullPath $InstallPath 'install folder'
Assert-NoLinkOnPath $InstallPath 'install folder'
if (-not (Test-Path -LiteralPath $InstallPath -PathType Container)) { Fail "No controller install found ($InstallPath does not exist). For another folder use -InstallPath or -GamePath." }
$marker = Read-Marker $InstallPath $InstallPath 'InstallRoot'
if (-not $marker) { Fail "$InstallPath is not an install made by this mod ($MarkerName is missing)." }

foreach ($protected in (Get-ProtectedFolders)) {
    if ((Test-Inside $InstallPath $protected) -or (Test-Inside $protected $InstallPath)) { Fail "$InstallPath is inside or around $protected." }
}
$profileRoot = [System.IO.Path]::GetFullPath($env:USERPROFILE).TrimEnd('\')
if (Test-Inside $profileRoot $InstallPath) { Fail "$InstallPath is your user folder or around it." }
$game = Get-RegistryGamePath
if ($game) {
    $game = [System.IO.Path]::GetFullPath($game).TrimEnd('\')
    if ((Test-Inside $InstallPath $game) -or (Test-Inside $game $InstallPath)) { Fail "$InstallPath is the original game folder, inside it, or around it." }
}
$link = Find-LinkInside $InstallPath
if ($link) { Fail "There is a link or junction inside $InstallPath ($link). Please remove the folder by hand." }

$exePath = Join-Path $InstallPath 'generalszh.exe'
Assert-GameNotRunningFrom $InstallPath
Confirm-Removal $InstallPath

# Step out of the folder first (a folder cannot be deleted while it is the current folder).
Set-Location -LiteralPath $env:TEMP
[System.IO.Directory]::SetCurrentDirectory($env:TEMP)
foreach ($file in [System.IO.Directory]::GetFiles($InstallPath, '*', [System.IO.SearchOption]::AllDirectories)) {
    $attributes = [System.IO.File]::GetAttributes($file)
    if ($attributes -band [System.IO.FileAttributes]::ReadOnly) {
        [System.IO.File]::SetAttributes($file, $attributes -band (-bnot [System.IO.FileAttributes]::ReadOnly))
    }
}
# Everything except the marker first, the marker last: if a file is in use and the delete stops,
# the marker is still there and the uninstaller can simply be run again.
$markerFile = Join-Path $InstallPath $MarkerName
try {
    foreach ($entry in [System.IO.Directory]::GetFileSystemEntries($InstallPath)) {
        if ([string]::Equals($entry, $markerFile, [System.StringComparison]::OrdinalIgnoreCase)) { continue }
        if ([System.IO.File]::GetAttributes($entry) -band [System.IO.FileAttributes]::Directory) {
            [System.IO.Directory]::Delete($entry, $true)
        } else {
            [System.IO.File]::Delete($entry)
        }
    }
    [System.IO.File]::Delete($markerFile)
    [System.IO.Directory]::Delete($InstallPath, $false)
} catch {
    Write-Host ''
    Write-Host "ERROR: not everything could be deleted: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host 'Close any program or window using that folder, then run the uninstaller again.'
    exit 1
}

Remove-ShortcutTo $exePath
Remove-Settings
Write-Host 'Uninstalled.' -ForegroundColor Green
exit 0
