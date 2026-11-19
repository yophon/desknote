// 窗口接口（ui/window.h）的 Windows 实现：对话框窗口、原生输入框、模态循环、确认框
#include "ui.h"
#include "window_win.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <commdlg.h>

struct TextField
{
    HWND hwnd = nullptr;
    Window* win = nullptr;
    HFONT font = nullptr;
    int lineH = 0;              // 字体行高（物理像素）
    float size = 14;
    int fontKind = FontNote, weight = 400;
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
    Canvas* canvas = nullptr;  // 弹出窗口、便签窗口：BeginPaint 到 EndPaint 之间
    int kind = 0;              // 0 对话框，1 弹出窗口，2 便签窗口
    int level = 0;             // 便签窗口的层级
    bool peek = false;
    bool moving = false, userResizing = false; // 在系统的拖动 / 改大小循环里；其中真的在拖边框
};

static const wchar_t* DialogClass = L"DeskNoteDialog";
static const wchar_t* PopupClass = L"DeskNotePopup";
static const wchar_t* NoteClass = L"DeskNoteNote";
static Vec<Window*> g_dialogs; // 消息循环里要做对话框导航的窗口

// 每次都问系统：窗口挪到缩放不同的屏上后，系统先改窗口的 DPI 再发 WM_DPICHANGED，
// 只在创建时记一次的话，便签挪到别的屏上会一直按原来那块屏的比例画
static float ScaleOf(Window* w)
{
    UINT dpi = w->hwnd ? GetDpiForWindow(w->hwnd) : 0;
    if (dpi) w->dpi = (int)dpi;
    return w->dpi / 96.f;
}

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
    // 便签字体用微软雅黑 UI，界面字体用 Segoe UI（系统输入框画不了可变字体）
    f->font = CreateFontW(-(int)(f->size * ScaleOf(f->win) + 0.5f), 0, 0, 0, f->weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                          f->fontKind == FontUi ? L"Segoe UI" : L"Microsoft YaHei UI");
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

// =====================================================================
// 弹出窗口和弹出会话
// =====================================================================
static Wnd::PopupLoop* g_loop;
static HHOOK g_hook;
static HWND g_loopFg, g_loopRoot;
enum { TimerForeground = 0x7F01 };

static LRESULT CALLBACK PopupProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
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
        delete w->canvas;
        delete w;
        return DefWindowProcW(hw, m, wp, lp);
    }
    Event e;
    switch (m)
    {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && w->h)
        {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hw, &p);
            Wnd::SetCursor((CursorShape)w->h->CursorAt(w, p.x / ScaleOf(w), p.y / ScaleOf(w)));
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (wp == TimerForeground)
        {
            // 别的程序抢到前台（按了 Win 键、Alt+Tab 等）：收起
            if (g_loop && !g_loop->keepOnForegroundChange)
            {
                HWND f = GetForegroundWindow();
                DWORD pid = 0;
                GetWindowThreadProcessId(f, &pid);
                if (f != g_loopFg && pid != GetCurrentProcessId()) g_loop->done = true;
            }
            return 0;
        }
        e.type = EvTimer;
        e.timer = (int)wp;
        Send(w, e);
        return 0;
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
    case WM_LBUTTONUP: MouseEvent(w, EvMouseUp, MouseLeft, 0, lp); return 0;
    case WM_DESTROY:
        e.type = EvDestroy;
        Send(w, e);
        w->h = nullptr;
        return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

// 点到弹出的窗口外面（包括别的程序的窗口）：结束会话，这一下点击不往下传
static LRESULT CALLBACK LoopMouseHook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_loop && !g_loop->done &&
        (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN || wp == WM_XBUTTONDOWN))
    {
        MSLLHOOKSTRUCT* hs = (MSLLHOOKSTRUCT*)lp;
        if (!g_loop->Inside(hs->pt.x, hs->pt.y))
        {
            g_loop->done = true;
            if (g_loopRoot) PostMessageW(g_loopRoot, WM_NULL, 0, 0);
            return 1;
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

namespace Wnd
{
    Window* NewPopup(WinHandler* h, void* owner, int x, int y, const Str& title)
    {
        Window* w = new Window();
        w->h = h;
        w->owner = (HWND)owner;
        w->kind = 1;
        HWND hw = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, PopupClass, title.c(), WS_POPUP,
                                  x, y, 1, 1, (HWND)owner, nullptr, g_inst, w);
        if (!hw) { delete w; return nullptr; }
        w->dpi = GetDpiForWindow(hw);
        return w;
    }
    void SetBounds(Window* w, int x, int y, int width, int height)
    {
        SetWindowPos(w->hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    }
    void ShowNoActivate(Window* w) { ShowWindow(w->hwnd, SW_SHOWNOACTIVATE); }
    Painter* BeginPaint(Window* w, int pw, int ph)
    {
        delete w->canvas;
        w->canvas = new Canvas();
        return w->canvas->Begin(pw, ph, 96.f * ScaleOf(w)) ? w->canvas->rt : nullptr;
    }
    void EndPaint(Window* w)
    {
        if (!w->canvas) return;
        w->canvas->End(w->hwnd);
        delete w->canvas;
        w->canvas = nullptr;
    }
    RectI WorkAreaAt(int x, int y)
    {
        POINT p = { x, y };
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
        RectI r = { mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom };
        return r;
    }

    void RunPopupLoop(Window* root, void* owner, PopupLoop* loop)
    {
        HWND rh = root->hwnd;
        if (owner) SetForegroundWindow((HWND)owner);
        g_loop = loop;
        g_loopRoot = rh;
        g_loopFg = GetForegroundWindow();
        SetCapture(rh);
        g_hook = SetWindowsHookExW(WH_MOUSE_LL, LoopMouseHook, g_inst, 0);
        ::SetTimer(rh, TimerForeground, 150, nullptr);
        MSG msg;
        while (!loop->done)
        {
            BOOL r = GetMessageW(&msg, nullptr, 0, 0);
            if (r <= 0) { PostQuitMessage((int)msg.wParam); break; }
            if (msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST)
            {
                int type = -1;
                switch (msg.message)
                {
                case WM_MOUSEMOVE: type = EvMouseMove; break;
                case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: type = EvMouseDown; break;
                case WM_LBUTTONUP: case WM_RBUTTONUP: type = EvMouseUp; break;
                }
                if (type >= 0)
                {
                    POINT p = { GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam) };
                    ClientToScreen(msg.hwnd, &p);
                    loop->OnMouse(type, p.x, p.y);
                }
                continue;
            }
            if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) { loop->OnKey((int)msg.wParam); continue; }
            if (msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP || msg.message == WM_CHAR || msg.message == WM_SYSCHAR) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (!loop->done && GetCapture() != rh && !loop->dragging) SetCapture(rh);
        }
        if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
        if (IsWindow(rh)) ::KillTimer(rh, TimerForeground);
        ReleaseCapture();
        g_loop = nullptr;
        g_loopRoot = nullptr;
        if (owner) PostMessageW((HWND)owner, WM_NULL, 0, 0); // 老办法：让主人窗口的消息循环醒一下，菜单收起后前台状态才对
    }
}

// =====================================================================
// 便签窗口
// =====================================================================
enum { TimerProgman = 0x7F02 };
static UINT g_taskbarCreated;

// 置底：让桌面(Progman)成为所有者，Win+D 显示桌面时不会被藏起来；开机时桌面可能还没建好，就每 2 秒重试
static void AttachToDesktop(Window* w)
{
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman)
    {
        SetWindowLongPtrW(w->hwnd, GWLP_HWNDPARENT, (LONG_PTR)progman);
        if (w->h && w->h->KeepAtBottom(w)) SetWindowPos(w->hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ::KillTimer(w->hwnd, TimerProgman);
        return;
    }
    ::SetTimer(w->hwnd, TimerProgman, 2000, nullptr);
}

static Str ImeString(HIMC imc, DWORD which)
{
    LONG bytes = ImmGetCompositionStringW(imc, which, nullptr, 0);
    Str s;
    if (bytes <= 0) return s;
    s.Reserve(bytes / 2 + 1);
    ImmGetCompositionStringW(imc, which, s.p, bytes);
    s.n = bytes / 2;
    s.p[s.n] = 0;
    return s;
}

static LRESULT CALLBACK NoteProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
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
        delete w->canvas;
        delete w;
        return DefWindowProcW(hw, m, wp, lp);
    }
    if (!w->h) return DefWindowProcW(hw, m, wp, lp);
    if (g_taskbarCreated && m == g_taskbarCreated)
    {
        // 资源管理器重启：重新挂到新的桌面窗口上
        if (w->level == Wnd::LevelDesktop && !w->peek) AttachToDesktop(w);
        return 0;
    }
    Event e;
    switch (m)
    {
    case WM_NCHITTEST: return HTCLIENT;
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    case WM_SETCURSOR:
    {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hw, &p);
        Wnd::SetCursor((CursorShape)w->h->CursorAt(w, p.x / ScaleOf(w), p.y / ScaleOf(w)));
        return TRUE;
    }
    case WM_LBUTTONDOWN: MouseEvent(w, EvMouseDown, MouseLeft, 1, lp); return 0;
    case WM_LBUTTONDBLCLK: MouseEvent(w, EvMouseDown, MouseLeft, 2, lp); return 0;
    case WM_LBUTTONUP: MouseEvent(w, EvMouseUp, MouseLeft, 0, lp); return 0;
    case WM_RBUTTONUP: MouseEvent(w, EvMouseUp, MouseRight, 0, lp); return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)lp != hw) { e.type = EvCaptureLost; Send(w, e); }
        return 0;
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
    case WM_CHAR:
        e.type = EvChar;
        e.ch = (u32)wp;
        if (Send(w, e)) return 0;
        break;
    case WM_KEYDOWN:
        e.type = EvKeyDown;
        e.key = (int)wp;
        e.mods = CurrentMods();
        if (Send(w, e)) return 0;
        break;
    case WM_IME_SETCONTEXT:
        // 组字自己画：不要系统的组字窗口
        if (w->h->WantsIme(w)) return DefWindowProcW(hw, m, wp, lp & ~ISC_SHOWUICOMPOSITIONWINDOW);
        break;
    case WM_IME_STARTCOMPOSITION:
        if (!w->h->WantsIme(w)) break;
        e.type = EvImeStart;
        Send(w, e);
        return 0;
    case WM_IME_COMPOSITION:
    {
        if (!w->h->WantsIme(w)) break;
        e.type = EvImeUpdate;
        HIMC imc = ImmGetContext(hw);
        if (imc)
        {
            if (lp & GCS_RESULTSTR) { e.hasResult = true; e.imeResult = ImeString(imc, GCS_RESULTSTR); }
            if (lp & GCS_COMPSTR)
            {
                e.hasComp = true;
                e.imeComp = ImeString(imc, GCS_COMPSTR);
                LONG cur = (lp & GCS_CURSORPOS) ? ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0) : e.imeComp.n;
                e.imeCursor = (int)LOWORD(cur);
            }
            ImmReleaseContext(hw, imc);
        }
        Send(w, e);
        return 0;
    }
    case WM_IME_ENDCOMPOSITION:
        if (!w->h->WantsIme(w)) break;
        e.type = EvImeEnd;
        Send(w, e);
        return 0;
    case WM_IME_CHAR:
        if (w->h->WantsIme(w)) return 0; // 结果已经在 WM_IME_COMPOSITION 里交出去了
        break;
    case WM_ACTIVATE:
        e.type = EvActivate;
        e.button = LOWORD(wp) != WA_INACTIVE;
        Send(w, e);
        return 0;
    case WM_WINDOWPOSCHANGING:
    {
        WINDOWPOS* pos = (WINDOWPOS*)lp;
        // 置底模式下（非编辑时）始终压在其他窗口下面
        if (w->h->KeepAtBottom(w) && !(pos->flags & SWP_NOZORDER)) pos->hwndInsertAfter = HWND_BOTTOM;
        float kw, kh;
        if (w->moving && !w->userResizing && !(pos->flags & SWP_NOSIZE) && w->h->KeepSizeOnMove(w, &kw, &kh))
        {
            // 只是拖着挪位置：系统想改大小（跨屏时按旧屏的像素摆窗口）就按窗口自己的大小来
            float S = ScaleOf(w);
            pos->cx = (int)(kw * S + 0.5);
            pos->cy = (int)(kh * S + 0.5);
        }
        break;
    }
    case WM_ENTERSIZEMOVE:
        w->moving = true;
        w->userResizing = false;
        e.type = EvMoveStart;
        Send(w, e);
        return 0;
    case WM_SIZING:
        w->userResizing = true;
        break;
    case WM_EXITSIZEMOVE:
        w->moving = false;
        w->userResizing = false;
        e.type = EvMoveEnd;
        Send(w, e);
        return 0;
    case WM_ACTIVATEAPP:
        e.type = EvAppActivate;
        e.button = wp != 0;
        Send(w, e);
        break;
    case WM_SIZE:
        // 只在用户拖边框缩放时告诉便签（创建窗口、挪位置、UpdateLayeredWindow 也会触发 WM_SIZE）
        if (w->moving && w->userResizing)
        {
            e.type = EvUserResize;
            e.pw = LOWORD(lp);
            e.ph = HIWORD(lp);
            Send(w, e);
        }
        return 0;
    case WM_GETMINMAXINFO:
    {
        float mw, mh;
        if (w->h->MinSize(w, &mw, &mh))
        {
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize.x = (int)(mw * ScaleOf(w));
            mm->ptMinTrackSize.y = (int)(mh * ScaleOf(w));
        }
        return 0;
    }
    case WM_DPICHANGED:
    {
        RECT* r = (RECT*)lp;
        SetWindowPos(hw, nullptr, r->left, r->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        e.type = EvScale;
        Send(w, e);
        return 0;
    }
    case WM_TIMER:
        if (wp == TimerProgman)
        {
            if (w->level != Wnd::LevelDesktop) ::KillTimer(hw, TimerProgman);
            else if (FindWindowW(L"Progman", nullptr)) AttachToDesktop(w);
            return 0;
        }
        e.type = EvTimer;
        e.timer = (int)wp;
        Send(w, e);
        return 0;
    case WM_CLOSE:
        e.type = EvClose;
        if (!Send(w, e)) DestroyWindow(hw);
        return 0;
    case WM_DESTROY:
        e.type = EvDestroy;
        Send(w, e);
        w->h = nullptr;
        return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

namespace Wnd
{
    Window* NewNoteWindow(WinHandler* h, const Str& title, int x, int y)
    {
        static bool registered;
        if (!registered)
        {
            // 程序图标加载好以后才注册（普通窗口模式时任务栏要用）
            registered = true;
            WNDCLASSW wc = {};
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = NoteProc;
            wc.hInstance = g_inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = g_icon;
            wc.lpszClassName = NoteClass;
            RegisterClassW(&wc);
            g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        }
        Window* w = new Window();
        w->h = h;
        w->kind = 2;
        HWND hw = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, NoteClass, title.c(), WS_POPUP, x, y, 10, 10, nullptr, nullptr, g_inst, w);
        if (!hw) { delete w; return nullptr; }
        w->dpi = GetDpiForWindow(hw);
        return w;
    }

    void SetLevel(Window* w, int level, bool peeking)
    {
        HWND hw = w->hwnd;
        w->level = level;
        w->peek = peeking;
        LONG_PTR ex = GetWindowLongPtrW(hw, GWL_EXSTYLE);
        if (level == LevelNormal) ex = (ex & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW; // 进任务栏和 Alt+Tab
        else ex = (ex | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
        SetWindowLongPtrW(hw, GWL_EXSTYLE, ex);
        if (level == LevelDesktop)
        {
            SetWindowPos(hw, peeking ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            if (!peeking) AttachToDesktop(w);
        }
        else
        {
            SetWindowLongPtrW(hw, GWLP_HWNDPARENT, 0);
            SetWindowPos(hw, level == LevelTopmost || peeking ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        SetWindowPos(hw, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    void PeekToTop(Window* w)
    {
        w->peek = true;
        // 挂在桌面窗口下的便签设不了置顶，先摘下来，结束时再挂回去
        SetWindowLongPtrW(w->hwnd, GWLP_HWNDPARENT, 0);
        SetWindowPos(w->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    void ToBottom(Window* w) { SetWindowPos(w->hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }
    void SetTitle(Window* w, const Str& title) { SetWindowTextW(w->hwnd, title.c()); }
    void Move(Window* w, int x, int y)
    {
        if (w->kind == 1) SetWindowPos(w->hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        else SetWindowPos(w->hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void Hide(Window* w) { ShowWindow(w->hwnd, SW_HIDE); }
    void Activate(Window* w) { SetForegroundWindow(w->hwnd); SetFocus(w->hwnd); }
    void Foreground(Window* w) { SetForegroundWindow(w->hwnd); }
    void DragMove(Window* w)
    {
        ReleaseCapture();
        SendMessageW(w->hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
    void DragResize(Window* w, int edge)
    {
        ReleaseCapture();
        SendMessageW(w->hwnd, WM_SYSCOMMAND, 0xF000 + edge, 0);
    }

    // ---- 屏幕 ----
    bool OnScreen(int x, int y)
    {
        POINT p = { x, y };
        return MonitorFromPoint(p, MONITOR_DEFAULTTONULL) != nullptr;
    }
    bool ScreenOf(RectI r, RectI* workArea)
    {
        RECT rc = { r.left, r.top, r.right, r.bottom };
        HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONULL);
        MONITORINFO mi = { sizeof(mi) };
        if (!mon || !GetMonitorInfoW(mon, &mi)) return false;
        if (workArea) { workArea->left = mi.rcWork.left; workArea->top = mi.rcWork.top; workArea->right = mi.rcWork.right; workArea->bottom = mi.rcWork.bottom; }
        return true;
    }
    RectI PrimaryWorkArea()
    {
        POINT p0 = { 0, 0 };
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(p0, MONITOR_DEFAULTTOPRIMARY), &mi);
        RectI r = { mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom };
        return r;
    }
    PointI CursorScreen()
    {
        POINT p;
        GetCursorPos(&p);
        PointI o = { p.x, p.y };
        return o;
    }

    // ---- 输入法和系统光标 ----
    void ImeAttach(Window* w, float caretH) { CreateCaret(w->hwnd, nullptr, 1, (int)(caretH * ScaleOf(w))); }
    void ImeDetach(Window*) { DestroyCaret(); }
    void ImeSetCaret(Window* w, RectF c)
    {
        float S = ScaleOf(w);
        POINT p = { (LONG)(c.left * S), (LONG)(c.top * S) };
        LONG bottom = (LONG)(c.bottom * S);
        SetCaretPos(p.x, p.y);
        HIMC imc = ImmGetContext(w->hwnd);
        if (!imc) return;
        COMPOSITIONFORM cf = {};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos = p;
        ImmSetCompositionWindow(imc, &cf);
        CANDIDATEFORM cand = {};
        cand.dwIndex = 0;
        cand.dwStyle = CFS_EXCLUDE;
        cand.ptCurrentPos.x = p.x;
        cand.ptCurrentPos.y = bottom;
        cand.rcArea.left = p.x;
        cand.rcArea.top = p.y;
        cand.rcArea.right = p.x + 1;
        cand.rcArea.bottom = bottom;
        ImmSetCandidateWindow(imc, &cand);
        ImmReleaseContext(w->hwnd, imc);
    }
    void ImeFinish(Window* w, bool cancel)
    {
        HIMC imc = ImmGetContext(w->hwnd);
        if (!imc) return;
        ImmNotifyIME(imc, NI_COMPOSITIONSTR, cancel ? CPS_CANCEL : CPS_COMPLETE, 0);
        ImmReleaseContext(w->hwnd, imc);
    }
    int CaretBlinkMs() { return (int)GetCaretBlinkTime(); }

    // ---- 剪贴板、选颜色 ----
    void ClipboardSet(const Str& s)
    {
        Str t = s.Replace(L"\n", L"\r\n");
        if (!OpenClipboard(nullptr)) return;
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (t.n + 1) * sizeof(wchar_t));
        if (g)
        {
            wchar_t* p = (wchar_t*)GlobalLock(g);
            for (int i = 0; i < t.n; i++) p[i] = t.p[i];
            p[t.n] = 0;
            GlobalUnlock(g);
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        }
        CloseClipboard();
    }
    Str ClipboardText()
    {
        Str s;
        if (!OpenClipboard(nullptr)) return s;
        HANDLE g = GetClipboardData(CF_UNICODETEXT);
        if (g)
        {
            const wchar_t* p = (const wchar_t*)GlobalLock(g);
            if (p) s = p;
            GlobalUnlock(g);
        }
        CloseClipboard();
        // 去掉控制字符（换行、Tab 保留）
        Str out;
        for (int i = 0; i < s.n; i++) if (s.p[i] >= 32 || s.p[i] == L'\n' || s.p[i] == L'\r' || s.p[i] == L'\t') out.Ch(s.p[i]);
        return out;
    }
    bool ClipboardHasText() { return IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE; }
    bool PickColor(Window* owner, u32* rgb)
    {
        static COLORREF custom[16];
        u32 cur = *rgb;
        CHOOSECOLORW cc = { sizeof(cc) };
        cc.hwndOwner = owner ? owner->hwnd : nullptr;
        cc.rgbResult = RGB((cur >> 16) & 255, (cur >> 8) & 255, cur & 255);
        cc.lpCustColors = custom;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (!ChooseColorW(&cc)) return false;
        *rgb = (GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
        return true;
    }
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
        wc.style = 0;
        wc.lpfnWndProc = PopupProc;
        wc.lpszClassName = PopupClass;
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
            SendMessageW(hw, WM_SETICON, ICON_BIG, (LPARAM)g_icon);
            SendMessageW(hw, WM_SETICON, ICON_SMALL, (LPARAM)g_iconSmall);
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
        POINT p = { (LONG)(x * ScaleOf(w) + 0.5f), (LONG)(y * ScaleOf(w) + 0.5f) };
        ClientToScreen(w->hwnd, &p);
        PointI o = { p.x, p.y };
        return o;
    }
    void FromScreen(Window* w, int sx, int sy, float* x, float* y)
    {
        POINT p = { sx, sy };
        ScreenToClient(w->hwnd, &p);
        *x = p.x / ScaleOf(w);
        *y = p.y / ScaleOf(w);
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
        FieldStyle(f, FontNote, 14, 400, Ui::T.text, Ui::T.inputBg, Ui::T.inputFocusBg);
        return f;
    }
    void FieldStyle(TextField* f, int font, float fontSize, int weight, u32 textColor, u32 bgRest, u32 bgFocus)
    {
        f->text = textColor;
        f->bgRest = bgRest;
        f->bgFocus = bgFocus;
        if (f->rest) DeleteObject(f->rest);
        if (f->focus) DeleteObject(f->focus);
        f->rest = CreateSolidBrush(Ui::Ref(bgRest));
        f->focus = CreateSolidBrush(Ui::Ref(bgFocus));
        if (fontSize != f->size || font != f->fontKind || weight != f->weight || !f->font)
        {
            f->size = fontSize;
            f->fontKind = font;
            f->weight = weight;
            FieldMakeFont(f);
        }
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
    void FieldSetMaxLen(TextField* f, int n) { SendMessageW(f->hwnd, EM_LIMITTEXT, (WPARAM)n, 0); }
    void FieldSetCue(TextField* f, const Str& cue) { SendMessageW(f->hwnd, EM_SETCUEBANNER, TRUE, (LPARAM)cue.c()); }

    void OpenUrl(const Str& url) { ShellExecuteW(nullptr, L"open", url.c(), nullptr, nullptr, SW_SHOWNORMAL); }
    void OpenFolder(const Str& path) { ShellExecuteW(nullptr, L"open", L"explorer.exe", path.c(), nullptr, SW_SHOWNORMAL); }

    bool Ask(Window* owner, const Str& title, const Str& text)
    {
        return MessageBoxW(owner ? owner->hwnd : nullptr, text.c(), title.c(), MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
    }
    bool Confirm(Window* owner, const Str& title, const Str& text)
    {
        return MessageBoxW(owner ? owner->hwnd : nullptr, text.c(), title.c(), MB_OKCANCEL | MB_ICONWARNING) == IDOK;
    }
}
