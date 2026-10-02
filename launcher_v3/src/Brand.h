// Brand.h —— 启动器品牌名与版本统一引用点。
// 值由 launcher_v3/CMakeLists.txt 编译定义注入 (HS_APP_NAME / HERMES_V3_VERSION),
// 此处仅做宽字符合成; 代码里一律用宏, 禁止再写字面量品牌串 (防改名漂移)。
#pragma once

#ifndef HS_APP_NAME
#define HS_APP_NAME L"AgentDock"
#endif
#ifndef HERMES_V3_VERSION
#define HERMES_V3_VERSION "0.4.0"
#endif

#define _HS_WIDEN2(x) L##x
#define _HS_WIDEN(x) _HS_WIDEN2(x)
#define HS_VERSION_W _HS_WIDEN(HERMES_V3_VERSION)  // L"0.4.0"
