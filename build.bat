@echo off
setlocal

set "BUILD_DIR=build"

if not exist "%BUILD_DIR%" (
    mkdir "%BUILD_DIR%"
)

cd "%BUILD_DIR%"

echo [*] Running CMake...
cmake .. -G "Visual Studio 17 2022" -A x64
if %ERRORLEVEL% NEQ 0 (
    echo [!] CMake configuration failed.
    exit /b %ERRORLEVEL%
)

echo [*] Building project (Release)...
cmake --build . --config Release
if %ERRORLEVEL% NEQ 0 (
    echo [!] Build failed.
    exit /b %ERRORLEVEL%
)

echo [+] Build successful! Output: %BUILD_DIR%\Release\RadeonOCSetter.exe
endlocal
