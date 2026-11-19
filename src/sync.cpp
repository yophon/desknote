// 同步到 gopan：每张便签的内容（不含窗口位置）存成远端文件夹里的 <id>.json，走 gopan 的 MCP 接口
// （list_files / read_text_file / write_text_file 带 base_sha256 防覆盖 / trash_nodes）。网络请求在后台线程。
#include "app.h"
#include <winhttp.h>
#include <bcrypt.h>

// ---------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------
static Str Sha256Hex(const Str& s)
{
    Bytes b = ToUtf8(s);
    u8 out[32] = {};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)b.p, b.n, out, 32);
    return HexOf(out, 32);
}

Str SyncNormFolder(const Str& f0)
{
    Str f = f0.Trim().Replace(L"\\", L"/");
    if (f.Empty()) f = L"/DeskNote";
    if (!f.StartsWith(L"/")) f = L"/" + f;
    while (f.n > 1 && f.EndsWith(L"/")) f.Truncate(f.n - 1);
    return f;
}

// ---------------------------------------------------------------------
// 最小的 MCP 客户端（gopan 是无状态 JSON 模式，不用先 initialize）
// ---------------------------------------------------------------------
struct Mcp
{
    Str server, key;
    Str errCode, errMsg;
    int nextId = 1;

    bool Post(const Bytes& body, Bytes* resp)
    {
        Str url = server;
        while (url.EndsWith(L"/")) url.Truncate(url.n - 1);
        url += L"/mcp";
        URL_COMPONENTSW uc = { sizeof(uc) };
        wchar_t host[256], path[1024];
        uc.lpszHostName = host; uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
        if (!WinHttpCrackUrl(url.c(), 0, 0, &uc)) { errCode = L"BAD_URL"; errMsg = L"服务器地址不对"; return false; }
        HINTERNET s = WinHttpOpen(L"DeskNote/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s) { errCode = L"NETWORK"; errMsg = L"网络初始化失败"; return false; }
        WinHttpSetTimeouts(s, 20000, 20000, 20000, 20000);
        HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
        HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
        bool ok = false;
        if (r)
        {
            Str headers = L"Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\nAuthorization: Bearer " + key + L"\r\n";
            if (WinHttpSendRequest(r, headers.c(), (DWORD)headers.n, (LPVOID)body.p, body.n, body.n, 0) && WinHttpReceiveResponse(r, nullptr))
            {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
                DWORD avail = 0;
                const DWORD bufSize = 8192;
                char* buf = (char*)MemAlloc(bufSize); // 别放栈上：超过 4KB 的局部变量要 C 运行库的栈探测
                while (WinHttpQueryDataAvailable(r, &avail) && avail)
                {
                    DWORD rd = 0;
                    if (!WinHttpReadData(r, buf, Min(avail, bufSize), &rd) || !rd) break;
                    resp->Append(buf, (int)rd);
                }
                MemFree(buf);
                if (status == 401 || status == 403) { errCode = L"UNAUTHORIZED"; errMsg = L"密钥无效或权限不够（HTTP "; errMsg.Int(status).Ch(L'）'); }
                else if (status != 200) { errCode = L"HTTP"; errMsg = L"服务器返回 HTTP "; errMsg.Int(status); }
                else ok = true;
            }
            else { errCode = L"NETWORK"; errMsg = L"连不上服务器（错误 "; errMsg.Int(GetLastError()).Ch(L'）'); }
            WinHttpCloseHandle(r);
        }
        else { errCode = L"NETWORK"; errMsg = L"连不上服务器"; }
        if (c) WinHttpCloseHandle(c);
        WinHttpCloseHandle(s);
        return ok;
    }

    // 调一个工具；成功返回结果对象（调用方 delete），失败返回 nullptr 并填 errCode/errMsg
    JVal* Call(const wchar_t* tool, JVal* args)
    {
        errCode = Str(); errMsg = Str();
        JVal* params = JNewObj();
        params->SetStr(L"name", Str(tool))->Set(L"arguments", args);
        JVal* req = JNewObj();
        req->SetStr(L"jsonrpc", Str(L"2.0"))->SetNum(L"id", nextId++)->SetStr(L"method", Str(L"tools/call"))->Set(L"params", params);
        Bytes body = ToUtf8(JWrite(req));
        delete req;
        Bytes resp;
        if (!Post(body, &resp)) return nullptr;
        Str text = FromUtf8(resp.p, resp.n);
        // 响应可能是普通 JSON，也可能是 SSE（data: {...}）
        Str t = text.Trim();
        if (!t.StartsWith(L"{"))
        {
            Str js;
            int i = 0;
            while (i < text.n)
            {
                int e = text.FindCh(L'\n', i);
                if (e < 0) e = text.n;
                Str line = text.Sub(i, e - i).Trim();
                if (line.StartsWith(L"data:")) js += line.Sub(5).Trim();
                i = e + 1;
            }
            t = js;
        }
        JVal* root = JParse(t);
        if (!root) { errCode = L"BAD_RESPONSE"; errMsg = L"服务器返回的不是 JSON"; return nullptr; }
        JVal* err = root->Get(L"error");
        if (err && err->t == JObj) { errCode = L"RPC"; errMsg = err->Str_(L"message", L"调用失败"); delete root; return nullptr; }
        JVal* result = root->Get(L"result");
        if (!result || result->t != JObj) { errCode = L"BAD_RESPONSE"; errMsg = L"没有 result"; delete root; return nullptr; }
        Str firstText;
        bool hasText = false;
        JVal* content = result->Get(L"content");
        if (content && content->t == JArr)
            for (int i = 0; i < content->Count(); i++)
            {
                JVal* c = content->At(i);
                if (c->t == JObj && c->Str_(L"type", L"").Eq(L"text")) { firstText = c->Str_(L"text", L""); hasText = true; break; }
            }
        if (result->Bool(L"isError", false))
        {
            // gopan 的业务错误是“CODE: 说明”
            int colon = firstText.FindCh(L':');
            Str head = colon > 0 ? firstText.Sub(0, colon).Trim() : Str();
            bool upper = head.n > 0;
            for (int i = 0; i < head.n; i++) if (!((head.p[i] >= L'A' && head.p[i] <= L'Z') || head.p[i] == L'_')) upper = false;
            errCode = upper ? head : Str(L"TOOL_ERROR");
            errMsg = colon > 0 ? firstText.Sub(colon + 1).Trim() : firstText;
            delete root;
            return nullptr;
        }
        // 把 structuredContent 摘下来返回，免得复制
        for (int i = 0; i < result->keys.n; i++)
            if (result->keys[i].Eq(L"structuredContent") && result->arr[i]->t == JObj)
            {
                JVal* sc = result->arr[i];
                result->arr[i] = new JVal(JNull);
                delete root;
                return sc;
            }
        JVal* parsed = hasText ? JParse(firstText) : nullptr;
        delete root;
        if (parsed && parsed->t == JObj) return parsed;
        delete parsed;
        return JNewObj();
    }
};

static JVal* Args() { return JNewObj(); }

// ---------------------------------------------------------------------
// 同步状态：每张便签上次同步时“本地内容的哈希”和“远端文件的 sha256”（和 WPF 版的 sync.json 相同）
// ---------------------------------------------------------------------
struct KnownNote { Str id, local, remote; };
struct SyncState
{
    Str server, folder;
    Vec<KnownNote> notes;
    Str lastOk;

    static Str FilePath() { return PathJoin(Store::Dir, L"sync.json"); }
    void Load()
    {
        Str content;
        if (!ReadFileText(FilePath().c(), &content)) return;
        JVal* d = JParse(content);
        if (!d) return;
        server = d->Str_(L"server", L"");
        folder = d->Str_(L"folder", L"");
        lastOk = d->Str_(L"lastOk", L"");
        JVal* ns = d->Get(L"notes");
        if (ns && ns->t == JObj)
            for (int i = 0; i < ns->keys.n; i++)
            {
                JVal* o = ns->arr[i];
                if (o->t != JObj) continue;
                KnownNote k;
                k.id = ns->keys[i];
                k.local = o->Str_(L"local", L"");
                k.remote = o->Str_(L"remote", L"");
                notes.Push(k);
            }
        delete d;
    }
    void Save()
    {
        JVal* ns = JNewObj();
        for (int i = 0; i < notes.n; i++)
        {
            JVal* o = JNewObj();
            o->SetStr(L"local", notes[i].local)->SetStr(L"remote", notes[i].remote);
            ns->Set(notes[i].id.c(), o);
        }
        JVal* o = JNewObj();
        o->SetStr(L"server", server)->SetStr(L"folder", folder);
        if (lastOk.n) o->SetStr(L"lastOk", lastOk); else o->SetNull(L"lastOk");
        o->Set(L"notes", ns);
        WriteAtomic(FilePath().c(), JWrite(o));
        delete o;
    }
    KnownNote* Find(const Str& id) { for (int i = 0; i < notes.n; i++) if (notes[i].id == id) return &notes[i]; return nullptr; }
};

// ---------------------------------------------------------------------
// 一轮同步：后台算出计划，回到界面线程执行
// ---------------------------------------------------------------------
struct LocalNote { Str id, content; };
struct Pull { Str id, content; };
struct Plan
{
    // 输入
    Str server, key, folder;
    bool ensureFolder = false;
    Vec<LocalNote> snapshot;
    Vec<KnownNote> known;
    // 输出
    bool failed = false;
    Str error;
    Vec<Pull> pulls, copies;        // copies：冲突时本地那份另存为新便签
    Vec<Str> localDeletes;          // 远端删了：本地移进回收站
    Vec<KnownNote> newState;
    int pushed = 0, pulled = 0, deletedN = 0, conflicts = 0;

    const Str* Local(const Str& id) const { for (int i = 0; i < snapshot.n; i++) if (snapshot[i].id == id) return &snapshot[i].content; return nullptr; }
    const KnownNote* Known(const Str& id) const { for (int i = 0; i < known.n; i++) if (known[i].id == id) return &known[i]; return nullptr; }
    void SetState(const Str& id, const Str& local, const Str& remote)
    {
        for (int i = 0; i < newState.n; i++) if (newState[i].id == id) { newState[i].local = local; newState[i].remote = remote; return; }
        KnownNote k; k.id = id; k.local = local; k.remote = remote; newState.Push(k);
    }
    void DropState(const Str& id) { for (int i = 0; i < newState.n; i++) if (newState[i].id == id) { newState.RemoveAt(i); return; } }
};

struct RemoteFile { Str id, nodeId, sha; };

static Str FilePathOf(const Str& folder, const Str& id) { return (folder.Eq(L"/") ? Str() : folder) + L"/" + id + L".json"; }
static Str Norm(const Str& s) { return s.Replace(L"\r\n", L"\n").Trim(); }

// 写入；返回新 sha，远端在这期间被改了（EDIT_CONFLICT/READ_FIRST）返回空串，其他错误 *fail = true
static Str WriteRemote(Mcp& c, const Str& path, const Str& content, const Str& baseSha, bool* fail)
{
    JVal* a = Args();
    a->SetStr(L"path", path)->SetStr(L"content", content);
    if (baseSha.n) a->SetStr(L"base_sha256", baseSha);
    JVal* r = c.Call(L"write_text_file", a);
    if (!r)
    {
        if (c.errCode.Eq(L"EDIT_CONFLICT") || c.errCode.Eq(L"READ_FIRST")) return Str();
        *fail = true;
        return Str();
    }
    Str sha = r->Str_(L"sha256", L"");
    delete r;
    if (sha.Empty())
    {
        JVal* a2 = Args();
        a2->SetStr(L"path", path);
        JVal* info = c.Call(L"get_file_info", a2);
        if (info) { sha = info->Str_(L"sha256", L""); delete info; }
    }
    return sha;
}

static bool ReadRemote(Mcp& c, const Str& path, Str* content, Str* sha)
{
    *content = Str();
    i64 offset = 0;
    for (int guard = 0; guard < 64; guard++)
    {
        JVal* a = Args();
        a->SetStr(L"path", path);
        if (offset > 0) a->SetNum(L"offset", (double)offset);
        JVal* r = c.Call(L"read_text_file", a);
        if (!r) return false;
        *content += r->Str_(L"text", L"");
        Str s = r->Str_(L"sha256", L"");
        if (s.n) *sha = s;
        bool eof = r->Bool(L"eof", true);
        i64 next = (i64)r->Num(L"next_offset", 0);
        delete r;
        if (eof || next <= offset) break;
        offset = next;
    }
    return true;
}

static void Compute(Plan* p)
{
    Mcp c;
    c.server = p->server;
    c.key = p->key;
#define FAIL() do { p->failed = true; p->error = c.errMsg.n ? c.errMsg : Str(L"同步出错"); return; } while (0)
    if (p->ensureFolder && !p->folder.Eq(L"/"))
    {
        JVal* a = Args();
        a->SetStr(L"path", p->folder);
        JVal* r = c.Call(L"create_directories", a);
        if (!r) FAIL();
        delete r;
    }
    // 远端清单
    Vec<RemoteFile> remote;
    Str cursor;
    do
    {
        JVal* a = Args();
        a->SetStr(L"path", p->folder);
        if (cursor.n) a->SetStr(L"cursor", cursor);
        JVal* r = c.Call(L"list_files", a);
        if (!r) FAIL();
        JVal* items = r->Get(L"items");
        if (items && items->t == JArr)
            for (int i = 0; i < items->Count(); i++)
            {
                JVal* it = items->At(i);
                if (!it->Str_(L"kind", L"").Eq(L"file")) continue;
                Str name = it->Str_(L"name", L"");
                if (!name.EndsWith(L".json")) continue;
                RemoteFile f;
                f.id = name.Sub(0, name.n - 5);
                f.nodeId = it->Str_(L"id", L"");
                f.sha = it->Str_(L"sha256", L"");
                remote.Push(f);
            }
        cursor = r->Str_(L"next_cursor", L"");
        delete r;
    } while (cursor.n);

    Vec<Str> ids;
    for (int i = 0; i < p->snapshot.n; i++) if (!ids.Contains(p->snapshot[i].id)) ids.Push(p->snapshot[i].id);
    for (int i = 0; i < remote.n; i++) if (!ids.Contains(remote[i].id)) ids.Push(remote[i].id);
    for (int i = 0; i < p->known.n; i++) if (!ids.Contains(p->known[i].id)) ids.Push(p->known[i].id);

    for (int k = 0; k < ids.n; k++)
    {
        const Str& id = ids[k];
        const Str* L = p->Local(id);
        const RemoteFile* R = nullptr;
        for (int i = 0; i < remote.n; i++) if (remote[i].id == id) R = &remote[i];
        const KnownNote* S = p->Known(id);
        Str Lh = L ? Sha256Hex(*L) : Str();
        Str path = FilePathOf(p->folder, id);
        if (!L && !R) continue; // 两边都没了，状态丢掉
        bool lc = !S || !L || Lh != S->local;
        bool rc = !S || !R || R->sha != S->remote;
        bool fail = false;
        if (S && !lc && !rc) { p->SetState(id, S->local, S->remote); continue; }
        if (S && lc && !rc)
        {
            if (!L)
            {
                // 本地删了 -> 远端进回收站
                JVal* a = Args();
                JVal* ids_ = JNewArr();
                ids_->Add(JNewStr(R->nodeId));
                a->Set(L"node_ids", ids_);
                JVal* r = c.Call(L"trash_nodes", a);
                if (!r && !c.errCode.Eq(L"NOT_FOUND")) FAIL();
                delete r;
                p->deletedN++;
                continue;
            }
            Str sha = WriteRemote(c, path, *L, S->remote, &fail); // 本地改了 -> 推上去
            if (fail) FAIL();
            if (sha.n) { p->SetState(id, Lh, sha); p->pushed++; }
            else p->SetState(id, S->local, S->remote); // 冲突了，下一轮按“两边都改了”处理
            continue;
        }
        if (S && !lc && rc)
        {
            if (!R) { p->localDeletes.Push(id); p->deletedN++; continue; } // 远端删了 -> 本地进回收站
            Str content, sha;
            if (!ReadRemote(c, path, &content, &sha)) FAIL();
            Pull pl; pl.id = id; pl.content = content;
            p->pulls.Push(pl);
            p->SetState(id, Sha256Hex(content), sha);
            p->pulled++;
            continue;
        }
        // 第一次见到，或两边都改了
        if (L && !R)
        {
            Str sha = WriteRemote(c, path, *L, Str(), &fail);
            if (fail) FAIL();
            if (sha.n) { p->SetState(id, Lh, sha); p->pushed++; }
            continue;
        }
        if (!L && R)
        {
            Str content, sha;
            if (!ReadRemote(c, path, &content, &sha)) FAIL();
            Pull pl; pl.id = id; pl.content = content;
            p->pulls.Push(pl);
            p->SetState(id, Sha256Hex(content), sha);
            p->pulled++;
            continue;
        }
        Str content, sha;
        if (!ReadRemote(c, path, &content, &sha)) FAIL();
        if (Norm(content) != Norm(*L))
        {
            // 两边内容不一样：远端那份留在原便签，本地这份另存为一张新便签，谁的都不丢
            Pull pl; pl.id = id; pl.content = content;
            p->pulls.Push(pl);
            Pull cp; cp.id = id; cp.content = *L;
            p->copies.Push(cp);
            p->conflicts++;
        }
        p->SetState(id, Sha256Hex(content), sha);
    }
#undef FAIL
}

static DWORD WINAPI SyncThread(LPVOID arg)
{
    Plan* p = (Plan*)arg;
    Compute(p);
    PostMessageW(g_app->msgWnd, WM_APP_SYNCDONE, 0, (LPARAM)p);
    return 0;
}

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
    if (!s->Configured()) { s->SetStatus(g_app->settings.syncEnabled ? Str(L"还没填服务器地址或密钥") : Str(L"未启用"), false); return; }
    s->debounceAt = GetTickCount() + 1000;
    s->nextPeriodic = GetTickCount() + 5 * 60 * 1000; // 定时拉一次别的电脑的改动（gopan 每次调用都记审计日志，别太勤）
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
    if (changed) s->debounceAt = GetTickCount() + 4000;
}

void SyncTick(Syncer* s)
{
    if (!s || !s->Configured()) return;
    DWORD now = GetTickCount();
    if (s->debounceAt && (int)(now - s->debounceAt) >= 0) { s->debounceAt = 0; SyncNow(s); }
    else if (s->nextPeriodic && (int)(now - s->nextPeriodic) >= 0) { s->nextPeriodic = now + 5 * 60 * 1000; SyncNow(s); }
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
    HANDLE th = CreateThread(nullptr, 0, SyncThread, p, 0, nullptr);
    if (th) CloseHandle(th);
    else { s->running = false; delete p; s->SetStatus(Str(L"同步失败：无法启动后台线程"), true); }
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
            if (w && snap && w->d->ContentJson() != *snap) { p->DropState(id); continue; } // 同步期间本地又改了：下一轮再处理，不丢本地的
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

const Str& SyncStatus(Syncer* s) { return s->status; }
bool SyncFailed(Syncer* s) { return s->failed; }
bool SyncBusy(Syncer* s) { return s->running || s->debounceAt != 0; }

// 自测用：直接调一个 MCP 工具
JVal* SyncTestCall(const Str& server, const Str& key, const wchar_t* tool, JVal* args)
{
    Mcp c;
    c.server = server;
    c.key = key;
    return c.Call(tool, args);
}
