// 程序共用的声明：程序主体、便签窗口、绘图资源、对话框、同步
#pragma once
#include "rt.h"
#include "version.h"
#include "store.h"
#include "links.h"
#include "window.h"
#include "gfx.h"

namespace Ui { struct Menu; }

// 项目和 gopan 的链接（设置页「关于」里打开）；gopan 网页也是同步的默认服务器
#define URL_DESKNOTE_REPO L"https://github.com/yophon/desknote"
#define URL_GOPAN_REPO    L"https://github.com/yophon/gopan"
#define URL_GOPAN_WEB     L"https://drive.979687.xyz"

// ---------------------------------------------------------------------
// 便签窗口
// ---------------------------------------------------------------------
struct UndoStep
{
    Str state;
    Vec<TrashEntry*> trashed;
};

struct RowInfo { int index; float top, bottom, textLeft, chipLeft; }; // chipLeft：提醒标签左边，0 = 没有

struct NotePreview;

class NoteWin : public WinHandler
{
public:
    NoteData* d;
    Window* win = nullptr;            // 隐藏着的便签没有窗口
    bool deleted = false;
    bool tempShown = false;           // 隐藏的便签因为提醒被临时显示出来，结束「提到最前」时收回去

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
    void AddItem(const Str& text);    // 末尾加一条（快速添加）
    void Undo();
    void Redo();
    bool CanUndo() const { return undo.n > 0; }
    void ApplyMode();
    void SetMode(WinMode m);
    void SetHidden(bool hidden);      // 便签管理里的显示 / 隐藏
    void ShowTemporarily();
    void Locate();                    // 便签管理里的「定位」：提到最前、描边闪一下
    void ShowSettingsMenu(int sx, int sy, void* owner); // 便签管理里的「设置」；owner：平台的窗口句柄
    void AddStyleItems(Ui::Menu& m);
    float Scale() const;

    // 自测、调界面用
    void DebugPreview() { Collapse(); ShowPreview(); Wnd::KillTimer(win, 1); }
    void DebugMenu(int sub);
    void DebugReminder(int i) { EditReminder(i); }
    static bool debugNoFocus; // 调界面用：进入编辑时不抢前台和键盘焦点（截图时别吃掉用户正在打的字）
    void DebugEdit(int mode)
    {
        debugNoFocus = true;
        // 1：最后一条里选中一段；2：新条目（空的，显示提示）；3：编辑标题
        if (mode == 1) { BeginEdit(d->items.n - 1); anchor = 4; caret = Min(editText.n, 16); EditUpdated(); }
        else if (mode == 2) BeginEdit(d->items.n);
        else if (mode == 3) BeginEdit(TitleIndex);
    }
    void DebugHover(int row, int btn) { hover = true; hoverRow = row; hoverBtn = btn; Render(); }
    void OnMenuLive(int id, float v);
    void TestTypeItem(int index, const wchar_t* text);
    void TestBeginEdit(int index, const wchar_t* text);
    void TestMenuDelete(int index);
    void TestToggleDone(int index);
    void TestSetRemind(int index, Time t, int repeat = 0);
    bool TestCheckPoint(int index, float* x, float* y); // 第 index 条勾选框的中心（窗口 DIP）
    bool TestDocked() const { return dockL || dockR || dockT || dockB; }
    bool TestCapHidden() const { return capHidden; }

    static const int NotEditing = -1, TitleIndex = -2;
    // 窗口事件（window.h）
    void Paint(Window*, Painter*, float, float) override {}
    bool OnEvent(Window* w, const Event& e) override;
    int CursorAt(Window* w, float x, float y) override;
    bool KeepAtBottom(Window* w) override;
    bool KeepSizeOnMove(Window* w, float* width, float* height) override;
    bool MinSize(Window* w, float* width, float* height) override;
    bool WantsIme(Window* w) override { (void)w; return Editing(); }

private:
    friend struct NotePreview;
    bool OnPreviewEvent(const Event& e);

    // 编辑
    int editIndex = NotEditing;
    Str editOrig, editText;           // 编辑前 / 正在编辑的文字
    int caret = 0, anchor = 0;        // 光标和选区另一端（UTF-16 位置）
    Str compText;                     // 输入法正在组的字
    int compCursor = 0;
    bool caretOn = true, editSelecting = false;
    float editGoalX = -1, editW = 0;  // 上下移动时保持的列；编辑区宽度
    RectF editBox, caretBox; // 编辑区、光标（DIP，内容坐标）
    struct EditSnap { Str text; int caret, anchor; };
    Vec<EditSnap> editUndo, editRedo; // 编辑中的撤销（提交后整条算一步，进便签的撤销）
    int editLastKind = 0, editLastPos = 0;
    TextLayout* EditLayout(int* dispCaret);
    float EditMeasure(float w, float minH);
    void DrawEditor(Painter* rt, float x, float y, float w, float h, bool centerV);
    int EditHit(float xd, float yd);
    bool InEditBox(float xd, float yd) const;
    int EditStep(int pos, int dir, bool word);
    int EditLineMove(int pos, int what);
    void EditSetCaret(int pos, bool extend);
    void EditReplace(int a, int b, const Str& s, int kind);
    void EditInsert(const Str& s, int kind);
    void EditUndoRedo(bool redoIt);
    void EditCopy(bool cut);
    void EditPaste();
    void EditSelectWord(int pos);
    bool EditKey(int vk, int mods);
    void EditChar(wchar_t c);
    void EditUpdated();
    void UpdateImePos();
    void EditFinishIme(bool cancel);
    void EditContextMenu(int sx, int sy);

    // 布局（上次绘制的结果，用于命中测试；单位 DIP，内容坐标）
    Vec<RowInfo> rows;
    float titleBottom = 0, contentHeight = 0, scrollY = 0;
    RectF toolBtn[3];   // 标题栏右侧：新增、菜单、折叠（悬停时出现）
    int hoverBtn = 0, hoverRow = -1;
    bool hover = false;
    int HitToolbar(float xDip, float yDip) const;
    u64 flashUntil = 0;       // 定位时描边闪烁到什么时候
    bool menuChanged = false;
    int HitRow(float yDip) const;
    int HitEdge(float x, float y) const; // 窗口 DIP；返回哪条边（DragResize 的编号），0 = 不在边上

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
    void OnLButtonDown(float x, float y, bool dbl, int mods); // 窗口 DIP
    void OnContextMenu(int x, int y);
    void RunCommand(int cmd, int target);
    bool TryOpenLink(float x, float y); // 窗口 DIP
    void EditReminder(int index);
    void PickColor(bool background);

    // 窗口模式、置顶
    bool peeking = false;
    void SendToBottom();
    void RestorePosition();
    void PlaceDefault();
    RectI WorkAreaPx() const;

    // 胶囊
    bool capHidden = false, sliding = false;
    int dockedX = 0, dockedY = 0;
    bool dockL = false, dockR = false, dockT = false, dockB = false;
    int slideFromX = 0, slideFromY = 0, slideToX = 0, slideToY = 0;
    u64 slideStart = 0;
    Window* preview = nullptr;
    NotePreview* previewHandler = nullptr;
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

};

// ---------------------------------------------------------------------
// 同步到 gopan
// ---------------------------------------------------------------------
class Syncer;
Syncer* SyncCreate();
void SyncStart(Syncer* s);
void SyncNow(Syncer* s);
void SyncTouch(Syncer* s);
void SyncOnDone(Syncer* s, void* plan);
void SyncTick(Syncer* s);
const Str& SyncStatus(Syncer* s);
bool SyncFailed(Syncer* s);
Str SyncNormFolder(const Str& f);
bool SyncBusy(Syncer* s);
bool SyncLiveConnected(); // 实时推送连着没有

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
    Window* trashWnd = nullptr;
    Window* manageWnd = nullptr;
    Window* settingsWnd = nullptr;
    Window* quickWnd = nullptr;
    NoteWin* balloonNote = nullptr;
    Syncer* sync = nullptr;

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
    void ShowManage();
    void TogglePeek();
    void EndPeek();
    void BringAllOnScreen();
    void CheckReminders();
    void ShowNotice(const Str& title, const Str& text, NoteWin* w);
    void OnNoteSaved();
    void OpenSyncedNote(NoteData* n);
    void CreateNoteFromContent(const Str& json, const wchar_t* suffix);
    enum { HotkeyPeek = 1, HotkeyQuickAdd = 2 };
    const wchar_t* ApplyHotkey(int id); // 按设置（重新）注册；nullptr = 成功或没启用
    void ApplyHotkeys();
    void PauseHotkey(int id);           // 录新快捷键时先放开旧的
    void OnHotkey(int id);
    // 快速添加：快捷键弹小输入框，加到选定的便签（没选或找不到就用第一张）
    void ShowQuickAdd();
    NoteWin* QuickAddTarget();
    bool QuickAdd(const Str& text);
    void CheckFileChanges();
    void Exit();

    // 系统外壳（shell.h）交过来的事
    enum { TimerReminders = 1, TimerGfx = 2, TimerBackup = 3, TimerFiles = 4, TimerSync = 5, TimerRemote = 6 };
    void OnTimer(int id);
    void OnTrayClick() { TogglePeek(); }
    void OnTrayMenu(int sx, int sy);
    void OnNoticeClicked();
    void OnFolderChanged();
    void OnRemoteChange();
    void TrayMenu(int sx, int sy);
};
extern App* g_app;

// 对话框（dialogs.cpp）
Window* ManageWindowCreate();
void ManageWindowRefresh(Window* w);
Window* TrashWindowCreate();
void TrashWindowRefresh(Window* w);
Window* SettingsWindowCreate();
Window* QuickAddWindowCreate();
void QuickAddWindowFocus(Window* w);
void SettingsWindowSyncStatus(Window* w);
int ReminderDialog(void* owner, const Str& itemText, bool hasCurrent, Time current, int* repeat, Time* result); // 0 取消，1 设定，2 移除；repeat 进出都是 Repeat*
bool HotkeyParse(const Str& s, int* mods, int* vk); // 修饰键是 Mod*，按键是 Key*
Str HotkeyFormat(int mods, int vk);


// 自测（selftest.cpp）
void SelfTestRun(const Str& phase, const Str& outFile, const Str& server, const Str& key);
void MemDiagRun(const Str& outFile);
