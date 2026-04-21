@echo off
setlocal
cd /d "%~dp0"

echo Starting kabu_micro_edge...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_live.ps1" %*
set EXIT_CODE=%ERRORLEVEL%

echo.
if not "%EXIT_CODE%"=="0" (
  echo kabu_micro_edge exited with code %EXIT_CODE%.
)
pause
exit /b %EXIT_CODE%
