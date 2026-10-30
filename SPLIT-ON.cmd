@echo off
setlocal
fltmc >nul 2>&1
if errorlevel 1 (
    set "DVE_CMD=%~f0"
    powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command "$p=$env:DVE_CMD; Start-Process -FilePath 'cmd.exe' -ArgumentList ('/d /c ""{0}""' -f $p) -Verb RunAs -WorkingDirectory (Split-Path -Parent $p)"
    exit /b
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\set-split.ps1" -Mode ON
pause
