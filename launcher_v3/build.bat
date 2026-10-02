@echo off
rem ============================================================
rem  launcher_v3 build script
rem  usage: build.bat [portable_root]
rem    1) MSVC env  2) cmake+ninja build  3) copy exe to portable root
rem  NOTE: keep this file ASCII-only (cmd parses bat as GBK)
rem ============================================================
setlocal
call "I:\HermesStudio\scripts\env_msvc.bat"
if errorlevel 1 exit /b 1

set CMAKE=I:\HermesStudio\_tmp\venv\Scripts\cmake.exe
set NINJA=I:\HermesStudio\_tmp\venv\Scripts\ninja.exe
set SRC=I:\HermesStudio\launcher_v3
set BLD=%SRC%\build

if not exist "%BLD%" mkdir "%BLD%"
rem single-file release: html/logo are compiled into exe resources.
rem copy /b self-append reliably refreshes mtime (type nul >> does NOT update
rem mtime on NTFS, ninja then skips the rc rebuild and embeds stale html)
copy /b "%SRC%\app.rc"+,, "%SRC%\app.rc" >nul
cd /d "%BLD%"
%CMAKE% -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "%SRC%"
if errorlevel 1 ( echo [ERROR] cmake configure failed & exit /b 1 )
%NINJA%
if errorlevel 1 ( echo [ERROR] build failed & exit /b 1 )
echo [OK] build: %BLD%\AgentDock.exe

if not "%~1"=="" (
    copy /Y "%BLD%\AgentDock.exe" "%~1\AgentDock.exe" >nul
    if errorlevel 1 ( echo [ERROR] deploy failed - is launcher still running? & exit /b 1 )
    echo [OK] deployed: %~1\AgentDock.exe
)
endlocal
