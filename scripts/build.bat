@echo off
rem ============================================================
rem  build.bat - configure and build AgentDock.
rem
rem  Depends on:
rem    scripts\env_msvc.bat               (MSVC env by hand; the
rem                                        bundled VC has no
rem                                        vcvarsall.bat)
rem    _tmp\venv\Scripts\{cmake,ninja}.exe
rem
rem  ASCII-only on purpose - see the header of env_msvc.bat.
rem ============================================================
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
pushd "%ROOT%"

call "%~dp0env_msvc.bat"
if errorlevel 1 (
    echo [build] env_msvc.bat failed
    popd
    exit /b 1
)

set "CMAKE_EXE=%ROOT%\_tmp\venv\Scripts\cmake.exe"
set "NINJA_EXE=%ROOT%\_tmp\venv\Scripts\ninja.exe"

if not exist "%CMAKE_EXE%" (
    echo [ERROR] cmake not found: %CMAKE_EXE%
    echo         Run scripts\bootstrap_qt.py first.
    popd
    exit /b 1
)
if not exist "%NINJA_EXE%" (
    echo [ERROR] ninja not found: %NINJA_EXE%
    popd
    exit /b 1
)

if not exist build mkdir build

echo [build] configuring CMake ...
"%CMAKE_EXE%" -G Ninja -S . -B build ^
    -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" ^
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 (
    echo [build] CMake configure failed
    popd
    exit /b 1
)

echo [build] compiling ...
"%CMAKE_EXE%" --build build --parallel
if errorlevel 1 (
    echo [build] compile failed
    popd
    exit /b 1
)

echo.
echo [build] OK: build\AgentDock.exe   (+ smoke_test.exe)
popd
exit /b 0
