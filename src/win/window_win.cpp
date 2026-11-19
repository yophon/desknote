// 窗口接口（ui/window.h）的 Windows 实现：对话框窗口、原生输入框、模态循环、确认框
#include "ui.h"
#include "window_win.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <imm.h>

struct TextField
{
    HWND hwnd = nullptr;
    Window* win = nullptr;
    HFONT font = nullptr;
    int lineH = 0;              // 字体行高（物理像素）
    float size = 14;
    u32 text = 0, bgRest = 0, bgFocus = 0;
    HBRUSH rest = nullptr, focus = nullptr;
};

struct Window
{
    HWND hwnd = nullptr;
    WinHandler* h = nullptr;
    int dpi = 96;
    bool tracking = false, allKeys = false;
    float minW = 0, minH = 0;
    HWND owner = nullptr;
    Vec<TextField*> fields;
};

static const wchar_t* DialogClass = L"DeskNoteDialog";
static Vec<Window*> g_dialogs; // 消息循环里要做对话框导航的窗口

static float ScaleOf(Window* w) { return w->dpi / 96.f; }

static int CurrentMods()
{
    int m = 0;
    if (GetKeyState(VK_SHIFT) < 0) m |= ModShift;
    if (GetKeyState(VK_CONTROL) < 0) m |= ModCtrl;
    if (GetKeyState(VK_MENU) < 0) m |= ModAlt;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) m |= ModWin;
    return m;
}

// 标题栏跟随深浅色，颜色和内容一样
static void ApplyFrameTheme(HWND h)
{
    BOOL dark = Ui::T.dark;
    DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF cap = Ui::Ref(Ui::T.winBg);
    DwmSetWindowAttribute(h, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap));
}

static TextField* FieldOf(Window* w, HWND h)
{
    for (int i = 0; i < w->fields.n; i++) if (w->fields[i]->hwnd == h) return w->fields[i];
    return nullptr;
}

static void FieldMakeFont(TextField* f)
{
    if (f->font) DeleteObject(f->font);
    f->font = CreateFontW(-(int)(f->size * ScaleOf(f->win) + 0.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    HDC dc = GetDC(f->hwnd);
    HGDIOBJ of = SelectObject(dc, f->font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    f->lineH = tm.tmHeight;
    SelectObject(dc, of);
    ReleaseDC(f->hwnd, dc);
    SendMessageW(f->hwnd, WM_SETFONT, (WPARAM)f->font, TRUE);
}

static bool Send(Window* w, const Event& e) { return w->h ? w->h->OnEvent(w, e) : false; }

static void MouseEvent(Window* w, int type, int button, int clicks, LPARAM lp)
{
    Event e;
    e.type = type;
    e.button = button;
    e.clicks = clicks;
    e.x = GET_X_LPARAM(lp) / ScaleOf(w);
    e.y = GET_Y_LPARAM(lp) / ScaleOf(w);
    e.mods = CurrentMods();
    Send(w, e);
}

static LRESULT CALLBACK DialogProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    Window* w = (Window*)GetWindowLongPtrW(hw, GWLP_USERDATA);
    if (m == WM_NCCREATE)
    {
        w = (Window*)((CREATESTRUCTW*)lp)->lpCreateParams;
        w->hwnd = hw;
        SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)w);
    }
    if (!w) return DefWindowProcW(hw, m, wp, lp);
    if (m == WM_NCDESTROY)
    {
        SetWindowLongPtrW(hw, GWLP_USERDATA, 0);
        for (int i = 0; i < w->fields.n; i++)
        {
            TextField* f = w->fields[i];
            if (f->font) DeleteObject(f->font);
            if (f->rest) DeleteObject(f->rest);
            if (f->focus) DeleteObject(f->focus);
            delete f;
        }
        delete w;
        return DefWindowProcW(hw, m, wp, lp);
    }
    if (!w->h) return DefWindowProcW(hw, m, wp, lp);
    Event e;
    switch (m)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        RECT rc;
        GetClientRect(hw, &rc);
        Canvas c;
        if (c.Begin(rc.right, rc.bottom, (float)w->dpi, true)) w->h->Paint(w, c.rt, rc.right / ScaleOf(w), rc.bottom / ScaleOf(w));
        c.EndBlt(dc);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        e.type = EvResize;
        Send(w, e);
        return 0;
    case WM_GETMINMAXINFO:
        if (w->minW > 0) ((MINMAXINFO*)lp)->ptMinTrackSize.x = (int)(w->minW * ScaleOf(w));
        if (w->minH > 0) ((MINMAXINFO*)lp)->ptMinTrackSize.y = (int)(w->minH * ScaleOf(w));
        return 0;
    case WM_CTLCOLOREDIT:
    {
        TextField* f = FieldOf(w, (HWND)lp);
        if (!f) break;
        bool focused = GetFocus() == f->hwnd;
        u32 bg = focused ? f->bgFocus : f->bgRest;
        SetTextColor((HDC)wp, Ui::Ref(Ui::Solid(f->text, bg)));
        SetBkColor((HDC)wp, Ui::Ref(bg));
        return (LRESULT)(focused ? f->focus : f->rest);
    }
    case WM_MOUSEMOVE:
        if (!w->tracking)
        {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hw, 0 };
            TrackMouseEvent(&t);
            w->tracking = true;
        }
        MouseEvent(w, EvMouseMove, 0, 0, lp);
        return 0;
    case WM_MOUSELEAVE:
        w->tracking = false;
        e.type = EvMouseLeave;
        Send(w, e);
        return 0;
    case WM_LBUTTONDOWN: MouseEvent(w, EvMouseDown, MouseLeft, 1, lp); return 0;
    case WM_LBUTTONDBLCLK: MouseEvent(w, EvMouseDown, MouseLeft, 2, lp); return 0;
    case WM_LBUTTONUP: MouseEvent(w, EvMouseUp, MouseLeft, 0, lp); return 0;
    case WM_RBUTTONDOWN: MouseEvent(w, EvMouseDown, MouseRight, 1, lp); return 0;
    case WM_RBUTTONUP: MouseEvent(w, EvMouseUp, MouseRight, 0, lp); return 0;
    case WM_MOUSEWHEEL:
    {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hw, &p);
        e.type = EvWheel;
        e.wheel = GET_WHEEL_DELTA_WPARAM(wp) / 120.f;
        e.x = p.x / ScaleOf(w);
        e.y = p.y / ScaleOf(w);
        e.mods = CurrentMods();
        Send(w, e);
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        e.type = EvKeyDown;
        e.key = (int)wp;
        e.mods = CurrentMods();
        if (Send(w, e)) return 0;
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        e.type = EvKeyUp;
        e.key = (int)wp;
        e.mods = CurrentMods();
        if (Send(w, e)) return 0;
        break;
    case WM_CHAR: case WM_SYSCHAR:
        if (w->allKeys) return 0;
        e.type = EvChar;
        e.ch = (u32)wp;
        if (Send(w, e)) return 0;
        break;
    case WM_SYSCOMMAND:
        if (w->allKeys && (wp & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        e.type = EvFocus;
        e.button = m == WM_SETFOCUS;
        Send(w, e);
        return 0;
    case WM_ACTIVATE:
        e.type = EvActivate;
        e.button = LOWORD(wp) != WA_INACTIVE;
        Send(w, e);
        break;
    case WM_COMMAND:
    {
        int id = LOWORD(wp), code = HIWORD(wp);
        TextField* f = lp ? FieldOf(w, (HWND)lp) : nullptr;
        if (f)
        {
            if (code == EN_CHANGE) e.type = EvFieldChange;
            else if (code == EN_SETFOCUS) e.type = EvFieldFocus;
            else if (code == EN_KILLFOCUS) e.type = EvFieldBlur;
            else return 0;
            if (code != EN_CHANGE) InvalidateRect(f->hwnd, nullptr, TRUE); // 换底色
            e.field = f;
            Send(w, e);
            return 0;
        }
        if (id == IDOK)
        {
            e.type = EvSubmit;
            e.field = FieldOf(w, GetFocus());
            Send(w, e);
            return 0;
        }
        if (id == IDCANCEL)
        {
            e.type = EvClose;
            e.key = KeyEscape;
            if (!Send(w, e)) DestroyWindow(hw);
            return 0;
        }
        return 0;
    }
    case WM_DPICHANGED:
    {
        w->dpi = HIWORD(wp);
        for (int i = 0; i < w->fields.n; i++) FieldMakeFont(w->fields[i]);
        RECT* rc = (RECT*)lp;
        SetWindowPos(hw, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        e.type = EvScale;
        Send(w, e);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && WEq((const wchar_t*)lp, L"ImmersiveColorSet"))
        {
            Ui::LoadTheme();
            ApplyFrameTheme(hw);
            for (int i = 0; i < w->fields.n; i++) Ui::AllowDarkControls(w->fields[i]->hwnd);
            e.type = EvTheme;
            Send(w, e);
        }
        break;
    case WM_CLOSE:
        e.type = EvClose;
        if (!Send(w, e)) DestroyWindow(hw);
        return 0;
    case WM_DESTROY:
        g_dialogs.Remove(w);
        e.type = EvDestroy;
        Send(w, e);
        w->h = nullptr;
        return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

namespace Wnd
{
    void RegisterClasses()
    {
        WNDCLASSW wc = {};
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = DialogProc;
        wc.lpszClassName = DialogClass;
        RegisterClassW(&wc);
    }

    bool PreTranslate(MSG* msg)
    {
        for (int i = 0; i < g_dialogs.n; i++)
        {
            Window* w = g_dialogs[i];
            if (!w->allKeys && IsDialogMessageW(w->hwnd, msg)) return true;
        }
        return false;
    }

    Window* NewDialog(const WinDesc& d, WinHandler* h)
    {
        Ui::LoadTheme();
        Window* w = new Window();
        w->h = h;
        w->minW = d.minW;
        w->minH = d.minH;
        w->owner = (HWND)d.owner;
        DWORD style = WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        if (d.resizable) style |= WS_THICKFRAME;
        if (d.minimizable) style |= WS_MINIMIZEBOX;
        DWORD ex = WS_EX_CONTROLPARENT;
        if (d.topmost) ex |= WS_EX_TOPMOST;
        if (d.owner) ex |= WS_EX_DLGMODALFRAME;
        HWND hw = CreateWindowExW(ex, DialogClass, d.title.c(), style, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, (HWND)d.owner, nullptr, g_inst, w);
        if (!hw) { delete w; return nullptr; }
        w->dpi = GetDpiForWindow(hw);
        if (d.appIcon)
        {
            SendMessageW(hw, WM_SETICON, ICON_BIG, (LPARAM)g_app->icon);
            SendMessageW(hw, WM_SETICON, ICON_SMALL, (LPARAM)g_app->iconSmall);
        }
        ApplyFrameTheme(hw);
        if (!d.owner) g_dialogs.Push(w); // 模态的在自己的循环里做导航
        return w;
    }

    void PlaceCentered(Window* w, float width, float height, float maxFrac, float marginDip)
    {
        POINT p;
        GetCursorPos(&p);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
        float S = ScaleOf(w);
        int maxH = (int)((mi.rcWork.bottom - mi.rcWork.top) * maxFrac) - (int)(marginDip * S);
        RECT r = { 0, 0, (int)(width * S + 0.5f), Min((int)(height * S + 0.5f), maxH) };
        AdjustWindowRectExForDpi(&r, GetWindowLongW(w->hwnd, GWL_STYLE), FALSE, GetWindowLongW(w->hwnd, GWL_EXSTYLE), w->dpi);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        SetWindowPos(w->hwnd, nullptr, (mi.rcWork.left + mi.rcWork.right - ww) / 2, (mi.rcWork.top + mi.rcWork.bottom - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void RunModal(Window* w, bool* done)
    {
        HWND dlg = w->hwnd, owner = w->owner;
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

    void Destroy(Window* w) { if (w) DestroyWindow(w->hwnd); }
    void Show(Window* w) { ShowWindow(w->hwnd, SW_SHOW); SetForegroundWindow(w->hwnd); }
    bool IsVisible(Window* w) { return IsWindowVisible(w->hwnd) != FALSE; }
    void Invalidate(Window* w) { InvalidateRect(w->hwnd, nullptr, FALSE); }
    void Update(Window* w) { UpdateWindow(w->hwnd); }
    void Focus(Window* w) { if (GetFocus() != w->hwnd) SetFocus(w->hwnd); }
    float Scale(Window* w) { return ScaleOf(w); }
    void ClientSize(Window* w, float* width, float* height)
    {
        RECT rc;
        GetClientRect(w->hwnd, &rc);
        if (width) *width = rc.right / ScaleOf(w);
        if (height) *height = rc.bottom / ScaleOf(w);
    }
    RectI ScreenBounds(Window* w)
    {
        RECT r;
        GetWindowRect(w->hwnd, &r);
        RectI o = { r.left, r.top, r.right, r.bottom };
        return o;
    }
    PointI ToScreen(Window* w, float x, float y)
    {
        POINT p = { (LONG)(x * ScaleOf(w)), (LONG)(y * ScaleOf(w)) };
        ClientToScreen(w->hwnd, &p);
        PointI o = { p.x, p.y };
        return o;
    }
    bool CursorPos(Window* w, float* x, float* y)
    {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(w->hwnd, &p);
        RECT rc;
        GetClientRect(w->hwnd, &rc);
        *x = p.x / ScaleOf(w);
        *y = p.y / ScaleOf(w);
        return PtInRect(&rc, p) != FALSE;
    }
    void SetTimer(Window* w, int id, int ms) { ::SetTimer(w->hwnd, (UINT_PTR)id, (UINT)ms, nullptr); }
    void KillTimer(Window* w, int id) { ::KillTimer(w->hwnd, (UINT_PTR)id); }
    void Capture(Window* w, bool on)
    {
        if (on) SetCapture(w->hwnd);
        else if (GetCapture() == w->hwnd) ReleaseCapture();
    }
    void SetCursor(CursorShape c)
    {
        static const LPCWSTR ids[] = { IDC_ARROW, IDC_HAND, IDC_IBEAM, IDC_SIZEALL, IDC_SIZEWE, IDC_SIZENS, IDC_SIZENWSE, IDC_SIZENESW };
        ::SetCursor(LoadCursorW(nullptr, ids[c]));
    }
    void WantAllKeys(Window* w, bool on) { w->allKeys = on; }
    void* Native(Window* w) { return w ? w->hwnd : nullptr; }
    WinHandler* Handler(Window* w) { return w ? w->h : nullptr; }

    // ---- 原生输入框 ----
    TextField* CreateField(Window* w, const Str& text, int flags)
    {
        TextField* f = new TextField();
        f->win = w;
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
        if (flags & FieldCenter) style |= ES_CENTER;
        if (flags & FieldPassword) style |= ES_PASSWORD;
        f->hwnd = CreateWindowExW(0, L"EDIT", text.c(), style, 0, 0, 10, 10, w->hwnd, (HMENU)(INT_PTR)(100 + w->fields.n), g_inst, nullptr);
        Ui::AllowDarkControls(f->hwnd);
        if (flags & FieldNoIme) ImmAssociateContextEx(f->hwnd, nullptr, 0);
        w->fields.Push(f);
        FieldStyle(f, 14, Ui::T.text, Ui::T.inputBg, Ui::T.inputFocusBg);
        return f;
    }
    void FieldStyle(TextField* f, float fontSize, u32 textColor, u32 bgRest, u32 bgFocus)
    {
        f->text = textColor;
        f->bgRest = bgRest;
        f->bgFocus = bgFocus;
        if (f->rest) DeleteObject(f->rest);
        if (f->focus) DeleteObject(f->focus);
        f->rest = CreateSolidBrush(Ui::Ref(bgRest));
        f->focus = CreateSolidBrush(Ui::Ref(bgFocus));
        if (fontSize != f->size || !f->font) { f->size = fontSize; FieldMakeFont(f); }
        InvalidateRect(f->hwnd, nullptr, TRUE);
    }
    void FieldSetBounds(TextField* f, RectF r)
    {
        float s = ScaleOf(f->win);
        int x = (int)(r.left * s), w = (int)(r.right * s) - x;
        int cy = (int)((r.top + r.bottom) / 2 * s);
        RECT cur;
        GetWindowRect(f->hwnd, &cur);
        MapWindowPoints(nullptr, f->win->hwnd, (POINT*)&cur, 2);
        if (cur.left != x || cur.top != cy - f->lineH / 2 || cur.right - cur.left != w || cur.bottom - cur.top != f->lineH)
            MoveWindow(f->hwnd, x, cy - f->lineH / 2, w, f->lineH, TRUE);
    }
    void FieldSetText(TextField* f, const Str& text) { SetWindowTextW(f->hwnd, text.c()); }
    Str FieldText(TextField* f)
    {
        int n = GetWindowTextLengthW(f->hwnd);
        Str s;
        s.Reserve(n);
        s.n = GetWindowTextW(f->hwnd, s.p, n + 1);
        s.p[s.n] = 0;
        return s;
    }
    void FieldFocus(TextField* f) { SetFocus(f->hwnd); }
    bool FieldHasFocus(TextField* f) { return GetFocus() == f->hwnd; }
    void FieldSelectAll(TextField* f) { SendMessageW(f->hwnd, EM_SETSEL, 0, -1); }
    void FieldSetCue(TextField* f, const Str& cue) { SendMessageW(f->hwnd, EM_SETCUEBANNER, TRUE, (LPARAM)cue.c()); }

    bool Confirm(Window* owner, const Str& title, const Str& text)
    {
        return MessageBoxW(owner ? owner->hwnd : nullptr, text.c(), title.c(), MB_OKCANCEL | MB_ICONWARNING) == IDOK;
    }
}
