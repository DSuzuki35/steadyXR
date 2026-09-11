@echo off
setlocal
if exist "%~dp0preview\layers\LayerEditor.ps1" (
  start "" "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0preview\layers\LayerEditor.ps1"
  exit /b 0
)
echo SteadyXR: The preview folder containing the layer editor is missing.
echo Please extract the complete ZIP package.
pause
exit /b 1
