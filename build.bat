@echo off
REM ============================================================================
REM build.bat - UDP C++ Audio Server CMake Build Script
REM ============================================================================

setlocal enabledelayedexpansion

echo ========================================
echo   UDP C++ Audio Server Build Script
echo ========================================
echo.

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cmake.exe not found.
    exit /b 1
)

set "BUILD_DIR=build\cmake-msvc"
set "DIST_DIR=subbuild"

echo [INFO] Configuring CMake...
cmake -S . -B "%BUILD_DIR%" -A x64 -DCMAKE_INSTALL_PREFIX="%CD%\%DIST_DIR%"
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
echo [INFO] Installing to %DIST_DIR%...
cmake --install "%BUILD_DIR%" --config Release --component runtime
if errorlevel 1 (
    echo [ERROR] Install failed!
    exit /b 1
)

echo.
echo ========================================
echo   BUILD SUCCESSFUL: audio_server_udp.exe
echo ========================================

endlocal
