@echo off
rem ============================================================
rem  build_clear_logs.bat - build the clear-logs plugin DLL and
rem  its selftest console exe (both land in examples\).
rem  ASCII-only on purpose.
rem ============================================================
setlocal
call "%~dp0..\scripts\env_msvc.bat"
if errorlevel 1 (
    echo [build] env_msvc.bat failed
    exit /b 1
)
cd /d "%~dp0"

cl /nologo /LD /EHsc /O2 /W4 /utf-8 /I..\include clear_logs_plugin.cpp ^
   /Fe:clear_logs_plugin.dll /link /EXPORT:hs_plugin_entry
if errorlevel 1 (
    echo [build] plugin dll failed
    exit /b 1
)
cl /nologo /EHsc /O2 /W4 /utf-8 /I..\include clear_logs_selftest.cpp ^
   /Fe:clear_logs_selftest.exe
if errorlevel 1 (
    echo [build] selftest failed
    exit /b 1
)
echo [OK] built clear_logs_plugin.dll + clear_logs_selftest.exe
endlocal
