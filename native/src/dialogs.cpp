// 回收站窗口、设置窗口、提醒对话框、不透明度和字号对话框（都用系统控件）
#include "app.h"
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

// =====================================================================
// 布局小工具：控件位置按 DIP 记下来，换 DPI 时重新排
// =====================================================================
struct CtlPos { HWND h; int x, y, w, ht; bool bold; };
struct DlgBase
{
    HWND hwnd = nullptr;
    Vec<CtlPos> ctls;
    Vec<HWND> hints;      // 灰色说明文字
    HWND status = nullptr;
    COLORREF statusColor = RGB(0x77, 0x77, 0x77);
    int dpi = 96;
    float S() const { return dpi / 96.f; }
    HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0)
    {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, (int)(x * S()), (int)(y * S()), (int)(w * S()), (int)(h * S()),
                                 hwnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_app->UiFont(dpi), FALSE);
        CtlPos p = { c, x, y, w, h, false };
        ctls.Push(p);
        return c;
    }
    void MakeBold(HWND c)
    {
        for (int i = 0; i < ctls.n; i++) if (ctls[i].h == c) ctls[i].bold = true;
        SendMessageW(c, WM_SETFONT, (WPARAM)g_app->UiFontBold(dpi), TRUE);
    }
    HWND Hint(const wchar_t* text, int x, int y, int w, int h)
    {
        HWND c = Add(L"STATIC", text, SS_LEFT, x, y, w, h, 0);
        hints.Push(c);
        return c;
    }
    void Relayout(int newDpi)
    {
        dpi = newDpi;
        HFONT f = g_app->UiFont(dpi);
        for (int i = 0; i < ctls.n; i++)
        {
            MoveWindow(ctls[i].h, (int)(ctls[i].x * S()), (int)(ctls[i].y * S()), (int)(ctls[i].w * S()), (int)(ctls[i].ht * S()), TRUE);
            SendMessageW(ctls[i].h, WM_SETFONT, (WPARAM)(ctls[i].bold ? g_app->UiFontBold(dpi) : f), TRUE);
        }
    }
    // 根据内容区大小（DIP）设窗口大小
    void SizeClient(int w, int h)
    {
        RECT r = { 0, 0, (int)(w * S()), (int)(h * S()) };
        AdjustWindowRectExForDpi(&r, GetWindowLongW(hwnd, GWL_STYLE), FALSE, GetWindowLongW(hwnd, GWL_EXSTYLE), dpi);
        SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void Center()
    {
        RECT r;
        GetWindowRect(hwnd, &r);
        POINT p;
        GetCursorPos(&p);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
        int w = r.right - r.left, h = r.bottom - r.top;
        SetWindowPos(hwnd, nullptr, (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - h) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // 共用消息：字体颜色、DPI 变化
    bool Common(UINT m, WPARAM w, LPARAM l, LRESULT* r)
    {
        if (m == WM_CTLCOLORSTATIC)
        {
            // 静态文字、勾选框、只读输入框都用窗口的白底
            HWND c = (HWND)l;
            HDC dc = (HDC)w;
            SetBkMode(dc, TRANSPARENT);
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            if (hints.Contains(c)) SetTextColor(dc, RGB(0x77, 0x77, 0x77));
            else if (c == status) SetTextColor(dc, statusColor);
            *r = (LRESULT)GetSysColorBrush(COLOR_WINDOW);
            return true;
        }
        if (m == WM_DPICHANGED)
        {
            RECT* rc = (RECT*)l;
            Relayout(HIWORD(w));
            SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            *r = 0;
            return true;
        }
        return false;
    }
};

static int TextHeightDip(const wchar_t* text, int widthDip)
{
    HDC dc = GetDC(nullptr);
    HFONT f = g_app->UiFont(96);
    HGDIOBJ of = SelectObject(dc, f);
    RECT r = { 0, 0, widthDip, 0 };
    DrawTextW(dc, text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, of);
    ReleaseDC(nullptr, dc);
    return r.bottom + 2;
}

static HWND CreateDlgWindow(const wchar_t* cls, const wchar_t* title, DWORD style, void* self, HWND owner)
{
    POINT p;
    GetCursorPos(&p);
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME, cls, title, style, p.x, p.y, 100, 100, owner, nullptr, g_inst, self);
    return h;
}

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
// 回收站：查看、恢复、彻底删除；不自动清理
// =====================================================================
enum { IdList = 10, IdRestore, IdPurge, IdEmpty, IdClose };
struct TrashDlg : DlgBase
{
    HWND list = nullptr, hdr = nullptr, bRestore, bPurge, bEmpty, bClose;
    void Build()
    {
        hdr = Hint(L"删掉的条目和便签都在这里，不会自动清理。恢复的条目会放回原来那张便签的原位置。", 16, 12, 528, 36);
        list = Add(WC_LISTVIEWW, L"", WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, 16, 52, 528, 400, IdList);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* cols[] = { L"内容", L"来自", L"删除时间" };
        int widths[] = { 290, 120, 130 };
        for (int i = 0; i < 3; i++)
        {
            LVCOLUMNW c = {};
            c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
            c.fmt = LVCFMT_LEFT;
            c.pszText = (LPWSTR)cols[i];
            c.cx = (int)(widths[i] * S());
            c.iSubItem = i;
            LRESULT rr = SendMessageW(list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
            (void)rr;
        }
        bRestore = Add(L"BUTTON", L"恢复", WS_TABSTOP | BS_PUSHBUTTON, 16, 464, 88, 28, IdRestore);
        bPurge = Add(L"BUTTON", L"彻底删除", WS_TABSTOP | BS_PUSHBUTTON, 112, 464, 88, 28, IdPurge);
        bEmpty = Add(L"BUTTON", L"清空回收站", WS_TABSTOP | BS_PUSHBUTTON, 360, 464, 96, 28, IdEmpty);
        bClose = Add(L"BUTTON", L"关闭", WS_TABSTOP | BS_PUSHBUTTON, 464, 464, 80, 28, IdClose);
        Refresh();
    }
    void Layout()
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int W = (int)(rc.right / S()), H = (int)(rc.bottom / S());
        auto place = [&](HWND h, int x, int y, int w, int ht) {
            for (int i = 0; i < ctls.n; i++) if (ctls[i].h == h) { ctls[i].x = x; ctls[i].y = y; ctls[i].w = w; ctls[i].ht = ht; }
            MoveWindow(h, (int)(x * S()), (int)(y * S()), (int)(w * S()), (int)(ht * S()), TRUE);
        };
        place(hdr, 16, 12, W - 32, 36);
        place(list, 16, 52, W - 32, H - 52 - 52);
        place(bRestore, 16, H - 40, 88, 28);
        place(bPurge, 112, H - 40, 88, 28);
        place(bEmpty, W - 16 - 80 - 8 - 96, H - 40, 96, 28);
        place(bClose, W - 16 - 80, H - 40, 80, 28);
        // 列宽按当前 DPI 和列表宽度重新分配（窗口挪到缩放不同的屏幕后也对）
        RECT lr;
        GetClientRect(list, &lr);
        int c1 = (int)(120 * S()), c2 = (int)(130 * S());
        ListView_SetColumnWidth(list, 1, c1);
        ListView_SetColumnWidth(list, 2, c2);
        ListView_SetColumnWidth(list, 0, Max(60, (int)lr.right - c1 - c2 - 2));
    }
    void Refresh()
    {
        ListView_DeleteAllItems(list);
        for (int i = 0; i < g_app->trash.n; i++)
        {
            TrashEntry* e = g_app->trash[i];
            Str main, from;
            if (e->kind.Eq(L"note") && e->note)
            {
                main = L"便签「" + e->note->title + L"」";
                from = L"整张便签 · ";
                from.Int(e->note->items.n).Append(L" 条");
            }
            else
            {
                main = e->item.text.Replace(L"\n", L" ");
                from = L"「" + e->noteTitle + L"」";
                if (e->item.done) from += L" · 已完成";
            }
            Str when = FormatTime(e->deletedAt, false);
            LVITEMW it = {};
            it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = i;
            it.pszText = (LPWSTR)main.c();
            it.lParam = (LPARAM)e;
            int idx = ListView_InsertItem(list, &it);
            ListView_SetItemText(list, idx, 1, (LPWSTR)from.c());
            ListView_SetItemText(list, idx, 2, (LPWSTR)when.c());
        }
        EnableWindow(bEmpty, g_app->trash.n > 0);
        UpdateButtons();
    }
    void UpdateButtons()
    {
        bool sel = ListView_GetSelectedCount(list) > 0;
        EnableWindow(bRestore, sel);
        EnableWindow(bPurge, sel);
    }
    void Selected(Vec<TrashEntry*>& out)
    {
        int i = -1;
        while ((i = ListView_GetNextItem(list, i, LVNI_SELECTED)) >= 0)
        {
            LVITEMW it = {};
            it.mask = LVIF_PARAM;
            it.iItem = i;
            ListView_GetItem(list, &it);
            out.Push((TrashEntry*)it.lParam);
        }
    }
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        TrashDlg* self = (TrashDlg*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (m == WM_NCCREATE)
        {
            self = (TrashDlg*)((CREATESTRUCTW*)l)->lpCreateParams;
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(h, m, w, l);
        LRESULT r;
        if (self->Common(m, w, l, &r)) { if (m == WM_DPICHANGED) self->Layout(); return r; }
        switch (m)
        {
        case WM_SIZE: self->Layout(); return 0;
        case WM_GETMINMAXINFO:
            ((MINMAXINFO*)l)->ptMinTrackSize.x = (int)(380 * self->S());
            ((MINMAXINFO*)l)->ptMinTrackSize.y = (int)(300 * self->S());
            return 0;
        case WM_NOTIFY:
        {
            NMHDR* nh = (NMHDR*)l;
            if (nh->idFrom == IdList && nh->code == LVN_ITEMCHANGED) self->UpdateButtons();
            if (nh->idFrom == IdList && nh->code == NM_DBLCLK) SendMessageW(h, WM_COMMAND, IdRestore, 0);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w))
            {
            case IdRestore: case IdPurge:
            {
                Vec<TrashEntry*> sel;
                self->Selected(sel);
                for (int i = 0; i < sel.n; i++)
                    if (g_app->trash.Contains(sel[i])) { if (LOWORD(w) == IdRestore) g_app->Restore(sel[i]); else g_app->Purge(sel[i]); }
                return 0;
            }
            case IdEmpty:
            {
                Str msg(L"彻底删除回收站里的全部 ");
                msg.Int(g_app->trash.n).Append(L" 项？删除后不能恢复。");
                if (MessageBoxW(h, msg.c(), L"清空回收站", MB_OKCANCEL | MB_ICONWARNING) == IDOK) g_app->EmptyTrash();
                return 0;
            }
            case IdClose: case IDCANCEL: DestroyWindow(h); return 0;
            }
            return 0;
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
    TrashDlg* d = new TrashDlg();
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, L"DeskNoteTrash", L"DeskNote 回收站", WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, g_inst, d);
    d->dpi = GetDpiForWindow(h);
    SetIcons(h);
    d->Build();
    d->SizeClient(560, 510);
    d->Center();
    d->Layout();
    return h;
}

void TrashWindowRefresh(HWND w)
{
    TrashDlg* d = (TrashDlg*)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (d) d->Refresh();
}

// =====================================================================
// 设置：开机启动、全局快捷键、数据文件夹、同步
// =====================================================================
enum { IdAuto = 20, IdHkEnable, IdHotkey, IdOpenDir, IdOpenBackup, IdSyncEnable, IdServer, IdKey, IdFolder, IdSyncNow, IdSetClose };
struct SettingsDlg : DlgBase
{
    HWND hkEnable, hotkey, hkStatus, syncEnable, server, key, folder, syncStatus;
    void Build()
    {
        Settings& s = g_app->settings;
        const int W = 470, X = 20, CW = W - 40;
        int y = 14;
        auto section = [&](const wchar_t* t) {
            HWND c = Add(L"STATIC", t, SS_LEFT, X, y, CW, 22, 0);
            MakeBold(c);
            y += 26;
            return c;
        };
        auto hint = [&](const wchar_t* t) {
            int h = TextHeightDip(t, CW);
            Hint(t, X, y, CW, h);
            y += h + 6;
        };
        section(L"启动");
        HWND autoBox = Add(L"BUTTON", g_app->isDefaultDir ? L"开机自动启动" : L"开机自动启动（测试数据目录下不可用）",
                           WS_TABSTOP | BS_AUTOCHECKBOX, X, y, CW, 24, IdAuto);
        Button_SetCheck(autoBox, App::IsAutoStart() ? BST_CHECKED : BST_UNCHECKED);
        if (!g_app->isDefaultDir) EnableWindow(autoBox, FALSE);
        y += 26;
        hint(L"移动 exe 后，打开一次新位置的 exe，开机启动会自动改到新位置。");
        y += 6;

        section(L"全局快捷键");
        hkEnable = Add(L"BUTTON", L"启用", WS_TABSTOP | BS_AUTOCHECKBOX, X, y, CW, 24, IdHkEnable);
        Button_SetCheck(hkEnable, s.hotkeyEnabled ? BST_CHECKED : BST_UNCHECKED);
        y += 28;
        hotkey = Add(HOTKEY_CLASSW, L"", WS_TABSTOP | WS_BORDER, X, y, 200, 26, IdHotkey);
        UINT mods, vk;
        if (HotkeyParse(s.hotkey, &mods, &vk))
        {
            WORD f = (mods & MOD_CONTROL ? HOTKEYF_CONTROL : 0) | (mods & MOD_ALT ? HOTKEYF_ALT : 0) | (mods & MOD_SHIFT ? HOTKEYF_SHIFT : 0);
            SendMessageW(hotkey, HKM_SETHOTKEY, MAKEWORD(vk, f), 0);
        }
        SendMessageW(hotkey, HKM_SETRULES, HKCOMB_NONE | HKCOMB_S, MAKELPARAM(HOTKEYF_CONTROL | HOTKEYF_ALT, 0));
        SetWindowSubclass(hotkey, HotkeySub, 1, (DWORD_PTR)this);
        EnableWindow(hotkey, s.hotkeyEnabled);
        y += 30;
        hkStatus = Add(L"STATIC", L"", SS_LEFT, X, y, CW, 22, 0);
        status = hkStatus;
        y += 24;
        hint(L"点输入框后按下新的组合键。按快捷键或单击托盘图标，会把所有便签临时提到最前面；再按一次或点别处恢复。");
        y += 6;

        section(L"数据");
        Add(L"EDIT", Store::Dir.c(), ES_READONLY | ES_AUTOHSCROLL, X, y + 3, CW - 100, 24, 0);
        Add(L"BUTTON", L"打开", WS_TABSTOP | BS_PUSHBUTTON, X + CW - 90, y, 90, 28, IdOpenDir);
        y += 32;
        hint(L"每张便签一个 json 文件，在 notes 文件夹里；回收站是 trash.json。");
        Str bh(L"每天自动备份一次，保留最近 ");
        bh.Int(Store::KeepBackups).Append(L" 天。要找回某天的内容，把备份里的文件复制回 notes 文件夹即可。");
        int bhh = TextHeightDip(bh.c(), CW - 100);
        Hint(bh.c(), X, y, CW - 100, bhh);
        Add(L"BUTTON", L"打开备份", WS_TABSTOP | BS_PUSHBUTTON, X + CW - 90, y, 90, 28, IdOpenBackup);
        y += Max(bhh, 30) + 12;

        section(L"同步到 gopan");
        syncEnable = Add(L"BUTTON", L"启用同步（多台电脑共用这些便签）", WS_TABSTOP | BS_AUTOCHECKBOX, X, y, CW, 24, IdSyncEnable);
        Button_SetCheck(syncEnable, s.syncEnabled ? BST_CHECKED : BST_UNCHECKED);
        y += 30;
        Add(L"STATIC", L"服务器", SS_LEFT, X, y + 4, 56, 22, 0);
        server = Add(L"EDIT", s.syncServer.c(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, X + 60, y, CW - 60, 26, IdServer);
        y += 32;
        Add(L"STATIC", L"密钥", SS_LEFT, X, y + 4, 56, 22, 0);
        key = Add(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD, X + 60, y, CW - 60, 26, IdKey);
        if (s.syncKey.n) SendMessageW(key, EM_SETCUEBANNER, TRUE, (LPARAM)L"已保存（不改就留空）");
        y += 32;
        Add(L"STATIC", L"文件夹", SS_LEFT, X, y + 4, 56, 22, 0);
        folder = Add(L"EDIT", s.syncFolder.c(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, X + 60, y, CW - 60, 26, IdFolder);
        y += 34;
        Add(L"BUTTON", L"保存并立即同步", WS_TABSTOP | BS_PUSHBUTTON, X, y, 130, 28, IdSyncNow);
        y += 34;
        syncStatus = Hint(L"", X, y, CW, 22);
        y += 26;
        hint(L"在 gopan 网页的「Agent 接入」里新建一把密钥，权限勾选：浏览与搜索、读取与下载内容、新建重命名移动与复制、移入回收站与恢复。"
             L"每张便签存成文件夹里的一个 json；只同步内容和外观，窗口位置、回收站各台电脑自己管。"
             L"两台电脑同时改了同一张便签时，本地那份会另存成一张「冲突副本」，不会丢。");
        y += 8;
        Str ver(L"DeskNote " APP_VERSION L"（原生版）· 编译于 ");
        const char* date = __DATE__ " " __TIME__;
        ver += FromUtf8(date, (int)strlen_(date));
        Hint(ver.c(), X, y + 6, CW - 100, 22);
        Add(L"BUTTON", L"关闭", WS_TABSTOP | BS_PUSHBUTTON, X + CW - 90, y, 90, 28, IdSetClose);
        y += 44;
        SizeClient(W, y);
        ShowHotkeyStatus(g_app->ApplyHotkey());
        RefreshSync();
    }
    static int strlen_(const char* s) { int n = 0; while (s[n]) n++; return n; }
    void ShowHotkeyStatus(const wchar_t* err)
    {
        if (!g_app->settings.hotkeyEnabled) { statusColor = RGB(0x77, 0x77, 0x77); SetWindowTextW(hkStatus, L"已停用"); }
        else if (err) { statusColor = RGB(0xCD, 0x5C, 0x5C); SetWindowTextW(hkStatus, err); }
        else { statusColor = RGB(0x2E, 0x8B, 0x57); SetWindowTextW(hkStatus, (L"已生效：" + g_app->settings.hotkey).c()); }
        InvalidateRect(hkStatus, nullptr, TRUE);
    }
    void RefreshSync()
    {
        SetWindowTextW(syncStatus, SyncStatus(g_app->sync).c());
        InvalidateRect(syncStatus, nullptr, TRUE);
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
    // 录快捷键时先停用当前快捷键，否则按下去会被系统直接截走
    static LRESULT CALLBACK HotkeySub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref)
    {
        SettingsDlg* self = (SettingsDlg*)ref;
        if (m == WM_SETFOCUS)
        {
            UnregisterHotKey(g_app->msgWnd, 1);
            self->statusColor = RGB(0x77, 0x77, 0x77);
            SetWindowTextW(self->hkStatus, L"请按下新的组合键…");
        }
        else if (m == WM_KILLFOCUS) self->ShowHotkeyStatus(g_app->ApplyHotkey());
        return DefSubclassProc(h, m, w, l);
    }
    void OnHotkeyChanged()
    {
        LRESULT v = SendMessageW(hotkey, HKM_GETHOTKEY, 0, 0);
        UINT vk = LOBYTE(LOWORD(v)), f = HIBYTE(LOWORD(v));
        UINT mods = (f & HOTKEYF_CONTROL ? MOD_CONTROL : 0) | (f & HOTKEYF_ALT ? MOD_ALT : 0) | (f & HOTKEYF_SHIFT ? MOD_SHIFT : 0);
        if (!vk || !(mods & (MOD_CONTROL | MOD_ALT)))
        {
            statusColor = RGB(0xCD, 0x5C, 0x5C);
            SetWindowTextW(hkStatus, L"要配合 Ctrl 或 Alt 一起按");
            InvalidateRect(hkStatus, nullptr, TRUE);
            return;
        }
        g_app->settings.hotkey = HotkeyFormat(mods, vk);
        Store::SaveSettings(g_app->settings);
        statusColor = RGB(0x77, 0x77, 0x77);
        SetWindowTextW(hkStatus, (L"已记录：" + g_app->settings.hotkey + L"，点别处后生效").c());
        InvalidateRect(hkStatus, nullptr, TRUE);
    }
    void SaveSync()
    {
        Settings& s = g_app->settings;
        s.syncEnabled = Button_GetCheck(syncEnable) == BST_CHECKED;
        Str sv = GetText(server).Trim();
        while (sv.EndsWith(L"/")) sv.Truncate(sv.n - 1);
        if (sv.n && !sv.StartsWith(L"http")) sv = L"https://" + sv;
        s.syncServer = sv;
        SetWindowTextW(server, sv.c());
        s.syncFolder = SyncNormFolder(GetText(folder));
        SetWindowTextW(folder, s.syncFolder.c());
        Str k = GetText(key).Trim();
        if (k.n) { s.syncKey = k; SetWindowTextW(key, L""); SendMessageW(key, EM_SETCUEBANNER, TRUE, (LPARAM)L"已保存（不改就留空）"); }
        Store::SaveSettings(s);
        SyncStart(g_app->sync);
        SyncNow(g_app->sync);
        RefreshSync();
    }
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        SettingsDlg* self = (SettingsDlg*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (m == WM_NCCREATE)
        {
            self = (SettingsDlg*)((CREATESTRUCTW*)l)->lpCreateParams;
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(h, m, w, l);
        LRESULT r;
        if (m == WM_CTLCOLORSTATIC && (HWND)l == self->syncStatus)
        {
            SetBkMode((HDC)w, TRANSPARENT);
            SetTextColor((HDC)w, SyncFailed(g_app->sync) ? RGB(0xCD, 0x5C, 0x5C) : RGB(0x77, 0x77, 0x77));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        if (self->Common(m, w, l, &r)) return r;
        switch (m)
        {
        case WM_COMMAND:
            switch (LOWORD(w))
            {
            case IdAuto: App::SetAutoStart(Button_GetCheck((HWND)l) == BST_CHECKED); return 0;
            case IdHkEnable:
                g_app->settings.hotkeyEnabled = Button_GetCheck((HWND)l) == BST_CHECKED;
                Store::SaveSettings(g_app->settings);
                EnableWindow(self->hotkey, g_app->settings.hotkeyEnabled);
                self->ShowHotkeyStatus(g_app->ApplyHotkey());
                return 0;
            case IdHotkey: if (HIWORD(w) == EN_CHANGE) self->OnHotkeyChanged(); return 0;
            case IdOpenDir: ShellExecuteW(nullptr, L"open", L"explorer.exe", Store::Dir.c(), nullptr, SW_SHOWNORMAL); return 0;
            case IdOpenBackup:
                EnsureDir(Store::BackupsDir.c());
                ShellExecuteW(nullptr, L"open", L"explorer.exe", Store::BackupsDir.c(), nullptr, SW_SHOWNORMAL);
                return 0;
            case IdSyncNow: self->SaveSync(); return 0;
            case IdSetClose: case IDCANCEL: DestroyWindow(h); return 0;
            }
            return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY:
            g_app->ApplyHotkey();
            g_app->settingsWnd = nullptr;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            delete self;
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

HWND SettingsWindowCreate()
{
    SettingsDlg* d = new SettingsDlg();
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, L"DeskNoteSettings", L"DeskNote 设置", WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, g_inst, d);
    d->dpi = GetDpiForWindow(h);
    SetIcons(h);
    d->Build();
    d->Center();
    return h;
}

void SettingsWindowSyncStatus(HWND w)
{
    SettingsDlg* d = (SettingsDlg*)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (d) d->RefreshSync();
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
// 设置提醒：常用快捷选项 + 日期 + 时间
// =====================================================================
enum { IdQuick = 40, IdDate = 50, IdTime, IdOk, IdCancel };
struct ReminderDlg : DlgBase
{
    bool done = false, ok = false;
    Time result = 0;
    Time quick[4];
    int quickN = 0;
    HWND date, time;
    void Build(const Str& itemText, bool hasCurrent, Time current)
    {
        const int W = 400, X = 20, CW = W - 40;
        int y = 16;
        Str shown = itemText.Replace(L"\n", L" ");
        if (shown.n > 80) { shown.Truncate(80); shown += L"…"; }
        int th = TextHeightDip(shown.c(), CW);
        Add(L"STATIC", shown.c(), SS_LEFT | SS_NOPREFIX, X, y, CW, th, 0);
        y += th + 12;
        SYSTEMTIME now;
        GetLocalTime(&now);
        Time t0 = Now();
        Time minuteNow = MakeTime(now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, 0);
        const wchar_t* labels[4];
        quick[quickN] = minuteNow + 30 * TicksPerMinute; labels[quickN++] = L"30 分钟后";
        quick[quickN] = minuteNow + 60 * TicksPerMinute; labels[quickN++] = L"1 小时后";
        Time tonight = Today() + 20 * 60 * TicksPerMinute;
        if (tonight > t0 + 5 * TicksPerMinute) { quick[quickN] = tonight; labels[quickN++] = L"今晚 20:00"; }
        quick[quickN] = Today() + TicksPerDay + 9 * 60 * TicksPerMinute; labels[quickN++] = L"明早 9:00";
        int x = X;
        for (int i = 0; i < quickN; i++) { Add(L"BUTTON", labels[i], WS_TABSTOP | BS_PUSHBUTTON, x, y, 84, 28, IdQuick + i); x += 90; }
        y += 40;
        date = Add(DATETIMEPICK_CLASSW, L"", WS_TABSTOP | WS_BORDER | DTS_SHORTDATEFORMAT, X, y, 160, 28, IdDate);
        time = Add(DATETIMEPICK_CLASSW, L"", WS_TABSTOP | WS_BORDER | DTS_TIMEFORMAT | DTS_UPDOWN, X + 170, y, 110, 28, IdTime);
        SendMessageW(time, DTM_SETFORMATW, 0, (LPARAM)L"HH':'mm");
        SYSTEMTIME st;
        SplitTime(hasCurrent ? current : minuteNow + 60 * TicksPerMinute, &st);
        DateTime_SetSystemtime(date, GDT_VALID, &st);
        DateTime_SetSystemtime(time, GDT_VALID, &st);
        y += 40;
        const wchar_t* h = L"到点后会弹出系统通知，条目上的提醒时间变红。程序没开着时错过的提醒，下次打开时补上。";
        int hh = TextHeightDip(h, CW);
        Hint(h, X, y, CW, hh);
        y += hh + 16;
        Add(L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, X + CW - 176, y, 84, 28, IdOk);
        Add(L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON, X + CW - 84, y, 84, 28, IdCancel);
        y += 44;
        SizeClient(W, y);
        Center();
    }
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        ReminderDlg* self = (ReminderDlg*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (m == WM_NCCREATE)
        {
            self = (ReminderDlg*)((CREATESTRUCTW*)l)->lpCreateParams;
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(h, m, w, l);
        LRESULT r;
        if (self->Common(m, w, l, &r)) return r;
        if (m == WM_COMMAND)
        {
            int id = LOWORD(w);
            if (id >= IdQuick && id < IdQuick + self->quickN) { self->result = self->quick[id - IdQuick]; self->ok = true; self->done = true; }
            else if (id == IdOk || id == IDOK)
            {
                SYSTEMTIME d, t;
                DateTime_GetSystemtime(self->date, &d);
                DateTime_GetSystemtime(self->time, &t);
                self->result = MakeTime(d.wYear, d.wMonth, d.wDay, t.wHour, t.wMinute, 0);
                self->ok = true;
                self->done = true;
            }
            else if (id == IdCancel || id == IDCANCEL) self->done = true;
            return 0;
        }
        if (m == WM_CLOSE) { self->done = true; return 0; }
        return DefWindowProcW(h, m, w, l);
    }
};

bool ReminderDialog(HWND owner, const Str& itemText, bool hasCurrent, Time current, Time* result)
{
    ReminderDlg d;
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_TOPMOST, L"DeskNoteReminder", L"设置提醒", WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, owner, nullptr, g_inst, &d);
    d.dpi = GetDpiForWindow(h);
    SetIcons(h);
    d.Build(itemText, hasCurrent, current);
    RunModal(h, owner, &d.done);
    if (d.ok) *result = d.result;
    return d.ok;
}

// =====================================================================
// 不透明度和字号（拖动时实时预览）
// =====================================================================
enum { IdOpacity = 60, IdFont, IdOpacityVal, IdFontVal, IdAppClose };
struct AppearanceDlg : DlgBase
{
    bool done = false;
    NoteData* d = nullptr;
    void (*apply)(void*) = nullptr;
    void* ctx = nullptr;
    HWND opacity, font, opacityVal, fontVal;
    void Build()
    {
        const int W = 360, X = 20;
        int y = 18;
        Add(L"STATIC", L"不透明度", SS_LEFT, X, y + 4, 64, 22, 0);
        opacity = Add(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, X + 66, y, 200, 30, IdOpacity);
        SendMessageW(opacity, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(opacity, TBM_SETPOS, TRUE, (LPARAM)(int)(d->opacity * 100 + 0.5));
        opacityVal = Add(L"STATIC", L"", SS_RIGHT, X + 270, y + 4, 50, 22, IdOpacityVal);
        y += 40;
        Add(L"STATIC", L"字号", SS_LEFT, X, y + 4, 64, 22, 0);
        font = Add(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, X + 66, y, 200, 30, IdFont);
        SendMessageW(font, TBM_SETRANGE, TRUE, MAKELPARAM(10, 40));
        SendMessageW(font, TBM_SETPOS, TRUE, (LPARAM)(int)(d->fontSize + 0.5));
        fontVal = Add(L"STATIC", L"", SS_RIGHT, X + 270, y + 4, 50, 22, IdFontVal);
        y += 46;
        Add(L"BUTTON", L"关闭", WS_TABSTOP | BS_DEFPUSHBUTTON, W - 20 - 84, y, 84, 28, IdAppClose);
        y += 44;
        SizeClient(W, y);
        Center();
        UpdateLabels();
    }
    void UpdateLabels()
    {
        Str a;
        a.Int((int)SendMessageW(opacity, TBM_GETPOS, 0, 0)).Ch(L'%');
        SetWindowTextW(opacityVal, a.c());
        Str f;
        f.Int((int)SendMessageW(font, TBM_GETPOS, 0, 0));
        SetWindowTextW(fontVal, f.c());
    }
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        AppearanceDlg* self = (AppearanceDlg*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (m == WM_NCCREATE)
        {
            self = (AppearanceDlg*)((CREATESTRUCTW*)l)->lpCreateParams;
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(h, m, w, l);
        LRESULT r;
        if (self->Common(m, w, l, &r)) return r;
        switch (m)
        {
        case WM_HSCROLL:
            self->d->opacity = SendMessageW(self->opacity, TBM_GETPOS, 0, 0) / 100.0;
            self->d->fontSize = (double)SendMessageW(self->font, TBM_GETPOS, 0, 0);
            self->UpdateLabels();
            self->apply(self->ctx);
            return 0;
        case WM_COMMAND:
            if (LOWORD(w) == IdAppClose || LOWORD(w) == IDCANCEL || LOWORD(w) == IDOK) self->done = true;
            return 0;
        case WM_CLOSE: self->done = true; return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

void AppearanceDialog(HWND owner, NoteData* d, void (*apply)(void*), void* ctx)
{
    AppearanceDlg dlg;
    dlg.d = d;
    dlg.apply = apply;
    dlg.ctx = ctx;
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_TOPMOST, L"DeskNoteAppearance", L"不透明度和字号", WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, owner, nullptr, g_inst, &dlg);
    dlg.dpi = GetDpiForWindow(h);
    SetIcons(h);
    dlg.Build();
    RunModal(h, owner, &dlg.done);
}

void RegisterDialogClasses()
{
    WNDCLASSW wc = {};
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpfnWndProc = TrashDlg::Proc; wc.lpszClassName = L"DeskNoteTrash"; RegisterClassW(&wc);
    wc.lpfnWndProc = SettingsDlg::Proc; wc.lpszClassName = L"DeskNoteSettings"; RegisterClassW(&wc);
    wc.lpfnWndProc = ReminderDlg::Proc; wc.lpszClassName = L"DeskNoteReminder"; RegisterClassW(&wc);
    wc.lpfnWndProc = AppearanceDlg::Proc; wc.lpszClassName = L"DeskNoteAppearance"; RegisterClassW(&wc);
}
