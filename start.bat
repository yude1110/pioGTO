@echo off
rem Open the 6-max table UI in your browser (keep this window open while using it)
cd /d "%~dp0"
if not exist solver.exe call build.bat
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0server.ps1"
echo.
echo The local server stopped. If there is a red error above, run check.bat and send check_result.txt.
pause
