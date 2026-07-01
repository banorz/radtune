@echo off
setlocal

set "BUILD_DIR=build"

REM CMake generator.
REM   - No argument: CMake auto-selects the newest Visual Studio installed
REM     (so VS 2022, 2026, ... all work without editing this file).
REM   - Override by passing a generator name, e.g.:
REM       build.bat "Visual Studio 18 2026"
REM       build.bat "Visual Studio 17 2022"
set "GENERATOR=%~1"

if not exist "%BUILD_DIR%" (
    mkdir "%BUILD_DIR%"
)

cd "%BUILD_DIR%"

echo [*] Running CMake...
if "%GENERATOR%"=="" (
    echo     Generator: auto-detect ^(newest Visual Studio installed^)
    cmake .. -A x64
) else (
    echo     Generator: %GENERATOR%
    cmake .. -G "%GENERATOR%" -A x64
)
if %ERRORLEVEL% NEQ 0 (
    echo [!] CMake configuration failed.
    echo     Tip: if you switched Visual Studio versions, delete the "%BUILD_DIR%" folder and retry.
    exit /b %ERRORLEVEL%
)

echo [*] Building project (Release)...
cmake --build . --config Release
if %ERRORLEVEL% NEQ 0 (
    echo [!] Build failed.
    exit /b %ERRORLEVEL%
)

echo [+] Build successful! Output: %BUILD_DIR%\Release\RadTune.exe and RadTuneGUI.exe
endlocal
