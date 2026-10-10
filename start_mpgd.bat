@echo off
setlocal EnableExtensions

REM ===========================================================================
REM  start_mpgd.bat - launcher for the mpgd pendant daemon (PlanetCNC TNG).
REM
REM  What it does:
REM   1. Self-elevates to Administrator (PlanetCNC in-process run requires it).
REM   2. Kills any already-running mpgd.exe before starting a fresh one.
REM   3. Adds the PlanetCNC install dir to PATH so PlanetCNCLib64.dll resolves.
REM
REM  Usage:  start_mpgd.bat [extra mpgd args, e.g. --no-gui]
REM ===========================================================================

REM --- 1. Self-elevate (fltmc works only when elevated) ---------------------
fltmc >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting Administrator privileges...
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "Start-Process -FilePath '%~f0' -Verb RunAs -WorkingDirectory '%~dp0'"
    exit /b
)

REM --- 2. Always work relative to this script's directory ------------------
cd /d "%~dp0"

REM --- 3. Make sure the binary exists --------------------------------------
if not exist "build\Release\mpgd.exe" (
    echo [ERROR] build\Release\mpgd.exe not found.
    echo         Build first:  cmake --build build --config Release
    pause
    exit /b 1
)

REM --- 4. Kill any already-running daemon(s) -------------------------------
tasklist /FI "IMAGENAME eq mpgd.exe" 2>nul | findstr /I /C:"mpgd.exe" >nul
if %errorlevel% equ 0 (
    echo Stopping running mpgd.exe...
    taskkill /F /IM mpgd.exe >nul 2>&1
    timeout /t 1 /nobreak >nul
) else (
    echo No running mpgd.exe detected.
)

REM --- 5. Locate the PlanetCNC DLL and start the daemon --------------------
if exist "C:\Program Files\PlanetCNC\PlanetCNCLib64.dll" (
    set "PATH=%PATH%;C:\Program Files\PlanetCNC"
) else (
    echo [WARN] C:\Program Files\PlanetCNC\PlanetCNCLib64.dll not found.
    echo        Set lib_path in config\mpgd.yaml or add the dir to PATH.
)

echo Starting mpgd daemon... press Ctrl+C to stop.
build\Release\mpgd.exe --config config\mpgd.yaml %*

endlocal
