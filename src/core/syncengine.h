// 同步引擎（跨平台）：gopan 的 MCP 客户端、同步状态、一轮同步的计算（后台线程里跑）、三方合并、实时推送连接。
// 什么时候同步、算完怎么改界面上的便签由各平台的程序主体决定（Windows 在 win/sync.cpp）。
#pragma once
#include "rt.h"
#include "json.h"
#include "store.h"

Str SyncNormFolder(const Str& f);

// ---------------------------------------------------------------------
// 同步状态：每张便签上次同步时“本地内容的哈希”和“远端文件的 sha256”（和 WPF 版的 sync.json 相同）
// ---------------------------------------------------------------------
// base：上次同步时的内容（规范化后），两边都改了时拿它做底稿三方合并
struct KnownNote { Str id, local, remote, base; };
struct SyncState
{
    Str server, folder;
    Vec<KnownNote> notes;
    Str lastOk;

    static Str FilePath();
    void Load();
    void Save();
    KnownNote* Find(const Str& id) { for (int i = 0; i < notes.n; i++) if (notes[i].id == id) return &notes[i]; return nullptr; }
};

// ---------------------------------------------------------------------
// 一轮同步：后台算出计划，回到界面线程执行
// ---------------------------------------------------------------------
struct LocalNote { Str id, content; };
struct Pull { Str id, content, base0; }; // base0：拉之前的底稿，本地在同步途中又改了时用它下一轮重新合并
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
    int pushed = 0, pulled = 0, deletedN = 0, conflicts = 0, merged = 0;

    const Str* Local(const Str& id) const { for (int i = 0; i < snapshot.n; i++) if (snapshot[i].id == id) return &snapshot[i].content; return nullptr; }
    const KnownNote* Known(const Str& id) const { for (int i = 0; i < known.n; i++) if (known[i].id == id) return &known[i]; return nullptr; }
    void SetState(const Str& id, const Str& local, const Str& remote, const Str& base)
    {
        for (int i = 0; i < newState.n; i++)
            if (newState[i].id == id) { newState[i].local = local; newState[i].remote = remote; newState[i].base = base; return; }
        KnownNote k; k.id = id; k.local = local; k.remote = remote; k.base = base; newState.Push(k);
    }
    void DropState(const Str& id) { for (int i = 0; i < newState.n; i++) if (newState[i].id == id) { newState.RemoveAt(i); return; } }
};


void SyncCompute(Plan* p);  // 后台线程里调：照 p 的输入算出并执行这一轮，结果填回 p
Str SyncMergeNote(const Str& base, const Str& local, const Str& remote, const Str& id); // 三方合并（测试用）

// 实时推送：按新配置重连（configured = false 只停掉旧连接）；收到改动时在后台线程里调 notify
void SyncLiveRestart(const Str& server, const Str& key, bool configured, void (*notify)());
bool SyncLiveConnected();

JVal* SyncTestCall(const Str& server, const Str& key, const wchar_t* tool, JVal* args); // 自测用：直接调一个 MCP 工具
