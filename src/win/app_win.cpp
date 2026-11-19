// Windows 外壳：入口、单实例、隐藏的主窗口（托盘、快捷键、定时器、后台线程的消息）、托盘、通知、开机自启、文件夹监视、主循环
#include "ui.h"
#include "window_win.h"
#include "shell.h"
#include "syncengine.h"
#include <shellapi.h>
#include <commctrl.h>

HINSTANCE g_inst;
HICON g_icon, g_iconSmall;
void RunGlobalCtors();

static HWND g_mainWnd;
static const wchar_t* RunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static HANDLE g_changeNotif = INVALID_HANDLE_VALUE;
static int g_taskbarCreated;

// 主窗口收的消息
#define WM_APP_TRAY        (WM_APP + 1)
#define WM_APP_SYNCDONE    (WM_APP + 2)
#define WM_APP_REMOTECHANGE (WM_APP + 3) // 实时推送：网盘里有改动
#define WM_APP_CALL        (WM_APP + 4) // 后台线程要在主线程上调的函数

// 文件监视：主循环和自测的消息泵都用
HANDLE AppWatchHandle() { return g_app && !g_app->exiting ? g_changeNotif : INVALID_HANDLE_VALUE; }
void AppWatchSignaled()
{
    FindNextChangeNotification(g_changeNotif);
    g_app->OnFolderChanged();
}

// =====================================================================
// 开机启动
// =====================================================================
static Str ExePath()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return Str(buf);
}
static Str AutoStartCommand() { return L"\"" + ExePath() + L"\""; }

namespace Shell
{
bool AutoStartGet()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(k, L"DeskNote", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

void AutoStartSet(bool on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on)
    {
        Str cmd = AutoStartCommand();
        RegSetValueExW(k, L"DeskNote", 0, REG_SZ, (const BYTE*)cmd.c(), (cmd.n + 1) * sizeof(wchar_t));
    }
    else RegDeleteValueW(k, L"DeskNote");
    RegCloseKey(k);
}

void AutoStartFix()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKey, 0, KEY_READ | KEY_WRITE, &k) != ERROR_SUCCESS) return;
    wchar_t buf[1024];
    DWORD size = sizeof(buf), type = 0;
    if (RegQueryValueExW(k, L"DeskNote", nullptr, &type, (BYTE*)buf, &size) == ERROR_SUCCESS)
    {
        Str want = AutoStartCommand();
        if (CompareStringOrdinal(buf, -1, want.c(), -1, TRUE) != CSTR_EQUAL)
            RegSetValueExW(k, L"DeskNote", 0, REG_SZ, (const BYTE*)want.c(), (want.n + 1) * sizeof(wchar_t));
    }
    RegCloseKey(k);
}

// =====================================================================
// 托盘
// =====================================================================
void TrayShow()
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_mainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_iconSmall ? g_iconSmall : LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, L"DeskNote " APP_VERSION, 128);
    Shell_NotifyIconW(NIM_ADD, &nid);
}
void TrayHide()
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_mainWnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void Notify(const Str& title, const Str& text)
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_mainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    lstrcpynW(nid.szInfoTitle, title.c(), 64);
    lstrcpynW(nid.szInfo, text.c(), 256);
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

bool HotkeyRegister(int id, int mods, int key)
{
    UINT m = MOD_NOREPEAT | (mods & ModCtrl ? MOD_CONTROL : 0) | (mods & ModAlt ? MOD_ALT : 0) | (mods & ModShift ? MOD_SHIFT : 0) | (mods & ModWin ? MOD_WIN : 0);
    return RegisterHotKey(g_mainWnd, id, m, (UINT)key) != FALSE;
}
void HotkeyUnregister(int id) { UnregisterHotKey(g_mainWnd, id); }
void SetTimer(int id, int ms) { ::SetTimer(g_mainWnd, (UINT_PTR)id, (UINT)ms, nullptr); }
void KillTimer(int id) { ::KillTimer(g_mainWnd, (UINT_PTR)id); }
void WatchFolder(const Str& dir)
{
    g_changeNotif = FindFirstChangeNotificationW(dir.c(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
}
void StopWatching()
{
    if (g_changeNotif != INVALID_HANDLE_VALUE) FindCloseChangeNotification(g_changeNotif);
    g_changeNotif = INVALID_HANDLE_VALUE;
}
void PostSyncDone(void* plan) { PostMessageW(g_mainWnd, WM_APP_SYNCDONE, 0, (LPARAM)plan); }
void PostRemoteChange() { PostMessageW(g_mainWnd, WM_APP_REMOTECHANGE, 0, 0); }
void Post(void (*fn)(void*), void* ctx) { PostMessageW(g_mainWnd, WM_APP_CALL, (WPARAM)fn, (LPARAM)ctx); }
void* MainHandle() { return g_mainWnd; }
void Quit() { PostQuitMessage(0); }
}

static LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_app && g_taskbarCreated && m == (UINT)g_taskbarCreated) { Shell::TrayShow(); return 0; }
    switch (m)
    {
    case WM_HOTKEY: g_app->OnHotkey((int)w); return 0;
    case WM_APP_TRAY:
        switch (LOWORD(l))
        {
        case WM_LBUTTONUP: g_app->TogglePeek(); break;
        case WM_RBUTTONUP: case WM_CONTEXTMENU:
        {
            POINT p;
            GetCursorPos(&p);
            g_app->OnTrayMenu(p.x, p.y);
            break;
        }
        case NIN_BALLOONUSERCLICK: g_app->OnNoticeClicked(); break;
        }
        return 0;
    case WM_APP_SYNCDONE: SyncOnDone(g_app->sync, (void*)l); return 0;
    case WM_APP_REMOTECHANGE: g_app->OnRemoteChange(); return 0;
    case WM_APP_CALL: ((void (*)(void*))w)((void*)l); return 0;
    case WM_TIMER: g_app->OnTimer((int)w); return 0;
    case WM_ACTIVATEAPP:
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// =====================================================================
// 入口（代替 C 运行库的启动代码）
// =====================================================================
static u32 Fnv(const Str& s)
{
    u32 h = 2166136261u;
    for (int i = 0; i < s.n; i++)
    {
        wchar_t c = s.p[i];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
        h = (h ^ (c & 0xFF)) * 16777619u;
        h = (h ^ (c >> 8)) * 16777619u;
    }
    return h;
}

extern "C" void __stdcall Start()
{
    RunGlobalCtors();
    g_inst = GetModuleHandleW(nullptr);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t appdata[MAX_PATH];
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    Str dir = PathJoin(Str(appdata), L"DeskNote");
    bool isDefault = true;
    Str open, testPhase, testOut, syncServer, syncKey, memdiag, updateUrl;
    for (int i = 1; i + 1 < argc; i++)
    {
        Str a(argv[i]);
        if (a.Eq(L"--data"))
        {
            wchar_t full[MAX_PATH];
            GetFullPathNameW(argv[i + 1], MAX_PATH, full, nullptr);
            dir = Str(full);
            isDefault = false;
        }
        else if (a.Eq(L"--open")) open = argv[i + 1];
        else if (a.Eq(L"--selftest") && i + 2 < argc)
        {
            testPhase = argv[i + 1];
            wchar_t full[MAX_PATH];
            GetFullPathNameW(argv[i + 2], MAX_PATH, full, nullptr);
            testOut = Str(full);
        }
        else if (a.Eq(L"--sync-server")) syncServer = argv[i + 1];
        else if (a.Eq(L"--sync-key")) syncKey = argv[i + 1];
        else if (a.Eq(L"--update-url")) updateUrl = argv[i + 1];
        else if (a.Eq(L"--memdiag")) memdiag = argv[i + 1];
        else if (a.Eq(L"--theme")) Ui::ForceTheme = Str(argv[i + 1]).Eq(L"light") ? 1 : 2;
        else if (a.Eq(L"--wait-pid"))
        {
            HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)ParseInt(Str(argv[i + 1]), 0));
            if (p) { WaitForSingleObject(p, 15000); CloseHandle(p); }
        }
    }
    LocalFree(argv);
    if ((!testPhase.Empty() || !memdiag.Empty()) && isDefault) ExitProcess(2); // 自测只许在测试目录里跑

    Store::Init(dir);

    // 单实例：同一个数据目录只开一个；再次启动时让已开的那个把便签提到最前
    Str key(L"DeskNoteNative_");
    key.Int(Fnv(dir));
    HANDLE mutex = CreateMutexW(nullptr, TRUE, key.c());
    HANDLE showEvent = CreateEventW(nullptr, FALSE, FALSE, (key + L"_show").c());
    if (GetLastError() == ERROR_ALREADY_EXISTS && WaitForSingleObject(mutex, 0) == WAIT_TIMEOUT)
    {
        SetEvent(showEvent);
        ExitProcess(0);
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_DATE_CLASSES | ICC_HOTKEY_CLASS | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    Wnd::RegisterClasses();

    g_icon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_iconSmall = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = g_inst;
    wc.lpszClassName = L"DeskNoteMain";
    RegisterClassW(&wc);
    // 隐藏的顶层窗口（不用纯消息窗口：它收不到“任务栏重建”广播）
    g_mainWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"DeskNote", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    g_taskbarCreated = (int)RegisterWindowMessageW(L"TaskbarCreated");

    g_app = new App();
    g_app->isDefaultDir = isDefault;
    g_app->silent = !testPhase.Empty() || !memdiag.Empty();
    if (updateUrl.n) g_app->updateCheckUrl = updateUrl; // 自测：连模拟服务器
    g_app->Start();
    if (open.Eq(L"trash")) g_app->ShowTrash();
    else if (open.Eq(L"settings")) g_app->ShowSettings();
    else if (open.StartsWith(L"edit") && open.n > 4 && g_app->notes.n) g_app->notes[0]->DebugEdit((int)ParseInt(open.Sub(4), 0));
    else if (open.Eq(L"edit") && g_app->notes.n) g_app->notes[0]->BeginEdit(0);            // 调界面用
    else if (open.Eq(L"capsule") && g_app->notes.n) g_app->notes[0]->Collapse();
    else if (open.Eq(L"preview") && g_app->notes.n) g_app->notes[0]->DebugPreview();
    else if (open.Eq(L"manage")) g_app->ShowManage();
    else if (open.Eq(L"quickadd")) g_app->ShowQuickAdd();
    else if (open.Eq(L"managemenu") && g_app->notes.n)
    {
        g_app->ShowManage();
        RECT r;
        GetWindowRect(Wnd::H(g_app->manageWnd), &r);
        Ui::MenuDebug = true;
        g_app->notes[0]->ShowSettingsMenu(r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 3, Wnd::H(g_app->manageWnd));
    }
    else if (open.Eq(L"reminder") && g_app->notes.n) g_app->notes[0]->DebugReminder(2);
    else if (open.Eq(L"hover") && g_app->notes.n) g_app->notes[0]->DebugHover(1, 2);
    else if (open.StartsWith(L"menu") && g_app->notes.n) g_app->notes[0]->DebugMenu((int)ParseInt(open.Sub(4), 0));
    else if (open.Eq(L"traymenu"))
    {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        Ui::MenuDebug = true;
        g_app->TrayMenu(wa.right - 200, wa.bottom - 10);
    }
    if (!testPhase.Empty()) SelfTestRun(testPhase, testOut, syncServer, syncKey);
    else if (!memdiag.Empty()) MemDiagRun(memdiag);

    HANDLE handles[2];
    MSG msg;
    while (true)
    {
        int count = 0;
        handles[count++] = showEvent;
        if (g_changeNotif != INVALID_HANDLE_VALUE && !g_app->exiting) handles[count++] = g_changeNotif;
        DWORD r = MsgWaitForMultipleObjectsEx(count, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) g_app->TogglePeek();
        else if (count > 1 && r == WAIT_OBJECT_0 + 1) AppWatchSignaled();
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { quit = true; break; }
            if (Wnd::PreTranslate(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
    }
    ReleaseMutex(mutex);
    ExitProcess(0);
}
