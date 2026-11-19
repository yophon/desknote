// 自绘界面的共用部分（跨平台）：系统深浅色主题、仿 Win11 的弹出菜单
#pragma once
#include "gfx.h"

namespace Ui
{
    // 颜色都是 0xAARRGGBB
    struct Theme
    {
        bool dark;
        u32 accent, onAccent;
        u32 winBg, cardBg, cardBg2, cardBorder;
        u32 menuBg, menuBorder;
        u32 text, text2, text3, disabled;
        u32 subtleHover, subtlePress, divider;
        u32 ctrlFill, ctrlHover, ctrlPress, ctrlBorder, ctrlBorderBottom;
        u32 strong, toggleOffFill, knob;
        u32 inputBg, inputFocusBg;
        u32 critical;
    };
    extern Theme T;
    void LoadTheme(); // 读系统深浅色和强调色

    u32 Solid(u32 over, u32 base); // 半透明色叠到不透明底色上

    // 画图形、文字的小工具在 gfx.h

    // ---------------------------------------------------------------
    // 弹出菜单
    // ---------------------------------------------------------------
    enum MenuKind { MkItem, MkSep, MkHeader, MkSwatches, MkSlider, MkSegment };
    struct Menu;
    struct MenuItem
    {
        int kind = MkItem;
        int id = 0;
        Str text, hint, desc;          // hint：右侧灰字（快捷键）；desc：第二行说明
        wchar_t icon = 0;
        bool check = false, checkable = false, radio = false, disabled = false;
        bool live = false;             // 点了不关菜单，只回调 live
        Menu* sub = nullptr;
        // 色块：id + 序号；customId 打开取色器
        const u32* colors = nullptr;
        int count = 0, sel = -1, customId = 0;
        u32 current = 0;
        // 滑块
        float minV = 0, maxV = 1, val = 0, step = 1;
        const wchar_t* unit = L"";
        // 分段按钮：id + 序号
        const wchar_t* const* labels = nullptr;
    };
    struct Menu
    {
        Vec<MenuItem> items;
        ~Menu();
        MenuItem& Item(int id, const wchar_t* text, wchar_t icon = 0, const wchar_t* hint = nullptr);
        void Sep();
        void Header(const Str& text);
        Menu* Sub(const wchar_t* text, wchar_t icon);
        MenuItem& Check(int id, const wchar_t* text, bool on, const wchar_t* desc = nullptr);
        MenuItem& Radio(int id, const wchar_t* text, const wchar_t* desc, bool on);
        MenuItem& Swatches(int id, const u32* colors, int n, int sel, int customId, u32 current);
        MenuItem& Slider(int id, const wchar_t* label, float minV, float maxV, float val, float step, const wchar_t* unit);
        MenuItem& Segment(int id, const wchar_t* label, const wchar_t* const* labels, int n, int sel);
    };
    typedef void (*MenuLiveFn)(void* ctx, int id, float value);
    // 在屏幕坐标 (sx, sy) 弹出，返回选中的命令（0 = 取消）；拖滑块、点色块时实时回调 live
    int MenuRun(void* owner, Menu* m, int sx, int sy, MenuLiveFn live, void* ctx); // owner：平台的窗口句柄
    bool MenuActive();
    extern int MenuDebugOpenSub; // 调界面用：弹出后自动展开第几个子菜单（从 1 数）
    extern bool MenuDebug;
    extern int ForceTheme;       // 调界面用：0 跟随系统，1 浅色，2 深色       // 调界面用：不因前台窗口变化而收起
}
