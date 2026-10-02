@echo off
rem ============================================================
rem  env_msvc.bat - compose the MSVC command-line environment.
rem
rem  The VC toolchain bundled with the IDE is a trimmed build that
rem  ships no VC\Auxiliary\Build\vcvarsall.bat, so INCLUDE / LIB /
rem  PATH are assembled by hand here.
rem
rem  Usage:  call scripts\env_msvc.bat
rem          (deliberately NO setlocal - the variables must leak
rem           into the caller's environment)
rem
rem  IMPORTANT: keep this file ASCII-only.
rem  cmd.exe decodes a .bat using the console codepage (GBK on a
rem  zh-CN box), so UTF-8 CJK text inside a batch file derails
rem  parsing - CJK lines get executed as if they were commands.
rem  The toolchain path itself contains CJK, so it is auto-detected
rem  instead of hardcoded. Override with HS_VCROOT / HS_WKROOT /
rem  HS_WKVER if detection picks the wrong one.
rem ============================================================

if not defined HS_WKVER set "HS_WKVER=10.0.19041.0"

set "VCROOT=%HS_VCROOT%"
set "WKROOT=%HS_WKROOT%"
set "WKVER=%HS_WKVER%"

rem ------------------------------------------------------------
rem  locate the VC toolchain:
rem    <some>:\<ide>\data\VC\VC2022\<version>\bin\Hostx64\x64\cl.exe
rem  NOTE: "if defined" is evaluated at run time (not parse time), so
rem  it is safe to test inside a for-block without delayed expansion.
rem ------------------------------------------------------------
if not defined VCROOT (
    for %%D in (C D E F G H I J) do (
        if not defined VCROOT if exist "%%D:\" (
            for /d %%A in (%%D:\*) do (
                if not defined VCROOT if exist "%%~fA\data\VC\VC2022" (
                    for /d %%B in ("%%~fA\data\VC\VC2022\*") do (
                        if not defined VCROOT if exist "%%~fB\bin\Hostx64\x64\cl.exe" set "VCROOT=%%~fB"
                    )
                )
            )
        )
    )
)

if not defined VCROOT (
    echo [ERROR] cl.exe not found. Scanned C:\..J:\ for
    echo           *\data\VC\VC2022\*\bin\Hostx64\x64\cl.exe
    echo         Set HS_VCROOT to the versioned VC dir and retry, e.g.
    echo           set HS_VCROOT=D:\path\data\VC\VC2022\14.41.34120
    exit /b 1
)

rem ------------------------------------------------------------
rem  Windows Kits sits next to VC2022: <ide>\data\VC\Windows Kits\10
rem ------------------------------------------------------------
rem  NOTE: cannot do this inside an if-block - %VCDATA% would be
rem  expanded at parse time (empty). Use a label instead.
if defined WKROOT goto :wk_ready
for %%P in ("%VCROOT%\..\..") do set "VCDATA=%%~fP"
set "WKROOT=%VCDATA%\Windows Kits\10"
:wk_ready

if not exist "%VCROOT%\bin\Hostx64\x64\cl.exe" (
    echo [ERROR] cl.exe not found: %VCROOT%\bin\Hostx64\x64\cl.exe
    exit /b 1
)
if not exist "%WKROOT%\Include\%WKVER%\ucrt" (
    echo [ERROR] Windows SDK not found: %WKROOT%\Include\%WKVER%
    echo         Set HS_WKROOT / HS_WKVER and retry.
    exit /b 1
)

set "PATH=%VCROOT%\bin\Hostx64\x64;%WKROOT%\bin\%WKVER%\x64;%PATH%"

set "INCLUDE=%VCROOT%\include;%VCROOT%\atlmfc\include;%WKROOT%\Include\%WKVER%\ucrt;%WKROOT%\Include\%WKVER%\shared;%WKROOT%\Include\%WKVER%\um;%WKROOT%\Include\%WKVER%\winrt"

set "LIB=%VCROOT%\lib\x64;%VCROOT%\atlmfc\lib\x64;%WKROOT%\Lib\%WKVER%\ucrt\x64;%WKROOT%\Lib\%WKVER%\um\x64"

echo [env_msvc] MSVC + Windows SDK %WKVER% ready
exit /b 0
