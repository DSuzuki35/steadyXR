@echo off
setlocal
if exist "%~dp0preview\SteadyXR.exe" (
  start "" "%~dp0preview\SteadyXR.exe"
  exit /b 0
)
echo SteadyXR: The preview folder containing the app is missing.
echo Please extract the complete ZIP package.
pause
exit /b 1
