// 核心（src/core）的测试：不碰任何界面，Windows、macOS、Linux 上跑同一套。
// 用法：coretest <临时目录>    输出每项 PASS/FAIL，最后一行 ALL PASSED 或 FAILED n
// Windows：tests\coretest.cmd    Linux / macOS：tests/coretest.sh
#include "rt.h"
#include "json.h"
#include "store.h"
#include "links.h"
#include "syncengine.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include "win.h"
#endif

static int g_fail = 0, g_pass = 0;
static void Check(const char* name, bool ok, const Str& detail = Str())
{
    if (ok) g_pass++;
    else g_fail++;
    Bytes d = ToUtf8(detail);
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.n ? " :: " : "", d.p ? d.p : "");
}

// 用 \u 写的字，免得源文件编码和 wchar_t 宽度影响测试本身
static Str Emoji() { Str s; s.CodePoint(0x1F600); return s; } // 😀
static Str Zh() { Str s; s.CodePoint(0x4F60).CodePoint(0x597D); return s; } // 你好

static void TestStrings()
{
    Str s = Str(L"a") + Zh() + Emoji();
    Bytes b = ToUtf8(s);
    const unsigned char want[] = { 'a', 0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD, 0xF0, 0x9F, 0x98, 0x80 };
    bool same = b.n == (int)sizeof(want);
    for (int i = 0; same && i < b.n; i++) same = (unsigned char)b.p[i] == want[i];
    Check("UTF-8 编码（中文、emoji）", same);
    Check("UTF-8 解码回来一样", FromUtf8(b.p, b.n) == s);
    const char bom[] = "\xEF\xBB\xBFok";
    Check("去掉 BOM", FromUtf8(bom, 5).Eq(L"ok"));
    const char bad[] = "a\xFF" "b\xE4\xBD";
    Str dec = FromUtf8(bad, 5);
    Check("坏字节换成 U+FFFD", dec.n == 4 && dec[0] == L'a' && (u32)dec[1] == 0xFFFD && dec[2] == L'b' && (u32)dec[3] == 0xFFFD);
    Check("emoji 占的单位数", Emoji().n == (sizeof(wchar_t) == 2 ? 2 : 1));
    Str n;
    n.Num(0.85).Ch(L' ').Num(14.5).Ch(L' ').Int(-42).Ch(L' ').Num(3.0);
    Check("数字格式", n.Eq(L"0.85 14.5 -42 3"), n);
    Check("WCmp", WCmp(L"20240101-1", L"20240102-0") < 0 && WCmp(L"b", L"a") > 0 && WCmp(L"x", L"x") == 0);
    Check("Replace / Trim", Str(L"  a\r\nb  ").Trim().Replace(L"\r\n", L"\n").Eq(L"a\nb"));
}

static void TestJson()
{
    JVal* v = JParse(L"{\"t\":\"\\ud83d\\ude00\\u4f60\",\"n\":[1,2.5,-3e2],\"b\":true,\"z\":null}");
    Check("JSON 解析", v && v->t == JObj);
    if (!v) return;
    Check("\\u 代理对拼成 emoji", v->Str_(L"t", L"") == Emoji() + Str(L"\x4F60"));
    JVal* arr = v->Get(L"n");
    Check("数组和数字", arr && arr->Count() == 3 && arr->At(1)->num == 2.5 && arr->At(2)->num == -300);
    Str w = JWrite(v);
    JVal* back = JParse(w);
    Check("写出再读回一样", back && JWrite(back) == w);
    delete back;
    delete v;
    JVal* o = JNewObj();
    o->SetStr(L"a", Str(L"x\"y"))->SetNum(L"b", 1);
    Check("写出格式（短对象一行、CRLF）", JWrite(o).Eq(L"{ \"a\": \"x\\\"y\", \"b\": 1 }\r\n"), JWrite(o));
    delete o;
    Check("坏 JSON 返回空", JParse(L"{\"a\":}") == nullptr && JParse(L"[1,2") == nullptr);
}

static void TestTime()
{
    Check("1970-01-01 的编码和 FILETIME 一样", MakeTime(1970, 1, 1, 0, 0, 0) == 116444736000000000LL);
    DateTime d;
    SplitTime(MakeTime(2026, 11, 19, 9, 30, 15), &d);
    Check("拆开（2026-11-19 周四）", d.year == 2026 && d.month == 11 && d.day == 19 && d.hour == 9 && d.minute == 30 && d.second == 15 && d.dayOfWeek == 4);
    SplitTime(MakeTime(2024, 2, 29, 23, 59, 0) + TicksPerMinute, &d);
    Check("闰日跨到 3 月 1 日", d.month == 3 && d.day == 1 && d.hour == 0 && d.minute == 0);
    Check("2 月 30 日顺延到 3 月 2 日", MakeTime(2026, 2, 30, 0, 0, 0) == MakeTime(2026, 3, 2, 0, 0, 0));
    bool ok = true;
    for (int y = 1900; y <= 2200 && ok; y += 7)
        for (int m = 1; m <= 12 && ok; m++)
        {
            Time t = MakeTime(y, m, 28, 13, 5, 0);
            SplitTime(t, &d);
            ok = d.year == y && d.month == m && d.day == 28 && d.hour == 13 && d.minute == 5;
        }
    Check("1900–2200 来回换算", ok);
    Time t;
    Check("解析和格式化", ParseTime(Str(L"2026-11-19 08:05"), &t) && FormatTime(t, false).Eq(L"2026-11-19 08:05") && FormatTime(t, true).Eq(L"2026-11-19 08:05:00"));
    Check("不合法的时间", !ParseTime(Str(L"2026-13-01 00:00"), &t) && !ParseTime(Str(L"hello"), &t));
    Time now = Now(), today = Today();
    Check("现在在今天之内", now >= today && now < today + TicksPerDay);
#ifdef _WIN32
    // 和系统的换算逐天对一遍
    ok = true;
    for (int i = 0; i < 3000 && ok; i++)
    {
        Time x = MakeTime(2000, 1, 1, 0, 0, 0) + (Time)i * 9 * TicksPerDay + (Time)i * 7 * TicksPerMinute;
        SYSTEMTIME st;
        FILETIME ft = { (DWORD)(x & 0xFFFFFFFF), (DWORD)((u64)x >> 32) };
        FileTimeToSystemTime(&ft, &st);
        SYSTEMTIME mine;
        SplitTime(x, &mine);
        ok = st.wYear == mine.wYear && st.wMonth == mine.wMonth && st.wDay == mine.wDay && st.wDayOfWeek == mine.wDayOfWeek
             && st.wHour == mine.wHour && st.wMinute == mine.wMinute;
    }
    Check("和 Windows 的 FileTimeToSystemTime 一致", ok);
#endif
}

static void TestSha()
{
    Check("SHA-256 空串", Sha256Hex(Str()).Eq(L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    Check("SHA-256 abc", Sha256Hex(Str(L"abc")).Eq(L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    Check("SHA-256 56 字节（补位跨块）", Sha256Hex(Str(L"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")).Eq(L"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    Str big;
    for (int i = 0; i < 1000; i++) big.Ch(L'a');
    Check("SHA-256 1000 个 a", Sha256Hex(big).Eq(L"41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
}

static void TestFiles(const Str& root)
{
    Str dir = PathJoin(root, L"dn-coretest");
    if (DirExists(dir.c())) DeleteTree(dir.c());
    Str deep = PathJoin(PathJoin(PathJoin(dir, L"a"), L"b"), L"c");
    EnsureDir(deep.c());
    Check("逐层建目录", DirExists(deep.c()));
    Str f = PathJoin(deep, (Zh() + L".txt").c());
    Check("写原子文件（中文文件名）", WriteAtomic(f.c(), Zh() + Emoji()) && FileExists(f.c()));
    Check("覆盖写", WriteAtomic(f.c(), Str(L"second")));
    Str back;
    Check("读回来", ReadFileText(f.c(), &back) && back.Eq(L"second"));
    Vec<DirEntry> list;
    ListDir(deep.c(), list);
    Check("列目录（有修改时间）", list.n == 1 && list[0].name == Zh() + L".txt" && !list[0].isDir && list[0].mtime > Today() - TicksPerDay);
    Str g = PathJoin(deep, L"copy.txt");
    Check("复制", CopyFileTo(f.c(), g.c()) && ReadFileText(g.c(), &back) && back.Eq(L"second"));
    Check("改名到已存在的目标会失败", !MoveTo(f.c(), g.c()));
    Check("删整个目录", DeleteTree(dir.c()) && !DirExists(dir.c()));
}

static void TestStore(const Str& root)
{
    Str dir = PathJoin(root, L"dn-coretest-store");
    if (DirExists(dir.c())) DeleteTree(dir.c());
    Store::Init(dir);
    NoteData* n = new NoteData();
    n->id = NoteData::NewId();
    n->title = Str(L"测试") + Emoji();
    NoteItem a; a.text = L"第一条"; n->items.Push(a);
    NoteItem b; b.text = L"带提醒"; b.hasRemind = true; b.remind = MakeTime(2026, 12, 1, 9, 0, 0); n->items.Push(b);
    n->mode = ModeTopmost;
    n->hidden = true;
    Store::SaveNote(n);
    Vec<NoteData*> loaded;
    Store::LoadNotes(loaded);
    bool ok = loaded.n == 1 && loaded[0]->id == n->id && loaded[0]->title == n->title && loaded[0]->items.n == 2
              && loaded[0]->items[1].hasRemind && loaded[0]->items[1].remind == b.remind && loaded[0]->mode == ModeTopmost && loaded[0]->hidden;
    Check("便签存了再读", ok);
    Check("内容 JSON 不含窗口段", n->ContentJson().Find(L"\"window\"") < 0 && n->ContentJson().Find(L"带提醒") >= 0);
    Check("自己写的不算外部改动", Store::ReadIfChangedExternally(n->id) == nullptr);
    Settings s;
    s.hotkey = L"Ctrl+Alt+K";
    s.syncFolder = L"/便签";
    Store::SaveSettings(s);
    Settings s2 = Store::LoadSettings();
    Check("设置存了再读", s2.hotkey.Eq(L"Ctrl+Alt+K") && s2.syncFolder == s.syncFolder && s2.syncServer.Eq(DefaultSyncServer));
    Check("每日备份", Store::DailyBackup() && !Store::DailyBackup());
    Store::DeleteNoteFile(n->id);
    Check("删便签文件", !FileExists(Store::NotePath(n->id).c()));
    for (int i = 0; i < loaded.n; i++) delete loaded[i];
    delete n;
    DeleteTree(dir.c());
}

static void TestLinks()
{
    Vec<LinkSpan> l;
    FindLinks(Str(L"看 https://drive.979687.xyz/a?b=1。还有 www.example.com/x）"), l);
    Check("找链接", l.n == 2 && l[0].url.Eq(L"https://drive.979687.xyz/a?b=1") && l[1].url.Eq(L"http://www.example.com/x"));
}

static Str NoteJson(const wchar_t* title, const wchar_t* const* texts, const bool* done, int count)
{
    NoteData n;
    n.id = L"t";
    n.title = title;
    for (int i = 0; i < count; i++) { NoteItem it; it.text = texts[i]; it.done = done[i]; n.items.Push(it); }
    return n.ContentJson();
}

static void TestMerge()
{
    const wchar_t* ab[] = { L"A", L"B" };
    const wchar_t* abc[] = { L"A", L"B", L"C" };
    const bool no2[] = { false, false }, no3[] = { false, false, false }, aDone[] = { true, false };
    Str base = NoteJson(L"T", ab, no2, 2);
    Str local = NoteJson(L"T", abc, no3, 3);   // 本地加了 C
    Str remote = NoteJson(L"T2", ab, aDone, 2); // 远端勾了 A、改了标题
    Str m = SyncMergeNote(base, local, remote, Str(L"t"));
    NoteData* r = NoteData::FromContentJson(m, Str(L"t"));
    bool ok = r && r->title.Eq(L"T2") && r->items.n == 3 && r->items[0].text.Eq(L"A") && r->items[0].done && r->items[2].text.Eq(L"C");
    Check("三方合并：两边的改动都留下", ok, m);
    delete r;
    Check("规范化目录", SyncNormFolder(Str(L" DeskNote\\sub/ ")).Eq(L"/DeskNote/sub") && SyncNormFolder(Str()).Eq(L"/DeskNote"));
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("用法：coretest <临时目录>\n"); return 2; }
    Str root = FromUtf8(argv[1], (int)strlen(argv[1]));
    TestStrings();
    TestJson();
    TestTime();
    TestSha();
    TestFiles(root);
    TestStore(root);
    TestLinks();
    TestMerge();
    if (g_fail) printf("FAILED %d\n", g_fail);
    else printf("ALL PASSED %d\n", g_pass);
    return g_fail ? 1 : 0;
}
