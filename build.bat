@echo off
rem Builds crosshair.exe (with its icon). Uses MSVC if available (run from "x64 Native Tools
rem Command Prompt"), otherwise MinGW-w64 g++ if it's on PATH.
rem   build.bat           build, then wait for a key
rem   build.bat nopause   build only (used by installer\build_installer.bat)
setlocal
cd /d "%~dp0"
set RESULT=1
where cl >nul 2>nul && goto msvc
where g++ >nul 2>nul && goto mingw
echo No compiler found. Install Visual Studio Build Tools (C++ workload) or MinGW-w64, then rerun.
goto done

:msvc
set RES=
rc /nologo /fo crosshair.res crosshair.rc >nul 2>nul && set RES=crosshair.res
if not defined RES echo Note: rc.exe failed, building without the icon.
cl /nologo /O2 /EHsc /std:c++17 crosshair.cpp %RES% /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib advapi32.lib dwmapi.lib winmm.lib
set RESULT=%errorlevel%
goto done

:mingw
set RES=
windres crosshair.rc -O coff -o crosshair_res.o >nul 2>nul && set RES=crosshair_res.o
if not defined RES echo Note: windres failed, building without the icon.
g++ -O2 -s -std=c++17 -municode -mwindows -static crosshair.cpp %RES% -o crosshair.exe -ldwmapi -lwinmm -lshell32 -lgdi32 -luser32 -ladvapi32
set RESULT=%errorlevel%
goto done

:done
if /i not "%~1"=="nopause" pause
exit /b %RESULT%
