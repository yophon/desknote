// Windows 这边的公共头：先引 windows.h，再引跨平台的核心
#pragma once
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "rt.h"
#include "version.h"

void SplitTime(Time t, SYSTEMTIME* st); // 界面代码用 SYSTEMTIME，换算在 plat_win.cpp
extern HINSTANCE g_inst;
extern HICON g_icon, g_iconSmall; // 程序图标（app_win.cpp 启动时加载）
