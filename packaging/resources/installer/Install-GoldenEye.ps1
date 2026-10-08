<#
    GoldenEye 007 PC - Setup

    Started by "Setup GoldenEye 007.exe" through a hidden Windows PowerShell
    host. Setup carries this script and the PC runtime inside itself and
    unpacks them to a temporary folder first (-PayloadDir); the release folder
    is the one holding Setup (-ReleaseRoot). Installs the PC runtime plus the
    game data from the player's own Xbox 360 package into ReleaseRoot\Game.

    Release folder before Setup:  Setup GoldenEye 007.exe, README.txt, licenses\
    After Setup, also:            Game\ (GoldenEye 007.exe, resources\,
                                  release-manifest.json, assets\, ...)

    No original game content ships with the release. Everything that ends up in
    Game\assets comes from the package the player selects, and that package is
    only ever opened for reading.

      -PackagePath <file>   preselect a package (the window still opens)
      -Unattended           install -PackagePath without a window; exit code 0/1
      -Extras a,b           with -Unattended: extras to switch on (fullscreen, complete, shortcut)
      -ShortcutFolder <dir> testing: put the desktop shortcut here instead
      -ReleaseRoot <dir>    the release folder (set by Setup)
      -PayloadDir <dir>     the unpacked payload: installer\ and game\ (set by Setup)
#>
[CmdletBinding()]
param(
    [string]$PackagePath,
    [switch]$Unattended,
    [string[]]$Extras = @(),
    [string]$ShortcutFolder,
    [string]$ReleaseRoot,
    [string]$PayloadDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

# ------------------------------------------------------------------ paths --
$installerDir = $PSScriptRoot
if (-not $PayloadDir) { $PayloadDir = Split-Path -Parent $installerDir }
$runtimeDir = Join-Path $PayloadDir 'game'
if (-not $ReleaseRoot) { $ReleaseRoot = Split-Path -Parent $PayloadDir }
$releaseRoot = [System.IO.Path]::GetFullPath($ReleaseRoot)
$extractorPath = Join-Path $installerDir 'Extract-STFS.ps1'
$setupExe = Join-Path $releaseRoot 'Setup GoldenEye 007.exe'
$gameDir = Join-Path $releaseRoot 'Game'
$gameExeName = 'GoldenEye 007.exe'
$gameExe = Join-Path $gameDir $gameExeName
$manifestName = 'release-manifest.json'
$assetsDir = Join-Path $gameDir 'assets'
$stagingDir = Join-Path $gameDir '.setup-import'
$previousAssetsDir = Join-Path $gameDir '.setup-previous-assets'
$progressFile = Join-Path $gameDir '.setup-progress'
# Logs go to Game\logs. Until the Game folder exists (a first install) they are
# written to a temporary folder and moved there once the install succeeds.
$logName = 'setup-{0}.log' -f (Get-Date -Format 'yyyyMMdd-HHmmss')
$logDir = if (Test-Path -LiteralPath $gameDir -PathType Container) { Join-Path $gameDir 'logs' }
    else { Join-Path $env:TEMP 'GoldenEye 007 Setup logs' }
$logFile = Join-Path $logDir $logName
# Files from releases before 0.4.41 (game program beside Setup, runtime in
# resources\): recognised by name and contents, and tidied away on install.
$legacyLauncher = Join-Path $releaseRoot 'GoldenEye 007.exe'
$legacyResources = Join-Path $releaseRoot 'resources'
$legacyManifest = Join-Path $releaseRoot $manifestName
$legacyLogDir = Join-Path $releaseRoot "logs"
$powershellExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

$versionFile = Join-Path $installerDir 'version.txt'
$releaseVersion = 'dev'
if (Test-Path -LiteralPath $versionFile -PathType Leaf) {
    $releaseVersion = (Get-Content -LiteralPath $versionFile -TotalCount 1).Trim()
}

# The port is a static recompilation of one specific build of the game
# program: its code is compiled into GoldenEye 007.exe, and the package supplies
# the data that build expects. Only a package carrying that exact build works.
$expectedTitleId = [uint32]0x584108A9
$expectedContentType = [uint32]0x000D0000
$expectedXexSha256 = '00B9197180CB044142DD78F2591FF721B1884F1B87ACA5B566A325770054B8FA'
$requiredGameFiles = @('default.xex', 'ArcadeInfo.xml', 'music.xwb', 'sfx.xwb', 'files\loc\english\title\default.str')

# Built from code points so this file stays plain ASCII for PowerShell 5.1.
$dot = [string][char]0x00B7
$ellipsis = [string][char]0x2026
$check = [string][char]0x2713

# ---------------------------------------------------------------- logging --
function Write-Log {
    param([string]$Message)
    try {
        if (-not (Test-Path -LiteralPath $logDir)) {
            New-Item -ItemType Directory -Path $logDir -Force | Out-Null
        }
        $line = '{0}  {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'), $Message
        [System.IO.File]::AppendAllText($logFile, $line + [Environment]::NewLine)
    }
    catch {
        # Logging must never be the reason Setup fails.
    }
}

# ------------------------------------------------------ package inspection --
function Read-UInt32BE {
    param([byte[]]$Bytes, [int]$Offset)
    return ([uint32]$Bytes[$Offset] -shl 24) -bor ([uint32]$Bytes[$Offset + 1] -shl 16) -bor
        ([uint32]$Bytes[$Offset + 2] -shl 8) -bor [uint32]$Bytes[$Offset + 3]
}

# Reads only the STFS header: magic, content type, title ID and display name.
function Get-PackageInfo {
    param([string]$Path)

    $info = @{ Ok = $false; Message = ''; Name = ''; Bytes = [long]0 }
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        $info.Message = 'The selected file could not be found.'
        return $info
    }

    try {
        $info.Bytes = (Get-Item -LiteralPath $Path).Length
        if ($info.Bytes -lt 0xC000) {
            $info.Message = 'This file is too small to be an Xbox 360 package.'
            return $info
        }

        $header = New-Object byte[] 0x1000
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
        try {
            [void]$stream.Read($header, 0, $header.Length)
        }
        finally {
            $stream.Dispose()
        }
    }
    catch {
        $info.Message = 'The package could not be read: ' + $_.Exception.Message
        return $info
    }

    $magic = [System.Text.Encoding]::ASCII.GetString($header, 0, 4)
    if (@('LIVE', 'PIRS', 'CON') -notcontains $magic.TrimEnd()) {
        $info.Message = 'This is not an Xbox 360 package. Choose the GoldenEye 007 package file itself.'
        return $info
    }

    $contentType = Read-UInt32BE $header 0x344
    $titleId = Read-UInt32BE $header 0x360
    if ($titleId -ne $expectedTitleId) {
        $info.Message = ('This package is for a different game (title ID {0:X8}). GoldenEye 007 is 584108A9.' -f $titleId)
        return $info
    }
    if ($contentType -ne $expectedContentType) {
        $info.Message = 'This GoldenEye 007 package is not the game itself (it may be a title update, save or theme). Choose the Xbox LIVE Arcade game package.'
        return $info
    }

    $info.Name = [System.Text.Encoding]::BigEndianUnicode.GetString($header, 0x411, 0x80).Trim([char]0).Trim()
    $info.Ok = $true
    return $info
}

# -------------------------------------------------------------- install ---
function Remove-Tree {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Test-GameRunning {
    $running = @(Get-Process -Name 'GoldenEye 007', 'GoldenEye' -ErrorAction SilentlyContinue | Where-Object {
            try { $_.Path -and $_.Path.StartsWith($gameDir, [StringComparison]::OrdinalIgnoreCase) }
            catch { $false }
        })
    return $running.Count -gt 0
}

# The extractor rewrites "doneBytes|totalBytes|doneFiles|totalFiles".
function Read-ExtractorProgress {
    if (-not (Test-Path -LiteralPath $progressFile -PathType Leaf)) {
        return $null
    }
    try {
        $share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
        $stream = [System.IO.File]::Open($progressFile, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, $share)
        try {
            $text = (New-Object System.IO.StreamReader($stream)).ReadToEnd()
        }
        finally {
            $stream.Dispose()
        }
        $parts = $text.Split('|')
        if ($parts.Count -ne 4) {
            return $null
        }
        return @{
            Done = [long]$parts[0]; Total = [long]$parts[1]
            Files = [int]$parts[2]; TotalFiles = [int]$parts[3]
        }
    }
    catch {
        # Mid-rewrite; the next poll will get it.
        return $null
    }
}

# ---------------------------------------------------- owned-file cleanup --
# Game\release-manifest.json lists every file Setup put in Game\ (paths
# relative to Game). Files a previous install listed that the new runtime no
# longer has are removed; nothing else in Game\ is touched.
function Get-ManifestPaths([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return @() }
    try {
        $data = [System.IO.File]::ReadAllText($Path) | ConvertFrom-Json
        return @($data.files | ForEach-Object { [string]$_.path })
    }
    catch {
        return @()
    }
}

function Remove-EmptyFolders([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return }
    foreach ($dir in @(Get-ChildItem -LiteralPath $Path -Directory -Recurse -Force | Sort-Object { $_.FullName.Length } -Descending)) {
        if (-not (Get-ChildItem -LiteralPath $dir.FullName -Force)) { Remove-Item -LiteralPath $dir.FullName -Force }
    }
    if (-not (Get-ChildItem -LiteralPath $Path -Force)) { Remove-Item -LiteralPath $Path -Force }
}

# A file of ours from an older release: same name as one the release carried.
function Remove-OwnedFile([string]$Path, [string]$Why) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        Remove-Item -LiteralPath $Path -Force
        Write-Log ('Removed {0} ({1})' -f $Path, $Why)
    }
}

# Releases before 0.4.41 kept the play launcher, release-manifest.json,
# resources\installer and resources\game beside Setup, the game program in
# Game as GoldenEye.exe, and Setup logs in logs\. Each is identified before it
# is removed: the launcher by its version info, the manifest by what it lists,
# the resources folders by the exact file names that release shipped.
function Remove-LegacyLayout {
    if (Test-Path -LiteralPath $legacyLauncher -PathType Leaf) {
        $info = (Get-Item -LiteralPath $legacyLauncher).VersionInfo
        if ($info.FileDescription -eq 'GoldenEye 007 PC' -and $info.OriginalFilename -eq 'GoldenEye 007.exe') {
            Remove-OwnedFile $legacyLauncher 'old play launcher'
        }
    }
    if (Test-Path -LiteralPath $legacyManifest -PathType Leaf) {
        $listed = Get-ManifestPaths $legacyManifest
        if ($listed -contains 'resources\installer\Install-GoldenEye.ps1') {
            Remove-OwnedFile $legacyManifest 'old release manifest'
        }
    }
    $oldInstaller = Join-Path $legacyResources 'installer'
    foreach ($name in @('Install-GoldenEye.ps1', 'Extract-STFS.ps1', 'Patch-MenuText.ps1', 'player-names.txt',
            'version.txt', 'GoldenEye 007.exe')) {
        Remove-OwnedFile (Join-Path $oldInstaller $name) 'old Setup files'
    }
    $oldGame = Join-Path $legacyResources 'game'
    foreach ($name in @('GoldenEye.exe', 'rexruntime.dll', 'rexgpu-xenos.dll', 'msvcp140.dll',
            'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'ge.toml',
            'menu-backdrop-169.png', 'menu-backdrop-219.png', 'version.txt')) {
        Remove-OwnedFile (Join-Path $oldGame $name) 'old runtime copy'
    }
    foreach ($folder in @('textures', 'portraits')) {
        $dir = Join-Path $oldGame $folder
        if (Test-Path -LiteralPath $dir -PathType Container) {
            Get-ChildItem -LiteralPath $dir -Filter *.png -File | ForEach-Object { Remove-OwnedFile $_.FullName 'old runtime copy' }
        }
    }
    if (Test-Path -LiteralPath $legacyResources -PathType Container) { Remove-EmptyFolders $legacyResources }
    # Old Setup logs move into Game\logs.
    if (Test-Path -LiteralPath $legacyLogDir -PathType Container) {
        $newLogs = Join-Path $gameDir 'logs'
        foreach ($log in @(Get-ChildItem -LiteralPath $legacyLogDir -Filter 'installer-*.log' -File)) {
            New-Item -ItemType Directory -Path $newLogs -Force | Out-Null
            Move-Item -LiteralPath $log.FullName -Destination $newLogs -Force
        }
        Remove-EmptyFolders $legacyLogDir
    }
    # In Game: the old program name, and the PC pictures now kept in resources\.
    if (Test-Path -LiteralPath (Join-Path $gameDir 'rexruntime.dll') -PathType Leaf) {
        Remove-OwnedFile (Join-Path $gameDir 'GoldenEye.exe') 'renamed to GoldenEye 007.exe'
    }
    foreach ($name in @('menu-backdrop-169.png', 'menu-backdrop-219.png', 'version.txt')) {
        Remove-OwnedFile (Join-Path $gameDir $name) 'moved to resources'
    }
    foreach ($folder in @('textures', 'portraits')) {
        $dir = Join-Path $gameDir $folder
        $newDir = Join-Path $runtimeDir ('resources\' + $folder)
        if ((Test-Path -LiteralPath $dir -PathType Container) -and (Test-Path -LiteralPath $newDir -PathType Container)) {
            foreach ($file in @(Get-ChildItem -LiteralPath $dir -Filter *.png -File)) {
                if (Test-Path -LiteralPath (Join-Path $newDir $file.Name)) { Remove-OwnedFile $file.FullName 'moved to resources' }
            }
            Remove-EmptyFolders $dir
        }
    }
}

# Installs into Game\. $Report receives (percent, phase, detail) as it goes.
function Invoke-GeInstall {
    param(
        [Parameter(Mandatory = $true)] [string]$PackagePath,
        [scriptblock]$Report
    )

    $script:lastPhase = ''
    function Step([int]$Percent, [string]$Phase, [string]$Detail) {
        if ($Phase -ne $script:lastPhase) {
            Write-Log ('{0} ({1}%)' -f $Phase, $Percent)
            $script:lastPhase = $Phase
        }
        if ($Report) {
            & $Report $Percent $Phase $Detail
        }
    }

    Write-Log ('Setup {0}: installing from {1}' -f $releaseVersion, $PackagePath)
    foreach ($required in @($extractorPath, (Join-Path $runtimeDir $gameExeName),
            (Join-Path $runtimeDir 'ge.toml'), (Join-Path $runtimeDir $manifestName))) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw ('This download is incomplete ({0} is missing). Extract the whole release again.' -f
                (Split-Path -Leaf $required))
        }
    }

    $package = Get-PackageInfo $PackagePath
    if (-not $package.Ok) {
        throw $package.Message
    }
    if (Test-GameRunning) {
        throw 'GoldenEye 007 is running from this folder. Close the game, then install again.'
    }

    Step 2 'PREPARING' ('Creating the Game folder' + $ellipsis)
    # A Game folder made by this run is removed again if the install fails, so
    # a failed first install leaves the release folder as it was.
    $gameDirCreated = -not (Test-Path -LiteralPath $gameDir)
    New-Item -ItemType Directory -Path $gameDir -Force | Out-Null
    Remove-Tree $stagingDir
    Remove-Tree $progressFile
    New-Item -ItemType Directory -Path $stagingDir -Force | Out-Null

    $extractOut = Join-Path $gameDir '.setup-extract-out.txt'
    $extractErr = Join-Path $gameDir '.setup-extract-err.txt'
    $process = $null
    $succeeded = $false
    try {
        # 1. Unpack into a staging folder. The extractor opens the package read-only.
        $arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -Path "{1}" -OutputDir "{2}" -ProgressPath "{3}"' -f
            $extractorPath, $PackagePath, $stagingDir, $progressFile
        $process = Start-Process -FilePath $powershellExe -ArgumentList $arguments -WindowStyle Hidden `
            -PassThru -RedirectStandardOutput $extractOut -RedirectStandardError $extractErr
        $null = $process.Handle  # keeps ExitCode readable once it exits (PowerShell 5.1)

        while (-not $process.HasExited) {
            $state = Read-ExtractorProgress
            if ($state -and $state.Total -gt 0) {
                $fraction = [Math]::Min(1.0, $state.Done / [double]$state.Total)
                Step ([int](5 + 80 * $fraction)) 'UNPACKING YOUR GAME DATA' (
                    '{0} of {1} files  {4}  {2:N0} of {3:N0} MB' -f $state.Files, $state.TotalFiles,
                    ($state.Done / 1MB), ($state.Total / 1MB), $dot)
            }
            else {
                Step 5 'UNPACKING YOUR GAME DATA' ('Reading the package file table' + $ellipsis)
            }
            Start-Sleep -Milliseconds 60
        }
        $process.WaitForExit()
        foreach ($captured in @($extractOut, $extractErr)) {
            if (Test-Path -LiteralPath $captured) {
                Get-Content -LiteralPath $captured | ForEach-Object { Write-Log ('  extractor: ' + $_) }
            }
        }
        if ($process.ExitCode -ne 0) {
            throw ('The package could not be unpacked (extractor exit code {0}). The file may be damaged or incomplete.' -f
                $process.ExitCode)
        }

        # 2. Check it is the build this port was recompiled from.
        Step 87 'CHECKING THE GAME DATA' ('Verifying the unpacked files' + $ellipsis)
        foreach ($relative in $requiredGameFiles) {
            if (-not (Test-Path -LiteralPath (Join-Path $stagingDir $relative) -PathType Leaf)) {
                throw ('The package unpacked, but {0} is missing from it. The package may be damaged.' -f $relative)
            }
        }
        $xexHash = (Get-FileHash -LiteralPath (Join-Path $stagingDir 'default.xex') -Algorithm SHA256).Hash
        Write-Log ('default.xex SHA-256 ' + $xexHash)
        if ($xexHash -ne $expectedXexSha256) {
            throw ('This GoldenEye 007 package carries a different build of the game program than the one this port was made from, so it cannot be used. Nothing was changed. (default.xex SHA-256 {0})' -f $xexHash)
        }

        # 2b. PC wording in the menus (PC SETTINGS, EXIT GAME, LAN). A failure
        # here only leaves the original Xbox wording, so it never stops the install.
        $menuPatch = Join-Path $installerDir 'Patch-MenuText.ps1'
        if (Test-Path -LiteralPath $menuPatch -PathType Leaf) {
            $patchOutput = & $menuPatch -AssetsDir $stagingDir
            Write-Log ('  ' + ($patchOutput -join ' '))
        }

        # 3. The PC runtime. Settings are only written when missing, so a
        # reinstall keeps the player's options and key bindings.
        Step 92 'INSTALLING GOLDENEYE 007' ('Copying the PC runtime' + $ellipsis)
        # Files the previous install listed that this release no longer has.
        $oldManifest = Join-Path $gameDir $manifestName
        $newPaths = Get-ManifestPaths (Join-Path $runtimeDir $manifestName)
        foreach ($relative in (Get-ManifestPaths $oldManifest)) {
            if ($relative -eq 'ge.toml' -or $newPaths -contains $relative) { continue }
            if ($relative -match '(^|\\)\.\.(\\|$)' -or [System.IO.Path]::IsPathRooted($relative)) { continue }
            Remove-OwnedFile (Join-Path $gameDir $relative) 'no longer part of the release'
        }
        Remove-LegacyLayout
        # The runtime (program, DLLs, manifest) and resources\, which Setup owns
        # outright: replaced as a whole so no stale picture stays behind.
        Get-ChildItem -LiteralPath $runtimeDir -File | Where-Object { $_.Name -ne 'ge.toml' } |
            ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $gameDir -Force }
        $resourcesTarget = Join-Path $gameDir 'resources'
        Remove-Tree $resourcesTarget
        Copy-Item -LiteralPath (Join-Path $runtimeDir 'resources') -Destination $gameDir -Recurse -Force
        $configTarget = Join-Path $gameDir 'ge.toml'
        if (Test-Path -LiteralPath $configTarget) {
            Write-Log 'Kept the existing ge.toml.'
            # 0.1.0-0.1.2 saved a 2560x1440 window size; windowed mode now opens at 720p.
            if ((Get-ConfigValue 'window_width') -eq '2560' -and (Get-ConfigValue 'window_height') -eq '1440') {
                Remove-ConfigValue 'window_width'
                Remove-ConfigValue 'window_height'
                Write-Log 'Reset the saved 2560x1440 window size.'
            }
        }
        else {
            Copy-Item -LiteralPath (Join-Path $runtimeDir 'ge.toml') -Destination $configTarget
        }
        # A player name for LAN and online play, picked at random from
        # player-names.txt, unless the player already has one.
        $currentName = Get-ConfigValue 'ge_username'
        if (-not $currentName -or $currentName -eq 'User') {
            $namesFile = Join-Path $installerDir 'player-names.txt'
            if (Test-Path -LiteralPath $namesFile -PathType Leaf) {
                $names = @([System.IO.File]::ReadAllLines($namesFile) | ForEach-Object { $_.Trim() } |
                    Where-Object { $_ -and $_.Length -le 15 -and $_ -notmatch '["\\]' })
                if ($names.Count -gt 0) {
                    $picked = $names | Get-Random
                    Set-ConfigValue 'ge_username' ('"' + $picked + '"')
                    Write-Log "Player name: $picked"
                }
            }
        }

        # 4. Swap the game data in, restoring the previous copy if the move fails.
        Step 96 'INSTALLING GOLDENEYE 007' ('Moving the game data into place' + $ellipsis)
        Remove-Tree $previousAssetsDir
        if (Test-Path -LiteralPath $assetsDir) {
            Move-Item -LiteralPath $assetsDir -Destination $previousAssetsDir
        }
        try {
            Move-Item -LiteralPath $stagingDir -Destination $assetsDir
        }
        catch {
            if ((Test-Path -LiteralPath $previousAssetsDir) -and -not (Test-Path -LiteralPath $assetsDir)) {
                Move-Item -LiteralPath $previousAssetsDir -Destination $assetsDir
            }
            throw
        }
        Remove-Tree $previousAssetsDir
        New-Item -ItemType Directory -Path (Join-Path $gameDir 'userdata') -Force | Out-Null

        # 5. A desktop shortcut that pointed at the old play launcher now opens
        # the game itself.
        if (Test-OurShortcut) { Set-Shortcut }

        Step 100 'GOLDENEYE 007 IS READY' 'Installed in the Game folder.'
        $succeeded = $true
        return $package
    }
    finally {
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        Remove-Tree $progressFile
        Remove-Tree $extractOut
        Remove-Tree $extractErr
        if (-not $succeeded) {
            Remove-Tree $stagingDir
            if ($gameDirCreated -and (Test-Path -LiteralPath $gameDir)) {
                Remove-Tree $gameDir
                Write-Log 'Removed the Game folder this attempt created.'
            }
        }
        else {
            Move-SetupLog
        }
    }
}

function Start-Game {
    if (Test-Path -LiteralPath $gameExe -PathType Leaf) {
        Start-Process -FilePath $gameExe -WorkingDirectory $gameDir
    }
}

# After a first install the log moves from the temporary folder into Game\logs.
function Move-SetupLog {
    $target = Join-Path $gameDir 'logs'
    if ($script:logDir -eq $target) { return }
    try {
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        if (Test-Path -LiteralPath $script:logFile -PathType Leaf) {
            Move-Item -LiteralPath $script:logFile -Destination $target -Force
        }
        $script:logDir = $target
        $script:logFile = Join-Path $target $logName
    }
    catch {
        # Logging must never be the reason Setup fails.
    }
}

# ----------------------------------------------------------------- extras --
# Optional features offered after the install. Every one is off unless the
# player ticks it. Game settings are keys in Game\ge.toml, which the
# game reads when it starts; the desktop shortcut is the only extra Setup
# applies itself. Adding an extra later means one entry here (and, for a game
# feature, the setting it controls).
$configPath = Join-Path $gameDir 'ge.toml'
$shortcutName = 'GoldenEye 007.lnk'
$extraDefinitions = @(
    @{ Id = 'fullscreen'; Title = 'Start in fullscreen'; Short = 'Fullscreen'
        Detail = 'You can switch at any time in Help & Options > Video Settings.'; Setting = 'fullscreen'; Default = $false },
    @{ Id = 'complete'; Title = '100% game completion'; Short = '100% completion'
        Detail = 'Unlocks every mission on every difficulty, 007 mode and all cheats. Your own best times still record.'; Setting = 'ge_unlock_all'; Default = $false },
    @{ Id = 'shortcut'; Title = 'Desktop shortcut'; Short = 'Desktop shortcut'
        Detail = 'Adds a GoldenEye 007 icon to your desktop. It stops working if you move this folder.'; Setting = $null; Default = $false }
)

function Get-ShortcutPath {
    $folder = if ($ShortcutFolder) { $ShortcutFolder } else { [Environment]::GetFolderPath('Desktop') }
    return Join-Path $folder $shortcutName
}

function Get-ConfigValue([string]$Key) {
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) { return $null }
    foreach ($line in [System.IO.File]::ReadAllLines($configPath)) {
        if ($line -match ('^\s*' + [regex]::Escape($Key) + '\s*=\s*(.+?)\s*$')) { return $Matches[1].Trim('"') }
    }
    return $null
}

# Replaces the key's line, or appends it; every other line is left as it was.
function Set-ConfigValue([string]$Key, [string]$Value) {
    $lines = New-Object System.Collections.Generic.List[string]
    if (Test-Path -LiteralPath $configPath -PathType Leaf) {
        $lines.AddRange([string[]][System.IO.File]::ReadAllLines($configPath))
    }
    $pattern = '^\s*' + [regex]::Escape($Key) + '\s*='
    $found = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match $pattern) { $lines[$i] = "$Key = $Value"; $found = $true }
    }
    if (-not $found) { $lines.Add("$Key = $Value") }
    [System.IO.File]::WriteAllLines($configPath, $lines, (New-Object System.Text.UTF8Encoding($false)))
}

function Remove-ConfigValue([string]$Key) {
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) { return }
    $pattern = '^\s*' + [regex]::Escape($Key) + '\s*='
    $lines = @([System.IO.File]::ReadAllLines($configPath) | Where-Object { $_ -notmatch $pattern })
    [System.IO.File]::WriteAllLines($configPath, [string[]]$lines, (New-Object System.Text.UTF8Encoding($false)))
}

# True only for a shortcut that points at this install's game (or the play
# launcher older releases kept beside Setup), so an unrelated "GoldenEye 007"
# shortcut is never touched.
function Test-OurShortcut {
    $path = Get-ShortcutPath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    try {
        $shell = New-Object -ComObject WScript.Shell
        $target = $shell.CreateShortcut($path).TargetPath
        foreach ($ours in @($gameExe, $legacyLauncher)) {
            if ([string]::Equals($target, $ours, [StringComparison]::OrdinalIgnoreCase)) { return $true }
        }
        return $false
    }
    catch {
        return $false
    }
}

function Set-Shortcut {
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut((Get-ShortcutPath))
    $link.TargetPath = $gameExe
    $link.WorkingDirectory = $gameDir
    $link.IconLocation = $gameExe + ',0'
    $link.Description = 'GoldenEye 007 (XBLA PC port)'
    $link.Save()
}

function Get-ExtraState([string]$Id) {
    $def = $extraDefinitions | Where-Object { $_.Id -eq $Id }
    if ($def.Setting) {
        $value = Get-ConfigValue $def.Setting
        if ($null -eq $value) { return [bool]$def.Default }
        return $value -eq 'true'
    }
    return (Test-OurShortcut)
}

# $Chosen holds the ids of the ticked extras. Every extra is written, so
# unticking one turns it back off.
function Set-Extras([string[]]$Chosen) {
    foreach ($def in $extraDefinitions) {
        $on = $Chosen -contains $def.Id
        if ($def.Setting) {
            Set-ConfigValue $def.Setting $(if ($on) { 'true' } else { 'false' })
        }
        elseif ($def.Id -eq 'shortcut') {
            $path = Get-ShortcutPath
            if ($on) {
                Set-Shortcut
            }
            elseif (Test-OurShortcut) {
                Remove-Item -LiteralPath $path -Force
            }
        }
        Write-Log ('Extra {0}: {1}' -f $def.Id, $(if ($on) { 'on' } else { 'off' }))
    }
}

# -------------------------------------------------------------- uninstall --
# Takes out what Setup put in and nothing else: the game program and the data
# unpacked from the player's own package, and the desktop shortcut if it points
# here. Saves (Game\userdata) and settings (Game\ge.toml) are left exactly as
# they are, so installing again picks up where the player left off. Setup,
# README.txt and licenses\ stay, because Setup is what puts the game back.
function Uninstall-Game {
    $kept = @('userdata', 'ge.toml')
    if (Test-Path -LiteralPath $gameDir -PathType Container) {
        foreach ($entry in Get-ChildItem -LiteralPath $gameDir -Force) {
            if ($kept -contains $entry.Name) { continue }
            Remove-Tree $entry.FullName
            Write-Log ('Removed ' + $entry.FullName)
        }
        # An empty Game folder is tidier gone, but one holding saves stays.
        if (-not (Get-ChildItem -LiteralPath $gameDir -Force)) {
            Remove-Tree $gameDir
            Write-Log 'Removed the empty Game folder.'
        }
    }
    Remove-LegacyLayout
    if (Test-OurShortcut) {
        Remove-Item -LiteralPath (Get-ShortcutPath) -Force
        Write-Log 'Removed the desktop shortcut.'
    }
}

function Test-SavesKept {
    $userdata = Join-Path $gameDir 'userdata'
    return (Test-Path -LiteralPath $userdata -PathType Container) -and
        [bool](Get-ChildItem -LiteralPath $userdata -Force -ErrorAction SilentlyContinue)
}

# ------------------------------------------------------------ unattended --
if ($Unattended) {
    if ([string]::IsNullOrWhiteSpace($PackagePath)) {
        Write-Log 'Unattended install needs -PackagePath.'
        exit 2
    }
    try {
        [void](Invoke-GeInstall -PackagePath $PackagePath)
        Write-Log 'Install finished.'
        # From a command line "-Extras a,b" arrives as one string.
        $chosen = @($Extras | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
        if ($chosen.Count -gt 0) { Set-Extras $chosen }
        exit 0
    }
    catch {
        Write-Log ('FAILED: ' + $_.Exception.Message)
        [Console]::Error.WriteLine($_.Exception.Message)
        exit 1
    }
}

# --------------------------------------------------------------- window ---
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace GeSetup -Name Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool ReleaseCapture();
[DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);
'@
[void][GeSetup.Native]::SetProcessDPIAware()
[System.Windows.Forms.Application]::EnableVisualStyles()

# Layout is written in 96-DPI units and scaled, so the window stays sharp on
# high-DPI displays instead of being bitmap-stretched by Windows.
$probe = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero)
# Everything is drawn 18% larger than the 96-DPI layout units, on top of
# the display's own DPI scaling, so the text reads comfortably.
$uiScale = 1.18
$script:scale = $probe.DpiX / 96.0 * $uiScale
$probe.Dispose()
function Px([double]$Value) { return [int][Math]::Round($Value * $script:scale) }
function Color([string]$Hex) { return [System.Drawing.ColorTranslator]::FromHtml($Hex) }

# Classified dossier, after the game's own main menu: a manila folder on a dark
# desk, a ruled sheet inside it, typewriter text in brown-black ink, and red
# rubber-stamp ink for what matters. The four steps are the folder's side tabs.
$palette = @{
    Desk = '#1E150E'; DeskLight = '#3A2A1C'
    Manila = '#D8C38E'; ManilaDark = '#C3A96C'; TabOn = '#E8D6A0'
    Paper = '#F2EAD2'; Rule = '#C9D3E0'
    Text = '#2B2418'; Soft = '#4A3F2A'; Muted = '#8A7A58'
    Cyan = '#6B5A38'   # phase line ink (brown); key kept for the shared code below
    Red = '#AC1C22'; Ok = '#3F6B2A'
}
$W = 730
$H = 560
$footerText = 'XBOX LIVE ARCADE  ' + $dot + '  TITLE ID 584108A9'

# Sheet geometry: the paper inside the folder, ruled every 28 units.
$sheetX = 36; $sheetY = 54; $sheetW = 630; $sheetH = 474
$folderX = 22; $folderY = 40; $folderW = 658; $folderH = 502
$tabX = $folderX + $folderW; $tabTop = 72; $tabPitch = 110; $tabH = 102

# Extras page geometry: one row per extra.
$extrasTop = 200
$rowPitch = 70
$firstRowY = $extrasTop + 6

function New-RoundedPath([single]$X, [single]$Y, [single]$Width, [single]$Height, [single]$Radius) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = [Math]::Max([single]1, $Radius * 2)
    $path.AddArc($X, $Y, $d, $d, 180, 90)
    $path.AddArc($X + $Width - $d, $Y, $d, $d, 270, 90)
    $path.AddArc($X + $Width - $d, $Y + $Height - $d, $d, $d, 0, 90)
    $path.AddArc($X, $Y + $Height - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return , $path
}

$form = New-Object System.Windows.Forms.Form
$form.Text = 'GoldenEye 007 Setup'
$form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::None
$form.StartPosition = [System.Windows.Forms.FormStartPosition]::CenterScreen
$form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::None
$form.ClientSize = New-Object System.Drawing.Size((Px $W), (Px $H))
$form.BackColor = Color $palette.Desk
$form.KeyPreview = $true
$form.AllowDrop = $true
$form.ShowInTaskbar = $true
$form.GetType().GetProperty('DoubleBuffered', [Reflection.BindingFlags]'NonPublic,Instance').SetValue($form, $true, $null)
$form.Region = New-Object System.Drawing.Region((New-RoundedPath 0 0 (Px $W) (Px $H) (Px 12)))
if (Test-Path -LiteralPath $setupExe -PathType Leaf) {
    try { $form.Icon = [System.Drawing.Icon]::ExtractAssociatedIcon($setupExe) } catch { }
}

$typeface = 'Courier New'
$fontBrand = New-Object System.Drawing.Font('Arial Black', (24 * $uiScale))
$fontTag = New-Object System.Drawing.Font($typeface, (9 * $uiScale), [System.Drawing.FontStyle]::Bold)
$fontTitle = New-Object System.Drawing.Font($typeface, (11 * $uiScale), [System.Drawing.FontStyle]::Bold)
$fontCaps = New-Object System.Drawing.Font($typeface, (9.5 * $uiScale), [System.Drawing.FontStyle]::Bold)
$fontBody = New-Object System.Drawing.Font('Segoe UI Semibold', (10.5 * $uiScale))
$fontSmall = New-Object System.Drawing.Font('Segoe UI', (9.75 * $uiScale))
$fontFoot = New-Object System.Drawing.Font($typeface, (8.5 * $uiScale))
$fontButton = New-Object System.Drawing.Font($typeface, (9.5 * $uiScale), [System.Drawing.FontStyle]::Bold)
$fontRowTitle = New-Object System.Drawing.Font($typeface, (10.5 * $uiScale), [System.Drawing.FontStyle]::Bold)
$fontRowDetail = New-Object System.Drawing.Font('Segoe UI', (9.5 * $uiScale))

function New-Label([string]$Text, [double]$X, [double]$Y, [double]$Width, [double]$Height,
    [System.Drawing.Font]$Font, [string]$ColorHex) {
    $label = New-Object System.Windows.Forms.Label
    $label.AutoSize = $false
    $label.Location = New-Object System.Drawing.Point((Px $X), (Px $Y))
    $label.Size = New-Object System.Drawing.Size((Px $Width), (Px $Height))
    $label.Font = $Font
    $label.ForeColor = Color $ColorHex
    $label.BackColor = [System.Drawing.Color]::Transparent
    $label.UseMnemonic = $false
    $label.Text = $Text
    $form.Controls.Add($label)
    return $label
}

# Buttons are typed labels on card: ink on paper, the main action in stamp red.
function Set-ButtonStyle($Button, [string]$Style) {
    $look = switch ($Style) {
        'primary' { @('#F4E3D2', '#AC1C22', '#AC1C22', '#EDD3BF', '#E3C2A9') }
        'disabled' { @('#E4DAC0', '#B2A585', '#C8BA95', '#E4DAC0', '#E4DAC0') }
        default { @('#EAE0C5', '#2B2418', '#3D3322', '#DDD0AE', '#D1C29A') }
    }
    $Button.BackColor = Color $look[0]
    $Button.ForeColor = Color $look[1]
    $Button.FlatAppearance.BorderColor = Color $look[2]
    $Button.FlatAppearance.MouseOverBackColor = Color $look[3]
    $Button.FlatAppearance.MouseDownBackColor = Color $look[4]
    $Button.FlatAppearance.BorderSize = 2
    if ($Style -eq 'disabled') {
        $Button.Cursor = [System.Windows.Forms.Cursors]::Default
    }
    else {
        $Button.Cursor = [System.Windows.Forms.Cursors]::Hand
    }
}

function New-Button([string]$Text, [double]$X, [double]$Y, [double]$Width, [double]$Height, [string]$Style) {
    $button = New-Object System.Windows.Forms.Button
    $button.Text = $Text
    $button.Location = New-Object System.Drawing.Point((Px $X), (Px $Y))
    $button.Size = New-Object System.Drawing.Size((Px $Width), (Px $Height))
    $button.FlatStyle = [System.Windows.Forms.FlatStyle]::Flat
    $button.Font = $fontButton
    $button.UseMnemonic = $false
    Set-ButtonStyle $button $Style
    $form.Controls.Add($button)
    return $button
}

# Shared by every page: the file heading beside the MI6 seal, and the memo line
# ("RE: ...") that names the page.
$lblBrand = New-Label 'GOLDENEYE 007' 164 64 380 48 $fontBrand $palette.Text
$lblTag = New-Label ('PC CONVERSION  ' + $dot + '  SETUP V' + $releaseVersion.ToUpperInvariant()) 170 114 340 20 $fontTag $palette.Muted
$lblTitle = New-Label 'RE: INSTALL YOUR GAME' 170 140 330 22 $fontTitle $palette.Text
$lblFoot = New-Label $footerText 56 496 330 18 $fontFoot $palette.Muted
$btnUninstall = New-Button 'UNINSTALL' 162 486 112 36 'secondary'
$btnExtras = New-Button 'CHANGE EXTRAS' 284 486 146 36 'secondary'
$btnLeft = New-Button 'CLOSE' 440 486 88 36 'secondary'
$btnRight = New-Button 'INSTALL GAME' 538 486 128 36 'disabled'
foreach ($button in @($btnUninstall, $btnExtras, $btnLeft, $btnRight)) { $button.BringToFront() }

# Package page.
$lblPkgCaps = New-Label 'Select your Xbox 360 GoldenEye 007 package' 56 198 600 24 $fontBody $palette.Text
$lblPkgHint = New-Label 'It is the 30BA9271... file. You can also drag it onto this window.' 56 224 600 22 $fontSmall $palette.Muted
$lblPath = New-Label 'No file selected' 56 256 444 30 $fontSmall $palette.Muted
$lblPath.TextAlign = [System.Drawing.ContentAlignment]::BottomLeft
$lblPath.AutoEllipsis = $true
$btnChoose = New-Button ('SELECT FILE' + $ellipsis) 516 254 150 36 'secondary'
$lblPkgStatus = New-Label '' 56 296 610 24 $fontSmall $palette.Muted
$lblNote1 = New-Label 'Installs beside Setup in the Game folder. Saves stay in userdata.' 76 330 590 22 $fontSmall $palette.Soft
$lblNote2 = New-Label 'Your package stays untouched. No game assets included.' 76 356 590 22 $fontSmall $palette.Soft
$lblPhase = New-Label '' 56 394 610 22 $fontCaps $palette.Cyan
# Live counts sit right-aligned on the phase line while installing. Kept in
# front of the phase label and hidden otherwise, so a long phase message is
# never covered.
$lblStats = New-Label '' 346 394 320 22 $fontSmall $palette.Muted
$lblStats.TextAlign = [System.Drawing.ContentAlignment]::TopRight
$lblStats.Visible = $false
$lblStats.BringToFront()
$lblDetail = New-Label '' 56 446 610 22 $fontSmall $palette.Muted
$packageControls = @($lblPkgCaps, $lblPkgHint, $lblPath, $btnChoose, $lblPkgStatus, $lblNote1, $lblNote2, $lblPhase, $lblDetail)

# Extras page: a typed box that gets a red X, a title and a one-line
# explanation per extra. Clicking any of the three toggles it.
$script:extraRows = New-Object System.Collections.Generic.List[object]
$toggleExtra = {
    param($sender, $e)
    $row = $script:extraRows[[int]$sender.Tag]
    $row.On = -not $row.On
    Update-ApplyButton
    $form.Invalidate()
}
for ($i = 0; $i -lt $extraDefinitions.Count; $i++) {
    $rowY = $firstRowY + $i * $rowPitch
    $box = New-Label '' 56 ($rowY + 1) 24 24 $fontSmall $palette.Text
    $title = New-Label $extraDefinitions[$i].Title.ToUpperInvariant() 94 ($rowY - 1) 570 24 $fontRowTitle $palette.Text
    $detail = New-Label $extraDefinitions[$i].Detail 94 ($rowY + 24) 570 40 $fontRowDetail $palette.Soft
    foreach ($control in @($box, $title, $detail)) {
        $control.Tag = $i
        $control.Cursor = [System.Windows.Forms.Cursors]::Hand
        $control.Add_Click($toggleExtra)
    }
    $script:extraRows.Add(@{ Def = $extraDefinitions[$i]; On = $false; Was = $false; Y = $rowY;
            Controls = @($box, $title, $detail) })
}
$lblExtrasNote = New-Label 'Nothing is switched on unless you mark it.' 56 ($firstRowY + $extraDefinitions.Count * $rowPitch + 4) 600 22 $fontSmall $palette.Muted

# Play page.
$lblPlay1 = New-Label 'Installed in the Game folder beside Setup. Your saves stay in Game\userdata.' 56 200 610 26 $fontSmall $palette.Soft
$lblPlay2 = New-Label '' 56 234 610 24 $fontBody $palette.Text
$lblPlay3 = New-Label 'To play later, run Game\GoldenEye 007.exe (or the desktop shortcut). Run Setup again to change your extras.' 56 268 610 48 $fontSmall $palette.Muted
$playControls = @($lblPlay1, $lblPlay2, $lblPlay3)

# Painted text runs under the DPI scale transform, so it is sized in 96-DPI
# pixels rather than points, which Windows would scale a second time.
function New-PixelFont([string]$Family, [single]$Points, [System.Drawing.FontStyle]$Style) {
    return New-Object System.Drawing.Font($Family, [single]($Points * 96 / 72), $Style, [System.Drawing.GraphicsUnit]::Pixel)
}
$script:tabFont = New-PixelFont $typeface 10.5 ([System.Drawing.FontStyle]::Bold)
$script:sealBig = New-PixelFont 'Arial Black' 16 ([System.Drawing.FontStyle]::Regular)
$script:sealSmall = New-PixelFont $typeface 7.5 ([System.Drawing.FontStyle]::Bold)
$script:stampFont = New-PixelFont 'Arial Black' 16 ([System.Drawing.FontStyle]::Regular)
$script:ghostFont = New-PixelFont 'Arial Black' 40 ([System.Drawing.FontStyle]::Regular)
$script:fileTabFont = New-PixelFont $typeface 8 ([System.Drawing.FontStyle]::Bold)
$script:centerFormat = New-Object System.Drawing.StringFormat
$script:centerFormat.Alignment = [System.Drawing.StringAlignment]::Center
$script:centerFormat.LineAlignment = [System.Drawing.StringAlignment]::Center

function Paint-Stamp($g, [string]$Text, [single]$CenterX, [single]$CenterY, [single]$Angle, [int]$Alpha) {
    $state = $g.Save()
    $g.TranslateTransform($CenterX, $CenterY)
    $g.RotateTransform($Angle)
    $size = $g.MeasureString($Text, $script:stampFont)
    $w = $size.Width + 16; $h = $size.Height + 4
    $ink = [System.Drawing.Color]::FromArgb($Alpha, 172, 28, 34)
    $pen = New-Object System.Drawing.Pen($ink, 3)
    $g.DrawRectangle($pen, [single](-$w / 2), [single](-$h / 2), [single]$w, [single]$h)
    $pen.Dispose()
    $brush = New-Object System.Drawing.SolidBrush($ink)
    $g.DrawString($Text, $script:stampFont, $brush,
        (New-Object System.Drawing.RectangleF([single](-$w / 2), [single](-$h / 2), [single]$w, [single]$h)), $script:centerFormat)
    $brush.Dispose()
    $g.Restore($state)
}

$form.Add_Paint({
        param($sender, $e)
        $g = $e.Graphics
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
        $g.ScaleTransform([single]$script:scale, [single]$script:scale)
        $ink = Color $palette.Text
        $red = Color $palette.Red

        # The desk, lit from the top left.
        $deskRect = New-Object System.Drawing.RectangleF(0, 0, $W, $H)
        $desk = New-Object System.Drawing.Drawing2D.LinearGradientBrush($deskRect,
            (Color $palette.DeskLight), (Color $palette.Desk), [single]60)
        $g.FillRectangle($desk, $deskRect)
        $desk.Dispose()

        # Step tabs down the folder's right edge, drawn first so the folder
        # overlaps their roots. Done steps get a tick; the current one is paler
        # and sticks out further.
        $stage = switch ($script:page) {
            'extras' { 2 }
            'play' { 3 }
            default { if ($script:showProgress) { 1 } else { 0 } }
        }
        $tabNames = @('PACKAGE', 'INSTALL', 'EXTRAS', 'PLAY')
        for ($i = 0; $i -lt 4; $i++) {
            $ty = $tabTop + $i * $tabPitch
            $reach = if ($i -eq $stage) { 42 } else { 34 }
            $tabPath = New-RoundedPath ($tabX - 10) $ty ($reach + 10) $tabH 6
            $fillHex = if ($i -eq $stage) { $palette.TabOn } elseif ($i -lt $stage) { $palette.Manila } else { $palette.ManilaDark }
            $tabFill = New-Object System.Drawing.SolidBrush((Color $fillHex))
            $g.FillPath($tabFill, $tabPath)
            $tabFill.Dispose()
            $tabPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(90, 60, 45, 20), 1)
            $g.DrawPath($tabPen, $tabPath)
            $tabPen.Dispose()
            $tabPath.Dispose()
            $label = $tabNames[$i]
            if ($i -lt $stage) { $label = $check + ' ' + $label }
            $tabInk = if ($i -eq $stage) { $ink } elseif ($i -lt $stage) { Color $palette.Soft } else { Color $palette.Muted }
            $state = $g.Save()
            $g.TranslateTransform([single]($tabX + $reach / 2), [single]($ty + $tabH / 2))
            $g.RotateTransform(90)
            $tabBrush = New-Object System.Drawing.SolidBrush($tabInk)
            $g.DrawString($label, $script:tabFont, $tabBrush,
                (New-Object System.Drawing.RectangleF([single](-$tabH / 2), -9, [single]$tabH, 18)), $script:centerFormat)
            $tabBrush.Dispose()
            $g.Restore($state)
        }

        # The folder: its file tab, a soft shadow, then the manila board.
        $shadow = New-RoundedPath ($folderX + 4) ($folderY + 7) $folderW $folderH 8
        $shadowFill = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(110, 0, 0, 0))
        $g.FillPath($shadowFill, $shadow)
        $shadowFill.Dispose()
        $shadow.Dispose()
        $fileTab = New-RoundedPath ($folderX + 30) ($folderY - 22) 214 30 8
        $fileTabFill = New-Object System.Drawing.SolidBrush((Color '#D0B87C'))
        $g.FillPath($fileTabFill, $fileTab)
        $fileTabFill.Dispose()
        $fileTab.Dispose()
        $fileInk = New-Object System.Drawing.SolidBrush((Color '#5A4A2A'))
        $g.DrawString('FILE 584108A9  ' + $dot + '  MI6', $script:fileTabFont, $fileInk, [single]($folderX + 44), [single]($folderY - 16))
        $fileInk.Dispose()
        $boardRect = New-Object System.Drawing.RectangleF($folderX, $folderY, $folderW, $folderH)
        $board = New-RoundedPath $folderX $folderY $folderW $folderH 8
        $boardFill = New-Object System.Drawing.Drawing2D.LinearGradientBrush($boardRect,
            (Color $palette.Manila), (Color $palette.ManilaDark), [single]80)
        $g.FillPath($boardFill, $board)
        $boardFill.Dispose()
        $board.Dispose()

        # The sheet: cream paper, blue rules, a red margin line.
        $paper = New-Object System.Drawing.SolidBrush((Color $palette.Paper))
        $g.FillRectangle($paper, $sheetX, $sheetY, $sheetW, $sheetH)
        $paper.Dispose()
        $rulePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(55, 150, 170, 200), 1)
        for ($y = $sheetY + 30; $y -lt $sheetY + $sheetH - 6; $y += 30) {
            $g.DrawLine($rulePen, $sheetX, $y, $sheetX + $sheetW, $y)
        }
        $rulePen.Dispose()
        $marginPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(70, 172, 28, 34), 1)
        $g.DrawLine($marginPen, ($sheetX + 10), $sheetY, ($sheetX + 10), ($sheetY + $sheetH))
        $marginPen.Dispose()
        $edgePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(60, 60, 45, 20), 1)
        $g.DrawRectangle($edgePen, $sheetX, $sheetY, $sheetW, $sheetH)
        $edgePen.Dispose()

        # The MI6 seal where a photo would go: a double ring in brown ink.
        $sealInk = Color '#5B4B2D'
        $sealPen = New-Object System.Drawing.Pen($sealInk, 2)
        $g.DrawEllipse($sealPen, 60, 68, 90, 90)
        $g.DrawEllipse($sealPen, 66, 74, 78, 78)
        $sealPen.Dispose()
        $sealBrush = New-Object System.Drawing.SolidBrush($sealInk)
        $g.DrawString('MI6', $script:sealBig, $sealBrush, (New-Object System.Drawing.RectangleF(60, 92, 90, 28)), $script:centerFormat)
        $g.DrawString('SETUP', $script:sealSmall, $sealBrush, (New-Object System.Drawing.RectangleF(60, 118, 90, 16)), $script:centerFormat)
        $sealBrush.Dispose()

        # The stamp beside the heading.
        Paint-Stamp $g 'CLASSIFIED' 578 150 -6 200

        if ($script:page -eq 'package') {
            # The line the file name is typed on.
            $linePen = New-Object System.Drawing.Pen($ink, 1.6)
            $g.DrawLine($linePen, 56, 287, 500, 287)
            $linePen.Dispose()
            $bullet = New-Object System.Drawing.SolidBrush($ink)
            foreach ($y in @(338, 364)) { $g.FillRectangle($bullet, 60, $y, 7, 7) }
            $bullet.Dispose()

            if ($script:showProgress) {
                # An inked bar that fills left to right, with the percentage
                # stamped in red at its end.
                $barPen = New-Object System.Drawing.Pen($ink, 1.4)
                $g.DrawRectangle($barPen, 56, 420, 610, 16)
                $barPen.Dispose()
                $width = [single](606 * $script:progress / 100.0)
                if ($width -gt 1) {
                    $inkFill = New-Object System.Drawing.Drawing2D.HatchBrush(
                        [System.Drawing.Drawing2D.HatchStyle]::DarkUpwardDiagonal, $ink, (Color '#3D3322'))
                    $g.FillRectangle($inkFill, 58, 422, $width, 12)
                    $inkFill.Dispose()
                }
            }
        }
        elseif ($script:page -eq 'extras') {
            foreach ($row in $script:extraRows) {
                $boxPen = New-Object System.Drawing.Pen($ink, 1.6)
                $g.DrawRectangle($boxPen, 58, ($row.Y + 3), 20, 20)
                $boxPen.Dispose()
                if ($row.On) {
                    $xPen = New-Object System.Drawing.Pen($red, 2.6)
                    $xPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
                    $xPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
                    $g.DrawLine($xPen, 60, ($row.Y + 5), 77, ($row.Y + 22))
                    $g.DrawLine($xPen, 77, ($row.Y + 4), 61, ($row.Y + 23))
                    $xPen.Dispose()
                }
            }
        }
        else {
            Paint-Stamp $g 'APPROVED' 540 400 -9 170
        }
    })

$script:page = 'package'
$script:selectedPackage = $null
$script:canInstall = $false
$script:installed = $false
$script:busy = $false
$script:showProgress = $false
$script:progress = 0
$script:existingInstall = $false
$script:extrasChanged = $false

# Apply is only worth pressing when the ticks differ from what is already set,
# which on a first run means at least one extra ticked, and on a later run
# means something ticked or unticked.
function Update-ApplyButton {
    $changed = $false
    foreach ($row in $script:extraRows) {
        if ([bool]$row.On -ne [bool]$row.Was) { $changed = $true }
    }
    Set-ButtonStyle $btnRight $(if ($changed) { 'primary' } else { 'disabled' })
    $script:extrasChanged = $changed
}

function Set-InstallReady([bool]$Ready) {
    $script:canInstall = $Ready
    if ($Ready) { Set-ButtonStyle $btnRight 'primary' } else { Set-ButtonStyle $btnRight 'disabled' }
}

# Shows one page's controls and relabels the shared buttons for it.
function Show-Page([string]$Name) {
    $script:page = $Name
    foreach ($control in $packageControls) { $control.Visible = ($Name -eq 'package') }
    if ($Name -ne 'package') { $lblStats.Visible = $false }
    foreach ($row in $script:extraRows) {
        foreach ($control in $row.Controls) { $control.Visible = ($Name -eq 'extras') }
    }
    foreach ($control in $playControls) { $control.Visible = ($Name -eq 'play') }
    $lblExtrasNote.Visible = ($Name -eq 'extras')
    $btnExtras.Visible = ($Name -eq 'package' -and $script:existingInstall)
    $btnUninstall.Visible = ($Name -eq 'package' -and $script:existingInstall)
    # With three buttons along the bottom there is no room left for the footer.
    $lblFoot.Visible = -not $btnUninstall.Visible
    switch ($Name) {
        'package' {
            $lblTitle.Text = 'RE: INSTALL YOUR GAME'
            $lblFoot.Text = $footerText
            $btnLeft.Text = 'CLOSE'
            $btnRight.Text = 'INSTALL GAME'
            Set-InstallReady $script:canInstall
        }
        'extras' {
            $lblTitle.Text = 'RE: CHOOSE YOUR EXTRAS'
            $lblFoot.Text = $footerText
            $btnLeft.Text = 'SKIP'
            $btnRight.Text = 'APPLY'
            Update-ApplyButton
        }
        'play' {
            $lblTitle.Text = 'RE: READY FOR DUTY'
            $lblFoot.Text = $footerText
            $btnLeft.Text = 'CLOSE'
            $btnRight.Text = 'PLAY NOW'
            Set-ButtonStyle $btnRight 'primary'
        }
    }
    $form.Invalidate()
}

# Ticks reflect what is set now, so re-running Setup shows the current extras.
function Enter-Extras {
    foreach ($row in $script:extraRows) {
        $row.On = [bool](Get-ExtraState $row.Def.Id)
        $row.Was = $row.On
    }
    Show-Page 'extras'
}

function Enter-Play {
    $on = @($extraDefinitions | Where-Object { Get-ExtraState $_.Id } | ForEach-Object { $_.Short })
    $lblPlay2.Text = if ($on.Count -gt 0) { 'Extras:  ' + ($on -join ('  ' + $dot + '  ')) } else { 'Extras:  none' }
    Show-Page 'play'
}

# While installing, $Detail (the live counts) sits right-aligned on the phase
# line; -Below puts it on its own line under the bar instead.
function Update-Progress([int]$Percent, [string]$Phase, [string]$Detail, [switch]$Below) {
    $script:progress = [Math]::Max(0, [Math]::Min(100, $Percent))
    if ($lblPhase.Text -ne $Phase) { $lblPhase.Text = $Phase }
    if ($Below) {
        $lblStats.Visible = $false
        $lblDetail.Text = $Detail
    }
    else {
        if ($lblStats.Text -ne $Detail) { $lblStats.Text = $Detail }
        $lblStats.Visible = $true
        if ($lblDetail.Text -ne '') { $lblDetail.Text = '' }
    }
    # The bar and the side tabs (Install becomes current) both change.
    $form.Invalidate((New-Object System.Drawing.Rectangle((Px 52), (Px 414), (Px 620), (Px 28))))
    # The tabs reach to x 722 plus their outline, so repaint to the window edge.
    $form.Invalidate((New-Object System.Drawing.Rectangle((Px 660), (Px 66), (Px ($W - 660)), (Px 400))))
    [System.Windows.Forms.Application]::DoEvents()
}

function Select-Package([string]$Path) {
    if ($script:busy -or $script:page -ne 'package') { return }
    $script:selectedPackage = $null
    $script:showProgress = $false
    $lblStats.Visible = $false
    $lblPhase.ForeColor = Color $palette.Cyan
    $lblPath.Text = $Path
    $lblPath.ForeColor = Color $palette.Text

    $info = Get-PackageInfo $Path
    if ($info.Ok) {
        $script:selectedPackage = $Path
        $name = 'GoldenEye 007'
        $lblPkgStatus.Text = ('{0}  {1}  {2}  Xbox LIVE Arcade  {2}  {3:N0} MB' -f $check, $name, $dot, ($info.Bytes / 1MB))
        $lblPkgStatus.ForeColor = Color $palette.Ok
        Set-InstallReady $true
        $lblPhase.Text = ('READY TO INSTALL  {0}  Setup unpacks about {1:N0} MB into the Game folder.' -f $dot, ($info.Bytes / 1MB))
        $lblDetail.Text = if ($script:existingInstall) { 'Installing again updates the game and keeps your saves and settings.' } else { '' }
        Write-Log ('Package accepted: {0} ({1} bytes)' -f $Path, $info.Bytes)
    }
    else {
        $lblPkgStatus.Text = $info.Message
        $lblPkgStatus.ForeColor = Color $palette.Red
        Set-InstallReady $false
        if ($script:existingInstall) {
            $lblPhase.Text = 'GOLDENEYE 007 IS ALREADY INSTALLED HERE'
            $lblDetail.Text = 'Installing again updates the game and keeps your saves and settings.'
        }
        else {
            $lblPhase.Text = ''
            $lblDetail.Text = ''
        }
        Write-Log ('Package rejected: {0} - {1}' -f $Path, $info.Message)
    }
    $form.Invalidate()
}

$btnChoose.Add_Click({
        if ($script:busy) { return }
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = 'Select your GoldenEye 007 Xbox 360 package'
        $dialog.Filter = 'Xbox 360 package (any file)|*.*'
        $dialog.CheckFileExists = $true
        $dialog.Multiselect = $false
        # Start in the folder Setup is in (where most people put the package),
        # or beside the package already chosen.
        $dialog.InitialDirectory = $releaseRoot
        if ($script:selectedPackage) {
            $dialog.InitialDirectory = Split-Path -Parent $script:selectedPackage
        }
        if ($dialog.ShowDialog($form) -eq [System.Windows.Forms.DialogResult]::OK) {
            Select-Package $dialog.FileName
        }
        $dialog.Dispose()
    })

function Start-Install {
    if (-not $script:canInstall -or -not $script:selectedPackage) { return }
    $script:busy = $true
    foreach ($button in @($btnRight, $btnChoose, $btnLeft, $btnExtras, $btnUninstall)) {
        Set-ButtonStyle $button 'disabled'
    }
    $lblPhase.ForeColor = Color $palette.Cyan
    $script:showProgress = $true
    Update-Progress 0 'PREPARING' ''
    try {
        [void](Invoke-GeInstall -PackagePath $script:selectedPackage -Report {
                param($p, $phase, $detail)
                Update-Progress $p $phase $detail
            })
        $script:installed = $true
        $script:existingInstall = $true
        $script:showProgress = $false
        Write-Log 'Install finished.'
        Enter-Extras
    }
    catch {
        $message = $_.Exception.Message
        Write-Log ('FAILED: ' + $message)
        $script:showProgress = $false
        $lblStats.Visible = $false
        $lblPhase.Text = 'INSTALLATION DID NOT COMPLETE'
        $lblPhase.ForeColor = Color $palette.Red
        $lblDetail.Text = ('Your package was not changed. Details are in ' + $script:logFile)
        $form.Invalidate()
        [void][System.Windows.Forms.MessageBox]::Show($form, $message, 'GoldenEye 007 Setup',
            [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Error)
    }
    finally {
        $script:busy = $false
        Set-ButtonStyle $btnChoose 'secondary'
        Set-ButtonStyle $btnLeft 'secondary'
        Set-ButtonStyle $btnExtras 'secondary'
        Set-ButtonStyle $btnUninstall 'secondary'
        switch ($script:page) {
            'package' { Set-InstallReady $script:canInstall }
            'extras' { Update-ApplyButton }   # nothing ticked yet, nothing to apply
            default { Set-ButtonStyle $btnRight 'primary' }
        }
    }
}

function Save-Extras {
    $chosen = @($script:extraRows | Where-Object { $_.On } | ForEach-Object { $_.Def.Id })
    try {
        Set-Extras $chosen
        Enter-Play
    }
    catch {
        Write-Log ('Extras not saved: ' + $_.Exception.Message)
        [void][System.Windows.Forms.MessageBox]::Show($form,
            ('Your extras could not be saved: ' + $_.Exception.Message + "`n`nThe game is installed and will still run."),
            'GoldenEye 007 Setup', [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Warning)
    }
}

$btnRight.Add_Click({
        if ($script:busy) { return }
        switch ($script:page) {
            'package' { Start-Install }
            'extras' { if ($script:extrasChanged) { Save-Extras } }
            'play' { Start-Game; $form.Close() }
        }
    })

$btnLeft.Add_Click({
        if ($script:busy) { return }
        if ($script:page -eq 'extras') {
            Write-Log 'Extras skipped.'
            Enter-Play
        }
        else {
            $form.Close()
        }
    })

$btnExtras.Add_Click({ if (-not $script:busy) { Enter-Extras } })

$btnUninstall.Add_Click({
        if ($script:busy) { return }
        $saves = if (Test-SavesKept) { 'Your saves are kept.' } else { 'You have no saves to keep.' }
        $answer = [System.Windows.Forms.MessageBox]::Show($form,
            ("Remove GoldenEye 007 from this folder?`n`n" +
                "The game and the data unpacked from your package are deleted, along with the " +
                "desktop shortcut.`n`n" + $saves +
                " Your settings are kept too, and Setup stays here to install the game again."),
            'GoldenEye 007 Setup', [System.Windows.Forms.MessageBoxButtons]::YesNo,
            [System.Windows.Forms.MessageBoxIcon]::Warning,
            [System.Windows.Forms.MessageBoxDefaultButton]::Button2)
        if ($answer -ne [System.Windows.Forms.DialogResult]::Yes) { return }
        if (Test-GameRunning) {
            [void][System.Windows.Forms.MessageBox]::Show($form,
                'Close GoldenEye 007 first, then try again.', 'GoldenEye 007 Setup',
                [System.Windows.Forms.MessageBoxButtons]::OK,
                [System.Windows.Forms.MessageBoxIcon]::Information)
            return
        }
        $script:busy = $true
        foreach ($button in @($btnRight, $btnChoose, $btnLeft, $btnExtras, $btnUninstall)) {
            Set-ButtonStyle $button 'disabled'
        }
        $lblPhase.Text = 'REMOVING'
        $lblDetail.Text = ''
        [System.Windows.Forms.Application]::DoEvents()
        try {
            Uninstall-Game
            $script:existingInstall = $false
            $script:installed = $false
            $script:selectedPackage = $null
            $lblPath.Text = 'No file selected'
            $lblPath.ForeColor = Color $palette.Muted
            $lblPkgStatus.Text = ''
            $lblPkgStatus.ForeColor = Color $palette.Muted
            $lblPhase.Text = 'REMOVED'
            $lblDetail.Text = if (Test-SavesKept) {
                'Your saves and settings are still in the Game folder. Install again whenever you like.'
            }
            else {
                'Install again whenever you like.'
            }
            Set-InstallReady $false
        }
        catch {
            Write-Log ('Uninstall failed: ' + $_.Exception.Message)
            $lblPhase.Text = 'COULD NOT REMOVE EVERYTHING'
            $lblDetail.Text = $_.Exception.Message
        }
        finally {
            $script:busy = $false
            Set-ButtonStyle $btnChoose 'secondary'
            Set-ButtonStyle $btnLeft 'secondary'
            Show-Page 'package'
        }
    })

$form.Add_FormClosing({
        param($sender, $e)
        if ($script:busy) { $e.Cancel = $true }
    })
$form.Add_KeyDown({
        param($sender, $e)
        if ($e.KeyCode -eq [System.Windows.Forms.Keys]::Escape -and -not $script:busy) { $form.Close() }
    })

# Borderless window: drag it by the header.
$dragWindow = {
    param($sender, $e)
    if ($e.Button -eq [System.Windows.Forms.MouseButtons]::Left) {
        [void][GeSetup.Native]::ReleaseCapture()
        [void][GeSetup.Native]::SendMessage($form.Handle, 0xA1, [IntPtr]2, [IntPtr]::Zero)
    }
}
$form.Add_MouseDown($dragWindow)
foreach ($label in @($lblBrand, $lblTag, $lblTitle)) { $label.Add_MouseDown($dragWindow) }

# Dropping the package anywhere on the package page selects it.
$dragEnter = {
    param($sender, $e)
    if (-not $script:busy -and $script:page -eq 'package' -and
        $e.Data.GetDataPresent([System.Windows.Forms.DataFormats]::FileDrop)) {
        $e.Effect = [System.Windows.Forms.DragDropEffects]::Copy
    }
    else {
        $e.Effect = [System.Windows.Forms.DragDropEffects]::None
    }
}
$dragDrop = {
    param($sender, $e)
    $files = $e.Data.GetData([System.Windows.Forms.DataFormats]::FileDrop)
    if ($files -and $files.Count -gt 0) { Select-Package ([string]$files[0]) }
}
$form.Add_DragEnter($dragEnter)
$form.Add_DragDrop($dragDrop)
foreach ($control in $form.Controls) {
    $control.AllowDrop = $true
    $control.Add_DragEnter($dragEnter)
    $control.Add_DragDrop($dragDrop)
}

$form.Add_Shown({
        $form.Activate()
        if (((Test-Path -LiteralPath $gameExe) -or (Test-Path -LiteralPath (Join-Path $gameDir 'GoldenEye.exe'))) -and
            (Test-Path -LiteralPath (Join-Path $assetsDir 'default.xex'))) {
            $script:existingInstall = $true
            $lblPhase.Text = 'GOLDENEYE 007 IS ALREADY INSTALLED HERE'
            $lblDetail.Text = 'Reinstalling keeps your saves and settings. You can also change your extras, or remove the game.'
        }
        Show-Page 'package'
        if ($PackagePath) { Select-Package $PackagePath }
    })

Show-Page 'package'
Write-Log ('Setup {0} opened in {1}' -f $releaseVersion, $releaseRoot)
[void]$form.ShowDialog()
$form.Dispose()
exit 0
