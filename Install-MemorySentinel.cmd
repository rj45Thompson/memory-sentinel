@echo off
rem Memory Sentinel installer. Double-click it and pick your .uproject, or run:
rem   Install-MemorySentinel.cmd -Project "C:\path\to\MyGame.uproject" -Yes
rem It runs Install-MemorySentinel.ps1 with the execution policy relaxed for this one PowerShell process only.
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Install-MemorySentinel.ps1" %*
set "RESULT=%ERRORLEVEL%"
if "%~1"=="" pause
exit /b %RESULT%
