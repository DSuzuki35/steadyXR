@echo off
cd /d "%~dp0sender"
vrst-sender.exe %*
if errorlevel 1 pause
