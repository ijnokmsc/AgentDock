cmake_minimum_required(VERSION 3.20)
project(AgentDock VERSION 0.1.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

# ---------------------------------------------------------------------------
# Qt 6 —— 优先用 thirdparty/Qt6 下由 scripts/bootstrap_qt.py 拉取的精简 Qt
# ---------------------------------------------------------------------------
set(HS_QT_ROOT "" CACHE PATH "Qt6 根目录 (留空则自动探测 thirdparty/Qt6)")

if(NOT HS_QT_ROOT)
    file(GLOB _qt_candidates "${CMAKE_SOURCE_DIR}/thirdparty/Qt6/*/msvc2022_64")
    if(_qt_candidates)
        list(GET _qt_candidates 0 HS_QT_ROOT)
    endif()
endif()

if(HS_QT_ROOT AND EXISTS "${HS_QT_ROOT}")
    set(CMAKE_PREFIX_PATH "${HS_QT_ROOT}" ${CMAKE_PREFIX_PATH})
    message(STATUS "[AgentDock] Qt: ${HS_QT_ROOT}")
else()
    message(STATUS "[AgentDock] Qt: 系统 find_package")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network)

# ---------------------------------------------------------------------------
# 源文件分组 —— 核心层 (与 UI 无关) 单独成表, 供 smoke_test 复用
# ---------------------------------------------------------------------------
set(HS_CORE_SRCS
    src/app/Paths.cpp

    src/core/ModelRegistry.cpp
    src/core/ConfigRenderer.cpp
    src/core/PreflightChecker.cpp
    src/core/Launcher.cpp
    src/core/Updater.cpp
    src/core/UpdateManager.cpp
    src/core/PortableLayout.cpp
    src/core/PortableBuilder.cpp
    src/core/PluginHost.cpp

    src/platform/ProcessUtil.cpp
    src/platform/PortScanner.cpp
    src/platform/Hash.cpp
    src/platform/HttpClient.cpp
    src/platform/Archive.cpp

    src/storage/JsonStore.cpp
)

set(HS_UI_SRCS
    src/ui/MainWindow.cpp
    src/ui/HomePage.cpp
    src/ui/DiagnosticsPage.cpp
    src/ui/ProvidersPage.cpp
    src/ui/ModelsPage.cpp
    src/ui/UpdatesPage.cpp
    src/ui/PluginsPage.cpp
    src/ui/SettingsPage.cpp
    src/ui/LogsPage.cpp
    src/ui/BuildPage.cpp
)

# ---------------------------------------------------------------------------
# 目标
# ---------------------------------------------------------------------------
add_executable(AgentDock WIN32
    src/main.cpp
    ${HS_CORE_SRCS}
    ${HS_UI_SRCS}
    resources/app.rc
)

# miniz (C 库) 源文件按 C 语言编译, 避免 C++ name-mangling 导致 LNK 未解析
set(MINIZ_SRCS
    thirdparty/miniz/miniz.c
    thirdparty/miniz/miniz_tdef.c
    thirdparty/miniz/miniz_tinfl.c
    thirdparty/miniz/miniz_zip.c
)
foreach(mz ${MINIZ_SRCS})
    set_source_files_properties(${mz} PROPERTIES LANGUAGE C)
endforeach()

# ---------------------------------------------------------------------------
# 共享配置 —— 主程序与测试目标保持一致, 避免 ODR / 宏差异
# ---------------------------------------------------------------------------
function(hs_configure_target tgt)
    target_sources(${tgt} PRIVATE ${MINIZ_SRCS})
    target_include_directories(${tgt} PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/thirdparty"
        "${CMAKE_SOURCE_DIR}/thirdparty/miniz"
    )
    target_link_libraries(${tgt} PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Concurrent
        Qt6::Network
        bcrypt
        iphlpapi
        ws2_32        # ntohs / AF_INET (PortScanner 用 GetExtendedTcpTable 查端口占用)
        version       # GetFileVersionInfo (后续 M3 读 exe 版本)
        winhttp
    )
    target_compile_definitions(${tgt} PRIVATE
        HERMESSTUDIO_VERSION="${PROJECT_VERSION}"
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
        # /utf-8: 源码含中文, MSVC 默认按 ACP 解析会踩坑
        target_compile_options(${tgt} PRIVATE /W3 /utf-8 /EHsc)
        # Qt 官方预编译库用 /MD, 这里必须保持一致, 不要设成 /MT
    endif()
endfunction()

hs_configure_target(AgentDock)

# ---------------------------------------------------------------------------
# 冒烟测试: 直接调核心层, 验证布局探测 / 组件探测 / 插件发现
#   cmake --build build --target smoke_test
# ---------------------------------------------------------------------------
add_executable(smoke_test
    tests/smoke_main.cpp
    ${HS_CORE_SRCS}
)
hs_configure_target(smoke_test)

# ---------------------------------------------------------------------------
# 端到端测试: 真起 web-ui + agent bridge, 再停干净
#   cmake --build build --target e2e_test
#   拷到便携包根目录双击/命令行运行
# ---------------------------------------------------------------------------
add_executable(e2e_test
    tests/e2e_main.cpp
    ${HS_CORE_SRCS}
)
hs_configure_target(e2e_test)

# ---------------------------------------------------------------------------
# 版本发现测试: 只读 GitHub releases, 打印 PortableBuilder::resolveLatest()
# 解析出的 runtime / web-ui 资产 (不下载)
#   cmake --build build --target resolve_test
# ---------------------------------------------------------------------------
add_executable(resolve_test
    tests/resolve_main.cpp
    ${HS_CORE_SRCS}
)
hs_configure_target(resolve_test)

# ---------------------------------------------------------------------------
# 远端版本源测试: 验证 Kernel/WebUI/Node/Python 的 checkRemote 拿到正确语义的
# 版本号 (内核与 Python 必须是 semver / 大版本, 不能是日期版), 以及 Node 版本列表。
#   cmake --build build --target remote_test
# ---------------------------------------------------------------------------
add_executable(remote_test
    tests/remote_main.cpp
    ${HS_CORE_SRCS}
)
hs_configure_target(remote_test)

# ---------------------------------------------------------------------------
# 部署 (可选): 拷贝 exe 到 Hermes 便携包根目录
# Qt DLL 由 scripts/deploy.bat 用 windeployqt 补齐
# ---------------------------------------------------------------------------
option(HS_DEPLOY_COPY "构建后自动拷贝 exe 到 I:/HermesPortable" OFF)

if(HS_DEPLOY_COPY)
    add_custom_command(TARGET AgentDock POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:AgentDock> "I:/HermesPortable/AgentDock.exe"
        COMMENT "拷贝 AgentDock.exe -> I:/HermesPortable"
        VERBATIM
    )
endif()
