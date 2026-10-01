@echo off
rem Builds crosshair.exe. Uses MSVC if available (run from "x64 Native Tools Command Prompt"),
rem otherwise MinGW-w64 g++ if it's on PATH.
cd /d "%~dp0"
where cl >nul 2>nul
if %errorlevel%==0 (
    cl /nologo /O2 /EHsc /std:c++17 crosshair.cpp /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib dwmapi.lib winmm.lib
    goto done
)
where g++ >nul 2>nul
if %errorlevel%==0 (
    g++ -O2 -std=c++17 -municode -mwindows -static crosshair.cpp -o crosshair.exe -ldwmapi -lwinmm -lshell32 -lgdi32 -luser32
    goto done
)
echo No compiler found. Install Visual Studio Build Tools (C++ workload) or MinGW-w64, then rerun.
:done
pause
