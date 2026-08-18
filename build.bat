@echo off
REM ============================================================================
REM build.bat - UDP C++ Audio Server CMake Build Script
REM ============================================================================

REM MSBuild treats Path and PATH as duplicate environment variables. Some managed
REM terminals expose both spellings, so relaunch once with a canonical Path key.
if defined LIVESUITE_BUILD_ENV_NORMALIZED goto :environment_normalized
set "LIVESUITE_BUILD_SCRIPT=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$pathValue = [Environment]::GetEnvironmentVariable('Path', 'Process'); [Environment]::SetEnvironmentVariable('PATH', $null, 'Process'); [Environment]::SetEnvironmentVariable('Path', $pathValue, 'Process'); $env:LIVESUITE_BUILD_ENV_NORMALIZED = '1'; & $env:LIVESUITE_BUILD_SCRIPT; exit $LASTEXITCODE"
exit /b %ERRORLEVEL%

:environment_normalized

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

set "SOURCE_DIR=%~dp0."
set "BUILD_DIR=%~dp0build\cmake-msvc"
set "DIST_DIR=%~dp0subbuild"

echo [INFO] Configuring CMake...
cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -A x64 -DCMAKE_INSTALL_PREFIX="%DIST_DIR%"
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
