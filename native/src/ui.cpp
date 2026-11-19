// 自绘界面：系统主题、画布、绘制小工具、仿 Win11 的弹出菜单
#include "ui.h"
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
    HRESULT hr = Gfx::D2D()->CreateDCRenderTarget(&props, &rt);
    if (FAILED(hr)) { Str m(L"CreateDCRenderTarget 失败 hr="); m.Int((i64)(u32)hr); Log(m); return false; }
    RECT rc = { 0, 0, pw, ph };
    rt->BindDC(mem, &rc);
    rt->BeginDraw();
    D2D1_COLOR_F clear = {};
    rt->Clear(&clear);
    return true;
}

void Canvas::Finish()
{
    if (rt)
    {
        HRESULT hr = rt->EndDraw();
        if (FAILED(hr)) { Str m(L"EndDraw 失败 hr="); m.Int((i64)(u32)hr); Log(m); }
        rt->Release();
        rt = nullptr;
    }
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
Theme T;
int MenuDebugOpenSub = 0;
bool MenuDebug = false;
int ForceTheme = 0;

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

D2D1_COLOR_F C(u32 argb)
{
    D2D1_COLOR_F c = { ((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f, ((argb >> 24) & 255) / 255.f };
    return c;
}

u32 Solid(u32 over, u32 base)
{
    u32 a = over >> 24;
    u32 r = (((over >> 16) & 255) * a + ((base >> 16) & 255) * (255 - a)) / 255;
    u32 g = (((over >> 8) & 255) * a + ((base >> 8) & 255) * (255 - a)) / 255;
    u32 b = ((over & 255) * a + (base & 255) * (255 - a)) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

COLORREF Ref(u32 argb) { return RGB((argb >> 16) & 255, (argb >> 8) & 255, argb & 255); }

// =====================================================================
// 绘制小工具（单位 DIP）
// =====================================================================
static ID2D1SolidColorBrush* MakeBrush(ID2D1RenderTarget* rt, u32 argb)
{
    ID2D1SolidColorBrush* b = nullptr;
    D2D1_COLOR_F c = C(argb);
    rt->CreateSolidColorBrush(&c, nullptr, &b);
    return b;
}

void Fill(ID2D1RenderTarget* rt, D2D1_RECT_F r, float radius, u32 argb)
{
    ID2D1SolidColorBrush* b = MakeBrush(rt, argb);
    if (!b) return;
    if (radius > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
    else rt->FillRectangle(&r, b);
    b->Release();
}

void FillCorners(ID2D1RenderTarget* rt, D2D1_RECT_F r, float tl, float tr, float br, float bl, u32 argb)
{
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* s = nullptr;
    Gfx::D2D()->CreatePathGeometry(&g);
    if (!g) return;
    g->Open(&s);
    s->BeginFigure(D2D1::Point2F(r.left + tl, r.top), D2D1_FIGURE_BEGIN_FILLED);
    s->AddLine(D2D1::Point2F(r.right - tr, r.top));
    if (tr > 0) s->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.right, r.top + tr), D2D1::SizeF(tr, tr), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    s->AddLine(D2D1::Point2F(r.right, r.bottom - br));
    if (br > 0) s->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.right - br, r.bottom), D2D1::SizeF(br, br), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    s->AddLine(D2D1::Point2F(r.left + bl, r.bottom));
    if (bl > 0) s->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.left, r.bottom - bl), D2D1::SizeF(bl, bl), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    s->AddLine(D2D1::Point2F(r.left, r.top + tl));
    if (tl > 0) s->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.left + tl, r.top), D2D1::SizeF(tl, tl), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    ID2D1SolidColorBrush* b = MakeBrush(rt, argb);
    if (b) { rt->FillGeometry(g, b); b->Release(); }
    s->Release();
    g->Release();
}

void Stroke(ID2D1RenderTarget* rt, D2D1_RECT_F r, float radius, u32 argb, float width)
{
    ID2D1SolidColorBrush* b = MakeBrush(rt, argb);
    if (!b) return;
    float h = width / 2;
    D2D1_RECT_F in = { r.left + h, r.top + h, r.right - h, r.bottom - h };
    if (radius > 0) rt->DrawRoundedRectangle(D2D1::RoundedRect(in, Max(0.f, radius - h), Max(0.f, radius - h)), b, width);
    else rt->DrawRectangle(&in, b, width);
    b->Release();
}

void Circle(ID2D1RenderTarget* rt, float cx, float cy, float radius, u32 argb)
{
    ID2D1SolidColorBrush* b = MakeBrush(rt, argb);
    if (!b) return;
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), b);
    b->Release();
}

void Ring(ID2D1RenderTarget* rt, float cx, float cy, float radius, u32 argb, float width)
{
    ID2D1SolidColorBrush* b = MakeBrush(rt, argb);
    if (!b) return;
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), b, width);
    b->Release();
}

void Line(ID2D1RenderTarget* rt, float x0, float y0, float x1, float y1, u32 argb, float width)
{
    ID2D1SolidColorBrush* b = MakeBrush(rt, argb);
    if (!b) return;
    rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), b, width);
    b->Release();
}

// 用一圈圈逐渐放大的半透明圆角矩形叠出柔和阴影（软件渲染没有模糊特效可用）
void Shadow(ID2D1RenderTarget* rt, D2D1_RECT_F r, float radius, int layers, float alpha, float dy)
{
    ID2D1SolidColorBrush* b = MakeBrush(rt, 0xFF000000);
    if (!b) return;
    b->SetOpacity(alpha);
    for (int k = layers; k >= 1; k--)
    {
        D2D1_RECT_F e = { r.left - k, r.top - k + dy, r.right + k, r.bottom + k + dy };
        rt->FillRoundedRectangle(D2D1::RoundedRect(e, radius + k, radius + k), b);
    }
    b->Release();
}

// 字体：Segoe UI Variable（Win11 的界面字体），中文由系统回退到微软雅黑；没有就用 Segoe UI
static const wchar_t* UiFamily()
{
    static int has = -1;
    if (has < 0)
    {
        has = 0;
        IDWriteFontCollection* fc = nullptr;
        if (SUCCEEDED(Gfx::DW()->GetSystemFontCollection(&fc, FALSE)) && fc)
        {
            UINT32 idx;
            BOOL exists = FALSE;
            fc->FindFamilyName(L"Segoe UI Variable Text", &idx, &exists);
            has = exists ? 1 : 0;
            fc->Release();
        }
    }
    return has ? L"Segoe UI Variable Text" : L"Segoe UI";
}

static IDWriteTextFormat* Format(float size, int weight, bool wrap)
{
    IDWriteTextFormat* f = nullptr;
    Gfx::DW()->CreateTextFormat(UiFamily(), nullptr, (DWRITE_FONT_WEIGHT)weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                size, L"zh-cn", &f);
    if (!f) return nullptr;
    float lh = (float)(int)(size * 1.43f + 0.5f);
    f->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lh, lh * 0.76f);
    if (!wrap)
    {
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        IDWriteInlineObject* sign = nullptr;
        Gfx::DW()->CreateEllipsisTrimmingSign(f, &sign);
        DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        f->SetTrimming(&trim, sign);
        SafeRelease(sign);
    }
    return f;
}

static IDWriteTextLayout* Layout(const Str& s, float size, int weight, bool wrap, float w)
{
    IDWriteTextFormat* f = Format(size, weight, wrap);
    if (!f) return nullptr;
    IDWriteTextLayout* l = nullptr;
    Gfx::DW()->CreateTextLayout(s.c(), s.n, f, Max(w, 1.f), 100000.f, &l);
    f->Release();
    return l;
}

float Text(ID2D1RenderTarget* rt, const Str& s, D2D1_RECT_F r, float size, int weight, u32 argb, int align)
{
    if (!s.n) return 0;
    IDWriteTextLayout* l = Layout(s, size, weight, false, r.right - r.left);
    if (!l) return 0;
    DWRITE_TEXT_METRICS m = {};
    l->GetMetrics(&m);
    if (align == Center) l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    else if (align == Right) l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    ID2D1SolidColorBrush* b = MakeBrush(rt, argb);
    if (b)
    {
        float y = r.top + ((r.bottom - r.top) - m.height) / 2;
        rt->DrawTextLayout(D2D1::Point2F(r.left, y), l, b, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        b->Release();
    }
    l->Release();
    return Min(m.widthIncludingTrailingWhitespace, r.right - r.left);
}

float TextWrap(ID2D1RenderTarget* rt, const Str& s, float x, float y, float w, float size, int weight, u32 argb)
{
    if (!s.n) return 0;
    IDWriteTextLayout* l = Layout(s, size, weight, true, w);
    if (!l) return 0;
    DWRITE_TEXT_METRICS m = {};
    l->GetMetrics(&m);
    if (rt)
    {
        ID2D1SolidColorBrush* b = MakeBrush(rt, argb);
        if (b) { rt->DrawTextLayout(D2D1::Point2F(x, y), l, b, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT); b->Release(); }
    }
    l->Release();
    return m.height;
}

float Measure(const Str& s, float size, int weight)
{
    if (!s.n) return 0;
    IDWriteTextLayout* l = Layout(s, size, weight, false, 100000.f);
    if (!l) return 0;
    DWRITE_TEXT_METRICS m = {};
    l->GetMetrics(&m);
    l->Release();
    return m.widthIncludingTrailingWhitespace;
}

float MeasureWrap(const Str& s, float w, float size, int weight) { return TextWrap(nullptr, s, 0, 0, w, size, weight, 0); }

void Icon(ID2D1RenderTarget* rt, wchar_t glyph, float cx, float cy, float size, u32 argb)
{
    static int fluent = -1;
    if (fluent < 0)
    {
        fluent = 0;
        IDWriteFontCollection* fc = nullptr;
        if (SUCCEEDED(Gfx::DW()->GetSystemFontCollection(&fc, FALSE)) && fc)
        {
            UINT32 idx;
            BOOL exists = FALSE;
            fc->FindFamilyName(L"Segoe Fluent Icons", &idx, &exists);
            fluent = exists ? 1 : 0;
            fc->Release();
        }
    }
    IDWriteTextFormat* f = nullptr;
    Gfx::DW()->CreateTextFormat(fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    if (!f) return;
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ID2D1SolidColorBrush* b = MakeBrush(rt, argb);
    if (b)
    {
        D2D1_RECT_F r = { cx - size, cy - size, cx + size, cy + size };
        rt->DrawTextW(&glyph, 1, f, &r, b, D2D1_DRAW_TEXT_OPTIONS_NONE);
        b->Release();
    }
    f->Release();
}

// =====================================================================
// 菜单数据
// =====================================================================
Menu::~Menu()
{
    for (int i = 0; i < items.n; i++) delete items[i].sub;
}

MenuItem& Menu::Item(int id, const wchar_t* text, wchar_t icon, const wchar_t* hint)
{
    MenuItem& it = items.PushDefault();
    it.id = id;
    it.text = text;
    it.icon = icon;
    if (hint) it.hint = hint;
    return it;
}

void Menu::Sep()
{
    if (!items.n || items.Last().kind == MkSep) return;
    items.PushDefault().kind = MkSep;
}

void Menu::Header(const Str& text)
{
    MenuItem& it = items.PushDefault();
    it.kind = MkHeader;
    it.text = text;
}

Menu* Menu::Sub(const wchar_t* text, wchar_t icon)
{
    MenuItem& it = Item(0, text, icon);
    it.sub = new Menu();
    return it.sub;
}

MenuItem& Menu::Check(int id, const wchar_t* text, bool on, const wchar_t* desc)
{
    MenuItem& it = Item(id, text);
    it.checkable = true;
    it.check = on;
    if (desc) it.desc = desc;
    return it;
}

MenuItem& Menu::Radio(int id, const wchar_t* text, const wchar_t* desc, bool on)
{
    MenuItem& it = Item(id, text);
    it.radio = true;
    it.check = on;
    if (desc) it.desc = desc;
    return it;
}

MenuItem& Menu::Swatches(int id, const u32* colors, int n, int sel, int customId, u32 current)
{
    MenuItem& it = items.PushDefault();
    it.kind = MkSwatches;
    it.id = id;
    it.colors = colors;
    it.count = n;
    it.sel = sel;
    it.customId = customId;
    it.current = current;
    return it;
}

MenuItem& Menu::Slider(int id, const wchar_t* label, float minV, float maxV, float val, float step, const wchar_t* unit)
{
    MenuItem& it = items.PushDefault();
    it.kind = MkSlider;
    it.id = id;
    it.text = label;
    it.minV = minV; it.maxV = maxV; it.val = Clamp(val, minV, maxV); it.step = step;
    it.unit = unit;
    return it;
}

MenuItem& Menu::Segment(int id, const wchar_t* label, const wchar_t* const* labels, int n, int sel)
{
    MenuItem& it = items.PushDefault();
    it.kind = MkSegment;
    it.id = id;
    it.text = label;
    it.labels = labels;
    it.count = n;
    it.sel = sel;
    return it;
}

// =====================================================================
// 菜单窗口：每一级一个分层窗口，四周留出画阴影的边
// =====================================================================
static const float Margin = 16, Pad = 4, Inset = 4, Radius = 8;
static const float ItemH = 32, DescItemH = 50, SepH = 9, HeaderH = 30, SwatchH = 40, SliderH = 40, SegH = 44;
static const float SwD = 20, SwGap = 8;

struct MenuLevel
{
    Menu* m = nullptr;
    HWND hwnd = nullptr;
    int parentItem = -1;
    float S = 1;
    int cx = 0, cy = 0;   // 内容区左上角，屏幕像素
    float w = 0, h = 0;   // 内容区大小，DIP
    float textLeft = 16;
    Vec<float> tops, heights;
    int hot = -1, hotPart = -1;
};

struct MenuSession
{
    HWND owner = nullptr;
    HWND fg = nullptr;
    Vec<MenuLevel*> levels;
    MenuLiveFn live = nullptr;
    void* ctx = nullptr;
    int result = 0;
    bool done = false;
    int pressLevel = -1, pressItem = -1, pressPart = -1;
    int dragLevel = -1, dragItem = -1;
    int timerLevel = -1;
};

static MenuSession* g_ms;
static HHOOK g_hook;
static const wchar_t* MenuClass = L"DeskNoteMenu";
bool MenuActive() { return g_ms != nullptr; }

static bool Selectable(const MenuItem& it) { return it.kind != MkSep && it.kind != MkHeader && !it.disabled; }

static float ItemHeight(const MenuItem& it)
{
    switch (it.kind)
    {
    case MkSep: return SepH;
    case MkHeader: return HeaderH;
    case MkSwatches: return SwatchH;
    case MkSlider: return SliderH;
    case MkSegment: return SegH;
    }
    return it.desc.n ? DescItemH : ItemH;
}

static float SegWidth(const MenuItem& it)
{
    float w = 0;
    for (int i = 0; i < it.count; i++) w += Max(44.f, Measure(Str(it.labels[i]), 13, 400) + 20);
    return w;
}

static void LayoutLevel(MenuLevel* lv)
{
    Menu* m = lv->m;
    bool anyIcon = false;
    for (int i = 0; i < m->items.n; i++)
    {
        const MenuItem& it = m->items[i];
        if (it.kind == MkItem && (it.icon || (it.checkable && !it.live) || it.radio)) anyIcon = true;
    }
    lv->textLeft = anyIcon ? Inset + 12 + 16 + 12 : Inset + 12;
    float w = 180;
    lv->tops.Clear();
    lv->heights.Clear();
    float y = Pad;
    for (int i = 0; i < m->items.n; i++)
    {
        const MenuItem& it = m->items[i];
        float need = 0;
        switch (it.kind)
        {
        case MkItem:
        {
            float tw = Measure(it.text, 14, 400);
            if (it.desc.n) tw = Max(tw, Measure(it.desc, 12, 400));
            need = lv->textLeft + tw + 12 + Inset;
            if (it.hint.n) need += 28 + Measure(it.hint, 12, 400);
            if (it.sub) need += 28;
            if (it.live && it.checkable) need += 40 + 24 + Inset + 12 - lv->textLeft;
            break;
        }
        case MkHeader: need = lv->textLeft + Min(Measure(it.text, 12, 400), 220.f) + 12 + Inset; break;
        case MkSwatches: need = (Inset + 12) * 2 + (it.count + 1) * SwD + it.count * SwGap; break;
        case MkSlider: need = 290; break;
        case MkSegment: need = Inset + 12 + 56 + SegWidth(it) + 12 + Inset; break;
        }
        w = Max(w, need);
        lv->tops.Push(y);
        float h = ItemHeight(it);
        lv->heights.Push(h);
        y += h;
    }
    lv->w = (float)(int)Min(w, 380.f);
    lv->h = y + Pad;
}

static D2D1_RECT_F RowRect(MenuLevel* lv, int i)
{
    return D2D1::RectF(Margin + Inset, Margin + lv->tops[i], Margin + lv->w - Inset, Margin + lv->tops[i] + lv->heights[i]);
}

// 色块 / 分段按钮的位置
static float SwatchCx(MenuLevel* lv, int part) { (void)lv; return Margin + Inset + 12 + SwD / 2 + part * (SwD + SwGap); }
static void SegRects(MenuLevel* lv, const MenuItem& it, int i, D2D1_RECT_F* box, D2D1_RECT_F* seg)
{
    D2D1_RECT_F r = RowRect(lv, i);
    float total = SegWidth(it);
    float right = r.right - 12;
    *box = D2D1::RectF(right - total, r.top + 8, right, r.bottom - 8);
    float x = box->left;
    for (int k = 0; k <= it.count && seg; k++)
    {
        if (k == it.count) break;
        float sw = Max(44.f, Measure(Str(it.labels[k]), 13, 400) + 20);
        seg[k] = D2D1::RectF(x, box->top, x + sw, box->bottom);
        x += sw;
    }
}
static void SliderTrack(MenuLevel* lv, int i, float* x0, float* x1, float* cy)
{
    D2D1_RECT_F r = RowRect(lv, i);
    *x0 = r.left + 12 + 64 + 8;
    *x1 = r.right - 12 - 40 - 8;
    *cy = (r.top + r.bottom) / 2;
}

static void RenderLevel(MenuLevel* lv)
{
    MenuSession* s = g_ms;
    float S = lv->S;
    int pw = (int)((lv->w + Margin * 2) * S + 0.5f), ph = (int)((lv->h + Margin * 2) * S + 0.5f);
    Canvas c;
    if (!c.Begin(pw, ph, 96 * S)) { c.End(lv->hwnd); return; }
    ID2D1RenderTarget* rt = c.rt;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    D2D1_RECT_F box = D2D1::RectF(Margin, Margin, Margin + lv->w, Margin + lv->h);
    Shadow(rt, box, Radius, 12, T.dark ? 0.030f : 0.016f, 3);
    Fill(rt, box, Radius, T.menuBg);
    Stroke(rt, box, Radius, T.menuBorder, 1);

    Menu* m = lv->m;
    int levelIndex = s->levels.IndexOf(lv);
    for (int i = 0; i < m->items.n; i++)
    {
        MenuItem& it = m->items[i];
        D2D1_RECT_F r = RowRect(lv, i);
        bool hot = lv->hot == i;
        bool pressed = s->pressLevel == levelIndex && s->pressItem == i;
        switch (it.kind)
        {
        case MkSep:
            Line(rt, Margin, (r.top + r.bottom) / 2, Margin + lv->w, (r.top + r.bottom) / 2, T.divider, 1);
            break;
        case MkHeader:
            Text(rt, it.text, D2D1::RectF(r.left + 12, r.top + 8, r.right - 12, r.bottom - 2), 12, 400, T.text2);
            break;
        case MkItem:
        {
            bool open = levelIndex + 1 < s->levels.n && s->levels[levelIndex + 1]->parentItem == i;
            if (!it.disabled && (hot || open)) Fill(rt, r, 4, pressed ? T.subtlePress : T.subtleHover);
            u32 fg = it.disabled ? T.disabled : T.text;
            float iconCx = r.left + 12 + 8;
            float titleTop = it.desc.n ? r.top + 7 : r.top;
            float titleBottom = it.desc.n ? r.top + 7 + 20 : r.bottom;
            float iconCy = (titleTop + titleBottom) / 2;
            float right = r.right - 12;
            float textX = Margin + lv->textLeft;
            if (it.live && it.checkable)
            {
                // 菜单里不关菜单的开关：画成右侧的拨动开关，文字和滑块那几行对齐
                textX = r.left + 12;
                float cy = (r.top + r.bottom) / 2;
                D2D1_RECT_F tr = D2D1::RectF(r.right - 12 - 40, cy - 10, r.right - 12, cy + 10);
                if (it.check)
                {
                    Fill(rt, tr, 10, T.accent);
                    Circle(rt, tr.right - 10, cy, hot ? 7.f : 6.f, T.onAccent);
                }
                else
                {
                    Fill(rt, tr, 10, T.toggleOffFill);
                    Stroke(rt, tr, 10, T.strong, 1);
                    Circle(rt, tr.left + 10, cy, hot ? 7.f : 6.f, T.text2);
                }
                right = tr.left - 12;
            }
            else if (it.radio) { if (it.check) Circle(rt, iconCx, iconCy, 3.5f, fg); }
            else if (it.checkable) { if (it.check) Icon(rt, 0xE73E, iconCx, iconCy, 14, fg); }
            else if (it.icon) Icon(rt, it.icon, iconCx, iconCy, 16, fg);
            if (it.sub) { Icon(rt, 0xE76C, right - 6, (r.top + r.bottom) / 2, 12, it.disabled ? T.disabled : T.text2); right -= 24; }
            if (it.hint.n)
            {
                float hw = Measure(it.hint, 12, 400);
                Text(rt, it.hint, D2D1::RectF(right - hw - 1, titleTop, right, titleBottom), 12, 400, it.disabled ? T.disabled : T.text2, Right);
                right -= hw + 24;
            }
            Text(rt, it.text, D2D1::RectF(textX, titleTop, right, titleBottom), 14, 400, fg);
            if (it.desc.n) Text(rt, it.desc, D2D1::RectF(textX, titleBottom, it.live && it.checkable ? right : r.right - 12, titleBottom + 16), 12, 400, it.disabled ? T.disabled : T.text2);
            break;
        }
        case MkSwatches:
        {
            float cy = (r.top + r.bottom) / 2;
            for (int k = 0; k <= it.count; k++)
            {
                float cx = SwatchCx(lv, k);
                bool isHot = hot && lv->hotPart == k;
                bool sel = k < it.count ? it.sel == k : it.sel < 0;
                if (sel) Ring(rt, cx, cy, SwD / 2 + 3, T.text, 1.5f);
                else if (isHot) Ring(rt, cx, cy, SwD / 2 + 3, T.text3, 1.5f);
                if (k < it.count)
                {
                    Circle(rt, cx, cy, SwD / 2, 0xFF000000u | it.colors[k]);
                    Ring(rt, cx, cy, SwD / 2 - 0.5f, T.dark ? 0x30FFFFFF : 0x30000000, 1);
                }
                else if (it.sel < 0)
                {
                    // 当前是自定义颜色：最后一格显示它
                    Circle(rt, cx, cy, SwD / 2, 0xFF000000u | it.current);
                    Ring(rt, cx, cy, SwD / 2 - 0.5f, T.dark ? 0x30FFFFFF : 0x30000000, 1);
                }
                else
                {
                    Ring(rt, cx, cy, SwD / 2 - 0.5f, T.strong, 1);
                    Icon(rt, 0xE710, cx, cy, 10, T.text2);
                }
            }
            break;
        }
        case MkSlider:
        {
            Text(rt, it.text, D2D1::RectF(r.left + 12, r.top, r.left + 12 + 64, r.bottom), 14, 400, T.text);
            float x0, x1, cy;
            SliderTrack(lv, i, &x0, &x1, &cy);
            float t = (it.val - it.minV) / Max(0.0001f, it.maxV - it.minV);
            float tx = x0 + (x1 - x0) * t;
            Fill(rt, D2D1::RectF(x0, cy - 2, x1, cy + 2), 2, T.strong);
            Fill(rt, D2D1::RectF(x0, cy - 2, tx, cy + 2), 2, T.accent);
            bool dragging = s->dragLevel == levelIndex && s->dragItem == i;
            Circle(rt, tx, cy, 10, T.knob);
            Ring(rt, tx, cy, 9.5f, T.dark ? 0x18FFFFFF : 0x0F000000, 1);
            Circle(rt, tx, cy, dragging ? 5 : hot ? 7 : 6, T.accent);
            Str v;
            v.Int((int)(it.val + 0.5f)).Append(it.unit);
            Text(rt, v, D2D1::RectF(r.right - 12 - 40, r.top, r.right - 12, r.bottom), 13, 400, T.text2, Right);
            break;
        }
        case MkSegment:
        {
            Text(rt, it.text, D2D1::RectF(r.left + 12, r.top, r.left + 12 + 56, r.bottom), 14, 400, T.text);
            D2D1_RECT_F bx, seg[8];
            SegRects(lv, it, i, &bx, seg);
            Fill(rt, bx, 5, T.ctrlFill);
            Stroke(rt, bx, 5, T.ctrlBorder, 1);
            for (int k = 0; k < it.count && k < 8; k++)
            {
                D2D1_RECT_F sr = { seg[k].left + 2, seg[k].top + 2, seg[k].right - 2, seg[k].bottom - 2 };
                if (it.sel == k) Fill(rt, sr, 4, T.accent);
                else if (hot && lv->hotPart == k) Fill(rt, sr, 4, T.subtleHover);
                Text(rt, Str(it.labels[k]), seg[k], 13, it.sel == k ? 600 : 400, it.sel == k ? T.onAccent : T.text, Center);
            }
            break;
        }
        }
    }
    c.End(lv->hwnd);
}

static RECT WorkAreaAt(int x, int y)
{
    POINT p = { x, y };
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcWork;
}

static void FirstSelectable(MenuLevel* lv)
{
    for (int i = 0; i < lv->m->items.n; i++) if (Selectable(lv->m->items[i])) { lv->hot = i; return; }
}

// 打开一级菜单：(ax, ay) 是想放的左上角；放不下就往左 / 往上翻
static MenuLevel* OpenLevel(Menu* m, int parentItem, int ax, int ay, int flipX, bool keyboard)
{
    MenuSession* s = g_ms;
    MenuLevel* lv = new MenuLevel();
    lv->m = m;
    lv->parentItem = parentItem;
    lv->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, MenuClass, L"DeskNote 菜单", WS_POPUP,
                               ax, ay, 1, 1, s->owner, nullptr, g_inst, nullptr);
    lv->S = GetDpiForWindow(lv->hwnd) / 96.f;
    LayoutLevel(lv);
    float S = lv->S;
    int w = (int)(lv->w * S + 0.5f), h = (int)(lv->h * S + 0.5f);
    RECT wa = WorkAreaAt(ax, ay);
    int x = ax, y = ay;
    if (x + w > wa.right) x = flipX - w;
    if (y + h > wa.bottom) y = parentItem < 0 ? ay - h : wa.bottom - h;
    x = Clamp(x, (int)wa.left, (int)Max(wa.left, wa.right - w));
    y = Clamp(y, (int)wa.top, (int)Max(wa.top, wa.bottom - h));
    lv->cx = x;
    lv->cy = y;
    int m0 = (int)(Margin * S + 0.5f);
    SetWindowPos(lv->hwnd, HWND_TOPMOST, x - m0, y - m0, w + 2 * m0, h + 2 * m0, SWP_NOACTIVATE);
    if (keyboard) FirstSelectable(lv);
    s->levels.Push(lv);
    RenderLevel(lv);
    ShowWindow(lv->hwnd, SW_SHOWNOACTIVATE);
    return lv;
}

static void CloseFrom(int level)
{
    MenuSession* s = g_ms;
    while (s->levels.n > level)
    {
        MenuLevel* lv = s->levels.Last();
        DestroyWindow(lv->hwnd);
        delete lv;
        s->levels.RemoveAt(s->levels.n - 1);
    }
    if (s->pressLevel >= level) s->pressLevel = s->pressItem = -1;
    if (s->dragLevel >= level) s->dragLevel = s->dragItem = -1;
    if (s->levels.n) RenderLevel(s->levels.Last());
}

static void OpenSub(int level, int item, bool keyboard)
{
    MenuSession* s = g_ms;
    MenuLevel* lv = s->levels[level];
    if (level + 1 < s->levels.n)
    {
        if (s->levels[level + 1]->parentItem == item) return;
        CloseFrom(level + 1);
    }
    MenuItem& it = lv->m->items[item];
    if (!it.sub || it.disabled) return;
    float S = lv->S;
    int ax = lv->cx + (int)((lv->w - 2) * S);
    int ay = lv->cy + (int)((lv->tops[item] - Pad) * S);
    int flipX = lv->cx + (int)(2 * S);
    OpenLevel(it.sub, item, ax, ay, flipX, keyboard);
    RenderLevel(lv);
}

// 屏幕坐标 -> 第几级菜单的第几项、项内哪个部件
static bool HitTest(POINT p, int* level, int* item, int* part)
{
    MenuSession* s = g_ms;
    for (int L = s->levels.n - 1; L >= 0; L--)
    {
        MenuLevel* lv = s->levels[L];
        float x = (p.x - lv->cx) / lv->S + Margin, y = (p.y - lv->cy) / lv->S + Margin;
        if (x < Margin || y < Margin || x >= Margin + lv->w || y >= Margin + lv->h) continue;
        *level = L;
        *item = -1;
        *part = -1;
        for (int i = 0; i < lv->m->items.n; i++)
        {
            D2D1_RECT_F r = RowRect(lv, i);
            if (y < r.top || y >= r.bottom) continue;
            MenuItem& it = lv->m->items[i];
            if (!Selectable(it)) return true;
            if (it.kind == MkSwatches)
            {
                for (int k = 0; k <= it.count; k++)
                    if (x >= SwatchCx(lv, k) - SwD / 2 - 4 && x < SwatchCx(lv, k) + SwD / 2 + 4) { *item = i; *part = k; }
                return true;
            }
            if (it.kind == MkSegment)
            {
                D2D1_RECT_F bx, seg[8];
                SegRects(lv, it, i, &bx, seg);
                for (int k = 0; k < it.count && k < 8; k++)
                    if (x >= seg[k].left && x < seg[k].right) { *item = i; *part = k; }
                return true;
            }
            *item = i;
            return true;
        }
        return true;
    }
    return false;
}

static void SetHot(int level, int item, int part)
{
    MenuSession* s = g_ms;
    MenuLevel* lv = s->levels[level];
    if (lv->hot == item && lv->hotPart == part) return;
    lv->hot = item;
    lv->hotPart = part;
    RenderLevel(lv);
    // 父级保持高亮在打开子菜单的那一项上
    for (int L = level - 1; L >= 0; L--)
    {
        MenuLevel* p = s->levels[L];
        int want = s->levels[L + 1]->parentItem;
        if (p->hot != want) { p->hot = want; p->hotPart = -1; RenderLevel(p); }
    }
    // 子菜单的开合稍等一下再做，斜着移向子菜单时不会被误关
    s->timerLevel = level;
    SetTimer(s->levels[0]->hwnd, 1, 220, nullptr);
}

static void OnSubTimer()
{
    MenuSession* s = g_ms;
    KillTimer(s->levels[0]->hwnd, 1);
    int L = s->timerLevel;
    if (L < 0 || L >= s->levels.n) return;
    MenuLevel* lv = s->levels[L];
    if (lv->hot < 0) return;
    MenuItem& it = lv->m->items[lv->hot];
    if (it.sub) OpenSub(L, lv->hot, false);
    else if (L + 1 < s->levels.n) CloseFrom(L + 1);
}

static void SetSlider(int level, int item, float x)
{
    MenuSession* s = g_ms;
    MenuLevel* lv = s->levels[level];
    MenuItem& it = lv->m->items[item];
    float x0, x1, cy;
    SliderTrack(lv, item, &x0, &x1, &cy);
    float t = Clamp((x - x0) / Max(1.f, x1 - x0), 0.f, 1.f);
    float v = it.minV + (it.maxV - it.minV) * t;
    v = (float)(int)(v / it.step + 0.5f) * it.step;
    v = Clamp(v, it.minV, it.maxV);
    if (v == it.val) return;
    it.val = v;
    RenderLevel(lv);
    if (s->live) s->live(s->ctx, it.id, v);
}

static void Activate(int level, int item, int part, bool keyboard)
{
    MenuSession* s = g_ms;
    MenuLevel* lv = s->levels[level];
    MenuItem& it = lv->m->items[item];
    if (!Selectable(it)) return;
    switch (it.kind)
    {
    case MkItem:
        if (it.sub) { OpenSub(level, item, keyboard); return; }
        if (it.live)
        {
            it.check = !it.check;
            RenderLevel(lv);
            if (s->live) s->live(s->ctx, it.id, it.check ? 1.f : 0.f);
            return;
        }
        s->result = it.id;
        s->done = true;
        return;
    case MkSwatches:
        if (part < 0) return;
        if (part == it.count) { s->result = it.customId; s->done = true; return; }
        it.sel = part;
        RenderLevel(lv);
        if (s->live) s->live(s->ctx, it.id + part, 0);
        return;
    case MkSegment:
        if (part < 0 || part == it.sel) return;
        it.sel = part;
        RenderLevel(lv);
        if (s->live) s->live(s->ctx, it.id + part, 0);
        return;
    }
}

static void OnMouse(const MSG& msg)
{
    MenuSession* s = g_ms;
    POINT p = { GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam) };
    if (msg.message == WM_MOUSEWHEEL || msg.message == WM_MOUSEHWHEEL) return;
    ClientToScreen(msg.hwnd, &p);
    int L = -1, item = -1, part = -1;
    bool inside = HitTest(p, &L, &item, &part);
    switch (msg.message)
    {
    case WM_MOUSEMOVE:
        if (s->dragLevel >= 0)
        {
            MenuLevel* lv = s->levels[s->dragLevel];
            SetSlider(s->dragLevel, s->dragItem, (p.x - lv->cx) / lv->S + Margin);
            return;
        }
        if (inside) SetHot(L, item, part);
        else
        {
            MenuLevel* last = s->levels.Last();
            if (last->hot >= 0) { last->hot = -1; last->hotPart = -1; RenderLevel(last); }
        }
        return;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK:
        if (!inside) { s->done = true; return; }
        if (item < 0) return;
        if (s->levels[L]->m->items[item].kind == MkSlider)
        {
            s->dragLevel = L;
            s->dragItem = item;
            MenuLevel* lv = s->levels[L];
            SetSlider(L, item, (p.x - lv->cx) / lv->S + Margin);
            RenderLevel(lv);
            return;
        }
        s->pressLevel = L;
        s->pressItem = item;
        s->pressPart = part;
        RenderLevel(s->levels[L]);
        return;
    case WM_LBUTTONUP: case WM_RBUTTONUP:
    {
        if (s->dragLevel >= 0)
        {
            int dl = s->dragLevel;
            s->dragLevel = s->dragItem = -1;
            if (dl < s->levels.n) RenderLevel(s->levels[dl]);
            return;
        }
        bool same = s->pressLevel < 0 || (s->pressLevel == L && s->pressItem == item && s->pressPart == part);
        int pl = s->pressLevel;
        s->pressLevel = s->pressItem = s->pressPart = -1;
        if (pl >= 0 && pl < s->levels.n) RenderLevel(s->levels[pl]);
        if (inside && item >= 0 && same) Activate(L, item, part, false);
        return;
    }
    }
}

static void MoveHot(MenuLevel* lv, int dir)
{
    int n = lv->m->items.n;
    int i = lv->hot;
    for (int k = 0; k < n; k++)
    {
        i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
        if (Selectable(lv->m->items[i])) break;
    }
    lv->hot = i;
    lv->hotPart = -1;
    RenderLevel(lv);
}

static void OnKey(WPARAM vk)
{
    MenuSession* s = g_ms;
    int L = s->levels.n - 1;
    MenuLevel* lv = s->levels[L];
    MenuItem* it = lv->hot >= 0 ? &lv->m->items[lv->hot] : nullptr;
    switch (vk)
    {
    case VK_ESCAPE:
        if (L > 0) CloseFrom(L); else s->done = true;
        return;
    case VK_MENU: case VK_F10: case VK_APPS:
        s->done = true;
        return;
    case VK_UP: MoveHot(lv, -1); return;
    case VK_DOWN: MoveHot(lv, 1); return;
    case VK_LEFT: case VK_RIGHT:
    {
        int d = vk == VK_RIGHT ? 1 : -1;
        if (it && it->kind == MkSlider)
        {
            it->val = Clamp(it->val + d * it->step, it->minV, it->maxV);
            RenderLevel(lv);
            if (s->live) s->live(s->ctx, it->id, it->val);
            return;
        }
        if (it && (it->kind == MkSegment || it->kind == MkSwatches))
        {
            int ns = Clamp((it->sel < 0 ? 0 : it->sel) + d, 0, it->count - 1);
            if (ns != it->sel) Activate(L, lv->hot, ns, true);
            return;
        }
        if (d > 0 && it && it->sub) { OpenSub(L, lv->hot, true); return; }
        if (d < 0 && L > 0) CloseFrom(L);
        return;
    }
    case VK_RETURN: case VK_SPACE:
        if (it) Activate(L, lv->hot, it->kind == MkSwatches || it->kind == MkSegment ? it->sel : -1, true);
        return;
    }
}

static LRESULT CALLBACK MenuProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER:
        if (g_ms && w == 1) OnSubTimer();
        else if (g_ms && w == 2 && !MenuDebug)
        {
            // 别的程序抢到前台（按了 Win 键、Alt+Tab 等）：收起菜单
            HWND f = GetForegroundWindow();
            DWORD pid = 0;
            GetWindowThreadProcessId(f, &pid);
            if (f != g_ms->fg && pid != GetCurrentProcessId()) g_ms->done = true;
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// 点到菜单外面（包括别的程序的窗口）：收起菜单，这一下点击不往下传
static LRESULT CALLBACK MouseHook(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION && g_ms && !g_ms->done &&
        (w == WM_LBUTTONDOWN || w == WM_RBUTTONDOWN || w == WM_MBUTTONDOWN || w == WM_XBUTTONDOWN))
    {
        MSLLHOOKSTRUCT* hs = (MSLLHOOKSTRUCT*)l;
        int L, item, part;
        if (!HitTest(hs->pt, &L, &item, &part))
        {
            g_ms->done = true;
            if (g_ms->levels.n) PostMessageW(g_ms->levels[0]->hwnd, WM_NULL, 0, 0);
            return 1;
        }
    }
    return CallNextHookEx(g_hook, code, w, l);
}

int MenuRun(HWND owner, Menu* m, int sx, int sy, MenuLiveFn live, void* ctx)
{
    if (g_ms || !m || !m->items.n) return 0;
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = MenuProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = MenuClass;
        RegisterClassW(&wc);
        registered = true;
    }
    LoadTheme();
    MenuSession s;
    s.owner = owner;
    s.live = live;
    s.ctx = ctx;
    g_ms = &s;
    if (owner) SetForegroundWindow(owner);
    s.fg = GetForegroundWindow();
    OpenLevel(m, -1, sx, sy, sx, false);
    HWND root = s.levels[0]->hwnd;
    if (MenuDebugOpenSub > 0)
    {
        for (int i = 0, k = 0; i < m->items.n; i++)
            if (m->items[i].sub && ++k == MenuDebugOpenSub) { s.levels[0]->hot = i; OpenSub(0, i, false); break; }
        MenuDebugOpenSub = 0;
    }
    SetCapture(root);
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, g_inst, 0);
    SetTimer(root, 2, 150, nullptr);
    MSG msg;
    while (!s.done)
    {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) { PostQuitMessage((int)msg.wParam); break; }
        if (msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST) { OnMouse(msg); continue; }
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) { OnKey(msg.wParam); continue; }
        if (msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP || msg.message == WM_CHAR || msg.message == WM_SYSCHAR) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!s.done && GetCapture() != root && s.dragLevel < 0) SetCapture(root);
    }
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    KillTimer(root, 1);
    KillTimer(root, 2);
    ReleaseCapture();
    CloseFrom(0);
    g_ms = nullptr;
    return s.result;
}
}
