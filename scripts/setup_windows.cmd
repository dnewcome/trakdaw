@echo off
rem ----------------------------------------------------------------------
rem  setup_windows.cmd - one-time prep for the Windows native build.
rem
rem  Linux has system LuaJIT (pkg-config), Windows doesn't, so this script
rem  clones LuaJIT into vendor/LuaJIT and builds it with MSVC. The
rem  resulting lua51.lib / lua51.dll are picked up by CMakeLists.txt
rem  inside the if(WIN32) branch.
rem
rem  Requires: Visual Studio 2022 with the C++ workload + Windows SDK,
rem            and git on PATH.
rem
rem  Run from repo root:    scripts\setup_windows.cmd
rem ----------------------------------------------------------------------

setlocal

rem Move to repo root (script lives in scripts/).
pushd "%~dp0\.."

set "LUAJIT_DIR=vendor\LuaJIT"

if exist "%LUAJIT_DIR%\src\lua51.lib" (
    echo [setup] LuaJIT already built at %LUAJIT_DIR%\src\lua51.lib - skipping.
    goto :done
)

if not exist "%LUAJIT_DIR%\src\msvcbuild.bat" (
    echo [setup] Cloning LuaJIT into %LUAJIT_DIR% ...
    git clone --depth 1 https://github.com/LuaJIT/LuaJIT.git "%LUAJIT_DIR%"
    if errorlevel 1 (
        echo [setup] git clone failed.
        exit /b 1
    )
)

rem Locate Visual Studio 2022 via vswhere (Microsoft-supported stable path).
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [setup] vswhere not found at "%VSWHERE%" - is Visual Studio 2022 installed?
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALL=%%i"
)

if not defined VSINSTALL (
    echo [setup] No VS install found with the MSVC x64 C++ toolchain.
    exit /b 1
)

set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [setup] vcvars64.bat not found at "%VCVARS%".
    exit /b 1
)

echo [setup] Building LuaJIT with MSVC ...
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [setup] vcvars64.bat failed.
    exit /b 1
)

pushd "%LUAJIT_DIR%\src"
rem cmd on some Windows installs has NoDefaultCurrentDirectoryInExePath set,
rem so msvcbuild's bare "minilua" / "buildvm" invocations don't resolve.
set "PATH=%CD%;%PATH%"
call msvcbuild.bat
if errorlevel 1 (
    popd
    echo [setup] LuaJIT build failed.
    exit /b 1
)
popd

:done
echo.
echo [setup] LuaJIT ready. Now configure and build trakdaw:
echo.
echo   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
echo   cmake --build build --config Release --parallel
echo.

popd
endlocal
