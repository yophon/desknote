// 自绘界面的 Windows 部分：画布；主题和菜单的声明在 menu.h
#pragma once
#include "win.h"
#include "app.h"
#include "menu.h"

template <class T> inline void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// 画布：一块 32 位位图 + Direct2D 渲染目标；在上面画东西用 rt（gfx.h 的绘图接口）
// 分层窗口用 End(hwnd) 交给 UpdateLayeredWindow；普通窗口用 EndBlt(hdc) 贴到窗口上
struct ID2D1DCRenderTarget;
struct Canvas
{
    HDC screen = nullptr, mem = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    void* bits = nullptr;
    ID2D1DCRenderTarget* dc = nullptr;
    Painter* rt = nullptr;
    int pw = 0, ph = 0;

    bool Begin(int w, int h, float dpi, bool opaque = false);
    void Finish();
    void End(HWND hwnd);
    void EndBlt(HDC dst);
};

namespace Ui
{
    void AllowDarkControls(HWND h); // 让系统控件（输入框右键菜单等）跟随深色
    COLORREF Ref(u32 argb);        // 给 GDI 用
}
