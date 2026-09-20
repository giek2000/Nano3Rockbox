@echo off
REM ============================================================
REM  Rockbox Installer for iPod Nano 3G — FTL v2 build script
REM  Produces one self-contained EXE in dist\
REM ============================================================

setlocal

echo.
echo ========================================
echo  Building Nano3RockboxInstaller (FTL v2)
echo ========================================
echo.

REM The MSYS2 Python on PATH has no PyInstaller; use the py launcher,
REM which resolves the normal CPython installation.
py --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: the 'py' launcher was not found. Install Python from python.org.
    pause
    exit /b 1
)

py -m PyInstaller --version >nul 2>&1
if errorlevel 1 (
    echo Installing PyInstaller...
    py -m pip install pyinstaller
    if errorlevel 1 (
        echo ERROR: could not install PyInstaller.
        pause
        exit /b 1
    )
)

REM Refresh the internally consistent v2 payload: temporary/NOR bootloaders,
REM NAND checker, eraser, .rockbox tree, mks5lboot and Zadig when available.
echo Preparing FTL v2 payload...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0prepare_payload.ps1"
if errorlevel 1 (
    echo ERROR: payload preparation failed.
    pause
    exit /b 1
)

if not exist "%~dp0payload\mks5lboot.exe" (
    echo ERROR: payload is incomplete - mks5lboot.exe is missing.
    pause
    exit /b 1
)
if not exist "%~dp0payload\check.bin" (
    echo ERROR: payload is incomplete - check.bin is missing.
    pause
    exit /b 1
)

echo.
echo Building EXE...
py -m PyInstaller --clean --noconfirm "%~dp0Nano3RockboxInstaller.spec"
if errorlevel 1 (
    echo.
    echo ERROR: build failed.
    pause
    exit /b 1
)

echo.
echo ========================================
echo  Done: see dist\ for Nano3RockboxInstaller_v*-ftlv2.exe
echo ========================================
echo.
pause
