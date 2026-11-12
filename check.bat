@echo off
rem Run this on a computer where start.bat / presolve.bat do not work, then send check_result.txt
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check.ps1"
pause
