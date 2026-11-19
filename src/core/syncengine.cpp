// 同步到 gopan：每张便签的内容（不含窗口位置）存成远端文件夹里的 <id>.json，走 gopan 的 MCP 接口
// （list_files / read_text_file / write_text_file 带 base_sha256 防覆盖 / trash_nodes）。网络请求在后台线程。
#include "syncengine.h"
#include "plat.h"

// ---------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------
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

    static bool OnBody(void* ctx, const char* d, int n) { ((Bytes*)ctx)->Append(d, n); return true; }
    bool Post(const Bytes& body, Bytes* resp)
    {
        Str url = server;
        while (url.EndsWith(L"/")) url.Truncate(url.n - 1);
        url += L"/mcp";
        Plat::HttpReq q;
        q.method = L"POST";
        q.url = url;
        q.headers = L"Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\nAuthorization: Bearer " + key + L"\r\n";
        q.body = body.p;
        q.bodyLen = body.n;
        q.ctx = resp;
        q.onData = OnBody;
        Str err;
        int status = Plat::Http(q, &err);
        if (status == Plat::HttpBadUrl) { errCode = L"BAD_URL"; errMsg = L"服务器地址不对"; return false; }
        if (status == Plat::HttpInitFailed) { errCode = L"NETWORK"; errMsg = L"网络初始化失败"; return false; }
        if (status <= 0) { errCode = L"NETWORK"; errMsg = err.n ? L"连不上服务器（" + err + L"）" : Str(L"连不上服务器"); return false; }
        if (status == 401 || status == 403) { errCode = L"UNAUTHORIZED"; errMsg = L"密钥无效或权限不够（HTTP "; errMsg.Int(status).Ch(L'）'); return false; }
        if (status != 200) { errCode = L"HTTP"; errMsg = L"服务器返回 HTTP "; errMsg.Int(status); return false; }
        return true;
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
// base：上次同步时的内容（规范化后），两边都改了时拿它做底稿三方合并
Str SyncState::FilePath() { return PathJoin(Store::Dir, L"sync.json"); }
void SyncState::Load()
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
            k.base = o->Str_(L"base", L"");
            notes.Push(k);
        }
    delete d;
}
void SyncState::Save()
{
    JVal* ns = JNewObj();
    for (int i = 0; i < notes.n; i++)
    {
        JVal* o = JNewObj();
        o->SetStr(L"local", notes[i].local)->SetStr(L"remote", notes[i].remote);
        if (notes[i].base.n) o->SetStr(L"base", notes[i].base);
        ns->Set(notes[i].id.c(), o);
    }
    JVal* o = JNewObj();
    o->SetStr(L"server", server)->SetStr(L"folder", folder);
    if (lastOk.n) o->SetStr(L"lastOk", lastOk); else o->SetNull(L"lastOk");
    o->Set(L"notes", ns);
    WriteAtomic(FilePath().c(), JWrite(o));
    delete o;
}

struct RemoteFile { Str id, nodeId, sha; };

static Str FilePathOf(const Str& folder, const Str& id) { return (folder.Eq(L"/") ? Str() : folder) + L"/" + id + L".json"; }
static Str Norm(const Str& s) { return s.Replace(L"\r\n", L"\n").Trim(); }

// ---------------------------------------------------------------------
// 合并：两边都改了时按条目三方合并（底稿 = 上次同步时的内容），不再整张另存冲突副本
// ---------------------------------------------------------------------
// 规范化：解析成便签再按 DeskNote 的格式写出。网页、App 写回的 json 排版不一样，内容相同不算改动
static Str Canon(const Str& text, const Str& id)
{
    NoteData* n = NoteData::FromContentJson(text, id);
    if (!n) return Norm(text);
    Str s = n->ContentJson();
    delete n;
    return s;
}

static Str JText(const JVal* v) { return v ? JWrite(v) : Str(); }
static bool JSame(const JVal* a, const JVal* b) { return JText(a) == JText(b); }
static JVal* JClone(const JVal* v)
{
    JVal* c = new JVal(v->t);
    c->b = v->b;
    c->num = v->num;
    c->s = v->s;
    c->keys = v->keys;
    for (int i = 0; i < v->arr.n; i++) c->arr.Push(JClone(v->arr[i]));
    return c;
}

// 条目按文字认（条目没有 id）；重复的文字认第一条
static Str ItemKey(const JVal* it) { return it && it->t == JObj ? it->Str_(L"text", L"") : JText(it); }
static int FindItem(const Vec<JVal*>& list, const Str& key)
{
    for (int i = 0; i < list.n; i++) if (ItemKey(list[i]) == key) return i;
    return -1;
}

static JVal* MergeObj(const JVal* b, const JVal* l, const JVal* r);
static JVal* MergeItems(const JVal* b, const JVal* l, const JVal* r);

// 一个值：一边没动取另一边（另一边删了就删）；两边都动了，对象逐个字段合并，items 按条目合并，其余取本地
static JVal* MergeVal(const JVal* b, const JVal* l, const JVal* r, bool items)
{
    if (JSame(l, r)) return l ? JClone(l) : nullptr;
    if (JSame(l, b)) return r ? JClone(r) : nullptr;
    if (JSame(r, b)) return l ? JClone(l) : nullptr;
    if (b && l && r && b->t == JObj && l->t == JObj && r->t == JObj) return MergeObj(b, l, r);
    if (items && l && r && l->t == JArr && r->t == JArr) return MergeItems(b && b->t == JArr ? b : nullptr, l, r);
    return l ? JClone(l) : JClone(r);
}

static JVal* MergeObj(const JVal* b, const JVal* l, const JVal* r)
{
    JVal* out = JNewObj();
    Vec<Str> keys = l->keys; // 本地的字段顺序在前，远端新加的接在后面
    for (int i = 0; i < r->keys.n; i++) if (!keys.Contains(r->keys[i])) keys.Push(r->keys[i]);
    for (int i = 0; i < keys.n; i++)
    {
        JVal* v = MergeVal(b->Get(keys[i].c()), l->Get(keys[i].c()), r->Get(keys[i].c()), keys[i].Eq(L"items"));
        if (v) out->Set(keys[i].c(), v);
    }
    return out;
}

// 条目列表：从本地的出发，把远端相对底稿的改动做上去
static JVal* MergeItems(const JVal* b, const JVal* l, const JVal* r)
{
    Vec<JVal*> out;
    for (int i = 0; i < l->Count(); i++) out.Push(JClone(l->At(i)));
    Vec<JVal*> none;
    const Vec<JVal*>& B = b ? b->arr : none;
    // 远端删掉的（改文字也算删旧的、加新的）：本地没动过这一条就跟着删
    for (int i = 0; i < B.n; i++)
    {
        Str k = ItemKey(B[i]);
        if (FindItem(r->arr, k) >= 0) continue;
        int li = FindItem(out, k);
        if (li >= 0 && JSame(out[li], B[i])) { delete out[li]; out.RemoveAt(li); }
    }
    // 远端改了别的字段（勾选、提醒）：逐字段合并
    for (int i = 0; i < r->Count(); i++)
    {
        const JVal* ri = r->At(i);
        Str k = ItemKey(ri);
        int bi = FindItem(B, k), li = FindItem(out, k);
        if (bi < 0 || li < 0 || JSame(ri, B[bi])) continue;
        JVal* m = MergeVal(B[bi], out[li], ri, false);
        delete out[li];
        out[li] = m ? m : JClone(ri);
    }
    // 远端新加的：插在它在远端的前一条后面
    for (int i = 0; i < r->Count(); i++)
    {
        const JVal* ri = r->At(i);
        Str k = ItemKey(ri);
        if (FindItem(B, k) >= 0 || FindItem(out, k) >= 0) continue;
        int at = 0;
        for (int j = i - 1; j >= 0; j--)
        {
            int p = FindItem(out, ItemKey(r->At(j)));
            if (p >= 0) { at = p + 1; break; }
        }
        out.Insert(at, JClone(ri));
    }
    JVal* arr = JNewArr();
    for (int i = 0; i < out.n; i++) arr->Add(out[i]);
    return arr;
}

// 三方合并一张便签；合并不了（解析失败）返回空串
Str SyncMergeNote(const Str& base, const Str& local, const Str& remote, const Str& id)
{
    JVal* b = JParse(base);
    JVal* l = JParse(local);
    JVal* r = JParse(remote);
    Str out;
    if (b && l && r && b->t == JObj && l->t == JObj && r->t == JObj)
    {
        JVal* m = MergeObj(b, l, r);
        Str text = JWrite(m);
        delete m;
        NoteData* n = NoteData::FromContentJson(text, id);
        if (n) { out = n->ContentJson(); delete n; }
    }
    delete b;
    delete l;
    delete r;
    return out;
}

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

void SyncCompute(Plan* p)
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
        Str Lc = L ? Canon(*L, id) : Str();
        Str Lh = L ? Sha256Hex(Lc) : Str();
        Str path = FilePathOf(p->folder, id);
        if (!L && !R) continue; // 两边都没了，状态丢掉
        bool lc = !S || !L || Lh != S->local;
        bool rc = !S || !R || R->sha != S->remote;
        bool fail = false;
        if (S && !lc && !rc) { p->SetState(id, S->local, S->remote, S->base); continue; }
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
            if (sha.n) { p->SetState(id, Lh, sha, Lc); p->pushed++; }
            else p->SetState(id, S->local, S->remote, S->base); // 冲突了，下一轮按“两边都改了”处理
            continue;
        }
        if (S && !lc && rc)
        {
            if (!R) { p->localDeletes.Push(id); p->deletedN++; continue; } // 远端删了 -> 本地进回收站
            Str content, sha;
            if (!ReadRemote(c, path, &content, &sha)) FAIL();
            Str rc_ = Canon(content, id);
            Pull pl; pl.id = id; pl.content = content; pl.base0 = S->base;
            p->pulls.Push(pl);
            p->SetState(id, Sha256Hex(rc_), sha, rc_);
            p->pulled++;
            continue;
        }
        // 第一次见到，或两边都改了
        if (L && !R)
        {
            Str sha = WriteRemote(c, path, *L, Str(), &fail);
            if (fail) FAIL();
            if (sha.n) { p->SetState(id, Lh, sha, Lc); p->pushed++; }
            continue;
        }
        if (!L && R)
        {
            Str content, sha;
            if (!ReadRemote(c, path, &content, &sha)) FAIL();
            Str rc_ = Canon(content, id);
            Pull pl; pl.id = id; pl.content = content;
            p->pulls.Push(pl);
            p->SetState(id, Sha256Hex(rc_), sha, rc_);
            p->pulled++;
            continue;
        }
        Str content, sha;
        if (!ReadRemote(c, path, &content, &sha)) FAIL();
        Str Rc = Canon(content, id);
        if (Rc == Lc) { p->SetState(id, Lh, sha, Lc); continue; } // 内容其实一样（只是排版不同）
        // 两边都改了：有底稿就按条目三方合并，合并结果写回远端、更新到本地
        Str merged = S && S->base.n ? SyncMergeNote(S->base, Lc, Rc, id) : Str();
        if (merged.n)
        {
            Str nsha = WriteRemote(c, path, merged, sha, &fail);
            if (fail) FAIL();
            if (nsha.Empty()) { p->SetState(id, S->local, S->remote, S->base); continue; } // 远端又被改了：下一轮再合并
            Pull pl; pl.id = id; pl.content = merged; pl.base0 = S->base;
            p->pulls.Push(pl);
            p->SetState(id, Sha256Hex(merged), nsha, merged);
            p->merged++;
            continue;
        }
        // 没有底稿（第一次同步、旧版本的同步状态）：远端那份留在原便签，本地这份另存为一张新便签，谁的都不丢
        Pull pl; pl.id = id; pl.content = content;
        p->pulls.Push(pl);
        Pull cp; cp.id = id; cp.content = *L;
        p->copies.Push(cp);
        p->conflicts++;
        p->SetState(id, Sha256Hex(Rc), sha, Rc);
    }
#undef FAIL
}

// ---------------------------------------------------------------------
// 实时推送：连着 gopan 的 GET /api/events（gopan doc/39 五），网盘里一有改动就同步，不用干等定时拉取。
// 后台线程阻塞着读流，收到一条就调 notify（Windows 给主窗口发消息，主线程稍等一下合并几条再同步）。
// 服务端每 5 分钟断一次，断了就重连（正常连过的 3 秒后，连不上的退避到最长 2 分钟）；
// 老版本的 gopan 不收密钥（401/403/404）：5 分钟后再试，这期间照旧靠定时拉取。
// 配置变了或停用：代数加一，旧线程读完手上这一段（最多等一次心跳，约 25 秒）就自己退出。
// ---------------------------------------------------------------------
static volatile int g_liveGen = 0;
static volatile int g_liveConnected = 0;
static void (*g_liveNotify)() = nullptr;
struct LiveArgs { Str server, key; int gen; Bytes line; };

bool SyncLiveConnected() { return Plat::AtomicGet(&g_liveConnected) != 0; }

static bool LiveCurrent(LiveArgs* a) { return a->gen == Plat::AtomicGet(&g_liveGen); }
static bool LiveOnStatus(void* ctx, int status)
{
    LiveArgs* a = (LiveArgs*)ctx;
    if (status != 200 || !LiveCurrent(a)) return false;
    Plat::AtomicSet(&g_liveConnected, 1);
    g_liveNotify(); // 刚连上同步一次：断开期间的改动可能漏了
    return true;
}
static bool LiveOnData(void* ctx, const char* d, int n)
{
    LiveArgs* a = (LiveArgs*)ctx;
    for (int i = 0; i < n; i++)
    {
        if (d[i] != '\n') { a->line.Append(d + i, 1); continue; }
        // 一行：data: 开头的是一条改动；注释（: ping）和空行不管
        Bytes& l = a->line;
        if (l.n >= 5 && l.p[0] == 'd' && l.p[1] == 'a' && l.p[2] == 't' && l.p[3] == 'a' && l.p[4] == ':') g_liveNotify();
        l.n = 0;
    }
    return LiveCurrent(a);
}

// 连一次，读到断开为止；返回有没有连上过，*status 是 HTTP 状态（没收到响应为 0）
static bool LiveOnce(LiveArgs* a, int* statusOut)
{
    Str url = a->server;
    while (url.EndsWith(L"/")) url.Truncate(url.n - 1);
    url += L"/api/events";
    Plat::HttpReq q;
    q.url = url;
    q.headers = L"Accept: text/event-stream\r\nCache-Control: no-cache\r\nAuthorization: Bearer " + a->key + L"\r\n";
    q.readTimeoutMs = 60000; // 要比服务端的心跳（25 秒）长；超时就当断了，重连
    q.ctx = a;
    q.onStatus = LiveOnStatus;
    q.onData = LiveOnData;
    a->line.n = 0;
    int status = Plat::Http(q, nullptr);
    *statusOut = status > 0 ? status : 0;
    if (LiveCurrent(a)) Plat::AtomicSet(&g_liveConnected, 0); // 换了配置的话新线程自己管这个标志
    return status == 200;
}

static void LiveThread(void* arg)
{
    LiveArgs* a = (LiveArgs*)arg;
    int backoff = 3000;
    while (LiveCurrent(a))
    {
        int status = 0;
        bool was = LiveOnce(a, &status);
        if (!LiveCurrent(a)) break;
        if (was) backoff = 3000;
        else if (status == 401 || status == 403 || status == 404) backoff = 5 * 60 * 1000;
        else backoff = Min(backoff * 2, 120000);
        for (int slept = 0; slept < backoff && LiveCurrent(a); slept += 500) SleepMs(500);
    }
    delete a;
}

void SyncLiveRestart(const Str& server, const Str& key, bool configured, void (*notify)())
{
    int gen = Plat::AtomicInc(&g_liveGen);
    Plat::AtomicSet(&g_liveConnected, 0);
    if (!configured) return;
    g_liveNotify = notify;
    LiveArgs* a = new LiveArgs();
    a->server = server;
    a->key = key;
    a->gen = gen;
    if (!Plat::StartThread(LiveThread, a)) delete a;
}

// 自测用：直接调一个 MCP 工具
JVal* SyncTestCall(const Str& server, const Str& key, const wchar_t* tool, JVal* args)
{
    Mcp c;
    c.server = server;
    c.key = key;
    return c.Call(tool, args);
}
