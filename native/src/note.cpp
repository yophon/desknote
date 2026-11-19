// 一张便签的窗口：绘制、布局、鼠标键盘、编辑、排序、撤销、胶囊、窗口模式、右键菜单
#include "ui.h"
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
static float TitleSize(const NoteData* d) { return R1((float)d->fontSize * 1.25f); }
static float LineH(const NoteData* d) { return R0((float)d->fontSize * 1.45f); }
static float CheckSize(const NoteData* d) { return R0((float)d->fontSize * 0.95f); }
static float CheckColW(const NoteData* d) { return CheckSize(d) + R0((float)d->fontSize * 0.6f); }
static float RowGap(const NoteData* d) { return R0((float)d->fontSize * 0.4f); }
static const float PadX = 24, PadTop = 12, PadBottom = 14, TitleGap = 8;
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

// withTag：把提醒时间接在文字后面（悬停预览用；便签本身画成右侧的标签）
static IDWriteTextLayout* ItemLayout(const NoteData* d, const NoteItem& it, IDWriteTextFormat* f, float width, ID2D1Brush* tagBrush, bool withTag)
{
    int tag = -1;
    Str s = withTag ? ItemDisplay(it, &tag) : it.text;
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
static u32 Argb(u32 rgb, float a) { return ((u32)(Clamp(a, 0.f, 1.f) * 255 + 0.5f) << 24) | (rgb & 0xFFFFFF); }

// 提醒标签：条目右侧的小圆角标签（🔔 15:00），到点后变红
static bool IsDue(const NoteItem& it) { return it.hasRemind && it.reminded && !it.done; }
static float ChipFont(const NoteData* d) { return R1((float)d->fontSize * 0.78f); }
static float ChipH(const NoteData* d) { return R0(LineH(d) * 0.86f); }
static float ChipWidth(const NoteData* d, const NoteItem& it)
{
    if (!it.hasRemind) return 0;
    float f = ChipFont(d);
    return R0(Ui::Measure(FormatWhen(it.remind), f, IsDue(it) ? 600 : 400) + f + 4 + 16);
}
// 条目文字可用的宽度（有提醒标签时让出位置）
static float ItemTextWidth(const NoteData* d, const NoteItem& it, float avail)
{
    float cw = ChipWidth(d, it);
    return cw > 0 ? Max(40.f, avail - cw - 8) : avail;
}
static void DrawChip(ID2D1RenderTarget* rt, const NoteData* d, const NoteItem& it, float x, float y, float alpha)
{
    float f = ChipFont(d), h = ChipH(d), w = ChipWidth(d, it);
    bool due = IsDue(it);
    u32 c = due ? DueColor : d->textColor;
    Ui::Fill(rt, D2D1::RectF(x, y, x + w, y + h), h / 2, Argb(c, (due ? 0.22f : 0.12f) * alpha));
    Ui::Icon(rt, 0xEA8F, x + 8 + f / 2, y + h / 2, f, Argb(c, (due ? 1.f : 0.8f) * alpha));
    Ui::Text(rt, FormatWhen(it.remind), D2D1::RectF(x + 8 + f + 4, y, x + w - 4, y + h), f, due ? 600 : 400, Argb(c, (due ? 1.f : 0.8f) * alpha));
}

// 勾选框：圆角方框；勾上后实心，中间一个对勾
static void DrawCheck(ID2D1RenderTarget* rt, const NoteData* d, float x, float y, float size, bool done, bool hot, float alpha)
{
    float r = R1(size * 0.26f);
    float bw = Max(1.25f, R1((float)d->fontSize / 11));
    D2D1_RECT_F b = D2D1::RectF(x, y, x + size, y + size);
    if (done)
    {
        Ui::Fill(rt, b, r, Argb(d->textColor, 0.8f * alpha));
        Ui::Icon(rt, 0xE73E, x + size / 2, y + size / 2, R1(size * 0.72f), Argb(d->bgColor, alpha));
    }
    else
    {
        if (hot) Ui::Fill(rt, b, r, Argb(d->textColor, 0.12f * alpha));
        Ui::Stroke(rt, b, r, Argb(d->textColor, (hot ? 0.85f : 0.6f) * alpha), bw);
    }
}

// 标题栏工具按钮的位置（DIP，内容坐标）
int NoteWin::HitToolbar(float xd, float yd) const
{
    if (!hover || Editing() || dragIndex >= 0) return 0;
    float y = yd + scrollY;
    for (int k = 0; k < 3; k++)
        if (xd >= toolBtn[k].left && xd < toolBtn[k].right && y >= toolBtn[k].top && y < toolBtn[k].bottom) return k + 1;
    return 0;
}

void NoteWin::Render()
{
    if (!hwnd) return;
    SetWindowTextW(hwnd, d->title.c());
    if (d->collapsed)
    {
        // ---- 胶囊：标题 + 未完成条数的小圆标 ----
        float S = Scale();
        float size = Min((float)d->fontSize, 15.f);
        IDWriteTextFormat* ft = MakeFormat(size, 600, false);
        int undone = 0;
        for (int i = 0; i < d->items.n; i++) if (!d->checkboxes || !d->items[i].done) undone++;
        Str count;
        count.Int(undone);
        IDWriteTextLayout* lt = MakeLayout(d->title, ft, 220, 0);
        float tw = Min(LayoutWidth(lt), 220.f);
        float th = LayoutHeight(lt);
        lt->SetMaxWidth(tw + 1);
        float cf = R1(size * 0.8f);
        float badgeH = R0(cf * 1.55f), badgeW = Max(badgeH, R0(Ui::Measure(count, cf, 600) + 12));
        float H = R0(Max(th, badgeH) + 12), W = R0(14 + tw + 8 + badgeW + 8);
        Canvas c;
        if (c.Begin((int)(W * S + 0.99f), (int)(H * S + 0.99f), 96 * S))
        {
            W = c.pw / S; H = c.ph / S;
            ID2D1SolidColorBrush *bg = nullptr, *fg = nullptr;
            c.rt->CreateSolidColorBrush(Gfx::Color(d->bgColor, (float)Max(d->opacity, 0.8)), &bg);
            c.rt->CreateSolidColorBrush(Gfx::Color(d->textColor, 1), &fg);
            const float r = H / 2;
            bool docked = dockL || dockR || dockT || dockB;
            FillCornerRect(c.rt, D2D1::RectF(0, 0, W, H), dockL || dockT ? 0 : r, dockR || dockT ? 0 : r,
                           dockR || dockB ? 0 : r, dockL || dockB ? 0 : r, bg);
            if (!docked) Ui::Stroke(c.rt, D2D1::RectF(0, 0, W, H), r, Argb(d->textColor, 0.14f), 1);
            c.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            c.rt->DrawTextLayout(D2D1::Point2F(14, (H - th) / 2), lt, fg, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
            D2D1_RECT_F br = D2D1::RectF(W - 8 - badgeW, (H - badgeH) / 2, W - 8, (H + badgeH) / 2);
            Ui::Fill(c.rt, br, badgeH / 2, Argb(d->textColor, 0.16f));
            Ui::Text(c.rt, count, br, cf, 600, Argb(d->textColor, 0.9f), Ui::Center);
            SafeRelease(bg); SafeRelease(fg);
        }
        c.End(hwnd);
        SafeRelease(lt); SafeRelease(ft);
        if (preview && IsWindowVisible(preview)) RenderPreview();
        return;
    }
    float S = Scale();
    int pw = (int)(d->width * S + 0.5), ph = (int)(d->height * S + 0.5);
    Canvas c;
    c.Begin(pw, ph, 96 * S);
    if (c.rt)
    {
        float W = pw / S, H = ph / S;
        ID2D1RenderTarget* rt = c.rt;
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        ID2D1SolidColorBrush *bg = nullptr, *fg = nullptr, *dim = nullptr, *shadow = nullptr;
        rt->CreateSolidColorBrush(Gfx::Color(d->bgColor, (float)Max(d->opacity, 1.0 / 255)), &bg);
        rt->CreateSolidColorBrush(Gfx::Color(d->textColor, 1), &fg);
        rt->CreateSolidColorBrush(Gfx::Color(d->textColor, 0.55f), &dim);
        rt->CreateSolidColorBrush(Gfx::Color(0, 0.75f), &shadow);
        const float radius = 8;
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), radius, radius), bg);
        // 细描边：背景很透明时只在鼠标移上来时出现，平时只剩文字
        if (d->opacity >= 0.12 || hover) Ui::Stroke(rt, D2D1::RectF(0, 0, W, H), radius, Argb(d->textColor, hover ? 0.18f : 0.1f), 1);

        bool showUi = hover && editIndex == NotEditing && dragIndex < 0 && !g_app->peeking;
        float left = PadX, right = W - PadX, top = PadTop, bottom = H - PadBottom;
        rt->PushAxisAlignedClip(D2D1::RectF(0, top - 6, W, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        float lineH = LineH(d), fs = (float)d->fontSize;
        float y = top - scrollY;

        // ---- 标题行：标题 + 悬停时出现的三个按钮 ----
        const float btn = 28, btnGap = 2, tbRight = W - 10;
        float ts = TitleSize(d);
        IDWriteTextFormat* ft = MakeFormat(ts, 700, false);
        float titleRight = showUi ? tbRight - 3 * btn - 2 * btnGap - 8 : right;
        IDWriteTextLayout* lt = MakeLayout(d->title, ft, titleRight - left, 0);
        float titleH = Max(LayoutHeight(lt), btn);
        if (editIndex == TitleIndex) titleH = Max(titleH, editHeightDip);
        else DrawLayout(rt, left, y + (titleH - LayoutHeight(lt)) / 2, lt, fg, d->shadow, shadow);
        float tcy = y + titleH / 2;
        static const wchar_t glyphs[3] = { 0xE710, 0xE712, 0xE73F };
        for (int k = 0; k < 3; k++)
        {
            float bx = tbRight - (3 - k) * btn - (2 - k) * btnGap;
            toolBtn[k] = D2D1::RectF(bx, tcy - btn / 2 + scrollY, bx + btn, tcy + btn / 2 + scrollY);
            if (!showUi) continue;
            D2D1_RECT_F r = D2D1::RectF(bx, tcy - btn / 2, bx + btn, tcy + btn / 2);
            if (hoverBtn == k + 1) Ui::Fill(rt, r, 5, Argb(d->textColor, 0.14f));
            Ui::Icon(rt, glyphs[k], bx + btn / 2, tcy, 14, Argb(d->textColor, hoverBtn == k + 1 ? 1.f : 0.75f));
        }
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

        float gap = RowGap(d);
        int n = d->items.n + (editIndex == d->items.n ? 1 : 0);
        for (int i = 0; i < n; i++)
        {
            bool real = i < d->items.n;
            const NoteItem* it = real ? &d->items[i] : nullptr;
            float rowAlpha = i == dragIndex ? 0.6f : 1;
            float textX = left + (d->checkboxes ? CheckColW(d) : 0);
            float avail = right - textX;
            bool editingRow = i == editIndex;
            bool dimmed = real && d->checkboxes && it->done;
            IDWriteTextLayout* l = nullptr;
            float h;
            if (editingRow) h = Max(lineH, editHeightDip);
            else
            {
                l = ItemLayout(d, *it, fb, ItemTextWidth(d, *it, avail), nullptr, false);
                h = Max(lineH, LayoutHeight(l));
            }
            // 悬停 / 拖动中的行：淡色底 + 左边距里的拖动手柄
            bool rowHot = real && !editingRow && (i == dragIndex || (showUi && hoverRow == i));
            if (rowHot)
            {
                Ui::Fill(rt, D2D1::RectF(6, y - gap / 2, W - 6, y + h + gap / 2), 6, Argb(d->textColor, i == dragIndex ? 0.16f : 0.07f));
                float cx = PadX / 2 + 2, cy = y + lineH / 2;
                for (int r = -1; r <= 1; r++)
                    for (int k = 0; k < 2; k++)
                        Ui::Circle(rt, cx - 2.5f + k * 5, cy + r * 5, 1.3f, Argb(d->textColor, 0.6f));
            }
            if (d->checkboxes)
            {
                float size = CheckSize(d);
                DrawCheck(rt, d, left, y + (lineH - size) / 2, size, real && it->done && !editingRow, rowHot, rowAlpha);
            }
            float chipLeft = 0;
            if (l)
            {
                ID2D1SolidColorBrush* tb = nullptr;
                rt->CreateSolidColorBrush(Gfx::Color(d->textColor, (dimmed ? 0.45f : 1.f) * rowAlpha), &tb);
                DrawLayout(rt, textX, y, l, tb, d->shadow && !dimmed, shadow);
                SafeRelease(tb);
                SafeRelease(l);
                if (it->hasRemind)
                {
                    chipLeft = right - ChipWidth(d, *it);
                    DrawChip(rt, d, *it, chipLeft, y + (lineH - ChipH(d)) / 2, rowAlpha * (dimmed ? 0.5f : 1.f));
                }
            }
            RowInfo ri = { i, y + scrollY, y + scrollY + h, textX, chipLeft };
            rows.Push(ri);
            y += h + gap;
        }
        SafeRelease(fb);
        contentHeight = y + scrollY - top;
        rt->PopAxisAlignedClip();
        // 内容超出时：右边一条细滚动条（鼠标移上来才显示）
        float viewH = bottom - top;
        if (hover && contentHeight > viewH + 1)
        {
            float th = Max(20.f, viewH * viewH / contentHeight);
            float ty = top + (viewH - th) * Clamp(scrollY / (contentHeight - viewH), 0.f, 1.f);
            Ui::Fill(rt, D2D1::RectF(W - 5, ty, W - 2, ty + th), 1.5f, Argb(d->textColor, 0.35f));
        }
        SafeRelease(bg); SafeRelease(fg); SafeRelease(dim); SafeRelease(shadow);
    }
    c.End(hwnd);
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
    if (editIndex == TitleIndex) y0 = PadTop - scrollY;
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
    float base = editIndex == TitleIndex ? 28.f : LineH(d);
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
    IDWriteTextLayout* l = ItemLayout(d, d->items[idx], fb, ItemTextWidth(d, d->items[idx], rc.right / S - PadX - row->textLeft), nullptr, false);
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
    // 标题栏按钮：新增、菜单、折叠
    switch (HitToolbar(xd, yd))
    {
    case 1: BeginEdit(d->items.n); return;
    case 2:
    {
        POINT p = { (int)(toolBtn[1].left * S), (int)((toolBtn[1].bottom - scrollY + 4) * S) };
        ClientToScreen(hwnd, &p);
        OnContextMenu(p.x, p.y);
        return;
    }
    case 3: Collapse(); return;
    }
    int idx = HitRow(yd);
    if (idx >= 0 && idx < d->items.n)
    {
        const RowInfo* row = nullptr;
        for (int i = 0; i < rows.n; i++) if (rows[i].index == idx) row = &rows[i];
        if (row)
        {
            // 左边距是拖动手柄，提醒标签点了改提醒
            if (xd < PadX && !dbl) { BeginReorder(idx); return; }
            if (row->chipLeft > 0 && xd >= row->chipLeft && xd < row->chipLeft + ChipWidth(d, d->items[idx]) &&
                yd + scrollY < row->top + LineH(d) && !dbl)
            {
                EditReminder(idx);
                return;
            }
            if (d->checkboxes && xd >= PadX && xd < PadX + CheckColW(d) && !dbl)
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
    const float padX = 16, padT = 12, padB = 12, maxW = 360, maxH = 460;
    IDWriteTextFormat* ft = MakeFormat(ts, 700, false);
    IDWriteTextFormat* fb = MakeFormat(fs, d->fontWeight, true);
    IDWriteTextLayout* lt = MakeLayout(d->title, ft, maxW - 2 * padX, 0);
    float contentW = Min(LayoutWidth(lt), maxW - 2 * padX);
    float markW = d->checkboxes ? CheckColW(d) : R0(fs * 0.9f);
    float gap = R0(fs * 0.35f);
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
        const NoteItem& it = d->items[i];
        float chip = ChipWidth(d, it);
        float avail = maxW - 2 * padX - markW - (chip > 0 ? chip + 8 : 0);
        IDWriteTextLayout* l = ItemLayout(d, it, fb, avail, nullptr, false);
        contentW = Max(contentW, Min(LayoutWidth(l), avail) + markW + (chip > 0 ? chip + 8 : 0));
        ls.Push(l);
        h += Max(lineH, LayoutHeight(l)) + gap;
    }
    float W = R0(contentW + 2 * padX), H = R0(Min(h - gap + padB, maxH));
    Canvas c;
    if (c.Begin((int)(W * S + 0.99f), (int)(H * S + 0.99f), 96 * S))
    {
        ID2D1RenderTarget* rt = c.rt;
        ID2D1SolidColorBrush *bg = nullptr, *fg = nullptr, *dim = nullptr;
        rt->CreateSolidColorBrush(Gfx::Color(d->bgColor, (float)Max(d->opacity, 0.94)), &bg);
        rt->CreateSolidColorBrush(Gfx::Color(d->textColor, 1), &fg);
        rt->CreateSolidColorBrush(Gfx::Color(d->textColor, 0.55f), &dim);
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), 8, 8), bg);
        Ui::Stroke(rt, D2D1::RectF(0, 0, W, H), 8, Argb(d->textColor, 0.16f), 1);
        rt->PushAxisAlignedClip(D2D1::RectF(0, 0, W, H - padB / 2), D2D1_ANTIALIAS_MODE_ALIASED);
        float y = padT;
        rt->DrawTextLayout(D2D1::Point2F(padX, y), lt, fg, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        y += LayoutHeight(lt) + 8;
        for (int i = 0; i < ls.n; i++)
        {
            if (d->items.n == 0) { rt->DrawTextLayout(D2D1::Point2F(padX, y), ls[i], dim); break; }
            const NoteItem& it = d->items[i];
            bool done = d->checkboxes && it.done;
            if (d->checkboxes)
            {
                float size = CheckSize(d);
                DrawCheck(rt, d, padX, y + (lineH - size) / 2, size, it.done, false, 1);
            }
            else Ui::Circle(rt, padX + 4, y + lineH / 2, 2.5f, Argb(d->textColor, 0.55f));
            ID2D1SolidColorBrush* tb = nullptr;
            rt->CreateSolidColorBrush(Gfx::Color(d->textColor, done ? 0.45f : 1.f), &tb);
            rt->DrawTextLayout(D2D1::Point2F(padX + markW, y), ls[i], tb, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            SafeRelease(tb);
            if (it.hasRemind) DrawChip(rt, d, it, W - padX - ChipWidth(d, it), y + (lineH - ChipH(d)) / 2, done ? 0.5f : 1.f);
            y += Max(lineH, LayoutHeight(ls[i])) + gap;
        }
        rt->PopAxisAlignedClip();
        SafeRelease(bg); SafeRelease(fg); SafeRelease(dim);
    }
    // 往屏幕里面的方向弹出
    RECT r;
    GetWindowRect(hwnd, &r);
    int pw = c.pw, ph = c.ph, off = (int)(6 * S), x, y;
    if (dockR) { x = (capHidden ? dockedX : r.left) - pw - off; y = r.top; }
    else if (dockL) { x = (capHidden ? dockedX + (r.right - r.left) : r.right) + off; y = r.top; }
    else if (dockB) { x = r.left; y = r.top - ph - off; }
    else { x = r.left; y = r.bottom + off; }
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
    CmdFold, CmdChecks, CmdModeDesktop, CmdModeTopmost, CmdModeNormal, CmdShadow, CmdOpacity, CmdFontSize,
    CmdNewNote, CmdDeleteNote, CmdTrash, CmdSettings, CmdExit,
    CmdWeight = 100, CmdTextColor = 200, CmdTextColorCustom = 220, CmdBgColor = 300, CmdBgColorCustom = 320
};
static const u32 TextColors[] = { DefaultTextColor, 0xF5F5F5, 0x222222, 0xF2C94C, 0xF08A3C, 0xE55B5B, 0x6CC27A, 0x5BA8E5 };
static const u32 BgColors[] = { DefaultBgColor, 0x1E1E1E, 0xFAFAFA, 0xFFF1A8, 0xD4F0D0, 0xD2E6F8, 0xF9D9E3, 0x233B33 };
static const int Weights[] = { 300, 400, 600, 700 };
static const wchar_t* const WeightNames[] = { L"细", L"常规", L"中粗", L"粗" };

static int IndexOfColor(const u32* list, u32 c)
{
    for (int i = 0; i < 8; i++) if (list[i] == c) return i;
    return -1;
}

// 菜单里拖滑块、点色块：直接改这张便签并重画，菜单不关
static void NoteMenuLive(void* ctx, int id, float v) { ((NoteWin*)ctx)->OnMenuLive(id, v); }

void NoteWin::OnMenuLive(int id, float v)
{
    if (id == CmdOpacity) d->opacity = v / 100.0;
    else if (id == CmdFontSize) d->fontSize = v;
    else if (id == CmdShadow) d->shadow = v > 0;
    else if (id >= CmdWeight && id < CmdWeight + 4) d->fontWeight = Weights[id - CmdWeight];
    else if (id >= CmdTextColor && id < CmdTextColor + 8) d->textColor = TextColors[id - CmdTextColor];
    else if (id >= CmdBgColor && id < CmdBgColor + 8) d->bgColor = BgColors[id - CmdBgColor];
    else return;
    menuChanged = true;
    Render();
}

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
    Ui::Menu m;
    // 右键点在条目上：先列针对这一条的操作，标题行显示是哪一条
    if (onItem)
    {
        const NoteItem& it = d->items[target];
        Str t = it.text.Replace(L"\r", L"").Replace(L"\n", L" ").Trim();
        m.Header(t.n ? t : Str(L"（空白条目）"));
        m.Item(CmdRemind, it.hasRemind ? L"修改提醒…" : L"设置提醒…", 0xEA8F);
        if (it.hasRemind) m.Item(CmdUnremind, L"取消提醒", 0xE7ED);
        if (target > 0) m.Item(CmdUp, L"上移", 0xE74A);
        if (target < d->items.n - 1) m.Item(CmdDown, L"下移", 0xE74B);
        m.Item(CmdDelete, L"删除这一条", 0xE74D);
        m.Sep();
    }
    m.Item(CmdAdd, L"新增一条", 0xE710);
    bool anyDone = false;
    for (int i = 0; i < d->items.n; i++) if (d->items[i].done) anyDone = true;
    if (d->checkboxes && anyDone) m.Item(CmdClearDone, L"清除已完成", 0xE9D5);
    if (undo.n || redo.n)
    {
        m.Item(CmdUndo, L"撤销", 0xE7A7, L"Ctrl+Z").disabled = !undo.n;
        m.Item(CmdRedo, L"重做", 0xE7A6, L"Ctrl+Y").disabled = !redo.n;
    }
    m.Sep();
    Ui::Menu* look = m.Sub(L"外观", 0xE790);
    look->Header(L"背景");
    look->Swatches(CmdBgColor, BgColors, 8, IndexOfColor(BgColors, d->bgColor), CmdBgColorCustom, d->bgColor);
    look->Header(L"文字");
    look->Swatches(CmdTextColor, TextColors, 8, IndexOfColor(TextColors, d->textColor), CmdTextColorCustom, d->textColor);
    look->Sep();
    look->Slider(CmdOpacity, L"不透明度", 0, 100, (float)(d->opacity * 100), 5, L"%");
    look->Slider(CmdFontSize, L"字号", 10, 40, (float)d->fontSize, 1, L"");
    int wsel = -1;
    for (int i = 0; i < 4; i++) if (d->fontWeight == Weights[i]) wsel = i;
    look->Segment(CmdWeight, L"粗细", WeightNames, 4, wsel);
    look->Sep();
    look->Check(CmdShadow, L"文字阴影", d->shadow, L"背景很透明时，文字更清楚").live = true;
    Ui::Menu* modes = m.Sub(L"窗口模式", 0xE718);
    modes->Radio(CmdModeDesktop, L"置底", L"贴在桌面上，显示桌面时也在", d->mode == ModeDesktop);
    modes->Radio(CmdModeTopmost, L"置顶", L"总在其他窗口前面", d->mode == ModeTopmost);
    modes->Radio(CmdModeNormal, L"普通窗口", L"和其他程序一样，出现在任务栏", d->mode == ModeNormal);
    m.Check(CmdChecks, L"显示勾选框", d->checkboxes);
    m.Item(CmdFold, d->collapsed ? L"展开" : L"折叠成胶囊", d->collapsed ? 0xE740 : 0xE73F);
    m.Sep();
    m.Item(CmdNewNote, L"新建便签", 0xE70B);
    m.Item(CmdDeleteNote, L"删除这张便签…", 0xE74D);
    m.Sep();
    m.Item(CmdTrash, L"回收站", 0xE7B8);
    m.Item(CmdSettings, L"设置", 0xE713);
    m.Item(CmdExit, L"退出 DeskNote", 0xE7E8);

    // 菜单里实时调的外观算一步撤销
    BeginChange();
    menuChanged = false;
    int cmd = Ui::MenuRun(hwnd, &m, sx, sy, NoteMenuLive, this);
    if (menuChanged) Save();
    EndChange();
    if (menuChanged) SendToBottom();
    PostMessageW(hwnd, WM_NULL, 0, 0);
    if (cmd) RunCommand(cmd, target);
    if (hwnd && d->collapsed) ScheduleCapsuleHide();
}

void NoteWin::DebugMenu(int sub)
{
    RECT r;
    GetWindowRect(hwnd, &r);
    float S = Scale();
    int y = r.top + (int)((rows.n ? rows[0].top - scrollY + 6 : 40) * S);
    Ui::MenuDebugOpenSub = sub;
    Ui::MenuDebug = true;
    OnContextMenu(r.left + (int)(60 * S), y);
}

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
        else if (cmd >= CmdTextColor && cmd < CmdTextColor + 8) d->textColor = TextColors[cmd - CmdTextColor];
        else if (cmd >= CmdBgColor && cmd < CmdBgColor + 8) d->bgColor = BgColors[cmd - CmdBgColor];
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
    int r = ReminderDialog(hwnd, cur.text, cur.hasRemind, cur.remind, &t);
    if (!r) { SendToBottom(); return; }
    if (index >= d->items.n || d->items[index].text != cur.text) return; // 对话框开着时条目被别处改了
    BeginChange();
    d->items[index].hasRemind = r == 1;
    if (r == 1) d->items[index].remind = t;
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
                float xd = p.x / Scale(), yd = p.y / Scale();
                int idx = HitRow(yd);
                bool row = idx >= 0 && idx < d->items.n;
                const RowInfo* ri = nullptr;
                for (int i = 0; row && i < rows.n; i++) if (rows[i].index == idx) ri = &rows[i];
                if (row && xd < PadX) cur = IDC_SIZEALL;
                else if (row && d->checkboxes && xd >= PadX && xd < PadX + CheckColW(d)) cur = IDC_HAND;
                else if (ri && ri->chipLeft > 0 && xd >= ri->chipLeft && yd + scrollY < ri->top + LineH(d)) cur = IDC_HAND;
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
    {
        if (dragIndex >= 0) { OnReorderMove(GET_Y_LPARAM(l) / Scale()); return 0; }
        bool entered = false;
        if (!hover)
        {
            hover = true;
            entered = true;
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&t);
            if (d->collapsed)
            {
                KillTimer(hwnd, TimerHide);
                if (capHidden) { capHidden = false; SlideTo(dockedX, dockedY); }
                ShowPreview();
                return 0;
            }
        }
        if (!d->collapsed)
        {
            // 鼠标所在的行、按钮变了才重画
            float xd = GET_X_LPARAM(l) / Scale(), yd = GET_Y_LPARAM(l) / Scale();
            int row = HitRow(yd);
            if (row >= d->items.n || HitEdge(GET_X_LPARAM(l), GET_Y_LPARAM(l))) row = -1;
            int b = HitToolbar(xd, yd);
            if (row != hoverRow || b != hoverBtn || entered) { hoverRow = row; hoverBtn = b; Render(); }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        hover = false;
        hoverRow = -1;
        hoverBtn = 0;
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
