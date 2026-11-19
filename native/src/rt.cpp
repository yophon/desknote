// 最小运行时的实现 + 替代 C 运行库的几个符号（memset/memcpy、new/delete、全局对象构造）
#include "rt.h"
#include <bcrypt.h>

extern "C" int _fltused = 0;

#pragma function(memset)
extern "C" void* __cdecl memset(void* d, int c, size_t n)
{
    u8* p = (u8*)d;
    while (n--) *p++ = (u8)c;
    return d;
}
#pragma function(memcpy)
extern "C" void* __cdecl memcpy(void* d, const void* s, size_t n)
{
    u8* a = (u8*)d;
    const u8* b = (const u8*)s;
    while (n--) *a++ = *b++;
    return d;
}
#pragma function(memmove)
extern "C" void* __cdecl memmove(void* d, const void* s, size_t n)
{
    u8* a = (u8*)d;
    const u8* b = (const u8*)s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
#pragma function(memcmp)
extern "C" int __cdecl memcmp(const void* x, const void* y, size_t n)
{
    const u8* a = (const u8*)x;
    const u8* b = (const u8*)y;
    for (; n; n--, a++, b++) if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}
extern "C" int __cdecl _purecall() { return 0; }

// ---- 全局对象的构造函数：编译器把它们的指针放进 .CRT$XC* 段，启动时自己挨个调用 ----
typedef void(__cdecl* PVFV)();
#pragma section(".CRT$XCA", long, read)
#pragma section(".CRT$XCZ", long, read)
__declspec(allocate(".CRT$XCA")) PVFV __xc_a[] = { nullptr };
__declspec(allocate(".CRT$XCZ")) PVFV __xc_z[] = { nullptr };
#pragma comment(linker, "/merge:.CRT=.rdata")
static PVFV g_atexit[64];
static int g_atexitN;
extern "C" int __cdecl atexit(PVFV f) { if (g_atexitN < 64) g_atexit[g_atexitN++] = f; return 0; }
void RunGlobalCtors() { for (PVFV* p = __xc_a; p < __xc_z; ++p) if (*p) (*p)(); }
void RunGlobalDtors() { while (g_atexitN > 0) g_atexit[--g_atexitN](); }

// ---- 内存 ----
static HANDLE Heap() { static HANDLE h = GetProcessHeap(); return h; }
void* MemAlloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
void* MemRealloc(void* p, size_t n) { return p ? HeapReAlloc(GetProcessHeap(), 0, p, n ? n : 1) : MemAlloc(n); }
void MemFree(void* p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
void* __cdecl operator new(size_t n) { return MemAlloc(n); }
void* __cdecl operator new[](size_t n) { return MemAlloc(n); }
void __cdecl operator delete(void* p) noexcept { MemFree(p); }
void __cdecl operator delete(void* p, size_t) noexcept { MemFree(p); }
void __cdecl operator delete[](void* p) noexcept { MemFree(p); }
void __cdecl operator delete[](void* p, size_t) noexcept { MemFree(p); }

// ---- 字符串 ----
int WLen(const wchar_t* s) { int n = 0; if (s) while (s[n]) n++; return n; }
bool WEq(const wchar_t* a, const wchar_t* b)
{
    if (!a) a = L"";
    if (!b) b = L"";
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void Str::Reserve(int c)
{
    if (c + 1 <= cap) return;
    int nc = Max(c + 1, cap * 2 + 16);
    p = (wchar_t*)MemRealloc(p, nc * sizeof(wchar_t));
    cap = nc;
}
Str& Str::Append(const wchar_t* s, int len)
{
    if (len < 0) len = WLen(s);
    Reserve(n + len);
    for (int i = 0; i < len; i++) p[n + i] = s[i];
    n += len;
    p[n] = 0;
    return *this;
}
Str& Str::Int(i64 v)
{
    wchar_t buf[24];
    int i = 23;
    buf[i] = 0;
    bool neg = v < 0;
    u64 u = neg ? (u64)(-v) : (u64)v;
    do { buf[--i] = (wchar_t)(L'0' + u % 10); u /= 10; } while (u);
    if (neg) buf[--i] = L'-';
    return Append(buf + i);
}
Str& Str::Num(double v, int decimals)
{
    if (v != v) return Append(L"0");
    if (v < 0) { Ch(L'-'); v = -v; }
    double scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    u64 total = (u64)(v * scale + 0.5);
    u64 ip = total / (u64)scale, fp = total % (u64)scale;
    Int((i64)ip);
    if (fp)
    {
        wchar_t buf[24];
        for (int i = decimals - 1; i >= 0; i--) { buf[i] = (wchar_t)(L'0' + fp % 10); fp /= 10; }
        int len = decimals;
        while (len > 0 && buf[len - 1] == L'0') len--;
        Ch(L'.');
        Append(buf, len);
    }
    return *this;
}
bool Str::operator==(const Str& o) const
{
    if (n != o.n) return false;
    for (int i = 0; i < n; i++) if (p[i] != o.p[i]) return false;
    return true;
}
bool Str::Eq(const wchar_t* s) const { return WEq(c(), s); }
static wchar_t Lower(wchar_t c) { return c >= L'A' && c <= L'Z' ? (wchar_t)(c + 32) : c; }
bool Str::EqI(const wchar_t* s) const
{
    int m = WLen(s);
    if (m != n) return false;
    for (int i = 0; i < n; i++) if (Lower(p[i]) != Lower(s[i])) return false;
    return true;
}
bool Str::StartsWith(const wchar_t* s) const
{
    int m = WLen(s);
    if (m > n) return false;
    for (int i = 0; i < m; i++) if (p[i] != s[i]) return false;
    return true;
}
bool Str::EndsWith(const wchar_t* s) const
{
    int m = WLen(s);
    if (m > n) return false;
    for (int i = 0; i < m; i++) if (p[n - m + i] != s[i]) return false;
    return true;
}
int Str::Find(const wchar_t* s, int from) const
{
    int m = WLen(s);
    for (int i = Max(0, from); i + m <= n; i++)
    {
        int k = 0;
        while (k < m && p[i + k] == s[k]) k++;
        if (k == m) return i;
    }
    return -1;
}
int Str::FindCh(wchar_t ch, int from) const { for (int i = Max(0, from); i < n; i++) if (p[i] == ch) return i; return -1; }
Str Str::Sub(int from, int len) const
{
    from = Clamp(from, 0, n);
    if (len < 0 || from + len > n) len = n - from;
    return Str(p + from, len);
}
Str Str::Trim() const
{
    int a = 0, b = n;
    while (a < b && (p[a] == L' ' || p[a] == L'\t' || p[a] == L'\r' || p[a] == L'\n')) a++;
    while (b > a && (p[b - 1] == L' ' || p[b - 1] == L'\t' || p[b - 1] == L'\r' || p[b - 1] == L'\n')) b--;
    return Sub(a, b - a);
}
Str Str::Replace(const wchar_t* a, const wchar_t* b) const
{
    Str r;
    int m = WLen(a);
    if (!m) return *this;
    int i = 0;
    while (i < n)
    {
        int k = Find(a, i);
        if (k < 0) { r.Append(p + i, n - i); break; }
        r.Append(p + i, k - i);
        r.Append(b);
        i = k + m;
    }
    return r;
}
Str operator+(const Str& a, const wchar_t* b) { Str r(a); r.Append(b); return r; }
Str operator+(const Str& a, const Str& b) { Str r(a); r.Append(b); return r; }
Str operator+(const wchar_t* a, const Str& b) { Str r(a); r.Append(b); return r; }

void Bytes::Append(const void* d, int len)
{
    if (n + len + 1 > cap)
    {
        cap = Max(n + len + 1, cap * 2 + 64);
        p = (char*)MemRealloc(p, cap);
    }
    memcpy(p + n, d, len);
    n += len;
    p[n] = 0;
}

Bytes ToUtf8(const Str& s)
{
    Bytes b;
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c(), s.n, nullptr, 0, nullptr, nullptr);
    b.cap = len + 1;
    b.p = (char*)MemAlloc(b.cap);
    WideCharToMultiByte(CP_UTF8, 0, s.c(), s.n, b.p, len, nullptr, nullptr);
    b.n = len;
    b.p[len] = 0;
    return b;
}
Str FromUtf8(const char* s, int len)
{
    Str r;
    if (len >= 3 && (u8)s[0] == 0xEF && (u8)s[1] == 0xBB && (u8)s[2] == 0xBF) { s += 3; len -= 3; }
    int wl = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    r.Reserve(wl);
    MultiByteToWideChar(CP_UTF8, 0, s, len, r.p, wl);
    r.n = wl;
    r.p[wl] = 0;
    return r;
}

// ---- 数字 ----
bool ParseNum(const wchar_t* s, int len, double* out, int* used)
{
    int i = 0;
    bool neg = false;
    if (i < len && (s[i] == L'-' || s[i] == L'+')) { neg = s[i] == L'-'; i++; }
    int start = i;
    double v = 0;
    while (i < len && s[i] >= L'0' && s[i] <= L'9') { v = v * 10 + (s[i] - L'0'); i++; }
    if (i < len && s[i] == L'.')
    {
        i++;
        double f = 0.1;
        while (i < len && s[i] >= L'0' && s[i] <= L'9') { v += (s[i] - L'0') * f; f *= 0.1; i++; }
    }
    if (i == start) return false;
    if (i < len && (s[i] == L'e' || s[i] == L'E'))
    {
        i++;
        bool eneg = false;
        if (i < len && (s[i] == L'-' || s[i] == L'+')) { eneg = s[i] == L'-'; i++; }
        int e = 0;
        while (i < len && s[i] >= L'0' && s[i] <= L'9') { e = e * 10 + (s[i] - L'0'); i++; }
        while (e-- > 0) v = eneg ? v / 10 : v * 10;
    }
    *out = neg ? -v : v;
    if (used) *used = i;
    return true;
}
i64 ParseInt(const Str& s, i64 def)
{
    double v;
    Str t = s.Trim();
    int used;
    if (!ParseNum(t.c(), t.n, &v, &used) || used != t.n) return def;
    return (i64)v;
}

// ---- 时间 ----
static Time FromSystemTime(const SYSTEMTIME& st)
{
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    return (Time)(((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
}
Time Now() { SYSTEMTIME st; GetLocalTime(&st); return FromSystemTime(st); }
Time Today() { SYSTEMTIME st; GetLocalTime(&st); st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0; return FromSystemTime(st); }
Time MakeTime(int y, int mo, int d, int h, int mi, int s)
{
    SYSTEMTIME st = {};
    st.wYear = (WORD)y; st.wMonth = (WORD)mo; st.wDay = (WORD)d;
    st.wHour = (WORD)h; st.wMinute = (WORD)mi; st.wSecond = (WORD)s;
    return FromSystemTime(st);
}
void SplitTime(Time t, SYSTEMTIME* st)
{
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(t & 0xFFFFFFFF);
    ft.dwHighDateTime = (DWORD)((u64)t >> 32);
    FileTimeToSystemTime(&ft, st);
}
Str FormatTime(Time t, bool seconds)
{
    SYSTEMTIME st;
    SplitTime(t, &st);
    Str s;
    s.Int(st.wYear).Ch(L'-').Pad2(st.wMonth).Ch(L'-').Pad2(st.wDay).Ch(L' ').Pad2(st.wHour).Ch(L':').Pad2(st.wMinute);
    if (seconds) s.Ch(L':').Pad2(st.wSecond);
    return s;
}
bool ParseTime(const Str& s, Time* out)
{
    // yyyy-MM-dd HH:mm[:ss]（也接受 / 和 T）
    int v[6] = { 0, 0, 0, 0, 0, 0 }, k = 0, i = 0;
    while (i < s.n && k < 6)
    {
        if (s.p[i] < L'0' || s.p[i] > L'9') { i++; continue; }
        int x = 0;
        while (i < s.n && s.p[i] >= L'0' && s.p[i] <= L'9') { x = x * 10 + (s.p[i] - L'0'); i++; }
        v[k++] = x;
    }
    if (k < 5 || v[0] < 1900 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31 || v[3] > 23 || v[4] > 59) return false;
    *out = MakeTime(v[0], v[1], v[2], v[3], v[4], k > 5 ? v[5] : 0);
    return true;
}

// ---- 文件 ----
bool ReadFileBytes(const wchar_t* path, Bytes* out)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    out->n = 0;
    if (sz.QuadPart > 64 * 1024 * 1024) { CloseHandle(h); return false; }
    int len = (int)sz.QuadPart;
    out->p = (char*)MemRealloc(out->p, len + 1);
    out->cap = len + 1;
    DWORD rd = 0;
    BOOL ok = ReadFile(h, out->p, len, &rd, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out->n = (int)rd;
    out->p[out->n] = 0;
    return true;
}
bool ReadFileText(const wchar_t* path, Str* out)
{
    Bytes b;
    if (!ReadFileBytes(path, &b)) return false;
    *out = FromUtf8(b.p, b.n);
    return true;
}
bool WriteFileBytes(const wchar_t* path, const void* d, int len)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, d, len, &wr, nullptr) && (int)wr == len;
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok != FALSE;
}
bool WriteAtomic(const wchar_t* path, const Str& text)
{
    Str tmp = Str(path) + L".tmp";
    Bytes b = ToUtf8(text);
    if (!WriteFileBytes(tmp.c(), b.p, b.n)) return false;
    if (FileExists(path))
    {
        if (ReplaceFileW(path, tmp.c(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) return true;
    }
    return MoveFileExW(tmp.c(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}
bool FileExists(const wchar_t* path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool DirExists(const wchar_t* path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
void EnsureDir(const wchar_t* path)
{
    if (DirExists(path)) return;
    Str s(path);
    for (int i = 3; i < s.n; i++)
        if (s.p[i] == L'\\')
        {
            s.p[i] = 0;
            CreateDirectoryW(s.p, nullptr);
            s.p[i] = L'\\';
        }
    CreateDirectoryW(path, nullptr);
}
bool DeleteTree(const wchar_t* path)
{
    Str pat = PathJoin(path, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (WEq(fd.cFileName, L".") || WEq(fd.cFileName, L"..")) continue;
            Str full = PathJoin(path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(full.c());
            else DeleteFileW(full.c());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(path) != FALSE;
}
Str PathJoin(const Str& a, const wchar_t* b)
{
    Str r(a);
    if (r.n && r.p[r.n - 1] != L'\\') r.Ch(L'\\');
    r.Append(b);
    return r;
}
Str PathFileName(const Str& p)
{
    int i = p.n - 1;
    while (i >= 0 && p.p[i] != L'\\' && p.p[i] != L'/') i--;
    return p.Sub(i + 1);
}
Str PathStem(const Str& p)
{
    Str f = PathFileName(p);
    int dot = -1;
    for (int i = 0; i < f.n; i++) if (f.p[i] == L'.') dot = i;
    return dot > 0 ? f.Sub(0, dot) : f;
}

// ---- 杂项 ----
u32 RandomU32()
{
    u32 v = 0;
    if (BCryptGenRandom(nullptr, (PUCHAR)&v, sizeof(v), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) v = GetTickCount() * 2654435761u;
    return v;
}
Str HexOf(const u8* d, int n)
{
    static const wchar_t* hex = L"0123456789abcdef";
    Str s;
    s.Reserve(n * 2);
    for (int i = 0; i < n; i++) { s.Ch(hex[d[i] >> 4]); s.Ch(hex[d[i] & 15]); }
    return s;
}
Str g_logDir;
void Log(const Str& msg)
{
    if (g_logDir.Empty()) return;
    Str path = PathJoin(g_logDir, L"error.log");
    Str line = FormatTime(Now(), true) + L" " + msg + L"\r\n";
    Bytes b = ToUtf8(line);
    HANDLE h = CreateFileW(path.c(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    WriteFile(h, b.p, b.n, &wr, nullptr);
    CloseHandle(h);
}
