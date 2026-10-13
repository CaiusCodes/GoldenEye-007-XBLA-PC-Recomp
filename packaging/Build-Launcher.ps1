# Builds the launcher into packaging\:
#   GoldenEye 007.exe     the release's only program at the top level (packaging\launcher\*.cpp)
# It starts the game in resources\runtime, and shows the setup window first when
# the game data has not been imported yet. A plain native Win32 program: static
# CRT (it needs nothing installed to start), no packing, no embedded payload,
# no scripts. It carries the default ge.toml and the player-name list as
# resources, plus the game icon and version information.
[CmdletBinding()]
param([string]$Version)

$ErrorActionPreference = 'Stop'
$packaging = $PSScriptRoot
$projectRoot = Split-Path -Parent $packaging
if (-not $Version) {
    $Version = ([System.IO.File]::ReadAllText((Join-Path $projectRoot 'VERSION'))).Trim()
}
if ($Version -notmatch '^[0-9][0-9A-Za-z.-]*$') { throw "Unexpected version text: $Version" }
$llvm = 'C:\Program Files\LLVM\bin'
$clang = Join-Path $llvm 'clang-cl.exe'
$rc = Join-Path $llvm 'llvm-rc.exe'
foreach ($tool in @($clang, $rc)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Missing build tool: $tool" }
}

# The game's own icon (the crosshair), the same one the game window uses.
$icon = Join-Path $projectRoot 'Icon.ico'
if (-not (Test-Path -LiteralPath $icon)) {
    & (Join-Path $packaging 'Make-IconFromPng.ps1') -Source (Join-Path $packaging 'art\game-crosshair.png') -Output $icon
}

# "0.2.0-preview" -> 0,2,0,0 for the binary version fields.
$parts = @(($Version -replace '[^0-9.].*$', '').Split('.') | Where-Object { $_ -ne '' })
while ($parts.Count -lt 4) { $parts += '0' }
$fileVersion = ($parts[0..3]) -join ','

$sourceDir = Join-Path $packaging 'launcher'
$output = Join-Path $packaging 'GoldenEye 007.exe'
$work = Join-Path $env:TEMP ('ge-launcher-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    Copy-Item -LiteralPath $icon -Destination (Join-Path $work 'app.ico')
    Copy-Item -LiteralPath (Join-Path $packaging 'ge.toml') -Destination (Join-Path $work 'ge.toml')
    Copy-Item -LiteralPath (Join-Path $packaging 'player-names.txt') -Destination (Join-Path $work 'player-names.txt')
    # asInvoker: never asks for administrator rights. PerMonitorV2: the setup
    # window scales itself for each monitor's DPI instead of being stretched.
    Set-Content -LiteralPath (Join-Path $work 'app.manifest') -Encoding ASCII -Value @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="GoldenEye007.Launcher" version="1.0.0.0" processorArchitecture="amd64"/>
  <description>GoldenEye 007 XBLA PC Recomp</description>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security><requestedPrivileges><requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security>
  </trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application><supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/></application>
  </compatibility>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
      <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
    </windowsSettings>
  </application>
</assembly>
'@
    $rcPath = Join-Path $work 'launcher.rc'
    Set-Content -LiteralPath $rcPath -Encoding ASCII -Value @"
1 ICON "app.ico"
1 24 "app.manifest"
101 RCDATA "ge.toml"
102 RCDATA "player-names.txt"
1 VERSIONINFO
 FILEVERSION $fileVersion
 PRODUCTVERSION $fileVersion
 FILEOS 0x40004
 FILETYPE 0x1
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "GoldenEye 007 XBLA PC Recomp project"
      VALUE "FileDescription", "GoldenEye 007"
      VALUE "ProductName", "GoldenEye 007 XBLA PC Recomp"
      VALUE "ProductVersion", "$Version"
      VALUE "FileVersion", "$Version"
      VALUE "InternalName", "GoldenEye 007 launcher"
      VALUE "OriginalFilename", "GoldenEye 007.exe"
      VALUE "LegalCopyright", "Open source (GPL-3.0 launcher; Unlicense and MIT game), see licenses folder"
      VALUE "Comments", "https://github.com/CaiusCodes/GoldenEye-007-XBLA-PC-Recomp"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"@
    $resPath = Join-Path $work 'launcher.res'
    Push-Location $work
    try {
        & $rc /nologo /FO $resPath $rcPath
        if ($LASTEXITCODE -ne 0) { throw 'llvm-rc failed for the launcher' }
    }
    finally {
        Pop-Location
    }

    $sources = @(Get-ChildItem -LiteralPath $sourceDir -Filter *.cpp | ForEach-Object FullName)
    & $clang /nologo /std:c++17 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE /DNOMINMAX "/DGE_VERSION=$Version" `
        "/I$sourceDir" "/Fo$work\" @sources "/Fe$output" /link /SUBSYSTEM:WINDOWS `
        user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib uuid.lib bcrypt.lib $resPath
    if ($LASTEXITCODE -ne 0) { throw 'clang-cl failed for the launcher' }
    Write-Host ('Built {0} ({1:N0} bytes)' -f (Split-Path -Leaf $output), (Get-Item -LiteralPath $output).Length)
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
