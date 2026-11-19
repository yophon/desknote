// 自绘界面的 Windows 部分：画布（位图 + Direct2D，贴到分层窗口或普通窗口）、读系统主题
#include "ui.h"
#include "gfx_d2d.h"
#include <windowsx.h>
#include <uxtheme.h>

// =====================================================================
// 画布
// =====================================================================
bool Canvas::Begin(int w, int h, float dpi, bool opaque)
{
    pw = Max(w, 1); ph = Max(h, 1);
    screen = GetDC(nullptr);
    mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = pw; bi.bmiHeader.biHeight = -ph;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return false;
    old = SelectObject(mem, bmp);
    D2D1_RENDER_TARGET_PROPERTIES props = {};
    props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = opaque ? D2D1_ALPHA_MODE_IGNORE : D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = props.dpiY = dpi;
    HRESULT hr = Gfx::D2D()->CreateDCRenderTarget(&props, &dc);
    if (FAILED(hr)) { Str m(L"CreateDCRenderTarget 失败 hr="); m.Int((i64)(u32)hr); Log(m); return false; }
    RECT rc = { 0, 0, pw, ph };
    dc->BindDC(mem, &rc);
    dc->BeginDraw();
    D2D1_COLOR_F clear = {};
    dc->Clear(&clear);
    rt = new Painter();
    rt->rt = dc;
    return true;
}

void Canvas::Finish()
{
    if (dc)
    {
        HRESULT hr = dc->EndDraw();
        if (FAILED(hr)) { Str m(L"EndDraw 失败 hr="); m.Int((i64)(u32)hr); Log(m); }
        dc->Release();
        dc = nullptr;
    }
    delete rt;
    rt = nullptr;
    GdiFlush();
}

void Canvas::End(HWND hwnd)
{
    Finish();
    POINT src = { 0, 0 };
    SIZE sz = { pw, ph };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    if (!UpdateLayeredWindow(hwnd, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA))
    {
        Str m(L"UpdateLayeredWindow 失败 err=");
        m.Int(GetLastError()).Append(L" size=").Int(pw).Ch(L'x').Int(ph).Append(L" bmp=").Int(bmp ? 1 : 0);
        Log(m);
    }
    if (old) SelectObject(mem, old);
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    if (screen) ReleaseDC(nullptr, screen);
    Gfx::Touch();
}

void Canvas::EndBlt(HDC dst)
{
    Finish();
    BitBlt(dst, 0, 0, pw, ph, mem, 0, 0, SRCCOPY);
    if (old) SelectObject(mem, old);
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    if (screen) ReleaseDC(nullptr, screen);
    Gfx::Touch();
}

namespace Ui
{
// =====================================================================
// 主题：跟随系统深浅色，强调色取系统的（深色用浅一档，浅色用深一档，和 Win11 控件一致）
// =====================================================================

static bool AccentFromPalette(int index, u32* out)
{
    BYTE b[32];
    DWORD sz = sizeof(b);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette",
                     RRF_RT_REG_BINARY, nullptr, b, &sz) != ERROR_SUCCESS || sz < 32)
        return false;
    *out = 0xFF000000u | ((u32)b[index * 4] << 16) | ((u32)b[index * 4 + 1] << 8) | b[index * 4 + 2];
    return true;
}

typedef int(WINAPI* SetPreferredAppModeFn)(int);
typedef BOOL(WINAPI* AllowDarkModeForWindowFn)(HWND, BOOL);
typedef void(WINAPI* FlushMenuThemesFn)();
static HMODULE g_uxtheme;

void LoadTheme()
{
    DWORD light = 1, sz = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &light, &sz);
    bool dark = ForceTheme ? ForceTheme == 2 : light == 0;
    T.dark = dark;
    u32 acc;
    if (!AccentFromPalette(dark ? 1 : 4, &acc)) acc = dark ? 0xFF60CDFF : 0xFF005FB8;
    T.accent = acc;
    if (dark)
    {
        T.onAccent = 0xFF000000;
        T.winBg = 0xFF202020; T.cardBg = 0xFF2B2B2B; T.cardBg2 = 0xFF272727; T.cardBorder = 0xFF1C1C1C;
        T.menuBg = 0xFF2C2C2C; T.menuBorder = 0x24FFFFFF;
        T.text = 0xFFFFFFFF; T.text2 = 0xC5FFFFFF; T.text3 = 0x87FFFFFF; T.disabled = 0x5DFFFFFF;
        T.subtleHover = 0x0FFFFFFF; T.subtlePress = 0x0AFFFFFF; T.divider = 0x15FFFFFF;
        T.ctrlFill = 0x0FFFFFFF; T.ctrlHover = 0x15FFFFFF; T.ctrlPress = 0x08FFFFFF; T.ctrlBorder = 0x12FFFFFF; T.ctrlBorderBottom = 0x12FFFFFF;
        T.strong = 0x8BFFFFFF; T.toggleOffFill = 0x19000000; T.knob = 0xFF454545;
        T.inputBg = Solid(0x0FFFFFFF, T.cardBg2); T.inputFocusBg = 0xFF1F1F1F;
        T.critical = 0xFFFF99A4;
    }
    else
    {
        T.onAccent = 0xFFFFFFFF;
        T.winBg = 0xFFF3F3F3; T.cardBg = 0xFFFBFBFB; T.cardBg2 = 0xFFF7F7F7; T.cardBorder = 0xFFE5E5E5;
        T.menuBg = 0xFFF9F9F9; T.menuBorder = 0x1A000000;
        T.text = 0xE4000000; T.text2 = 0x9E000000; T.text3 = 0x72000000; T.disabled = 0x5C000000;
        T.subtleHover = 0x09000000; T.subtlePress = 0x06000000; T.divider = 0x0F000000;
        T.ctrlFill = 0xB3FFFFFF; T.ctrlHover = 0x80F9F9F9; T.ctrlPress = 0x4DF9F9F9; T.ctrlBorder = 0x0F000000; T.ctrlBorderBottom = 0x29000000;
        T.strong = 0x72000000; T.toggleOffFill = 0x06000000; T.knob = 0xFFFFFFFF;
        T.inputBg = 0xFFFDFDFD; T.inputFocusBg = 0xFFFFFFFF;
        T.critical = 0xFFC42B1C;
    }
    // 系统控件（输入框的右键菜单等）也跟着深浅色走；用的是 uxtheme 未公开的序号导出，拿不到就算了
    if (!g_uxtheme) g_uxtheme = LoadLibraryW(L"uxtheme.dll");
    if (g_uxtheme)
    {
        SetPreferredAppModeFn setMode = (SetPreferredAppModeFn)GetProcAddress(g_uxtheme, MAKEINTRESOURCEA(135));
        FlushMenuThemesFn flush = (FlushMenuThemesFn)GetProcAddress(g_uxtheme, MAKEINTRESOURCEA(136));
        if (setMode) setMode(1); // AllowDark：跟随系统
        if (flush) flush();
    }
}

void AllowDarkControls(HWND h)
{
    if (!g_uxtheme) return;
    AllowDarkModeForWindowFn allow = (AllowDarkModeForWindowFn)GetProcAddress(g_uxtheme, MAKEINTRESOURCEA(133));
    if (allow) allow(h, TRUE);
    SetWindowTheme(h, T.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}


COLORREF Ref(u32 argb) { return RGB((argb >> 16) & 255, (argb >> 8) & 255, argb & 255); }
}
