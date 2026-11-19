// 程序主体：便签增删、回收站、提醒、同步联动、外部修改、提到最前、托盘菜单。
// 系统那边的事（托盘、通知、快捷键、开机自启、定时器、文件夹监视、入口和主循环）在 shell.h 后面
#include "app.h"
#include "menu.h"
#include "shell.h"
#include "syncengine.h"

App* g_app;
static Vec<TrashEntry*> g_deadTrash; // 移出回收站的记录：撤销步骤可能还引用着，留到退出

// =====================================================================
// 启动
// =====================================================================
void App::Start()
{
    settings = Store::LoadSettings();
    Store::LoadTrash(trash);

    Vec<NoteData*> list;
    Store::LoadNotes(list);
    if (!settings.autoStartInit)
    {
        // 全新安装（还没有任何便签）默认开机自启；已有数据的老用户保持原样
        if (list.n == 0 && isDefaultDir) Shell::AutoStartSet(true);
        settings.autoStartInit = true;
        Store::SaveSettings(settings);
    }
    // exe 挪过位置后，把开机自启指向现在这个 exe（只在已打开自启时更新）
    if (isDefaultDir) Shell::AutoStartFix();
    if (list.n == 0)
    {
        NoteData* n = new NoteData();
        n->id = NoteData::NewId();
        Store::SaveNote(n);
        list.Push(n);
    }

    for (int i = 0; i < list.n; i++) OpenWindow(list[i]);
    Store::DailyBackup();
    Shell::TrayShow();
    ApplyHotkeys();
    Shell::WatchFolder(Store::NotesDir);
    Shell::SetTimer(TimerReminders, 15000);
    Shell::SetTimer(TimerGfx, 1000);
    Shell::SetTimer(TimerBackup, 60 * 60 * 1000);
    sync = SyncCreate();
    SyncStart(sync);
    CheckReminders(); // 关机期间错过的提醒，启动后马上补
}

NoteWin* App::OpenWindow(NoteData* n)
{
    NoteWin* w = new NoteWin(n);
    notes.Push(w);
    if (!n->hidden) w->Create(); // 隐藏的不建窗口
    if (manageWnd) ManageWindowRefresh(manageWnd);
    return w;
}

NoteWin* App::Find(const Str& id)
{
    for (int i = 0; i < notes.n; i++) if (notes[i]->d->id == id) return notes[i];
    return nullptr;
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
        if (!Wnd::Ask(w->win, Str(L"删除便签"), msg)) return;
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
    if (trashWnd) Wnd::Show(trashWnd);
}

void App::ShowManage()
{
    if (!manageWnd) manageWnd = ManageWindowCreate();
    if (manageWnd) Wnd::Show(manageWnd);
}

void App::ShowSettings()
{
    if (!settingsWnd) settingsWnd = SettingsWindowCreate();
    if (settingsWnd) Wnd::Show(settingsWnd);
}

// =====================================================================
// 临时把所有便签提到最前（快捷键 / 单击托盘）
// =====================================================================
void App::TogglePeek()
{
    if (peeking) { EndPeek(); return; }
    NoteWin* last = nullptr;
    for (int i = 0; i < notes.n; i++) if (notes[i]->win) last = notes[i]; // 隐藏的便签不参与
    if (!last) return;
    peeking = true;
    for (int i = 0; i < notes.n; i++) if (notes[i]->win) notes[i]->BeginPeek();
    Wnd::Foreground(last->win);
}

void App::EndPeek()
{
    peeking = false;
    for (int i = 0; i < notes.n; i++)
    {
        NoteWin* w = notes[i];
        if (w->tempShown) { w->tempShown = false; w->Destroy(); } // 因提醒临时显示的隐藏便签：收回去
        else if (w->win) w->EndPeek();
    }
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
            // 重复的不看「已提醒」（别处同步来的、改成重复前响过的，都要顺延）
            if (!it.hasRemind || it.remind > now || (it.repeat == RepeatNone && it.reminded)) continue;
            if (it.repeat != RepeatNone)
            {
                // 重复的：顺延到下一次；已经勾掉的重新打开（今天做完了，下次还要做）
                it.done = false;
                it.reminded = false;
                it.remind = NextRepeat(it.remind, it.repeat, now);
            }
            else if (it.done) continue;
            else it.reminded = true;
            if (!first) first = &it;
            count++;
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
    Shell::Notify(title, text);
}

// =====================================================================
// 同步联动
// =====================================================================
void App::OnNoteSaved()
{
    if (sync) SyncTouch(sync);
    if (manageWnd) ManageWindowRefresh(manageWnd);
}

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
    Vec<DirEntry> files;
    if (!ListDir(Store::NotesDir.c(), files)) return;
    Vec<Str> ids;
    for (int i = 0; i < files.n; i++)
        if (!files[i].isDir && files[i].name.EndsWith(L".json")) ids.Push(PathStem(files[i].name));
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
const wchar_t* App::ApplyHotkey(int id)
{
    Shell::HotkeyUnregister(id);
    bool on = id == HotkeyPeek ? settings.hotkeyEnabled : settings.quickAddEnabled;
    const Str& hk = id == HotkeyPeek ? settings.hotkey : settings.quickAddHotkey;
    if (!on) return nullptr;
    int mods, vk;
    if (!HotkeyParse(hk, &mods, &vk)) return L"快捷键格式不对";
    if (!Shell::HotkeyRegister(id, mods, vk)) return L"注册失败，可能被其他软件占用了";
    return nullptr;
}

void App::ApplyHotkeys()
{
    ApplyHotkey(HotkeyPeek);
    ApplyHotkey(HotkeyQuickAdd);
}

void App::PauseHotkey(int id) { Shell::HotkeyUnregister(id); }

void App::OnHotkey(int id)
{
    if (id == HotkeyPeek) TogglePeek();
    else if (id == HotkeyQuickAdd) ShowQuickAdd();
}

// =====================================================================
// 快速添加
// =====================================================================
NoteWin* App::QuickAddTarget()
{
    if (settings.quickAddNote.n)
    {
        NoteWin* w = Find(settings.quickAddNote);
        if (w && !w->deleted) return w;
    }
    return notes.n ? notes[0] : nullptr;
}

bool App::QuickAdd(const Str& text)
{
    Str t = text.Trim();
    if (!t.n) return false;
    NoteWin* w = QuickAddTarget();
    if (!w)
    {
        // 一张便签都没有：新建一张
        NoteData* n = new NoteData();
        n->id = NoteData::NewId();
        Store::SaveNote(n);
        w = OpenWindow(n);
    }
    w->AddItem(t);
    return true;
}

void App::ShowQuickAdd()
{
    if (!quickWnd) quickWnd = QuickAddWindowCreate();
    if (!quickWnd) return;
    Wnd::Show(quickWnd);
    QuickAddWindowFocus(quickWnd);
}

// =====================================================================
// 退出
// =====================================================================
void App::Exit()
{
    if (exiting) return;
    exiting = true;
    for (int i = 0; i < notes.n; i++) { notes[i]->Destroy(); }
    Shell::HotkeyUnregister(HotkeyPeek);
    Shell::HotkeyUnregister(HotkeyQuickAdd);
    Shell::TrayHide();
    if (quickWnd) Wnd::Destroy(quickWnd);
    if (trashWnd) Wnd::Destroy(trashWnd);
    if (manageWnd) Wnd::Destroy(manageWnd);
    if (settingsWnd) Wnd::Destroy(settingsWnd);
    Shell::StopWatching();
    Shell::Quit();
}

// =====================================================================
// 系统外壳交过来的事
// =====================================================================
void App::TrayMenu(int sx, int sy)
{
    enum { CShow = 1, CNew, CBring, CTrash, CSettings, CExit, CManage };
    Ui::Menu m;
    m.Item(CShow, L"便签提到最前", 0xE7C4, settings.hotkeyEnabled ? settings.hotkey.c() : nullptr);
    m.Item(CNew, L"新建便签", 0xE70B);
    m.Item(CBring, L"把便签拉回屏幕", 0xE7F4);
    m.Sep();
    m.Item(CManage, L"便签管理", 0xE71D);
    m.Item(CTrash, L"回收站", 0xE7B8);
    m.Item(CSettings, L"设置", 0xE713);
    m.Sep();
    m.Item(CExit, L"退出", 0xE7E8);
    int cmd = Ui::MenuRun(Shell::MainHandle(), &m, sx, sy, nullptr, nullptr);
    switch (cmd)
    {
    case CShow: TogglePeek(); break;
    case CNew: CreateNote(notes.n ? notes.Last() : nullptr); break;
    case CBring: BringAllOnScreen(); break;
    case CTrash: ShowTrash(); break;
    case CSettings: ShowSettings(); break;
    case CManage: ShowManage(); break;
    case CExit: Exit(); break;
    }
}

void App::OnTrayMenu(int sx, int sy) { TrayMenu(sx, sy); }

void App::OnNoticeClicked()
{
    if (balloonNote && notes.Contains(balloonNote))
    {
        if (!balloonNote->win) balloonNote->ShowTemporarily(); // 隐藏着：临时显示
        balloonNote->Expand();
        if (!peeking) TogglePeek();
    }
}

void App::OnFolderChanged() { Shell::SetTimer(TimerFiles, 300); } // 等对方写完再读

// 网盘里有改动：稍等一下，连着来的几条合成一次同步
void App::OnRemoteChange() { Shell::SetTimer(TimerRemote, 300); }

void App::OnTimer(int id)
{
    switch (id)
    {
    case TimerReminders: CheckReminders(); break;
    case TimerGfx: Gfx::ReleaseIfIdle(); SyncTick(sync); break;
    case TimerBackup: Store::DailyBackup(); break;
    case TimerFiles: Shell::KillTimer(TimerFiles); CheckFileChanges(); break;
    case TimerRemote: Shell::KillTimer(TimerRemote); SyncNow(sync); break;
    }
}
