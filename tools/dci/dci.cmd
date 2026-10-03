@echo off
setlocal
where py >nul 2>nul
if %errorlevel%==0 (
  py -3 "%~dp0dci.py" %*
) else (
  python "%~dp0dci.py" %*
)
exit /b %errorlevel%
