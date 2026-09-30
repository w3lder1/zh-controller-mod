<#
    Zero Hour Controller Mod - installer.

    DEFAULT: adds the controller to your Command & Conquer Generals Zero Hour 1.04 folder.
      Added (nothing of the game is replaced, renamed or changed):
        <game folder>\generalszh.exe          the controller game program
        <game folder>\ZH Controller\          readme, controls, uninstaller, and a record of
                                              exactly what was added (ZHController.install.txt)
        a "Zero Hour Controller" shortcut on your desktop
      Your normal game (and GenTool, if you use it) keeps working as before. The game folder is
      usually in Program Files, so Windows asks for permission once.

    -Copy: makes a separate copy of the whole game instead (about 3 GB) and adds the controller
      there; the original game folder is only read. See README.txt.

    Usage (or double-click Install.cmd):
      .\Install-ZHController.ps1                          add to the game folder from the registry
      .\Install-ZHController.ps1 -GamePath <folder>       add to this Zero Hour folder
      .\Install-ZHController.ps1 -NoShortcut              no desktop shortcut
      .\Install-ZHController.ps1 -Copy [-InstallPath D:\ZHC] [-Repair]   the separate copy
    Running it again updates an install. Nothing is written to the registry; nothing is downloaded.

    Safety rules (see ControllerMod/DEVELOPMENT.md, installer): literal paths, full drive-letter paths
    only, no link or junction on the paths used or inside what is removed or overwritten, never
    overwrite a file this installer did not put there, a record of every added file (with its
    checksum) so the uninstaller removes exactly those.
#>
[CmdletBinding()]
param(
    [switch]$Copy,
    [string]$InstallPath = (Join-Path $env:USERPROFILE 'Games\ZH Controller'),
    [string]$GamePath = '',
    [switch]$NoShortcut,
    [switch]$Repair,
    [switch]$ElevatedChild   # internal: the part that writes to the game folder, run with permission
)

$ErrorActionPreference = 'Stop'
$MarkerName = 'ZHController.install.txt'
$ShortcutName = 'Zero Hour Controller.lnk'
$DocsName = 'ZH Controller docs'      # copy mode
$InPlaceDirName = 'ZH Controller'     # in-place mode
$DocFiles = @('README.txt', 'CONTROLS.txt', 'RELEASE_NOTES.txt', 'LICENSE.txt', 'VERSION.txt', 'Uninstall-ZHController.ps1', 'Uninstall.cmd')
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Fail([string]$message) {
    Write-Host ''
    Write-Host "ERROR: $message" -ForegroundColor Red
    Write-Host 'Nothing else was changed.'
    if ($ElevatedChild) { Read-Host 'Press Enter to close this window' | Out-Null }
    exit 1
}

# --- Shared path safety (same rules as the uninstaller) ------------------------------------------

# A full, literal path on a drive letter, without a trailing backslash. Drive roots, relative
# and network paths are refused.
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

# Is $inner the same folder as $outer or inside it? Both must come from Get-SafeFullPath.
function Test-Inside([string]$inner, [string]$outer) {
    return ($inner + '\').StartsWith($outer + '\', [System.StringComparison]::OrdinalIgnoreCase)
}

# Refuse a path if it, or any existing folder above it, is a link or junction.
function Assert-NoLinkOnPath([string]$path, [string]$what) {
    $p = $path
    while ($p) {
        if (Test-Path -LiteralPath $p) {
            if ([System.IO.File]::GetAttributes($p) -band [System.IO.FileAttributes]::ReparsePoint) {
                Fail "The $what goes through a link or junction ($p). Please use a plain folder."
            }
        }
        $p = [System.IO.Path]::GetDirectoryName($p)
    }
}

# The first link or junction inside a folder, without following any (or $null).
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

# Windows and both Program Files folders, whichever PowerShell (32- or 64-bit) runs this.
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

# The record: key=value lines; File= lines are "relative path|SHA-256". $null when there is none;
# fails when there is one that is not for this folder.
function Read-Marker([string]$markerDir, [string]$expectedRoot, [string]$rootKey) {
    $file = Join-Path $markerDir $MarkerName
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { Fail "$file is not a file. This folder was not made by this installer." }
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
        Fail "The record in $markerDir does not belong to this folder. Nothing was changed."
    }
    return $values
}

function Get-Hash([string]$path) {
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}

# --- The package ---------------------------------------------------------------------------------
$exe = Join-Path $Here 'generalszh.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { Fail "generalszh.exe is not next to this script ($Here)." }
$exeHash = Get-Hash $exe
$sumFile = Join-Path $Here 'SHA256.txt'
if (Test-Path -LiteralPath $sumFile -PathType Leaf) {
    $expected = ((Get-Content -LiteralPath $sumFile -TotalCount 1) -split '\s+')[-1]
    if ($expected -and $exeHash -ne $expected) { Fail 'generalszh.exe does not match SHA256.txt (the package is damaged). Unzip it again.' }
}
$version = 'unknown'
$versionFile = Join-Path $Here 'VERSION.txt'
if (Test-Path -LiteralPath $versionFile -PathType Leaf) { $version = (Get-Content -LiteralPath $versionFile -TotalCount 1) }

if (-not $ElevatedChild) { Write-Host 'Zero Hour Controller Mod - install' -ForegroundColor Cyan }

# --- The game ------------------------------------------------------------------------------------
function Test-ZeroHourFolder([string]$path) {
    foreach ($f in @('INIZH.big', 'WindowZH.big', 'MapsZH.big')) {
        if (-not (Test-Path -LiteralPath (Join-Path $path $f) -PathType Leaf)) { return $false }
    }
    return $true
}

# The Zero Hour files are in the given folder or, as in the EA App layout, in a subfolder named
# "Command and Conquer Generals Zero Hour" (some installs' registry points one level up).
function Resolve-ZeroHourFolder([string]$path) {
    if (-not $path -or $path -notmatch '^[A-Za-z]:[\\/]') { return '' }
    try { $full = [System.IO.Path]::GetFullPath($path).TrimEnd('\') } catch { return '' }
    foreach ($candidate in @($full, (Join-Path $full 'Command and Conquer Generals Zero Hour'))) {
        if ((Test-Path -LiteralPath $candidate -PathType Container) -and (Test-ZeroHourFolder $candidate)) { return $candidate }
    }
    return ''
}

$given = $GamePath
$found = ''
if ($given) {
    $found = Resolve-ZeroHourFolder $given
} else {
    foreach ($key in @('HKLM:\SOFTWARE\WOW6432Node\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour',
                       'HKLM:\SOFTWARE\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour')) {
        try {
            $p = (Get-ItemProperty -LiteralPath $key -Name InstallPath -ErrorAction Stop).InstallPath
            if ($p) { $given = $p; $found = Resolve-ZeroHourFolder $p; if ($found) { break } }
        } catch { }
    }
}
# Not found: ask (the normal install is a double-click, so no command line is needed).
while (-not $found -and -not $ElevatedChild) {
    Write-Host ''
    if ($given) { Write-Host "Zero Hour was not found in $given" -ForegroundColor Yellow } else { Write-Host 'Zero Hour was not found automatically.' -ForegroundColor Yellow }
    Write-Host 'Paste the path of your Zero Hour folder (the one with INIZH.big and WindowZH.big in it),'
    Write-Host 'for example: C:\Program Files\EA Games\Command and Conquer Generals Zero Hour\Command and Conquer Generals Zero Hour'
    $answer = Read-Host 'Zero Hour folder (or just Enter to cancel)'
    if ([string]::IsNullOrWhiteSpace($answer)) { Write-Host 'Cancelled. Nothing was changed.'; exit 1 }
    $given = $answer.Trim().Trim('"')
    $found = Resolve-ZeroHourFolder $given
}
if (-not $found) { Fail "$given does not look like a Zero Hour folder (INIZH.big is missing)." }
$GamePath = Get-SafeFullPath $found 'game folder'
Assert-NoLinkOnPath $GamePath 'game folder'
if ($env:windir -and (Test-Inside $GamePath ([System.IO.Path]::GetFullPath($env:windir).TrimEnd('\')))) { Fail 'The game folder cannot be inside Windows.' }

# --- Runtime -------------------------------------------------------------------------------------
function Show-RuntimeWarning {
    $runtime = Join-Path $env:windir 'SysWOW64\vcruntime140.dll'
    if (-not (Test-Path -LiteralPath $runtime)) { $runtime = Join-Path $env:windir 'System32\vcruntime140.dll' }
    if (-not (Test-Path -LiteralPath $runtime)) {
        Write-Host ''
        Write-Host 'WARNING: the Microsoft Visual C++ 2015-2022 Redistributable (x86) seems to be missing.' -ForegroundColor Yellow
        Write-Host 'The game will not start without it. Get "vc_redist.x86.exe" from Microsoft''s website' -ForegroundColor Yellow
        Write-Host '(search: "Visual C++ Redistributable latest supported downloads"), install it, then play.' -ForegroundColor Yellow
    }
}

function New-DesktopShortcut([string]$target, [string]$workDir) {
    if ($NoShortcut) { return }
    try {
        $shell = New-Object -ComObject WScript.Shell
        $link = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) $ShortcutName))
        $link.TargetPath = $target
        $link.WorkingDirectory = $workDir
        $icon = Join-Path $workDir 'GeneralsZH.ico'
        if (Test-Path -LiteralPath $icon) { $link.IconLocation = $icon }
        $link.Description = 'Command & Conquer Generals Zero Hour with the controller mod'
        $link.Save()
        Write-Host "Desktop shortcut: $ShortcutName"
    } catch {
        Write-Host "Could not make a desktop shortcut; start $target instead." -ForegroundColor Yellow
    }
}

# =================================================================================================
# DEFAULT: add to the game folder
# =================================================================================================
if (-not $Copy) {
    $ctrlDir = Join-Path $GamePath $InPlaceDirName
    $exeTarget = Join-Path $GamePath 'generalszh.exe'
    if (-not $ElevatedChild) {
        Write-Host "Your game:      $GamePath"
        Write-Host "Adds:           generalszh.exe and the folder '$InPlaceDirName' (nothing of the game is changed)"
    }

    # What is there now, and is it ours?
    $record = $null
    if (Test-Path -LiteralPath $ctrlDir) {
        if (-not (Test-Path -LiteralPath $ctrlDir -PathType Container)) { Fail "$ctrlDir is a file." }
        $link = Find-LinkInside $ctrlDir
        if ($link) { Fail "There is a link or junction inside $ctrlDir ($link)." }
        if (Test-Path -LiteralPath (Join-Path $ctrlDir 'unins000.exe')) {
            Fail "The controller mod is already installed here with the Setup program. Update it with the new Setup program, or uninstall it in Windows Settings > Apps first."
        }
        $record = Read-Marker $ctrlDir $GamePath 'GameRoot'
        if (-not $record -and [System.IO.Directory]::GetFileSystemEntries($ctrlDir).Length -gt 0) {
            Fail "$ctrlDir already exists and was not made by this installer."
        }
    }
    # The whole old record is checked before anything is changed.
    $recorded = Get-RecordedFiles $record
    if (Test-Path -LiteralPath $exeTarget) {
        if (-not (Test-Path -LiteralPath $exeTarget -PathType Leaf)) { Fail "$exeTarget is not a file." }
        if ([System.IO.File]::GetAttributes($exeTarget) -band [System.IO.FileAttributes]::ReparsePoint) { Fail "$exeTarget is a link." }
        $current = Get-Hash $exeTarget
        if ($current -ne $exeHash -and @($recorded['generalszh.exe']) -notcontains $current) {
            Fail "There is already a different generalszh.exe in your game folder (perhaps another community build). It was not replaced. Rename or move it first if you want this one."
        }
        # In use (the controller game is running)? Then stop before changing anything.
        foreach ($process in (Get-Process -Name generalszh -ErrorAction SilentlyContinue)) {
            try { $running = $process.Path } catch { $running = $null }
            if ($running -and [string]::Equals([System.IO.Path]::GetFullPath($running), $exeTarget, [System.StringComparison]::OrdinalIgnoreCase)) {
                Fail 'The controller game is running. Close it, then run the installer again.'
            }
        }
        try {
            [System.IO.File]::Open($exeTarget, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::None).Dispose()
        } catch {
            Fail 'generalszh.exe in your game folder is in use (is the controller game running?). Close it, then run the installer again.'
        }
    }

    # Writing to the game folder usually needs Windows' permission (Program Files).
    $writable = $true
    try {
        $probe = Join-Path $GamePath ('zhcontroller-write-check-' + [guid]::NewGuid().ToString('N') + '.tmp')
        [System.IO.File]::WriteAllText($probe, '')
        [System.IO.File]::Delete($probe)
    } catch { $writable = $false }

    if (-not $writable) {
        if ($ElevatedChild) { Fail "Even with permission the game folder cannot be written: $GamePath" }
        Write-Host ''
        Write-Host 'Windows will now ask for permission to add the files to the game folder.'
        $shellExe = (Get-Process -Id $PID).Path
        $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -GamePath `"$GamePath`" -NoShortcut -ElevatedChild"
        try {
            $child = Start-Process -FilePath $shellExe -ArgumentList $arguments -Verb RunAs -Wait -PassThru
        } catch {
            Fail 'Windows permission was not given. Nothing was changed.'
        }
        if ($child.ExitCode -ne 0) { Write-Host 'The install did not finish (see the other window). Your game is unchanged.' -ForegroundColor Red; exit 1 }
    } else {
        # --- The actual writes (in this process, or in the child with permission) --------------------
        $plan = [ordered]@{ 'generalszh.exe' = $exe }
        foreach ($f in $DocFiles) {
            $src = Join-Path $Here $f
            if (Test-Path -LiteralPath $src -PathType Leaf) { $plan["$InPlaceDirName\$f"] = $src }
        }
        $lines = @(
            'ZHControllerInstall=1'
            'Mode=InPlace'
            'State=InProgress'
            "GameRoot=$GamePath"
            "Version=$version"
            "Date=$(Get-Date -Format 'yyyy-MM-dd HH:mm')"
            '# Added to the game folder by the Zero Hour Controller Mod installer. The uninstaller removes'
            '# exactly the files below (only if they are unchanged), then this folder.'
        )
        # While the update runs, the record owns both the old and the new version of every file, so
        # a stop at any point (a locked file, a crash) can simply be finished by running it again,
        # and the uninstaller still recognises whatever is there.
        $header = $lines
        foreach ($old in $recorded.Keys) { foreach ($h in $recorded[$old]) { $lines += "File=$old|$h" } }
        foreach ($rel in $plan.Keys) { $lines += "File=$rel|$(Get-Hash $plan[$rel])" }

        [System.IO.Directory]::CreateDirectory($ctrlDir) | Out-Null
        $markerFile = Join-Path $ctrlDir $MarkerName
        try {
            Write-Record $markerFile $lines
        } catch {
            Fail "The install record could not be written: $($_.Exception.Message)"
        }

        foreach ($rel in $plan.Keys) {
            $dest = Join-Path $GamePath $rel
            try {
                Copy-Item -LiteralPath $plan[$rel] -Destination $dest -Force
            } catch {
                Fail "Copying $rel failed: $($_.Exception.Message) Close the controller game if it is running, then run the installer again; it finishes the update."
            }
            if ((Get-Hash $dest) -ne (Get-Hash $plan[$rel])) { Fail "Copying $rel failed. Run the installer again." }
        }
        # Files the new version no longer has, if unchanged.
        foreach ($old in $recorded.Keys) {
            if ($plan.Contains($old)) { continue }
            $path = Join-Path $GamePath $old
            if ((Test-Path -LiteralPath $path -PathType Leaf) -and @($recorded[$old]) -contains (Get-Hash $path)) {
                try { Remove-Item -LiteralPath $path -Force } catch { Fail "Removing the old $old failed: $($_.Exception.Message) Run the installer again." }
            }
        }
        $final = @($header) -replace '^State=InProgress$', 'State=Complete'
        foreach ($rel in $plan.Keys) { $final += "File=$rel|$(Get-Hash $plan[$rel])" }
        try {
            Write-Record $markerFile $final
        } catch {
            Fail "The install record could not be finished: $($_.Exception.Message) Run the installer again."
        }

        if ($ElevatedChild) {
            Write-Host 'Files added to the game folder.' -ForegroundColor Green
            Start-Sleep -Seconds 2
            exit 0
        }
    }

    Show-RuntimeWarning
    New-DesktopShortcut $exeTarget $GamePath
    Write-Host ''
    if ($record) { Write-Host 'Updated.' -ForegroundColor Green } else { Write-Host 'Installed.' -ForegroundColor Green }
    Write-Host "Play: the desktop shortcut, or $exeTarget"
    Write-Host "To remove it again: $ctrlDir\Uninstall.cmd (or delete generalszh.exe and that folder)"
    Write-Host 'Your normal game is unchanged and still starts as before.'
    exit 0
}

# =================================================================================================
# -Copy: a separate copy of the whole game
# =================================================================================================
function Write-CopyMarker([string]$installRoot, [string]$state) {
    $lines = @(
        'ZHControllerInstall=1'
        'Mode=Copy'
        "State=$state"
        "InstallRoot=$installRoot"
        "Version=$version"
        "Source=$GamePath"
        "Date=$(Get-Date -Format 'yyyy-MM-dd HH:mm')"
        '# Made by the Zero Hour Controller Mod installer. The uninstaller only removes folders with this file.'
    )
    Write-Record (Join-Path $installRoot $MarkerName) $lines
}

Write-Host "Your game:      $GamePath (only read)"
$InstallPath = Get-SafeFullPath $InstallPath 'install folder'
Write-Host "Copy to:        $InstallPath"
Assert-NoLinkOnPath $InstallPath 'install folder'
if ((Test-Inside $InstallPath $GamePath) -or (Test-Inside $GamePath $InstallPath)) {
    Fail 'The install folder must not be the game folder, inside it, or around it.'
}
foreach ($protected in (Get-ProtectedFolders)) {
    if ((Test-Inside $InstallPath $protected) -or (Test-Inside $protected $InstallPath)) { Fail "Please install outside $protected (for example the default folder)." }
}
$profileRoot = [System.IO.Path]::GetFullPath($env:USERPROFILE).TrimEnd('\')
if (Test-Inside $profileRoot $InstallPath) { Fail 'The install folder cannot be your user folder or a folder around it.' }

$marker = Read-Marker $InstallPath $InstallPath 'InstallRoot'
$fullCopy = $true
if ($marker) {
    if ($marker['State'] -eq 'Complete' -and -not $Repair) { $fullCopy = $false }
    if ($marker['State'] -ne 'Complete') { Write-Host 'An earlier install stopped half-way; finishing it.' }
} elseif (Test-Path -LiteralPath $InstallPath) {
    if (-not (Test-Path -LiteralPath $InstallPath -PathType Container)) { Fail "$InstallPath is a file." }
    if ([System.IO.Directory]::GetFileSystemEntries($InstallPath).Length -gt 0) {
        Fail "$InstallPath already exists, is not empty, and was not made by this installer. Choose a new or empty folder with -InstallPath."
    }
}
if (Test-Path -LiteralPath $InstallPath) {
    $link = Find-LinkInside $InstallPath
    if ($link) { Fail "There is a link or junction inside the install folder ($link). Nothing was changed." }
}
Show-RuntimeWarning

if ($fullCopy) {
    $size = 0
    foreach ($file in [System.IO.Directory]::GetFiles($GamePath, '*', [System.IO.SearchOption]::AllDirectories)) {
        $size += (New-Object System.IO.FileInfo $file).Length
    }
    $drive = New-Object System.IO.DriveInfo ([System.IO.Path]::GetPathRoot($InstallPath))
    if ($drive.AvailableFreeSpace -lt $size + 300MB) {
        Fail ('Not enough free space on {0}: {1:N1} GB needed, {2:N1} GB free.' -f $drive.Name, (($size + 300MB) / 1GB), ($drive.AvailableFreeSpace / 1GB))
    }
    [System.IO.Directory]::CreateDirectory($InstallPath) | Out-Null
    # Owned from now on: a stopped copy is finished by running the installer again, and the
    # uninstaller accepts it.
    Write-CopyMarker $InstallPath 'InProgress'
    Write-Host ('Copying your game ({0:N1} GB). This takes a few minutes...' -f ($size / 1GB))
    # robocopy only reads the source; /XJ skips junctions. Exit codes below 8 mean success.
    & robocopy $GamePath $InstallPath /E /XJ /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { Fail "Copying the game failed (robocopy code $LASTEXITCODE). Run the installer again to finish the copy." }
    $link = Find-LinkInside $InstallPath
    if ($link) { Fail "The copy contains a link or junction ($link). Please report this." }
}

$target = Join-Path $InstallPath 'generalszh.exe'
Copy-Item -LiteralPath $exe -Destination $target -Force
if ((Get-Hash $target) -ne $exeHash) { Fail 'The copied generalszh.exe is not identical to the package.' }
$docs = Join-Path $InstallPath $DocsName
[System.IO.Directory]::CreateDirectory($docs) | Out-Null
foreach ($f in $DocFiles) {
    $src = Join-Path $Here $f
    if (Test-Path -LiteralPath $src -PathType Leaf) { Copy-Item -LiteralPath $src -Destination (Join-Path $docs $f) -Force }
}
Write-CopyMarker $InstallPath 'Complete'

New-DesktopShortcut $target $InstallPath
Write-Host ''
if ($fullCopy) { Write-Host 'Installed (separate copy).' -ForegroundColor Green } else { Write-Host 'Updated.' -ForegroundColor Green }
Write-Host "Play: the desktop shortcut, or $target"
Write-Host "To uninstall later: $docs\Uninstall.cmd"
Write-Host 'Your original game is unchanged and still starts as before.'
exit 0
