@echo off
REM ============================================================
REM  Rockbox Installer for iPod Nano 3G - build script
REM  Produces a single self-contained EXE in dist\
REM ============================================================

echo.
echo ========================================
echo  Building Nano3RockboxInstaller
echo ========================================
echo.

REM The MSYS2 python on PATH has no PyInstaller; use the py launcher, which
REM resolves the real CPython install.
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

REM Refresh the embedded payload (tool, bootloaders, .rockbox tree, Zadig).
echo Preparing payload...
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
echo  Done: dist\Nano3RockboxInstaller_v1.1.0.exe
echo ========================================
echo.
pause
