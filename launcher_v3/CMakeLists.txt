cmake_minimum_required(VERSION 3.15)
project(agentdock_v3 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# AgentDock 启动器 v3 —— WebView2 嵌入式生命周期启动器 (docs/ADR-007)
# 壳层: MainWnd 页签 + WebView2 双视图(探测回退) + FabBtn/FabMenu + 托盘/热键
# 核心层: src/core|app|platform|storage 纯 C++ 零 Qt 0 改动接入 (构建★/更新★/体检/Provider/模型/插件)
# 依赖: 系统 DLL + WebView2LoaderStatic; nlohmann/json 头文件; miniz(C)

# WebView2 SDK (nuget Microsoft.Web.WebView2, 静态加载)
set(WV2_ROOT   "${CMAKE_SOURCE_DIR}/../_tmp/wv2sdk")
set(WV2_INCLUDE "${WV2_ROOT}/build/native/include")
set(WV2_STATIC  "${WV2_ROOT}/build/native/x64/WebView2LoaderStatic.lib")

# ---------------------------------------------------------------------------
# 核心层 (与 UI 无关, 与 Qt6/FLTK 版共用同一组源文件)
# ---------------------------------------------------------------------------
set(HS_CORE_SRCS
    ../src/app/Paths.cpp

    ../src/core/ModelRegistry.cpp
    ../src/core/ConfigRenderer.cpp
    ../src/core/ProviderPresets.cpp
    ../src/core/AgentSpec.cpp
    ../src/core/PreflightChecker.cpp
    ../src/core/Launcher.cpp
    ../src/core/Updater.cpp
    ../src/core/UpdateManager.cpp
    ../src/core/PortableLayout.cpp
    ../src/core/PortableBuilder.cpp
    ../src/core/DshRuntime.cpp
    ../src/core/DshBuilder.cpp
    ../src/core/PluginHost.cpp

    ../src/platform/ProcessUtil.cpp
    ../src/platform/PortScanner.cpp
    ../src/platform/Hash.cpp
    ../src/platform/HttpClient.cpp
    ../src/platform/ProxyTester.cpp
    ../src/platform/Archive.cpp

    ../src/storage/JsonStore.cpp
)

# miniz (C 库) 按纯 C 编译, 避免 C++ name-mangling 导致 LNK 未解析
set(MINIZ_SRCS
    ../thirdparty/miniz/miniz.c
    ../thirdparty/miniz/miniz_tdef.c
    ../thirdparty/miniz/miniz_tinfl.c
    ../thirdparty/miniz/miniz_zip.c
)
foreach(mz ${MINIZ_SRCS})
    set_source_files_properties(${mz} PROPERTIES LANGUAGE C)
endforeach()

add_executable(AgentDock WIN32
    src/main.cpp
    src/Ui.cpp
    src/FabConfig.cpp
    src/HermesProc.cpp
    src/DshProc.cpp
    src/WebAgentProc.cpp
    src/LogSink.cpp
    src/TaskRunner.cpp
    src/Bridge.cpp
    src/FabBtn.cpp
    src/FabMenu.cpp
    src/DshProc.cpp
    src/WebViews.cpp
    src/MainWnd.cpp
    ${HS_CORE_SRCS}
    ${MINIZ_SRCS}
    app.rc
)

# 单文件发布: html/logo 经 app.rc RCDATA 嵌入。ninja 不跟踪 rc 内部引用的资源,
# 不声明依赖则改 HTML 不会触发 RC 重编, exe 永远嵌旧页面 (touch 的 mtime 更新也不可靠)
set_source_files_properties(app.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/shell/index.html;${CMAKE_SOURCE_DIR}/shell/logo.png")

target_include_directories(AgentDock PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/../src"              # core/Types.h 等 (核心层头)
    "${CMAKE_SOURCE_DIR}/../include"          # hs_plugin.h (插件 C ABI)
    "${CMAKE_SOURCE_DIR}/../thirdparty"       # nlohmann/json.hpp
    "${CMAKE_SOURCE_DIR}/../thirdparty/miniz" # miniz.h (Archive 用)
    "${WV2_INCLUDE}"
)

target_compile_definitions(AgentDock PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00 WINVER=0x0A00       # DPI_AWARENESS_CONTEXT / WebView2 头
    HERMES_V3_VERSION="0.1.0"               # 与发布仓库 release tag (v0.1.0) 对齐, 自更新比对基线
    HS_APP_NAME=L"AgentDock"                # 用户可见品牌名 (托盘/标题/弹窗统一引用)
)

target_link_libraries(AgentDock PRIVATE
    ${WV2_STATIC}
    user32 gdi32 gdiplus shell32 comctl32 ole32
    comdlg32      # GetOpenFileNameW (插件 zip 安装选包)
    iphlpapi      # GetExtendedTcpTable: 端口 -> PID (壳层 HermesProc + core PortScanner)
    psapi         # GetProcessMemoryInfo
    advapi32      # WebView2LoaderStatic + 注册表
    bcrypt        # core Hash (sha256)
    winhttp       # core HttpClient (构建下载/远端版本)
    ws2_32        # core PortScanner (ntohs/AF_INET)
    version       # GetFileVersionInfo (Updater 组件探测预留)
)

if(MSVC)
    target_compile_options(AgentDock PRIVATE /utf-8 /W3 /EHsc)
endif()

# app.rc 是 UTF-8 无 BOM: 老版 rc.exe 缺省按系统 ACP(GBK) 解析, 中文版本信息会乱码。
# /c65001 告诉 rc 源文件是 UTF-8 (BOM 方案在本 SDK 的 rc 10.0.10011 上直接 RC2135)。
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /c65001")
