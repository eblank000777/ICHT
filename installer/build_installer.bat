@echo off
rem Builds CrosshairSetup.exe in the repository root: compiles crosshair.exe with ..\build.bat,
rem then packs it with NSIS (https://nsis.sourceforge.io).
setlocal
cd /d "%~dp0"
call ..\build.bat nopause
if errorlevel 1 (
    echo Building crosshair.exe failed.
    goto end
)
cd /d "%~dp0"
set MAKENSIS=
where makensis >nul 2>nul && set MAKENSIS=makensis
if not defined MAKENSIS if exist "%ProgramFiles(x86)%\NSIS\makensis.exe" set MAKENSIS="%ProgramFiles(x86)%\NSIS\makensis.exe"
if not defined MAKENSIS if exist "%ProgramFiles%\NSIS\makensis.exe" set MAKENSIS="%ProgramFiles%\NSIS\makensis.exe"
if not defined MAKENSIS (
    echo NSIS not found. Install it from https://nsis.sourceforge.io and rerun.
    goto end
)
%MAKENSIS% /V2 crosshair.nsi && echo. && echo Built ..\CrosshairSetup.exe
:end
pause
