@echo off
setlocal
if exist "%~dp0preview\horizonlock-preview.exe" (
  start "" "%~dp0preview\horizonlock-preview.exe"
  exit /b 0
)
echo Horizon Lock VR: Der Ordner preview mit der App fehlt.
echo Bitte das komplette ZIP entpacken.
pause
exit /b 1
