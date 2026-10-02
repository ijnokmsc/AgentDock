@echo off
rem ============================================================
rem  deploy.bat - install AgentDock into the portable package.
rem
rem  1. windeployqt fills in the Qt runtime DLLs
rem  2. copy the exe itself
rem
rem  The portable package already contains python/ node/ webui/ git/
rem  and friends; this only ever ADDS Qt DLLs (a few tens of MB) and
rem  overwrites AgentDock.exe. Nothing else is touched.
rem
rem  ASCII-only on purpose - see the header of env_msvc.bat.
rem  Do NOT use "->" inside echo: cmd treats '>' as a redirect and
rem  writes a file instead of printing the message.
rem ============================================================
setlocal

set "ROOT=%~dp0.."
set "QT_DIR=%ROOT%\thirdparty\Qt6"

if not defined HS_DEST set "HS_DEST=I:\HermesPortable"
set "DEST=%HS_DEST%"

rem --- pick the Qt version dir that actually has windeployqt ---
set "QT_BIN="
for /d %%D in ("%QT_DIR%\*") do (
    if exist "%%~fD\msvc2022_64\bin\windeployqt.exe" set "QT_BIN=%%~fD\msvc2022_64\bin"
)
if not defined QT_BIN (
    echo [ERROR] windeployqt not found under %QT_DIR%
    echo         Run scripts\bootstrap_qt.py first.
    exit /b 1
)

set "BUILD=%ROOT%\build\AgentDock.exe"

if not exist "%BUILD%" (
    echo [ERROR] not built yet: %BUILD%
    echo         Run scripts\build.bat first.
    exit /b 1
)
if not exist "%DEST%\data" (
    echo [ERROR] target does not look like a Hermes portable package
    echo         ^(missing data\^): %DEST%
    echo         Override with:  set HS_DEST=D:\some\HermesPortable
    exit /b 1
)

echo [deploy] Qt runtime to %DEST%
"%QT_BIN%\windeployqt.exe" "%BUILD%" --dir "%DEST%" ^
    --no-translations ^
    --no-system-d3d-compiler ^
    --no-opengl-sw ^
    --no-compiler-runtime
if errorlevel 1 (
    echo [deploy] windeployqt failed
    exit /b 1
)

echo [deploy] copying AgentDock.exe
copy /Y "%BUILD%" "%DEST%\AgentDock.exe" >nul
if errorlevel 1 (
    echo [deploy] copy failed - is AgentDock still running?
    exit /b 1
)

echo.
echo [deploy] done: %DEST%\AgentDock.exe
echo          Double-click it to launch; Hermes.bat remains as fallback.
exit /b 0
