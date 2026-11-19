// 绘图接口的 Windows 实现要用的东西：Direct2D / DirectWrite 工厂，Painter 的定义。
// 只有 gfx_d2d.cpp 和画布（ui.cpp 里的 Canvas）引用；界面代码只认 gfx.h
#pragma once
#include "win.h"
#include "gfx.h"
#include <d2d1.h>
#include <dwrite.h>

struct Painter { ID2D1RenderTarget* rt = nullptr; };

namespace Gfx
{
    ID2D1Factory* D2D();
    IDWriteFactory* DW();
    D2D1_COLOR_F Color(u32 argb);
}
