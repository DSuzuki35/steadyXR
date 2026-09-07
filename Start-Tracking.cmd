@echo off
setlocal
cd /d "%~dp0sender"
vrst-sender.exe %*
set "vrst_exit=%errorlevel%"
if not "%vrst_exit%"=="0" (
    echo VR Stabilizer stopped. Exit code: %vrst_exit%
    pause
)
exit /b %vrst_exit%
