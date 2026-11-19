// 回收站、设置、提醒对话框（都是自绘，跟随系统深浅色）
#include "app.h"
#include "menu.h"
#include "shell.h"

// =====================================================================
// 快捷键文字 <-> 修饰键 + 按键
// =====================================================================
bool HotkeyParse(const Str& s, int* mods, int* vk)
{
    *mods = 0;
    *vk = 0;
    int start = 0;
    for (int i = 0; i <= s.n; i++)
    {
        if (i < s.n && s.p[i] != L'+') continue;
        Str p = s.Sub(start, i - start).Trim();
        start = i + 1;
        if (p.EqI(L"Ctrl")) *mods |= ModCtrl;
        else if (p.EqI(L"Alt")) *mods |= ModAlt;
        else if (p.EqI(L"Shift")) *mods |= ModShift;
        else if (p.EqI(L"Win")) *mods |= ModWin;
        else if (p.n == 1 && ((p.p[0] >= L'A' && p.p[0] <= L'Z') || (p.p[0] >= L'a' && p.p[0] <= L'z') || (p.p[0] >= L'0' && p.p[0] <= L'9')))
            *vk = p.p[0] >= L'a' ? p.p[0] - 32 : p.p[0];
        else if (p.n == 2 && (p.p[0] == L'D' || p.p[0] == L'd') && p.p[1] >= L'0' && p.p[1] <= L'9') *vk = p.p[1];
        else if (p.n >= 2 && (p.p[0] == L'F' || p.p[0] == L'f'))
        {
            i64 k = ParseInt(p.Sub(1), 0);
            if (k >= 1 && k <= 24) *vk = KeyF1 + (int)k - 1; else return false;
        }
        else if (p.EqI(L"Space")) *vk = KeySpace;
        else return false;
    }
    return *vk != 0 && *mods != 0;
}

Str HotkeyFormat(int mods, int vk)
{
    Str s;
    if (mods & ModCtrl) s += L"Ctrl+";
    if (mods & ModAlt) s += L"Alt+";
    if (mods & ModShift) s += L"Shift+";
    if (mods & ModWin) s += L"Win+";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) s.Ch((wchar_t)vk);
    else if (vk >= KeyF1 && vk <= KeyF24) { s.Ch(L'F'); s.Int(vk - KeyF1 + 1); }
    else if (vk == KeySpace) s += L"Space";
    else { s += L"VK"; s.Int(vk); }
    return s;
}

// =====================================================================
// 回收站：按删除日期分组的卡片列表，每行带“恢复”和“彻底删除”；不自动清理
// =====================================================================

struct TrashWin : WinHandler
{
    Window* win = nullptr;
    struct Hit { int id; RectF r; TrashEntry* e; };
    Vec<Hit> hits;            // 列表里的区域是内容坐标（要加滚动），顶栏是窗口坐标
    int hover = 0, press = 0;
    TrashEntry* hoverEntry = nullptr;
    float scroll = 0, contentH = 0, listTop = 0;
    enum { HEmpty = 1, HRow = 2, HRestore = 3, HPurge = 4 };

    void Invalidate() { Wnd::Invalidate(win); }
    float ClientW() { float w; Wnd::ClientSize(win, &w, nullptr); return w; }
    float ClientH() { float h; Wnd::ClientSize(win, nullptr, &h); return h; }
    void ClampScroll() { scroll = Clamp(scroll, 0.f, Max(0.f, contentH - (ClientH() - listTop))); }

    static Str DayLabel(Time t)
    {
        DateTime st;
        SplitTime(t, &st);
        Time day = MakeTime(st.year, st.month, st.day, 0, 0, 0);
        Time today = Today();
        if (day == today) return Str(L"今天");
        if (day == today - TicksPerDay) return Str(L"昨天");
        DateTime now;
        LocalNow(&now);
        Str s;
        if (st.year != now.year) s.Int(st.year).Append(L"年");
        s.Int(st.month).Append(L"月").Int(st.day).Append(L"日");
        return s;
    }
    static Time DayOf(Time t)
    {
        DateTime st;
        SplitTime(t, &st);
        return MakeTime(st.year, st.month, st.day, 0, 0, 0);
    }

    // 普通按钮 / 图标按钮
    void DrawButton(Painter* rt, RectF r, const wchar_t* text, wchar_t icon, bool hot, bool down, bool enabled, bool danger)
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

    void Build(Painter* rt)
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
        RectF eb = RectF(R - bw, y, R, y + 32);
        if (list.n) { Hit h = { HEmpty, eb, nullptr }; hits.Push(h); }
        if (rt)
        {
            float tw = Ui::Text(rt, Str(L"回收站"), RectF(L, y, R - bw - 16, y + 32), 20, 600, T.text);
            if (count.n) Ui::Text(rt, count, RectF(L + tw + 10, y + 4, R - bw - 16, y + 32), 14, 400, T.text2);
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
                Ui::Text(rt, Str(L"回收站是空的"), RectF(L, cy + 6, R, cy + 30), 14, 400, T.text2, Ui::Center);
                Ui::Text(rt, Str(L"删除的条目和便签会先放在这里"), RectF(L, cy + 30, R, cy + 50), 12, 400, T.text3, Ui::Center);
            }
            contentH = 0;
            return;
        }
        if (rt)
        {
            Gfx::PushClip(rt, RectF(0, listTop, W, H));
            Gfx::SetOffset(rt, 0, -scroll);
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
                if (rt) Ui::Text(rt, DayLabel(e->deletedAt), RectF(L, ly, R, ly + 20), 14, 600, T.text);
                ly += 20 + 8;
                lastDay = day;
            }
            RectF card = RectF(L, ly, R, ly + rowH);
            float cy = ly + rowH / 2;
            RectF purge = RectF(R - 12 - 32, cy - 16, R - 12, cy + 16);
            Str restoreText(L"恢复");
            float rw = Max(72.f, Ui::Measure(restoreText, 14, 400) + 24);
            RectF restore = RectF(purge.left - 8 - rw, cy - 16, purge.left - 8, cy + 16);
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
                DateTime st;
                SplitTime(e->deletedAt, &st);
                sub += L" · ";
                sub.Pad2(st.hour).Ch(L':').Pad2(st.minute).Append(L" 删除");
                float textL = L + 16 + 20 + 16, textR = restore.left - 16;
                Ui::Text(rt, main, RectF(textL, cy - 19, textR, cy + 1), 14, 400, T.text);
                Ui::Text(rt, sub, RectF(textL, cy + 1, textR, cy + 18), 12, 400, T.text2);
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
            Gfx::SetOffset(rt, 0, 0);
            Gfx::PopClip(rt);
            float viewH = H - listTop;
            if (contentH > viewH + 1)
            {
                float th = Max(24.f, viewH * viewH / contentH);
                float ty = listTop + (viewH - th) * Clamp(scroll / (contentH - viewH), 0.f, 1.f);
                Ui::Fill(rt, RectF(W - 7, ty + 2, W - 4, ty + th - 2), 1.5f, T.text3);
            }
        }
    }

    void Paint(Window*, Painter* rt, float, float) override
    {
        Gfx::Clear(rt, Ui::T.winBg);
        Build(rt);
    }

    // 返回命中区域的 id，并给出对应条目
    int HitAt(float x, float y, TrashEntry** e)
    {
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
            if (Wnd::Confirm(win, Str(L"清空回收站"), msg)) g_app->EmptyTrash();
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

    bool OnEvent(Window* w, const Event& ev) override
    {
        TrashEntry* e;
        switch (ev.type)
        {
        case EvResize: Build(nullptr); ClampScroll(); Invalidate(); return true;
        case EvMouseMove:
        {
            int id = HitAt(ev.x, ev.y, &e);
            if (id != hover || e != hoverEntry) { hover = id; hoverEntry = e; Invalidate(); }
            return true;
        }
        case EvMouseLeave:
            hover = 0;
            hoverEntry = nullptr;
            Invalidate();
            return true;
        case EvMouseDown:
            if (ev.button != MouseLeft) return false;
            if (ev.clicks == 2)
            {
                if (HitAt(ev.x, ev.y, &e) == HRow) Activate(HRestore, e);
                return true;
            }
            press = HitAt(ev.x, ev.y, &e);
            if (press) Wnd::Capture(w, true);
            Invalidate();
            return true;
        case EvMouseUp:
        {
            if (ev.button != MouseLeft) return false;
            int p = press;
            press = 0;
            Wnd::Capture(w, false);
            int id = HitAt(ev.x, ev.y, &e);
            if (p && id == p && p != HRow) Activate(p, e);
            Invalidate();
            return true;
        }
        case EvWheel:
        {
            float old = scroll;
            scroll -= ev.wheel * 66;
            ClampScroll();
            if (scroll != old)
            {
                Build(nullptr);
                hover = HitAt(ev.x, ev.y, &e);
                hoverEntry = e;
                Invalidate();
            }
            return true;
        }
        case EvScale: case EvTheme: Invalidate(); return true;
        case EvDestroy:
            g_app->trashWnd = nullptr;
            delete this;
            return true;
        }
        return false;
    }
};

Window* TrashWindowCreate()
{
    TrashWin* d = new TrashWin();
    WinDesc desc;
    desc.title = L"DeskNote 回收站";
    desc.minW = 440;
    desc.minH = 320;
    d->win = Wnd::NewDialog(desc, d);
    if (!d->win) { delete d; return nullptr; }
    Wnd::PlaceCentered(d->win, 640, 580, 0.85f, 0);
    d->Build(nullptr);
    return d->win;
}

void TrashWindowRefresh(Window* w)
{
    TrashWin* d = (TrashWin*)Wnd::Handler(w);
    if (d) d->Refresh();
}

// =====================================================================
// 设置：仿 Win11 设置页（分组标题 + 卡片 + 开关），整页自绘，只有三个输入框用系统控件
// =====================================================================
enum { SNone, SAuto, SHkToggle, SHkKeys, SOpenDir, SOpenBackup, SSyncToggle, SSyncSave, SOpenGopan, SLinkRepo, SLinkGopanRepo, SLinkGopanWeb, SQkToggle, SQkKeys };
struct HitArea { int id; RectF r; };

struct SettingsWin : WinHandler
{
    static const int Width = 600;
    Window* win = nullptr;
    TextField* edits[3] = {};         // 服务器、密钥、文件夹
    RectF frames[3];                  // 输入框外框（DIP，内容坐标）
    Vec<HitArea> hits;
    int hover = SNone, press = SNone;
    float scroll = 0, contentH = 0;
    bool recording = false;
    int recMods = 0;
    int recWhich = 0;                 // 正在录哪个：0 提到最前，1 快速添加
    Str hkMsg[2];                     // 快捷键出错时的提示

    void Invalidate() { Wnd::Invalidate(win); }

    void KeyCue() { Wnd::FieldSetCue(edits[1], Str(g_app->settings.syncKey.n ? L"已保存（不改就留空）" : L"粘贴 gopan 的密钥")); }

    void Init()
    {
        Settings& s = g_app->settings;
        Str init[3] = { s.syncServer, Str(), s.syncFolder };
        for (int i = 0; i < 3; i++) edits[i] = Wnd::CreateField(win, init[i], i == 1 ? Wnd::FieldPassword : 0);
        Wnd::FieldSetCue(edits[0], Str(DefaultSyncServer));
        Wnd::FieldSetCue(edits[2], Str(L"DeskNote"));
        KeyCue();
        for (int i = 0; i < 2; i++)
        {
            const wchar_t* e = g_app->ApplyHotkey(HkId(i));
            hkMsg[i] = e ? e : L"";
        }
    }

    static int HkId(int which) { return which ? App::HotkeyQuickAdd : App::HotkeyPeek; }
    static Str& HkText(int which) { return which ? g_app->settings.quickAddHotkey : g_app->settings.hotkey; }

    // ---- 画控件 ----
    void Toggle(Painter* rt, int id, bool on, bool enabled, float right, float cy)
    {
        RectF tr = RectF(right - 40, cy - 10, right, cy + 10);
        if (enabled) { HitArea a = { id, RectF(tr.left - 44, cy - 16, tr.right, cy + 16) }; hits.Push(a); }
        if (!rt) return;
        const Ui::Theme& T = Ui::T;
        bool hot = enabled && hover == id, down = enabled && press == id;
        Ui::Text(rt, Str(on ? L"开" : L"关"), RectF(tr.left - 44, cy - 12, tr.left - 12, cy + 12), 14, 400, enabled ? T.text : T.disabled, Ui::Right);
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

    float Button(Painter* rt, int id, const wchar_t* text, float right, float cy, bool accent)
    {
        Str t(text);
        float w = Max(88.f, Ui::Measure(t, 14, 400) + 24);
        RectF r = RectF(right - w, cy - 16, right, cy + 16);
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

    // 文字链接（强调色，后面跟一个“新窗口打开”的小图标，悬停加下划线）；x 是左边，返回宽度
    float Link(Painter* rt, int id, const wchar_t* text, float x, float cy)
    {
        Str t(text);
        float tw = Ui::Measure(t, 14, 400);
        float w = tw + 6 + 12;
        RectF r = RectF(x - 4, cy - 14, x + w + 4, cy + 14);
        HitArea a = { id, r };
        hits.Push(a);
        if (!rt) return w;
        const Ui::Theme& T = Ui::T;
        bool hot = hover == id, down = press == id;
        u32 c = down ? (T.accent & 0x00FFFFFF) | 0xA0000000 : hot ? (T.accent & 0x00FFFFFF) | 0xD0000000 : T.accent;
        Ui::Text(rt, t, RectF(x, cy - 12, x + tw + 2, cy + 12), 14, 400, c);
        Ui::Icon(rt, 0xE8A7, x + tw + 6 + 6, cy, 12, c);
        if (hot) Ui::Line(rt, x, cy + 9.5f, x + tw, cy + 9.5f, c, 1);
        return w;
    }

    // 快捷键：一组按键帽 + 编辑按钮；返回占的宽度
    float HotkeyKeys(Painter* rt, float right, float cy, bool enabled, int which)
    {
        const Ui::Theme& T = Ui::T;
        const int keysId = which ? SQkKeys : SHkKeys;
        const bool rec = recording && recWhich == which;
        Vec<Str> keys;
        if (rec)
        {
            if (recMods & ModCtrl) keys.Push(Str(L"Ctrl"));
            if (recMods & ModAlt) keys.Push(Str(L"Alt"));
            if (recMods & ModShift) keys.Push(Str(L"Shift"));
            if (recMods & ModWin) keys.Push(Str(L"Win"));
        }
        else
        {
            const Str& hk = HkText(which);
            int start = 0;
            for (int i = 0; i <= hk.n; i++)
                if (i == hk.n || hk.p[i] == L'+') { Str k = hk.Sub(start, i - start).Trim(); if (k.n) keys.Push(k); start = i + 1; }
        }
        float x = right;
        // 编辑按钮
        RectF pen = RectF(x - 32, cy - 16, x, cy + 16);
        x -= 32 + 6;
        float widths[8];
        int n = Min(keys.n, 8);
        float total = 0;
        for (int i = 0; i < n; i++) { widths[i] = Max(30.f, Ui::Measure(keys[i], 12, 600) + 18); total += widths[i] + (i ? 4 : 0); }
        Str waiting(L"请按组合键");
        if (rec && !n) total = Ui::Measure(waiting, 12, 400) + 24;
        float left = x - total;
        if (enabled) { HitArea a = { keysId, RectF(left - 4, cy - 18, right, cy + 18) }; hits.Push(a); }
        if (rt)
        {
            bool hot = enabled && hover == keysId;
            if (hot || rec) Ui::Fill(rt, pen, 4, rec ? T.subtlePress : T.subtleHover);
            Ui::Icon(rt, 0xE70F, (pen.left + pen.right) / 2, cy, 14, enabled ? T.text2 : T.disabled);
            if (rec && !n)
            {
                RectF r = RectF(left, cy - 15, x, cy + 15);
                Ui::Stroke(rt, r, 4, T.accent, 1.5f);
                Ui::Text(rt, waiting, r, 12, 400, T.accent, Ui::Center);
            }
            float kx = left;
            for (int i = 0; i < n; i++)
            {
                RectF r = RectF(kx, cy - 15, kx + widths[i], cy + 15);
                if (enabled) Ui::Fill(rt, r, 4, T.accent);
                else { Ui::Fill(rt, r, 4, T.ctrlFill); Ui::Stroke(rt, r, 4, T.ctrlBorder, 1); }
                Ui::Text(rt, keys[i], r, 12, 600, enabled ? T.onAccent : T.disabled, Ui::Center);
                kx += widths[i] + 4;
            }
        }
        return right - left;
    }

    // 卡片：左边图标，标题 + 说明，右边留 rightW 给控件；below = 下面接着的展开区高度
    RectF Card(Painter* rt, float& y, wchar_t icon, const Str& title, const Str& desc, u32 descColor, float rightW, bool wrapDesc, float below)
    {
        const float L = 24, R = Width - 24;
        float textL = L + 16 + 20 + 16, textR = R - 16 - (rightW > 0 ? rightW + 16 : 0);
        float descH = desc.n ? (wrapDesc ? Ui::MeasureWrap(desc, textR - textL, 12, 400) : 17) : 0;
        float block = 20 + descH;
        float h = Max(68.f, block + 30);
        RectF r = RectF(L, y, R, y + h);
        if (rt)
        {
            const Ui::Theme& T = Ui::T;
            RectF all = RectF(L, y, R, y + h + below);
            Ui::Fill(rt, all, 4, T.cardBg);
            Ui::Stroke(rt, all, 4, T.cardBorder, 1);
            Ui::Icon(rt, icon, L + 16 + 10, y + h / 2, 20, T.text);
            float ty = y + (h - block) / 2;
            Ui::Text(rt, title, RectF(textL, ty, textR, ty + 20), 14, 400, T.text);
            if (desc.n)
            {
                if (wrapDesc) Ui::TextWrap(rt, desc, textL, ty + 20, textR - textL, 12, 400, descColor);
                else Ui::Text(rt, desc, RectF(textL, ty + 20, textR, ty + 20 + descH), 12, 400, descColor);
            }
        }
        y += h;
        return r;
    }

    void Section(Painter* rt, float& y, const wchar_t* t, bool first)
    {
        if (!first) y += 24;
        if (rt) Ui::Text(rt, Str(t), RectF(24, y, Width - 24, y + 20), 14, 600, Ui::T.text);
        y += 20 + 8;
    }

    // 布局并（rt 不为空时）绘制；同时记下可点区域和输入框位置
    void Build(Painter* rt)
    {
        hits.Clear();
        const Ui::Theme& T = Ui::T;
        Settings& s = g_app->settings;
        const float L = 24, R = Width - 24;
        float y = 16;

        Section(rt, y, L"常规", true);
        RectF c = Card(rt, y, 0xE7E8, Str(L"开机自动启动"),
                             Str(g_app->isDefaultDir ? L"移动 exe 后，打开一次新位置的 exe，自启会跟着改过去" : L"用测试数据目录打开时不可用"),
                             T.text2, 84, true, 0);
        Toggle(rt, SAuto, Shell::AutoStartGet(), g_app->isDefaultDir, R - 16, (c.top + c.bottom) / 2);
        for (int which = 0; which < 2; which++)
        {
            y += 4;
            bool on = which ? s.quickAddEnabled : s.hotkeyEnabled;
            Str desc;
            u32 dc = T.text2;
            if (hkMsg[which].n) { desc = hkMsg[which]; dc = T.critical; }
            else if (recording && recWhich == which) { desc = L"按下新的组合键（要带 Ctrl 或 Alt），Esc 取消"; dc = T.accent; }
            else desc = which ? L"弹出一个小输入框，回车就加到选定的便签（便签右键菜单里选）" : L"临时把所有便签提到最前面，再按一次恢复";
            // 右边控件的宽度要先量出来
            int keep = hits.n;
            float keysW = HotkeyKeys(nullptr, 0, 0, on, which);
            while (hits.n > keep) hits.RemoveAt(hits.n - 1);
            c = Card(rt, y, which ? 0xE710 : 0xE765, Str(which ? L"快速添加" : L"全局快捷键"), desc, dc, 84 + 20 + keysW, true, 0);
            float cy = (c.top + c.bottom) / 2;
            Toggle(rt, which ? SQkToggle : SHkToggle, on, true, R - 16, cy);
            HotkeyKeys(rt, R - 16 - 84 - 20, cy, on, which);
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
            Ui::FillCorners(rt, RectF(L + 1, y, R - 1, y + below - 1), 0, 0, 3, 3, T.cardBg2);
            Ui::Line(rt, L + 1, y + 0.5f, R - 1, y + 0.5f, T.cardBorder, 1);
        }
        y += 8;
        const wchar_t* labels[3] = { L"服务器", L"密钥", L"文件夹" };
        for (int i = 0; i < 3; i++)
        {
            float cy = y + 22;
            frames[i] = RectF(R - 16 - 340, cy - 16, R - 16, cy + 16);
            if (rt)
            {
                Ui::Text(rt, Str(labels[i]), RectF(textL, cy - 12, frames[i].left - 12, cy + 12), 14, 400, T.text);
                bool f = Wnd::FieldHasFocus(edits[i]);
                RectF fr = frames[i];
                Ui::Fill(rt, fr, 4, f ? T.inputFocusBg : T.inputBg);
                Ui::Stroke(rt, fr, 4, T.ctrlBorder, 1);
                if (f) Ui::Fill(rt, RectF(fr.left + 1, fr.bottom - 2, fr.right - 1, fr.bottom), 1, T.accent);
                else Ui::Line(rt, fr.left + 3, fr.bottom - 0.5f, fr.right - 3, fr.bottom - 0.5f, T.strong, 1);
            }
            y += 44;
        }
        y += 6;
        if (rt) Ui::TextWrap(rt, help, textL, y, R - 16 - textL, 12, 400, T.text2);
        y += helpH + 14;
        Button(rt, SSyncSave, L"保存并立即同步", R - 16, y + 16, true);
        Link(rt, SOpenGopan, L"打开 gopan 网页建密钥", textL, y + 16);
        y += 32 + 18;

        // ---- 关于：版本 + 项目链接 ----
        Section(rt, y, L"关于", false);
        Str ver(L"原生版 · 编译于 ");
        const char* date = __DATE__ " " __TIME__;
        int dn = 0;
        while (date[dn]) dn++;
        ver += FromUtf8(date, dn);
        float linksH = 8 + 3 * 36 + 8;
        Card(rt, y, 0xE946, Str(L"DeskNote " APP_VERSION), ver, T.text2, 0, false, linksH);
        if (rt)
        {
            Ui::FillCorners(rt, RectF(L + 1, y, R - 1, y + linksH - 1), 0, 0, 3, 3, T.cardBg2);
            Ui::Line(rt, L + 1, y + 0.5f, R - 1, y + 0.5f, T.cardBorder, 1);
        }
        y += 8;
        struct { int id; const wchar_t* label; const wchar_t* url; } links[3] = {
            { SLinkRepo, L"DeskNote 源码", URL_DESKNOTE_REPO },
            { SLinkGopanRepo, L"gopan 源码", URL_GOPAN_REPO },
            { SLinkGopanWeb, L"gopan 网页", URL_GOPAN_WEB } };
        for (int i = 0; i < 3; i++)
        {
            float cy = y + 18;
            if (rt) Ui::Text(rt, Str(links[i].label), RectF(textL, cy - 12, textL + 120, cy + 12), 14, 400, T.text);
            // 网址本身做成链接，靠右
            float w = Ui::Measure(Str(links[i].url), 14, 400) + 6 + 12;
            Link(rt, links[i].id, links[i].url, R - 16 - w, cy);
            y += 36;
        }
        y += 8 + 24;
        contentH = y;
    }

    float ClientHDip() { float h; Wnd::ClientSize(win, nullptr, &h); return h; }

    void ClampScroll() { scroll = Clamp(scroll, 0.f, Max(0.f, contentH - ClientHDip())); }

    void PlaceEdits()
    {
        for (int i = 0; i < 3; i++)
        {
            RectF f = frames[i];
            Wnd::FieldSetBounds(edits[i], RectF(f.left + 11, f.top - scroll, f.right - 11, f.bottom - scroll));
        }
    }

    void Paint(Window*, Painter* rt, float, float viewH) override
    {
        Gfx::Clear(rt, Ui::T.winBg);
        Gfx::SetOffset(rt, 0, -scroll);
        Build(rt);
        Gfx::SetOffset(rt, 0, 0);
        if (contentH > viewH + 1)
        {
            // 细滚动条：只是位置指示，用滚轮滚
            float th = Max(24.f, viewH * viewH / contentH);
            float ty = (viewH - th) * scroll / (contentH - viewH);
            Ui::Fill(rt, RectF(Width - 7, ty + 2, Width - 4, ty + th - 2), 1.5f, Ui::T.text3);
        }
        PlaceEdits();
    }

    int HitAt(float x, float y)
    {
        y += scroll;
        for (int i = hits.n - 1; i >= 0; i--)
        {
            const RectF& r = hits[i].r;
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return hits[i].id;
        }
        return SNone;
    }

    static Str GetText(TextField* f) { return Wnd::FieldText(f); }
    void SaveSync()
    {
        Settings& s = g_app->settings;
        Str sv = GetText(edits[0]).Trim();
        while (sv.EndsWith(L"/")) sv.Truncate(sv.n - 1);
        if (sv.n && !sv.StartsWith(L"http")) sv = L"https://" + sv;
        if (!sv.n) sv = DefaultSyncServer;
        s.syncServer = sv;
        Wnd::FieldSetText(edits[0], sv);
        s.syncFolder = SyncNormFolder(GetText(edits[2]));
        Wnd::FieldSetText(edits[2], s.syncFolder);
        Str k = GetText(edits[1]).Trim();
        if (k.n) { s.syncKey = k; Wnd::FieldSetText(edits[1], Str()); }
        KeyCue();
        Store::SaveSettings(s);
        SyncStart(g_app->sync);
        SyncNow(g_app->sync);
        Invalidate();
    }

    // ---- 录快捷键 ----
    void StartRecording(int which)
    {
        recording = true;
        recWhich = which;
        recMods = 0;
        hkMsg[which] = L"";
        g_app->PauseHotkey(HkId(which)); // 否则按下去会被系统直接截走
        Wnd::WantAllKeys(win, true);
        Wnd::Focus(win);
        Invalidate();
    }
    void StopRecording()
    {
        if (!recording) return;
        recording = false;
        Wnd::WantAllKeys(win, false);
        const wchar_t* e = g_app->ApplyHotkey(HkId(recWhich));
        hkMsg[recWhich] = e ? e : L"";
        Invalidate();
    }
    void OnRecordKey(int vk, int mods)
    {
        if (vk == KeyEscape) { StopRecording(); return; }
        if (vk == KeyCtrl || vk == KeyAlt || vk == KeyShift || vk == KeyLWin || vk == KeyRWin ||
            vk == KeyLCtrl || vk == KeyRCtrl || vk == KeyLAlt || vk == KeyRAlt || vk == KeyLShift || vk == KeyRShift)
        {
            recMods = mods;
            Invalidate();
            return;
        }
        bool valid = (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || (vk >= KeyF1 && vk <= KeyF24) || vk == KeySpace;
        if (!valid || !(mods & (ModCtrl | ModAlt)))
        {
            recMods = mods;
            hkMsg[recWhich] = L"要带 Ctrl 或 Alt，再加一个字母、数字或 F 键";
            Invalidate();
            return;
        }
        Str hk = HotkeyFormat(mods, vk);
        if (hk == HkText(1 - recWhich))
        {
            recMods = mods;
            hkMsg[recWhich] = L"和另一个快捷键重复了";
            Invalidate();
            return;
        }
        HkText(recWhich) = hk;
        Store::SaveSettings(g_app->settings);
        StopRecording();
    }

    void Activate(int id)
    {
        Settings& s = g_app->settings;
        switch (id)
        {
        case SAuto: if (g_app->isDefaultDir) Shell::AutoStartSet(!Shell::AutoStartGet()); break;
        case SHkToggle: case SQkToggle:
        {
            int which = id == SQkToggle;
            if (recording) StopRecording();
            bool& on = which ? s.quickAddEnabled : s.hotkeyEnabled;
            on = !on;
            Store::SaveSettings(s);
            const wchar_t* e = g_app->ApplyHotkey(HkId(which));
            hkMsg[which] = e ? e : L"";
            break;
        }
        case SHkKeys: case SQkKeys:
        {
            int which = id == SQkKeys;
            bool again = recording && recWhich == which;
            if (recording) StopRecording();
            if (!again) StartRecording(which);
            break;
        }
        case SOpenDir: Wnd::OpenFolder(Store::Dir); break;
        case SOpenBackup:
            EnsureDir(Store::BackupsDir.c());
            Wnd::OpenFolder(Store::BackupsDir);
            break;
        case SSyncToggle: s.syncEnabled = !s.syncEnabled; SaveSync(); break;
        case SSyncSave: SaveSync(); break;
        case SOpenGopan:
        {
            // 填了服务器就开那个，否则开默认的 gopan
            Str sv = GetText(edits[0]).Trim();
            if (!sv.n) sv = DefaultSyncServer;
            if (!sv.StartsWith(L"http")) sv = L"https://" + sv;
            Wnd::OpenUrl(sv);
            break;
        }
        case SLinkRepo: Wnd::OpenUrl(Str(URL_DESKNOTE_REPO)); break;
        case SLinkGopanRepo: Wnd::OpenUrl(Str(URL_GOPAN_REPO)); break;
        case SLinkGopanWeb: Wnd::OpenUrl(Str(URL_GOPAN_WEB)); break;
        }
        Invalidate();
    }

    void ApplyTheme()
    {
        for (int i = 0; i < 3; i++) Wnd::FieldStyle(edits[i], FontNote, 14, 400, Ui::T.text, Ui::T.inputBg, Ui::T.inputFocusBg);
        Invalidate();
    }

    bool OnEvent(Window* w, const Event& ev) override
    {
        switch (ev.type)
        {
        case EvMouseMove:
        {
            int id = HitAt(ev.x, ev.y);
            if (id != hover) { hover = id; Invalidate(); }
            return true;
        }
        case EvMouseLeave:
            if (hover) { hover = SNone; Invalidate(); }
            return true;
        case EvMouseDown:
        {
            if (ev.button != MouseLeft) return false;
            int id = HitAt(ev.x, ev.y);
            if (recording && id != (recWhich ? SQkKeys : SHkKeys)) StopRecording();
            Wnd::Focus(w); // 点空白处：输入框失去焦点
            press = id;
            if (id) Wnd::Capture(w, true);
            Invalidate();
            return true;
        }
        case EvMouseUp:
        {
            if (ev.button != MouseLeft) return false;
            int p = press;
            press = SNone;
            Wnd::Capture(w, false);
            if (p && HitAt(ev.x, ev.y) == p) Activate(p);
            Invalidate();
            return true;
        }
        case EvWheel:
        {
            float old = scroll;
            scroll -= ev.wheel * 60;
            ClampScroll();
            if (scroll != old) { Invalidate(); Wnd::Update(w); }
            return true;
        }
        case EvKeyDown:
            if (recording) { OnRecordKey(ev.key, ev.mods); return true; }
            return false;
        case EvKeyUp:
            if (recording) { recMods = ev.mods; Invalidate(); return true; }
            return false;
        case EvChar: return recording;
        case EvFocus: if (!ev.button) StopRecording(); return true;
        case EvActivate: if (!ev.button) StopRecording(); return false;
        case EvFieldFocus: case EvFieldBlur: Invalidate(); return true;
        case EvSubmit:
            for (int i = 0; i < 3; i++) if (ev.field == edits[i]) SaveSync();
            return true;
        case EvResize: ClampScroll(); PlaceEdits(); return true;
        case EvScale: ClampScroll(); PlaceEdits(); Invalidate(); return true;
        case EvTheme: ApplyTheme(); return true;
        case EvDestroy:
            g_app->ApplyHotkeys();
            g_app->settingsWnd = nullptr;
            delete this;
            return true;
        }
        return false;
    }
};

Window* SettingsWindowCreate()
{
    SettingsWin* d = new SettingsWin();
    WinDesc desc;
    desc.title = L"DeskNote 设置";
    desc.resizable = false;
    d->win = Wnd::NewDialog(desc, d);
    if (!d->win) { delete d; return nullptr; }
    d->Init();
    d->Build(nullptr);
    // 放到鼠标所在屏幕中间；内容太高时窗口按屏幕高度截短，用滚轮滚
    Wnd::PlaceCentered(d->win, SettingsWin::Width, d->contentH, 0.9f, 40);
    d->ClampScroll();
    d->PlaceEdits();
    return d->win;
}

void SettingsWindowSyncStatus(Window* w)
{
    SettingsWin* d = (SettingsWin*)Wnd::Handler(w);
    if (d) d->Invalidate();
}

// =====================================================================
// 设置提醒：常用时间一键选；或者自己定日期和时间（自绘，和设置页一个样式）
// =====================================================================
static void DrawBtn(Painter* rt, RectF r, const Str& text, wchar_t icon, int kind, bool hot, bool down, u32 under)
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

struct ReminderWin : WinHandler
{
    Window* win = nullptr;
    bool done = false;
    int result = 0;           // 0 取消，1 设定，2 移除
    Time resultTime = 0;
    int repeat = RepeatNone;
    Str itemText;
    bool hasCurrent = false;
    Time quick[4];
    const wchar_t* quickLabel[4];
    int quickN = 0;
    Time day = 0;             // 自定义：哪一天（零点）
    TextField* timeEdit = nullptr;
    RectF timeFrame = {}, dateRow = {}, timeRow = {};
    struct Hit { int id; RectF r; };
    Vec<Hit> hits;
    int hover = 0, press = 0;
    float contentH = 0;
    static const int Width = 420;
    enum { HQuick = 1, HPrevDay = 10, HNextDay, HMinus, HPlus, HOk, HCancel, HRemove, HRepeat = 20 };

    void Invalidate() { Wnd::Invalidate(win); }

    void Init(const Str& text, bool has, Time current)
    {
        Str shown = text.Replace(L"\r", L"").Replace(L"\n", L" ").Trim();
        if (shown.n > 80) { shown.Truncate(80); shown += L"…"; }
        itemText = shown;
        hasCurrent = has;
        DateTime now;
        LocalNow(&now);
        Time t0 = Now();
        Time minuteNow = MakeTime(now.year, now.month, now.day, now.hour, now.minute, 0);
        quick[quickN] = minuteNow + 30 * TicksPerMinute; quickLabel[quickN++] = L"30 分钟后";
        quick[quickN] = minuteNow + 60 * TicksPerMinute; quickLabel[quickN++] = L"1 小时后";
        Time tonight = Today() + 20 * 60 * TicksPerMinute;
        if (tonight > t0 + 5 * TicksPerMinute) { quick[quickN] = tonight; quickLabel[quickN++] = L"今晚"; }
        quick[quickN] = Today() + TicksPerDay + 9 * 60 * TicksPerMinute; quickLabel[quickN++] = L"明早";
        // 自定义的初始值：已有提醒就用它，否则一小时后（取整到 5 分钟）
        Time init = has ? current : minuteNow + 60 * TicksPerMinute;
        DateTime st;
        SplitTime(init, &st);
        int mins = st.hour * 60 + st.minute;
        if (!has) mins = (mins + 4) / 5 * 5;
        day = MakeTime(st.year, st.month, st.day, 0, 0, 0);
        if (mins >= 24 * 60) { mins -= 24 * 60; day += TicksPerDay; }
        // 只输数字，不要输入法（中文标点会把冒号变成全角）
        timeEdit = Wnd::CreateField(win, Str(), Wnd::FieldCenter | Wnd::FieldNoIme);
        Wnd::FieldSetMaxLen(timeEdit, 5);
        Wnd::FieldStyle(timeEdit, FontUi, 15, 600, Ui::T.text, Ui::T.inputBg, Ui::T.inputFocusBg);
        SetMinutes(mins);
    }

    // 时间输入框：接受 15:00、15：00、15.00、1500、9
    int Minutes()
    {
        Str text = Wnd::FieldText(timeEdit);
        wchar_t buf[16] = {};
        for (int i = 0; i < Min(text.n, 15); i++) buf[i] = text[i];
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
        Wnd::FieldSetText(timeEdit, s);
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
        DateTime st;
        SplitTime(d, &st);
        Time today = Today();
        Str s;
        if (d == today) s = L"今天 ";
        else if (d == today + TicksPerDay) s = L"明天 ";
        else if (d == today + 2 * TicksPerDay) s = L"后天 ";
        DateTime now;
        LocalNow(&now);
        if (st.year != now.year) s.Int(st.year).Append(L"年");
        s.Int(st.month).Append(L"月").Int(st.day).Append(L"日 ").Append(wk[st.dayOfWeek % 7]);
        return s;
    }
    static Str WhenText(Time t)
    {
        DateTime st;
        SplitTime(t, &st);
        Time d = MakeTime(st.year, st.month, st.day, 0, 0, 0);
        Str s;
        if (d == Today() + TicksPerDay) s = L"明天 ";
        s.Pad2(st.hour).Ch(L':').Pad2(st.minute);
        return s;
    }

    void Push(int id, RectF r) { Hit h = { id, r }; hits.Push(h); }

    void Build(Painter* rt)
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
        if (rt) Ui::Text(rt, Str(L"快速选择"), RectF(L, y, R, y + 18), 12, 400, T.text2);
        y += 18 + 8;
        float qw = (CW - 8) / 2, qh = 52;
        for (int i = 0; i < quickN; i++)
        {
            float x = L + (i % 2) * (qw + 8), qy = y + (i / 2) * (qh + 8);
            RectF r = RectF(x, qy, x + qw, qy + qh);
            Push(HQuick + i, r);
            if (!rt) continue;
            bool hot = hover == HQuick + i, down = press == HQuick + i;
            Ui::Fill(rt, r, 6, Ui::Solid(down ? T.ctrlPress : hot ? T.ctrlHover : T.ctrlFill, T.winBg));
            Ui::Stroke(rt, r, 6, hot ? (T.accent & 0x00FFFFFF) | 0x80000000 : T.ctrlBorder, 1);
            Ui::Text(rt, Str(quickLabel[i]), RectF(x + 14, qy + 7, x + qw - 14, qy + 27), 14, 400, T.text);
            Ui::Text(rt, WhenText(quick[i]), RectF(x + 14, qy + 27, x + qw - 14, qy + 45), 12, 400, T.text2);
        }
        y += ((quickN + 1) / 2) * (qh + 8) + 10;

        // 自定义：日期（前一天 / 后一天）+ 时间（输入框，±15 分钟）
        if (rt) Ui::Text(rt, Str(L"自定义"), RectF(L, y, R, y + 18), 12, 400, T.text2);
        y += 18 + 8;
        RectF card = RectF(L, y, R, y + 56);
        if (rt) { Ui::Fill(rt, card, 6, T.cardBg); Ui::Stroke(rt, card, 6, T.cardBorder, 1); }
        float cy = y + 28;
        // 时间部分在右边
        RectF plus = RectF(R - 10 - 32, cy - 16, R - 10, cy + 16);
        timeFrame = RectF(plus.left - 4 - 72, cy - 16, plus.left - 4, cy + 16);
        RectF minus = RectF(timeFrame.left - 4 - 32, cy - 16, timeFrame.left - 4, cy + 16);
        timeRow = RectF(minus.left, card.top, R, card.bottom);
        // 日期部分在左边
        RectF prev = RectF(L + 10, cy - 16, L + 10 + 32, cy + 16);
        RectF next = RectF(minus.left - 16 - 32, cy - 16, minus.left - 16, cy + 16);
        dateRow = RectF(L, card.top, next.right, card.bottom);
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
            Ui::Text(rt, DayText(day), RectF(prev.right + 4, cy - 12, next.left - 4, cy + 12), 14, 400, T.text, Ui::Center);
            DrawBtn(rt, minus, Str(), 0xE738, 2, hover == HMinus, press == HMinus, T.cardBg);
            DrawBtn(rt, plus, Str(), 0xE710, 2, hover == HPlus, press == HPlus, T.cardBg);
            bool f = Wnd::FieldHasFocus(timeEdit);
            Ui::Fill(rt, timeFrame, 4, f ? T.inputFocusBg : T.inputBg);
            Ui::Stroke(rt, timeFrame, 4, T.ctrlBorder, 1);
            if (f) Ui::Fill(rt, RectF(timeFrame.left + 1, timeFrame.bottom - 2, timeFrame.right - 1, timeFrame.bottom), 1, T.accent);
            else Ui::Line(rt, timeFrame.left + 3, timeFrame.bottom - 0.5f, timeFrame.right - 3, timeFrame.bottom - 0.5f, T.strong, 1);
        }
        y += 56 + 12;

        // 重复：不重复 / 每天 / 工作日 / 每周
        {
            static const wchar_t* names[4] = { L"不重复", L"每天", L"工作日", L"每周" };
            const float rh = 32, sw = 72, gap = 4;
            float ry = y + rh / 2;
            if (rt) Ui::Text(rt, Str(L"重复"), RectF(L, ry - 12, L + 80, ry + 12), 14, 400, T.text);
            float sx = R - 4 * sw - 3 * gap;
            for (int i = 0; i < 4; i++)
            {
                RectF r = RectF(sx + i * (sw + gap), y, sx + i * (sw + gap) + sw, y + rh);
                Push(HRepeat + i, r);
                if (!rt) continue;
                bool on = repeat == i, hot = hover == HRepeat + i;
                if (on)
                {
                    Ui::Fill(rt, r, 4, T.accent);
                    Ui::Text(rt, Str(names[i]), r, 14, 400, T.onAccent, Ui::Center);
                }
                else
                {
                    Ui::Fill(rt, r, 4, Ui::Solid(press == HRepeat + i ? T.ctrlPress : hot ? T.ctrlHover : T.ctrlFill, T.winBg));
                    Ui::Stroke(rt, r, 4, T.ctrlBorder, 1);
                    Ui::Text(rt, Str(names[i]), r, 14, 400, T.text, Ui::Center);
                }
            }
            y += rh + 12;
        }
        // 状态：时间写错了 / 已经过了
        int mins = Minutes();
        Str status;
        u32 sc = T.text2;
        if (mins < 0) { status = L"时间格式不对，例如 15:30"; sc = T.critical; }
        else if (day + mins * TicksPerMinute <= Now()) { status = L"这个时间已经过了，确定后会马上提醒"; sc = T.critical; }
        else if (repeat != RepeatNone) status = L"到点后弹出系统通知，然后自动顺延到下一次；这一条已经勾掉的话，到点会重新打开。";
        else status = L"到点后弹出系统通知，条目上的提醒标签变红；程序没开着时错过的提醒，下次打开时补上。";
        float sh = Ui::MeasureWrap(status, CW, 12, 400);
        if (rt) Ui::TextWrap(rt, status, L, y, CW, 12, 400, sc);
        y += sh + 20;

        // 底部按钮
        float bw = 88;
        RectF ok = RectF(R - bw, y, R, y + 32), cancel = RectF(R - 2 * bw - 8, y, R - bw - 8, y + 32);
        if (mins >= 0) Push(HOk, ok);
        Push(HCancel, cancel);
        Str removeText(L"移除提醒");
        RectF remove = RectF(L - 8, y, L - 8 + Ui::Measure(removeText, 14, 400) + 16, y + 32);
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

    void PlaceEdit() { Wnd::FieldSetBounds(timeEdit, RectF(timeFrame.left + 6, timeFrame.top, timeFrame.right - 6, timeFrame.bottom)); }

    void Paint(Window*, Painter* rt, float, float) override
    {
        Gfx::Clear(rt, Ui::T.winBg);
        Build(rt);
    }

    int HitAt(float x, float y)
    {
        for (int i = hits.n - 1; i >= 0; i--)
        {
            const RectF& r = hits[i].r;
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return hits[i].id;
        }
        return 0;
    }

    void Activate(int id)
    {
        if (id >= HQuick && id < HQuick + quickN) { result = 1; resultTime = quick[id - HQuick]; done = true; return; }
        if (id >= HRepeat && id < HRepeat + 4) { repeat = id - HRepeat; Invalidate(); return; }
        switch (id)
        {
        case HPrevDay: if (day > Today()) day -= TicksPerDay; break;
        case HNextDay: day += TicksPerDay; break;
        case HMinus: Step(-15); break;
        case HPlus: Step(15); break;
        case HOk:
        {
            int m = Minutes();
            if (m < 0) { Wnd::FieldFocus(timeEdit); break; }
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

    bool OnEvent(Window* w, const Event& ev) override
    {
        switch (ev.type)
        {
        case EvMouseMove:
        {
            int id = HitAt(ev.x, ev.y);
            if (id != hover) { hover = id; Invalidate(); }
            return true;
        }
        case EvMouseLeave:
            hover = 0;
            Invalidate();
            return true;
        case EvMouseDown:
            if (ev.button != MouseLeft) return false;
            press = HitAt(ev.x, ev.y);
            if (press) Wnd::Capture(w, true);
            Invalidate();
            return true;
        case EvMouseUp:
        {
            if (ev.button != MouseLeft) return false;
            int p = press;
            press = 0;
            Wnd::Capture(w, false);
            if (p && HitAt(ev.x, ev.y) == p) Activate(p);
            Invalidate();
            return true;
        }
        case EvWheel:
        {
            // 滚轮：在日期上换天，在时间上 ±15 分钟
            int dir = ev.wheel > 0 ? 1 : -1;
            const RectF& dr = dateRow;
            const RectF& tr = timeRow;
            if (ev.x >= dr.left && ev.x < dr.right && ev.y >= dr.top && ev.y < dr.bottom)
            {
                if (dir > 0) day += TicksPerDay;
                else if (day > Today()) day -= TicksPerDay;
                Invalidate();
            }
            else if (ev.x >= tr.left && ev.x < tr.right && ev.y >= tr.top && ev.y < tr.bottom) Step(dir * 15);
            return true;
        }
        case EvFieldFocus: case EvFieldBlur: case EvFieldChange: Invalidate(); return true;
        case EvSubmit: Activate(HOk); return true;
        case EvClose: done = true; return true;
        case EvScale: PlaceEdit(); Invalidate(); return true;
        case EvTheme:
            Wnd::FieldStyle(timeEdit, FontUi, 15, 600, Ui::T.text, Ui::T.inputBg, Ui::T.inputFocusBg);
            Invalidate();
            return true;
        }
        return false;
    }
};

int ReminderDialog(void* owner, const Str& itemText, bool hasCurrent, Time current, int* repeat, Time* result)
{
    ReminderWin d;
    d.repeat = Clamp(*repeat, (int)RepeatNone, (int)RepeatWeekly);
    WinDesc desc;
    desc.title = L"设置提醒";
    desc.resizable = false;
    desc.minimizable = false;
    desc.topmost = true;
    desc.appIcon = false;
    desc.owner = owner;
    d.win = Wnd::NewDialog(desc, &d);
    if (!d.win) return 0;
    d.Init(itemText, hasCurrent, current);
    d.Build(nullptr);
    // 放在鼠标所在屏幕的中间
    Wnd::PlaceCentered(d.win, ReminderWin::Width, d.contentH, 100, 0);
    d.PlaceEdit();
    Wnd::RunModal(d.win, &d.done);
    if (d.result == 1) { *result = d.resultTime; *repeat = d.repeat; }
    return d.result;
}

// =====================================================================
// 快速添加：快捷键弹出的小输入框，Enter 加到选定的便签，Esc 或点到别处就关
// =====================================================================
struct QuickAddWin : WinHandler
{
    Window* win = nullptr;
    TextField* field = nullptr;
    RectF frame;
    float contentH = 0;
    static const int Width = 460;

    void Invalidate() { Wnd::Invalidate(win); }

    void Build(Painter* rt)
    {
        const Ui::Theme& T = Ui::T;
        const float L = 20, R = Width - 20;
        float y = 16;
        NoteWin* n = g_app->QuickAddTarget();
        Str to = n ? L"加到「" + n->d->title + L"」" : Str(L"还没有便签，会新建一张");
        if (rt) Ui::Text(rt, to, RectF(L, y, R, y + 22), 14, 600, T.text);
        y += 22 + 10;
        frame = RectF(L, y, R, y + 36);
        if (rt)
        {
            bool f = Wnd::FieldHasFocus(field);
            Ui::Fill(rt, frame, 4, f ? T.inputFocusBg : T.inputBg);
            Ui::Stroke(rt, frame, 4, T.ctrlBorder, 1);
            if (f) Ui::Fill(rt, RectF(frame.left + 1, frame.bottom - 2, frame.right - 1, frame.bottom), 1, T.accent);
            else Ui::Line(rt, frame.left + 3, frame.bottom - 0.5f, frame.right - 3, frame.bottom - 0.5f, T.strong, 1);
        }
        y += 36 + 10;
        Str hint(L"Enter 添加，Esc 取消。换一张便签：在便签上右键 →「快速添加到这张便签」");
        float hh = Ui::MeasureWrap(hint, R - L, 12, 400);
        if (rt) Ui::TextWrap(rt, hint, L, y, R - L, 12, 400, T.text2);
        y += hh + 16;
        contentH = y;
    }

    void PlaceEdit() { Wnd::FieldSetBounds(field, RectF(frame.left + 11, frame.top, frame.right - 11, frame.bottom)); }

    void Paint(Window*, Painter* rt, float, float) override
    {
        Gfx::Clear(rt, Ui::T.winBg);
        Build(rt);
        PlaceEdit();
    }

    bool OnEvent(Window* w, const Event& ev) override
    {
        switch (ev.type)
        {
        case EvSubmit:
            if (g_app->QuickAdd(Wnd::FieldText(field))) Wnd::Destroy(w);
            return true;
        case EvActivate:
            if (!ev.button) Wnd::Destroy(w); // 点到别处：收起来
            return false;
        case EvFieldFocus: case EvFieldBlur: Invalidate(); return true;
        case EvScale: PlaceEdit(); Invalidate(); return true;
        case EvTheme:
            Wnd::FieldStyle(field, FontNote, 15, 400, Ui::T.text, Ui::T.inputBg, Ui::T.inputFocusBg);
            Invalidate();
            return true;
        case EvDestroy:
            g_app->quickWnd = nullptr;
            delete this;
            return true;
        }
        return false;
    }
};

Window* QuickAddWindowCreate()
{
    QuickAddWin* d = new QuickAddWin();
    WinDesc desc;
    desc.title = L"快速添加";
    desc.resizable = false;
    desc.minimizable = false;
    desc.topmost = true;
    d->win = Wnd::NewDialog(desc, d);
    if (!d->win) { delete d; return nullptr; }
    d->field = Wnd::CreateField(d->win, Str(), 0);
    Wnd::FieldStyle(d->field, FontNote, 15, 400, Ui::T.text, Ui::T.inputBg, Ui::T.inputFocusBg);
    Wnd::FieldSetCue(d->field, Str(L"要做的事"));
    d->Build(nullptr);
    Wnd::PlaceCentered(d->win, QuickAddWin::Width, d->contentH, 100, 0);
    d->PlaceEdit();
    return d->win;
}

void QuickAddWindowFocus(Window* w)
{
    QuickAddWin* d = (QuickAddWin*)Wnd::Handler(w);
    if (d) Wnd::FieldFocus(d->field);
}

// =====================================================================
// 便签管理：所有便签一览；显示 / 隐藏（隐藏只在这台电脑上，不同步）、定位、删除、新建；搜索标题和内容
// =====================================================================
static void DrawToggle(Painter* rt, RectF tr, bool on, bool hot)
{
    const Ui::Theme& T = Ui::T;
    float cy = (tr.top + tr.bottom) / 2;
    if (on)
    {
        Ui::Fill(rt, tr, 10, hot ? (T.accent & 0x00FFFFFF) | 0xE6000000 : T.accent);
        Ui::Circle(rt, tr.right - 10, cy, hot ? 7.f : 6.f, T.onAccent);
    }
    else
    {
        Ui::Fill(rt, tr, 10, hot ? T.subtleHover : T.toggleOffFill);
        Ui::Stroke(rt, tr, 10, T.strong, 1);
        Ui::Circle(rt, tr.left + 10, cy, hot ? 7.f : 6.f, T.text2);
    }
}

static u32 Rgb(u32 rgb, float a) { return ((u32)(Clamp(a, 0.f, 1.f) * 255 + 0.5f) << 24) | (rgb & 0xFFFFFF); }

// 便签文件的修改时间（本地时间）
static bool NoteModified(const Str& id, DateTime* st, u64* key)
{
    Time t;
    if (!FileMTime(Store::NotePath(id).c(), &t)) return false;
    *key = (u64)t;
    SplitTime(t, st);
    return true;
}

static Str WhenText(const DateTime& st)
{
    DateTime now;
    LocalNow(&now);
    Time day = MakeTime(st.year, st.month, st.day, 0, 0, 0);
    Str s;
    if (day == Today()) s = L"今天 ";
    else if (day == Today() - TicksPerDay) s = L"昨天 ";
    else
    {
        if (st.year != now.year) s.Int(st.year).Append(L"年");
        s.Int(st.month).Append(L"月").Int(st.day).Append(L"日");
        return s;
    }
    s.Pad2(st.hour).Ch(L':').Pad2(st.minute);
    return s;
}

static Str Lower(const Str& s) { return LowerCase(s); }

struct ManageWin : WinHandler
{
    Window* win = nullptr;
    float S() const { return Wnd::Scale(win); }
    TextField* search = nullptr;
    RectF searchFrame;
    struct Hit { int id; RectF r; NoteWin* w; };
    Vec<Hit> hits;              // 列表里的区域是内容坐标（要加滚动），顶栏是窗口坐标
    int hover = 0, press = 0;
    NoteWin* hoverNote = nullptr;
    NoteWin* pressNote = nullptr;
    NoteWin* expanded = nullptr; // 展开看条目的那张
    float scroll = 0, contentH = 0, listTop = 0;
    enum { HNew = 1, HShowAll, HRow, HToggle, HLocate, HDelete, HSettings };

    void Invalidate() { Wnd::Invalidate(win); }
    float ClientW() { float w; Wnd::ClientSize(win, &w, nullptr); return w; }
    float ClientH() { float h; Wnd::ClientSize(win, nullptr, &h); return h; }
    void ClampScroll() { scroll = Clamp(scroll, 0.f, Max(0.f, contentH - (ClientH() - listTop))); }
    bool Alive(NoteWin* w) const { return w && g_app->notes.Contains(w); }

    void Init()
    {
        search = Wnd::CreateField(win, Str(), 0);
        Wnd::FieldSetCue(search, Str(L"搜索标题或内容"));
    }
    Str Query() { return Lower(Wnd::FieldText(search).Trim()); }

    struct Entry { NoteWin* w; u64 key; DateTime st; bool hasTime; };
    // 按搜索过滤，按修改时间从新到旧
    void Collect(Vec<Entry>& out)
    {
        Str q = Query();
        for (int i = 0; i < g_app->notes.n; i++)
        {
            NoteWin* w = g_app->notes[i];
            if (q.n)
            {
                bool hit = Lower(w->d->title).Find(q.c()) >= 0;
                for (int k = 0; k < w->d->items.n && !hit; k++) if (Lower(w->d->items[k].text).Find(q.c()) >= 0) hit = true;
                if (!hit) continue;
            }
            Entry e = { w, 0, {}, false };
            e.hasTime = NoteModified(w->d->id, &e.st, &e.key);
            int at = out.n;
            while (at > 0 && out[at - 1].key < e.key) at--;
            out.Insert(at, e);
        }
    }

    void Build(Painter* rt)
    {
        hits.Clear();
        const Ui::Theme& T = Ui::T;
        float W = ClientW(), H = ClientH();
        const float L = 24, R = W - 24;
        int hiddenN = 0;
        for (int i = 0; i < g_app->notes.n; i++) if (g_app->notes[i]->d->hidden) hiddenN++;

        // ---- 顶栏 ----
        float y = 18;
        Str newText(L"新建便签"), allText(L"全部显示");
        float nw = Ui::Measure(newText, 14, 400) + 28, aw = Ui::Measure(allText, 14, 400) + 24;
        RectF nb = RectF(R - nw, y, R, y + 32), ab = RectF(nb.left - 8 - aw, y, nb.left - 8, y + 32);
        Hit h1 = { HNew, nb, nullptr };
        hits.Push(h1);
        if (hiddenN) { Hit h2 = { HShowAll, ab, nullptr }; hits.Push(h2); }
        if (rt)
        {
            float tw = Ui::Text(rt, Str(L"便签管理"), RectF(L, y, (hiddenN ? ab.left : nb.left) - 16, y + 32), 20, 600, T.text);
            Str count;
            count.Int(g_app->notes.n).Append(L" 张");
            if (hiddenN) { count.Append(L" · "); count.Int(hiddenN).Append(L" 张隐藏"); }
            Ui::Text(rt, count, RectF(L + tw + 10, y + 4, (hiddenN ? ab.left : nb.left) - 16, y + 32), 14, 400, T.text2);
            DrawBtn(rt, nb, newText, 0, 1, hover == HNew, press == HNew, T.winBg);
            if (hiddenN) DrawBtn(rt, ab, allText, 0, 0, hover == HShowAll, press == HShowAll, T.winBg);
        }
        y += 32 + 14;
        // 搜索框
        searchFrame = RectF(L, y, R, y + 34);
        if (rt)
        {
            bool f = Wnd::FieldHasFocus(search);
            Ui::Fill(rt, searchFrame, 4, f ? T.inputFocusBg : T.inputBg);
            Ui::Stroke(rt, searchFrame, 4, T.ctrlBorder, 1);
            if (f) Ui::Fill(rt, RectF(searchFrame.left + 1, searchFrame.bottom - 2, searchFrame.right - 1, searchFrame.bottom), 1, T.accent);
            else Ui::Line(rt, searchFrame.left + 3, searchFrame.bottom - 0.5f, searchFrame.right - 3, searchFrame.bottom - 0.5f, T.strong, 1);
            Ui::Icon(rt, 0xE721, searchFrame.left + 18, (searchFrame.top + searchFrame.bottom) / 2, 14, T.text2);
        }
        y += 34 + 14;
        listTop = y;

        // ---- 列表 ----
        Vec<Entry> list;
        Collect(list);
        if (!list.n)
        {
            if (rt)
            {
                float cy = listTop + (H - listTop) * 0.35f;
                bool none = g_app->notes.n == 0;
                Ui::Icon(rt, none ? 0xE70B : 0xE721, W / 2, cy - 26, 36, T.text3);
                Ui::Text(rt, Str(none ? L"还没有便签" : L"没有找到"), RectF(L, cy + 4, R, cy + 28), 14, 400, T.text2, Ui::Center);
            }
            contentH = 0;
            return;
        }
        if (rt)
        {
            Gfx::PushClip(rt, RectF(0, listTop, W, H));
            Gfx::SetOffset(rt, 0, -scroll);
        }
        float ly = listTop;
        const float rowH = 64, lineH = 22;
        const int maxLines = 10;
        for (int i = 0; i < list.n; i++)
        {
            NoteWin* w = list[i].w;
            NoteData* d = w->d;
            bool hidden = d->hidden;
            bool open = expanded == w;
            int shown = open ? Min(d->items.n, maxLines) : 0;
            float extra = open ? 8 + Max(1, shown + (d->items.n > maxLines ? 1 : 0)) * lineH + 10 : 0;
            RectF card = RectF(L, ly, R, ly + rowH + extra);
            float cy = ly + rowH / 2;
            RectF del = RectF(R - 12 - 32, cy - 16, R - 12, cy + 16);
            RectF loc = RectF(del.left - 4 - 32, cy - 16, del.left - 4, cy + 16);
            RectF set = RectF(loc.left - 4 - 32, cy - 16, loc.left - 4, cy + 16);
            RectF tog = RectF(set.left - 14 - 40, cy - 10, set.left - 14, cy + 10);
            RectF togHit = RectF(tog.left - 40, cy - 16, tog.right + 4, cy + 16);
            Hit hr = { HRow, card, w }, ht = { HToggle, togHit, w }, hd = { HDelete, del, w };
            hits.Push(hr);
            hits.Push(ht);
            Hit hs = { HSettings, set, w };
            hits.Push(hs);
            if (!hidden) { Hit hl = { HLocate, loc, w }; hits.Push(hl); }
            hits.Push(hd);
            if (rt && ly - scroll < H && card.bottom - scroll > listTop)
            {
                bool rowHot = hoverNote == w;
                Ui::Fill(rt, card, 4, rowHot ? Ui::Solid(T.subtleHover, T.cardBg) : T.cardBg);
                Ui::Stroke(rt, card, 4, T.cardBorder, 1);
                // 色块：便签自己的底色，上面用字色写 Aa
                float a = hidden ? 0.4f : 1.f;
                RectF sw = RectF(L + 14, cy - 18, L + 14 + 36, cy + 18);
                Ui::Fill(rt, sw, 8, Rgb(d->bgColor, a));
                Ui::Stroke(rt, sw, 8, T.ctrlBorder, 1);
                Ui::Text(rt, Str(L"Aa"), sw, 13, 600, Rgb(d->textColor, a), Ui::Center);
                float textL = sw.right + 14, textR = togHit.left - 8;
                Ui::Text(rt, d->title, RectF(textL, cy - 19, textR, cy + 1), 14, hidden ? 400 : 600, hidden ? T.text2 : T.text);
                int undone = 0;
                for (int k = 0; k < d->items.n; k++) if (!d->checkboxes || !d->items[k].done) undone++;
                Str sub;
                if (hidden) sub = L"已隐藏 · ";
                if (d->checkboxes) { sub.Int(undone).Append(L" 条未完成 · "); }
                sub.Append(L"共 ").Int(d->items.n).Append(L" 条 · ");
                sub += d->mode == ModeTopmost ? L"置顶" : d->mode == ModeNormal ? L"普通窗口" : L"置底";
                if (d->collapsed) sub += L"（胶囊）";
                if (list[i].hasTime) { sub += L" · "; sub += WhenText(list[i].st); }
                Ui::Text(rt, sub, RectF(textL, cy + 1, textR, cy + 18), 12, 400, T.text2);
                Ui::Text(rt, Str(L"显示"), RectF(tog.left - 40, cy - 10, tog.left - 8, cy + 10), 12, 400, T.text2, Ui::Right);
                DrawToggle(rt, tog, !hidden, hover == HToggle && hoverNote == w);
                DrawBtn(rt, set, Str(), 0xE713, 2, hover == HSettings && hoverNote == w, press == HSettings && pressNote == w, T.cardBg);
                if (hidden) Ui::Icon(rt, 0xE81D, (loc.left + loc.right) / 2, cy, 14, T.disabled);
                else DrawBtn(rt, loc, Str(), 0xE81D, 2, hover == HLocate && hoverNote == w, press == HLocate && pressNote == w, T.cardBg);
                bool hdel = hover == HDelete && hoverNote == w;
                if (hdel) Ui::Fill(rt, del, 4, press == HDelete ? T.subtlePress : T.subtleHover);
                Ui::Icon(rt, 0xE74D, (del.left + del.right) / 2, cy, 14, hdel ? T.critical : T.text2);
                // 展开：条目预览
                if (open)
                {
                    float py = ly + rowH + 4;
                    Ui::Line(rt, L + 14, py - 0.5f, R - 14, py - 0.5f, T.divider, 1);
                    py += 4;
                    if (!d->items.n)
                        Ui::Text(rt, Str(L"（还没有条目）"), RectF(textL, py, R - 16, py + lineH), 13, 400, T.text3);
                    for (int k = 0; k < shown; k++)
                    {
                        const NoteItem& it = d->items[k];
                        bool done = d->checkboxes && it.done;
                        float icy = py + lineH / 2;
                        if (d->checkboxes)
                        {
                            RectF cb = RectF(textL, icy - 6, textL + 12, icy + 6);
                            if (done) { Ui::Fill(rt, cb, 3, T.text3); Ui::Icon(rt, 0xE73E, textL + 6, icy, 9, T.cardBg); }
                            else Ui::Stroke(rt, cb, 3, T.text3, 1.2f);
                        }
                        else Ui::Circle(rt, textL + 6, icy, 2.5f, T.text3);
                        Str t = it.text.Replace(L"\r", L"").Replace(L"\n", L" ");
                        Ui::Text(rt, t, RectF(textL + 20, py, R - 16, py + lineH), 13, 400, done ? T.text3 : T.text);
                        py += lineH;
                    }
                    if (d->items.n > maxLines)
                    {
                        Str more(L"…还有 ");
                        more.Int(d->items.n - maxLines).Append(L" 条");
                        Ui::Text(rt, more, RectF(textL + 20, py, R - 16, py + lineH), 12, 400, T.text3);
                    }
                }
            }
            ly = card.bottom + 4;
        }
        ly += 20;
        contentH = ly - listTop;
        if (rt)
        {
            Gfx::SetOffset(rt, 0, 0);
            Gfx::PopClip(rt);
            float viewH = H - listTop;
            if (contentH > viewH + 1)
            {
                float th = Max(24.f, viewH * viewH / contentH);
                float ty = listTop + (viewH - th) * Clamp(scroll / (contentH - viewH), 0.f, 1.f);
                Ui::Fill(rt, RectF(W - 7, ty + 2, W - 4, ty + th - 2), 1.5f, T.text3);
            }
        }
    }

    void PlaceEdit() { Wnd::FieldSetBounds(search, RectF(searchFrame.left + 34, searchFrame.top, searchFrame.right - 10, searchFrame.bottom)); }

    void Paint(Window*, Painter* rt, float, float) override
    {
        Gfx::Clear(rt, Ui::T.winBg);
        Build(rt);
        PlaceEdit();
    }

    int HitAt(float x, float y, NoteWin** w)
    {
        *w = nullptr;
        for (int i = hits.n - 1; i >= 0; i--)
        {
            const Hit& h = hits[i];
            float yy = h.w ? y + scroll : y;
            if (h.w && y < listTop) continue;
            if (x >= h.r.left && x < h.r.right && yy >= h.r.top && yy < h.r.bottom) { *w = h.w; return h.id; }
        }
        return 0;
    }

    void Activate(int id, NoteWin* w)
    {
        if (id == HNew) { g_app->CreateNote(nullptr); return; }
        if (id == HShowAll)
        {
            for (int i = 0; i < g_app->notes.n; i++) if (g_app->notes[i]->d->hidden) g_app->notes[i]->SetHidden(false);
            return;
        }
        if (!Alive(w)) return;
        switch (id)
        {
        case HRow: expanded = expanded == w ? nullptr : w; Refresh(); break;
        case HToggle: w->SetHidden(!w->d->hidden); break;
        case HLocate: w->Locate(); break;
        case HDelete: g_app->DeleteNote(w, true); break;
        case HSettings:
        {
            // 菜单弹在按钮下面
            for (int i = 0; i < hits.n; i++)
                if (hits[i].id == HSettings && hits[i].w == w)
                {
                    PointI p = Wnd::ToScreen(win, hits[i].r.left, hits[i].r.bottom - scroll + 2);
                    w->ShowSettingsMenu(p.x, p.y, Wnd::Native(win));
                    break;
                }
            break;
        }
        }
    }

    void Refresh()
    {
        if (!Alive(expanded)) expanded = nullptr;
        if (!Alive(hoverNote)) { hoverNote = nullptr; hover = 0; }
        Build(nullptr);
        ClampScroll();
        Invalidate();
    }

    bool OnEvent(Window* w, const Event& ev) override
    {
        NoteWin* n;
        switch (ev.type)
        {
        case EvResize: Build(nullptr); ClampScroll(); PlaceEdit(); Invalidate(); return true;
        case EvMouseMove:
        {
            int id = HitAt(ev.x, ev.y, &n);
            if (id != hover || n != hoverNote) { hover = id; hoverNote = n; Invalidate(); }
            return true;
        }
        case EvMouseLeave:
            hover = 0;
            hoverNote = nullptr;
            Invalidate();
            return true;
        case EvMouseDown:
            if (ev.button != MouseLeft) return false;
            if (ev.clicks == 2)
            {
                // 双击一行：显示出来并定位
                if (HitAt(ev.x, ev.y, &n) == HRow && Alive(n))
                {
                    if (n->d->hidden) n->SetHidden(false);
                    n->Locate();
                }
                return true;
            }
            Wnd::Focus(w); // 点空白处：搜索框失去焦点
            press = HitAt(ev.x, ev.y, &pressNote);
            if (press) Wnd::Capture(w, true);
            Invalidate();
            return true;
        case EvMouseUp:
        {
            if (ev.button == MouseRight)
            {
                // 右键一行：这张便签的设置菜单
                if (HitAt(ev.x, ev.y, &n) && Alive(n))
                {
                    PointI p = Wnd::ToScreen(w, ev.x, ev.y);
                    n->ShowSettingsMenu(p.x, p.y, Wnd::Native(w));
                }
                return true;
            }
            int p = press;
            NoteWin* pn = pressNote;
            press = 0;
            pressNote = nullptr;
            Wnd::Capture(w, false);
            int id = HitAt(ev.x, ev.y, &n);
            if (p && id == p && n == pn) Activate(p, n);
            Invalidate();
            return true;
        }
        case EvWheel:
        {
            float old = scroll;
            scroll -= ev.wheel * 66;
            ClampScroll();
            if (scroll != old) Invalidate();
            return true;
        }
        case EvFieldChange: scroll = 0; Refresh(); return true;
        case EvFieldFocus: case EvFieldBlur: Invalidate(); return true;
        case EvClose:
            // Esc：搜索框里有字先清空，没有就关窗口
            if (ev.key == KeyEscape && Wnd::FieldText(search).n > 0) { Wnd::FieldSetText(search, Str()); return true; }
            return false;
        case EvScale: Invalidate(); return true;
        case EvTheme:
            Wnd::FieldStyle(search, FontNote, 14, 400, Ui::T.text, Ui::T.inputBg, Ui::T.inputFocusBg);
            Invalidate();
            return true;
        case EvDestroy:
            g_app->manageWnd = nullptr;
            delete this;
            return true;
        }
        return false;
    }
};

Window* ManageWindowCreate()
{
    ManageWin* d = new ManageWin();
    WinDesc desc;
    desc.title = L"DeskNote 便签管理";
    desc.minW = 480;
    desc.minH = 360;
    d->win = Wnd::NewDialog(desc, d);
    if (!d->win) { delete d; return nullptr; }
    d->Init();
    Wnd::PlaceCentered(d->win, 640, 600, 0.85f, 0);
    d->Build(nullptr);
    d->PlaceEdit();
    return d->win;
}

void ManageWindowRefresh(Window* w)
{
    ManageWin* d = (ManageWin*)Wnd::Handler(w);
    if (d) d->Refresh();
}
