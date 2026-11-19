// 数据：便签（每张一个 json）、回收站、全局设置、每日备份。格式和 WPF 版相同，两边可以互相读写
#pragma once
#include "rt.h"
#include "json.h"

enum WinMode { ModeDesktop = 0, ModeTopmost = 1, ModeNormal = 2 };

const u32 DefaultTextColor = 0xA9B1B7; // 0xRRGGBB
const u32 DefaultBgColor = 0x34424D;

struct NoteItem
{
    Str text;
    bool done = false;
    bool hasRemind = false;
    Time remind = 0;
    bool reminded = false;
    JVal* ToJson() const;
    static bool From(JVal* v, NoteItem* out);
};

struct NoteData
{
    Str id;
    Str title = L"ToDo";
    Vec<NoteItem> items;
    double opacity = 0.85, fontSize = 14.5;
    int fontWeight = 400;
    u32 textColor = DefaultTextColor, bgColor = DefaultBgColor;
    bool shadow = false, checkboxes = false;
    WinMode mode = ModeDesktop;
    bool collapsed = false;
    bool hasPos = false;
    double x = 0, y = 0;              // 物理像素
    double width = 340, height = 420; // DIP

    static Str NewId();
    NoteData* CloneStyle() const;
    JVal* ToJson() const;
    Str ContentJson() const;          // 不含窗口位置：撤销、同步用
    void CopyContentFrom(const NoteData& s);
    static NoteData* From(JVal* v, const Str& id);
    static NoteData* FromContentJson(const Str& json, const Str& id);
};

struct TrashEntry
{
    Str kind = L"item";   // item | note
    Time deletedAt = 0;
    Str noteId, noteTitle;
    int index = 0;
    NoteItem item;
    NoteData* note = nullptr; // kind == note 时拥有
    ~TrashEntry() { delete note; }
    JVal* ToJson() const;
    static TrashEntry* From(JVal* v);
};

// 同步默认连的 gopan（和 app.h 里的 URL_GOPAN_WEB 是同一个地址）
static const wchar_t* const DefaultSyncServer = L"https://drive.979687.xyz";

struct Settings
{
    bool hotkeyEnabled = true;
    Str hotkey = L"Ctrl+Alt+N";
    bool autoStartInit = false;
    bool syncEnabled = false;
    Str syncServer = DefaultSyncServer, syncFolder = L"/DeskNote", syncKey;
    bool hardwareRender = false; // WPF 版的设置，原样保留
};

namespace Store
{
    extern Str Dir, NotesDir, TrashFile, SettingsFile, BackupsDir;
    const int KeepBackups = 14;

    void Init(const Str& dir);
    Str NotePath(const Str& id);
    void LoadNotes(Vec<NoteData*>& out);
    void SaveNote(const NoteData* n);
    void DeleteNoteFile(const Str& id);
    void Revive(const Str& id);
    bool IsDeleted(const Str& id);
    NoteData* ReadIfChangedExternally(const Str& id); // 不是自己写的改动才返回
    void ForgetWritten(const Str& id);

    void LoadTrash(Vec<TrashEntry*>& out);
    void SaveTrash(const Vec<TrashEntry*>& list);
    Settings LoadSettings();
    void SaveSettings(const Settings& s);
    bool DailyBackup();
}
