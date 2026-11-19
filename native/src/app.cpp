// 程序主体：入口、单实例、托盘、全局快捷键、开机自启、文件监视、提醒、备份、回收站、消息循环
#include "ui.h"
#include <shellapi.h>
#include <commctrl.h>

App* g_app;
HINSTANCE g_inst;
void RunGlobalCtors();

static const wchar_t* RunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
enum { TimerReminders = 1, TimerGfx = 2, TimerBackup = 3, TimerFiles = 4, TimerSync = 5 };
static HANDLE g_changeNotif = INVALID_HANDLE_VALUE;
static Vec<TrashEntry*> g_deadTrash; // 移出回收站的记录：撤销步骤可能还引用着，留到退出
static int g_taskbarCreated;

// 文件监视：主循环和自测的消息泵都用
HANDLE AppWatchHandle() { return g_app && !g_app->exiting ? g_changeNotif : INVALID_HANDLE_VALUE; }
void AppWatchSignaled()
{
    FindNextChangeNotification(g_changeNotif);
    SetTimer(g_app->msgWnd, TimerFiles, 300, nullptr); // 等对方写完再读
}

static void TrayAdd();
static void TrayRemove();
static LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l);

// =====================================================================
// 启动
// =====================================================================
void App::Start()
{
    icon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    iconSmall = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    settings = Store::LoadSettings();
    Store::LoadTrash(trash);

    Vec<NoteData*> list;
    Store::LoadNotes(list);
    if (!settings.autoStartInit)
    {
        // 全新安装（还没有任何便签）默认开机自启；已有数据的老用户保持原样
        if (list.n == 0 && isDefaultDir) SetAutoStart(true);
        settings.autoStartInit = true;
        Store::SaveSettings(settings);
    }
    // exe 挪过位置后，把开机自启指向现在这个 exe（只在已打开自启时更新）
    if (isDefaultDir) FixAutoStartPath();
    if (list.n == 0)
    {
        NoteData* n = new NoteData();
        n->id = NoteData::NewId();
        Store::SaveNote(n);
        list.Push(n);
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = g_inst;
    wc.lpszClassName = L"DeskNoteMain";
    RegisterClassW(&wc);
    // 隐藏的顶层窗口（不用纯消息窗口：它收不到“任务栏重建”广播）
    msgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"DeskNote", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    g_taskbarCreated = (int)RegisterWindowMessageW(L"TaskbarCreated");

    for (int i = 0; i < list.n; i++) OpenWindow(list[i]);
    Store::DailyBackup();
    TrayAdd();
    ApplyHotkey();
    g_changeNotif = FindFirstChangeNotificationW(Store::NotesDir.c(), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
    SetTimer(msgWnd, TimerReminders, 15000, nullptr);
    SetTimer(msgWnd, TimerGfx, 1000, nullptr);
    SetTimer(msgWnd, TimerBackup, 60 * 60 * 1000, nullptr);
    sync = SyncCreate();
    SyncStart(sync);
    CheckReminders(); // 关机期间错过的提醒，启动后马上补
}

NoteWin* App::OpenWindow(NoteData* n)
{
    NoteWin* w = new NoteWin(n);
    notes.Push(w);
    w->Create();
    return w;
}

NoteWin* App::Find(const Str& id)
{
    for (int i = 0; i < notes.n; i++) if (notes[i]->d->id == id) return notes[i];
    return nullptr;
}

HFONT App::UiFont(int dpi)
{
    if (uiFont && uiFontDpi == dpi) return uiFont;
    if (uiFont) DeleteObject(uiFont);
    uiFont = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    uiFontDpi = dpi;
    return uiFont;
}

HFONT App::UiFontBold(int dpi)
{
    if (uiFontBold && uiFontBoldDpi == dpi) return uiFontBold;
    if (uiFontBold) DeleteObject(uiFontBold);
    uiFontBold = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    uiFontBoldDpi = dpi;
    return uiFontBold;
}

// =====================================================================
// 便签增删
// =====================================================================
void App::CreateNote(NoteWin* from)
{
    NoteData* n = from ? from->d->CloneStyle() : nullptr;
    if (!n) { n = new NoteData(); n->id = NoteData::NewId(); n->title = L"新便签"; }
    n->collapsed = false;
    if (from)
    {
        // 放在原便签右下方一点（位置是物理像素）
        from->SaveLayout();
        if (from->d->hasPos && !from->d->collapsed) { n->hasPos = true; n->x = from->d->x + 60; n->y = from->d->y + 60; }
    }
    Store::SaveNote(n);
    NoteWin* w = OpenWindow(n);
    w->BeginEdit(0);
}

void App::DeleteNote(NoteWin* w, bool confirm)
{
    if (confirm)
    {
        Str msg = L"把便签「" + w->d->title + L"」（";
        msg.Int(w->d->items.n).Append(L" 条）移到回收站？\n之后可以在回收站里恢复。");
        if (MessageBoxW(w->hwnd, msg.c(), L"删除便签", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    }
    w->CommitEdit(true);
    w->SaveLayout();
    w->deleted = true;
    TrashEntry* e = new TrashEntry();
    e->kind = L"note";
    e->deletedAt = Now();
    e->note = new NoteData(*w->d);
    trash.Insert(0, e);
    SaveTrash();
    notes.Remove(w);
    Store::DeleteNoteFile(w->d->id);
    if (balloonNote == w) balloonNote = nullptr;
    w->Destroy();
    delete w;
    OnNoteSaved();
}

// =====================================================================
// 回收站
// =====================================================================
TrashEntry* App::TrashItem(NoteData* note, int index, const NoteItem& item)
{
    TrashEntry* e = new TrashEntry();
    e->kind = L"item";
    e->deletedAt = Now();
    e->noteId = note->id;
    e->noteTitle = note->title;
    e->index = index;
    e->item = item;
    trash.Insert(0, e);
    SaveTrash();
    return e;
}

// 撤销删除时把对应的回收站记录拿掉；重做时放回去
void App::Untrash(const Vec<TrashEntry*>& list)
{
    if (!list.n) return;
    for (int i = 0; i < list.n; i++) trash.Remove(list[i]);
    SaveTrash();
}
void App::Retrash(const Vec<TrashEntry*>& list)
{
    if (!list.n) return;
    for (int i = 0; i < list.n; i++) if (!trash.Contains(list[i])) trash.Insert(0, list[i]);
    SaveTrash();
}

void App::SaveTrash()
{
    Store::SaveTrash(trash);
    if (trashWnd) TrashWindowRefresh(trashWnd);
}

void App::Restore(TrashEntry* e)
{
    if (e->kind.Eq(L"note") && e->note)
    {
        NoteData* n = new NoteData(*e->note);
        if (Find(n->id) || FileExists(Store::NotePath(n->id).c())) n->id = NoteData::NewId();
        Store::Revive(n->id);
        Store::SaveNote(n);
        OpenWindow(n);
    }
    else
    {
        // 原便签还在就放回原处，否则放进第一张便签，一张都没有就新建
        NoteWin* w = Find(e->noteId);
        if (!w && notes.n) w = notes[0];
        if (!w)
        {
            NoteData* n = new NoteData();
            n->id = NoteData::NewId();
            n->title = e->noteTitle.Empty() ? Str(L"ToDo") : e->noteTitle;
            Store::SaveNote(n);
            w = OpenWindow(n);
        }
        w->RestoreItem(e->item, w->d->id == e->noteId ? e->index : w->d->items.n);
    }
    trash.Remove(e);
    g_deadTrash.Push(e);
    SaveTrash();
}

void App::Purge(TrashEntry* e)
{
    trash.Remove(e);
    g_deadTrash.Push(e);
    SaveTrash();
}

void App::EmptyTrash()
{
    for (int i = 0; i < trash.n; i++) g_deadTrash.Push(trash[i]);
    trash.Clear();
    SaveTrash();
}

void App::ShowTrash()
{
    if (!trashWnd) trashWnd = TrashWindowCreate();
    ShowWindow(trashWnd, SW_SHOW);
    SetForegroundWindow(trashWnd);
}

void App::ShowSettings()
{
    if (!settingsWnd) settingsWnd = SettingsWindowCreate();
    ShowWindow(settingsWnd, SW_SHOW);
    SetForegroundWindow(settingsWnd);
}

// =====================================================================
// 临时把所有便签提到最前（快捷键 / 单击托盘）
// =====================================================================
void App::TogglePeek()
{
    if (peeking) { EndPeek(); return; }
    if (!notes.n) return;
    peeking = true;
    for (int i = 0; i < notes.n; i++) notes[i]->BeginPeek();
    SetForegroundWindow(notes.Last()->hwnd);
}

void App::EndPeek()
{
    peeking = false;
    for (int i = 0; i < notes.n; i++) notes[i]->EndPeek();
}

void App::BringAllOnScreen()
{
    for (int i = 0; i < notes.n; i++) notes[i]->BringOnScreen();
}

// =====================================================================
// 提醒：到点后弹系统通知，条目上的时间变红
// =====================================================================
void App::CheckReminders()
{
    Time now = Now();
    for (int k = 0; k < notes.n; k++)
    {
        NoteWin* w = notes[k];
        int count = 0;
        const NoteItem* first = nullptr;
        for (int i = 0; i < w->d->items.n; i++)
        {
            NoteItem& it = w->d->items[i];
            if (it.hasRemind && !it.reminded && !it.done && it.remind <= now)
            {
                it.reminded = true;
                if (!first) first = &it;
                count++;
            }
        }
        if (!count) continue;
        Str text = first->text.Replace(L"\n", L" ");
        if (text.n > 60) { text.Truncate(60); text += L"…"; }
        if (count > 1) { text += L"（另有 "; text.Int(count - 1).Append(L" 条）"); }
        w->OnRemindersFired();
        ShowNotice(L"提醒 · " + w->d->title, text, w);
    }
}

void App::ShowNotice(const Str& title, const Str& text, NoteWin* w)
{
    balloonNote = w;
    if (silent) return;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = msgWnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    lstrcpynW(nid.szInfoTitle, title.c(), 64);
    lstrcpynW(nid.szInfo, text.c(), 256);
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// =====================================================================
// 同步联动
// =====================================================================
void App::OnNoteSaved() { if (sync) SyncTouch(sync); }

void App::OpenSyncedNote(NoteData* n)
{
    Store::Revive(n->id);
    Store::SaveNote(n);
    OpenWindow(n);
}

void App::CreateNoteFromContent(const Str& json, const wchar_t* suffix)
{
    NoteData* n = NoteData::FromContentJson(json, NoteData::NewId());
    if (!n) return;
    n->title += suffix;
    Store::SaveNote(n);
    OpenWindow(n);
}

// =====================================================================
// 外部修改便签文件：重新读入；放进来的新文件直接打开
// =====================================================================
void App::CheckFileChanges()
{
    if (exiting) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(PathJoin(Store::NotesDir, L"*.json").c(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    Vec<Str> ids;
    do ids.Push(PathStem(Str(fd.cFileName)));
    while (FindNextFileW(h, &fd));
    FindClose(h);
    for (int i = 0; i < ids.n; i++)
    {
        NoteData* fresh = Store::ReadIfChangedExternally(ids[i]);
        if (!fresh) continue;
        NoteWin* w = Find(ids[i]);
        if (w) w->ReloadFrom(fresh, false);
        else if (!Store::IsDeleted(ids[i])) OpenWindow(fresh);
        else delete fresh;
    }
}

// =====================================================================
// 全局快捷键
// =====================================================================
const wchar_t* App::ApplyHotkey()
{
    UnregisterHotKey(msgWnd, 1);
    if (!settings.hotkeyEnabled) return nullptr;
    UINT mods, vk;
    if (!HotkeyParse(settings.hotkey, &mods, &vk)) return L"快捷键格式不对";
    if (!RegisterHotKey(msgWnd, 1, mods | MOD_NOREPEAT, vk)) return L"注册失败，可能被其他软件占用了";
    return nullptr;
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

bool App::IsAutoStart()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(k, L"DeskNote", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

void App::SetAutoStart(bool on)
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

void App::FixAutoStartPath()
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
static void TrayAdd()
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_app->msgWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_app->iconSmall ? g_app->iconSmall : LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, L"DeskNote " APP_VERSION, 128);
    Shell_NotifyIconW(NIM_ADD, &nid);
}
static void TrayRemove()
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_app->msgWnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void TrayMenu(int sx, int sy)
{
    enum { CShow = 1, CNew, CBring, CTrash, CSettings, CExit };
    Ui::Menu m;
    m.Item(CShow, L"显示所有便签", 0xE7C4, g_app->settings.hotkeyEnabled ? g_app->settings.hotkey.c() : nullptr);
    m.Item(CNew, L"新建便签", 0xE70B);
    m.Item(CBring, L"把便签拉回屏幕", 0xE7F4);
    m.Sep();
    m.Item(CTrash, L"回收站", 0xE7B8);
    m.Item(CSettings, L"设置", 0xE713);
    m.Sep();
    m.Item(CExit, L"退出", 0xE7E8);
    if (sx == -1 && sy == -1)
    {
        POINT p;
        GetCursorPos(&p);
        sx = p.x;
        sy = p.y;
    }
    int cmd = Ui::MenuRun(g_app->msgWnd, &m, sx, sy, nullptr, nullptr);
    PostMessageW(g_app->msgWnd, WM_NULL, 0, 0);
    switch (cmd)
    {
    case CShow: g_app->TogglePeek(); break;
    case CNew: g_app->CreateNote(g_app->notes.n ? g_app->notes.Last() : nullptr); break;
    case CBring: g_app->BringAllOnScreen(); break;
    case CTrash: g_app->ShowTrash(); break;
    case CSettings: g_app->ShowSettings(); break;
    case CExit: g_app->Exit(); break;
    }
}

static LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_app && g_taskbarCreated && m == (UINT)g_taskbarCreated) { TrayAdd(); return 0; }
    switch (m)
    {
    case WM_HOTKEY: if (w == 1) g_app->TogglePeek(); return 0;
    case WM_APP_TRAY:
        switch (LOWORD(l))
        {
        case WM_LBUTTONUP: g_app->TogglePeek(); break;
        case WM_RBUTTONUP: case WM_CONTEXTMENU: TrayMenu(-1, -1); break;
        case NIN_BALLOONUSERCLICK:
            if (g_app->balloonNote && g_app->notes.Contains(g_app->balloonNote))
            {
                g_app->balloonNote->Expand();
                if (!g_app->peeking) g_app->TogglePeek();
            }
            break;
        }
        return 0;
    case WM_APP_SYNCDONE: SyncOnDone(g_app->sync, l); return 0;
    case WM_TIMER:
        switch (w)
        {
        case TimerReminders: g_app->CheckReminders(); break;
        case TimerGfx: Gfx::ReleaseIfIdle(); SyncTick(g_app->sync); break;
        case TimerBackup: Store::DailyBackup(); break;
        case TimerFiles: KillTimer(h, TimerFiles); g_app->CheckFileChanges(); break;
        }
        return 0;
    case WM_ACTIVATEAPP:
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// =====================================================================
// 退出
// =====================================================================
void App::Exit()
{
    if (exiting) return;
    exiting = true;
    for (int i = 0; i < notes.n; i++) { notes[i]->Destroy(); }
    UnregisterHotKey(msgWnd, 1);
    TrayRemove();
    if (trashWnd) DestroyWindow(trashWnd);
    if (settingsWnd) DestroyWindow(settingsWnd);
    if (g_changeNotif != INVALID_HANDLE_VALUE) FindCloseChangeNotification(g_changeNotif);
    PostQuitMessage(0);
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
    Str open, testPhase, testOut, syncServer, syncKey, memdiag;
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
    RegisterDialogClasses();

    g_app = new App();
    g_app->isDefaultDir = isDefault;
    g_app->silent = !testPhase.Empty() || !memdiag.Empty();
    g_app->Start();
    if (open.Eq(L"trash")) g_app->ShowTrash();
    else if (open.Eq(L"settings")) g_app->ShowSettings();
    else if (open.Eq(L"edit") && g_app->notes.n) g_app->notes[0]->BeginEdit(0);            // 调界面用
    else if (open.Eq(L"capsule") && g_app->notes.n) g_app->notes[0]->Collapse();
    else if (open.Eq(L"preview") && g_app->notes.n) g_app->notes[0]->DebugPreview();
    else if (open.StartsWith(L"menu") && g_app->notes.n) g_app->notes[0]->DebugMenu((int)ParseInt(open.Sub(4), 0));
    else if (open.Eq(L"traymenu"))
    {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        Ui::MenuDebug = true;
        TrayMenu(wa.right - 200, wa.bottom - 10);
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
            if (g_app->settingsWnd && !SettingsWindowWantsKeys(g_app->settingsWnd) && IsDialogMessageW(g_app->settingsWnd, &msg)) continue;
            if (g_app->trashWnd && IsDialogMessageW(g_app->trashWnd, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
    }
    ReleaseMutex(mutex);
    ExitProcess(0);
}
