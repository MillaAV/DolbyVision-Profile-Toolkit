@echo off
setlocal EnableExtensions
set "DVE_SCRIPT=%~dp0scripts\uninstall.ps1"
if not exist "%DVE_SCRIPT%" (
    echo ERROR: Uninstall script not found.
    pause
    exit /b 1
)
cd /d "%SystemRoot%\System32"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%DVE_SCRIPT%"
if errorlevel 1 (
    echo ERROR: Could not start uninstaller.
    pause
)
exit /b
