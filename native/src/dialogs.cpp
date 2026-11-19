// 回收站、设置、提醒对话框（都是自绘，跟随系统深浅色）
#include "ui.h"
#include <dwmapi.h>
#include <imm.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

static void SetIcons(HWND h)
{
    SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)g_app->icon);
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)g_app->iconSmall);
}

// =====================================================================
// 快捷键文字 <-> 修饰键 + 按键
// =====================================================================
bool HotkeyParse(const Str& s, UINT* mods, UINT* vk)
{
    *mods = 0;
    *vk = 0;
    int start = 0;
    for (int i = 0; i <= s.n; i++)
    {
        if (i < s.n && s.p[i] != L'+') continue;
        Str p = s.Sub(start, i - start).Trim();
        start = i + 1;
        if (p.EqI(L"Ctrl")) *mods |= MOD_CONTROL;
        else if (p.EqI(L"Alt")) *mods |= MOD_ALT;
        else if (p.EqI(L"Shift")) *mods |= MOD_SHIFT;
        else if (p.EqI(L"Win")) *mods |= MOD_WIN;
        else if (p.n == 1 && ((p.p[0] >= L'A' && p.p[0] <= L'Z') || (p.p[0] >= L'a' && p.p[0] <= L'z') || (p.p[0] >= L'0' && p.p[0] <= L'9')))
            *vk = p.p[0] >= L'a' ? p.p[0] - 32 : p.p[0];
        else if (p.n == 2 && (p.p[0] == L'D' || p.p[0] == L'd') && p.p[1] >= L'0' && p.p[1] <= L'9') *vk = p.p[1];
        else if (p.n >= 2 && (p.p[0] == L'F' || p.p[0] == L'f'))
        {
            i64 k = ParseInt(p.Sub(1), 0);
            if (k >= 1 && k <= 24) *vk = VK_F1 + (UINT)k - 1; else return false;
        }
        else if (p.EqI(L"Space")) *vk = VK_SPACE;
        else return false;
    }
    return *vk != 0 && *mods != 0;
}

Str HotkeyFormat(UINT mods, UINT vk)
{
    Str s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_SHIFT) s += L"Shift+";
    if (mods & MOD_WIN) s += L"Win+";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) s.Ch((wchar_t)vk);
    else if (vk >= VK_F1 && vk <= VK_F24) { s.Ch(L'F'); s.Int(vk - VK_F1 + 1); }
    else if (vk == VK_SPACE) s += L"Space";
    else { s += L"VK"; s.Int(vk); }
    return s;
}

// =====================================================================
// 回收站：按删除日期分组的卡片列表，每行带“恢复”和“彻底删除”；不自动清理
// =====================================================================
static void ApplyFrameTheme(HWND h);

struct TrashWin
{
    HWND hwnd = nullptr;
    int dpi = 96;
    float S() const { return dpi / 96.f; }
    struct Hit { int id; D2D1_RECT_F r; TrashEntry* e; };
    Vec<Hit> hits;            // 列表里的区域是内容坐标（要加滚动），顶栏是窗口坐标
    int hover = 0, press = 0;
    TrashEntry* hoverEntry = nullptr;
    bool tracking = false;
    float scroll = 0, contentH = 0, listTop = 0;
    enum { HEmpty = 1, HRow = 2, HRestore = 3, HPurge = 4 };

    void Invalidate() { InvalidateRect(hwnd, nullptr, FALSE); }
    float ClientW() { RECT rc; GetClientRect(hwnd, &rc); return rc.right / S(); }
    float ClientH() { RECT rc; GetClientRect(hwnd, &rc); return rc.bottom / S(); }
    void ClampScroll() { scroll = Clamp(scroll, 0.f, Max(0.f, contentH - (ClientH() - listTop))); }

    static Str DayLabel(Time t)
    {
        SYSTEMTIME st;
        SplitTime(t, &st);
        Time day = MakeTime(st.wYear, st.wMonth, st.wDay, 0, 0, 0);
        Time today = Today();
        if (day == today) return Str(L"今天");
        if (day == today - TicksPerDay) return Str(L"昨天");
        SYSTEMTIME now;
        GetLocalTime(&now);
        Str s;
        if (st.wYear != now.wYear) s.Int(st.wYear).Append(L"年");
        s.Int(st.wMonth).Append(L"月").Int(st.wDay).Append(L"日");
        return s;
    }
    static Time DayOf(Time t)
    {
        SYSTEMTIME st;
        SplitTime(t, &st);
        return MakeTime(st.wYear, st.wMonth, st.wDay, 0, 0, 0);
    }

    // 普通按钮 / 图标按钮
    void DrawButton(ID2D1RenderTarget* rt, D2D1_RECT_F r, const wchar_t* text, wchar_t icon, bool hot, bool down, bool enabled, bool danger)
    {
        const Ui::Theme& T = Ui::T;
        if (text)
        {
            Ui::Fill(rt, r, 4, Ui::Solid(down ? T.ctrlPress : hot ? T.ctrlHover : T.ctrlFill, T.cardBg));
            Ui::Stroke(rt, r, 4, T.ctrlBorder, 1);
            if (!T.dark) Ui::Line(rt, r.left + 4, r.bottom - 0.5f, r.right - 4, r.bottom - 0.5f, T.ctrlBorderBottom, 1);
            Ui::Text(rt, Str(text), r, 14, 400, !enabled ? T.disabled : down ? T.text2 : T.text, Ui::Center);
        }
        else
        {
            if (hot) Ui::Fill(rt, r, 4, down ? T.subtlePress : T.subtleHover);
            Ui::Icon(rt, icon, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 16, hot && danger ? T.critical : T.text2);
        }
    }

    void Build(ID2D1RenderTarget* rt)
    {
        hits.Clear();
        const Ui::Theme& T = Ui::T;
        float W = ClientW(), H = ClientH();
        const float L = 24, R = W - 24;
        Vec<TrashEntry*>& list = g_app->trash;

        // ---- 顶栏（不随列表滚动）----
        float y = 18;
        Str count;
        if (list.n) count.Int(list.n).Append(L" 项");
        Str emptyText(L"清空回收站");
        float bw = Ui::Measure(emptyText, 14, 400) + 24;
        D2D1_RECT_F eb = D2D1::RectF(R - bw, y, R, y + 32);
        if (list.n) { Hit h = { HEmpty, eb, nullptr }; hits.Push(h); }
        if (rt)
        {
            float tw = Ui::Text(rt, Str(L"回收站"), D2D1::RectF(L, y, R - bw - 16, y + 32), 20, 600, T.text);
            if (count.n) Ui::Text(rt, count, D2D1::RectF(L + tw + 10, y + 4, R - bw - 16, y + 32), 14, 400, T.text2);
            DrawButton(rt, eb, emptyText.c(), 0, hover == HEmpty, press == HEmpty, list.n > 0, false);
        }
        y += 32 + 6;
        Str hint(L"删掉的条目和便签都在这里，不会自动清理。恢复的条目会放回原来那张便签的原位置；双击一行也能恢复。");
        float hh = Ui::MeasureWrap(hint, R - L, 12, 400);
        if (rt) Ui::TextWrap(rt, hint, L, y, R - L, 12, 400, T.text2);
        y += hh + 14;
        listTop = y;

        // ---- 列表 ----
        if (!list.n)
        {
            if (rt)
            {
                float cy = listTop + (H - listTop) * 0.4f;
                Ui::Icon(rt, 0xE7B8, W / 2, cy - 28, 40, T.text3);
                Ui::Text(rt, Str(L"回收站是空的"), D2D1::RectF(L, cy + 6, R, cy + 30), 14, 400, T.text2, Ui::Center);
                Ui::Text(rt, Str(L"删除的条目和便签会先放在这里"), D2D1::RectF(L, cy + 30, R, cy + 50), 12, 400, T.text3, Ui::Center);
            }
            contentH = 0;
            return;
        }
        if (rt)
        {
            rt->PushAxisAlignedClip(D2D1::RectF(0, listTop, W, H), D2D1_ANTIALIAS_MODE_ALIASED);
            rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll));
        }
        float ly = listTop;
        Time lastDay = -1;
        const float rowH = 62;
        for (int i = 0; i < list.n; i++)
        {
            TrashEntry* e = list[i];
            Time day = DayOf(e->deletedAt);
            if (day != lastDay)
            {
                if (lastDay != -1) ly += 14;
                if (rt) Ui::Text(rt, DayLabel(e->deletedAt), D2D1::RectF(L, ly, R, ly + 20), 14, 600, T.text);
                ly += 20 + 8;
                lastDay = day;
            }
            D2D1_RECT_F card = D2D1::RectF(L, ly, R, ly + rowH);
            float cy = ly + rowH / 2;
            D2D1_RECT_F purge = D2D1::RectF(R - 12 - 32, cy - 16, R - 12, cy + 16);
            Str restoreText(L"恢复");
            float rw = Max(72.f, Ui::Measure(restoreText, 14, 400) + 24);
            D2D1_RECT_F restore = D2D1::RectF(purge.left - 8 - rw, cy - 16, purge.left - 8, cy + 16);
            Hit h1 = { HRow, card, e }, h2 = { HRestore, restore, e }, h3 = { HPurge, purge, e };
            hits.Push(h1); hits.Push(h2); hits.Push(h3);
            // 只画看得见的行
            if (rt && ly - scroll < H && ly + rowH - scroll > listTop)
            {
                bool rowHot = hoverEntry == e;
                Ui::Fill(rt, card, 4, rowHot ? Ui::Solid(T.subtleHover, T.cardBg) : T.cardBg);
                Ui::Stroke(rt, card, 4, T.cardBorder, 1);
                bool isNote = e->kind.Eq(L"note") && e->note;
                Ui::Icon(rt, isNote ? 0xE70B : e->item.done ? 0xE9D5 : 0xE7C3, L + 16 + 10, cy, 20, T.text2);
                Str main, sub;
                if (isNote)
                {
                    main = L"便签「" + e->note->title + L"」";
                    sub = L"整张便签 · ";
                    sub.Int(e->note->items.n).Append(L" 条");
                }
                else
                {
                    main = e->item.text.Replace(L"\r", L"").Replace(L"\n", L" ");
                    sub = L"来自「" + e->noteTitle + L"」";
                    if (e->item.done) sub += L" · 已完成";
                }
                SYSTEMTIME st;
                SplitTime(e->deletedAt, &st);
                sub += L" · ";
                sub.Pad2(st.wHour).Ch(L':').Pad2(st.wMinute).Append(L" 删除");
                float textL = L + 16 + 20 + 16, textR = restore.left - 16;
                Ui::Text(rt, main, D2D1::RectF(textL, cy - 19, textR, cy + 1), 14, 400, T.text);
                Ui::Text(rt, sub, D2D1::RectF(textL, cy + 1, textR, cy + 18), 12, 400, T.text2);
                bool hr = hover == HRestore && hoverEntry == e, hp = hover == HPurge && hoverEntry == e;
                DrawButton(rt, restore, restoreText.c(), 0, hr, hr && press == HRestore, true, false);
                DrawButton(rt, purge, nullptr, 0xE74D, hp, hp && press == HPurge, true, true);
            }
            ly += rowH + 4;
        }
        ly += 20;
        contentH = ly - listTop;
        if (rt)
        {
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
            rt->PopAxisAlignedClip();
            float viewH = H - listTop;
            if (contentH > viewH + 1)
            {
                float th = Max(24.f, viewH * viewH / contentH);
                float ty = listTop + (viewH - th) * Clamp(scroll / (contentH - viewH), 0.f, 1.f);
                Ui::Fill(rt, D2D1::RectF(W - 7, ty + 2, W - 4, ty + th - 2), 1.5f, T.text3);
            }
        }
    }

    void Paint()
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        Canvas c;
        if (c.Begin(rc.right, rc.bottom, (float)dpi, true))
        {
            D2D1_COLOR_F bg = Ui::C(Ui::T.winBg);
            c.rt->Clear(&bg);
            Build(c.rt);
        }
        c.EndBlt(dc);
        EndPaint(hwnd, &ps);
    }

    // 返回命中区域的 id，并给出对应条目
    int HitAt(int px, int py, TrashEntry** e)
    {
        float x = px / S(), y = py / S();
        *e = nullptr;
        for (int i = hits.n - 1; i >= 0; i--)
        {
            const Hit& h = hits[i];
            float yy = h.e ? y + scroll : y;
            if (h.e && y < listTop) continue;
            if (x >= h.r.left && x < h.r.right && yy >= h.r.top && yy < h.r.bottom) { *e = h.e; return h.id; }
        }
        return 0;
    }

    void Activate(int id, TrashEntry* e)
    {
        if (id == HEmpty)
        {
            Str msg(L"彻底删除回收站里的全部 ");
            msg.Int(g_app->trash.n).Append(L" 项？删除后不能恢复。");
            if (MessageBoxW(hwnd, msg.c(), L"清空回收站", MB_OKCANCEL | MB_ICONWARNING) == IDOK) g_app->EmptyTrash();
            return;
        }
        if (!e || !g_app->trash.Contains(e)) return;
        if (id == HRestore) g_app->Restore(e);
        else if (id == HPurge) g_app->Purge(e);
    }

    void Refresh()
    {
        TrashEntry* e = nullptr;
        if (hoverEntry && !g_app->trash.Contains(hoverEntry)) { hoverEntry = nullptr; hover = 0; }
        (void)e;
        Build(nullptr);
        ClampScroll();
        Invalidate();
    }

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        TrashWin* self = (TrashWin*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (m == WM_NCCREATE)
        {
            self = (TrashWin*)((CREATESTRUCTW*)l)->lpCreateParams;
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(h, m, w, l);
        switch (m)
        {
        case WM_PAINT: self->Paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: self->Build(nullptr); self->ClampScroll(); self->Invalidate(); return 0;
        case WM_GETMINMAXINFO:
            ((MINMAXINFO*)l)->ptMinTrackSize.x = (int)(440 * self->S());
            ((MINMAXINFO*)l)->ptMinTrackSize.y = (int)(320 * self->S());
            return 0;
        case WM_MOUSEMOVE:
        {
            if (!self->tracking)
            {
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
                self->tracking = true;
            }
            TrashEntry* e;
            int id = self->HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l), &e);
            if (id != self->hover || e != self->hoverEntry) { self->hover = id; self->hoverEntry = e; self->Invalidate(); }
            return 0;
        }
        case WM_MOUSELEAVE:
            self->tracking = false;
            self->hover = 0;
            self->hoverEntry = nullptr;
            self->Invalidate();
            return 0;
        case WM_LBUTTONDOWN:
        {
            TrashEntry* e;
            self->press = self->HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l), &e);
            if (self->press) SetCapture(h);
            self->Invalidate();
            return 0;
        }
        case WM_LBUTTONUP:
        {
            int p = self->press;
            self->press = 0;
            if (GetCapture() == h) ReleaseCapture();
            TrashEntry* e;
            int id = self->HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l), &e);
            if (p && id == p && p != HRow) self->Activate(p, e);
            self->Invalidate();
            return 0;
        }
        case WM_LBUTTONDBLCLK:
        {
            TrashEntry* e;
            if (self->HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l), &e) == HRow) self->Activate(HRestore, e);
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            float old = self->scroll;
            self->scroll -= GET_WHEEL_DELTA_WPARAM(w) / 120.f * 66;
            self->ClampScroll();
            if (self->scroll != old)
            {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(h, &p);
                TrashEntry* e;
                self->Build(nullptr);
                self->hover = self->HitAt(p.x, p.y, &e);
                self->hoverEntry = e;
                self->Invalidate();
            }
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(w) == IDCANCEL) DestroyWindow(h);
            return 0;
        case WM_DPICHANGED:
        {
            self->dpi = HIWORD(w);
            RECT* rc = (RECT*)l;
            SetWindowPos(h, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            self->Invalidate();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (l && WEq((const wchar_t*)l, L"ImmersiveColorSet")) { Ui::LoadTheme(); ApplyFrameTheme(h); self->Invalidate(); }
            break;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY:
            g_app->trashWnd = nullptr;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            delete self;
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

HWND TrashWindowCreate()
{
    Ui::LoadTheme();
    TrashWin* d = new TrashWin();
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, L"DeskNoteTrash", L"DeskNote 回收站", WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, g_inst, d);
    d->dpi = GetDpiForWindow(h);
    SetIcons(h);
    ApplyFrameTheme(h);
    POINT p;
    GetCursorPos(&p);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
    float S = d->S();
    int maxH = (int)((mi.rcWork.bottom - mi.rcWork.top) * 0.85f);
    RECT r = { 0, 0, (int)(640 * S), Min((int)(580 * S), maxH) };
    AdjustWindowRectExForDpi(&r, GetWindowLongW(h, GWL_STYLE), FALSE, GetWindowLongW(h, GWL_EXSTYLE), d->dpi);
    int w = r.right - r.left, ht = r.bottom - r.top;
    SetWindowPos(h, nullptr, (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - ht) / 2, w, ht, SWP_NOZORDER | SWP_NOACTIVATE);
    d->Build(nullptr);
    return h;
}

void TrashWindowRefresh(HWND w)
{
    TrashWin* d = (TrashWin*)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (d) d->Refresh();
}

// =====================================================================
// 设置：仿 Win11 设置页（分组标题 + 卡片 + 开关），整页自绘，只有三个输入框用系统控件
// =====================================================================
enum { SNone, SAuto, SHkToggle, SHkKeys, SOpenDir, SOpenBackup, SSyncToggle, SSyncSave };
struct HitArea { int id; D2D1_RECT_F r; };

static void ApplyFrameTheme(HWND h)
{
    BOOL dark = Ui::T.dark;
    DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF cap = Ui::Ref(Ui::T.winBg);
    DwmSetWindowAttribute(h, 35 /* DWMWA_CAPTION_COLOR：标题栏和内容同色 */, &cap, sizeof(cap));
}

struct SettingsWin
{
    static const int Width = 600;
    HWND hwnd = nullptr;
    int dpi = 96;
    float S() const { return dpi / 96.f; }
    HWND edits[3] = {};               // 服务器、密钥、文件夹
    D2D1_RECT_F frames[3] = {};       // 输入框外框（DIP，内容坐标）
    HFONT editFont = nullptr;
    int editH = 0;
    HBRUSH brushRest = nullptr, brushFocus = nullptr;
    Vec<HitArea> hits;
    int hover = SNone, press = SNone;
    bool tracking = false;
    float scroll = 0, contentH = 0;
    bool recording = false;
    UINT recMods = 0;
    Str hkMsg;                        // 快捷键出错时的提示

    void Invalidate() { InvalidateRect(hwnd, nullptr, FALSE); }

    void MakeBrushes()
    {
        if (brushRest) DeleteObject(brushRest);
        if (brushFocus) DeleteObject(brushFocus);
        brushRest = CreateSolidBrush(Ui::Ref(Ui::T.inputBg));
        brushFocus = CreateSolidBrush(Ui::Ref(Ui::T.inputFocusBg));
    }

    void MakeFont()
    {
        if (editFont) DeleteObject(editFont);
        editFont = CreateFontW(-(int)(14 * S() + 0.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        HDC dc = GetDC(hwnd);
        HGDIOBJ of = SelectObject(dc, editFont);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        editH = tm.tmHeight;
        SelectObject(dc, of);
        ReleaseDC(hwnd, dc);
        for (int i = 0; i < 3; i++) if (edits[i]) SendMessageW(edits[i], WM_SETFONT, (WPARAM)editFont, TRUE);
    }

    void KeyCue()
    {
        SendMessageW(edits[1], EM_SETCUEBANNER, TRUE, (LPARAM)(g_app->settings.syncKey.n ? L"已保存（不改就留空）" : L"粘贴 gopan 的密钥"));
    }

    void Init()
    {
        Settings& s = g_app->settings;
        MakeBrushes();
        const wchar_t* init[3] = { s.syncServer.c(), L"", s.syncFolder.c() };
        for (int i = 0; i < 3; i++)
        {
            edits[i] = CreateWindowExW(0, L"EDIT", init[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (i == 1 ? ES_PASSWORD : 0),
                                       0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(100 + i), g_inst, nullptr);
            Ui::AllowDarkControls(edits[i]);
        }
        MakeFont();
        SendMessageW(edits[0], EM_SETCUEBANNER, TRUE, (LPARAM)L"https://");
        SendMessageW(edits[2], EM_SETCUEBANNER, TRUE, (LPARAM)L"DeskNote");
        KeyCue();
        const wchar_t* e = g_app->ApplyHotkey();
        hkMsg = e ? e : L"";
    }

    // ---- 画控件 ----
    void Toggle(ID2D1RenderTarget* rt, int id, bool on, bool enabled, float right, float cy)
    {
        D2D1_RECT_F tr = D2D1::RectF(right - 40, cy - 10, right, cy + 10);
        if (enabled) { HitArea a = { id, D2D1::RectF(tr.left - 44, cy - 16, tr.right, cy + 16) }; hits.Push(a); }
        if (!rt) return;
        const Ui::Theme& T = Ui::T;
        bool hot = enabled && hover == id, down = enabled && press == id;
        Ui::Text(rt, Str(on ? L"开" : L"关"), D2D1::RectF(tr.left - 44, cy - 12, tr.left - 12, cy + 12), 14, 400, enabled ? T.text : T.disabled, Ui::Right);
        float kr = down ? 7.f : hot ? 7.f : 6.f;
        if (on)
        {
            Ui::Fill(rt, tr, 10, enabled ? (hot ? (T.accent & 0x00FFFFFF) | 0xE6000000 : T.accent) : T.disabled);
            Ui::Circle(rt, tr.right - 10 - (down ? 2.f : 0.f), cy, kr, T.onAccent);
        }
        else
        {
            Ui::Fill(rt, tr, 10, hot ? T.subtleHover : T.toggleOffFill);
            Ui::Stroke(rt, tr, 10, enabled ? T.strong : T.disabled, 1);
            Ui::Circle(rt, tr.left + 10 + (down ? 2.f : 0.f), cy, kr, enabled ? T.text2 : T.disabled);
        }
    }

    float Button(ID2D1RenderTarget* rt, int id, const wchar_t* text, float right, float cy, bool accent)
    {
        Str t(text);
        float w = Max(88.f, Ui::Measure(t, 14, 400) + 24);
        D2D1_RECT_F r = D2D1::RectF(right - w, cy - 16, right, cy + 16);
        HitArea a = { id, r };
        hits.Push(a);
        if (!rt) return w;
        const Ui::Theme& T = Ui::T;
        bool hot = hover == id, down = press == id;
        if (accent)
        {
            Ui::Fill(rt, r, 4, down ? (T.accent & 0x00FFFFFF) | 0xCC000000 : hot ? (T.accent & 0x00FFFFFF) | 0xE6000000 : T.accent);
            Ui::Text(rt, t, r, 14, 400, down ? (T.onAccent & 0x00FFFFFF) | 0x80000000 : T.onAccent, Ui::Center);
        }
        else
        {
            Ui::Fill(rt, r, 4, Ui::Solid(down ? T.ctrlPress : hot ? T.ctrlHover : T.ctrlFill, T.cardBg));
            Ui::Stroke(rt, r, 4, T.ctrlBorder, 1);
            if (!T.dark) Ui::Line(rt, r.left + 4, r.bottom - 0.5f, r.right - 4, r.bottom - 0.5f, T.ctrlBorderBottom, 1);
            Ui::Text(rt, t, r, 14, 400, down ? T.text2 : T.text, Ui::Center);
        }
        return w;
    }

    // 快捷键：一组按键帽 + 编辑按钮；返回占的宽度
    float HotkeyKeys(ID2D1RenderTarget* rt, float right, float cy, bool enabled)
    {
        const Ui::Theme& T = Ui::T;
        Vec<Str> keys;
        if (recording)
        {
            if (recMods & MOD_CONTROL) keys.Push(Str(L"Ctrl"));
            if (recMods & MOD_ALT) keys.Push(Str(L"Alt"));
            if (recMods & MOD_SHIFT) keys.Push(Str(L"Shift"));
            if (recMods & MOD_WIN) keys.Push(Str(L"Win"));
        }
        else
        {
            const Str& hk = g_app->settings.hotkey;
            int start = 0;
            for (int i = 0; i <= hk.n; i++)
                if (i == hk.n || hk.p[i] == L'+') { Str k = hk.Sub(start, i - start).Trim(); if (k.n) keys.Push(k); start = i + 1; }
        }
        float x = right;
        // 编辑按钮
        D2D1_RECT_F pen = D2D1::RectF(x - 32, cy - 16, x, cy + 16);
        x -= 32 + 6;
        float widths[8];
        int n = Min(keys.n, 8);
        float total = 0;
        for (int i = 0; i < n; i++) { widths[i] = Max(30.f, Ui::Measure(keys[i], 12, 600) + 18); total += widths[i] + (i ? 4 : 0); }
        Str waiting(L"请按组合键");
        if (recording && !n) total = Ui::Measure(waiting, 12, 400) + 24;
        float left = x - total;
        if (enabled) { HitArea a = { SHkKeys, D2D1::RectF(left - 4, cy - 18, right, cy + 18) }; hits.Push(a); }
        if (rt)
        {
            bool hot = enabled && hover == SHkKeys;
            if (hot || recording) Ui::Fill(rt, pen, 4, recording ? T.subtlePress : T.subtleHover);
            Ui::Icon(rt, 0xE70F, (pen.left + pen.right) / 2, cy, 14, enabled ? T.text2 : T.disabled);
            if (recording && !n)
            {
                D2D1_RECT_F r = D2D1::RectF(left, cy - 15, x, cy + 15);
                Ui::Stroke(rt, r, 4, T.accent, 1.5f);
                Ui::Text(rt, waiting, r, 12, 400, T.accent, Ui::Center);
            }
            float kx = left;
            for (int i = 0; i < n; i++)
            {
                D2D1_RECT_F r = D2D1::RectF(kx, cy - 15, kx + widths[i], cy + 15);
                if (enabled) Ui::Fill(rt, r, 4, T.accent);
                else { Ui::Fill(rt, r, 4, T.ctrlFill); Ui::Stroke(rt, r, 4, T.ctrlBorder, 1); }
                Ui::Text(rt, keys[i], r, 12, 600, enabled ? T.onAccent : T.disabled, Ui::Center);
                kx += widths[i] + 4;
            }
        }
        return right - left;
    }

    // 卡片：左边图标，标题 + 说明，右边留 rightW 给控件；below = 下面接着的展开区高度
    D2D1_RECT_F Card(ID2D1RenderTarget* rt, float& y, wchar_t icon, const Str& title, const Str& desc, u32 descColor, float rightW, bool wrapDesc, float below)
    {
        const float L = 24, R = Width - 24;
        float textL = L + 16 + 20 + 16, textR = R - 16 - (rightW > 0 ? rightW + 16 : 0);
        float descH = desc.n ? (wrapDesc ? Ui::MeasureWrap(desc, textR - textL, 12, 400) : 17) : 0;
        float block = 20 + descH;
        float h = Max(68.f, block + 30);
        D2D1_RECT_F r = D2D1::RectF(L, y, R, y + h);
        if (rt)
        {
            const Ui::Theme& T = Ui::T;
            D2D1_RECT_F all = D2D1::RectF(L, y, R, y + h + below);
            Ui::Fill(rt, all, 4, T.cardBg);
            Ui::Stroke(rt, all, 4, T.cardBorder, 1);
            Ui::Icon(rt, icon, L + 16 + 10, y + h / 2, 20, T.text);
            float ty = y + (h - block) / 2;
            Ui::Text(rt, title, D2D1::RectF(textL, ty, textR, ty + 20), 14, 400, T.text);
            if (desc.n)
            {
                if (wrapDesc) Ui::TextWrap(rt, desc, textL, ty + 20, textR - textL, 12, 400, descColor);
                else Ui::Text(rt, desc, D2D1::RectF(textL, ty + 20, textR, ty + 20 + descH), 12, 400, descColor);
            }
        }
        y += h;
        return r;
    }

    void Section(ID2D1RenderTarget* rt, float& y, const wchar_t* t, bool first)
    {
        if (!first) y += 24;
        if (rt) Ui::Text(rt, Str(t), D2D1::RectF(24, y, Width - 24, y + 20), 14, 600, Ui::T.text);
        y += 20 + 8;
    }

    // 布局并（rt 不为空时）绘制；同时记下可点区域和输入框位置
    void Build(ID2D1RenderTarget* rt)
    {
        hits.Clear();
        const Ui::Theme& T = Ui::T;
        Settings& s = g_app->settings;
        const float L = 24, R = Width - 24;
        float y = 16;

        Section(rt, y, L"常规", true);
        D2D1_RECT_F c = Card(rt, y, 0xE7E8, Str(L"开机自动启动"),
                             Str(g_app->isDefaultDir ? L"移动 exe 后，打开一次新位置的 exe，自启会跟着改过去" : L"用测试数据目录打开时不可用"),
                             T.text2, 84, true, 0);
        Toggle(rt, SAuto, App::IsAutoStart(), g_app->isDefaultDir, R - 16, (c.top + c.bottom) / 2);
        y += 4;
        {
            Str desc;
            u32 dc = T.text2;
            if (hkMsg.n) { desc = hkMsg; dc = T.critical; }
            else if (recording) { desc = L"按下新的组合键（要带 Ctrl 或 Alt），Esc 取消"; dc = T.accent; }
            else desc = L"临时把所有便签提到最前面，再按一次恢复";
            // 右边控件的宽度要先量出来
            int keep = hits.n;
            float keysW = HotkeyKeys(nullptr, 0, 0, s.hotkeyEnabled);
            while (hits.n > keep) hits.RemoveAt(hits.n - 1);
            c = Card(rt, y, 0xE765, Str(L"全局快捷键"), desc, dc, 84 + 20 + keysW, true, 0);
            float cy = (c.top + c.bottom) / 2;
            Toggle(rt, SHkToggle, s.hotkeyEnabled, true, R - 16, cy);
            HotkeyKeys(rt, R - 16 - 84 - 20, cy, s.hotkeyEnabled);
        }

        Section(rt, y, L"数据", false);
        Str openW(L"打开");
        float bw = Max(88.f, Ui::Measure(openW, 14, 400) + 24);
        c = Card(rt, y, 0xE8B7, Str(L"数据文件夹"), Store::Dir, T.text2, bw, false, 0);
        Button(rt, SOpenDir, L"打开", R - 16, (c.top + c.bottom) / 2, false);
        y += 4;
        Str bh(L"每天自动备份一次，保留最近 ");
        bh.Int(Store::KeepBackups).Append(L" 天。要找回某天的内容，把备份里的文件复制回 notes 文件夹");
        c = Card(rt, y, 0xE81C, Str(L"每日备份"), bh, T.text2, bw, true, 0);
        Button(rt, SOpenBackup, L"打开", R - 16, (c.top + c.bottom) / 2, false);

        Section(rt, y, L"同步", false);
        const float textL = L + 16 + 20 + 16;
        Str help(L"密钥在 gopan 网页的「Agent 接入」里新建，权限勾选：浏览与搜索、读取与下载内容、新建重命名移动与复制、移入回收站与恢复。"
                 L"只同步内容和外观，窗口位置、回收站各台电脑自己管；两台电脑同时改了同一张便签时，本地那份另存成「冲突副本」，不会丢。");
        float helpH = Ui::MeasureWrap(help, R - 16 - textL, 12, 400);
        float below = 8 + 3 * 44 + 6 + helpH + 14 + 32 + 18;
        Str status = SyncStatus(g_app->sync);
        if (!status.n) status = L"多台电脑共用这些便签";
        c = Card(rt, y, 0xE753, Str(L"同步到 gopan"), status, SyncFailed(g_app->sync) ? T.critical : T.text2, 84, false, below);
        Toggle(rt, SSyncToggle, s.syncEnabled, true, R - 16, (c.top + c.bottom) / 2);
        if (rt)
        {
            Ui::FillCorners(rt, D2D1::RectF(L + 1, y, R - 1, y + below - 1), 0, 0, 3, 3, T.cardBg2);
            Ui::Line(rt, L + 1, y + 0.5f, R - 1, y + 0.5f, T.cardBorder, 1);
        }
        y += 8;
        const wchar_t* labels[3] = { L"服务器", L"密钥", L"文件夹" };
        for (int i = 0; i < 3; i++)
        {
            float cy = y + 22;
            frames[i] = D2D1::RectF(R - 16 - 340, cy - 16, R - 16, cy + 16);
            if (rt)
            {
                Ui::Text(rt, Str(labels[i]), D2D1::RectF(textL, cy - 12, frames[i].left - 12, cy + 12), 14, 400, T.text);
                bool f = GetFocus() == edits[i];
                D2D1_RECT_F fr = frames[i];
                Ui::Fill(rt, fr, 4, f ? T.inputFocusBg : T.inputBg);
                Ui::Stroke(rt, fr, 4, T.ctrlBorder, 1);
                if (f) Ui::Fill(rt, D2D1::RectF(fr.left + 1, fr.bottom - 2, fr.right - 1, fr.bottom), 1, T.accent);
                else Ui::Line(rt, fr.left + 3, fr.bottom - 0.5f, fr.right - 3, fr.bottom - 0.5f, T.strong, 1);
            }
            y += 44;
        }
        y += 6;
        if (rt) Ui::TextWrap(rt, help, textL, y, R - 16 - textL, 12, 400, T.text2);
        y += helpH + 14;
        Button(rt, SSyncSave, L"保存并立即同步", R - 16, y + 16, true);
        y += 32 + 18;

        y += 20;
        Str ver(L"DeskNote " APP_VERSION L" · 原生版 · 编译于 ");
        const char* date = __DATE__ " " __TIME__;
        int dn = 0;
        while (date[dn]) dn++;
        ver += FromUtf8(date, dn);
        if (rt) Ui::Text(rt, ver, D2D1::RectF(L, y, R, y + 17), 12, 400, T.text3);
        y += 17 + 24;
        contentH = y;
    }

    float ClientHDip()
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        return rc.bottom / S();
    }

    void ClampScroll() { scroll = Clamp(scroll, 0.f, Max(0.f, contentH - ClientHDip())); }

    void PlaceEdits()
    {
        float s = S();
        for (int i = 0; i < 3; i++)
        {
            D2D1_RECT_F f = frames[i];
            int x = (int)((f.left + 11) * s), w = (int)((f.right - 11) * s) - x;
            int cy = (int)(((f.top + f.bottom) / 2 - scroll) * s);
            RECT cur;
            GetWindowRect(edits[i], &cur);
            MapWindowPoints(nullptr, hwnd, (POINT*)&cur, 2);
            if (cur.left != x || cur.top != cy - editH / 2 || cur.right - cur.left != w || cur.bottom - cur.top != editH)
                MoveWindow(edits[i], x, cy - editH / 2, w, editH, TRUE);
        }
    }

    void Paint()
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        Canvas c;
        if (c.Begin(rc.right, rc.bottom, (float)dpi, true))
        {
            ID2D1RenderTarget* rt = c.rt;
            D2D1_COLOR_F bg = Ui::C(Ui::T.winBg);
            rt->Clear(&bg);
            rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll));
            Build(rt);
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
            float viewH = rc.bottom / S();
            if (contentH > viewH + 1)
            {
                // 细滚动条：只是位置指示，用滚轮滚
                float th = Max(24.f, viewH * viewH / contentH);
                float ty = (viewH - th) * scroll / (contentH - viewH);
                Ui::Fill(rt, D2D1::RectF(Width - 7, ty + 2, Width - 4, ty + th - 2), 1.5f, Ui::T.text3);
            }
        }
        c.EndBlt(dc);
        EndPaint(hwnd, &ps);
        PlaceEdits();
    }

    int HitAt(int px, int py)
    {
        float x = px / S(), y = py / S() + scroll;
        for (int i = hits.n - 1; i >= 0; i--)
        {
            const D2D1_RECT_F& r = hits[i].r;
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return hits[i].id;
        }
        return SNone;
    }

    static Str GetText(HWND h)
    {
        int len = GetWindowTextLengthW(h);
        Str s;
        s.Reserve(len);
        GetWindowTextW(h, s.p, len + 1);
        s.n = WLen(s.p);
        return s;
    }

    void SaveSync()
    {
        Settings& s = g_app->settings;
        Str sv = GetText(edits[0]).Trim();
        while (sv.EndsWith(L"/")) sv.Truncate(sv.n - 1);
        if (sv.n && !sv.StartsWith(L"http")) sv = L"https://" + sv;
        s.syncServer = sv;
        SetWindowTextW(edits[0], sv.c());
        s.syncFolder = SyncNormFolder(GetText(edits[2]));
        SetWindowTextW(edits[2], s.syncFolder.c());
        Str k = GetText(edits[1]).Trim();
        if (k.n) { s.syncKey = k; SetWindowTextW(edits[1], L""); }
        KeyCue();
        Store::SaveSettings(s);
        SyncStart(g_app->sync);
        SyncNow(g_app->sync);
        Invalidate();
    }

    // ---- 录快捷键 ----
    static UINT CurMods()
    {
        UINT m = 0;
        if (GetKeyState(VK_CONTROL) < 0) m |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) < 0) m |= MOD_ALT;
        if (GetKeyState(VK_SHIFT) < 0) m |= MOD_SHIFT;
        if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) m |= MOD_WIN;
        return m;
    }
    void StartRecording()
    {
        recording = true;
        recMods = 0;
        hkMsg = L"";
        UnregisterHotKey(g_app->msgWnd, 1); // 否则按下去会被系统直接截走
        SetFocus(hwnd);
        Invalidate();
    }
    void StopRecording()
    {
        if (!recording) return;
        recording = false;
        const wchar_t* e = g_app->ApplyHotkey();
        hkMsg = e ? e : L"";
        Invalidate();
    }
    void OnRecordKey(UINT vk)
    {
        if (vk == VK_ESCAPE) { StopRecording(); return; }
        UINT mods = CurMods();
        if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN ||
            vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LSHIFT || vk == VK_RSHIFT)
        {
            recMods = mods;
            Invalidate();
            return;
        }
        bool valid = (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || (vk >= VK_F1 && vk <= VK_F24) || vk == VK_SPACE;
        if (!valid || !(mods & (MOD_CONTROL | MOD_ALT)))
        {
            recMods = mods;
            hkMsg = L"要带 Ctrl 或 Alt，再加一个字母、数字或 F 键";
            Invalidate();
            return;
        }
        g_app->settings.hotkey = HotkeyFormat(mods, vk);
        Store::SaveSettings(g_app->settings);
        StopRecording();
    }

    void Activate(int id)
    {
        Settings& s = g_app->settings;
        switch (id)
        {
        case SAuto: if (g_app->isDefaultDir) App::SetAutoStart(!App::IsAutoStart()); break;
        case SHkToggle:
        {
            recording = false;
            s.hotkeyEnabled = !s.hotkeyEnabled;
            Store::SaveSettings(s);
            const wchar_t* e = g_app->ApplyHotkey();
            hkMsg = e ? e : L"";
            break;
        }
        case SHkKeys: if (recording) StopRecording(); else StartRecording(); break;
        case SOpenDir: ShellExecuteW(nullptr, L"open", L"explorer.exe", Store::Dir.c(), nullptr, SW_SHOWNORMAL); break;
        case SOpenBackup:
            EnsureDir(Store::BackupsDir.c());
            ShellExecuteW(nullptr, L"open", L"explorer.exe", Store::BackupsDir.c(), nullptr, SW_SHOWNORMAL);
            break;
        case SSyncToggle: s.syncEnabled = !s.syncEnabled; SaveSync(); break;
        case SSyncSave: SaveSync(); break;
        }
        Invalidate();
    }

    void ApplyTheme()
    {
        Ui::LoadTheme();
        MakeBrushes();
        ApplyFrameTheme(hwnd);
        for (int i = 0; i < 3; i++) { Ui::AllowDarkControls(edits[i]); InvalidateRect(edits[i], nullptr, TRUE); }
        Invalidate();
    }

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        SettingsWin* self = (SettingsWin*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (m == WM_NCCREATE)
        {
            self = (SettingsWin*)((CREATESTRUCTW*)l)->lpCreateParams;
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(h, m, w, l);
        switch (m)
        {
        case WM_PAINT: self->Paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_CTLCOLOREDIT:
        {
            bool f = GetFocus() == (HWND)l;
            u32 bg = f ? Ui::T.inputFocusBg : Ui::T.inputBg;
            SetTextColor((HDC)w, Ui::Ref(Ui::Solid(Ui::T.text, bg)));
            SetBkColor((HDC)w, Ui::Ref(bg));
            return (LRESULT)(f ? self->brushFocus : self->brushRest);
        }
        case WM_MOUSEMOVE:
        {
            if (!self->tracking)
            {
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
                self->tracking = true;
            }
            int id = self->HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (id != self->hover) { self->hover = id; self->Invalidate(); }
            return 0;
        }
        case WM_MOUSELEAVE:
            self->tracking = false;
            if (self->hover) { self->hover = SNone; self->Invalidate(); }
            return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        {
            int id = self->HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (self->recording && id != SHkKeys) self->StopRecording();
            if (GetFocus() != h) SetFocus(h); // 点空白处：输入框失去焦点
            self->press = id;
            if (id) SetCapture(h);
            self->Invalidate();
            return 0;
        }
        case WM_LBUTTONUP:
        {
            int p = self->press;
            self->press = SNone;
            if (GetCapture() == h) ReleaseCapture();
            if (p && self->HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l)) == p) self->Activate(p);
            self->Invalidate();
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            float old = self->scroll;
            self->scroll -= GET_WHEEL_DELTA_WPARAM(w) / 120.f * 60;
            self->ClampScroll();
            if (self->scroll != old) { self->Invalidate(); UpdateWindow(h); }
            return 0;
        }
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            if (self->recording) { self->OnRecordKey((UINT)w); return 0; }
            break;
        case WM_KEYUP: case WM_SYSKEYUP:
            if (self->recording) { self->recMods = CurMods(); self->Invalidate(); return 0; }
            break;
        case WM_SYSCHAR: case WM_CHAR:
            if (self->recording) return 0;
            break;
        case WM_SYSCOMMAND:
            if (self->recording && (w & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_KILLFOCUS:
            self->StopRecording();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE) self->StopRecording();
            break;
        case WM_COMMAND:
        {
            int id = LOWORD(w), code = HIWORD(w);
            if (id >= 100 && id < 103 && (code == EN_SETFOCUS || code == EN_KILLFOCUS))
            {
                self->Invalidate();
                InvalidateRect((HWND)l, nullptr, TRUE);
                return 0;
            }
            if (id == IDOK) { HWND f = GetFocus(); for (int i = 0; i < 3; i++) if (f == self->edits[i]) self->SaveSync(); return 0; }
            if (id == IDCANCEL) { DestroyWindow(h); return 0; }
            return 0;
        }
        case WM_SIZE:
            self->ClampScroll();
            self->PlaceEdits();
            return 0;
        case WM_DPICHANGED:
        {
            self->dpi = HIWORD(w);
            self->MakeFont();
            RECT* rc = (RECT*)l;
            SetWindowPos(h, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            self->ClampScroll();
            self->PlaceEdits();
            self->Invalidate();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (l && WEq((const wchar_t*)l, L"ImmersiveColorSet")) self->ApplyTheme();
            break;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY:
            g_app->ApplyHotkey();
            g_app->settingsWnd = nullptr;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            if (self->editFont) DeleteObject(self->editFont);
            if (self->brushRest) DeleteObject(self->brushRest);
            if (self->brushFocus) DeleteObject(self->brushFocus);
            delete self;
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

HWND SettingsWindowCreate()
{
    Ui::LoadTheme();
    SettingsWin* d = new SettingsWin();
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, L"DeskNoteSettings", L"DeskNote 设置", WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, g_inst, d);
    d->dpi = GetDpiForWindow(h);
    SetIcons(h);
    ApplyFrameTheme(h);
    d->Init();
    d->Build(nullptr);
    // 放到鼠标所在屏幕中间；内容太高时窗口按屏幕高度截短，用滚轮滚
    POINT p;
    GetCursorPos(&p);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
    float S = d->S();
    int maxH = (int)((mi.rcWork.bottom - mi.rcWork.top) * 0.9f);
    RECT r = { 0, 0, (int)(SettingsWin::Width * S + 0.5f), Min((int)(d->contentH * S + 0.5f), maxH - (int)(40 * S)) };
    AdjustWindowRectExForDpi(&r, GetWindowLongW(h, GWL_STYLE), FALSE, GetWindowLongW(h, GWL_EXSTYLE), d->dpi);
    int w = r.right - r.left, ht = r.bottom - r.top;
    SetWindowPos(h, nullptr, (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - ht) / 2, w, ht, SWP_NOZORDER | SWP_NOACTIVATE);
    d->ClampScroll();
    d->PlaceEdits();
    return h;
}

void SettingsWindowSyncStatus(HWND w)
{
    SettingsWin* d = (SettingsWin*)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (d) d->Invalidate();
}

bool SettingsWindowWantsKeys(HWND w)
{
    SettingsWin* d = (SettingsWin*)GetWindowLongPtrW(w, GWLP_USERDATA);
    return d && d->recording;
}

// =====================================================================
// 模态对话框的消息循环
// =====================================================================
static void RunModal(HWND dlg, HWND owner, bool* done)
{
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    MSG msg;
    while (!*done && GetMessageW(&msg, nullptr, 0, 0))
    {
        if (IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
}

// =====================================================================
// 设置提醒：常用时间一键选；或者自己定日期和时间（自绘，和设置页一个样式）
// =====================================================================
static void DrawBtn(ID2D1RenderTarget* rt, D2D1_RECT_F r, const Str& text, wchar_t icon, int kind, bool hot, bool down, u32 under)
{
    // kind：0 普通按钮，1 强调色按钮，2 无底色的图标/文字按钮
    const Ui::Theme& T = Ui::T;
    u32 fg = T.text;
    if (kind == 1)
    {
        Ui::Fill(rt, r, 4, down ? (T.accent & 0x00FFFFFF) | 0xCC000000 : hot ? (T.accent & 0x00FFFFFF) | 0xE6000000 : T.accent);
        fg = down ? (T.onAccent & 0x00FFFFFF) | 0x80000000 : T.onAccent;
    }
    else if (kind == 0)
    {
        Ui::Fill(rt, r, 4, Ui::Solid(down ? T.ctrlPress : hot ? T.ctrlHover : T.ctrlFill, under));
        Ui::Stroke(rt, r, 4, T.ctrlBorder, 1);
        if (!T.dark) Ui::Line(rt, r.left + 4, r.bottom - 0.5f, r.right - 4, r.bottom - 0.5f, T.ctrlBorderBottom, 1);
        if (down) fg = T.text2;
    }
    else
    {
        if (hot) Ui::Fill(rt, r, 4, down ? T.subtlePress : T.subtleHover);
        fg = T.text2;
    }
    if (icon) Ui::Icon(rt, icon, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 14, fg);
    else Ui::Text(rt, text, r, 14, 400, fg, Ui::Center);
}

struct ReminderWin
{
    HWND hwnd = nullptr;
    int dpi = 96;
    float S() const { return dpi / 96.f; }
    bool done = false;
    int result = 0;           // 0 取消，1 设定，2 移除
    Time resultTime = 0;
    Str itemText;
    bool hasCurrent = false;
    Time quick[4];
    const wchar_t* quickLabel[4];
    int quickN = 0;
    Time day = 0;             // 自定义：哪一天（零点）
    HWND timeEdit = nullptr;
    HFONT editFont = nullptr;
    int editH = 0;
    HBRUSH brushRest = nullptr, brushFocus = nullptr;
    D2D1_RECT_F timeFrame = {}, dateRow = {}, timeRow = {};
    struct Hit { int id; D2D1_RECT_F r; };
    Vec<Hit> hits;
    int hover = 0, press = 0;
    bool tracking = false;
    float contentH = 0;
    static const int Width = 420;
    enum { HQuick = 1, HPrevDay = 10, HNextDay, HMinus, HPlus, HOk, HCancel, HRemove };

    void Invalidate() { InvalidateRect(hwnd, nullptr, FALSE); }

    void Init(const Str& text, bool has, Time current)
    {
        Str shown = text.Replace(L"\r", L"").Replace(L"\n", L" ").Trim();
        if (shown.n > 80) { shown.Truncate(80); shown += L"…"; }
        itemText = shown;
        hasCurrent = has;
        SYSTEMTIME now;
        GetLocalTime(&now);
        Time t0 = Now();
        Time minuteNow = MakeTime(now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, 0);
        quick[quickN] = minuteNow + 30 * TicksPerMinute; quickLabel[quickN++] = L"30 分钟后";
        quick[quickN] = minuteNow + 60 * TicksPerMinute; quickLabel[quickN++] = L"1 小时后";
        Time tonight = Today() + 20 * 60 * TicksPerMinute;
        if (tonight > t0 + 5 * TicksPerMinute) { quick[quickN] = tonight; quickLabel[quickN++] = L"今晚"; }
        quick[quickN] = Today() + TicksPerDay + 9 * 60 * TicksPerMinute; quickLabel[quickN++] = L"明早";
        // 自定义的初始值：已有提醒就用它，否则一小时后（取整到 5 分钟）
        Time init = has ? current : minuteNow + 60 * TicksPerMinute;
        SYSTEMTIME st;
        SplitTime(init, &st);
        int mins = st.wHour * 60 + st.wMinute;
        if (!has) mins = (mins + 4) / 5 * 5;
        day = MakeTime(st.wYear, st.wMonth, st.wDay, 0, 0, 0);
        if (mins >= 24 * 60) { mins -= 24 * 60; day += TicksPerDay; }
        brushRest = CreateSolidBrush(Ui::Ref(Ui::T.inputBg));
        brushFocus = CreateSolidBrush(Ui::Ref(Ui::T.inputFocusBg));
        timeEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
                                   (HMENU)100, g_inst, nullptr);
        Ui::AllowDarkControls(timeEdit);
        ImmAssociateContextEx(timeEdit, nullptr, 0); // 只输数字，不要输入法（中文标点会把冒号变成全角）
        SendMessageW(timeEdit, EM_LIMITTEXT, 5, 0);
        editFont = CreateFontW(-(int)(15 * S() + 0.5f), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SendMessageW(timeEdit, WM_SETFONT, (WPARAM)editFont, FALSE);
        HDC dc = GetDC(hwnd);
        HGDIOBJ of = SelectObject(dc, editFont);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        editH = tm.tmHeight;
        SelectObject(dc, of);
        ReleaseDC(hwnd, dc);
        SetMinutes(mins);
    }

    // 时间输入框：接受 15:00、15：00、15.00、1500、9
    int Minutes()
    {
        wchar_t buf[16] = {};
        GetWindowTextW(timeEdit, buf, 16);
        if (!buf[0]) return -1;
        int h = -1, m = 0, digits = 0, val = 0, part = 0;
        for (int i = 0;; i++)
        {
            wchar_t c = buf[i];
            if (c >= L'0' && c <= L'9') { val = val * 10 + (c - L'0'); digits++; continue; }
            if (c == L':' || c == 0xFF1A || c == L'.' || c == L' ' || c == 0)
            {
                if (part == 0) { if (c == 0 && digits >= 3) { h = val / 100; m = val % 100; } else h = val; }
                else if (part == 1) m = val;
                part++;
                val = 0;
                digits = 0;
                if (c == 0) break;
                continue;
            }
            return -1;
        }
        if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
        return h * 60 + m;
    }
    void SetMinutes(int mins)
    {
        Str s;
        s.Pad2(mins / 60).Ch(L':').Pad2(mins % 60);
        SetWindowTextW(timeEdit, s.c());
    }
    void Step(int delta)
    {
        // 对齐到整 15 分钟：往后取下整，往前取上整（14:07 → 14:15 / 14:00）
        int m = Minutes();
        if (m < 0) m = 9 * 60;
        m += delta;
        if (m < 0) { m += 24 * 60; day -= TicksPerDay; }
        m = delta > 0 ? m / 15 * 15 : (m + 14) / 15 * 15;
        if (m >= 24 * 60) { m -= 24 * 60; day += TicksPerDay; }
        SetMinutes(m);
        Invalidate();
    }

    static Str DayText(Time d)
    {
        static const wchar_t* wk[7] = { L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六" };
        SYSTEMTIME st;
        SplitTime(d, &st);
        Time today = Today();
        Str s;
        if (d == today) s = L"今天 ";
        else if (d == today + TicksPerDay) s = L"明天 ";
        else if (d == today + 2 * TicksPerDay) s = L"后天 ";
        SYSTEMTIME now;
        GetLocalTime(&now);
        if (st.wYear != now.wYear) s.Int(st.wYear).Append(L"年");
        s.Int(st.wMonth).Append(L"月").Int(st.wDay).Append(L"日 ").Append(wk[st.wDayOfWeek % 7]);
        return s;
    }
    static Str WhenText(Time t)
    {
        SYSTEMTIME st;
        SplitTime(t, &st);
        Time d = MakeTime(st.wYear, st.wMonth, st.wDay, 0, 0, 0);
        Str s;
        if (d == Today() + TicksPerDay) s = L"明天 ";
        s.Pad2(st.wHour).Ch(L':').Pad2(st.wMinute);
        return s;
    }

    void Push(int id, D2D1_RECT_F r) { Hit h = { id, r }; hits.Push(h); }

    void Build(ID2D1RenderTarget* rt)
    {
        hits.Clear();
        const Ui::Theme& T = Ui::T;
        const float L = 24, R = Width - 24, CW = R - L;
        float y = 20;
        // 条目内容
        Str quoted = L"「" + itemText + L"」";
        float th = Ui::MeasureWrap(quoted, CW, 14, 600);
        if (rt) Ui::TextWrap(rt, quoted, L, y, CW, 14, 600, T.text);
        y += th + 18;

        // 快捷选项：两列，每格上面是说法、下面是具体时间
        if (rt) Ui::Text(rt, Str(L"快速选择"), D2D1::RectF(L, y, R, y + 18), 12, 400, T.text2);
        y += 18 + 8;
        float qw = (CW - 8) / 2, qh = 52;
        for (int i = 0; i < quickN; i++)
        {
            float x = L + (i % 2) * (qw + 8), qy = y + (i / 2) * (qh + 8);
            D2D1_RECT_F r = D2D1::RectF(x, qy, x + qw, qy + qh);
            Push(HQuick + i, r);
            if (!rt) continue;
            bool hot = hover == HQuick + i, down = press == HQuick + i;
            Ui::Fill(rt, r, 6, Ui::Solid(down ? T.ctrlPress : hot ? T.ctrlHover : T.ctrlFill, T.winBg));
            Ui::Stroke(rt, r, 6, hot ? (T.accent & 0x00FFFFFF) | 0x80000000 : T.ctrlBorder, 1);
            Ui::Text(rt, Str(quickLabel[i]), D2D1::RectF(x + 14, qy + 7, x + qw - 14, qy + 27), 14, 400, T.text);
            Ui::Text(rt, WhenText(quick[i]), D2D1::RectF(x + 14, qy + 27, x + qw - 14, qy + 45), 12, 400, T.text2);
        }
        y += ((quickN + 1) / 2) * (qh + 8) + 10;

        // 自定义：日期（前一天 / 后一天）+ 时间（输入框，±15 分钟）
        if (rt) Ui::Text(rt, Str(L"自定义"), D2D1::RectF(L, y, R, y + 18), 12, 400, T.text2);
        y += 18 + 8;
        D2D1_RECT_F card = D2D1::RectF(L, y, R, y + 56);
        if (rt) { Ui::Fill(rt, card, 6, T.cardBg); Ui::Stroke(rt, card, 6, T.cardBorder, 1); }
        float cy = y + 28;
        // 时间部分在右边
        D2D1_RECT_F plus = D2D1::RectF(R - 10 - 32, cy - 16, R - 10, cy + 16);
        timeFrame = D2D1::RectF(plus.left - 4 - 72, cy - 16, plus.left - 4, cy + 16);
        D2D1_RECT_F minus = D2D1::RectF(timeFrame.left - 4 - 32, cy - 16, timeFrame.left - 4, cy + 16);
        timeRow = D2D1::RectF(minus.left, card.top, R, card.bottom);
        // 日期部分在左边
        D2D1_RECT_F prev = D2D1::RectF(L + 10, cy - 16, L + 10 + 32, cy + 16);
        D2D1_RECT_F next = D2D1::RectF(minus.left - 16 - 32, cy - 16, minus.left - 16, cy + 16);
        dateRow = D2D1::RectF(L, card.top, next.right, card.bottom);
        bool canPrev = day > Today();
        if (canPrev) Push(HPrevDay, prev);
        Push(HNextDay, next);
        Push(HMinus, minus);
        Push(HPlus, plus);
        if (rt)
        {
            if (canPrev) DrawBtn(rt, prev, Str(), 0xE76B, 2, hover == HPrevDay, press == HPrevDay, T.cardBg);
            else Ui::Icon(rt, 0xE76B, (prev.left + prev.right) / 2, cy, 14, T.disabled);
            DrawBtn(rt, next, Str(), 0xE76C, 2, hover == HNextDay, press == HNextDay, T.cardBg);
            Ui::Text(rt, DayText(day), D2D1::RectF(prev.right + 4, cy - 12, next.left - 4, cy + 12), 14, 400, T.text, Ui::Center);
            DrawBtn(rt, minus, Str(), 0xE738, 2, hover == HMinus, press == HMinus, T.cardBg);
            DrawBtn(rt, plus, Str(), 0xE710, 2, hover == HPlus, press == HPlus, T.cardBg);
            bool f = GetFocus() == timeEdit;
            Ui::Fill(rt, timeFrame, 4, f ? T.inputFocusBg : T.inputBg);
            Ui::Stroke(rt, timeFrame, 4, T.ctrlBorder, 1);
            if (f) Ui::Fill(rt, D2D1::RectF(timeFrame.left + 1, timeFrame.bottom - 2, timeFrame.right - 1, timeFrame.bottom), 1, T.accent);
            else Ui::Line(rt, timeFrame.left + 3, timeFrame.bottom - 0.5f, timeFrame.right - 3, timeFrame.bottom - 0.5f, T.strong, 1);
        }
        y += 56 + 8;
        // 状态：时间写错了 / 已经过了
        int mins = Minutes();
        Str status;
        u32 sc = T.text2;
        if (mins < 0) { status = L"时间格式不对，例如 15:30"; sc = T.critical; }
        else if (day + mins * TicksPerMinute <= Now()) { status = L"这个时间已经过了，确定后会马上提醒"; sc = T.critical; }
        else status = L"到点后弹出系统通知，条目上的提醒标签变红；程序没开着时错过的提醒，下次打开时补上。";
        float sh = Ui::MeasureWrap(status, CW, 12, 400);
        if (rt) Ui::TextWrap(rt, status, L, y, CW, 12, 400, sc);
        y += sh + 20;

        // 底部按钮
        float bw = 88;
        D2D1_RECT_F ok = D2D1::RectF(R - bw, y, R, y + 32), cancel = D2D1::RectF(R - 2 * bw - 8, y, R - bw - 8, y + 32);
        if (mins >= 0) Push(HOk, ok);
        Push(HCancel, cancel);
        Str removeText(L"移除提醒");
        D2D1_RECT_F remove = D2D1::RectF(L - 8, y, L - 8 + Ui::Measure(removeText, 14, 400) + 16, y + 32);
        if (hasCurrent) Push(HRemove, remove);
        if (rt)
        {
            if (mins >= 0) DrawBtn(rt, ok, Str(L"确定"), 0, 1, hover == HOk, press == HOk, T.winBg);
            else { Ui::Fill(rt, ok, 4, T.disabled); Ui::Text(rt, Str(L"确定"), ok, 14, 400, T.onAccent, Ui::Center); }
            DrawBtn(rt, cancel, Str(L"取消"), 0, 0, hover == HCancel, press == HCancel, T.winBg);
            if (hasCurrent)
            {
                if (hover == HRemove) Ui::Fill(rt, remove, 4, press == HRemove ? T.subtlePress : T.subtleHover);
                Ui::Text(rt, removeText, remove, 14, 400, T.critical, Ui::Center);
            }
        }
        y += 32 + 20;
        contentH = y;
    }

    void PlaceEdit()
    {
        float s = S();
        int x = (int)((timeFrame.left + 6) * s), w = (int)((timeFrame.right - 6) * s) - x;
        int cy = (int)((timeFrame.top + timeFrame.bottom) / 2 * s);
        MoveWindow(timeEdit, x, cy - editH / 2, w, editH, TRUE);
    }

    void Paint()
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        Canvas c;
        if (c.Begin(rc.right, rc.bottom, (float)dpi, true))
        {
            D2D1_COLOR_F bg = Ui::C(Ui::T.winBg);
            c.rt->Clear(&bg);
            Build(c.rt);
        }
        c.EndBlt(dc);
        EndPaint(hwnd, &ps);
    }

    int HitAt(int px, int py)
    {
        float x = px / S(), y = py / S();
        for (int i = hits.n - 1; i >= 0; i--)
        {
            const D2D1_RECT_F& r = hits[i].r;
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return hits[i].id;
        }
        return 0;
    }

    void Activate(int id)
    {
        if (id >= HQuick && id < HQuick + quickN) { result = 1; resultTime = quick[id - HQuick]; done = true; return; }
        switch (id)
        {
        case HPrevDay: if (day > Today()) day -= TicksPerDay; break;
        case HNextDay: day += TicksPerDay; break;
        case HMinus: Step(-15); break;
        case HPlus: Step(15); break;
        case HOk:
        {
            int m = Minutes();
            if (m < 0) { SetFocus(timeEdit); break; }
            result = 1;
            resultTime = day + m * TicksPerMinute;
            done = true;
            break;
        }
        case HCancel: done = true; break;
        case HRemove: result = 2; done = true; break;
        }
        Invalidate();
    }

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        ReminderWin* self = (ReminderWin*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (m == WM_NCCREATE)
        {
            self = (ReminderWin*)((CREATESTRUCTW*)l)->lpCreateParams;
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(h, m, w, l);
        switch (m)
        {
        case WM_PAINT: self->Paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_CTLCOLOREDIT:
        {
            bool f = GetFocus() == (HWND)l;
            u32 bg = f ? Ui::T.inputFocusBg : Ui::T.inputBg;
            SetTextColor((HDC)w, Ui::Ref(Ui::Solid(Ui::T.text, bg)));
            SetBkColor((HDC)w, Ui::Ref(bg));
            return (LRESULT)(f ? self->brushFocus : self->brushRest);
        }
        case WM_MOUSEMOVE:
        {
            if (!self->tracking)
            {
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
                self->tracking = true;
            }
            int id = self->HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (id != self->hover) { self->hover = id; self->Invalidate(); }
            return 0;
        }
        case WM_MOUSELEAVE:
            self->tracking = false;
            self->hover = 0;
            self->Invalidate();
            return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
            self->press = self->HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (self->press) SetCapture(h);
            self->Invalidate();
            return 0;
        case WM_LBUTTONUP:
        {
            int p = self->press;
            self->press = 0;
            if (GetCapture() == h) ReleaseCapture();
            if (p && self->HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l)) == p) self->Activate(p);
            self->Invalidate();
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            // 滚轮：在日期上换天，在时间上 ±15 分钟
            POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ScreenToClient(h, &p);
            float x = p.x / self->S(), y = p.y / self->S();
            int dir = GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1;
            const D2D1_RECT_F& dr = self->dateRow;
            const D2D1_RECT_F& tr = self->timeRow;
            if (x >= dr.left && x < dr.right && y >= dr.top && y < dr.bottom)
            {
                if (dir > 0) self->day += TicksPerDay;
                else if (self->day > Today()) self->day -= TicksPerDay;
                self->Invalidate();
            }
            else if (x >= tr.left && x < tr.right && y >= tr.top && y < tr.bottom) self->Step(dir * 15);
            return 0;
        }
        case WM_COMMAND:
        {
            int id = LOWORD(w), code = HIWORD(w);
            if (id == 100 && (code == EN_SETFOCUS || code == EN_KILLFOCUS || code == EN_CHANGE))
            {
                self->Invalidate();
                if (code != EN_CHANGE) InvalidateRect((HWND)l, nullptr, TRUE);
                return 0;
            }
            if (id == IDOK) self->Activate(HOk);
            else if (id == IDCANCEL) self->done = true;
            return 0;
        }
        case WM_DPICHANGED:
        {
            self->dpi = HIWORD(w);
            RECT* rc = (RECT*)l;
            SetWindowPos(h, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            self->PlaceEdit();
            self->Invalidate();
            return 0;
        }
        case WM_CLOSE: self->done = true; return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

int ReminderDialog(HWND owner, const Str& itemText, bool hasCurrent, Time current, Time* result)
{
    Ui::LoadTheme();
    ReminderWin d;
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"DeskNoteReminder", L"设置提醒", WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, owner, nullptr, g_inst, &d);
    d.dpi = GetDpiForWindow(h);
    ApplyFrameTheme(h);
    d.Init(itemText, hasCurrent, current);
    d.Build(nullptr);
    // 放在鼠标所在屏幕的中间
    POINT p;
    GetCursorPos(&p);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
    float S = d.S();
    RECT r = { 0, 0, (int)(ReminderWin::Width * S + 0.5f), (int)(d.contentH * S + 0.5f) };
    AdjustWindowRectExForDpi(&r, GetWindowLongW(h, GWL_STYLE), FALSE, GetWindowLongW(h, GWL_EXSTYLE), d.dpi);
    int w = r.right - r.left, ht = r.bottom - r.top;
    SetWindowPos(h, nullptr, (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - ht) / 2, w, ht, SWP_NOZORDER | SWP_NOACTIVATE);
    d.PlaceEdit();
    RunModal(h, owner, &d.done);
    if (d.editFont) DeleteObject(d.editFont);
    if (d.brushRest) DeleteObject(d.brushRest);
    if (d.brushFocus) DeleteObject(d.brushFocus);
    if (d.result == 1) *result = d.resultTime;
    return d.result;
}

void RegisterDialogClasses()
{
    WNDCLASSW wc = {};
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.style = CS_DBLCLKS;
    wc.hbrBackground = nullptr;
    wc.lpfnWndProc = SettingsWin::Proc; wc.lpszClassName = L"DeskNoteSettings"; RegisterClassW(&wc);
    wc.lpfnWndProc = TrashWin::Proc; wc.lpszClassName = L"DeskNoteTrash"; RegisterClassW(&wc);
    wc.lpfnWndProc = ReminderWin::Proc; wc.lpszClassName = L"DeskNoteReminder"; RegisterClassW(&wc);
}
