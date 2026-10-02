@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0zyn.ps1" %*
exit /b %ERRORLEVEL%
