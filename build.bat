@echo off
REM ============================================================================
REM build.bat - UDP C++ Audio Server Build Script
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

echo [INFO] Setting up x64 build environment...
call "%VCVARSALL%" x64 >nul 2>&1

echo [INFO] Compiling...
echo.

cl.exe /nologo /EHsc /O2 /utf-8 /std:c++17 /W3 ^
    /D_UNICODE /DUNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    main.cpp wasapi_renderer.cpp ^
    /Fe:./subbuild/audio_server_udp.exe ^
    /link ws2_32.lib ole32.lib avrt.lib uuid.lib

if errorlevel 1 (
    echo.
    echo [ERROR] Compilation failed!
    exit /b 1
)

echo.
echo ========================================
echo   BUILD SUCCESSFUL: audio_server_udp.exe
echo ========================================

endlocal
