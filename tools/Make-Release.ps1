# Builds the GoldenEye 007 portable release in out\release. The ZIP holds one folder:
#
#   GoldenEye 007\
#     GoldenEye 007.exe           the launcher: the only program to open
#     README.txt
#     licenses\
#     resources\
#       runtime\                  the game (GoldenEye 007.exe, rexruntime.dll,
#                                 rexgpu-xenos.dll, VC++ runtime DLLs) and the
#                                 port's own pictures and version.txt
#
# The first time the launcher runs it imports the game data from the player's
# own Xbox 360 package into resources\assets; saves and settings go to
# resources\userdata. The release carries no original game content; the audit
# below refuses to package anything that looks like game data, debug output or
# user state.
[CmdletBinding()]
param(
    [string]$BuildPreset = "win-amd64-release",
    # Defaults to the VERSION file at the project root, the single source of truth.
    [string]$Version,
    # Folder holding the Visual C++ runtime DLLs to ship; found automatically.
    [string]$VcRedistDir
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if (-not $Version) {
    $Version = ([System.IO.File]::ReadAllText((Join-Path $PSScriptRoot "..\VERSION"))).Trim()
}

if ($Version -notmatch '^[A-Za-z0-9._-]+$') {
    throw "Version may contain only letters, numbers, dots, underscores and hyphens."
}

$projectRoot = [System.IO.Path]::GetFullPath((Join-Path -Path $PSScriptRoot -ChildPath ".."))
$buildRoot = Join-Path $projectRoot "out\build\$BuildPreset"
$packagingRoot = Join-Path $projectRoot "packaging"
$releaseRoot = Join-Path $projectRoot "out\release"
# Staged under out\release\staging so a copy someone plays from out\release is never touched.
$releaseName = "GoldenEye 007"
$stageRoot = Join-Path $releaseRoot "staging\$releaseName"
$zipPath = Join-Path $releaseRoot "GoldenEye-007-XBLA-PC-Recomp-v$Version.zip"
$exeName = "GoldenEye 007.exe"

$runtimeFiles = @($exeName, "rexruntime.dll", "rexgpu-xenos.dll")
$vcRuntimeFiles = @("msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll")

# --- 1. Inputs ---------------------------------------------------------------
foreach ($file in $runtimeFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $buildRoot $file) -PathType Leaf)) {
        throw "Build output is missing: $file. Build $BuildPreset first."
    }
}

# A release must never ship a binary older than the source it claims to be.
$exeTime = (Get-Item -LiteralPath (Join-Path $buildRoot $exeName)).LastWriteTimeUtc
$newestSource = Get-ChildItem -LiteralPath (Join-Path $projectRoot "src") -File -Recurse |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if ($newestSource.LastWriteTimeUtc -gt $exeTime) {
    throw "$exeName is older than src\$($newestSource.Name). Rebuild before packaging."
}

if (-not $VcRedistDir) {
    $candidates = @(Get-Item -Path "C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT" -ErrorAction SilentlyContinue |
        Where-Object { $dir = $_.FullName; @($vcRuntimeFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $dir $_)) }).Count -eq 0 } |
        Sort-Object { [version]($_.FullName -replace '^.*\\MSVC\\([0-9.]+)\\.*$', '$1') } -Descending)
    if ($candidates.Count -eq 0) {
        throw "Visual C++ redistributable DLLs not found. Pass -VcRedistDir."
    }
    $VcRedistDir = $candidates[0].FullName
}
foreach ($file in $vcRuntimeFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $VcRedistDir $file) -PathType Leaf)) {
        throw "Missing $file in $VcRedistDir"
    }
}

# ReXGlue 0.10.0 (GoldenEye SDK) uses GPU plugin ABI 1. Validate the exported ABI function
# before packaging so a stale plugin cannot produce a broken release.
if (-not ("GpuPluginInspector" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class GpuPluginInspector {
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate UInt32 GpuAbiVersionDelegate();

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryExW(
        string fileName, IntPtr file, UInt32 flags);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string symbolName);

    [DllImport("kernel32.dll")]
    private static extern bool FreeLibrary(IntPtr module);

    public static UInt32 GetAbiVersion(string pluginPath) {
        const UInt32 LoadLibrarySearchDllLoadDir = 0x00000100;
        const UInt32 LoadLibrarySearchDefaultDirs = 0x00001000;
        IntPtr module = LoadLibraryExW(
            pluginPath, IntPtr.Zero,
            LoadLibrarySearchDllLoadDir | LoadLibrarySearchDefaultDirs);
        if (module == IntPtr.Zero) {
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Could not load GPU plugin for ABI validation");
        }

        try {
            IntPtr symbol = GetProcAddress(module, "rex_gpu_abi_version");
            if (symbol == IntPtr.Zero) {
                throw new InvalidOperationException(
                    "GPU plugin does not export rex_gpu_abi_version");
            }

            var function = (GpuAbiVersionDelegate)Marshal.GetDelegateForFunctionPointer(
                symbol, typeof(GpuAbiVersionDelegate));
            return function();
        }
        finally {
            FreeLibrary(module);
        }
    }
}
"@
}

$gpuPluginAbi = [GpuPluginInspector]::GetAbiVersion((Join-Path $buildRoot "rexgpu-xenos.dll"))
if ($gpuPluginAbi -ne 1) {
    throw "GPU plugin ABI mismatch: found $gpuPluginAbi, expected 1. Rebuild with GPU_PLUGINS xenos."
}

# --- 2. Launcher ----------------------------------------------------------------
& (Join-Path $packagingRoot "Build-Launcher.ps1") -Version $Version
$launcher = Join-Path $packagingRoot $exeName
if (-not (Test-Path -LiteralPath $launcher -PathType Leaf)) {
    throw "The launcher was not built: $exeName"
}

# --- 3. Stage -----------------------------------------------------------------
foreach ($played in @("resources\assets", "resources\userdata")) {
    if (Test-Path -LiteralPath (Join-Path $stageRoot $played)) {
        throw "The staging folder has been played from ($played; saves?). Move it away first: $stageRoot"
    }
}
foreach ($old in @($stageRoot, $zipPath)) {
    if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Recurse -Force }
}
# Staging left by releases before 0.9.1's single launcher.
foreach ($old in @("staging\GoldenEye 007 XBLA PC Recomp", "staging\payload", "staging\payload.zip")) {
    $path = Join-Path $releaseRoot $old
    if ((Test-Path -LiteralPath $path) -and -not (Test-Path -LiteralPath (Join-Path $path "Game"))) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}

$licensesStage = Join-Path $stageRoot "licenses"
$runtimeStage = Join-Path $stageRoot "resources\runtime"
foreach ($dir in @($stageRoot, $licensesStage, $runtimeStage)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

# Release root: the launcher, README and licences.
Copy-Item -LiteralPath $launcher -Destination $stageRoot
$readme = [System.IO.File]::ReadAllText((Join-Path $packagingRoot "README.txt")).Replace("@VERSION@", $Version)
$readme = $readme -replace "`r?`n", "`r`n"  # Notepad-friendly line endings
[System.IO.File]::WriteAllText((Join-Path $stageRoot "README.txt"), $readme, (New-Object System.Text.UTF8Encoding($false)))
Get-ChildItem -LiteralPath (Join-Path $packagingRoot "licenses") -File |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $licensesStage }

# resources\runtime: the game, its DLLs, and the port's own pictures (the desk
# picture behind the front-end folder on wide screens, re-painted menu folder
# textures and character select pictures when present) and the version shown on
# the main menu. The game reads its data from resources\assets and resources\userdata.
foreach ($file in $runtimeFiles) { Copy-Item -LiteralPath (Join-Path $buildRoot $file) -Destination $runtimeStage }
foreach ($file in $vcRuntimeFiles) { Copy-Item -LiteralPath (Join-Path $VcRedistDir $file) -Destination $runtimeStage }
foreach ($shape in @("169", "219")) {
    Copy-Item -LiteralPath (Join-Path $packagingRoot "menu-backdrop-$shape.png") -Destination $runtimeStage
}
foreach ($folder in @("textures", "portraits")) {
    # Optional: the game falls back to the original pictures without them.
    if (-not (Test-Path -LiteralPath (Join-Path $packagingRoot $folder))) { continue }
    $target = Join-Path $runtimeStage $folder
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $packagingRoot $folder) -Filter *.png -File |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $target }
}
[System.IO.File]::WriteAllText((Join-Path $runtimeStage "version.txt"), $Version)

# --- 4. Audit: nothing from the game package, no debug output, no user state -
$staged = @(Get-ChildItem -LiteralPath $stageRoot -Recurse -Force)
foreach ($item in @(Get-ChildItem -LiteralPath $stageRoot -Force)) {
    if (@($exeName, "README.txt", "licenses", "resources") -notcontains $item.Name) {
        throw "Unexpected item in the release root: $($item.Name)"
    }
}
foreach ($item in @(Get-ChildItem -LiteralPath (Join-Path $stageRoot "resources") -Force)) {
    if ($item.Name -ne "runtime") { throw "Unexpected item in resources: $($item.Name)" }
}
$forbiddenNames = @("assets", "userdata", "logs", "cache", "Game", "ge.toml",
    "default.xex", "ArcadeInfo.xml", "Bean.xex", "music.xwb", "sfx.xwb", "files",
    "guest_image.bin", "xstr_decompressed.bin")
$forbiddenPatterns = @("*.pdb", "*.xex", "*.bin", "*.log", "*.ilk", "*.exp", "*.lib", "*.ps1",
    "*.xwb", "*.xsb", "*.xgs", "*.rba", "*.rbh", "*.str")
foreach ($item in $staged) {
    if ($forbiddenNames -contains $item.Name) {
        throw "Forbidden item entered the release: $($item.FullName)"
    }
    foreach ($pattern in $forbiddenPatterns) {
        if ($item.Name -like $pattern) { throw "Forbidden file type entered the release: $($item.FullName)" }
    }
    if (-not $item.PSIsContainer -and $item.Length -gt 64MB) {
        throw "Unexpectedly large file in the release: $($item.FullName)"
    }
}

# Byte-level check against the developer's extracted game data, when present:
# no shipped file may be identical to any file from the package.
$stagedFiles = @($staged | Where-Object { -not $_.PSIsContainer })
$assetsRoot = Join-Path $buildRoot "assets"
if (-not (Test-Path -LiteralPath $assetsRoot)) { $assetsRoot = Join-Path $projectRoot "game" }
$assetComparisons = 0
if (Test-Path -LiteralPath $assetsRoot) {
    $stagedSizes = @{}
    foreach ($file in $stagedFiles) { $stagedSizes[$file.Length] = $true }
    $stagedHashes = @{}
    foreach ($file in $stagedFiles) {
        $stagedHashes[(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash] = $file.FullName
    }
    foreach ($asset in @(Get-ChildItem -LiteralPath $assetsRoot -File -Recurse)) {
        if (-not $stagedSizes.ContainsKey($asset.Length)) { continue }
        $assetComparisons++
        $hash = (Get-FileHash -LiteralPath $asset.FullName -Algorithm SHA256).Hash
        if ($stagedHashes.ContainsKey($hash)) {
            throw "Release file $($stagedHashes[$hash]) is identical to game data $($asset.FullName)"
        }
    }
}

# --- 5. The release ZIP ---------------------------------------------------------
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stageRoot, $zipPath,
    [System.IO.Compression.CompressionLevel]::Optimal, $true)

$releaseSize = ($stagedFiles | Measure-Object -Property Length -Sum).Sum
$zip = Get-Item -LiteralPath $zipPath
Write-Host "Portable release created."
Write-Host "Folder:   $stageRoot"
Write-Host "ZIP:      $zipPath"
Write-Host "Root:     $(@(Get-ChildItem -LiteralPath $stageRoot | ForEach-Object Name) -join ', ')"
Write-Host "Runtime:  $(@(Get-ChildItem -LiteralPath $runtimeStage -File -Recurse).Count) files in resources\runtime"
Write-Host "GPU ABI:  $gpuPluginAbi"
Write-Host "VC++ CRT: $VcRedistDir"
Write-Host "Audit:    no forbidden items; $assetComparisons same-size game files compared, none identical"
Write-Host ("Size:     {0:N2} MB uncompressed; {1:N2} MB ZIP" -f ($releaseSize / 1MB), ($zip.Length / 1MB))
Write-Host "SHA256:   $((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash)"
