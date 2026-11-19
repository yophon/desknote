#include "store.h"
#include "plat.h"

static Str ColorStr(u32 c)
{
    static const wchar_t* hex = L"0123456789ABCDEF";
    Str s(L"#");
    for (int sh = 20; sh >= 0; sh -= 4) s.Ch(hex[(c >> sh) & 15]);
    return s;
}
static u32 ParseColor(const Str& s, u32 def)
{
    if (s.n != 7 || s.p[0] != L'#') return def;
    u32 v = 0;
    for (int i = 1; i < 7; i++)
    {
        wchar_t c = s.p[i];
        int h = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
        if (h < 0) return def;
        v = v * 16 + h;
    }
    return v;
}

// ---- 条目 ----
JVal* NoteItem::ToJson() const
{
    JVal* o = JNewObj();
    o->SetStr(L"text", text)->SetBool(L"done", done);
    if (hasRemind)
    {
        o->SetStr(L"remind", FormatTime(remind, false))->SetBool(L"reminded", reminded);
        static const wchar_t* names[] = { L"", L"daily", L"weekdays", L"weekly" };
        if (repeat > RepeatNone && repeat <= RepeatWeekly) o->SetStr(L"repeat", Str(names[repeat]));
    }
    return o;
}

Time NextRepeat(Time t, int repeat, Time after)
{
    if (repeat == RepeatNone) return t;
    Time step = repeat == RepeatWeekly ? 7 * TicksPerDay : TicksPerDay;
    // 关机很久后：先整周期跳到 after 附近，免得一次次加
    if (t <= after) t += ((after - t) / step) * step;
    for (int i = 0; i < 16; i++)
    {
        if (t > after)
        {
            if (repeat != RepeatWeekdays) return t;
            DateTime d;
            SplitTime(t, &d);
            if (d.dayOfWeek >= 1 && d.dayOfWeek <= 5) return t;
        }
        t += step;
    }
    return t;
}
bool NoteItem::From(JVal* v, NoteItem* out)
{
    if (!v) return false;
    if (v->t == JStr) { out->text = v->s.Replace(L"\r\n", L"\n"); return true; }
    if (v->t != JObj) return false;
    out->text = v->Str_(L"text", L"").Replace(L"\r\n", L"\n");
    out->done = v->Bool(L"done", false);
    Time t;
    const Str* r = v->GetStr(L"remind");
    if (r && ParseTime(*r, &t))
    {
        out->hasRemind = true;
        out->remind = t;
        out->reminded = v->Bool(L"reminded", false);
        Str rp = v->Str_(L"repeat", L"");
        out->repeat = rp.Eq(L"daily") ? RepeatDaily : rp.Eq(L"weekdays") ? RepeatWeekdays : rp.Eq(L"weekly") ? RepeatWeekly : RepeatNone;
    }
    return true;
}

// ---- 便签 ----
Str NoteData::NewId()
{
    DateTime st;
    LocalNow(&st);
    Str s;
    s.Int(st.year).Pad2(st.month).Pad2(st.day).Ch(L'-').Pad2(st.hour).Pad2(st.minute).Pad2(st.second).Ch(L'-');
    u32 r = RandomU32();
    u8 b[3] = { (u8)(r >> 16), (u8)(r >> 8), (u8)r };
    s += HexOf(b, 3);
    return s;
}
NoteData* NoteData::CloneStyle() const
{
    NoteData* n = new NoteData();
    n->id = NewId();
    n->title = L"新便签";
    n->opacity = opacity; n->fontSize = fontSize; n->fontWeight = fontWeight;
    n->textColor = textColor; n->bgColor = bgColor; n->shadow = shadow; n->checkboxes = checkboxes;
    n->mode = mode; n->width = width; n->height = height;
    return n;
}
static double Round1(double v) { return (double)(i64)(v * 10 + (v >= 0 ? 0.5 : -0.5)) / 10; }
static double Round0(double v) { return (double)(i64)(v + (v >= 0 ? 0.5 : -0.5)); }

JVal* NoteData::ToJson() const
{
    JVal* items_ = JNewArr();
    for (int i = 0; i < items.n; i++) items_->Add(items.p[i].ToJson());
    JVal* style = JNewObj();
    style->SetNum(L"opacity", opacity)->SetNum(L"fontSize", fontSize)->SetNum(L"fontWeight", fontWeight)
        ->SetStr(L"textColor", ColorStr(textColor))->SetStr(L"bgColor", ColorStr(bgColor))
        ->SetBool(L"shadow", shadow)->SetBool(L"checkboxes", checkboxes);
    JVal* win = JNewObj();
    win->SetStr(L"mode", mode == ModeTopmost ? L"topmost" : mode == ModeNormal ? L"normal" : L"desktop")->SetBool(L"collapsed", collapsed);
    if (hidden) win->SetBool(L"hidden", true);
    if (hasPos) win->SetNum(L"x", Round0(x))->SetNum(L"y", Round0(y));
    else win->SetNull(L"x")->SetNull(L"y");
    win->SetNum(L"width", Round1(width))->SetNum(L"height", Round1(height));
    JVal* o = JNewObj();
    o->SetStr(L"title", title)->Set(L"items", items_)->Set(L"style", style)->Set(L"window", win);
    return o;
}
Str NoteData::ContentJson() const
{
    JVal* j = ToJson();
    j->RemoveKey(L"window");
    Str s = JWrite(j);
    delete j;
    return s;
}
void NoteData::CopyContentFrom(const NoteData& s)
{
    title = s.title; items = s.items;
    opacity = s.opacity; fontSize = s.fontSize; fontWeight = s.fontWeight;
    textColor = s.textColor; bgColor = s.bgColor; shadow = s.shadow; checkboxes = s.checkboxes;
}
NoteData* NoteData::From(JVal* d, const Str& id)
{
    if (!d || d->t != JObj) return nullptr;
    NoteData* n = new NoteData();
    n->id = id;
    Str t = d->Str_(L"title", L"").Trim();
    if (t.n) n->title = t;
    JVal* items = d->Get(L"items");
    if (items && items->t == JArr)
        for (int i = 0; i < items->Count(); i++)
        {
            NoteItem it;
            if (NoteItem::From(items->At(i), &it) && it.text.Trim().n) n->items.Push(Move(it));
        }
    JVal* s = d->Get(L"style");
    if (s && s->t == JObj)
    {
        n->opacity = Clamp(s->Num(L"opacity", n->opacity), 0.0, 1.0);
        n->fontSize = Clamp(s->Num(L"fontSize", n->fontSize), 8.0, 60.0);
        n->fontWeight = (int)Clamp(s->Num(L"fontWeight", n->fontWeight), 100.0, 900.0);
        n->textColor = ParseColor(s->Str_(L"textColor", L""), n->textColor);
        n->bgColor = ParseColor(s->Str_(L"bgColor", L""), n->bgColor);
        n->shadow = s->Bool(L"shadow", false);
        n->checkboxes = s->Bool(L"checkboxes", false);
    }
    JVal* w = d->Get(L"window");
    if (w && w->t == JObj)
    {
        Str m = w->Str_(L"mode", L"desktop");
        n->mode = m.Eq(L"topmost") ? ModeTopmost : m.Eq(L"normal") ? ModeNormal : ModeDesktop;
        n->collapsed = w->Bool(L"collapsed", false);
        n->hidden = w->Bool(L"hidden", false);
        JVal* x = w->Get(L"x");
        JVal* y = w->Get(L"y");
        if (x && y && x->t == JNum && y->t == JNum) { n->hasPos = true; n->x = x->num; n->y = y->num; }
        n->width = Max(160.0, w->Num(L"width", n->width));
        n->height = Max(120.0, w->Num(L"height", n->height));
    }
    return n;
}
NoteData* NoteData::FromContentJson(const Str& json, const Str& id)
{
    JVal* j = JParse(json);
    NoteData* n = NoteData::From(j, id);
    delete j;
    return n;
}

// ---- 回收站条目 ----
JVal* TrashEntry::ToJson() const
{
    JVal* o = JNewObj();
    o->SetStr(L"kind", kind)->SetStr(L"deletedAt", FormatTime(deletedAt, true));
    if (kind.Eq(L"note") && note) { o->SetStr(L"noteId", note->id)->Set(L"note", note->ToJson()); }
    else o->SetStr(L"noteId", noteId)->SetStr(L"noteTitle", noteTitle)->SetNum(L"index", index)->Set(L"item", item.ToJson());
    return o;
}
TrashEntry* TrashEntry::From(JVal* d)
{
    if (!d || d->t != JObj) return nullptr;
    TrashEntry* e = new TrashEntry();
    e->kind = d->Str_(L"kind", L"item");
    if (!ParseTime(d->Str_(L"deletedAt", L""), &e->deletedAt)) e->deletedAt = Now();
    if (e->kind.Eq(L"note"))
    {
        e->note = NoteData::From(d->Get(L"note"), d->Str_(L"noteId", NoteData::NewId().c()));
        if (!e->note) { delete e; return nullptr; }
    }
    else
    {
        if (!NoteItem::From(d->Get(L"item"), &e->item)) { delete e; return nullptr; }
        e->noteId = d->Str_(L"noteId", L"");
        e->noteTitle = d->Str_(L"noteTitle", L"");
        e->index = (int)d->Num(L"index", 0);
    }
    return e;
}

// ---- 文件读写 ----
namespace Store
{
    Str Dir, NotesDir, TrashFile, SettingsFile, BackupsDir;

    struct Written { Str id, content; };
    static Vec<Written> lastWritten;  // 自己最后写进每个便签文件的内容，用来分辨外部修改
    static Vec<Str> deleted;          // 已移进回收站的便签：不再写回

    static Str* FindWritten(const Str& id)
    {
        for (int i = 0; i < lastWritten.n; i++) if (lastWritten.p[i].id == id) return &lastWritten.p[i].content;
        return nullptr;
    }
    static void SetWritten(const Str& id, const Str& content)
    {
        Str* w = FindWritten(id);
        if (w) *w = content;
        else { Written x; x.id = id; x.content = content; lastWritten.Push(Move(x)); }
    }
    void ForgetWritten(const Str& id)
    {
        for (int i = 0; i < lastWritten.n; i++) if (lastWritten.p[i].id == id) { lastWritten.RemoveAt(i); return; }
    }

    void Init(const Str& dir)
    {
        Dir = dir;
        NotesDir = PathJoin(dir, L"notes");
        TrashFile = PathJoin(dir, L"trash.json");
        SettingsFile = PathJoin(dir, L"settings.json");
        BackupsDir = PathJoin(dir, L"backups");
        EnsureDir(NotesDir.c());
        g_logDir = dir;
    }

    Str NotePath(const Str& id) { return PathJoin(NotesDir, (id + L".json").c()); }

    void LoadNotes(Vec<NoteData*>& out)
    {
        Vec<DirEntry> files;
        ListDir(NotesDir.c(), files);
        for (int fi = 0; fi < files.n; fi++)
        {
            if (files[fi].isDir) continue;
            const Str& name = files[fi].name;
            if (!name.EndsWith(L".json")) continue;
            Str id = name.Sub(0, name.n - 5);
            Str content;
            if (!ReadFileText(PathJoin(NotesDir, name.c()).c(), &content)) continue;
            JVal* j = JParse(content);
            NoteData* n = NoteData::From(j, id);
            delete j;
            if (!n) { Log(L"读取便签失败 " + name); continue; }
            SetWritten(id, content);
            out.Push(n);
        }
        // 按 id 排序（id 以创建时间开头）
        for (int i = 1; i < out.n; i++)
            for (int k = i; k > 0 && WCmp(out.p[k - 1]->id.c(), out.p[k]->id.c()) > 0; k--)
                Swap(out.p[k - 1], out.p[k]);
    }

    void SaveNote(const NoteData* n)
    {
        if (IsDeleted(n->id)) return;
        JVal* j = n->ToJson();
        Str content = JWrite(j);
        delete j;
        Str* last = FindWritten(n->id);
        if (last && *last == content) return;
        SetWritten(n->id, content);
        if (!WriteAtomic(NotePath(n->id).c(), content)) Log(L"保存便签失败 " + n->id);
    }

    void DeleteNoteFile(const Str& id)
    {
        if (!deleted.Contains(id)) deleted.Push(id);
        RemoveFile(NotePath(id).c());
        ForgetWritten(id);
    }
    void Revive(const Str& id) { deleted.Remove(id); }
    bool IsDeleted(const Str& id) { return deleted.Contains(id); }

    NoteData* ReadIfChangedExternally(const Str& id)
    {
        Str content;
        bool ok = false;
        for (int i = 0; i < 5 && !ok; i++)
        {
            ok = ReadFileText(NotePath(id).c(), &content);
            if (!ok)
            {
                if (!FileExists(NotePath(id).c())) return nullptr;
                SleepMs(80); // 对方可能还没写完
            }
        }
        if (!ok) return nullptr;
        Str* last = FindWritten(id);
        if (last && *last == content) return nullptr;
        JVal* j = JParse(content);
        NoteData* n = NoteData::From(j, id);
        delete j;
        if (!n) { Log(id + L".json 格式有误，暂不重新读入"); return nullptr; }
        SetWritten(id, content);
        return n;
    }

    void LoadTrash(Vec<TrashEntry*>& out)
    {
        Str content;
        if (!ReadFileText(TrashFile.c(), &content)) return;
        JVal* j = JParse(content);
        JVal* entries = j ? j->Get(L"entries") : nullptr;
        if (entries && entries->t == JArr)
            for (int i = 0; i < entries->Count(); i++)
            {
                TrashEntry* e = TrashEntry::From(entries->At(i));
                if (e) out.Push(e);
            }
        else if (!j) Log(L"读取回收站失败");
        delete j;
    }
    void SaveTrash(const Vec<TrashEntry*>& list)
    {
        JVal* arr = JNewArr();
        for (int i = 0; i < list.n; i++) arr->Add(list.p[i]->ToJson());
        JVal* o = JNewObj();
        o->Set(L"entries", arr);
        if (!WriteAtomic(TrashFile.c(), JWrite(o))) Log(L"保存回收站失败");
        delete o;
    }

    // 同步密钥交给平台层加密保存（Windows 是 DPAPI，和 WPF 版兼容）
    Settings LoadSettings()
    {
        Settings s;
        Str content;
        if (!ReadFileText(SettingsFile.c(), &content)) return s;
        JVal* d = JParse(content);
        if (!d) { Log(L"读取设置失败"); return s; }
        s.hotkeyEnabled = d->Bool(L"hotkeyEnabled", s.hotkeyEnabled);
        s.hotkey = d->Str_(L"hotkey", s.hotkey.c());
        s.quickAddEnabled = d->Bool(L"quickAddEnabled", s.quickAddEnabled);
        s.quickAddHotkey = d->Str_(L"quickAddHotkey", s.quickAddHotkey.c());
        s.quickAddNote = d->Str_(L"quickAddNote", L"");
        s.checkUpdates = d->Bool(L"checkUpdates", true);
        s.updateCheckedAt = d->Str_(L"updateCheckedAt", L"");
        s.updateNotified = d->Str_(L"updateNotified", L"");
        s.autoStartInit = d->Bool(L"autoStartInit", false);
        s.syncEnabled = d->Bool(L"syncEnabled", false);
        s.syncServer = d->Str_(L"syncServer", DefaultSyncServer);
        if (s.syncServer.Trim().Empty()) s.syncServer = DefaultSyncServer; // 留空 = 用默认的 gopan
        s.syncFolder = d->Str_(L"syncFolder", s.syncFolder.c());
        s.syncKey = Plat::Unprotect(d->Str_(L"syncKey", L""));
        s.hardwareRender = d->Bool(L"hardwareRender", false);
        delete d;
        return s;
    }
    void SaveSettings(const Settings& s)
    {
        JVal* o = JNewObj();
        o->SetBool(L"hotkeyEnabled", s.hotkeyEnabled)->SetStr(L"hotkey", s.hotkey)
            ->SetBool(L"quickAddEnabled", s.quickAddEnabled)->SetStr(L"quickAddHotkey", s.quickAddHotkey)->SetStr(L"quickAddNote", s.quickAddNote)
            ->SetBool(L"checkUpdates", s.checkUpdates)->SetStr(L"updateCheckedAt", s.updateCheckedAt)->SetStr(L"updateNotified", s.updateNotified)
            ->SetBool(L"autoStartInit", s.autoStartInit)
            ->SetBool(L"syncEnabled", s.syncEnabled)->SetStr(L"syncServer", s.syncServer)->SetStr(L"syncFolder", s.syncFolder)
            ->SetStr(L"syncKey", Plat::Protect(s.syncKey))->SetBool(L"hardwareRender", s.hardwareRender);
        if (!WriteAtomic(SettingsFile.c(), JWrite(o))) Log(L"保存设置失败");
        delete o;
    }

    // backups\yyyy-MM-dd\ 里放一份当天第一次检查时的全部数据，保留最近 KeepBackups 天
    bool DailyBackup()
    {
        DateTime st;
        LocalNow(&st);
        Str day;
        day.Int(st.year).Ch(L'-').Pad2(st.month).Ch(L'-').Pad2(st.day);
        Str target = PathJoin(BackupsDir, day.c());
        if (DirExists(target.c())) return false;
        Str tmp = target + L".partial";
        if (DirExists(tmp.c())) DeleteTree(tmp.c());
        EnsureDir(PathJoin(tmp, L"notes").c());
        Vec<DirEntry> files;
        ListDir(NotesDir.c(), files);
        for (int i = 0; i < files.n; i++)
            if (!files[i].isDir && files[i].name.EndsWith(L".json"))
                CopyFileTo(PathJoin(NotesDir, files[i].name.c()).c(), PathJoin(PathJoin(tmp, L"notes"), files[i].name.c()).c());
        if (FileExists(TrashFile.c())) CopyFileTo(TrashFile.c(), PathJoin(tmp, L"trash.json").c());
        if (FileExists(SettingsFile.c())) CopyFileTo(SettingsFile.c(), PathJoin(tmp, L"settings.json").c());
        if (!MoveTo(tmp.c(), target.c())) { Log(L"每日备份失败"); return false; }

        Vec<Str> dirs;
        Vec<DirEntry> all;
        ListDir(BackupsDir.c(), all);
        for (int i = 0; i < all.n; i++)
        {
            const Str& n = all[i].name;
            if (all[i].isDir && n.n == 10 && n.p[4] == L'-' && n.p[7] == L'-') dirs.Push(n);
        }
        for (int i = 1; i < dirs.n; i++)
            for (int k = i; k > 0 && WCmp(dirs.p[k - 1].c(), dirs.p[k].c()) > 0; k--)
                Swap(dirs.p[k - 1], dirs.p[k]);
        for (int i = 0; i < dirs.n - KeepBackups; i++) DeleteTree(PathJoin(BackupsDir, dirs.p[i].c()).c());
        return true;
    }
}
