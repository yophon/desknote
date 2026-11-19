// 一张便签的窗口：绘制、布局、鼠标键盘、编辑、排序、撤销、胶囊、窗口模式、右键菜单
#include "ui.h"

// =====================================================================
// 便签正文的排版（gfx.h 的 FontNote；固定行高时基线在 0.78 处）
// =====================================================================
struct NoteFont { float size; int weight; bool wrap; };
static NoteFont MakeFormat(float size, int weight, bool wrap) { NoteFont f = { size, weight, wrap }; return f; }

static TextLayout* MakeLayout(const Str& text, NoteFont nf, float maxW, float lineH)
{
    TextFmt f;
    f.font = FontNote;
    f.size = nf.size;
    f.weight = nf.weight;
    f.wrap = nf.wrap;
    f.lineH = lineH;
    f.baseline = 0.78f;
    return Gfx::Layout(text, f, maxW);
}

static float LayoutHeight(TextLayout* l) { return Gfx::Height(l); }
static float LayoutWidth(TextLayout* l) { return Gfx::Width(l); }

static float R1(float v) { return (float)(int)(v * 10 + 0.5f) / 10; }
static float R0(float v) { return (float)(int)(v + 0.5f); }

// 尺寸（单位 DIP，大多跟着字号走）
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
static TextLayout* ItemLayout(const NoteData* d, const NoteItem& it, NoteFont f, float width, bool withTag)
{
    int tag = -1;
    Str s = withTag ? ItemDisplay(it, &tag) : it.text;
    TextLayout* l = MakeLayout(s, f, width, LineH(d));
    if (!l) return nullptr;
    Vec<LinkSpan> links;
    FindLinks(it.text, links);
    for (int i = 0; i < links.n; i++) Gfx::SetUnderline(l, links[i].start, links[i].len);
    if (d->checkboxes && it.done) Gfx::SetStrike(l, 0, it.text.n);
    if (tag >= 0) Gfx::SetSize(l, tag, s.n - tag, R1((float)d->fontSize * 0.85f));
    return l;
}

// 文字（可选阴影）
static const u32 TextShadow = 0xBF000000; // 黑色，不透明度 0.75
static void DrawLayout(Painter* rt, float x, float y, TextLayout* l, u32 color, bool shadow)
{
    if (shadow)
    {
        Gfx::Draw(rt, l, x + 1, y + 1, TextShadow);
        Gfx::Draw(rt, l, x + 0.5f, y + 0.5f, TextShadow);
    }
    Gfx::Draw(rt, l, x, y, color);
}

// =====================================================================
// 创建、销毁
// =====================================================================
enum { TimerHide = 1, TimerSlide = 2, TimerCaret = 4, TimerFlash = 5 };

// 胶囊的悬停预览：单独一个弹出窗口
struct NotePreview : WinHandler
{
    NoteWin* n;
    explicit NotePreview(NoteWin* note) : n(note) {}
    void Paint(Window*, Painter*, float, float) override {}
    int CursorAt(Window*, float, float) override { return CursorHand; }
    bool OnEvent(Window*, const Event& e) override { return n->OnPreviewEvent(e); }
};

bool NoteWin::debugNoFocus = false;
NoteWin::NoteWin(NoteData* data) : d(data) {}
NoteWin::~NoteWin()
{
    delete d;
    delete pendingReload;
    for (int i = 0; i < undo.n; i++) delete undo[i];
    for (int i = 0; i < redo.n; i++) delete redo[i];
    delete pending;
    delete previewHandler;
}


float NoteWin::Scale() const { return win ? Wnd::Scale(win) : 1.f; }

void NoteWin::Create()
{
    win = Wnd::NewNoteWindow(this, d->title, 80, 80);
    if (!win) return;
    RestorePosition();
    ApplyMode();
    if (d->collapsed) EnterCollapsed();
    else Render();
    Wnd::ShowNoActivate(win);
    if (d->collapsed) { SnapCapsule(); ScheduleCapsuleHide(); }
}

void NoteWin::Destroy()
{
    CommitEdit(true);
    ClosePreview();
    if (win)
    {
        SaveLayout();
        Window* w = win;
        win = nullptr;
        Wnd::Destroy(w);
    }
}

// =====================================================================
// 绘制
// =====================================================================

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
static void DrawChip(Painter* rt, const NoteData* d, const NoteItem& it, float x, float y, float alpha)
{
    float f = ChipFont(d), h = ChipH(d), w = ChipWidth(d, it);
    bool due = IsDue(it);
    u32 c = due ? DueColor : d->textColor;
    Ui::Fill(rt, RectF(x, y, x + w, y + h), h / 2, Argb(c, (due ? 0.22f : 0.12f) * alpha));
    Ui::Icon(rt, 0xEA8F, x + 8 + f / 2, y + h / 2, f, Argb(c, (due ? 1.f : 0.8f) * alpha));
    Ui::Text(rt, FormatWhen(it.remind), RectF(x + 8 + f + 4, y, x + w - 4, y + h), f, due ? 600 : 400, Argb(c, (due ? 1.f : 0.8f) * alpha));
}

// 勾选框：圆角方框；勾上后实心，中间一个对勾
static void DrawCheck(Painter* rt, const NoteData* d, float x, float y, float size, bool done, bool hot, float alpha)
{
    float r = R1(size * 0.26f);
    float bw = Max(1.25f, R1((float)d->fontSize / 11));
    RectF b = RectF(x, y, x + size, y + size);
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
    if (!win) return;
    Wnd::SetTitle(win, d->title);
    if (d->collapsed)
    {
        // ---- 胶囊：标题 + 未完成条数的小圆标 ----
        float S = Scale();
        float size = Min((float)d->fontSize, 15.f);
        NoteFont ft = MakeFormat(size, 600, false);
        int undone = 0;
        for (int i = 0; i < d->items.n; i++) if (!d->checkboxes || !d->items[i].done) undone++;
        Str count;
        count.Int(undone);
        TextLayout* lt = MakeLayout(d->title, ft, 220, 0);
        float tw = Min(LayoutWidth(lt), 220.f);
        float th = LayoutHeight(lt);
        Gfx::SetMaxWidth(lt, tw + 1);
        float cf = R1(size * 0.8f);
        float badgeH = R0(cf * 1.55f), badgeW = Max(badgeH, R0(Ui::Measure(count, cf, 600) + 12));
        float H = R0(Max(th, badgeH) + 12), W = R0(14 + tw + 8 + badgeW + 8);
        int cpw = (int)(W * S + 0.99f), cph = (int)(H * S + 0.99f);
        if (Painter* crt = Wnd::BeginPaint(win, cpw, cph))
        {
            W = cpw / S; H = cph / S;
            const float r = H / 2;
            bool docked = dockL || dockR || dockT || dockB;
            Ui::FillCorners(crt, RectF(0, 0, W, H), dockL || dockT ? 0 : r, dockR || dockT ? 0 : r,
                            dockR || dockB ? 0 : r, dockL || dockB ? 0 : r, Argb(d->bgColor, (float)Max(d->opacity, 0.8)));
            if (!docked) Ui::Stroke(crt, RectF(0, 0, W, H), r, Argb(d->textColor, 0.14f), 1);
            if (flashUntil && (TickMs() / 200) % 2 == 0) Ui::Stroke(crt, RectF(0, 0, W, H), r, Ui::T.accent, 3);
            Gfx::GrayscaleText(crt);
            Gfx::Draw(crt, lt, 14, (H - th) / 2, Argb(d->textColor, 1), true);
            RectF br = RectF(W - 8 - badgeW, (H - badgeH) / 2, W - 8, (H + badgeH) / 2);
            Ui::Fill(crt, br, badgeH / 2, Argb(d->textColor, 0.16f));
            Ui::Text(crt, count, br, cf, 600, Argb(d->textColor, 0.9f), Ui::Center);
        }
        Wnd::EndPaint(win);
        Gfx::Free(lt);
        if (preview && Wnd::IsVisible(preview)) RenderPreview();
        return;
    }
    float S = Scale();
    int pw = (int)(d->width * S + 0.5), ph = (int)(d->height * S + 0.5);
    if (Painter* rt = Wnd::BeginPaint(win, pw, ph))
    {
        float W = pw / S, H = ph / S;
        Gfx::GrayscaleText(rt);
        const u32 fg = Argb(d->textColor, 1), dim = Argb(d->textColor, 0.55f);
        const float radius = 8;
        Ui::Fill(rt, RectF(0, 0, W, H), radius, Argb(d->bgColor, (float)Max(d->opacity, 1.0 / 255)));
        // 细描边：背景很透明时只在鼠标移上来时出现，平时只剩文字
        if (d->opacity >= 0.12 || hover) Ui::Stroke(rt, RectF(0, 0, W, H), radius, Argb(d->textColor, hover ? 0.18f : 0.1f), 1);
        if (flashUntil && (TickMs() / 200) % 2 == 0) Ui::Stroke(rt, RectF(0, 0, W, H), radius, Ui::T.accent, 3);

        bool showUi = hover && editIndex == NotEditing && dragIndex < 0 && !g_app->peeking;
        float left = PadX, right = W - PadX, top = PadTop, bottom = H - PadBottom;
        Gfx::PushClip(rt, RectF(0, top - 6, W, bottom));
        float lineH = LineH(d), fs = (float)d->fontSize;
        float y = top - scrollY;

        // ---- 标题行：标题 + 悬停时出现的三个按钮 ----
        const float btn = 28, btnGap = 2, tbRight = W - 10;
        float ts = TitleSize(d);
        NoteFont ft = MakeFormat(ts, 700, false);
        float titleRight = showUi ? tbRight - 3 * btn - 2 * btnGap - 8 : right;
        TextLayout* lt = MakeLayout(d->title, ft, titleRight - left, 0);
        float titleH = Max(LayoutHeight(lt), btn);
        if (editIndex == TitleIndex)
        {
            titleH = Max(btn, EditMeasure(right - left, 0));
            Ui::Fill(rt, RectF(left - 8, y - 2, right + 8, y + titleH + 2), 6, Argb(d->textColor, 0.08f));
            DrawEditor(rt, left, y, right - left, titleH, true);
        }
        else DrawLayout(rt, left, y + (titleH - LayoutHeight(lt)) / 2, lt, fg, d->shadow);
        float tcy = y + titleH / 2;
        static const wchar_t glyphs[3] = { 0xE710, 0xE712, 0xE73F };
        for (int k = 0; k < 3; k++)
        {
            float bx = tbRight - (3 - k) * btn - (2 - k) * btnGap;
            toolBtn[k] = RectF(bx, tcy - btn / 2 + scrollY, bx + btn, tcy + btn / 2 + scrollY);
            if (!showUi) continue;
            RectF r = RectF(bx, tcy - btn / 2, bx + btn, tcy + btn / 2);
            if (hoverBtn == k + 1) Ui::Fill(rt, r, 5, Argb(d->textColor, 0.14f));
            Ui::Icon(rt, glyphs[k], bx + btn / 2, tcy, 14, Argb(d->textColor, hoverBtn == k + 1 ? 1.f : 0.75f));
        }
        Gfx::Free(lt);
        y += titleH + TitleGap;
        titleBottom = y + scrollY;

        NoteFont fb = MakeFormat(fs, d->fontWeight, true);
        rows.Clear();
        if (d->items.n == 0 && editIndex != 0)
        {
            TextLayout* lh = MakeLayout(Str(L"双击新增，右键设置"), fb, right - left, lineH);
            DrawLayout(rt, left, y, lh, dim, false);
            y += LayoutHeight(lh);
            Gfx::Free(lh);
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
            TextLayout* l = nullptr;
            float h;
            if (editingRow) h = EditMeasure(avail, lineH);
            else
            {
                l = ItemLayout(d, *it, fb, ItemTextWidth(d, *it, avail), false);
                h = Max(lineH, LayoutHeight(l));
            }
            // 悬停 / 拖动中的行：淡色底 + 左边距里的拖动手柄
            bool rowHot = real && !editingRow && (i == dragIndex || (showUi && hoverRow == i));
            if (rowHot)
            {
                Ui::Fill(rt, RectF(6, y - gap / 2, W - 6, y + h + gap / 2), 6, Argb(d->textColor, i == dragIndex ? 0.16f : 0.07f));
                float cx = PadX / 2 + 2, cy = y + lineH / 2;
                for (int r = -1; r <= 1; r++)
                    for (int k = 0; k < 2; k++)
                        Ui::Circle(rt, cx - 2.5f + k * 5, cy + r * 5, 1.3f, Argb(d->textColor, 0.6f));
            }
            if (editingRow) Ui::Fill(rt, RectF(6, y - gap / 2, W - 6, y + h + gap / 2), 6, Argb(d->textColor, 0.08f));
            if (d->checkboxes)
            {
                float size = CheckSize(d);
                DrawCheck(rt, d, left, y + (lineH - size) / 2, size, real && it->done && !editingRow, rowHot, rowAlpha);
            }
            if (editingRow) DrawEditor(rt, textX, y, avail, h, false);
            float chipLeft = 0;
            if (l)
            {
                DrawLayout(rt, textX, y, l, Argb(d->textColor, (dimmed ? 0.45f : 1.f) * rowAlpha), d->shadow && !dimmed);
                Gfx::Free(l);
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
        contentHeight = y + scrollY - top;
        Gfx::PopClip(rt);
        // 内容超出时：右边一条细滚动条（鼠标移上来才显示）
        float viewH = bottom - top;
        if (hover && contentHeight > viewH + 1)
        {
            float th = Max(20.f, viewH * viewH / contentHeight);
            float ty = top + (viewH - th) * Clamp(scrollY / (contentHeight - viewH), 0.f, 1.f);
            Ui::Fill(rt, RectF(W - 5, ty, W - 2, ty + th), 1.5f, Argb(d->textColor, 0.35f));
        }
    }
    Wnd::EndPaint(win);
}

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
int NoteWin::HitEdge(float x, float y) const
{
    if (d->collapsed) return 0;
    float W, H;
    Wnd::ClientSize(win, &W, &H);
    const float e = 8;
    bool l = x < e, r = x > W - e, t = y < e, b = y > H - e;
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
    if (keepWindow) { fresh->mode = d->mode; fresh->collapsed = d->collapsed; fresh->hidden = d->hidden; }
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
    if (fresh->hidden != d->hidden) SetHidden(fresh->hidden);
    if (!win) { d->mode = fresh->mode; d->collapsed = fresh->collapsed; } // 隐藏着：只记下来，显示时按它建窗口
    else
    {
        if (fresh->mode != d->mode) SetMode(fresh->mode);
        if (fresh->collapsed != wasCollapsed) { if (fresh->collapsed) Collapse(); else Expand(); }
    }
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
    Wnd::Capture(win, true);
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
    float H;
    Wnd::ClientSize(win, nullptr, &H);
    float maxScroll = Max(0.f, contentHeight - (H - PadTop - PadBottom));
    if (yDip < PadTop + 20 && scrollY > 0) { scrollY = Max(0.f, scrollY - LineH(d)); Render(); }
    else if (yDip > H - PadBottom - 20 && scrollY < maxScroll) { scrollY = Min(maxScroll, scrollY + LineH(d)); Render(); }
}

void NoteWin::EndReorder()
{
    if (dragIndex < 0) return;
    dragIndex = -1;
    Wnd::Capture(win, false);
    if (dragChanged) Save();
    EndChange();
    Render();
    SendToBottom();
}

// =====================================================================
// 编辑：直接在便签里画正在编辑的文字（和显示时同一套排版、彩色 emoji），
// 光标、选区、键盘、剪贴板、输入法都自己处理
// =====================================================================
static bool IsSpaceCh(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == 0x3000; }
// 按词移动用的字符分类：空白、字母数字、汉字、代理对（emoji 等）、标点
static int CharClass(wchar_t c)
{
    if (IsSpaceCh(c)) return 0;
    if ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'_' || (c >= 0xC0 && c < 0x2000)) return 1;
    if ((c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF)) return 2;
    if (c >= 0xD800 && c <= 0xDFFF) return 3;
    return 4;
}

void NoteWin::BeginEdit(int index)
{
    CommitEdit(true);
    if (d->collapsed) Expand();
    editIndex = index;
    editOrig = index == TitleIndex ? d->title : index >= 0 && index < d->items.n ? d->items[index].text : Str();
    editText = editOrig;
    caret = anchor = editText.n;
    compText = Str();
    compCursor = 0;
    editGoalX = -1;
    editUndo.Clear();
    editRedo.Clear();
    editLastKind = 0;
    editSelecting = false;
    if (!debugNoFocus) Wnd::Activate(win);
    // 系统光标不显示，只是让输入法、读屏软件知道光标在哪
    Wnd::ImeAttach(win, LineH(d));
    EditUpdated();
}

// 正在编辑的文字的排版：输入法组字插在光标处；末尾是换行时补一个零宽字符，光标才能落到新的一行
TextLayout* NoteWin::EditLayout(int* dispCaret)
{
    bool title = editIndex == TitleIndex;
    Str s = editText;
    int c = caret;
    if (compText.n) { s = editText.Sub(0, caret) + compText + editText.Sub(caret); c = caret + compCursor; }
    if (s.n && s.p[s.n - 1] == L'\n') s.Ch(0x200B);
    NoteFont f = title ? MakeFormat(TitleSize(d), 700, true) : MakeFormat((float)d->fontSize, d->fontWeight, true);
    TextLayout* l = MakeLayout(s, f, editW, title ? 0 : LineH(d));
    if (l && compText.n) Gfx::SetUnderline(l, caret, compText.n);
    if (dispCaret) *dispCaret = c;
    return l;
}

float NoteWin::EditMeasure(float w, float minH)
{
    editW = w;
    TextLayout* l = EditLayout(nullptr);
    if (!l) return minH;
    float h = Max(minH, LayoutHeight(l));
    Gfx::Free(l);
    return h;
}

// 画编辑中的文字：选区底色、文字、组字下划线、光标。(x, y) 是窗口坐标（已减去滚动）
void NoteWin::DrawEditor(Painter* rt, float x, float y, float w, float h, bool centerV)
{
    editW = w;
    int dc = 0;
    TextLayout* l = EditLayout(&dc);
    if (!l) return;
    float lh = LayoutHeight(l);
    float ty = centerV ? y + Max(0.f, (h - lh) / 2) : y;
    editBox = RectF(x, ty + scrollY, x + w, ty + scrollY + Max(lh, h - (ty - y)));
    // 选区
    int a = Min(caret, anchor), b = Max(caret, anchor);
    if (b > a && !compText.n)
    {
        Vec<RectF> ms;
        Gfx::RangeRects(l, a, b - a, ms);
        for (int i = 0; i < ms.n; i++)
            Ui::Fill(rt, RectF(x + ms[i].left, ty + ms[i].top, x + ms[i].left + Max(ms[i].W(), 4.f), ty + ms[i].bottom), 0,
                     Argb(d->textColor, 0.3f));
    }
    // 空的新条目：灰色提示
    if (!editText.n && !compText.n && editIndex != TitleIndex)
    {
        NoteFont f = MakeFormat((float)d->fontSize, d->fontWeight, true);
        TextLayout* ph = MakeLayout(Str(L"输入内容，Enter 保存，Esc 取消"), f, w, LineH(d));
        if (ph) Gfx::Draw(rt, ph, x, ty, Argb(d->textColor, 0.35f));
        Gfx::Free(ph);
    }
    Gfx::Draw(rt, l, x, ty, Argb(d->textColor, 1));
    // 光标位置（输入法候选框、滚动都要用，所以总是算；有选区时不画）
    Gfx::CaretPos cp = Gfx::Caret(l, dc);
    float px = cp.x, py = cp.y;
    float ch = cp.h > 0 ? cp.h : (editIndex == TitleIndex ? TitleSize(d) * 1.3f : LineH(d));
    caretBox = RectF(x + px, ty + py + scrollY, x + px + 1.5f, ty + py + ch + scrollY);
    if (caretOn && (caret == anchor || compText.n))
        Ui::Fill(rt, RectF(x + px, ty + py, x + px + Max(1.5f, R1(1.5f)), ty + py + ch), 0, Argb(d->textColor, 1));
    Gfx::Free(l);
}

// 鼠标位置（窗口 DIP）-> 文字里的位置
int NoteWin::EditHit(float xd, float yd)
{
    TextLayout* l = EditLayout(nullptr);
    if (!l) return caret;
    int pos = Gfx::HitPoint(l, xd - editBox.left, yd + scrollY - editBox.top);
    Gfx::Free(l);
    return Clamp(pos, 0, editText.n);
}

bool NoteWin::InEditBox(float xd, float yd) const
{
    float y = yd + scrollY;
    return Editing() && xd >= editBox.left - 6 && xd < editBox.right + 6 && y >= editBox.top - 4 && y < editBox.bottom + 4;
}

// 光标能停的位置：按字形簇走（emoji、组合字符算一个）；word = 按词
int NoteWin::EditStep(int pos, int dir, bool word)
{
    const wchar_t* t = editText.p;
    int n = editText.n;
    if (word)
    {
        int p = pos;
        if (dir < 0)
        {
            while (p > 0 && IsSpaceCh(t[p - 1])) p--;
            if (p > 0) { int c = CharClass(t[p - 1]); while (p > 0 && CharClass(t[p - 1]) == c) p--; }
        }
        else
        {
            if (p < n) { int c = CharClass(t[p]); while (p < n && CharClass(t[p]) == c) p++; }
            while (p < n && IsSpaceCh(t[p])) p++;
        }
        return p;
    }
    Str saveComp = compText;
    compText = Str();
    TextLayout* l = EditLayout(nullptr);
    compText = saveComp;
    int result = Clamp(pos + dir, 0, n);
    if (l)
    {
        Vec<int> cm;
        Gfx::Clusters(l, cm);
        if (cm.n)
        {
            int p = 0, prev = 0;
            result = dir < 0 ? 0 : n;
            for (int i = 0; i < cm.n; i++)
            {
                int next = Min(p + cm[i], n);
                if (dir < 0 && next >= pos) { result = p < pos ? p : prev; break; }
                if (dir > 0 && p >= pos && next > pos) { result = next; break; }
                prev = p;
                p = next;
            }
        }
        Gfx::Free(l);
    }
    return Clamp(result, 0, n);
}

// what：0 行首，1 行尾，-1 上一行，+1 下一行（保持同一列）
int NoteWin::EditLineMove(int pos, int what)
{
    TextLayout* l = EditLayout(nullptr);
    if (!l) return pos;
    int n = editText.n, result = pos;
    Vec<Gfx::LineInfo> lm;
    Gfx::Lines(l, lm);
    int lc = lm.n;
    if (what == 0 || what == 1)
    {
        int start = 0;
        for (int i = 0; i < lc; i++)
        {
            int len = (int)lm[i].length;
            if (pos < start + len || i == lc - 1)
            {
                result = what == 0 ? start : start + len - (int)lm[i].newlineLength;
                // 自动折行处的行尾：退到行尾空格前，免得光标跑到下一行开头
                if (what == 1 && lm[i].newlineLength == 0 && i < lc - 1)
                    while (result > start && result <= n && IsSpaceCh(editText.p[result - 1])) result--;
                break;
            }
            start += len;
        }
    }
    else
    {
        Gfx::CaretPos cp = Gfx::Caret(l, Min(pos, n));
        if (editGoalX < 0) editGoalX = cp.x;
        float y = what < 0 ? cp.y - 1 : cp.y + cp.h + 1;
        if (y < 0) result = 0;
        else if (y >= LayoutHeight(l)) result = n;
        else result = Gfx::HitPoint(l, editGoalX, y);
    }
    Gfx::Free(l);
    return Clamp(result, 0, n);
}

void NoteWin::EditSetCaret(int pos, bool extend)
{
    caret = Clamp(pos, 0, editText.n);
    if (!extend) anchor = caret;
    editLastKind = 0;
    EditUpdated();
}

// 把 [a, b) 换成 s；kind：1 打字，2 删除（连续的同类操作合成一步撤销），0 其他
void NoteWin::EditReplace(int a, int b, const Str& s, int kind)
{
    Str ins = s.Replace(L"\r\n", L"\n").Replace(L"\r", L"\n");
    if (editIndex == TitleIndex) ins = ins.Replace(L"\n", L" ");
    if (!(kind && kind == editLastKind && a == editLastPos))
    {
        EditSnap snap = { editText, caret, anchor };
        editUndo.Push(snap);
        if (editUndo.n > 100) editUndo.RemoveAt(0);
    }
    editRedo.Clear();
    editText = editText.Sub(0, a) + ins + editText.Sub(b);
    caret = anchor = a + ins.n;
    editLastKind = kind;
    editLastPos = kind == 2 ? a : caret;
    EditUpdated();
}

void NoteWin::EditInsert(const Str& s, int kind)
{
    EditReplace(Min(caret, anchor), Max(caret, anchor), s, kind);
}

void NoteWin::EditUndoRedo(bool redoIt)
{
    Vec<EditSnap>& from = redoIt ? editRedo : editUndo;
    Vec<EditSnap>& to = redoIt ? editUndo : editRedo;
    if (!from.n) return;
    EditSnap cur = { editText, caret, anchor };
    to.Push(cur);
    EditSnap s = from.Last();
    from.RemoveAt(from.n - 1);
    editText = s.text;
    caret = s.caret;
    anchor = s.anchor;
    editLastKind = 0;
    EditUpdated();
}



void NoteWin::EditCopy(bool cut)
{
    int a = Min(caret, anchor), b = Max(caret, anchor);
    if (b <= a) return;
    Wnd::ClipboardSet(editText.Sub(a, b - a));
    if (cut) EditReplace(a, b, Str(), 0);
}

void NoteWin::EditPaste()
{
    Str s = Wnd::ClipboardText();
    if (s.n) EditInsert(s, 0);
}

void NoteWin::EditSelectWord(int pos)
{
    int n = editText.n;
    if (!n) return;
    int p = Clamp(pos, 0, n - 1);
    if (pos == n || (p > 0 && editText.p[p] == L'\n')) p = Max(0, pos - 1);
    int c = CharClass(editText.p[p]);
    int a = p, b = p + 1;
    while (a > 0 && CharClass(editText.p[a - 1]) == c && editText.p[a - 1] != L'\n') a--;
    while (b < n && CharClass(editText.p[b]) == c && editText.p[b] != L'\n') b++;
    anchor = a;
    caret = b;
    editLastKind = 0;
    EditUpdated();
}

// 键盘：返回 true 表示已处理
bool NoteWin::EditKey(int vk, int mods)
{
    if (compText.n) return false; // 输入法组字时按键归输入法
    bool shift = (mods & ModShift) != 0, ctrl = (mods & ModCtrl) != 0;
    int n = editText.n;
    bool hasSel = caret != anchor;
    switch (vk)
    {
    case KeyLeft: case KeyRight:
    {
        int dir = vk == KeyRight ? 1 : -1;
        editGoalX = -1;
        if (hasSel && !shift && !ctrl) EditSetCaret(dir < 0 ? Min(caret, anchor) : Max(caret, anchor), false);
        else EditSetCaret(EditStep(caret, dir, ctrl), shift);
        return true;
    }
    case KeyUp: case KeyDown:
    {
        float keep = editGoalX;
        int p = EditLineMove(caret, vk == KeyUp ? -1 : 1);
        float goal = editGoalX;
        EditSetCaret(p, shift);
        editGoalX = keep >= 0 ? keep : goal;
        return true;
    }
    case KeyHome: editGoalX = -1; EditSetCaret(ctrl ? 0 : EditLineMove(caret, 0), shift); return true;
    case KeyEnd: editGoalX = -1; EditSetCaret(ctrl ? n : EditLineMove(caret, 1), shift); return true;
    case KeyBack:
        if (hasSel) EditInsert(Str(), 0);
        else if (caret > 0) EditReplace(EditStep(caret, -1, ctrl), caret, Str(), 2);
        return true;
    case KeyDelete:
        if (shift && hasSel) { EditCopy(true); return true; }
        if (hasSel) EditInsert(Str(), 0);
        else if (caret < n) EditReplace(caret, EditStep(caret, 1, ctrl), Str(), 2);
        return true;
    case KeyInsert:
        if (ctrl) EditCopy(false);
        else if (shift) EditPaste();
        return true;
    case KeyEnter:
        if (shift)
        {
            if (editIndex != TitleIndex) EditInsert(Str(L"\n"), 0); // Shift+Enter：在这一条里换行
            return true;
        }
        {
            // 新增的条目按 Enter 后接着写下一条；空着按 Enter 就结束
            bool continueAdding = editIndex == d->items.n && editText.Trim().n > 0;
            CommitEdit(true);
            if (continueAdding) BeginEdit(d->items.n);
        }
        return true;
    case KeyEscape: CommitEdit(false); return true;
    case KeyTab: return true;
    }
    if (ctrl)
    {
        switch (vk)
        {
        case 'A': anchor = 0; caret = n; editLastKind = 0; EditUpdated(); return true;
        case 'C': EditCopy(false); return true;
        case 'X': EditCopy(true); return true;
        case 'V': EditPaste(); return true;
        case 'Z': EditUndoRedo(shift); return true;
        case 'Y': EditUndoRedo(true); return true;
        }
    }
    return false;
}

void NoteWin::EditChar(wchar_t c)
{
    if (c < 32 || c == 127) return; // 回车、退格、Ctrl+字母产生的控制字符在 EditKey 里处理
    EditInsert(Str(&c, 1), 1);
}

// 改完文字或挪了光标：光标重新亮起、重画、滚到看得见、告诉输入法光标在哪
void NoteWin::EditUpdated()
{
    if (!Editing()) return;
    caretOn = true;
    Wnd::SetTimer(win, TimerCaret, Wnd::CaretBlinkMs());
    Render();
    // 滚动到光标可见
    float H = (float)d->height;
    float top = PadTop, bottom = H - PadBottom;
    float ns = scrollY;
    if (caretBox.top - ns < top) ns = caretBox.top - top;
    if (caretBox.bottom - ns > bottom) ns = caretBox.bottom - bottom;
    ns = Max(0.f, ns);
    if (ns != scrollY) { scrollY = ns; Render(); }
    UpdateImePos();
}

void NoteWin::UpdateImePos()
{
    if (!Editing()) return;
    Wnd::ImeSetCaret(win, RectF(caretBox.left, caretBox.top - scrollY, caretBox.right, caretBox.bottom - scrollY));
}


// 输入法消息：组字直接画在光标处，候选框跟着光标；返回 true 表示已处理

// 结束输入法正在进行的组字（点鼠标、提交时）
void NoteWin::EditFinishIme(bool cancel)
{
    if (!compText.n) return;
    Wnd::ImeFinish(win, cancel);
    compText = Str();
}

void NoteWin::EditContextMenu(int sx, int sy)
{
    enum { CCut = 1, CCopy, CPaste, CAll };
    bool sel = caret != anchor;
    Ui::Menu m;
    m.Item(CCut, L"剪切", 0xE8C6, L"Ctrl+X").disabled = !sel;
    m.Item(CCopy, L"复制", 0xE8C8, L"Ctrl+C").disabled = !sel;
    m.Item(CPaste, L"粘贴", 0xE77F, L"Ctrl+V").disabled = !Wnd::ClipboardHasText();
    m.Sep();
    m.Item(CAll, L"全选", 0xE8B3, L"Ctrl+A");
    int cmd = Ui::MenuRun(Wnd::Native(win), &m, sx, sy, nullptr, nullptr);
    if (!Editing()) return;
    switch (cmd)
    {
    case CCut: EditCopy(true); break;
    case CCopy: EditCopy(false); break;
    case CPaste: EditPaste(); break;
    case CAll: anchor = 0; caret = editText.n; EditUpdated(); break;
    }
}

void NoteWin::CommitEdit(bool save)
{
    if (!Editing()) return;
    EditFinishIme(true);
    int idx = editIndex;
    // 条目内部换行统一存成 \n，去掉每行行尾空格（Tab 保留），首尾空行去掉
    Str norm = editText.Replace(L"\r\n", L"\n");
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
    Str text = out.Sub(a, b - a);
    if (idx == TitleIndex) text = text.Replace(L"\n", L" ").Trim();
    editIndex = NotEditing;
    editSelecting = false;
    Wnd::KillTimer(win, TimerCaret);
    Wnd::ImeDetach(win);
    editUndo.Clear();
    editRedo.Clear();
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
bool NoteWin::TryOpenLink(float xd, float yd)
{
    int idx = HitRow(yd);
    if (idx < 0 || idx >= d->items.n) return false;
    const RowInfo* row = nullptr;
    for (int i = 0; i < rows.n; i++) if (rows[i].index == idx) row = &rows[i];
    if (!row) return false;
    float cw;
    Wnd::ClientSize(win, &cw, nullptr);
    NoteFont fb = MakeFormat((float)d->fontSize, d->fontWeight, true);
    TextLayout* l = ItemLayout(d, d->items[idx], fb, ItemTextWidth(d, d->items[idx], cw - PadX - row->textLeft), false);
    bool inside = false;
    int at = 0;
    Gfx::HitPoint(l, xd - row->textLeft, yd + scrollY - row->top, &inside, &at);
    Gfx::Free(l);
    if (!inside) return false;
    Vec<LinkSpan> links;
    FindLinks(d->items[idx].text, links);
    for (int i = 0; i < links.n; i++)
        if (at >= links[i].start && at < links[i].start + links[i].len)
        {
            Wnd::OpenUrl(links[i].url);
            return true;
        }
    return false;
}

// =====================================================================
// 鼠标
// =====================================================================
void NoteWin::OnLButtonDown(float xd, float yd, bool dbl, int mods)
{
    if (d->collapsed)
    {
        // 胶囊：按住拖动；没拖动就是单击，展开
        ClosePreview();
        Wnd::KillTimer(win, TimerSlide);
        sliding = false;
        if (capHidden) { Wnd::Move(win, dockedX, dockedY); capHidden = false; }
        RectI before = Wnd::ScreenBounds(win);
        Wnd::DragMove(win);
        RectI after = Wnd::ScreenBounds(win);
        if (Abs(after.left - before.left) < 3 && Abs(after.top - before.top) < 3) Expand();
        else { SnapCapsule(); SaveLayout(); ScheduleCapsuleHide(); }
        SendToBottom();
        return;
    }
    int edge = HitEdge(xd, yd);
    if (edge)
    {
        Wnd::DragResize(win, edge);
        return;
    }
    // 正在编辑：点在编辑区里是挪光标（Shift 扩选、双击选词），点到别处先保存这一条
    if (Editing())
    {
        if (InEditBox(xd, yd))
        {
            EditFinishIme(false);
            int p = EditHit(xd, yd);
            if (dbl) EditSelectWord(p);
            else
            {
                editGoalX = -1;
                EditSetCaret(p, (mods & ModShift) != 0);
                editSelecting = true;
                Wnd::Capture(win, true);
            }
            return;
        }
        CommitEdit(true);
        return;
    }
    Wnd::Focus(win);
    if ((mods & ModCtrl) && TryOpenLink(xd, yd)) return;
    // 标题栏按钮：新增、菜单、折叠
    switch (HitToolbar(xd, yd))
    {
    case 1: BeginEdit(d->items.n); return;
    case 2:
    {
        PointI p = Wnd::ToScreen(win, toolBtn[1].left, toolBtn[1].bottom - scrollY + 4);
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
    Wnd::DragMove(win);
    SaveLayout();
    SendToBottom();
}

// =====================================================================
// 窗口模式：置底在桌面 / 置顶 / 普通窗口；快捷键临时提到最前
// =====================================================================
void NoteWin::ApplyMode()
{
    if (!win) return;
    Wnd::SetLevel(win, d->mode == ModeDesktop ? Wnd::LevelDesktop : d->mode == ModeNormal ? Wnd::LevelNormal : Wnd::LevelTopmost, peeking);
}

void NoteWin::SetMode(WinMode m)
{
    if (m == d->mode) return;
    d->mode = m;
    if (!win) return; // 隐藏着：显示时按它建窗口
    // 任务栏按钮要在窗口重新显示时才会增减
    Wnd::Hide(win);
    ApplyMode();
    Wnd::ShowNoActivate(win);
}

// 隐藏：销毁窗口（绘图资源一起释放，不占内存），便签对象和数据都留着，提醒、同步照常
void NoteWin::SetHidden(bool hide)
{
    tempShown = false;
    d->hidden = hide;
    if (hide) { if (win) { peeking = false; Destroy(); } }
    else if (!win) Create();
    Save();
}

// 隐藏的便签到了提醒：临时建出窗口，结束「提到最前」时收回去（不改隐藏状态）
void NoteWin::ShowTemporarily()
{
    if (win) return;
    tempShown = true;
    Create();
}

void NoteWin::Locate()
{
    if (!win) return;
    Ui::LoadTheme();
    BringOnScreen();
    BeginPeek(); // 置顶，闪完放回去
    flashUntil = TickMs() + 1600;
    Wnd::SetTimer(win, TimerFlash, 200);
    Render();
}


void NoteWin::SendToBottom()
{
    if (win && KeepAtBottom(win)) Wnd::ToBottom(win);
}

// 置底模式下（非编辑时）始终压在其他窗口下面
bool NoteWin::KeepAtBottom(Window*) { return d->mode == ModeDesktop && !Editing() && !peeking; }

void NoteWin::BeginPeek()
{
    peeking = true;
    Wnd::KillTimer(win, TimerHide);
    if (capHidden) { Wnd::KillTimer(win, TimerSlide); sliding = false; Wnd::Move(win, dockedX, dockedY); capHidden = false; }
    Wnd::PeekToTop(win);
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
        RectI probe = { (int)d->x, (int)d->y, (int)d->x + 40, (int)d->y + 20 };
        if (Wnd::ScreenOf(probe, nullptr))
        {
            Wnd::Move(win, (int)d->x, (int)d->y);
            return;
        }
    }
    PlaceDefault();
}

void NoteWin::PlaceDefault()
{
    RectI wa = Wnd::PrimaryWorkArea();
    float S = Scale();
    int off = (int)(Max(0, g_app->IndexOf(this)) * 30 * S);
    Wnd::Move(win, wa.left + (int)(80 * S) + off, wa.top + (int)(80 * S) + off);
}

void NoteWin::BringOnScreen()
{
    if (!win) return;
    if (capHidden) { Wnd::Move(win, dockedX, dockedY); capHidden = false; }
    RectI r = Wnd::ScreenBounds(win), wa;
    bool visible = false;
    if (Wnd::ScreenOf(r, &wa))
    {
        int iw = Min(wa.right, r.right) - Max(wa.left, r.left), ih = Min(wa.bottom, r.bottom) - Max(wa.top, r.top);
        visible = iw >= Min(r.right - r.left, 60) && ih >= Min(r.bottom - r.top, 20);
    }
    if (!visible) { PlaceDefault(); if (d->collapsed) SnapCapsule(); }
    SaveLayout();
}

void NoteWin::SaveLayout()
{
    if (!win) return;
    // 只存位置。宽高只在用户拖边框缩放时改（WM_SIZE），不从窗口像素反推：
    // 跨缩放不同的屏拖动时，窗口像素可能还是旧屏的，反推出来会越拖越大
    RectI r = Wnd::ScreenBounds(win);
    d->hasPos = true;
    if (capHidden) { d->x = dockedX; d->y = dockedY; }
    else { d->x = r.left; d->y = r.top; }
    Save();
}

RectI NoteWin::WorkAreaPx() const
{
    RectI r = Wnd::ScreenBounds(win);
    return Wnd::WorkAreaAt((r.left + r.right) / 2, (r.top + r.bottom) / 2);
}

// =====================================================================
// 胶囊：折叠、吸边、贴边后自动缩进、悬停预览
// =====================================================================
void NoteWin::Collapse()
{
    if (d->collapsed) return;
    if (!win) { d->collapsed = true; Save(); return; }
    CommitEdit(true);
    SaveLayout();
    RectI wa = WorkAreaPx(), r = Wnd::ScreenBounds(win);
    int snap = (int)(24 * Scale());
    bool keepRight = Abs(wa.right - r.right) < snap, keepBottom = Abs(wa.bottom - r.bottom) < snap;
    EnterCollapsed();
    RectI c = Wnd::ScreenBounds(win);
    int x = c.left, y = c.top;
    if (keepRight) x = wa.right - (c.right - c.left);
    if (keepBottom) y = wa.bottom - (c.bottom - c.top);
    Wnd::Move(win, x, y);
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
    if (!win) { d->collapsed = false; Save(); return; }
    Wnd::KillTimer(win, TimerHide);
    Wnd::KillTimer(win, TimerSlide);
    sliding = false;
    ClosePreview();
    if (capHidden) { Wnd::Move(win, dockedX, dockedY); capHidden = false; }
    // 贴在右/下边的胶囊，展开时朝屏幕里面展开
    RectI wa = WorkAreaPx(), c = Wnd::ScreenBounds(win);
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
    Wnd::Move(win, x, y);
    Render();
    SaveLayout();
}

// 胶囊离屏幕边缘不到 24 DIP 就贴上去；跑出屏幕的部分拉回来；贴着的一侧改成直角
void NoteWin::SnapCapsule()
{
    if (!d->collapsed || capHidden) return;
    RectI wa = WorkAreaPx(), r = Wnd::ScreenBounds(win);
    int w = r.right - r.left, h = r.bottom - r.top, x = r.left, y = r.top;
    int snap = (int)(24 * Scale());
    if (x - wa.left < snap) x = wa.left;
    else if (wa.right - (x + w) < snap) x = wa.right - w;
    if (y - wa.top < snap) y = wa.top;
    else if (wa.bottom - (y + h) < snap) y = wa.bottom - h;
    bool l = x == wa.left, rr = x + w == wa.right, t = y == wa.top, b = y + h == wa.bottom;
    bool changed = l != dockL || rr != dockR || t != dockT || b != dockB;
    dockL = l; dockR = rr; dockT = t; dockB = b;
    if (x != r.left || y != r.top) Wnd::Move(win, x, y);
    if (changed) Render();
}

void NoteWin::ScheduleCapsuleHide()
{
    if (!d->collapsed) return;
    Wnd::SetTimer(win, TimerHide, 700);
}

bool NoteWin::OtherScreenAt(int x, int y) const { return Wnd::OnScreen(x, y); }

void NoteWin::OnHideTimer()
{
    Wnd::KillTimer(win, TimerHide);
    PointI pt = Wnd::CursorScreen();
    RectI r = Wnd::ScreenBounds(win);
    auto in = [](const RectI& a, PointI p) { return p.x >= a.left && p.x < a.right && p.y >= a.top && p.y < a.bottom; };
    bool overMe = in(r, pt);
    bool overPreview = false;
    if (preview && Wnd::IsVisible(preview)) overPreview = in(Wnd::ScreenBounds(preview), pt);
    if (!d->collapsed || overMe || overPreview || peeking) { if (d->collapsed && (overMe || overPreview)) ScheduleCapsuleHide(); return; }
    ClosePreview();
    if (capHidden || !(dockL || dockR || dockT || dockB)) return;
    RectI wa = WorkAreaPx();
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
    RectI r = Wnd::ScreenBounds(win);
    slideFromX = r.left; slideFromY = r.top; slideToX = x; slideToY = y;
    slideStart = TickMs();
    sliding = true;
    Wnd::SetTimer(win, TimerSlide, 8);
}

// ---- 悬停预览卡片：单独一个分层窗口，画出所有条目 ----
void NoteWin::ShowPreview()
{
    if (!d->collapsed || dragIndex >= 0) return;
    if (!preview)
    {
        if (!previewHandler) previewHandler = new NotePreview(this);
        RectI r = Wnd::ScreenBounds(win);
        preview = Wnd::NewPopup(previewHandler, Wnd::Native(win), r.left, r.top, Str(L"DeskNote 预览"));
    }
    if (!preview) return;
    RenderPreview();
    Wnd::ShowNoActivate(preview);
}

void NoteWin::ClosePreview()
{
    if (preview) { Window* p = preview; preview = nullptr; Wnd::Destroy(p); }
    previewHover = false;
}

void NoteWin::RenderPreview()
{
    if (!preview) return;
    float S = Scale();
    float fs = (float)d->fontSize, ts = Min(TitleSize(d), 18.f), lineH = LineH(d);
    const float padX = 16, padT = 12, padB = 12, maxW = 360, maxH = 460;
    NoteFont ft = MakeFormat(ts, 700, false);
    NoteFont fb = MakeFormat(fs, d->fontWeight, true);
    TextLayout* lt = MakeLayout(d->title, ft, maxW - 2 * padX, 0);
    float contentW = Min(LayoutWidth(lt), maxW - 2 * padX);
    float markW = d->checkboxes ? CheckColW(d) : R0(fs * 0.9f);
    float gap = R0(fs * 0.35f);
    Vec<TextLayout*> ls;
    float h = padT + LayoutHeight(lt) + 8;
    if (d->items.n == 0)
    {
        TextLayout* l = MakeLayout(Str(L"（空）"), fb, maxW - 2 * padX, lineH);
        contentW = Max(contentW, LayoutWidth(l));
        ls.Push(l);
        h += LayoutHeight(l);
    }
    for (int i = 0; i < d->items.n; i++)
    {
        const NoteItem& it = d->items[i];
        float chip = ChipWidth(d, it);
        float avail = maxW - 2 * padX - markW - (chip > 0 ? chip + 8 : 0);
        TextLayout* l = ItemLayout(d, it, fb, avail, false);
        contentW = Max(contentW, Min(LayoutWidth(l), avail) + markW + (chip > 0 ? chip + 8 : 0));
        ls.Push(l);
        h += Max(lineH, LayoutHeight(l)) + gap;
    }
    float W = R0(contentW + 2 * padX), H = R0(Min(h - gap + padB, maxH));
    int pw = (int)(W * S + 0.99f), ph = (int)(H * S + 0.99f);
    if (Painter* rt = Wnd::BeginPaint(preview, pw, ph))
    {
        Gfx::GrayscaleText(rt);
        Ui::Fill(rt, RectF(0, 0, W, H), 8, Argb(d->bgColor, (float)Max(d->opacity, 0.94)));
        Ui::Stroke(rt, RectF(0, 0, W, H), 8, Argb(d->textColor, 0.16f), 1);
        Gfx::PushClip(rt, RectF(0, 0, W, H - padB / 2));
        float y = padT;
        Gfx::Draw(rt, lt, padX, y, Argb(d->textColor, 1));
        y += LayoutHeight(lt) + 8;
        for (int i = 0; i < ls.n; i++)
        {
            if (d->items.n == 0) { Gfx::Draw(rt, ls[i], padX, y, Argb(d->textColor, 0.55f)); break; }
            const NoteItem& it = d->items[i];
            bool done = d->checkboxes && it.done;
            if (d->checkboxes)
            {
                float size = CheckSize(d);
                DrawCheck(rt, d, padX, y + (lineH - size) / 2, size, it.done, false, 1);
            }
            else Ui::Circle(rt, padX + 4, y + lineH / 2, 2.5f, Argb(d->textColor, 0.55f));
            Gfx::Draw(rt, ls[i], padX + markW, y, Argb(d->textColor, done ? 0.45f : 1.f));
            if (it.hasRemind) DrawChip(rt, d, it, W - padX - ChipWidth(d, it), y + (lineH - ChipH(d)) / 2, done ? 0.5f : 1.f);
            y += Max(lineH, LayoutHeight(ls[i])) + gap;
        }
        Gfx::PopClip(rt);
    }
    // 往屏幕里面的方向弹出
    RectI r = Wnd::ScreenBounds(win);
    int off = (int)(6 * S), x, y;
    if (dockR) { x = (capHidden ? dockedX : r.left) - pw - off; y = r.top; }
    else if (dockL) { x = (capHidden ? dockedX + (r.right - r.left) : r.right) + off; y = r.top; }
    else if (dockB) { x = r.left; y = r.top - ph - off; }
    else { x = r.left; y = r.bottom + off; }
    RectI wa = WorkAreaPx();
    if (y + ph > wa.bottom) y = wa.bottom - ph;
    if (y < wa.top) y = wa.top;
    if (x + pw > wa.right) x = wa.right - pw;
    if (x < wa.left) x = wa.left;
    Wnd::Move(preview, x, y);
    Wnd::EndPaint(preview);
    for (int i = 0; i < ls.n; i++) Gfx::Free(ls[i]);
    Gfx::Free(lt);
}

bool NoteWin::OnPreviewEvent(const Event& e)
{
    switch (e.type)
    {
    case EvMouseDown: if (e.button == MouseLeft) Expand(); return true;
    case EvMouseMove:
        if (!previewHover)
        {
            previewHover = true;
            Wnd::KillTimer(win, TimerHide);
        }
        return true;
    case EvMouseLeave:
        previewHover = false;
        ScheduleCapsuleHide();
        return true;
    }
    return false;
}

// =====================================================================
// 右键菜单
// =====================================================================
enum
{
    CmdUndo = 1, CmdRedo, CmdUp, CmdDown, CmdDelete, CmdRemind, CmdUnremind, CmdAdd, CmdClearDone,
    CmdFold, CmdChecks, CmdModeDesktop, CmdModeTopmost, CmdModeNormal, CmdShadow, CmdOpacity, CmdFontSize,
    CmdNewNote, CmdDeleteNote, CmdTrash, CmdSettings, CmdExit, CmdHide, CmdManage, CmdRename, CmdShow, CmdLocate,
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

// 外观、窗口模式、勾选框、折叠：便签右键菜单和便签管理里的「设置」共用
void NoteWin::AddStyleItems(Ui::Menu& m)
{
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
}

// 便签管理里一张便签的「设置」：和右键菜单同一套，隐藏着的便签也能改（显示时按新设置）
void NoteWin::ShowSettingsMenu(int sx, int sy, void* owner)
{
    CommitEdit(true);
    ClosePreview();
    Ui::Menu m;
    m.Header(d->title);
    if (win) m.Item(CmdRename, L"改标题", 0xE70F);
    AddStyleItems(m);
    m.Sep();
    if (d->hidden) m.Item(CmdShow, L"显示这张便签", 0xE890);
    else
    {
        m.Item(CmdLocate, L"定位", 0xE81D);
        m.Item(CmdHide, L"隐藏这张便签", 0xED1A);
    }
    m.Item(CmdDeleteNote, L"删除这张便签…", 0xE74D);
    BeginChange();
    menuChanged = false;
    int cmd = Ui::MenuRun(owner, &m, sx, sy, NoteMenuLive, this);
    if (menuChanged) Save();
    EndChange();
    if (cmd) RunCommand(cmd, NotEditing); // 可能删掉了这张便签，之后不再碰 this
}

void NoteWin::OnContextMenu(int sx, int sy)
{
    if (Editing())
    {
        float ex, ey;
        Wnd::FromScreen(win, sx, sy, &ex, &ey);
        if (InEditBox(ex, ey)) { EditContextMenu(sx, sy); return; }
    }
    CommitEdit(true);
    ClosePreview();
    int target = NotEditing;
    if (!d->collapsed)
    {
        float px, py;
        Wnd::FromScreen(win, sx, sy, &px, &py);
        target = HitRow(py);
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
    AddStyleItems(m);
    m.Sep();
    m.Item(CmdNewNote, L"新建便签", 0xE70B);
    m.Item(CmdHide, L"隐藏这张便签", 0xED1A);
    m.Item(CmdDeleteNote, L"删除这张便签…", 0xE74D);
    m.Sep();
    m.Item(CmdManage, L"便签管理", 0xE71D);
    m.Item(CmdTrash, L"回收站", 0xE7B8);
    m.Item(CmdSettings, L"设置", 0xE713);
    m.Item(CmdExit, L"退出 DeskNote", 0xE7E8);

    // 菜单里实时调的外观算一步撤销
    BeginChange();
    menuChanged = false;
    int cmd = Ui::MenuRun(Wnd::Native(win), &m, sx, sy, NoteMenuLive, this);
    if (menuChanged) Save();
    EndChange();
    if (menuChanged) SendToBottom();
    if (cmd) RunCommand(cmd, target);
    if (win && d->collapsed) ScheduleCapsuleHide();
}

void NoteWin::DebugMenu(int sub)
{
    RectI r = Wnd::ScreenBounds(win);
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
    case CmdManage: g_app->ShowManage(); return;
    case CmdHide: SetHidden(true); return; // 之后 this 的窗口没了，直接返回
    case CmdShow: SetHidden(false); return;
    case CmdLocate: Locate(); return;
    case CmdRename: Locate(); BeginEdit(TitleIndex); return;
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
    u32 v = background ? d->bgColor : d->textColor;
    if (Wnd::PickColor(win, &v))
    {
        BeginChange();
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
    int r = ReminderDialog(Wnd::Native(win), cur.text, cur.hasRemind, cur.remind, &t);
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

int NoteWin::CursorAt(Window*, float xd, float yd)
{
    int cur = CursorArrow;
    if (Editing() && InEditBox(xd, yd) && !HitEdge(xd, yd)) cur = CursorIBeam;
    else if (!d->collapsed && dragIndex < 0)
    {
        switch (HitEdge(xd, yd))
        {
        case 1: case 2: cur = CursorSizeWE; break;
        case 3: case 6: cur = CursorSizeNS; break;
        case 4: case 8: cur = CursorSizeNWSE; break;
        case 5: case 7: cur = CursorSizeNESW; break;
        default:
        {
            int idx = HitRow(yd);
            bool row = idx >= 0 && idx < d->items.n;
            const RowInfo* ri = nullptr;
            for (int i = 0; row && i < rows.n; i++) if (rows[i].index == idx) ri = &rows[i];
            if (row && xd < PadX) cur = CursorMove;
            else if (row && d->checkboxes && xd >= PadX && xd < PadX + CheckColW(d)) cur = CursorHand;
            else if (ri && ri->chipLeft > 0 && xd >= ri->chipLeft && yd + scrollY < ri->top + LineH(d)) cur = CursorHand;
        }
        }
    }
    else if (dragIndex >= 0) cur = CursorSizeNS;
    return cur;
}

// 只是拖着挪位置时（跨屏时系统会按旧屏的像素摆窗口），保持便签自己的大小
bool NoteWin::KeepSizeOnMove(Window*, float* width, float* height)
{
    if (d->collapsed) return false;
    *width = (float)d->width;
    *height = (float)d->height;
    return true;
}

bool NoteWin::MinSize(Window*, float* width, float* height)
{
    if (d->collapsed) return false;
    *width = 160;
    *height = 120;
    return true;
}

bool NoteWin::OnEvent(Window* w, const Event& e)
{
    if (!win || w != win) return false;
    switch (e.type)
    {
    case EvMouseDown:
        if (e.button == MouseLeft) OnLButtonDown(e.x, e.y, e.clicks == 2, e.mods);
        return true;
    case EvMouseUp:
        if (e.button == MouseRight)
        {
            PointI p = Wnd::ToScreen(win, e.x, e.y);
            OnContextMenu(p.x, p.y);
            return true;
        }
        if (editSelecting) { editSelecting = false; Wnd::Capture(win, false); }
        if (dragIndex >= 0) EndReorder();
        return true;
    case EvCaptureLost:
        editSelecting = false;
        if (dragIndex >= 0) EndReorder();
        return true;
    case EvMouseMove:
    {
        if (dragIndex >= 0) { OnReorderMove(e.y); return true; }
        if (editSelecting && Editing())
        {
            int pos = EditHit(e.x, e.y);
            if (pos != caret) EditSetCaret(pos, true);
            return true;
        }
        bool entered = false;
        if (!hover)
        {
            hover = true;
            entered = true;
            if (d->collapsed)
            {
                Wnd::KillTimer(win, TimerHide);
                if (capHidden) { capHidden = false; SlideTo(dockedX, dockedY); }
                ShowPreview();
                return true;
            }
        }
        if (!d->collapsed)
        {
            // 鼠标所在的行、按钮变了才重画
            int row = HitRow(e.y);
            if (row >= d->items.n || HitEdge(e.x, e.y)) row = -1;
            int b = HitToolbar(e.x, e.y);
            if (row != hoverRow || b != hoverBtn || entered) { hoverRow = row; hoverBtn = b; Render(); }
        }
        return true;
    }
    case EvMouseLeave:
        hover = false;
        hoverRow = -1;
        hoverBtn = 0;
        if (d->collapsed) ScheduleCapsuleHide();
        else Render();
        return true;
    case EvWheel:
    {
        if (d->collapsed) return true;
        float H;
        Wnd::ClientSize(win, nullptr, &H);
        float maxScroll = Max(0.f, contentHeight - (H - PadTop - PadBottom));
        float ns = Clamp(scrollY - e.wheel * LineH(d) * 3, 0.f, maxScroll);
        if (ns != scrollY) { scrollY = ns; Render(); UpdateImePos(); }
        return true;
    }
    case EvChar:
        if (Editing()) { EditChar((wchar_t)e.ch); return true; }
        return false;
    case EvImeStart:
        if (caret != anchor) EditInsert(Str(), 0);
        UpdateImePos();
        return true;
    case EvImeUpdate:
        if (e.hasResult)
        {
            compText = Str();
            compCursor = 0;
            if (e.imeResult.n) EditInsert(e.imeResult, 1);
        }
        if (e.hasComp)
        {
            compText = e.imeComp;
            compCursor = Clamp(e.imeCursor, 0, compText.n);
        }
        else if (!e.hasResult) compText = Str();
        EditUpdated();
        return true;
    case EvImeEnd:
        compText = Str();
        compCursor = 0;
        EditUpdated();
        return true;
    case EvKeyDown:
    {
        if (Editing()) return EditKey(e.key, e.mods);
        bool ctrl = (e.mods & ModCtrl) != 0, shift = (e.mods & ModShift) != 0;
        if (ctrl && e.key == 'Z' && !shift) { Undo(); return true; }
        if (ctrl && (e.key == 'Y' || (e.key == 'Z' && shift))) { Redo(); return true; }
        return false;
    }
    case EvActivate:
        if (!e.button)
        {
            if (Editing()) CommitEdit(true);
            if (!Editing()) SendToBottom();
        }
        return true;
    case EvMoveEnd:
        if (!d->collapsed) Render(); // 拖到缩放不同的屏上以后，按这块屏重新定大小
        SaveLayout();
        return true;
    case EvAppActivate:
        // 点到别的程序：结束“临时提到最前”
        if (!e.button && g_app->peeking) g_app->EndPeek();
        return false;
    case EvUserResize:
        if (!d->collapsed)
        {
            float S = Scale();
            int pw = (int)(d->width * S + 0.5), ph = (int)(d->height * S + 0.5);
            if ((e.pw != pw || e.ph != ph) && e.pw > 0 && e.ph > 0)
            {
                d->width = e.pw / S;
                d->height = e.ph / S;
                Render();
                UpdateImePos();
            }
        }
        return true;
    case EvScale:
        CommitEdit(true);
        Render();
        return true;
    case EvTimer:
        if (e.timer == TimerCaret) { caretOn = !caretOn; Render(); }
        else if (e.timer == TimerFlash)
        {
            if ((i64)(TickMs() - flashUntil) >= 0)
            {
                Wnd::KillTimer(win, TimerFlash);
                flashUntil = 0;
                if (!g_app->peeking) EndPeek();
            }
            Render();
        }
        else if (e.timer == TimerHide) OnHideTimer();
        else if (e.timer == TimerSlide)
        {
            float t = Min(1.f, (TickMs() - slideStart) / 150.f);
            float k = 1 - (1 - t) * (1 - t) * (1 - t); // ease-out
            Wnd::Move(win, slideFromX + (int)((slideToX - slideFromX) * k), slideFromY + (int)((slideToY - slideFromY) * k));
            if (t >= 1) { Wnd::KillTimer(win, TimerSlide); sliding = false; }
        }
        return true;
    case EvClose:
        return true; // 只有退出程序或删除便签时才真正关闭
    }
    return false;
}

// =====================================================================
// 自测入口：走和鼠标键盘一样的代码路径
// =====================================================================
void NoteWin::TestTypeItem(int index, const wchar_t* text)
{
    BeginEdit(index);
    editText = text;
    CommitEdit(true);
}
void NoteWin::TestBeginEdit(int index, const wchar_t* text)
{
    BeginEdit(index);
    editText = text;
    caret = anchor = editText.n;
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
