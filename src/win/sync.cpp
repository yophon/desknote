// 同步器（界面线程那一半）：什么时候同步、同步完怎么改便签窗口。计算和联网在 core/syncengine.cpp
#include "app.h"
#include "syncengine.h"
#include "plat.h"

static void SyncThread(void* arg)
{
    Plan* p = (Plan*)arg;
    SyncCompute(p);
    PostMessageW(g_app->msgWnd, WM_APP_SYNCDONE, 0, (LPARAM)p);
}

// 实时推送收到改动（后台线程）：交给主线程，稍等一下合并几条再同步
static void OnRemoteChange() { PostMessageW(g_app->msgWnd, WM_APP_REMOTECHANGE, 0, 0); }

// ---------------------------------------------------------------------
// 同步器（界面线程）
// ---------------------------------------------------------------------
class Syncer
{
public:
    SyncState state;
    Str status = L"未启用";
    bool failed = false, running = false, again = false;
    DWORD debounceAt = 0, nextPeriodic = 0;
    Str readyFolder;

    bool Configured() const
    {
        const Settings& s = g_app->settings;
        return s.syncEnabled && s.syncServer.n && s.syncKey.n;
    }
    void SetStatus(const Str& s, bool f)
    {
        status = s;
        failed = f;
        if (g_app->settingsWnd) SettingsWindowSyncStatus(g_app->settingsWnd);
    }
};

Syncer* SyncCreate()
{
    Syncer* s = new Syncer();
    s->state.Load();
    return s;
}

void SyncStart(Syncer* s)
{
    s->nextPeriodic = 0;
    SyncLiveRestart(g_app->settings.syncServer, g_app->settings.syncKey, s->Configured(), OnRemoteChange);
    if (!s->Configured()) { s->SetStatus(g_app->settings.syncEnabled ? Str(L"还没填服务器地址或密钥") : Str(L"未启用"), false); return; }
    s->debounceAt = GetTickCount() + 1000;
    s->nextPeriodic = GetTickCount() + 5 * 60 * 1000; // 定时拉一次别的电脑的改动（连上实时推送后放慢到 30 分钟，只当兜底）
}

// 便签存过盘：内容跟上次同步时不一样才安排同步（只是挪了位置不算），几秒内的连续改动只同步一次
void SyncTouch(Syncer* s)
{
    if (!s || !s->Configured()) return;
    bool changed = false;
    Vec<Str> seen;
    for (int i = 0; i < g_app->notes.n && !changed; i++)
    {
        NoteWin* w = g_app->notes[i];
        if (w->deleted) continue;
        seen.Push(w->d->id);
        KnownNote* k = s->state.Find(w->d->id);
        if (!k || k->local != Sha256Hex(w->d->ContentJson())) changed = true;
    }
    for (int i = 0; i < s->state.notes.n && !changed; i++) if (!seen.Contains(s->state.notes[i].id)) changed = true;
    if (changed) s->debounceAt = GetTickCount() + 1500;
}

void SyncTick(Syncer* s)
{
    if (!s || !s->Configured()) return;
    DWORD now = GetTickCount();
    if (s->debounceAt && (int)(now - s->debounceAt) >= 0) { s->debounceAt = 0; SyncNow(s); }
    else if (s->nextPeriodic && (int)(now - s->nextPeriodic) >= 0)
    {
        // gopan 每次调用都记审计日志：有实时推送时定时拉取只是兜底，放慢
        s->nextPeriodic = now + (SyncLiveConnected() ? 30 : 5) * 60 * 1000;
        SyncNow(s);
    }
}

void SyncNow(Syncer* s)
{
    if (!s->Configured()) return;
    if (s->running) { s->again = true; return; }
    s->running = true;
    s->SetStatus(Str(L"正在同步…"), false);
    Plan* p = new Plan();
    const Settings& st = g_app->settings;
    p->server = st.syncServer;
    p->key = st.syncKey;
    p->folder = SyncNormFolder(st.syncFolder);
    if (s->state.server != p->server || s->state.folder != p->folder)
    {
        // 换了服务器或文件夹：当作第一次同步
        s->state.server = p->server;
        s->state.folder = p->folder;
        s->state.notes.Clear();
    }
    p->ensureFolder = p->folder != s->readyFolder;
    for (int i = 0; i < g_app->notes.n; i++)
    {
        NoteWin* w = g_app->notes[i];
        if (w->deleted) continue;
        LocalNote ln;
        ln.id = w->d->id;
        ln.content = w->d->ContentJson();
        p->snapshot.Push(ln);
    }
    p->known = s->state.notes;
    if (!Plat::StartThread(SyncThread, p))
    { s->running = false; delete p; s->SetStatus(Str(L"同步失败：无法启动后台线程"), true); }
}

void SyncOnDone(Syncer* s, LPARAM lp)
{
    Plan* p = (Plan*)lp;
    s->running = false;
    if (p->failed) s->SetStatus(L"同步失败：" + p->error, true);
    else
    {
        s->readyFolder = p->folder;
        // 冲突副本
        for (int i = 0; i < p->copies.n; i++) g_app->CreateNoteFromContent(p->copies[i].content, L"（冲突副本）");
        // 拉下来的内容
        for (int i = 0; i < p->pulls.n; i++)
        {
            const Str& id = p->pulls[i].id;
            NoteWin* w = g_app->Find(id);
            const Str* snap = p->Local(id);
            if (w && snap && w->d->ContentJson() != *snap)
            {
                // 同步期间本地又改了：不拿远端的盖掉。记成「本地是同步开始时那份、远端未知」，
                // 下一轮按两边都改了，用原来的底稿再合并一次，两边的改动都留下
                p->SetState(id, Sha256Hex(*snap), Str(), p->pulls[i].base0);
                continue;
            }
            NoteData* fresh = NoteData::FromContentJson(p->pulls[i].content, id);
            if (!fresh) continue;
            if (w) w->ReloadFrom(fresh, true);
            else g_app->OpenSyncedNote(fresh);
        }
        // 远端删掉的
        for (int i = 0; i < p->localDeletes.n; i++)
        {
            NoteWin* w = g_app->Find(p->localDeletes[i]);
            const Str* snap = p->Local(p->localDeletes[i]);
            if (!w) continue;
            if (snap && w->d->ContentJson() != *snap) continue; // 本地刚改过，留着（状态已丢，下一轮会推上去）
            g_app->DeleteNote(w, false);
        }
        s->state.notes = p->newState;
        s->state.lastOk = FormatTime(Now(), true);
        s->state.Save();
        Str msg = L"上次同步 " + FormatTime(Now(), true).Sub(11);
        Vec<Str> parts;
        if (p->pushed) { Str t(L"上传 "); t.Int(p->pushed); parts.Push(t); }
        if (p->pulled) { Str t(L"下载 "); t.Int(p->pulled); parts.Push(t); }
        if (p->deletedN) { Str t(L"删除 "); t.Int(p->deletedN); parts.Push(t); }
        if (p->merged) { Str t(L"合并 "); t.Int(p->merged); parts.Push(t); }
        if (p->conflicts) { Str t(L"冲突 "); t.Int(p->conflicts).Append(L"（已另存副本）"); parts.Push(t); }
        if (parts.n)
        {
            msg += L"：";
            for (int i = 0; i < parts.n; i++) { if (i) msg += L"，"; msg += parts[i]; }
        }
        else msg += L"，没有变化";
        s->SetStatus(msg, false);
    }
    delete p;
    if (s->again) { s->again = false; SyncTouch(s); s->debounceAt = GetTickCount() + 500; }
}

const Str& SyncStatus(Syncer* s)
{
    static Str out;
    out = s->status;
    if (SyncLiveConnected() && s->Configured() && !s->failed) out += L" · 已连上实时推送";
    return out;
}
bool SyncFailed(Syncer* s) { return s->failed; }
bool SyncBusy(Syncer* s) { return s->running || s->debounceAt != 0; }

