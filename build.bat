@echo off
rem Windows build: MSVC (run inside "x64 Native Tools Command Prompt for VS") or MinGW-w64 g++
rem (on PATH, or at E:\foC++\ucrt64\bin)
cd /d "%~dp0"
where cl >nul 2>nul
if %errorlevel%==0 (
  cl /nologo /O2 /std:c++17 /EHsc /arch:AVX2 /Fe:solver.exe solver.cpp
  goto done
)
where g++ >nul 2>nul
if not %errorlevel%==0 if exist "E:\foC++\ucrt64\bin\g++.exe" set "PATH=E:\foC++\ucrt64\bin;%PATH%"
g++ -O3 -std=c++17 -march=x86-64-v3 -static -pthread solver.cpp -o solver.exe
:done
if exist solver.exe (echo OK: solver.exe config.txt   or double-click start.bat) else (echo Build failed & pause)
