// 一张便签的窗口：绘制、布局、鼠标键盘、编辑、排序、撤销、胶囊、窗口模式、右键菜单
#include "app.h"
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>

// =====================================================================
// 绘图资源
// =====================================================================
namespace Gfx
{
    static ID2D1Factory* d2d;
    static IDWriteFactory* dw;
    static DWORD lastUse;
    ID2D1Factory* D2D()
    {
        if (!d2d) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void**)&d2d);
        lastUse = GetTickCount();
        return d2d;
    }
    IDWriteFactory* DW()
    {
        // 独立工厂：字体缓存归它自己，释放工厂时一起释放（共享工厂的缓存会一直留在进程里）
        if (!dw) DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), (IUnknown**)&dw);
        lastUse = GetTickCount();
        return dw;
    }
    void Touch() { lastUse = GetTickCount(); }
    void ReleaseAll()
    {
        if (dw) { dw->Release(); dw = nullptr; }
        if (d2d) { d2d->Release(); d2d = nullptr; }
        HeapCompact(GetProcessHeap(), 0);
    }
    void ReleaseIfIdle() { if ((d2d || dw) && GetTickCount() - lastUse > 3000) ReleaseAll(); }
    D2D1_COLOR_F Color(u32 rgb, float a)
    {
        D2D1_COLOR_F c = { ((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a };
        return c;
    }
}

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// 画布：一块 32 位位图 + Direct2D 渲染目标，画完交给 UpdateLayeredWindow
struct Canvas
{
    HDC screen = nullptr, mem = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    void* bits = nullptr;
    ID2D1DCRenderTarget* rt = nullptr;
    int pw = 0, ph = 0;

    bool Begin(int w, int h, float dpi)
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
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
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
    void End(HWND hwnd)
    {
        if (rt)
        {
            HRESULT hr = rt->EndDraw();
            if (FAILED(hr)) { Str m(L"EndDraw 失败 hr="); m.Int((i64)(u32)hr); Log(m); }
            rt->Release();
            rt = nullptr;
        }
        GdiFlush();
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
};

static IDWriteTextFormat* MakeFormat(float size, int weight, bool wrap)
{
    IDWriteTextFormat* f = nullptr;
    Gfx::DW()->CreateTextFormat(L"Microsoft YaHei UI", nullptr, (DWRITE_FONT_WEIGHT)weight, DWRITE_FONT_STYLE_NORMAL,
                                DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", &f);
    if (f && !wrap)
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

static IDWriteTextLayout* MakeLayout(const Str& text, IDWriteTextFormat* f, float maxW, float lineH)
{
    IDWriteTextLayout* l = nullptr;
    Gfx::DW()->CreateTextLayout(text.c(), text.n, f, Max(maxW, 1.f), 100000.f, &l);
    if (l && lineH > 0) l->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineH, lineH * 0.78f);
    return l;
}

static float LayoutHeight(IDWriteTextLayout* l)
{
    DWRITE_TEXT_METRICS m = {};
    l->GetMetrics(&m);
    return m.height;
}
static float LayoutWidth(IDWriteTextLayout* l)
{
    DWRITE_TEXT_METRICS m = {};
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

static float R1(float v) { return (float)(int)(v * 10 + 0.5f) / 10; }
static float R0(float v) { return (float)(int)(v + 0.5f); }

// 尺寸（和 WPF 版一致，单位 DIP）
static float TitleSize(const NoteData* d) { return R1((float)d->fontSize * 1.3f); }
static float LineH(const NoteData* d) { return R0((float)d->fontSize * 1.45f); }
static float HandleW(const NoteData* d) { return R0((float)d->fontSize * 1.1f); }
static float CheckSize(const NoteData* d) { return R0((float)d->fontSize * 0.95f); }
static float CheckColW(const NoteData* d) { return CheckSize(d) + R0((float)d->fontSize * 0.55f); }
static float RowGap(const NoteData* d) { return R0((float)d->fontSize * 0.4f); }
static const float PadX = 20, PadTop = 16, PadBottom = 16, TitleGap = 12;
static const u32 DueColor = 0xE55B5B;

static Str FormatWhen(Time t)
{
    SYSTEMTIME st;
    SplitTime(t, &st);
    Time day = MakeTime(st.wYear, st.wMonth, st.wDay, 0, 0, 0);
    Time today = Today();
    Str hm;
    hm.Pad2(st.wHour).Ch(L':').Pad2(st.wMinute);
    if (day == today) return hm;
    if (day == today + TicksPerDay) return L"明天 " + hm;
    SYSTEMTIME now;
    GetLocalTime(&now);
    Str s;
    if (st.wYear == now.wYear) s.Int(st.wMonth).Append(L"月").Int(st.wDay).Append(L"日 ");
    else s.Int(st.wYear).Ch(L'-').Int(st.wMonth).Ch(L'-').Int(st.wDay).Ch(L' ');
    return s + hm;
}

// 链接：http(s):// 或 www. 开头，到空白或中文标点为止
static bool IsUrlStop(wchar_t c)
{
    if (c <= L' ') return true;
    const wchar_t* stops = L"<>\"'，。；：、！？（）【】《》「」";
    for (const wchar_t* p = stops; *p; p++) if (*p == c) return true;
    return false;
}
void FindLinks(const Str& text, Vec<LinkSpan>& out)
{
    int i = 0;
    while (i < text.n)
    {
        bool http = (text.n - i >= 7 && (text.Sub(i, 7).EqI(L"http://"))) || (text.n - i >= 8 && text.Sub(i, 8).EqI(L"https://"));
        bool www = !http && text.n - i >= 4 && text.Sub(i, 4).EqI(L"www.");
        if (!http && !www) { i++; continue; }
        int j = i;
        while (j < text.n && !IsUrlStop(text.p[j])) j++;
        while (j > i && Str(L".,;:!?)]}").FindCh(text.p[j - 1]) >= 0) j--;
        if (j - i >= 8)
        {
            LinkSpan s;
            s.start = i;
            s.len = j - i;
            s.url = www ? L"http://" + text.Sub(i, j - i) : text.Sub(i, j - i);
            out.Push(s);
        }
        i = Max(j, i + 1);
    }
}

// 条目显示的文字（提醒时间接在后面）和它的排版
static Str ItemDisplay(const NoteItem& it, int* tagStart)
{
    Str s = it.text;
    *tagStart = -1;
    if (it.hasRemind)
    {
        *tagStart = s.n;
        s += L"  ⏰ ";
        s += FormatWhen(it.remind);
    }
    return s;
}

static IDWriteTextLayout* ItemLayout(const NoteData* d, const NoteItem& it, IDWriteTextFormat* f, float width, ID2D1RenderTarget* rt,
                                     ID2D1Brush* tagBrush)
{
    int tag;
    Str s = ItemDisplay(it, &tag);
    IDWriteTextLayout* l = MakeLayout(s, f, width, LineH(d));
    if (!l) return nullptr;
    Vec<LinkSpan> links;
    FindLinks(it.text, links);
    for (int i = 0; i < links.n; i++) l->SetUnderline(TRUE, { (UINT32)links[i].start, (UINT32)links[i].len });
    if (d->checkboxes && it.done) l->SetStrikethrough(TRUE, { 0, (UINT32)it.text.n });
    if (tag >= 0)
    {
        DWRITE_TEXT_RANGE r = { (UINT32)tag, (UINT32)(s.n - tag) };
        l->SetFontSize(R1((float)d->fontSize * 0.85f), r);
        if (tagBrush) l->SetDrawingEffect(tagBrush, r);
    }
    (void)rt;
    return l;
}

// 每个角半径可以不同的圆角矩形（胶囊贴边的一侧是直角）
static void FillCornerRect(ID2D1RenderTarget* rt, D2D1_RECT_F r, float tl, float tr, float br, float bl, ID2D1Brush* b)
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
    rt->FillGeometry(g, b);
    s->Release();
    g->Release();
}

// 文字（可选阴影）
static void DrawLayout(ID2D1RenderTarget* rt, float x, float y, IDWriteTextLayout* l, ID2D1Brush* b, bool shadow, ID2D1Brush* shadowBrush)
{
    if (shadow && shadowBrush)
    {
        rt->DrawTextLayout(D2D1::Point2F(x + 1, y + 1), l, shadowBrush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        rt->DrawTextLayout(D2D1::Point2F(x + 0.5f, y + 0.5f), l, shadowBrush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
    rt->DrawTextLayout(D2D1::Point2F(x, y), l, b, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

// =====================================================================
// 创建、销毁
// =====================================================================
static const wchar_t* NoteClass = L"DeskNoteNote";
static const wchar_t* EditHostClass = L"DeskNoteEditHost";
static const wchar_t* PreviewClass = L"DeskNotePreview";
enum { TimerHide = 1, TimerSlide = 2, TimerProgman = 3 };

NoteWin::NoteWin(NoteData* data) : d(data) {}
NoteWin::~NoteWin()
{
    delete d;
    delete pendingReload;
    for (int i = 0; i < undo.n; i++) delete undo[i];
    for (int i = 0; i < redo.n; i++) delete redo[i];
    delete pending;
}

static void RegisterNoteClasses()
{
    static bool done;
    if (done) return;
    done = true;
    WNDCLASSW wc = {};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = NoteWin::WndProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = g_app->icon;
    wc.lpszClassName = NoteClass;
    RegisterClassW(&wc);
    wc.style = 0;
    wc.lpfnWndProc = NoteWin::EditHostProc;
    wc.lpszClassName = EditHostClass;
    RegisterClassW(&wc);
    wc.lpfnWndProc = NoteWin::PreviewProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = PreviewClass;
    RegisterClassW(&wc);
}

float NoteWin::Scale() const { return hwnd ? GetDpiForWindow(hwnd) / 96.f : 1.f; }

void NoteWin::Create()
{
    RegisterNoteClasses();
    hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, NoteClass, d->title.c(), WS_POPUP,
                           80, 80, 10, 10, nullptr, nullptr, g_inst, this);
    taskbarCreatedMsg = (int)RegisterWindowMessageW(L"TaskbarCreated");
    RestorePosition();
    ApplyMode();
    if (d->collapsed) EnterCollapsed();
    else Render();
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    if (d->collapsed) { SnapCapsule(); ScheduleCapsuleHide(); }
}

void NoteWin::Destroy()
{
    CommitEdit(true);
    ClosePreview();
    if (hwnd)
    {
        SaveLayout();
        HWND h = hwnd;
        hwnd = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
    }
}

// =====================================================================
// 绘制
// =====================================================================
void NoteWin::Render()
{
    if (!hwnd) return;
    SetWindowTextW(hwnd, d->title.c());
    if (d->collapsed)
    {
        // ---- 胶囊：标题 + 未完成条数 ----
        float S = Scale();
        float size = Min((float)d->fontSize, 15.f);
        IDWriteTextFormat* ft = MakeFormat(size, 700, false);
        IDWriteTextFormat* fc = MakeFormat(size, 400, false);
        int undone = 0;
        for (int i = 0; i < d->items.n; i++) if (!d->checkboxes || !d->items[i].done) undone++;
        Str count(L"  ");
        count.Int(undone);
        IDWriteTextLayout* lt = MakeLayout(d->title, ft, 220, 0);
        IDWriteTextLayout* lc = MakeLayout(count, fc, 100, 0);
        float tw = Min(LayoutWidth(lt), 220.f), cw = LayoutWidth(lc);
        float th = Max(LayoutHeight(lt), LayoutHeight(lc));
        lt->SetMaxWidth(tw + 1);
        float W = 14 + tw + cw + 14, H = 5 + th + 6;
        Canvas c;
        if (c.Begin((int)(W * S + 0.99f), (int)(H * S + 0.99f), 96 * S))
        {
            ID2D1SolidColorBrush *bg = nullptr, *fg = nullptr, *dim = nullptr;
            c.rt->CreateSolidColorBrush(Gfx::Color(d->bgColor, (float)Max(d->opacity, 0.75)), &bg);
            c.rt->CreateSolidColorBrush(Gfx::Color(d->textColor, 1), &fg);
            c.rt->CreateSolidColorBrush(Gfx::Color(d->textColor, 0xA0 / 255.f), &dim);
            const float r = 14;
            FillCornerRect(c.rt, D2D1::RectF(0, 0, c.pw / S, c.ph / S), dockL || dockT ? 0 : r, dockR || dockT ? 0 : r,
                           dockR || dockB ? 0 : r, dockL || dockB ? 0 : r, bg);
            c.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            c.rt->DrawTextLayout(D2D1::Point2F(14, 5), lt, fg, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
            c.rt->DrawTextLayout(D2D1::Point2F(14 + tw, 5), lc, dim, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            SafeRelease(bg); SafeRelease(fg); SafeRelease(dim);
        }
        c.End(hwnd);
        SafeRelease(lt); SafeRelease(lc); SafeRelease(ft); SafeRelease(fc);
        if (preview && IsWindowVisible(preview)) RenderPreview();
        return;
    }
    RECT wr;
    GetWindowRect(hwnd, &wr);
    float S = Scale();
    int pw = (int)(d->width * S + 0.5), ph = (int)(d->height * S + 0.5);
    Canvas c;
    c.Begin(pw, ph, 96 * S);
    if (c.rt)
    {
        float W = pw / S, H = ph / S;
        ID2D1RenderTarget* rt = c.rt;
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        ID2D1SolidColorBrush *bg = nullptr, *fg = nullptr, *dim = nullptr, *due = nullptr, *shadow = nullptr, *checkFg = nullptr;
        rt->CreateSolidColorBrush(Gfx::Color(d->bgColor, (float)Max(d->opacity, 1.0 / 255)), &bg);
        rt->CreateSolidColorBrush(Gfx::Color(d->textColor, 1), &fg);
        rt->CreateSolidColorBrush(Gfx::Color(d->textColor, 0xA0 / 255.f), &dim);
        rt->CreateSolidColorBrush(Gfx::Color(DueColor, 1), &due);
        rt->CreateSolidColorBrush(Gfx::Color(0, 0.75f), &shadow);
        rt->CreateSolidColorBrush(Gfx::Color(d->bgColor, 1), &checkFg);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), 4, 4), bg);

        float left = PadX, right = W - PadX, top = PadTop, bottom = H - PadBottom;
        rt->PushAxisAlignedClip(D2D1::RectF(0, top - 4, W, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        float lineH = LineH(d), fs = (float)d->fontSize;
        float y = top - scrollY;

        // 标题行
        float ts = TitleSize(d);
        IDWriteTextFormat* ft = MakeFormat(ts, 700, false);
        IDWriteTextLayout* lt = MakeLayout(d->title, ft, right - left - 24, 0);
        float titleH = Max(LayoutHeight(lt), 24.f);
        if (editIndex != TitleIndex) DrawLayout(rt, left, y + (titleH - LayoutHeight(lt)) / 2, lt, fg, d->shadow, shadow);
        if (editIndex == TitleIndex) titleH = Max(titleH, editHeightDip);
        collapseBtn = D2D1::RectF(right - 18, y + titleH / 2 - 12 + scrollY, right + 6, y + titleH / 2 + 12 + scrollY);
        if (hover && editIndex == NotEditing)
            rt->FillRectangle(D2D1::RectF(right - 12, y + titleH / 2 - 1, right, y + titleH / 2 + 1), dim);
        SafeRelease(lt); SafeRelease(ft);
        y += titleH + TitleGap;
        titleBottom = y + scrollY;

        IDWriteTextFormat* fb = MakeFormat(fs, d->fontWeight, true);
        rows.Clear();
        if (d->items.n == 0 && editIndex != 0)
        {
            IDWriteTextLayout* lh = MakeLayout(Str(L"双击新增，右键设置"), fb, right - left, lineH);
            DrawLayout(rt, left, y, lh, dim, false, nullptr);
            y += LayoutHeight(lh);
            SafeRelease(lh);
        }

        ID2D1StrokeStyle* round = nullptr;
        Gfx::D2D()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                  D2D1_LINE_JOIN_ROUND), nullptr, 0, &round);
        int n = d->items.n + (editIndex == d->items.n ? 1 : 0);
        for (int i = 0; i < n; i++)
        {
            bool real = i < d->items.n;
            const NoteItem* it = real ? &d->items[i] : nullptr;
            float rowAlpha = i == dragIndex ? 0.45f : 1;
            float x = left;
            // 拖动手柄
            if (real)
            {
                float dd = Max(2.f, R1(fs * 0.17f)), gap = dd * 0.9f;
                float ch = dd * 3 + gap * 2;
                ID2D1SolidColorBrush* hb = nullptr;
                rt->CreateSolidColorBrush(Gfx::Color(d->textColor, 0xA0 / 255.f * rowAlpha), &hb);
                for (int r = 0; r < 3; r++)
                    for (int k = 0; k < 2; k++)
                        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + k * (dd + gap) + dd / 2, y + (lineH - ch) / 2 + r * (dd + gap) + dd / 2), dd / 2, dd / 2), hb);
                SafeRelease(hb);
            }
            x += HandleW(d);
            // 勾选框
            if (d->checkboxes)
            {
                float size = CheckSize(d);
                float bx = x, by = y + (lineH - size) / 2;
                float bw = Max(1.f, R0(fs / 10));
                float rr = Max(2.f, size * 0.22f);
                bool done = it && it->done && i != editIndex;
                ID2D1SolidColorBrush* cb = nullptr;
                rt->CreateSolidColorBrush(Gfx::Color(d->textColor, 0xA0 / 255.f * rowAlpha), &cb);
                if (done)
                {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, by, bx + size, by + size), rr, rr), cb);
                    float m = size * 0.18f, iw = size - 2 * m;
                    D2D1_POINT_2F p1 = D2D1::Point2F(bx + m + iw * 0.2f, by + m + iw * 0.55f);
                    D2D1_POINT_2F p2 = D2D1::Point2F(bx + m + iw * 0.42f, by + m + iw * 0.76f);
                    D2D1_POINT_2F p3 = D2D1::Point2F(bx + m + iw * 0.8f, by + m + iw * 0.28f);
                    float sw = Max(1.5f, fs / 8);
                    rt->DrawLine(p1, p2, checkFg, sw, round);
                    rt->DrawLine(p2, p3, checkFg, sw, round);
                }
                else
                    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx + bw / 2, by + bw / 2, bx + size - bw / 2, by + size - bw / 2), rr, rr), cb, bw);
                SafeRelease(cb);
                x += CheckColW(d);
            }
            // 文字
            float h;
            if (i == editIndex) h = Max(lineH, editHeightDip);
            else
            {
                bool dimmed = d->checkboxes && it->done;
                ID2D1SolidColorBrush *tb = nullptr, *tag = nullptr;
                rt->CreateSolidColorBrush(Gfx::Color(d->textColor, (dimmed ? 0.5f : 1.f) * rowAlpha), &tb);
                bool isDue = it->hasRemind && it->reminded && !it->done;
                rt->CreateSolidColorBrush(isDue ? Gfx::Color(DueColor, rowAlpha) : Gfx::Color(d->textColor, 0xA0 / 255.f * rowAlpha), &tag);
                IDWriteTextLayout* l = ItemLayout(d, *it, fb, right - x, rt, tag);
                h = Max(lineH, LayoutHeight(l));
                DrawLayout(rt, x, y, l, tb, d->shadow, shadow);
                SafeRelease(l); SafeRelease(tb); SafeRelease(tag);
            }
            RowInfo ri = { i, y + scrollY, y + scrollY + h, x };
            rows.Push(ri);
            y += h + RowGap(d);
        }
        SafeRelease(round);
        SafeRelease(fb);
        contentHeight = y + scrollY - top;
        rt->PopAxisAlignedClip();
        SafeRelease(bg); SafeRelease(fg); SafeRelease(dim); SafeRelease(due); SafeRelease(shadow); SafeRelease(checkFg);
    }
    c.End(hwnd);
    (void)wr;
}

void NoteWin::PaintTo(HDC, int, int) {}

// =====================================================================
// 命中测试
// =====================================================================
int NoteWin::HitRow(float yDip) const
{
    float y = yDip + scrollY;
    if (y < titleBottom - TitleGap && y >= PadTop - 4) return TitleIndex;
    for (int i = 0; i < rows.n; i++)
        if (y >= rows[i].top && y < rows[i].bottom + RowGap(d)) return rows[i].index;
    return NotEditing;
}

// WMSZ_*: 1 左 2 右 3 上 4 左上 5 右上 6 下 7 左下 8 右下
int NoteWin::HitEdge(int x, int y) const
{
    if (d->collapsed) return 0;
    RECT rc;
    GetClientRect(hwnd, &rc);
    int e = (int)(8 * Scale());
    bool l = x < e, r = x > rc.right - e, t = y < e, b = y > rc.bottom - e;
    if (t && l) return 4; if (t && r) return 5; if (b && l) return 7; if (b && r) return 8;
    if (l) return 1; if (r) return 2; if (t) return 3; if (b) return 6;
    return 0;
}

// =====================================================================
// 保存、外部修改
// =====================================================================
void NoteWin::Save()
{
    if (deleted) return;
    Store::SaveNote(d);
    g_app->OnNoteSaved();
}

void NoteWin::ReloadFrom(NoteData* fresh, bool keepWindow)
{
    if (keepWindow) { fresh->mode = d->mode; fresh->collapsed = d->collapsed; }
    if (Editing() || dragIndex >= 0 || changeDepth > 0)
    {
        delete pendingReload;
        pendingReload = fresh;
        return;
    }
    ApplyExternal(fresh);
    delete fresh;
    Render();
}

void NoteWin::ApplyExternal(NoteData* fresh)
{
    bool wasCollapsed = d->collapsed;
    d->CopyContentFrom(*fresh);
    ClearUndo(); // 外部改过以后，旧的撤销记录对不上了
    if (fresh->mode != d->mode) SetMode(fresh->mode);
    if (fresh->collapsed != wasCollapsed) { if (fresh->collapsed) Collapse(); else Expand(); }
    Save();
}

// =====================================================================
// 撤销 / 重做：每一步记下改动前的整张便签内容；删进回收站的条目一并记下
// =====================================================================
void NoteWin::BeginChange()
{
    if (changeDepth++ == 0)
    {
        delete pending;
        pending = new UndoStep();
        pending->state = d->ContentJson();
    }
}

void NoteWin::EndChange()
{
    if (changeDepth == 0) return;
    if (--changeDepth > 0) return;
    UndoStep* p = pending;
    pending = nullptr;
    if (p && (p->trashed.n > 0 || p->state != d->ContentJson()))
    {
        undo.Push(p);
        if (undo.n > 50) { delete undo[0]; undo.RemoveAt(0); }
        for (int i = 0; i < redo.n; i++) delete redo[i];
        redo.Clear();
    }
    else delete p;
    if (pendingReload && !Editing() && dragIndex < 0)
    {
        NoteData* pr = pendingReload;
        pendingReload = nullptr;
        ApplyExternal(pr);
        delete pr;
        Render();
    }
}

void NoteWin::ClearUndo()
{
    for (int i = 0; i < undo.n; i++) delete undo[i];
    for (int i = 0; i < redo.n; i++) delete redo[i];
    undo.Clear();
    redo.Clear();
}

void NoteWin::ApplyState(const Str& json)
{
    delete pending; // 从菜单里点的撤销：菜单结束时别再把“撤销”本身记成一步
    pending = nullptr;
    NoteData* s = NoteData::FromContentJson(json, d->id);
    if (s) { d->CopyContentFrom(*s); delete s; }
    Save();
    Render();
}

void NoteWin::Undo()
{
    CommitEdit(true);
    if (!undo.n) return;
    UndoStep* s = undo.Last();
    undo.RemoveAt(undo.n - 1);
    UndoStep* r = new UndoStep();
    r->state = d->ContentJson();
    r->trashed = s->trashed;
    redo.Push(r);
    ApplyState(s->state);
    g_app->Untrash(s->trashed);
    delete s;
}

void NoteWin::Redo()
{
    CommitEdit(true);
    if (!redo.n) return;
    UndoStep* s = redo.Last();
    redo.RemoveAt(redo.n - 1);
    UndoStep* u = new UndoStep();
    u->state = d->ContentJson();
    u->trashed = s->trashed;
    undo.Push(u);
    ApplyState(s->state);
    g_app->Retrash(s->trashed);
    delete s;
}

// =====================================================================
// 条目操作
// =====================================================================
void NoteWin::TrashItemAt(int i)
{
    TrashEntry* e = g_app->TrashItem(d, i, d->items[i]);
    if (pending) pending->trashed.Push(e);
    d->items.RemoveAt(i);
}

void NoteWin::ClearDone()
{
    for (int i = d->items.n - 1; i >= 0; i--)
        if (d->items[i].done) TrashItemAt(i);
    Save();
    Render();
}

void NoteWin::RestoreItem(const NoteItem& it, int index)
{
    CommitEdit(true);
    ClearUndo(); // 恢复后再撤销以前的操作会把恢复的条目弄丢，干脆清掉
    d->items.Insert(Clamp(index, 0, d->items.n), it);
    Save();
    Render();
}

void NoteWin::MoveItem(int from, int to)
{
    if (from < 0 || from >= d->items.n || to < 0 || to >= d->items.n || from == to) return;
    NoteItem t = d->items[from];
    d->items.RemoveAt(from);
    d->items.Insert(to, t);
    Save();
    Render();
}

void NoteWin::BeginReorder(int index)
{
    CommitEdit(true);
    BeginChange();
    dragIndex = index;
    dragChanged = false;
    Render();
    SetCapture(hwnd);
}

void NoteWin::OnReorderMove(float yDip)
{
    if (dragIndex < 0) return;
    float y = yDip + scrollY;
    for (int k = 0; k < rows.n; k++)
    {
        int i = rows[k].index;
        if (i == dragIndex || i >= d->items.n) continue;
        float mid = (rows[k].top + rows[k].bottom) / 2;
        if ((i > dragIndex && y > mid) || (i < dragIndex && y < mid))
        {
            NoteItem t = d->items[dragIndex];
            d->items.RemoveAt(dragIndex);
            d->items.Insert(i, t);
            dragIndex = i;
            dragChanged = true;
            Render();
            break;
        }
    }
    // 拖到上下边缘时自动滚动
    RECT rc;
    GetClientRect(hwnd, &rc);
    float H = rc.bottom / Scale();
    float maxScroll = Max(0.f, contentHeight - (H - PadTop - PadBottom));
    if (yDip < PadTop + 20 && scrollY > 0) { scrollY = Max(0.f, scrollY - LineH(d)); Render(); }
    else if (yDip > H - PadBottom - 20 && scrollY < maxScroll) { scrollY = Min(maxScroll, scrollY + LineH(d)); Render(); }
}

void NoteWin::EndReorder()
{
    if (dragIndex < 0) return;
    dragIndex = -1;
    if (GetCapture() == hwnd) ReleaseCapture();
    if (dragChanged) Save();
    EndChange();
    Render();
    SendToBottom();
}

// =====================================================================
// 编辑：在条目上盖一个系统输入框（中文输入法、复制粘贴都是系统原生的）
// =====================================================================
void NoteWin::BeginEdit(int index)
{
    CommitEdit(true);
    if (d->collapsed) Expand();
    editIndex = index;
    editOrig = index == TitleIndex ? d->title : index >= 0 && index < d->items.n ? d->items[index].text : Str();
    editHeightDip = index == TitleIndex ? 24 : LineH(d);
    Render();
    OpenEditor();
    Render();
    PositionEditor();
}

LRESULT CALLBACK EditSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref)
{
    NoteWin* self = (NoteWin*)ref;
    if (m == WM_KEYDOWN)
    {
        bool handled = false;
        self->OnEditKey(w, &handled);
        if (handled) return 0;
    }
    else if (m == WM_CHAR && (w == L'\r' || w == 27 || w == 1)) return 0; // 回车、Esc、Ctrl+A 的字符不进输入框
    else if (m == WM_KILLFOCUS)
    {
        HWND to = (HWND)w;
        if (to != self->editHost && to != h && self->Editing()) PostMessageW(self->hwnd, WM_APP_COMMITEDIT, 0, 0);
    }
    return DefSubclassProc(h, m, w, l);
}

void NoteWin::OnEditKey(WPARAM vk, bool* handled)
{
    bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    if (vk == VK_RETURN && shift)
    {
        // Shift+Enter：在这一条里换行（标题不换行）
        if (editIndex != TitleIndex) SendMessageW(editCtl, EM_REPLACESEL, TRUE, (LPARAM)L"\r\n");
        *handled = true;
    }
    else if (vk == VK_RETURN)
    {
        // 新增的条目按 Enter 后接着写下一条；空着按 Enter 就结束
        int len = GetWindowTextLengthW(editCtl);
        bool continueAdding = editIndex == d->items.n && len > 0;
        CommitEdit(true);
        if (continueAdding) BeginEdit(d->items.n);
        *handled = true;
    }
    else if (vk == VK_ESCAPE) { CommitEdit(false); *handled = true; }
    else if (ctrl && vk == 'A') { SendMessageW(editCtl, EM_SETSEL, 0, -1); *handled = true; }
}

void NoteWin::OpenEditor()
{
    float S = Scale();
    bool title = editIndex == TitleIndex;
    float size = title ? TitleSize(d) : (float)d->fontSize;
    editFont = CreateFontW(-(int)(size * S + 0.5f), 0, 0, 0, title ? 700 : d->fontWeight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    u32 bg = d->bgColor;
    int r = ((bg >> 16) & 255) * 3 / 4, g = ((bg >> 8) & 255) * 3 / 4, b = (bg & 255) * 3 / 4;
    editBrush = CreateSolidBrush(RGB(r, g, b));
    editHost = CreateWindowExW(WS_EX_TOOLWINDOW, EditHostClass, L"DeskNote 编辑", WS_POPUP, 0, 0, 10, 10, hwnd, nullptr, g_inst, this);
    DWORD style = WS_CHILD | WS_VISIBLE | (title ? ES_AUTOHSCROLL : (ES_MULTILINE | ES_AUTOVSCROLL));
    editCtl = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, editHost, (HMENU)1, g_inst, nullptr);
    SendMessageW(editCtl, WM_SETFONT, (WPARAM)editFont, FALSE);
    SendMessageW(editCtl, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM((int)(2 * S), (int)(2 * S)));
    SetWindowTextW(editCtl, editOrig.Replace(L"\n", L"\r\n").c());
    SetWindowSubclass(editCtl, EditSubclass, 1, (DWORD_PTR)this);
    PositionEditor();
    ShowWindow(editHost, SW_SHOW);
    SetForegroundWindow(editHost);
    SetFocus(editCtl);
    int len = GetWindowTextLengthW(editCtl);
    SendMessageW(editCtl, EM_SETSEL, len, len);
}

void NoteWin::PositionEditor()
{
    if (!editHost) return;
    float S = Scale();
    RECT wr;
    GetWindowRect(hwnd, &wr);
    float x0 = PadX, y0 = PadTop, x1;
    RECT rc;
    GetClientRect(hwnd, &rc);
    x1 = rc.right / S - PadX;
    if (editIndex == TitleIndex) { y0 = PadTop - scrollY; x1 -= 24; }
    else
    {
        bool found = false;
        for (int i = 0; i < rows.n; i++)
            if (rows[i].index == editIndex) { y0 = rows[i].top - scrollY; x0 = rows[i].textLeft; found = true; break; }
        if (!found) return;
    }
    // 输入框需要的高度：行数 × 行高
    HDC dc = GetDC(editCtl);
    HGDIOBJ of = SelectObject(dc, editFont);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, of);
    ReleaseDC(editCtl, dc);
    int lines = editIndex == TitleIndex ? 1 : (int)SendMessageW(editCtl, EM_GETLINECOUNT, 0, 0);
    int hpx = Max(1, lines) * tm.tmHeight + (int)(4 * S);
    float need = hpx / S;
    float base = editIndex == TitleIndex ? 24.f : LineH(d);
    if (need < base) { need = base; hpx = (int)(base * S + 0.5f); }
    int x = wr.left + (int)((x0 - 2) * S), y = wr.top + (int)(y0 * S);
    int w = (int)((x1 - x0 + 4) * S);
    SetWindowPos(editHost, HWND_TOP, x, y, w, hpx, SWP_NOACTIVATE);
    MoveWindow(editCtl, 0, (hpx - Max(1, lines) * tm.tmHeight) / 2, w, Max(1, lines) * tm.tmHeight + 2, TRUE);
    if (need != editHeightDip) { editHeightDip = need; Render(); }
}

void NoteWin::CloseEditor()
{
    if (editCtl) RemoveWindowSubclass(editCtl, EditSubclass, 1);
    if (editHost) { HWND h = editHost; editHost = nullptr; editCtl = nullptr; DestroyWindow(h); }
    if (editFont) { DeleteObject(editFont); editFont = nullptr; }
    if (editBrush) { DeleteObject(editBrush); editBrush = nullptr; }
}

LRESULT CALLBACK NoteWin::EditHostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    NoteWin* self = (NoteWin*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE)
    {
        self = (NoteWin*)((CREATESTRUCTW*)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (!self) return DefWindowProcW(h, m, w, l);
    switch (m)
    {
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w, RGB((self->d->textColor >> 16) & 255, (self->d->textColor >> 8) & 255, self->d->textColor & 255));
        {
            LOGBRUSH lb;
            GetObjectW(self->editBrush, sizeof(lb), &lb);
            SetBkColor((HDC)w, lb.lbColor);
        }
        return (LRESULT)self->editBrush;
    case WM_ERASEBKGND:
    {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, self->editBrush);
        return 1;
    }
    case WM_COMMAND:
        if (HIWORD(w) == EN_CHANGE) self->PositionEditor();
        return 0;
    case WM_SETFOCUS:
        if (self->editCtl) SetFocus(self->editCtl);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void NoteWin::CommitEdit(bool save)
{
    if (!Editing()) return;
    int idx = editIndex;
    Str text;
    if (editCtl)
    {
        int len = GetWindowTextLengthW(editCtl);
        Str raw;
        raw.Reserve(len);
        GetWindowTextW(editCtl, raw.p, len + 1);
        raw.n = WLen(raw.p);
        // 条目内部换行统一存成 \n，去掉每行行尾空格（Tab 保留），首尾空行去掉
        Str norm = raw.Replace(L"\r\n", L"\n");
        Str out;
        int start = 0;
        for (int i = 0; i <= norm.n; i++)
            if (i == norm.n || norm.p[i] == L'\n')
            {
                int e = i;
                while (e > start && norm.p[e - 1] == L' ') e--;
                out.Append(norm.p + start, e - start);
                if (i < norm.n) out.Ch(L'\n');
                start = i + 1;
            }
        int a = 0, b = out.n;
        while (a < b && (out.p[a] == L'\n' || out.p[a] == L' ')) a++;
        while (b > a && (out.p[b - 1] == L'\n' || out.p[b - 1] == L' ')) b--;
        text = out.Sub(a, b - a);
        if (idx == TitleIndex) text = text.Replace(L"\n", L" ").Trim();
    }
    editIndex = NotEditing;
    CloseEditor();
    if (pendingReload && changeDepth == 0)
    {
        // 编辑期间别处改了这张便签：先换成外部内容，再把这次编辑合进去
        NoteData* pr = pendingReload;
        pendingReload = nullptr;
        ApplyExternal(pr);
        delete pr;
        if (idx >= 0 && (idx >= d->items.n || d->items[idx].text != editOrig) && !editOrig.Empty())
        {
            int found = -1;
            for (int i = 0; i < d->items.n; i++) if (d->items[i].text == editOrig) { found = i; break; }
            idx = found >= 0 ? found : d->items.n; // 原条目在别处被改或删了：这次编辑作为新条目加上，不丢
        }
    }
    if (save)
    {
        BeginChange();
        if (idx == TitleIndex) d->title = text.n ? text : Str(L"ToDo");
        else if (idx < d->items.n)
        {
            if (text.Trim().n) d->items[idx].text = text;
            else TrashItemAt(idx); // 清空文字 = 删除，进回收站
        }
        else if (text.Trim().n)
        {
            NoteItem it;
            it.text = text;
            d->items.Push(it);
        }
        Save();
        EndChange();
    }
    Render();
    SendToBottom();
}

// =====================================================================
// 链接
// =====================================================================
bool NoteWin::TryOpenLink(int x, int y)
{
    float S = Scale();
    float xd = x / S, yd = y / S;
    int idx = HitRow(yd);
    if (idx < 0 || idx >= d->items.n) return false;
    const RowInfo* row = nullptr;
    for (int i = 0; i < rows.n; i++) if (rows[i].index == idx) row = &rows[i];
    if (!row) return false;
    RECT rc;
    GetClientRect(hwnd, &rc);
    IDWriteTextFormat* fb = MakeFormat((float)d->fontSize, d->fontWeight, true);
    IDWriteTextLayout* l = ItemLayout(d, d->items[idx], fb, rc.right / S - PadX - row->textLeft, nullptr, nullptr);
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS m = {};
    l->HitTestPoint(xd - row->textLeft, yd + scrollY - row->top, &trailing, &inside, &m);
    SafeRelease(l);
    SafeRelease(fb);
    if (!inside) return false;
    Vec<LinkSpan> links;
    FindLinks(d->items[idx].text, links);
    for (int i = 0; i < links.n; i++)
        if ((int)m.textPosition >= links[i].start && (int)m.textPosition < links[i].start + links[i].len)
        {
            ShellExecuteW(nullptr, L"open", links[i].url.c(), nullptr, nullptr, SW_SHOWNORMAL);
            return true;
        }
    return false;
}

// =====================================================================
// 鼠标
// =====================================================================
void NoteWin::OnLButtonDown(int x, int y, bool dbl)
{
    float S = Scale();
    if (d->collapsed)
    {
        // 胶囊：按住拖动；没拖动就是单击，展开
        ClosePreview();
        KillTimer(hwnd, TimerSlide);
        sliding = false;
        if (capHidden) { SetWindowPos(hwnd, nullptr, dockedX, dockedY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); capHidden = false; }
        RECT before;
        GetWindowRect(hwnd, &before);
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        RECT after;
        GetWindowRect(hwnd, &after);
        if (Abs(after.left - before.left) < 3 && Abs(after.top - before.top) < 3) Expand();
        else { SnapCapsule(); SaveLayout(); ScheduleCapsuleHide(); }
        SendToBottom();
        return;
    }
    int edge = HitEdge(x, y);
    if (edge)
    {
        ReleaseCapture();
        SendMessageW(hwnd, WM_SYSCOMMAND, 0xF000 + edge, 0);
        return;
    }
    // 正在编辑时点到别处：先保存这一条
    if (Editing()) { CommitEdit(true); return; }
    SetFocus(hwnd);
    if (GetKeyState(VK_CONTROL) < 0 && TryOpenLink(x, y)) return;
    float xd = x / S, yd = y / S;
    // 折叠按钮
    if (xd >= collapseBtn.left && xd <= collapseBtn.right && yd + scrollY >= collapseBtn.top && yd + scrollY <= collapseBtn.bottom)
    {
        Collapse();
        return;
    }
    int idx = HitRow(yd);
    if (idx >= 0 && idx < d->items.n)
    {
        const RowInfo* row = nullptr;
        for (int i = 0; i < rows.n; i++) if (rows[i].index == idx) row = &rows[i];
        if (row)
        {
            float handleRight = PadX + HandleW(d);
            if (xd >= PadX - 4 && xd < handleRight && !dbl) { BeginReorder(idx); return; }
            if (d->checkboxes && xd >= handleRight && xd < handleRight + CheckColW(d) && !dbl)
            {
                BeginChange();
                d->items[idx].done = !d->items[idx].done;
                EndChange();
                Save();
                Render();
                return;
            }
        }
    }
    if (dbl)
    {
        BeginEdit(idx == NotEditing ? d->items.n : idx); // 双击空白处 = 新增一条
        return;
    }
    ReleaseCapture();
    SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    SaveLayout();
    SendToBottom();
}

// =====================================================================
// 窗口模式：置底在桌面 / 置顶 / 普通窗口；快捷键临时提到最前
// =====================================================================
void NoteWin::ApplyMode()
{
    if (!hwnd) return;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (d->mode == ModeNormal) ex = (ex & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW; // 进任务栏和 Alt+Tab
    else ex = (ex | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    if (d->mode == ModeDesktop)
    {
        SetWindowPos(hwnd, peeking ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!peeking) AttachToDesktop();
    }
    else
    {
        SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, 0);
        SetWindowPos(hwnd, d->mode == ModeTopmost || peeking ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void NoteWin::SetMode(WinMode m)
{
    if (m == d->mode) return;
    d->mode = m;
    // 任务栏按钮要在窗口重新显示时才会增减
    ShowWindow(hwnd, SW_HIDE);
    ApplyMode();
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
}

// 置底模式：让桌面(Progman)成为所有者，Win+D 显示桌面时不会被藏起来；开机时桌面可能还没建好，就每 2 秒重试
void NoteWin::AttachToDesktop()
{
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman)
    {
        SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)progman);
        SendToBottom();
        KillTimer(hwnd, TimerProgman);
        return;
    }
    SetTimer(hwnd, TimerProgman, 2000, nullptr);
}

void NoteWin::SendToBottom()
{
    if (hwnd && d->mode == ModeDesktop && !Editing() && !peeking)
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void NoteWin::BeginPeek()
{
    peeking = true;
    KillTimer(hwnd, TimerHide);
    if (capHidden) { KillTimer(hwnd, TimerSlide); sliding = false; SetWindowPos(hwnd, nullptr, dockedX, dockedY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); capHidden = false; }
    // 挂在桌面窗口下的便签设不了置顶，先摘下来，结束时再挂回去
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, 0);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void NoteWin::EndPeek()
{
    if (!peeking) return;
    peeking = false;
    CommitEdit(true);
    ApplyMode();
    if (d->collapsed) ScheduleCapsuleHide();
}

// =====================================================================
// 位置：用物理像素保存和恢复
// =====================================================================
void NoteWin::RestorePosition()
{
    if (d->hasPos)
    {
        RECT probe = { (int)d->x, (int)d->y, (int)d->x + 40, (int)d->y + 20 };
        if (MonitorFromRect(&probe, MONITOR_DEFAULTTONULL))
        {
            SetWindowPos(hwnd, nullptr, (int)d->x, (int)d->y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return;
        }
    }
    PlaceDefault();
}

void NoteWin::PlaceDefault()
{
    POINT p0 = { 0, 0 };
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(p0, MONITOR_DEFAULTTOPRIMARY), &mi);
    float S = Scale();
    int off = (int)(Max(0, g_app->IndexOf(this)) * 30 * S);
    SetWindowPos(hwnd, nullptr, mi.rcWork.left + (int)(80 * S) + off, mi.rcWork.top + (int)(80 * S) + off, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void NoteWin::BringOnScreen()
{
    if (!hwnd) return;
    if (capHidden) { SetWindowPos(hwnd, nullptr, dockedX, dockedY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); capHidden = false; }
    RECT r;
    GetWindowRect(hwnd, &r);
    MONITORINFO mi = { sizeof(mi) };
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    bool visible = false;
    if (mon && GetMonitorInfoW(mon, &mi))
    {
        RECT in;
        IntersectRect(&in, &mi.rcWork, &r);
        visible = in.right - in.left >= Min((int)(r.right - r.left), 60) && in.bottom - in.top >= Min((int)(r.bottom - r.top), 20);
    }
    if (!visible) { PlaceDefault(); if (d->collapsed) SnapCapsule(); }
    SaveLayout();
}

void NoteWin::SaveLayout()
{
    if (!hwnd) return;
    RECT r;
    GetWindowRect(hwnd, &r);
    if (!d->collapsed)
    {
        float S = Scale();
        d->width = (r.right - r.left) / S;
        d->height = (r.bottom - r.top) / S;
    }
    d->hasPos = true;
    if (capHidden) { d->x = dockedX; d->y = dockedY; }
    else { d->x = r.left; d->y = r.top; }
    Save();
}

RECT NoteWin::WorkAreaPx() const
{
    RECT r;
    GetWindowRect(hwnd, &r);
    POINT c = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcWork;
}

// =====================================================================
// 胶囊：折叠、吸边、贴边后自动缩进、悬停预览
// =====================================================================
void NoteWin::Collapse()
{
    if (d->collapsed) return;
    CommitEdit(true);
    SaveLayout();
    RECT wa = WorkAreaPx(), r;
    GetWindowRect(hwnd, &r);
    int snap = (int)(24 * Scale());
    bool keepRight = Abs(wa.right - r.right) < snap, keepBottom = Abs(wa.bottom - r.bottom) < snap;
    EnterCollapsed();
    RECT c;
    GetWindowRect(hwnd, &c);
    int x = c.left, y = c.top;
    if (keepRight) x = wa.right - (c.right - c.left);
    if (keepBottom) y = wa.bottom - (c.bottom - c.top);
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SnapCapsule();
    SaveLayout();
    ScheduleCapsuleHide();
}

void NoteWin::EnterCollapsed()
{
    d->collapsed = true;
    hover = false;
    Render();
}

void NoteWin::Expand()
{
    if (!d->collapsed) return;
    KillTimer(hwnd, TimerHide);
    KillTimer(hwnd, TimerSlide);
    sliding = false;
    ClosePreview();
    if (capHidden) { SetWindowPos(hwnd, nullptr, dockedX, dockedY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); capHidden = false; }
    // 贴在右/下边的胶囊，展开时朝屏幕里面展开
    RECT wa = WorkAreaPx(), c;
    GetWindowRect(hwnd, &c);
    bool wasR = dockR, wasB = dockB;
    dockL = dockR = dockT = dockB = false;
    d->collapsed = false;
    float S = Scale();
    int w = (int)(d->width * S + 0.5), h = (int)(d->height * S + 0.5);
    int x = wasR ? c.right - w : c.left, y = wasB ? c.bottom - h : c.top;
    // 不跑出当前屏幕
    if (x + w > wa.right) x = wa.right - w;
    if (y + h > wa.bottom) y = wa.bottom - h;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    Render();
    SaveLayout();
}

// 胶囊离屏幕边缘不到 24 DIP 就贴上去；跑出屏幕的部分拉回来；贴着的一侧改成直角
void NoteWin::SnapCapsule()
{
    if (!d->collapsed || capHidden) return;
    RECT wa = WorkAreaPx(), r;
    GetWindowRect(hwnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top, x = r.left, y = r.top;
    int snap = (int)(24 * Scale());
    if (x - wa.left < snap) x = wa.left;
    else if (wa.right - (x + w) < snap) x = wa.right - w;
    if (y - wa.top < snap) y = wa.top;
    else if (wa.bottom - (y + h) < snap) y = wa.bottom - h;
    bool l = x == wa.left, rr = x + w == wa.right, t = y == wa.top, b = y + h == wa.bottom;
    bool changed = l != dockL || rr != dockR || t != dockT || b != dockB;
    dockL = l; dockR = rr; dockT = t; dockB = b;
    if (x != r.left || y != r.top) SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (changed) Render();
}

void NoteWin::ScheduleCapsuleHide()
{
    if (!d->collapsed) return;
    SetTimer(hwnd, TimerHide, 700, nullptr);
}

bool NoteWin::OtherScreenAt(int x, int y) const
{
    POINT p = { x, y };
    return MonitorFromPoint(p, MONITOR_DEFAULTTONULL) != nullptr;
}

void NoteWin::OnHideTimer()
{
    KillTimer(hwnd, TimerHide);
    POINT pt;
    GetCursorPos(&pt);
    RECT r;
    GetWindowRect(hwnd, &r);
    bool overMe = PtInRect(&r, pt) != FALSE;
    bool overPreview = false;
    if (preview && IsWindowVisible(preview)) { RECT pr; GetWindowRect(preview, &pr); overPreview = PtInRect(&pr, pt) != FALSE; }
    if (!d->collapsed || overMe || overPreview || peeking) { if (d->collapsed && (overMe || overPreview)) ScheduleCapsuleHide(); return; }
    ClosePreview();
    if (capHidden || !(dockL || dockR || dockT || dockB)) return;
    RECT wa = WorkAreaPx();
    int w = r.right - r.left, h = r.bottom - r.top, x = r.left, y = r.top;
    int strip = (int)(5 * Scale());
    // 优先左右缩进；缩进方向那边要是挨着另一块屏幕就不缩，免得跑到那块屏上
    if (dockL && !OtherScreenAt(wa.left - 2, r.top + h / 2)) x = wa.left - w + strip;
    else if (dockR && !OtherScreenAt(wa.right + 2, r.top + h / 2)) x = wa.right - strip;
    else if (dockT && !OtherScreenAt(r.left + w / 2, wa.top - 2)) y = wa.top - h + strip;
    else if (dockB && !OtherScreenAt(r.left + w / 2, wa.bottom + 2)) y = wa.bottom - strip;
    else return;
    dockedX = r.left;
    dockedY = r.top;
    capHidden = true;
    SlideTo(x, y);
}

// 缓动移动窗口（约 150ms）
void NoteWin::SlideTo(int x, int y)
{
    RECT r;
    GetWindowRect(hwnd, &r);
    slideFromX = r.left; slideFromY = r.top; slideToX = x; slideToY = y;
    slideStart = GetTickCount();
    sliding = true;
    SetTimer(hwnd, TimerSlide, 8, nullptr);
}

// ---- 悬停预览卡片：单独一个分层窗口，画出所有条目 ----
void NoteWin::ShowPreview()
{
    if (!d->collapsed || dragIndex >= 0) return;
    if (!preview)
        preview = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, PreviewClass, L"DeskNote 预览", WS_POPUP,
                                  0, 0, 10, 10, hwnd, nullptr, g_inst, this);
    RenderPreview();
    ShowWindow(preview, SW_SHOWNOACTIVATE);
}

void NoteWin::ClosePreview()
{
    if (preview) { DestroyWindow(preview); preview = nullptr; }
    previewHover = false;
}

void NoteWin::RenderPreview()
{
    if (!preview) return;
    float S = Scale();
    float fs = (float)d->fontSize, ts = Min(TitleSize(d), 18.f), lineH = LineH(d);
    const float padX = 16, padT = 12, padB = 10, maxW = 340, maxH = 460;
    IDWriteTextFormat* ft = MakeFormat(ts, 700, false);
    IDWriteTextFormat* fb = MakeFormat(fs, d->fontWeight, true);
    IDWriteTextLayout* lt = MakeLayout(d->title, ft, maxW - 2 * padX, 0);
    float contentW = Min(LayoutWidth(lt), maxW - 2 * padX);
    float bulletW = fs * 0.9f + 8;
    Vec<IDWriteTextLayout*> ls;
    float h = padT + LayoutHeight(lt) + 8;
    if (d->items.n == 0)
    {
        IDWriteTextLayout* l = MakeLayout(Str(L"（空）"), fb, maxW - 2 * padX, lineH);
        contentW = Max(contentW, LayoutWidth(l));
        ls.Push(l);
        h += LayoutHeight(l);
    }
    for (int i = 0; i < d->items.n; i++)
    {
        IDWriteTextLayout* l = ItemLayout(d, d->items[i], fb, maxW - 2 * padX - bulletW, nullptr, nullptr);
        contentW = Max(contentW, Min(LayoutWidth(l) + bulletW, maxW - 2 * padX));
        ls.Push(l);
        h += Max(lineH, LayoutHeight(l)) + R0(fs * 0.35f);
    }
    float W = contentW + 2 * padX, H = Min(h + padB, maxH);
    Canvas c;
    if (c.Begin((int)(W * S + 0.99f), (int)(H * S + 0.99f), 96 * S))
    {
        ID2D1SolidColorBrush *bg = nullptr, *fg = nullptr, *dim = nullptr;
        c.rt->CreateSolidColorBrush(Gfx::Color(d->bgColor, (float)Max(d->opacity, 0.92)), &bg);
        c.rt->CreateSolidColorBrush(Gfx::Color(d->textColor, 1), &fg);
        c.rt->CreateSolidColorBrush(Gfx::Color(d->textColor, 0xA0 / 255.f), &dim);
        c.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        c.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), 6, 6), bg);
        c.rt->PushAxisAlignedClip(D2D1::RectF(0, 0, W, H - padB / 2), D2D1_ANTIALIAS_MODE_ALIASED);
        float y = padT;
        c.rt->DrawTextLayout(D2D1::Point2F(padX, y), lt, fg, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        y += LayoutHeight(lt) + 8;
        IDWriteTextLayout* lb = MakeLayout(Str(L"•"), fb, bulletW, lineH);
        IDWriteTextLayout* lc = MakeLayout(Str(L"✓"), fb, bulletW, lineH);
        for (int i = 0; i < ls.n; i++)
        {
            if (d->items.n == 0) { c.rt->DrawTextLayout(D2D1::Point2F(padX, y), ls[i], dim); break; }
            const NoteItem& it = d->items[i];
            c.rt->DrawTextLayout(D2D1::Point2F(padX, y), d->checkboxes && it.done ? lc : lb, dim);
            ID2D1SolidColorBrush* tb = nullptr;
            c.rt->CreateSolidColorBrush(Gfx::Color(d->textColor, d->checkboxes && it.done ? 0.5f : 1.f), &tb);
            c.rt->DrawTextLayout(D2D1::Point2F(padX + bulletW, y), ls[i], tb, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            SafeRelease(tb);
            y += Max(lineH, LayoutHeight(ls[i])) + R0(fs * 0.35f);
        }
        c.rt->PopAxisAlignedClip();
        SafeRelease(lb); SafeRelease(lc);
        SafeRelease(bg); SafeRelease(fg); SafeRelease(dim);
    }
    // 往屏幕里面的方向弹出
    RECT r;
    GetWindowRect(hwnd, &r);
    int pw = c.pw, ph = c.ph, gap = (int)(6 * S), x, y;
    if (dockR) { x = (capHidden ? dockedX : r.left) - pw - gap; y = r.top; }
    else if (dockL) { x = (capHidden ? dockedX + (r.right - r.left) : r.right) + gap; y = r.top; }
    else if (dockB) { x = r.left; y = r.top - ph - gap; }
    else { x = r.left; y = r.bottom + gap; }
    RECT wa = WorkAreaPx();
    if (y + ph > wa.bottom) y = wa.bottom - ph;
    if (y < wa.top) y = wa.top;
    if (x + pw > wa.right) x = wa.right - pw;
    if (x < wa.left) x = wa.left;
    SetWindowPos(preview, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    c.End(preview);
    for (int i = 0; i < ls.n; i++) SafeRelease(ls[i]);
    SafeRelease(lt); SafeRelease(ft); SafeRelease(fb);
}

LRESULT CALLBACK NoteWin::PreviewProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    NoteWin* self = (NoteWin*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE)
    {
        self = (NoteWin*)((CREATESTRUCTW*)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (!self) return DefWindowProcW(h, m, w, l);
    switch (m)
    {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: self->Expand(); return 0;
    case WM_MOUSEMOVE:
        if (!self->previewHover)
        {
            self->previewHover = true;
            KillTimer(self->hwnd, TimerHide);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
        }
        return 0;
    case WM_MOUSELEAVE:
        self->previewHover = false;
        self->ScheduleCapsuleHide();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// =====================================================================
// 右键菜单
// =====================================================================
enum
{
    CmdUndo = 1, CmdRedo, CmdUp, CmdDown, CmdDelete, CmdRemind, CmdUnremind, CmdAdd, CmdClearDone,
    CmdFold, CmdChecks, CmdModeDesktop, CmdModeTopmost, CmdModeNormal, CmdAppearance, CmdShadow,
    CmdNewNote, CmdDeleteNote, CmdTrash, CmdSettings, CmdExit,
    CmdWeight = 100, CmdTextColor = 200, CmdTextColorCustom = 220, CmdBgColor = 300, CmdBgColorCustom = 320
};
struct Swatch { const wchar_t* name; u32 rgb; };
static const Swatch TextSwatches[] = {
    { L"默认灰", DefaultTextColor }, { L"白", 0xF5F5F5 }, { L"黑", 0x222222 }, { L"黄", 0xF2C94C },
    { L"橙", 0xF08A3C }, { L"红", 0xE55B5B }, { L"绿", 0x6CC27A }, { L"蓝", 0x5BA8E5 } };
static const Swatch BgSwatches[] = {
    { L"默认蓝灰", DefaultBgColor }, { L"黑", 0x1E1E1E }, { L"白", 0xFAFAFA }, { L"便签黄", 0xFFF1A8 },
    { L"浅绿", 0xD4F0D0 }, { L"浅蓝", 0xD2E6F8 }, { L"浅粉", 0xF9D9E3 }, { L"墨绿", 0x233B33 } };
static const int Weights[] = { 300, 400, 600, 700 };
static const wchar_t* WeightNames[] = { L"细", L"常规", L"中粗", L"粗" };

static void AddItem(HMENU m, int id, const wchar_t* text, bool checked = false, bool radio = false)
{
    MENUITEMINFOW mi = { sizeof(mi) };
    mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
    mi.fType = MFT_STRING | (radio ? MFT_RADIOCHECK : 0);
    mi.fState = checked ? MFS_CHECKED : 0;
    mi.wID = id;
    mi.dwTypeData = (LPWSTR)text;
    InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
}
static void AddSep(HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); }

void NoteWin::OnContextMenu(int sx, int sy)
{
    CommitEdit(true);
    ClosePreview();
    int target = NotEditing;
    if (!d->collapsed)
    {
        POINT p = { sx, sy };
        ScreenToClient(hwnd, &p);
        target = HitRow(p.y / Scale());
    }
    bool onItem = !d->collapsed && target >= 0 && target < d->items.n;
    HMENU m = CreatePopupMenu();
    if (undo.n) AddItem(m, CmdUndo, L"撤销\tCtrl+Z");
    if (redo.n) AddItem(m, CmdRedo, L"重做\tCtrl+Y");
    if (undo.n || redo.n) AddSep(m);
    if (onItem && target > 0) AddItem(m, CmdUp, L"上移");
    if (onItem && target < d->items.n - 1) AddItem(m, CmdDown, L"下移");
    if (onItem) AddItem(m, CmdDelete, L"删除这一条");
    AddItem(m, CmdAdd, L"新增一条");
    if (onItem)
    {
        AddItem(m, CmdRemind, d->items[target].hasRemind ? L"修改提醒…" : L"设置提醒…");
        if (d->items[target].hasRemind) AddItem(m, CmdUnremind, L"取消提醒");
    }
    bool anyDone = false;
    for (int i = 0; i < d->items.n; i++) if (d->items[i].done) anyDone = true;
    if (d->checkboxes && anyDone) AddItem(m, CmdClearDone, L"清除已完成的条目");
    AddSep(m);
    AddItem(m, CmdFold, d->collapsed ? L"展开" : L"折叠成胶囊");
    AddItem(m, CmdChecks, L"显示勾选框", d->checkboxes);
    HMENU modes = CreatePopupMenu();
    AddItem(modes, CmdModeDesktop, L"置底在桌面（显示桌面时也在）", d->mode == ModeDesktop, true);
    AddItem(modes, CmdModeTopmost, L"置顶（总在最前面）", d->mode == ModeTopmost, true);
    AddItem(modes, CmdModeNormal, L"普通窗口（出现在任务栏）", d->mode == ModeNormal, true);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)modes, L"窗口模式");
    AddSep(m);
    AddItem(m, CmdAppearance, L"不透明度和字号…");
    HMENU weight = CreatePopupMenu();
    for (int i = 0; i < 4; i++) AddItem(weight, CmdWeight + i, WeightNames[i], d->fontWeight == Weights[i], true);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)weight, L"粗细");
    HMENU tc = CreatePopupMenu();
    for (int i = 0; i < 8; i++) AddItem(tc, CmdTextColor + i, TextSwatches[i].name, d->textColor == TextSwatches[i].rgb, true);
    AddSep(tc);
    AddItem(tc, CmdTextColorCustom, L"自定义…");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)tc, L"文字颜色");
    HMENU bc = CreatePopupMenu();
    for (int i = 0; i < 8; i++) AddItem(bc, CmdBgColor + i, BgSwatches[i].name, d->bgColor == BgSwatches[i].rgb, true);
    AddSep(bc);
    AddItem(bc, CmdBgColorCustom, L"自定义…");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)bc, L"背景颜色");
    AddItem(m, CmdShadow, L"文字阴影（背景很透明时更清楚）", d->shadow);
    AddSep(m);
    AddItem(m, CmdNewNote, L"新建便签");
    AddItem(m, CmdDeleteNote, L"删除这张便签…");
    AddItem(m, CmdTrash, L"回收站…");
    AddItem(m, CmdSettings, L"设置…");
    AddSep(m);
    AddItem(m, CmdExit, L"退出 DeskNote");
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, hwnd, nullptr);
    DestroyMenu(m);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    if (cmd) RunCommand(cmd, target);
    if (hwnd && d->collapsed) ScheduleCapsuleHide();
}

static void ApplyAppearance(void* ctx) { ((NoteWin*)ctx)->Render(); }

void NoteWin::RunCommand(int cmd, int target)
{
    bool onItem = target >= 0 && target < d->items.n;
    switch (cmd)
    {
    case CmdUndo: Undo(); return;
    case CmdRedo: Redo(); return;
    case CmdNewNote: g_app->CreateNote(this); return;
    case CmdDeleteNote: g_app->DeleteNote(this, true); return;
    case CmdTrash: g_app->ShowTrash(); return;
    case CmdSettings: g_app->ShowSettings(); return;
    case CmdExit: g_app->Exit(); return;
    case CmdAdd: Expand(); BeginEdit(d->items.n); return;
    case CmdRemind: if (onItem) EditReminder(target); return;
    case CmdFold: if (d->collapsed) Expand(); else Collapse(); return;
    case CmdTextColorCustom: PickColor(false); return;
    case CmdBgColorCustom: PickColor(true); return;
    case CmdAppearance:
        BeginChange();
        AppearanceDialog(hwnd, d, ApplyAppearance, this);
        Save();
        EndChange();
        Render();
        SendToBottom();
        return;
    }
    // 其余都是改内容/外观：算一步撤销
    BeginChange();
    switch (cmd)
    {
    case CmdUp: if (onItem) MoveItem(target, target - 1); break;
    case CmdDown: if (onItem) MoveItem(target, target + 1); break;
    case CmdDelete: if (onItem) TrashItemAt(target); break;
    case CmdUnremind: if (onItem) { d->items[target].hasRemind = false; d->items[target].reminded = false; } break;
    case CmdClearDone: ClearDone(); break;
    case CmdChecks: d->checkboxes = !d->checkboxes; break;
    case CmdShadow: d->shadow = !d->shadow; break;
    case CmdModeDesktop: SetMode(ModeDesktop); break;
    case CmdModeTopmost: SetMode(ModeTopmost); break;
    case CmdModeNormal: SetMode(ModeNormal); break;
    default:
        if (cmd >= CmdWeight && cmd < CmdWeight + 4) d->fontWeight = Weights[cmd - CmdWeight];
        else if (cmd >= CmdTextColor && cmd < CmdTextColor + 8) d->textColor = TextSwatches[cmd - CmdTextColor].rgb;
        else if (cmd >= CmdBgColor && cmd < CmdBgColor + 8) d->bgColor = BgSwatches[cmd - CmdBgColor].rgb;
        break;
    }
    Save();
    EndChange();
    Render();
    SendToBottom();
}

void NoteWin::PickColor(bool background)
{
    static COLORREF custom[16];
    u32 cur = background ? d->bgColor : d->textColor;
    CHOOSECOLORW cc = { sizeof(cc) };
    cc.hwndOwner = hwnd;
    cc.rgbResult = RGB((cur >> 16) & 255, (cur >> 8) & 255, cur & 255);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc))
    {
        BeginChange();
        u32 v = (GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
        if (background) d->bgColor = v; else d->textColor = v;
        Save();
        EndChange();
        Render();
    }
    SendToBottom();
}

void NoteWin::EditReminder(int index)
{
    if (index < 0 || index >= d->items.n) return;
    NoteItem cur = d->items[index];
    Time t;
    if (!ReminderDialog(hwnd, cur.text, cur.hasRemind, cur.remind, &t)) { SendToBottom(); return; }
    if (index >= d->items.n || d->items[index].text != cur.text) return; // 对话框开着时条目被别处改了
    BeginChange();
    d->items[index].hasRemind = true;
    d->items[index].remind = t;
    d->items[index].reminded = false;
    Save();
    EndChange();
    Render();
    SendToBottom();
}

void NoteWin::OnRemindersFired()
{
    Save();
    if (!Editing()) Render(); // 编辑中先不重绘，结束时自然会刷新
}

// =====================================================================
// 窗口消息
// =====================================================================
LRESULT CALLBACK NoteWin::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    NoteWin* self = (NoteWin*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE)
    {
        self = (NoteWin*)((CREATESTRUCTW*)l)->lpCreateParams;
        self->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (!self || !self->hwnd) return DefWindowProcW(h, m, w, l);
    return self->Handle(m, w, l);
}

LRESULT NoteWin::Handle(UINT m, WPARAM w, LPARAM l)
{
    if (taskbarCreatedMsg && m == (UINT)taskbarCreatedMsg)
    {
        // 资源管理器重启：重新挂到新的桌面窗口上
        if (d->mode == ModeDesktop && !peeking) AttachToDesktop();
        return 0;
    }
    switch (m)
    {
    case WM_NCHITTEST: return HTCLIENT;
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    case WM_SETCURSOR:
    {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        LPCWSTR cur = IDC_ARROW;
        if (!d->collapsed && dragIndex < 0)
        {
            switch (HitEdge(p.x, p.y))
            {
            case 1: case 2: cur = IDC_SIZEWE; break;
            case 3: case 6: cur = IDC_SIZENS; break;
            case 4: case 8: cur = IDC_SIZENWSE; break;
            case 5: case 7: cur = IDC_SIZENESW; break;
            default:
            {
                float xd = p.x / Scale();
                int idx = HitRow(p.y / Scale());
                float hl = PadX + HandleW(d);
                if (idx >= 0 && idx < d->items.n && xd >= PadX - 4 && xd < hl) cur = IDC_SIZENS;
                else if (idx >= 0 && idx < d->items.n && d->checkboxes && xd >= hl && xd < hl + CheckColW(d)) cur = IDC_HAND;
            }
            }
        }
        else if (dragIndex >= 0) cur = IDC_SIZENS;
        SetCursor(LoadCursorW(nullptr, cur));
        return TRUE;
    }
    case WM_LBUTTONDOWN: OnLButtonDown(GET_X_LPARAM(l), GET_Y_LPARAM(l), false); return 0;
    case WM_LBUTTONDBLCLK: OnLButtonDown(GET_X_LPARAM(l), GET_Y_LPARAM(l), true); return 0;
    case WM_LBUTTONUP: if (dragIndex >= 0) EndReorder(); return 0;
    case WM_CAPTURECHANGED: if (dragIndex >= 0 && (HWND)l != hwnd) EndReorder(); return 0;
    case WM_MOUSEMOVE:
        if (dragIndex >= 0) { OnReorderMove(GET_Y_LPARAM(l) / Scale()); return 0; }
        if (!hover)
        {
            hover = true;
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&t);
            if (d->collapsed)
            {
                KillTimer(hwnd, TimerHide);
                if (capHidden) { capHidden = false; SlideTo(dockedX, dockedY); }
                ShowPreview();
            }
            else Render();
        }
        return 0;
    case WM_MOUSELEAVE:
        hover = false;
        if (d->collapsed) ScheduleCapsuleHide();
        else Render();
        return 0;
    case WM_MOUSEWHEEL:
    {
        if (d->collapsed) return 0;
        RECT rc;
        GetClientRect(hwnd, &rc);
        float H = rc.bottom / Scale();
        float maxScroll = Max(0.f, contentHeight - (H - PadTop - PadBottom));
        float ns = Clamp(scrollY - GET_WHEEL_DELTA_WPARAM(w) / 120.f * LineH(d) * 3, 0.f, maxScroll);
        if (ns != scrollY) { scrollY = ns; Render(); PositionEditor(); }
        return 0;
    }
    case WM_RBUTTONUP:
    {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ClientToScreen(hwnd, &p);
        OnContextMenu(p.x, p.y);
        return 0;
    }
    case WM_KEYDOWN:
    {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (ctrl && w == 'Z' && !shift) { Undo(); return 0; }
        if (ctrl && (w == 'Y' || (w == 'Z' && shift))) { Redo(); return 0; }
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE)
        {
            HWND to = (HWND)l;
            if (Editing() && to != editHost) CommitEdit(true);
            if (!Editing()) SendToBottom();
        }
        return 0;
    case WM_APP_COMMITEDIT:
    {
        HWND f = GetFocus();
        if (Editing() && f != editCtl) CommitEdit(true);
        return 0;
    }
    case WM_WINDOWPOSCHANGING:
        if (d->mode == ModeDesktop && !Editing() && !peeking)
        {
            // 置底模式下（非编辑时）始终压在其他窗口下面
            WINDOWPOS* wp = (WINDOWPOS*)l;
            if (!(wp->flags & SWP_NOZORDER)) wp->hwndInsertAfter = HWND_BOTTOM;
        }
        break;
    case WM_ENTERSIZEMOVE:
        sizing = true;
        return 0;
    case WM_EXITSIZEMOVE:
        sizing = false;
        SaveLayout();
        return 0;
    case WM_ACTIVATEAPP:
        // 点到别的程序：结束“临时提到最前”
        if (!w && g_app->peeking) g_app->EndPeek();
        break;
    case WM_SIZE:
        // 只在用户拖边框缩放时跟着改大小（创建窗口、UpdateLayeredWindow 也会触发 WM_SIZE）
        if (sizing && !d->collapsed)
        {
            float S = Scale();
            int pw = (int)(d->width * S + 0.5), ph = (int)(d->height * S + 0.5);
            int nw = LOWORD(l), nh = HIWORD(l);
            if ((nw != pw || nh != ph) && nw > 0 && nh > 0)
            {
                d->width = nw / S;
                d->height = nh / S;
                Render();
                PositionEditor();
            }
        }
        return 0;
    case WM_GETMINMAXINFO:
        if (!d->collapsed)
        {
            MINMAXINFO* mm = (MINMAXINFO*)l;
            mm->ptMinTrackSize.x = (int)(160 * Scale());
            mm->ptMinTrackSize.y = (int)(120 * Scale());
        }
        return 0;
    case WM_DPICHANGED:
    {
        CommitEdit(true);
        RECT* r = (RECT*)l;
        SetWindowPos(hwnd, nullptr, r->left, r->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Render();
        return 0;
    }
    case WM_TIMER:
        if (w == TimerHide) OnHideTimer();
        else if (w == TimerSlide)
        {
            float t = Min(1.f, (GetTickCount() - slideStart) / 150.f);
            float k = 1 - (1 - t) * (1 - t) * (1 - t); // ease-out
            SetWindowPos(hwnd, nullptr, slideFromX + (int)((slideToX - slideFromX) * k), slideFromY + (int)((slideToY - slideFromY) * k), 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            if (t >= 1) { KillTimer(hwnd, TimerSlide); sliding = false; }
        }
        else if (w == TimerProgman)
        {
            if (d->mode != ModeDesktop) KillTimer(hwnd, TimerProgman);
            else if (FindWindowW(L"Progman", nullptr)) AttachToDesktop();
        }
        return 0;
    case WM_CLOSE:
        return 0; // 只有退出程序或删除便签时才真正关闭
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// =====================================================================
// 自测入口：走和鼠标键盘一样的代码路径
// =====================================================================
void NoteWin::TestTypeItem(int index, const wchar_t* text)
{
    BeginEdit(index);
    SetWindowTextW(editCtl, text);
    CommitEdit(true);
}
void NoteWin::TestBeginEdit(int index, const wchar_t* text)
{
    BeginEdit(index);
    SetWindowTextW(editCtl, text);
}
void NoteWin::TestMenuDelete(int index)
{
    BeginChange();
    TrashItemAt(index);
    Save();
    EndChange();
    Render();
}
void NoteWin::TestToggleDone(int index)
{
    BeginChange();
    d->items[index].done = !d->items[index].done;
    EndChange();
    Save();
}
void NoteWin::TestSetRemind(int index, Time t)
{
    BeginChange();
    d->items[index].hasRemind = true;
    d->items[index].remind = t;
    d->items[index].reminded = false;
    Save();
    EndChange();
}
