<#
    Builds the PLAYER version of the controller mod and assembles the offline release package.

      - Separate build folder (build\player), so the test build in build\win32 is untouched.
      - CONTROLLERMOD_TEST_INPUT=OFF: no environment variable can drive the controller.
      - The package holds exactly the files in $Manifest below: files made by this project, no game
        files. The installer copies the player's own game on their own PC.
      - Output: <work folder>\Release\ZHController-Setup-<version>.exe (the Setup program, built
        with Inno Setup from ControllerMod\release\ZHController.iss) plus the manual package
        ZHController-<version>\ and ZHController-<version>.zip.
        Nothing is uploaded or published anywhere.

    Safety: the version must be digits and dots;
    all work happens in a new staging folder; the staged folder and the zip are compared with the
    manifest; an existing package is only replaced after everything passed, and is kept (renamed),
    never deleted.

      - A release is built from a commit: uncommitted changes to tracked files stop it (the package
        must match one fixed version), unless -AllowDirty is given for a trial build. VERSION.txt
        names the public release tag; the private build commit goes only into a local build-info
        file next to the package.

    Paths: the repository is two folders above this script; the work folder (logs, Release) is the
    folder around the repository. $env:ZHC_VCVARS can point at another vcvars32.bat.

    Usage: powershell -File package.ps1 -Version 1.0.0 [-AllowDirty]     (or package.cmd 1.0.0)
#>
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Version, [switch]$AllowDirty)

$ErrorActionPreference = 'Stop'
$Src = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$Root = [System.IO.Path]::GetFullPath((Join-Path $Src '..'))
$Logs = Join-Path $Root 'logs'
$ReleaseRoot = Join-Path $Root 'Release'
$VcVars = if ($env:ZHC_VCVARS) { $env:ZHC_VCVARS } else { 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat' }

function Fail([string]$message) {
    Write-Host "ERROR: $message" -ForegroundColor Red
    exit 1
}

# --- A version that can only be a folder name ---------------------------------------------------
if ($Version -notmatch '^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$') { Fail "The version must look like 1.0.0 (digits and dots only), not: $Version" }
$Name = "ZHController-$Version"

# --- Inputs, all checked before anything is built or changed -------------------------------------
$Release = Join-Path $Src 'ControllerMod\release'
# Package file name -> source (the exe and pdb are checked after the build).
$Manifest = [ordered]@{
    'generalszh.exe'                   = (Join-Path $Src 'build\player\GeneralsMD\Release\generalszh.exe')
    'Install-ZHController.ps1'         = (Join-Path $Release 'Install-ZHController.ps1')
    'Uninstall-ZHController.ps1'       = (Join-Path $Release 'Uninstall-ZHController.ps1')
    'Install.cmd'                      = (Join-Path $Release 'Install.cmd')
    'Uninstall.cmd'                    = (Join-Path $Release 'Uninstall.cmd')
    'README.txt'                       = (Join-Path $Release 'README.txt')
    'CONTROLS.txt'                     = (Join-Path $Release 'CONTROLS.txt')
    'RELEASE_NOTES.txt'                = (Join-Path $Release 'RELEASE_NOTES.txt')
    'LICENSE.txt'                      = (Join-Path $Src 'LICENSE.md')
}
$Generated = @('VERSION.txt', 'SHA256.txt')
foreach ($entry in $Manifest.GetEnumerator()) {
    if ($entry.Key -like 'generalszh.*') { continue }
    if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) { Fail "Missing package input: $($entry.Value)" }
}
if (-not (Test-Path -LiteralPath $VcVars -PathType Leaf)) { Fail "Visual Studio 2022 Build Tools not found: $VcVars" }
$SetupScript = Join-Path $Release 'ZHController.iss'
if (-not (Test-Path -LiteralPath $SetupScript -PathType Leaf)) { Fail "Missing package input: $SetupScript" }
$Iscc = @((Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'), (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'), (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')) |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $Iscc) { Fail 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup).' }
Push-Location -LiteralPath $Src
try {
    $dirtyTree = (& git status --porcelain --untracked-files=no)
    $commit = (& git rev-parse HEAD).Trim()
} finally {
    Pop-Location
}
$versionHeader = Join-Path $Src 'Core\GameEngine\Include\Common\ControllerModVersion.h'
if (-not (Select-String -LiteralPath $versionHeader -SimpleMatch -Pattern ('#define ZH_CONTROLLER_MOD_VERSION "' + $Version + '"') -Quiet)) {
    Fail "The package version $Version does not match ZH_CONTROLLER_MOD_VERSION in $versionHeader."
}
if ($dirtyTree -and -not $AllowDirty) { Fail "The repository has uncommitted changes; commit them first (or -AllowDirty for a trial build):`n$($dirtyTree -join "`n")" }
[System.IO.Directory]::CreateDirectory($Logs) | Out-Null
[System.IO.Directory]::CreateDirectory($ReleaseRoot) | Out-Null

# --- Build ---------------------------------------------------------------------------------------
Write-Host '=== Configure and build (player build, test input OFF) ==='
$configure = "cmake --preset win32 -B build\player -DRTS_BUILD_GENERALS=OFF -DRTS_BUILD_CORE_TOOLS=OFF -DRTS_BUILD_ZEROHOUR_TOOLS=OFF -DCONTROLLERMOD_TEST_INPUT=OFF -DCONTROLLERMOD_RELEASE_PATHS=ON `"-DRTS_INSTALL_PREFIX_ZEROHOUR=$ReleaseRoot\install-unused`""
$build = 'cmake --build build\player --config Release --target z_generals'
Push-Location -LiteralPath $Src
try {
    & cmd /c "call `"$VcVars`" >nul && $configure > `"$Logs\package-configure.log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Fail "Configure failed, see $Logs\package-configure.log" }
    & cmd /c "call `"$VcVars`" >nul && $build > `"$Logs\package-build.log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Fail "Build failed, see $Logs\package-build.log" }
    $cache = Get-Content -LiteralPath (Join-Path $Src 'build\player\CMakeCache.txt')
    if (-not ($cache -match '^CONTROLLERMOD_TEST_INPUT:BOOL=OFF$')) { Fail 'The player build does not have CONTROLLERMOD_TEST_INPUT=OFF.' }
    $dirty = (& git status --porcelain --untracked-files=no)
} finally {
    Pop-Location
}

# --- Stage in a new folder -----------------------------------------------------------------------
$Staging = Join-Path $ReleaseRoot ('.staging-' + [guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $Staging) { Fail "Staging folder already exists: $Staging" }
$Stage = Join-Path $Staging $Name
[System.IO.Directory]::CreateDirectory($Stage) | Out-Null

foreach ($entry in $Manifest.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) { Fail "Missing package input: $($entry.Value)" }
    $dest = Join-Path $Stage $entry.Key
    [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($dest)) | Out-Null
    Copy-Item -LiteralPath $entry.Value -Destination $dest
    if ((Get-FileHash -LiteralPath $dest).Hash -ne (Get-FileHash -LiteralPath $entry.Value).Hash) { Fail "Copy check failed: $($entry.Key)" }
}
$hash = (Get-FileHash -LiteralPath (Join-Path $Stage 'generalszh.exe') -Algorithm SHA256).Hash
# Times are UTC everywhere in the package (VERSION.txt, file dates in the zip and in Setup), so the
# download carries no time zone.
$now = [DateTime]::UtcNow
$stampUtc = New-Object DateTime ($now.Year, $now.Month, $now.Day, $now.Hour, $now.Minute, 0, [DateTimeKind]::Utc)
$versionLines = @(
    "Zero Hour Controller Mod $Version"
    "Source: https://github.com/w3lder1/zh-controller-mod/releases/tag/v$Version"
    "Built on the Community Patch (GeneralsGameCode) by TheSuperHackers; original game and source code by Electronic Arts"
    "Built $($stampUtc.ToString('yyyy-MM-dd HH:mm')) UTC with Visual Studio 2022, 32-bit Release, test input OFF"
)
Set-Content -LiteralPath (Join-Path $Stage 'VERSION.txt') -Value $versionLines -Encoding ascii
Set-Content -LiteralPath (Join-Path $Stage 'SHA256.txt') -Value "generalszh.exe  $hash" -Encoding ascii
Get-ChildItem -LiteralPath $Stage -Recurse -File | ForEach-Object { [System.IO.File]::SetLastWriteTimeUtc($_.FullName, $stampUtc) }

# --- The staged folder must hold exactly the manifest -------------------------------------------
$expected = @($Manifest.Keys) + $Generated | Sort-Object
$actual = Get-ChildItem -LiteralPath $Stage -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($Stage.Length + 1) } | Sort-Object
if (Compare-Object $expected $actual) { Fail "The staged package does not match the manifest:`n$((Compare-Object $expected $actual | Out-String))" }

$zip = Join-Path $Staging "$Name.zip"
# The zip is written entry by entry so each entry's date is the UTC build time (Compress-Archive
# would store local times).
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipStream = [System.IO.File]::Open($zip, [System.IO.FileMode]::CreateNew)
$archiveOut = New-Object System.IO.Compression.ZipArchive ($zipStream, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($rel in ($expected | Sort-Object)) {
        $entryOut = $archiveOut.CreateEntry(("$Name/" + $rel.Replace('\', '/')), [System.IO.Compression.CompressionLevel]::Optimal)
        $entryOut.LastWriteTime = New-Object DateTimeOffset ($stampUtc, [TimeSpan]::Zero)
        $inStream = [System.IO.File]::OpenRead((Join-Path $Stage $rel))
        $outStream = $entryOut.Open()
        try { $inStream.CopyTo($outStream) } finally { $outStream.Dispose(); $inStream.Dispose() }
    }
} finally {
    $archiveOut.Dispose()
    $zipStream.Dispose()
}
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    $zipped = $archive.Entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName.Replace('/', '\') } | Sort-Object
} finally {
    $archive.Dispose()
}
$expectedZip = $expected | ForEach-Object { "$Name\$_" } | Sort-Object
if (Compare-Object $expectedZip $zipped) { Fail "The zip does not match the manifest:`n$((Compare-Object $expectedZip $zipped | Out-String))" }

# --- The Setup program, from the same checked staged folder --------------------------------------
$SetupName = "ZHController-Setup-$Version.exe"
& $Iscc /Q "/DAppVersion=$Version" "/DSourceDir=$Stage" "/DOutputDir=$Staging" "/DExeHash=$hash" $SetupScript > "$Logs\package-setup.log" 2>&1
if ($LASTEXITCODE -ne 0) { Fail "Building the Setup program failed, see $Logs\package-setup.log" }
$setup = Join-Path $Staging $SetupName
if (-not (Test-Path -LiteralPath $setup -PathType Leaf)) { Fail "The Setup program was not made: $setup" }

# --- Put it in place (an older package is kept, renamed) -----------------------------------------
$finalDir = Join-Path $ReleaseRoot $Name
$finalZip = Join-Path $ReleaseRoot "$Name.zip"
$finalSetup = Join-Path $ReleaseRoot $SetupName
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if (Test-Path -LiteralPath $finalSetup) { Rename-Item -LiteralPath $finalSetup -NewName "ZHController-Setup-$Version.previous-$stamp.exe" }
if (Test-Path -LiteralPath $finalDir) { Rename-Item -LiteralPath $finalDir -NewName "$Name.previous-$stamp" }
if (Test-Path -LiteralPath $finalZip) { Rename-Item -LiteralPath $finalZip -NewName "$Name.previous-$stamp.zip" }
Move-Item -LiteralPath $Stage -Destination $finalDir
Move-Item -LiteralPath $zip -Destination $finalZip
Move-Item -LiteralPath $setup -Destination $finalSetup
# Kept here, not in the download: the build's private commit and local time, and the debug symbols
# (to read crash reports).
Set-Content -LiteralPath (Join-Path $ReleaseRoot "$Name.buildinfo.txt") -Encoding ascii -Value @(
    "Package $Name built $(Get-Date -Format 'yyyy-MM-dd HH:mm zzz') (local), $($stampUtc.ToString('yyyy-MM-dd HH:mm')) UTC"
    "Private build commit $commit$(if ($dirty) { ' plus uncommitted changes' })"
    "exe SHA-256 $hash"
)
$pdb = Join-Path $Src 'build\player\GeneralsMD\Release\generalszh.pdb'
if (Test-Path -LiteralPath $pdb -PathType Leaf) { Copy-Item -LiteralPath $pdb -Destination (Join-Path $ReleaseRoot "$Name.pdb") -Force }
[System.IO.Directory]::Delete($Staging)   # now empty; not recursive

Write-Host "exe SHA-256 $hash"
Write-Host "Setup SHA-256 $((Get-FileHash -LiteralPath $finalSetup -Algorithm SHA256).Hash)"
Write-Host "=== Package OK: $finalSetup and $finalZip ===" -ForegroundColor Green
exit 0
