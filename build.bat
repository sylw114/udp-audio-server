@echo off
REM ============================================================================
REM build.bat - UDP C++ Audio Server CMake Build Script
REM ============================================================================

setlocal enabledelayedexpansion

echo ========================================
echo   UDP C++ Audio Server Build Script
echo ========================================
echo.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%i"
)

if not defined VS_PATH (
    echo [ERROR] Visual Studio with C++ tools not found.
    exit /b 1
)

echo [INFO] Found Visual Studio: %VS_PATH%

set "VCVARSALL=%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARSALL%" (
    echo [ERROR] vcvarsall.bat not found.
    exit /b 1
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cmake.exe not found.
    exit /b 1
)

echo [INFO] Setting up x64 build environment...
call "%VCVARSALL%" x64 >nul 2>&1

set "BUILD_DIR=build\cmake"
set "DIST_DIR=subbuild"

echo [INFO] Configuring CMake...
cmake -S . -B "%BUILD_DIR%" -G "Ninja" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo.
    echo [ERROR] CMake configure failed!
    exit /b 1
)

echo.
echo [INFO] Building...
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 (
    echo.
    echo [ERROR] Build failed!
    exit /b 1
)

echo.
echo [INFO] Publishing to %DIST_DIR%...
if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"
copy /Y "%BUILD_DIR%\audio_server_udp.exe" "%DIST_DIR%\audio_server_udp.exe" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy audio_server_udp.exe.
    exit /b 1
)
copy /Y "THIRD_PARTY_NOTICES.md" "%DIST_DIR%\THIRD_PARTY_NOTICES.md" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy THIRD_PARTY_NOTICES.md.
    exit /b 1
)

echo.
echo ========================================
echo   BUILD SUCCESSFUL: audio_server_udp.exe
echo ========================================

endlocal
