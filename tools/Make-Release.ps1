# Builds the GoldenEye 007 release in out\release. The ZIP holds one folder:
#
#   GoldenEye 007 XBLA PC Recomp\
#     Setup GoldenEye 007.exe     Setup, carrying the payload ZIP below inside itself
#     README.txt
#     licenses\
#
# Setup creates Game\ beside itself from the payload and the player's package:
#
#     Game\
#       GoldenEye 007.exe         the game
#       rexruntime.dll, rexgpu-xenos.dll, VC++ runtime DLLs
#       ge.toml                   settings (only written when missing)
#       release-manifest.json     bytes + SHA-256 of every file Setup puts here
#       resources\                the port's own pictures and version.txt
#       assets\                   unpacked from the player's package
#       userdata\, logs\          saves, Setup and game logs
#
# The payload (staging\payload) is installer\ (Setup scripts) and game\ (the
# runtime above). The release carries no original game content; the audit below
# refuses to package anything that looks like game data, debug output or user state.
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
# Staged under out\release\staging so a copy someone installed and plays from
# out\release (with saves in its Game folder) is never touched.
$releaseName = "GoldenEye 007 XBLA PC Recomp"
$stageRoot = Join-Path $releaseRoot "staging\$releaseName"
$payloadRoot = Join-Path $releaseRoot "staging\payload"
$payloadZip = Join-Path $releaseRoot "staging\payload.zip"
$zipPath = Join-Path $releaseRoot "GoldenEye-007-XBLA-PC-Recomp-v$Version.zip"
$setupName = "Setup GoldenEye 007.exe"
$gameExeName = "GoldenEye 007.exe"

$runtimeFiles = @($gameExeName, "rexruntime.dll", "rexgpu-xenos.dll")
$vcRuntimeFiles = @("msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll")

# --- 1. Inputs ---------------------------------------------------------------
foreach ($file in $runtimeFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $buildRoot $file) -PathType Leaf)) {
        throw "Build output is missing: $file. Build $BuildPreset first."
    }
}

# A release must never ship a binary older than the source it claims to be.
$exeTime = (Get-Item -LiteralPath (Join-Path $buildRoot $gameExeName)).LastWriteTimeUtc
$newestSource = Get-ChildItem -LiteralPath (Join-Path $projectRoot "src") -File -Recurse |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if ($newestSource.LastWriteTimeUtc -gt $exeTime) {
    throw "$gameExeName is older than src\$($newestSource.Name). Rebuild before packaging."
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

# --- 2. Setup program -----------------------------------------------------------
& (Join-Path $packagingRoot "Build-Stubs.ps1") -Version $Version
$setupStub = Join-Path $packagingRoot $setupName
if (-not (Test-Path -LiteralPath $setupStub -PathType Leaf)) {
    throw "Setup was not built: $setupName"
}

# --- 3. Stage -----------------------------------------------------------------
if (Test-Path -LiteralPath (Join-Path $stageRoot "Game")) {
    throw "The staging folder holds an installed Game folder (saves?). Move it away first: $stageRoot"
}
foreach ($old in @($stageRoot, $payloadRoot, $payloadZip, $zipPath)) {
    if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Recurse -Force }
}

$licensesStage = Join-Path $stageRoot "licenses"
$installerStage = Join-Path $payloadRoot "installer"
$gameStage = Join-Path $payloadRoot "game"
$resourcesStage = Join-Path $gameStage "resources"
foreach ($dir in @($stageRoot, $licensesStage, $installerStage, $gameStage, $resourcesStage)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

# Release root: README and licences (Setup is added once the payload is in it).
$readme = [System.IO.File]::ReadAllText((Join-Path $packagingRoot "README.txt")).Replace("@VERSION@", $Version)
$readme = $readme -replace "`r?`n", "`r`n"  # Notepad-friendly line endings
[System.IO.File]::WriteAllText((Join-Path $stageRoot "README.txt"), $readme, (New-Object System.Text.UTF8Encoding($false)))
Get-ChildItem -LiteralPath (Join-Path $packagingRoot "licenses") -File |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $licensesStage }

# Payload: Setup's scripts.
Get-ChildItem -LiteralPath (Join-Path $packagingRoot "resources\installer") -File |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $installerStage }
[System.IO.File]::WriteAllText((Join-Path $installerStage "version.txt"), $Version)

# Payload: the runtime that goes into Game\.
foreach ($file in $runtimeFiles) { Copy-Item -LiteralPath (Join-Path $buildRoot $file) -Destination $gameStage }
foreach ($file in $vcRuntimeFiles) { Copy-Item -LiteralPath (Join-Path $VcRedistDir $file) -Destination $gameStage }
Copy-Item -LiteralPath (Join-Path $packagingRoot "ge.toml") -Destination $gameStage
# Game\resources: the desk picture behind the front-end folder on wide screens,
# the re-painted menu folder textures, the character select pictures and the
# version shown on the main menu.
foreach ($shape in @("169", "219")) {
    Copy-Item -LiteralPath (Join-Path $packagingRoot "menu-backdrop-$shape.png") -Destination $resourcesStage
}
foreach ($folder in @("textures", "portraits")) {
    # Optional: the game falls back to the original pictures without them.
    if (-not (Test-Path -LiteralPath (Join-Path $packagingRoot $folder))) { continue }
    $target = Join-Path $resourcesStage $folder
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $packagingRoot $folder) -Filter *.png -File |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $target }
}
[System.IO.File]::WriteAllText((Join-Path $resourcesStage "version.txt"), $Version)

# --- 4. Audit: nothing from the game package, no debug output, no user state -
$staged = @(Get-ChildItem -LiteralPath $stageRoot -Recurse -Force) + @(Get-ChildItem -LiteralPath $payloadRoot -Recurse -Force)
# The release root holds Setup, README and licences only; Setup makes Game\.
foreach ($item in @(Get-ChildItem -LiteralPath $stageRoot -Force)) {
    if (@("README.txt", "licenses") -notcontains $item.Name) {
        throw "Unexpected item in the release root: $($item.Name)"
    }
}
$forbiddenNames = @("assets", "assets_import_tmp", "userdata", "logs", "cache",
    "default.xex", "ArcadeInfo.xml", "Bean.xex", "music.xwb", "sfx.xwb", "files",
    "guest_image.bin", "xstr_decompressed.bin")
$forbiddenPatterns = @("*.pdb", "*.xex", "*.bin", "*.log", "*.ilk", "*.exp", "*.lib",
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

# --- 5. Manifest (Game\release-manifest.json, paths relative to Game) ---------
$gameFiles = @(Get-ChildItem -LiteralPath $gameStage -File -Recurse)
$entries = New-Object System.Collections.Generic.List[object]
foreach ($file in $gameFiles) {
    $entries.Add([pscustomobject]@{
            Path = $file.FullName.Substring($gameStage.Length + 1)
            Bytes = $file.Length
            Sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        })
}
$entries.Sort([System.Comparison[object]] { param($a, $b) [string]::CompareOrdinal($a.Path, $b.Path) })

$json = New-Object System.Text.StringBuilder
[void]$json.Append("{`n  `"release`": `"$releaseName`",`n  `"version`": `"$Version`",`n  `"files`": [`n")
for ($i = 0; $i -lt $entries.Count; $i++) {
    $entry = $entries[$i]
    $path = $entry.Path.Replace('\', '\\').Replace('"', '\"')
    [void]$json.Append("    {`n")
    [void]$json.Append("      `"bytes`": $($entry.Bytes),`n")
    [void]$json.Append("      `"path`": `"$path`",`n")
    [void]$json.Append("      `"sha256`": `"$($entry.Sha256)`"`n")
    [void]$json.Append("    }")
    if ($i -lt $entries.Count - 1) { [void]$json.Append(",") }
    [void]$json.Append("`n")
}
[void]$json.Append("  ]`n}`n")
[System.IO.File]::WriteAllText((Join-Path $gameStage "release-manifest.json"), $json.ToString(),
    (New-Object System.Text.UTF8Encoding($false)))

# --- 6. Setup with the payload inside, then the release ZIP -------------------
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($payloadRoot, $payloadZip,
    [System.IO.Compression.CompressionLevel]::Optimal, $false)
# Setup = the stub, the payload ZIP, then "GE7SETUP" + the ZIP's offset (UInt64 LE).
$setupTarget = Join-Path $stageRoot $setupName
$stubBytes = [System.IO.File]::ReadAllBytes($setupStub)
$out = [System.IO.File]::Create($setupTarget)
try {
    $out.Write($stubBytes, 0, $stubBytes.Length)
    $payloadStream = [System.IO.File]::OpenRead($payloadZip)
    try { $payloadStream.CopyTo($out) } finally { $payloadStream.Dispose() }
    $magic = [System.Text.Encoding]::ASCII.GetBytes("GE7SETUP")
    $out.Write($magic, 0, 8)
    $offset = [System.BitConverter]::GetBytes([uint64]$stubBytes.Length)
    $out.Write($offset, 0, 8)
}
finally {
    $out.Dispose()
}

[System.IO.Compression.ZipFile]::CreateFromDirectory($stageRoot, $zipPath,
    [System.IO.Compression.CompressionLevel]::Optimal, $true)

$stagedFiles = @(Get-ChildItem -LiteralPath $stageRoot -File -Recurse)
$releaseSize = ($stagedFiles | Measure-Object -Property Length -Sum).Sum
$zip = Get-Item -LiteralPath $zipPath
Write-Host "Portable release created."
Write-Host "Folder:   $stageRoot"
Write-Host "ZIP:      $zipPath"
Write-Host "Root:     $(@(Get-ChildItem -LiteralPath $stageRoot | ForEach-Object Name) -join ', ')"
Write-Host "Game:     $($entries.Count) files + release-manifest.json (inside Setup)"
Write-Host "GPU ABI:  $gpuPluginAbi"
Write-Host "VC++ CRT: $VcRedistDir"
Write-Host "Audit:    no forbidden items; $assetComparisons same-size game files compared, none identical"
Write-Host ("Size:     {0:N2} MB uncompressed; {1:N2} MB ZIP" -f ($releaseSize / 1MB), ($zip.Length / 1MB))
Write-Host "SHA256:   $((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash)"
