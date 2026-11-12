@echo off
rem Pre-solve every spot/flop listed in presolve.txt into the library folder.
rem Close the window any time; run again to continue where it stopped.
cd /d "%~dp0"
if not exist solver.exe call build.bat
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0presolve.ps1"
pause
