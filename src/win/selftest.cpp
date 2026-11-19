// 自测：DeskNote.exe --data <空目录> --selftest <1|2> <结果文件> [--sync-server <地址> --sync-key <密钥>]
// 在真实的程序对象上跑一遍常用操作，结果逐行写进结果文件（PASS/FAIL），跑完自动退出。
// 内存诊断：--data <目录> --memdiag <结果文件>
#include "app.h"
#include "window_win.h"
#include <psapi.h>

JVal* SyncTestCall(const Str& server, const Str& key, const wchar_t* tool, JVal* args);
HANDLE AppWatchHandle();
void AppWatchSignaled();

static Str g_log;
static int g_fails;

static void Check(const wchar_t* name, bool ok, const Str& detail = Str())
{
    if (!ok) g_fails++;
    g_log += ok ? L"PASS " : L"FAIL ";
    g_log += name;
    if (!ok && detail.n) { g_log += L"  -- "; g_log += detail; }
    g_log += L"\r\n";
}

// 让消息循环跑一会儿（计时器、后台线程的回调都靠它）
static void Pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    while (true)
    {
        int left = (int)(end - GetTickCount());
        if (left <= 0) break;
        HANDLE watch = AppWatchHandle();
        DWORD r = watch != INVALID_HANDLE_VALUE ? MsgWaitForMultipleObjects(1, &watch, FALSE, (DWORD)left, QS_ALLINPUT)
                                                : MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)left, QS_ALLINPUT);
        if (watch != INVALID_HANDLE_VALUE && r == WAIT_OBJECT_0) AppWatchSignaled();
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

template <class F> static bool WaitUntil(F cond, DWORD timeout)
{
    DWORD end = GetTickCount() + timeout;
    while ((int)(end - GetTickCount()) > 0) { if (cond()) return true; Pump(50); }
    return cond();
}

static NoteData* ReadNoteFile(const Str& id)
{
    Str content;
    if (!ReadFileText(Store::NotePath(id).c(), &content)) return nullptr;
    JVal* j = JParse(content);
    NoteData* n = NoteData::From(j, id);
    delete j;
    return n;
}

static int FileItemCount(const Str& id)
{
    NoteData* n = ReadNoteFile(id);
    int c = n ? n->items.n : -1;
    delete n;
    return c;
}

static Str NoteFiles()
{
    Vec<Str> ids;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(PathJoin(Store::NotesDir, L"*.json").c(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do ids.Push(PathStem(Str(fd.cFileName)));
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (int i = 1; i < ids.n; i++)
        for (int k = i; k > 0 && CompareStringOrdinal(ids[k - 1].c(), -1, ids[k].c(), -1, FALSE) == CSTR_GREATER_THAN; k--) Swap(ids[k - 1], ids[k]);
    Str s;
    for (int i = 0; i < ids.n; i++) { if (i) s += L","; s += ids[i]; }
    return s;
}

static Str Texts(NoteData* d)
{
    Str s;
    for (int i = 0; i < d->items.n; i++) { if (i) s += L" / "; s += d->items[i].text; }
    return s;
}

static bool HasItem(NoteData* d, const wchar_t* t)
{
    for (int i = 0; i < d->items.n; i++) if (d->items[i].text.Eq(t)) return true;
    return false;
}

static Str Num(int v) { Str s; s.Int(v); return s; }

static Vec<Str> RemoteNames(const Str& server, const Str& key)
{
    Vec<Str> names;
    JVal* a = JNewObj();
    a->SetStr(L"path", Str(L"/DeskNote"));
    JVal* r = SyncTestCall(server, key, L"list_files", a);
    JVal* items = r ? r->Get(L"items") : nullptr;
    if (items) for (int i = 0; i < items->Count(); i++) names.Push(items->At(i)->Str_(L"name", L""));
    delete r;
    return names;
}

static Str RemoteRead(const Str& server, const Str& key, const Str& path, Str* sha)
{
    JVal* a = JNewObj();
    a->SetStr(L"path", path);
    JVal* r = SyncTestCall(server, key, L"read_text_file", a);
    Str text = r ? r->Str_(L"text", L"") : Str();
    if (sha) *sha = r ? r->Str_(L"sha256", L"") : Str();
    delete r;
    return text;
}

static void RemoteWrite(const Str& server, const Str& key, const Str& path, const Str& content, const Str& base)
{
    JVal* a = JNewObj();
    a->SetStr(L"path", path)->SetStr(L"content", content)->SetStr(L"base_sha256", base);
    delete SyncTestCall(server, key, L"write_text_file", a);
}

static void SyncTests(const Str& server, const Str& key)
{
    App* app = g_app;
    Settings& s = app->settings;
    s.syncEnabled = true; s.syncServer = server; s.syncKey = key; s.syncFolder = L"/DeskNote";
    Store::SaveSettings(s);
    Str settingsText;
    ReadFileText(Store::SettingsFile.c(), &settingsText);
    Check(L"密钥加密保存", settingsText.Find(key.c()) < 0);
    Check(L"密钥能解回来", Store::LoadSettings().syncKey == key);

    auto idle = [&]() { return !SyncBusy(app->sync); };
    SyncStart(app->sync);
    SyncNow(app->sync);
    WaitUntil([&]() { return SyncStatus(app->sync).StartsWith(L"上次同步") || SyncFailed(app->sync); }, 15000);
    Check(L"首次同步成功", SyncStatus(app->sync).StartsWith(L"上次同步") && !SyncFailed(app->sync), SyncStatus(app->sync));
    Vec<Str> names = RemoteNames(server, key);
    Check(L"两张便签都传上去了", names.n == 2, Num(names.n));

    NoteWin* a = app->notes[0];
    Str path = L"/DeskNote/" + a->d->id + L".json";
    Str sha;
    Str remoteText = RemoteRead(server, key, path, &sha);
    Check(L"远端内容不含窗口位置", remoteText.Find(L"\"window\"") < 0 && remoteText.Find(L"\"items\"") >= 0);

    // 别的电脑改了：拉下来
    NoteData* other = NoteData::FromContentJson(remoteText, a->d->id);
    NoteItem it; it.text = L"另一台电脑加的";
    other->items.Push(it);
    RemoteWrite(server, key, path, other->ContentJson(), sha);
    delete other;
    SyncNow(app->sync);
    WaitUntil([&]() { return HasItem(a->d, L"另一台电脑加的"); }, 15000);
    Check(L"拉下另一台电脑的修改", HasItem(a->d, L"另一台电脑加的"));
    Check(L"拉取后没有冲突副本", app->notes.n == 2, Num(app->notes.n));
    WaitUntil(idle, 10000);

    // 实时推送(gopan doc/39 五):别的电脑一改,不用等定时拉取,几秒内就拉下来
    WaitUntil([&]() { return SyncLiveConnected(); }, 8000);
    Check(L"连上了实时推送", SyncLiveConnected());
    {
        Str t = RemoteRead(server, key, path, &sha);
        NoteData* o = NoteData::FromContentJson(t, a->d->id);
        NoteItem li; li.text = L"推送带来的";
        o->items.Push(li);
        RemoteWrite(server, key, path, o->ContentJson(), sha);
        delete o;
        DWORD t0 = GetTickCount();
        WaitUntil([&]() { return HasItem(a->d, L"推送带来的"); }, 5000);
        Str ms;
        ms.Int((int)(GetTickCount() - t0)).Append(L" ms");
        Check(L"远端一改几秒内自动拉下来(没手动同步)", HasItem(a->d, L"推送带来的"), ms);
        WaitUntil(idle, 10000);
    }

    // 本地改：几秒后自动推上去
    a->TestTypeItem(a->d->items.n, L"本地刚加的");
    WaitUntil([&]() { return RemoteRead(server, key, path, nullptr).Find(L"本地刚加的") >= 0; }, 15000);
    Check(L"本地修改自动推上去", RemoteRead(server, key, path, nullptr).Find(L"本地刚加的") >= 0);
    WaitUntil(idle, 10000);

    auto findCopy = [&]() -> NoteWin* { for (int i = 0; i < app->notes.n; i++) if (app->notes[i]->d->title.EndsWith(L"（冲突副本）")) return app->notes[i]; return nullptr; };
    // 改远端。推送会让本地马上同步，所以要测「两边都改了」就先改本地、紧接着改远端（本地的推送要等 1.5 秒）
    auto remoteEdit = [&](void (*fn)(NoteData*)) {
        Str t = RemoteRead(server, key, path, &sha);
        NoteData* o = NoteData::FromContentJson(t, a->d->id);
        fn(o);
        // 换一种排版写回（网页、App 写的就和桌面端排得不一样）
        RemoteWrite(server, key, path, o->ContentJson().Replace(L"\r\n", L"\n").Replace(L"  ", L"\t"), sha);
        delete o;
    };

    // 两边同时改了不同的地方：按条目合并，不生成副本
    WaitUntil(idle, 10000);
    Str first = a->d->items[0].text;
    a->TestTypeItem(a->d->items.n, L"冲突时本地加的");
    remoteEdit([](NoteData* o) {
        o->title = L"远端改的标题";
        o->items[0].done = true;
        NoteItem ri; ri.text = L"远端加的";
        o->items.Push(ri);
    });
    SyncNow(app->sync);
    WaitUntil([&]() { return HasItem(a->d, L"远端加的") && HasItem(a->d, L"冲突时本地加的"); }, 15000);
    WaitUntil(idle, 10000);
    Check(L"两边改了不同地方：合并而不是另存副本", findCopy() == nullptr);
    Check(L"合并后有远端改的标题", a->d->title.Eq(L"远端改的标题"), a->d->title);
    Check(L"合并后两边加的条目都在", HasItem(a->d, L"远端加的") && HasItem(a->d, L"冲突时本地加的"));
    Check(L"合并后远端的勾选也在", a->d->items.n > 0 && a->d->items[0].text == first && a->d->items[0].done);
    Str merged = RemoteRead(server, key, path, nullptr);
    Check(L"合并结果写回了远端", merged.Find(L"远端加的") >= 0 && merged.Find(L"冲突时本地加的") >= 0);

    // 同一条两边改成了不同的文字：两个版本都留下
    WaitUntil(idle, 10000);
    for (int i = 0; i < a->d->items.n; i++) if (a->d->items[i].text.Eq(L"远端加的")) a->TestTypeItem(i, L"远端加的（本地改）");
    remoteEdit([](NoteData* o) { for (int i = 0; i < o->items.n; i++) if (o->items[i].text.Eq(L"远端加的")) o->items[i].text = L"远端加的（远端改）"; });
    SyncNow(app->sync);
    WaitUntil([&]() { return HasItem(a->d, L"远端加的（远端改）"); }, 15000);
    WaitUntil(idle, 10000);
    Check(L"同一条两边都改了：两个版本都留下", HasItem(a->d, L"远端加的（远端改）") && HasItem(a->d, L"远端加的（本地改）") && findCopy() == nullptr);

    // 两边改成了一样的内容，只是排版不同：不算冲突
    WaitUntil(idle, 10000);
    a->TestTypeItem(a->d->items.n, L"两边都加的");
    remoteEdit([](NoteData* o) { NoteItem ri; ri.text = L"两边都加的"; o->items.Push(ri); });
    SyncNow(app->sync);
    WaitUntil(idle, 15000);
    Pump(500);
    WaitUntil(idle, 10000);
    int both = 0;
    for (int i = 0; i < a->d->items.n; i++) if (a->d->items[i].text.Eq(L"两边都加的")) both++;
    Check(L"内容一样只是排版不同：不重复、不另存副本", both == 1 && findCopy() == nullptr, Num(both));

    // 删除测试用的便签（原来用冲突副本，现在不生成副本了）
    app->CreateNoteFromContent(a->d->ContentJson(), L"（删除测试）");
    NoteWin* copy = nullptr;
    for (int i = 0; i < app->notes.n; i++) if (app->notes[i]->d->title.EndsWith(L"（删除测试）")) copy = app->notes[i];
    WaitUntil(idle, 10000);

    // 本地删除：远端也删
    SyncNow(app->sync);
    WaitUntil(idle, 15000);
    if (copy)
    {
        Str cid = copy->d->id;
        app->DeleteNote(copy, false);
        Pump(200);
        SyncNow(app->sync);
        WaitUntil(idle, 15000);
        Pump(300);
        Vec<Str> ns = RemoteNames(server, key);
        Check(L"本地删除后远端也删掉", !ns.Contains(cid + L".json"));
    }

    // 远端删除：本地进回收站
    NoteWin* b = app->notes.Last();
    Str bid = b->d->id;
    JVal* la = JNewObj();
    la->SetStr(L"path", Str(L"/DeskNote"));
    JVal* list = SyncTestCall(server, key, L"list_files", la);
    Str nodeId;
    JVal* items = list ? list->Get(L"items") : nullptr;
    if (items) for (int i = 0; i < items->Count(); i++) if (items->At(i)->Str_(L"name", L"") == bid + L".json") nodeId = items->At(i)->Str_(L"id", L"");
    delete list;
    if (nodeId.n)
    {
        JVal* ta = JNewObj();
        JVal* ids = JNewArr();
        ids->Add(JNewStr(nodeId));
        ta->Set(L"node_ids", ids);
        delete SyncTestCall(server, key, L"trash_nodes", ta);
    }
    int trashBefore = app->trash.n;
    SyncNow(app->sync);
    WaitUntil([&]() { return app->Find(bid) == nullptr; }, 15000);
    Check(L"远端删除后本地移进回收站", app->Find(bid) == nullptr && app->trash.n == trashBefore + 1);
    WaitUntil(idle, 10000);
    Pump(500);
    Check(L"最后一次同步没有失败", !SyncFailed(app->sync), SyncStatus(app->sync));
}

static void Phase1(const Str& server, const Str& key)
{
    App* app = g_app;
    SYSTEMTIME st;
    GetLocalTime(&st);
    Str today;
    today.Int(st.wYear).Ch(L'-').Pad2(st.wMonth).Ch(L'-').Pad2(st.wDay);
    Check(L"首次启动有一张便签", app->notes.n == 1, Num(app->notes.n));
    Check(L"启动时做了今天的备份", DirExists(PathJoin(Store::BackupsDir, today.c()).c()));
    NoteWin* a = app->notes[0];

    // ---- 拖着挪位置不改大小，拖边框才改（跨缩放不同的屏拖动曾经越拖越大）----
    {
        HWND h = Wnd::H(a->win);
        float S = GetDpiForWindow(h) / 96.f;
        double w0 = a->d->width, h0 = a->d->height;
        RECT r;
        GetWindowRect(h, &r);
        int pw = r.right - r.left, ph = r.bottom - r.top;
        SendMessageW(h, WM_ENTERSIZEMOVE, 0, 0);
        // 模拟系统拖动循环按别的像素大小摆窗口（跨屏时会发生）
        SetWindowPos(h, nullptr, r.left + 30, r.top + 20, pw * 2, ph * 2, SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(h, WM_EXITSIZEMOVE, 0, 0);
        Check(L"拖动挪位置不改便签大小", a->d->width == w0 && a->d->height == h0, Num((int)a->d->width));
        GetWindowRect(h, &r);
        Check(L"拖动后窗口还是便签的大小", r.right - r.left == pw && r.bottom - r.top == ph, Num(r.right - r.left));
        SendMessageW(h, WM_ENTERSIZEMOVE, 0, 0);
        RECT sr = { r.left, r.top, r.left + (int)(400 * S), r.bottom };
        SendMessageW(h, WM_SIZING, WMSZ_RIGHT, (LPARAM)&sr);
        SetWindowPos(h, nullptr, 0, 0, (int)(400 * S), ph, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(h, WM_EXITSIZEMOVE, 0, 0);
        Check(L"拖边框能改宽度", Abs(a->d->width - 400) < 1, Num((int)a->d->width));
        a->d->width = w0;
        a->Render();
        a->SaveLayout();
    }

    // ---- 添加、撤销、重做 ----
    a->TestTypeItem(a->d->items.n, L"第一条");
    a->TestTypeItem(a->d->items.n, L"第二条 https://example.com/a?b=1。");
    a->TestTypeItem(a->d->items.n, L"第三条");
    Check(L"添加 3 条", a->d->items.n == 3, Num(a->d->items.n));
    Check(L"添加后已存盘", FileItemCount(a->d->id) == 3);
    a->Undo();
    Check(L"撤销添加", a->d->items.n == 2 && FileItemCount(a->d->id) == 2);
    a->Redo();
    Check(L"重做添加", a->d->items.n == 3 && a->d->items[2].text.Eq(L"第三条"));
    a->TestTypeItem(0, L"第一条（改过）");
    a->Undo();
    Check(L"撤销修改文字", a->d->items[0].text.Eq(L"第一条"));

    // ---- 删除一条进回收站，撤销后回收站里也拿掉 ----
    int trash0 = app->trash.n;
    a->TestMenuDelete(2);
    Check(L"删除一条进回收站", a->d->items.n == 2 && app->trash.n == trash0 + 1);
    a->Undo();
    Check(L"撤销删除：条目回来、回收站记录拿掉", a->d->items.n == 3 && app->trash.n == trash0);
    a->Redo();
    Check(L"重做删除：回收站记录又放回去", a->d->items.n == 2 && app->trash.n == trash0 + 1);
    a->Undo();
    a->TestToggleDone(1);
    a->Undo();
    Check(L"撤销勾选", !a->d->items[1].done);

    // ---- 清空文字 = 删除 ----
    a->TestTypeItem(2, L"");
    Check(L"清空文字删除并进回收站", a->d->items.n == 2 && app->trash.n == trash0 + 1, Num(app->trash.n));
    a->Undo();
    Check(L"撤销清空", a->d->items.n == 3 && app->trash.n == trash0);

    // ---- 链接 ----
    Vec<LinkSpan> links;
    FindLinks(Str(L"看 https://example.com/a?b=1。和 www.qq.com，完"), links);
    Check(L"识别出两个链接", links.n == 2, Num(links.n));
    Check(L"链接末尾的中文句号不算进去", links.n > 0 && links[0].url.Eq(L"https://example.com/a?b=1"), links.n ? links[0].url : Str());
    Check(L"www 开头补上 http://", links.n > 1 && links[1].url.Eq(L"http://www.qq.com"));

    // ---- 提醒 ----
    Time now = Now();
    SYSTEMTIME ds;
    SplitTime(now - TicksPerMinute, &ds);
    Time due = MakeTime(ds.wYear, ds.wMonth, ds.wDay, ds.wHour, ds.wMinute, 0);
    a->TestSetRemind(0, due);
    a->TestSetRemind(1, now + TicksPerDay);
    app->CheckReminders();
    Check(L"到点的提醒标记为已提醒", a->d->items[0].reminded);
    Check(L"没到点的不提醒", !a->d->items[1].reminded);
    NoteData* fa = ReadNoteFile(a->d->id);
    Check(L"提醒存进文件", fa && fa->items[0].hasRemind && fa->items[0].remind == due && fa->items[0].reminded && fa->items[1].hasRemind && !fa->items[1].reminded);
    delete fa;

    // ---- 重复提醒：到点后顺延到下一次，已勾掉的重新打开，不变红 ----
    a->TestSetRemind(1, due, RepeatDaily);
    a->d->items[1].done = true;
    app->CheckReminders();
    Check(L"每天重复：顺延到明天同一时刻", a->d->items[1].hasRemind && a->d->items[1].remind == due + TicksPerDay && !a->d->items[1].reminded,
          FormatTime(a->d->items[1].remind, false));
    Check(L"每天重复：已勾掉的重新打开", !a->d->items[1].done);
    fa = ReadNoteFile(a->d->id);
    Check(L"重复规律存进文件", fa && fa->items[1].repeat == RepeatDaily && fa->items[1].remind == due + TicksPerDay);
    delete fa;
    app->CheckReminders();
    Check(L"顺延后今天不再提醒", a->d->items[1].remind == due + TicksPerDay);
    a->TestSetRemind(1, due, RepeatWeekly);
    a->d->items[1].reminded = true; // 别处同步来的「已提醒」
    app->CheckReminders();
    Check(L"重复的带着已提醒标记也会顺延", a->d->items[1].remind == due + 7 * TicksPerDay && !a->d->items[1].reminded,
          FormatTime(a->d->items[1].remind, false));
    a->TestSetRemind(1, now + TicksPerDay);

    // ---- 快速添加 ----
    {
        Str keepTarget = app->settings.quickAddNote;
        app->settings.quickAddNote = a->d->id;
        int n0 = a->d->items.n;
        Check(L"快速添加：加到选定的便签", app->QuickAddTarget() == a && app->QuickAdd(Str(L"  快速添加的一条 ")) && a->d->items.n == n0 + 1 &&
              a->d->items[a->d->items.n - 1].text.Eq(L"快速添加的一条"), Texts(a->d));
        Check(L"快速添加：空内容不加", !app->QuickAdd(Str(L"   ")) && a->d->items.n == n0 + 1);
        NoteData* qa = ReadNoteFile(a->d->id);
        Check(L"快速添加：存进文件", qa && qa->items.n == n0 + 1);
        delete qa;
        app->settings.quickAddNote = L"不存在的便签";
        Check(L"快速添加：选的便签找不到时用第一张", app->QuickAddTarget() == app->notes[0]);
        app->settings.quickAddNote = keepTarget;
        a->Undo();
        Check(L"快速添加能撤销", a->d->items.n == n0, Texts(a->d));
        app->ShowQuickAdd();
        Pump(200);
        Check(L"快速添加窗口能打开", app->quickWnd != nullptr);
        if (app->quickWnd) Wnd::Destroy(app->quickWnd);
        Pump(100);
        Check(L"快速添加窗口关掉后清空", app->quickWnd == nullptr);
    }

    // ---- 编辑时外部改了文件：合并而不是覆盖 ----
    a->TestBeginEdit(1, L"正在编辑的这条");
    NoteData* ext = NoteData::FromContentJson(a->d->ContentJson(), a->d->id);
    NoteItem ei; ei.text = L"外部加的一条";
    ext->items.Insert(0, ei);
    a->ReloadFrom(ext, true);
    a->CommitEdit(true);
    Check(L"外部新增的条目保留", HasItem(a->d, L"外部加的一条"), Texts(a->d));
    Check(L"编辑内容落在原来那条上", HasItem(a->d, L"正在编辑的这条") && !HasItem(a->d, L"第二条 https://example.com/a?b=1。"), Texts(a->d));
    Check(L"合并后条数正确", a->d->items.n == 4, Num(a->d->items.n));

    // ---- 新建、删除整张便签（删掉后不能被关窗时的保存写回来） ----
    app->CreateNote(a);
    NoteWin* b = app->notes.Last();
    b->CommitEdit(true);
    b->TestTypeItem(0, L"B 便签的内容");
    Str bid = b->d->id;
    Check(L"新建便签有文件", FileExists(Store::NotePath(bid).c()));
    app->DeleteNote(b, false);
    Pump(300);
    Check(L"删除便签后文件不在", !FileExists(Store::NotePath(bid).c()));
    Check(L"删除的便签进了回收站", app->trash.n > 0 && app->trash[0]->kind.Eq(L"note") && app->trash[0]->note->id == bid);
    WriteAtomic(PathJoin(Store::Dir, L"deleted-id.txt").c(), bid);

    // ---- 恢复 ----
    app->CreateNote(a);
    NoteWin* c = app->notes.Last();
    c->CommitEdit(true);
    c->TestTypeItem(0, L"C");
    Str cid = c->d->id;
    app->DeleteNote(c, false);
    app->Restore(app->trash[0]);
    Pump(300);
    Check(L"恢复的便签用回原 id，文件回来", FileExists(Store::NotePath(cid).c()) && app->Find(cid) != nullptr);
    Check(L"恢复后只有 2 张", app->notes.n == 2, Num(app->notes.n));

    // ---- 外部放进来的新文件自动打开 ----
    NoteData* dropped = new NoteData();
    dropped->id = L"zz-dropped";
    NoteItem di; di.text = L"外部放进来的";
    dropped->items.Push(di);
    JVal* dj = dropped->ToJson();
    WriteAtomic(Store::NotePath(dropped->id).c(), JWrite(dj));
    delete dj;
    delete dropped;
    WaitUntil([&]() { return app->Find(Str(L"zz-dropped")) != nullptr; }, 5000);
    Check(L"外部放进来的便签文件自动打开", app->Find(Str(L"zz-dropped")) != nullptr);
    NoteWin* z = app->Find(Str(L"zz-dropped"));
    if (z) { app->DeleteNote(z, false); Pump(200); }

    if (server.n) SyncTests(server, key);

    // ---- 交互：直接给窗口发系统消息（不动真鼠标、不抢焦点），走完整的「系统消息 → 事件 → 逻辑」 ----
    {
        bool keepFocus = NoteWin::debugNoFocus;
        NoteWin::debugNoFocus = true;
        app->CreateNoteFromContent(app->notes[0]->d->ContentJson(), L"（交互测试）");
        NoteWin* t = app->notes.Last();
        t->CommitEdit(true);
        HWND h = Wnd::H(t->win);
        // 单击勾选框
        t->d->checkboxes = true;
        t->Render();
        float cx, cy;
        bool hasRow = t->d->items.n > 0 && t->TestCheckPoint(0, &cx, &cy);
        bool before = hasRow && t->d->items[0].done;
        if (hasRow)
        {
            float S = GetDpiForWindow(h) / 96.f;
            LPARAM lp = MAKELPARAM((int)(cx * S), (int)(cy * S));
            SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
            SendMessageW(h, WM_LBUTTONUP, 0, lp);
        }
        Check(L"交互：单击勾选框勾上", hasRow && t->d->items[0].done != before);
        // 编辑时打字（WM_CHAR）
        t->BeginEdit(t->d->items.n);
        const wchar_t* typed = L"打字测试 ok";
        for (const wchar_t* p = typed; *p; p++) SendMessageW(h, WM_CHAR, *p, 0);
        t->CommitEdit(true);
        Check(L"交互：编辑时打字", HasItem(t->d, typed), Texts(t->d));
        // 输入法：组字时不进文字，上屏后才进
        t->BeginEdit(t->d->items.n);
        Event e;
        e.type = EvImeStart;
        t->OnEvent(t->win, e);
        Event c;
        c.type = EvImeUpdate;
        c.hasComp = true;
        c.imeComp = L"nihao";
        c.imeCursor = 5;
        t->OnEvent(t->win, c);
        Event r;
        r.type = EvImeUpdate;
        r.hasResult = true;
        r.imeResult = L"你好";
        t->OnEvent(t->win, r);
        Event en;
        en.type = EvImeEnd;
        t->OnEvent(t->win, en);
        t->CommitEdit(true);
        Check(L"交互：输入法上屏的字进了条目，组字没进", HasItem(t->d, L"你好") && !HasItem(t->d, L"nihao你好"), Texts(t->d));

        // 跨屏：直接挪到别的屏、在拖动循环里挪过去，窗口都要按那块屏的缩放比例
        struct Mons { RECT r[8]; RECT work[8]; int n; } mons = {};
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT rc, LPARAM l) -> BOOL {
            Mons* ms = (Mons*)l;
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfoW(m, &mi);
            if (ms->n < 8) { ms->r[ms->n] = *rc; ms->work[ms->n] = mi.rcWork; ms->n++; }
            return TRUE;
        }, (LPARAM)&mons);
        bool scaleOk = true;
        Str detail;
        for (int i = 0; i < mons.n; i++)
        {
            for (int loop = 0; loop < 2; loop++)
            {
                if (loop) SendMessageW(h, WM_ENTERSIZEMOVE, 0, 0);
                SetWindowPos(h, nullptr, mons.r[i].left + 120, mons.r[i].top + 120, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                if (loop) SendMessageW(h, WM_EXITSIZEMOVE, 0, 0);
                Pump(100);
                RECT wr;
                GetWindowRect(h, &wr);
                int want = (int)(t->d->width * GetDpiForWindow(h) / 96.0 + 0.5);
                if (wr.right - wr.left != want)
                {
                    scaleOk = false;
                    detail.Append(L"屏").Int(i).Append(loop ? L"（拖动中）" : L"").Append(L" 宽=").Int(wr.right - wr.left).Append(L" 应为=").Int(want).Append(L"；");
                }
            }
        }
        Check(L"交互：挪到别的屏后按那块屏的缩放比例画", scaleOk, detail);

        // 胶囊：离屏幕右边很近时贴边，到时间后缩进去（右边挨着别的屏时不缩）
        if (mons.n > 0)
        {
            RECT wa = mons.work[0];
            RECT wr;
            GetWindowRect(h, &wr);
            SetWindowPos(h, nullptr, wa.right - (wr.right - wr.left) - 5, wa.top + 200, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            t->Collapse();
            Pump(100);
            GetWindowRect(h, &wr);
            Check(L"交互：胶囊靠近右边就贴上去", t->TestDocked() && wr.right == wa.right, Num(wa.right - wr.right));
            POINT cur;
            GetCursorPos(&cur);
            bool cursorOver = PtInRect(&wr, cur) != FALSE;
            bool neighbor = Wnd::OnScreen(wa.right + 2, (wr.top + wr.bottom) / 2);
            SendMessageW(h, WM_TIMER, 1, 0); // 胶囊收起的定时器
            Pump(400);
            GetWindowRect(h, &wr);
            if (cursorOver || neighbor) Check(L"交互：贴边胶囊到时间缩进去（跳过：鼠标在上面或右边挨着别的屏）", true);
            else Check(L"交互：贴边胶囊到时间缩进去", t->TestCapHidden() && wr.left >= wa.right - 10, Num(wa.right - wr.left));
            t->Expand();
            Check(L"交互：缩进的胶囊能展开", !t->d->collapsed && t->win != nullptr);
        }
        app->DeleteNote(t, false);
        Pump(200);
        NoteWin::debugNoFocus = keepFocus;
    }

    // ---- 每块屏上：窗口大小 = 便签大小 × 那块屏的缩放比例（便签先建在主屏再挪过去，曾按主屏的比例画）----
    {
        struct Mons { RECT r[8]; int n; } mons = {};
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT rc, LPARAM l) -> BOOL {
            Mons* m = (Mons*)l;
            if (m->n < 8) m->r[m->n++] = *rc;
            return TRUE;
        }, (LPARAM)&mons);
        app->CreateNoteFromContent(app->notes[0]->d->ContentJson(), L"（缩放测试）");
        NoteWin* sn = app->notes.Last();
        sn->CommitEdit(true);
        bool ok = true;
        Str detail;
        for (int i = 0; i < mons.n && sn->win; i++)
        {
            sn->Destroy();
            sn->d->hasPos = true;
            sn->d->x = mons.r[i].left + 100;
            sn->d->y = mons.r[i].top + 100;
            sn->Create();
            Pump(200);
            HWND h = Wnd::H(sn->win);
            RECT wr;
            GetWindowRect(h, &wr);
            int want = (int)(sn->d->width * GetDpiForWindow(h) / 96.0 + 0.5);
            detail.Append(L"屏").Int(i).Append(L" dpi=").Int(GetDpiForWindow(h)).Append(L" 宽=").Int(wr.right - wr.left).Append(L" 应为=").Int(want).Append(L"；");
            if (wr.right - wr.left != want) ok = false;
        }
        Check(L"每块屏上便签窗口按那块屏的缩放比例画", ok, detail);
        app->DeleteNote(sn, false);
        Pump(200);
    }

    // ---- 隐藏（便签管理）：只在这台电脑上，窗口销毁，提到最前时跳过 ----
    app->CreateNoteFromContent(app->notes[0]->d->ContentJson(), L"（隐藏测试）");
    NoteWin* hdn = nullptr;
    for (int i = 0; i < app->notes.n; i++) if (app->notes[i]->d->title.EndsWith(L"（隐藏测试）")) hdn = app->notes[i];
    Check(L"隐藏测试便签建好了", hdn && hdn->win);
    if (hdn)
    {
        hdn->SetHidden(true);
        Str ft;
        ReadFileText(Store::NotePath(hdn->d->id).c(), &ft);
        Check(L"隐藏：窗口销毁、文件里记下", !hdn->win && ft.Find(L"\"hidden\"") >= 0);
        Check(L"隐藏状态不进同步的内容", hdn->d->ContentJson().Find(L"hidden") < 0);
        app->TogglePeek();
        Pump(100);
        Check(L"提到最前跳过隐藏的便签", !hdn->win);
        app->EndPeek();
        app->ShowManage();
        Pump(200);
        Check(L"便签管理窗口能打开", app->manageWnd != nullptr);
        if (app->manageWnd) Wnd::Destroy(app->manageWnd);
        Pump(50);
        // 隐藏着改设置（便签管理里的「设置」）：只记下来，显示时按新设置建窗口
        WinMode oldMode = hdn->d->mode;
        hdn->SetMode(ModeTopmost);
        hdn->Collapse();
        Str ft2;
        ReadFileText(Store::NotePath(hdn->d->id).c(), &ft2);
        Check(L"隐藏着改窗口模式和折叠：记下来", !hdn->win && hdn->d->mode == ModeTopmost && hdn->d->collapsed && ft2.Find(L"topmost") >= 0);
        hdn->SetHidden(false);
        Check(L"重新显示：窗口回来", hdn->win != nullptr && !hdn->d->hidden);
        Check(L"重新显示：按改过的设置（胶囊）", hdn->d->collapsed && hdn->d->mode == ModeTopmost);
        hdn->Expand();
        hdn->SetMode(oldMode);
        hdn->SetHidden(true);
        WriteAtomic(PathJoin(Store::Dir, L"hidden-id.txt").c(), hdn->d->id);
    }
    WriteAtomic(PathJoin(Store::Dir, L"expected-notes.txt").c(), NoteFiles());
}

static void Phase2()
{
    App* app = g_app;
    Str expected, deleted;
    ReadFileText(PathJoin(Store::Dir, L"expected-notes.txt").c(), &expected);
    ReadFileText(PathJoin(Store::Dir, L"deleted-id.txt").c(), &deleted);
    Str now = NoteFiles();
    Check(L"重启后便签文件和退出前一致", now == expected, now + L" vs " + expected);
    int count = expected.n ? 1 : 0;
    for (int i = 0; i < expected.n; i++) if (expected.p[i] == L',') count++;
    Check(L"重启后打开的便签数一致", app->notes.n == count, Num(app->notes.n));
    Check(L"删掉的便签没有回来", !app->Find(deleted) && !FileExists(Store::NotePath(deleted).c()));
    Check(L"回收站记录保留", app->trash.n > 0);
    bool reminded = false;
    for (int k = 0; k < app->notes.n; k++)
        for (int i = 0; i < app->notes[k]->d->items.n; i++) if (app->notes[k]->d->items[i].reminded) reminded = true;
    Check(L"提醒状态保留", reminded);
    Str hid;
    ReadFileText(PathJoin(Store::Dir, L"hidden-id.txt").c(), &hid);
    NoteWin* hw = app->Find(hid.Trim());
    Check(L"重启后隐藏的便签还隐藏着（没有窗口）", hw && hw->d->hidden && !hw->win);
}

void SelfTestRun(const Str& phase, const Str& outFile, const Str& server, const Str& key)
{
    g_log = Str();
    g_fails = 0;
    Pump(300);
    if (phase.Eq(L"1")) Phase1(server, key);
    else Phase2();
    Str errLog;
    bool hasErr = ReadFileText(PathJoin(Store::Dir, L"error.log").c(), &errLog);
    Check(L"没有 error.log", !hasErr, errLog);
    if (g_fails == 0) g_log += L"RESULT OK\r\n";
    else { g_log += L"RESULT "; g_log.Int(g_fails); g_log += L" FAILED\r\n"; }
    Bytes b = ToUtf8(g_log);
    WriteFileBytes(outFile.c(), b.p, b.n);
    g_app->Exit();
}

// ---------------------------------------------------------------------
// 内存诊断
// ---------------------------------------------------------------------
static void Sample(Str& out, const wchar_t* step)
{
    Pump(1500);
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    Str line(step);
    while (line.n < 16) line += L" ";
    line += L" private=";
    line.Num(pmc.PrivateUsage / 1048576.0, 2);
    line += L"MB WS=";
    line.Num(pmc.WorkingSetSize / 1048576.0, 2);
    line += L"MB handles=";
    line.Int(handles);
    out += line + L"\r\n";
}

void MemDiagRun(const Str& outFile)
{
    Str out;
    Sample(out, L"启动后");
    Pump(5000);
    Sample(out, L"空闲 5 秒后");
    NoteWin* w = g_app->notes[0];
    for (int i = 0; i < 3; i++) { g_app->ShowSettings(); Pump(300); Wnd::Destroy(g_app->settingsWnd); }
    Sample(out, L"开关设置窗口 3 次");
    for (int i = 0; i < 3; i++) { g_app->ShowTrash(); Pump(300); Wnd::Destroy(g_app->trashWnd); }
    Sample(out, L"开关回收站 3 次");
    for (int i = 0; i < 200; i++) { w->Render(); if (i % 20 == 0) Pump(10); }
    Sample(out, L"重绘 200 次");
    Pump(4500);
    Sample(out, L"空闲后（释放绘图）");
    for (int i = 0; i < 20; i++) { g_app->ShowSettings(); Pump(200); Wnd::Destroy(g_app->settingsWnd); }
    Pump(4500);
    Sample(out, L"再开关设置 20 次、空闲后（看有没有越开越多）");
    Bytes b = ToUtf8(out);
    WriteFileBytes(outFile.c(), b.p, b.n);
    g_app->Exit();
}
