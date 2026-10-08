@echo off
cmake --build "%~dp0out\build\win-amd64-release" --config Release
if errorlevel 1 (
    echo.
    echo Build failed. No release ZIP was created.
    pause
    exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Make-Release.ps1"
pause
