// 窗口接口（ui/window.h）的 Windows 实现里，程序主体还要直接用的几个函数
#pragma once
#include "win.h"
#include "window.h"

namespace Wnd
{
    void RegisterClasses();
    bool PreTranslate(MSG* msg);   // 消息循环里先给它：对话框的 Tab 切换、Esc 关闭
    inline HWND H(Window* w) { return (HWND)Native(w); }
}
