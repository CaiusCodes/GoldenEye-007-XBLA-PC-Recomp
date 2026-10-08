# Builds the native Setup program into packaging\:
#   Setup GoldenEye 007.exe     Setup   (installer_stub.cpp)
# Make-Release appends the payload ZIP to a copy of it. Static CRT, so it
# does not need the Visual C++ runtime to start. (Releases before 0.4.41 also
# had a play launcher beside Setup; the game now runs as Game\GoldenEye 007.exe.)
[CmdletBinding()]
param([string]$Version)

$ErrorActionPreference = 'Stop'
$packaging = $PSScriptRoot
if (-not $Version) {
    $Version = ([System.IO.File]::ReadAllText((Join-Path $packaging '..\VERSION'))).Trim()
}
$llvm = 'C:\Program Files\LLVM\bin'
$clang = Join-Path $llvm 'clang-cl.exe'
$rc = Join-Path $llvm 'llvm-rc.exe'
foreach ($tool in @($clang, $rc)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Missing build tool: $tool" }
}

# Two icon files: the game's crosshair icon and Setup's folder icon. Both are
# tracked; if one is missing it is rebuilt from its artwork in packaging\art.
$icon = Join-Path $packaging 'goldeneye.ico'
$setupIcon = Join-Path $packaging 'setup_icon.ico'
$makeIcon = Join-Path $packaging 'Make-IconFromPng.ps1'
if (-not (Test-Path -LiteralPath $icon)) {
    & $makeIcon -Source (Join-Path $packaging 'art\game-crosshair.png') -Output $icon
}
if (-not (Test-Path -LiteralPath $setupIcon)) {
    & $makeIcon -Source (Join-Path $packaging 'art\setup-folder.png') -Output $setupIcon
}

# "0.2.0-preview" -> 0,2,0,0 for the binary version fields.
$parts = @(($Version -replace '[^0-9.].*$', '').Split('.') | Where-Object { $_ -ne '' })
while ($parts.Count -lt 4) { $parts += '0' }
$fileVersion = ($parts[0..3]) -join ','

$work = Join-Path $env:TEMP ('ge-stubs-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    Set-Content -LiteralPath (Join-Path $work 'app.manifest') -Encoding ASCII -Value @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security><requestedPrivileges><requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security>
  </trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application><supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/></application>
  </compatibility>
</assembly>
'@

    $stubs = @(
        @{ Source = 'installer_stub.cpp'; Output = 'Setup GoldenEye 007.exe'; Description = 'GoldenEye 007 PC Setup'; Icon = $setupIcon }
    )
    foreach ($stub in $stubs) {
        $name = [IO.Path]::GetFileNameWithoutExtension($stub.Source)
        Copy-Item -LiteralPath $stub.Icon -Destination (Join-Path $work 'app.ico') -Force
        $rcPath = Join-Path $work "$name.rc"
        # Windows "installer detection" can force elevation on programs named
        # like setup tools; the asInvoker manifest (RT_MANIFEST = 24) stops it.
        Set-Content -LiteralPath $rcPath -Encoding ASCII -Value @"
1 ICON "app.ico"
1 24 "app.manifest"
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
      VALUE "FileDescription", "$($stub.Description)"
      VALUE "ProductName", "GoldenEye 007 PC"
      VALUE "ProductVersion", "$Version"
      VALUE "FileVersion", "$Version"
      VALUE "OriginalFilename", "$($stub.Output)"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"@
        $resPath = Join-Path $work "$name.res"
        Push-Location $work
        try {
            & $rc /nologo /FO $resPath $rcPath
            if ($LASTEXITCODE -ne 0) { throw "llvm-rc failed for $($stub.Source)" }
        }
        finally {
            Pop-Location
        }

        $output = Join-Path $packaging $stub.Output
        & $clang /nologo /std:c++17 /O2 /MT /EHsc /DUNICODE /D_UNICODE "/Fo$work\$name.obj" (Join-Path $packaging $stub.Source) "/Fe$output" /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib $resPath
        if ($LASTEXITCODE -ne 0) { throw "clang-cl failed for $($stub.Source)" }
        Write-Host ('Built {0} ({1:N0} bytes)' -f $stub.Output, (Get-Item -LiteralPath $output).Length)
    }
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
