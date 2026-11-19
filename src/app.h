// 程序共用的声明：程序主体、便签窗口、绘图资源、对话框、同步
#pragma once
#include "rt.h"
#include "store.h"
#include <d2d1.h>
#include <dwrite.h>

#define APP_VERSION L"2.0.0"

// 项目和 gopan 的链接（设置页「关于」里打开）；gopan 网页也是同步的默认服务器
#define URL_DESKNOTE_REPO L"https://github.com/yophon/desknote"
#define URL_GOPAN_REPO    L"https://github.com/yophon/gopan"
#define URL_GOPAN_WEB     L"https://drive.979687.xyz"

// 程序内部消息
#define WM_APP_TRAY        (WM_APP + 1)
#define WM_APP_SYNCDONE    (WM_APP + 2)
#define WM_APP_COMMITEDIT  (WM_APP + 3)

// ---------------------------------------------------------------------
// 绘图资源：Direct2D/DirectWrite 工厂，用时创建，空闲几秒后整体释放（省内存）
// ---------------------------------------------------------------------
namespace Gfx
{
    ID2D1Factory* D2D();
    IDWriteFactory* DW();
    void Touch();          // 刚用过：推迟释放
    void ReleaseIfIdle();  // 定时器调用
    void ReleaseAll();
    D2D1_COLOR_F Color(u32 rgb, float a);
}

// ---------------------------------------------------------------------
// 便签窗口
// ---------------------------------------------------------------------
struct UndoStep
{
    Str state;
    Vec<TrashEntry*> trashed;
};

struct RowInfo { int index; float top, bottom, textLeft, chipLeft; }; // chipLeft：提醒标签左边，0 = 没有

class NoteWin
{
public:
    NoteData* d;
    HWND hwnd = nullptr;
    bool deleted = false;

    explicit NoteWin(NoteData* data);
    ~NoteWin();
    void Create();
    void Destroy();

    // 供程序主体调用
    void Render();
    void BeginEdit(int index);
    void CommitEdit(bool save);
    bool Editing() const { return editIndex != NotEditing; }
    void ReloadFrom(NoteData* fresh, bool keepWindow); // 接管 fresh
    void RestoreItem(const NoteItem& it, int index);
    void SaveLayout();
    void Save();
    void BringOnScreen();
    void BeginPeek();
    void EndPeek();
    void Expand();
    void Collapse();
    void OnRemindersFired();
    void Undo();
    void Redo();
    bool CanUndo() const { return undo.n > 0; }
    void ApplyMode();
    void SetMode(WinMode m);
    float Scale() const;

    // 自测、调界面用
    void DebugPreview() { Collapse(); ShowPreview(); KillTimer(hwnd, 1); }
    void DebugMenu(int sub);
    void DebugReminder(int i) { EditReminder(i); }
    void DebugHover(int row, int btn) { hover = true; hoverRow = row; hoverBtn = btn; Render(); }
    void OnMenuLive(int id, float v);
    void TestTypeItem(int index, const wchar_t* text);
    void TestBeginEdit(int index, const wchar_t* text);
    void TestMenuDelete(int index);
    void TestToggleDone(int index);
    void TestSetRemind(int index, Time t);

    static const int NotEditing = -1, TitleIndex = -2;
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EditHostProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PreviewProc(HWND, UINT, WPARAM, LPARAM);

private:
    friend struct NotePainter;
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);

    // 编辑
    int editIndex = NotEditing;
    Str editOrig;
    HWND editHost = nullptr, editCtl = nullptr;
    HFONT editFont = nullptr;
    HBRUSH editBrush = nullptr;
    float editHeightDip = 0;
    void OpenEditor();
    void CloseEditor();
    void PositionEditor();
    void OnEditKey(WPARAM vk, bool* handled);
    friend LRESULT CALLBACK EditSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    // 布局（上次绘制的结果，用于命中测试；单位 DIP，内容坐标）
    Vec<RowInfo> rows;
    float titleBottom = 0, contentHeight = 0, scrollY = 0;
    D2D1_RECT_F toolBtn[3] = {};   // 标题栏右侧：新增、菜单、折叠（悬停时出现）
    int hoverBtn = 0, hoverRow = -1;
    bool hover = false;
    int HitToolbar(float xDip, float yDip) const;
    bool sizing = false;
    bool menuChanged = false;
    int HitRow(float yDip) const;
    int HitEdge(int x, int y) const;

    // 拖动排序
    int dragIndex = -1;
    bool dragChanged = false;
    void BeginReorder(int index);
    void OnReorderMove(float yDip);
    void EndReorder();

    // 撤销
    Vec<UndoStep*> undo, redo;
    UndoStep* pending = nullptr;
    int changeDepth = 0;
    NoteData* pendingReload = nullptr;
    void BeginChange();
    void EndChange();
    void ClearUndo();
    void ApplyState(const Str& json);
    void ApplyExternal(NoteData* fresh);
    void TrashItemAt(int i);
    void ClearDone();
    void MoveItem(int from, int to);

    // 鼠标、菜单
    void OnLButtonDown(int x, int y, bool dbl);
    void OnContextMenu(int x, int y);
    void RunCommand(int cmd, int target);
    bool TryOpenLink(int x, int y);
    void EditReminder(int index);
    void PickColor(bool background);

    // 窗口模式、置顶
    bool peeking = false;
    int taskbarCreatedMsg = 0;
    void AttachToDesktop();
    void SendToBottom();
    void RestorePosition();
    void PlaceDefault();
    RECT WorkAreaPx() const;

    // 胶囊
    bool capHidden = false, sliding = false;
    int dockedX = 0, dockedY = 0;
    bool dockL = false, dockR = false, dockT = false, dockB = false;
    int slideFromX = 0, slideFromY = 0, slideToX = 0, slideToY = 0;
    DWORD slideStart = 0;
    HWND preview = nullptr;
    bool previewHover = false;
    void EnterCollapsed();
    void SnapCapsule();
    void ScheduleCapsuleHide();
    void OnHideTimer();
    bool OtherScreenAt(int x, int y) const;
    void SlideTo(int x, int y);
    void ShowPreview();
    void ClosePreview();
    void RenderPreview();
    void ResizeTo(int w, int h);

    void PaintTo(HDC mem, int pw, int ph);
};

// ---------------------------------------------------------------------
// 同步到 gopan
// ---------------------------------------------------------------------
class Syncer;
Syncer* SyncCreate();
void SyncStart(Syncer* s);
void SyncNow(Syncer* s);
void SyncTouch(Syncer* s);
void SyncOnDone(Syncer* s, LPARAM plan);
void SyncTick(Syncer* s);
const Str& SyncStatus(Syncer* s);
bool SyncFailed(Syncer* s);
Str SyncNormFolder(const Str& f);
bool SyncBusy(Syncer* s);

// ---------------------------------------------------------------------
// 程序主体
// ---------------------------------------------------------------------
struct App
{
    Vec<NoteWin*> notes;
    Vec<TrashEntry*> trash;
    Settings settings;
    bool isDefaultDir = true;
    bool silent = false;
    bool exiting = false;
    bool peeking = false;
    HWND msgWnd = nullptr;
    HWND trashWnd = nullptr, settingsWnd = nullptr;
    NoteWin* balloonNote = nullptr;
    Syncer* sync = nullptr;
    HICON icon = nullptr, iconSmall = nullptr;

    void Start();
    NoteWin* OpenWindow(NoteData* n);
    void CreateNote(NoteWin* from);
    void DeleteNote(NoteWin* w, bool confirm);
    TrashEntry* TrashItem(NoteData* note, int index, const NoteItem& item);
    void Untrash(const Vec<TrashEntry*>& list);
    void Retrash(const Vec<TrashEntry*>& list);
    void Restore(TrashEntry* e);
    void Purge(TrashEntry* e);
    void EmptyTrash();
    void SaveTrash();
    NoteWin* Find(const Str& id);
    int IndexOf(NoteWin* w) { return notes.IndexOf(w); }
    void ShowTrash();
    void ShowSettings();
    void TogglePeek();
    void EndPeek();
    void BringAllOnScreen();
    void CheckReminders();
    void ShowNotice(const Str& title, const Str& text, NoteWin* w);
    void OnNoteSaved();
    void OpenSyncedNote(NoteData* n);
    void CreateNoteFromContent(const Str& json, const wchar_t* suffix);
    const wchar_t* ApplyHotkey(); // nullptr = 成功
    void CheckFileChanges();
    void Exit();

    static bool IsAutoStart();
    static void SetAutoStart(bool on);
    static void FixAutoStartPath();
};
extern App* g_app;
extern HINSTANCE g_inst;

// 对话框（dialogs.cpp）
HWND TrashWindowCreate();
void TrashWindowRefresh(HWND w);
HWND SettingsWindowCreate();
void SettingsWindowSyncStatus(HWND w);
bool SettingsWindowWantsKeys(HWND w); // 正在录快捷键：按键别交给对话框导航处理
int ReminderDialog(HWND owner, const Str& itemText, bool hasCurrent, Time current, Time* result); // 0 取消，1 设定，2 移除
bool HotkeyParse(const Str& s, UINT* mods, UINT* vk);
Str HotkeyFormat(UINT mods, UINT vk);
void RegisterDialogClasses();

// 链接
struct LinkSpan { int start, len; Str url; };
void FindLinks(const Str& text, Vec<LinkSpan>& out);

// 自测（selftest.cpp）
void SelfTestRun(const Str& phase, const Str& outFile, const Str& server, const Str& key);
void MemDiagRun(const Str& outFile);
