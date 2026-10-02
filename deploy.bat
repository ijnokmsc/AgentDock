@echo off
rem ============================================================
rem  AgentDock 一键打包部署脚本
rem  用法: 在 HermesStudio 工程目录 (含 CMakeLists.txt) 下运行
rem      deploy.bat [便携包根目录]
rem  默认目标: ..\HermesPortable  (exe 同级, 即工程目录的上级)
rem
rem  步骤: 1) 定位 MSVC 环境  2) cmake 配置+构建  3) 复制 exe 到便携包
rem        4) windeployqt 补齐 Qt DLL (若便携包内缺 Qt)
rem ============================================================

setlocal enabledelayedexpansion

set "PROJ=%~dp0"
cd /d "%PROJ%"

rem ---- 目标便携包根 ----
if "%~1"=="" (
    set "PORTABLE=%~dp0..\HermesPortable"
) else (
    set "PORTABLE=%~1"
)
if not exist "%PORTABLE%\data" (
    echo [ERROR] 便携包根目录无效: %PORTABLE%
    exit /b 1
)

rem ---- 1) 定位 MSVC (尝试 vcvarsall, 失败则提示) ----
set "VSCMD="
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VSROOT=%%i"
)
if defined VSROOT if exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
    call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
    echo [OK] MSVC 环境已加载: %VSROOT%
) else (
    echo [WARN] 未找到 vcvarsall。请确保已在开发者命令行中运行本脚本。
    echo        (或手动设置 INCLUDE / LIB / PATH 指向 MSVC + Windows SDK)
)

rem ---- 2) CMake 构建 ----
where cmake >nul 2>&1
if errorlevel 1 (
    echo [ERROR] 未找到 cmake, 请安装或加入 PATH。
    exit /b 1
)
if not exist build mkdir build
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 ( echo [ERROR] CMake 配置失败 & exit /b 1 )
cmake --build build --parallel
if errorlevel 1 ( echo [ERROR] 编译失败 & exit /b 1 )
echo [OK] 编译完成

rem ---- 3) 复制 exe ----
copy /Y "build\AgentDock.exe" "%PORTABLE%\AgentDock.exe" >nul
if errorlevel 1 ( echo [ERROR] 复制 exe 失败 & exit /b 1 )
echo [OK] exe 已复制到 %PORTABLE%\AgentDock.exe

rem ---- 4) windeployqt 补齐 Qt DLL ----
rem   若便携包根缺 Qt6Core.dll 则运行 windeployqt
if not exist "%PORTABLE%\Qt6Core.dll" (
    where windeployqt >nul 2>&1
    if errorlevel 1 (
        echo [WARN] 便携包缺 Qt DLL 且未找到 windeployqt, 请在带 Qt 的环境手动部署。
    ) else (
        windeployqt --dir "%PORTABLE%" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "build\AgentDock.exe"
        if errorlevel 1 ( echo [WARN] windeployqt 部署有警告 ) else ( echo [OK] Qt DLL 已部署 )
    )
) else (
    echo [OK] Qt DLL 已存在
)

echo.
echo ============================================
echo  部署完成: %PORTABLE%\AgentDock.exe
echo ============================================
endlocal
