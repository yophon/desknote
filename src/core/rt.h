// 最小运行时：内存、动态数组、宽字符串、数字格式化、时间、文件。
// 这一层和下面的 json、store、syncengine 是跨平台的核心，不引用任何系统头文件；
// 标着「各平台实现」的函数在 win/plat_win.cpp（Windows）和 posix/plat_posix.cpp（macOS、Linux）里。
// 字符串是 wchar_t：Windows 上是 UTF-16，macOS、Linux 上是 UTF-32，转换都走 ToUtf8 / FromUtf8。
#pragma once
#include <stddef.h>

#ifdef _WIN32
#define PATH_SEP L'\\'
#else
#define PATH_SEP L'/'
#ifndef __cdecl
#define __cdecl
#endif
#endif

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;

// ---- 内存（各平台实现） ----
void* MemAlloc(size_t n);
void* MemRealloc(void* p, size_t n);
void MemFree(void* p);

#ifdef _WIN32
#ifndef __PLACEMENT_NEW_INLINE
#define __PLACEMENT_NEW_INLINE
inline void* __cdecl operator new(size_t, void* p) noexcept { return p; }
inline void __cdecl operator delete(void*, void*) noexcept {}
#endif
#else
#include <new>
#endif

template <class T> inline T Min(T a, T b) { return a < b ? a : b; }
template <class T> inline T Max(T a, T b) { return a > b ? a : b; }
template <class T> inline T Clamp(T v, T lo, T hi) { return v < lo ? lo : v > hi ? hi : v; }
template <class T> inline T Abs(T v) { return v < 0 ? -v : v; }
template <class T> inline void Swap(T& a, T& b) { T t = static_cast<T&&>(a); a = static_cast<T&&>(b); b = static_cast<T&&>(t); }
template <class T> inline T&& Move(T& v) { return static_cast<T&&>(v); }

// ---- 动态数组（元素可以有构造/析构） ----
template <class T>
struct Vec
{
    T* p = nullptr;
    int n = 0, cap = 0;

    Vec() {}
    Vec(const Vec& o) { *this = o; }
    Vec(Vec&& o) noexcept { p = o.p; n = o.n; cap = o.cap; o.p = nullptr; o.n = o.cap = 0; }
    ~Vec() { Clear(); MemFree(p); }
    Vec& operator=(const Vec& o)
    {
        if (this == &o) return *this;
        Clear();
        Reserve(o.n);
        for (int i = 0; i < o.n; i++) new (&p[i]) T(o.p[i]);
        n = o.n;
        return *this;
    }
    Vec& operator=(Vec&& o) noexcept
    {
        if (this == &o) return *this;
        Clear(); MemFree(p);
        p = o.p; n = o.n; cap = o.cap; o.p = nullptr; o.n = o.cap = 0;
        return *this;
    }
    void Reserve(int c)
    {
        if (c <= cap) return;
        int nc = Max(c, cap * 2 + 4);
        T* np = (T*)MemAlloc(sizeof(T) * nc);
        for (int i = 0; i < n; i++) { new (&np[i]) T(Move(p[i])); p[i].~T(); }
        MemFree(p);
        p = np; cap = nc;
    }
    T& Push(const T& v) { T tmp(v); Reserve(n + 1); new (&p[n]) T(Move(tmp)); return p[n++]; }
    T& Push(T&& v) { Reserve(n + 1); new (&p[n]) T(Move(v)); return p[n++]; }
    T& PushDefault() { Reserve(n + 1); new (&p[n]) T(); return p[n++]; }
    void Insert(int at, const T& v)
    {
        T tmp(v);
        Reserve(n + 1);
        new (&p[n]) T();
        for (int i = n; i > at; i--) p[i] = Move(p[i - 1]);
        p[at] = Move(tmp);
        n++;
    }
    void RemoveAt(int at)
    {
        for (int i = at; i < n - 1; i++) p[i] = Move(p[i + 1]);
        p[--n].~T();
    }
    void Clear() { for (int i = 0; i < n; i++) p[i].~T(); n = 0; }
    T& operator[](int i) { return p[i]; }
    const T& operator[](int i) const { return p[i]; }
    T& Last() { return p[n - 1]; }
    int IndexOf(const T& v) const { for (int i = 0; i < n; i++) if (p[i] == v) return i; return -1; }
    bool Contains(const T& v) const { return IndexOf(v) >= 0; }
    void Remove(const T& v) { int i = IndexOf(v); if (i >= 0) RemoveAt(i); }
    T* begin() { return p; }
    T* end() { return p + n; }
};

// ---- 宽字符串（UTF-16） ----
struct Str
{
    wchar_t* p = nullptr;
    int n = 0, cap = 0;

    Str() {}
    Str(const wchar_t* s) { Append(s); }
    Str(const wchar_t* s, int len) { Append(s, len); }
    Str(const Str& o) { Append(o.p, o.n); }
    Str(Str&& o) noexcept { p = o.p; n = o.n; cap = o.cap; o.p = nullptr; o.n = o.cap = 0; }
    ~Str() { MemFree(p); }
    Str& operator=(const Str& o) { if (this != &o) { n = 0; Append(o.p, o.n); } return *this; }
    Str& operator=(Str&& o) noexcept { if (this != &o) { MemFree(p); p = o.p; n = o.n; cap = o.cap; o.p = nullptr; o.n = o.cap = 0; } return *this; }
    Str& operator=(const wchar_t* s) { n = 0; Append(s); return *this; }

    const wchar_t* c() const { return p ? p : L""; }
    int Len() const { return n; }
    bool Empty() const { return n == 0; }
    void Reserve(int c);
    Str& Append(const wchar_t* s, int len = -1);
    Str& Append(const Str& s) { return Append(s.p, s.n); }
    Str& Ch(wchar_t c) { return Append(&c, 1); }
    Str& CodePoint(u32 cp); // UTF-16 上超出 BMP 的拆成两个代理
    Str& Int(i64 v);
    Str& Num(double v, int decimals = 4); // 去掉末尾 0
    Str& Pad2(int v) { if (v < 10) Ch(L'0'); return Int(v); }
    Str& operator+=(const wchar_t* s) { return Append(s); }
    Str& operator+=(const Str& s) { return Append(s); }
    Str& operator+=(wchar_t c) { return Ch(c); }
    bool operator==(const Str& o) const;
    bool operator!=(const Str& o) const { return !(*this == o); }
    bool Eq(const wchar_t* s) const;
    bool EqI(const wchar_t* s) const;
    bool StartsWith(const wchar_t* s) const;
    bool EndsWith(const wchar_t* s) const;
    int Find(const wchar_t* s, int from = 0) const;
    int FindCh(wchar_t c, int from = 0) const;
    Str Sub(int from, int len = -1) const;
    Str Trim() const;
    Str Replace(const wchar_t* a, const wchar_t* b) const;
    wchar_t operator[](int i) const { return p[i]; }
    void Truncate(int len) { if (len < n) { n = len; p[n] = 0; } }
};
Str operator+(const Str& a, const wchar_t* b);
Str operator+(const Str& a, const Str& b);
Str operator+(const wchar_t* a, const Str& b);

int WLen(const wchar_t* s);
bool WEq(const wchar_t* a, const wchar_t* b);
int WCmp(const wchar_t* a, const wchar_t* b); // 按编码逐个比较：<0、0、>0

// UTF-8 字节串
struct Bytes
{
    char* p = nullptr;
    int n = 0, cap = 0;
    Bytes() {}
    Bytes(const Bytes&) = delete;
    Bytes(Bytes&& o) noexcept { p = o.p; n = o.n; cap = o.cap; o.p = nullptr; o.n = o.cap = 0; }
    ~Bytes() { MemFree(p); }
    Bytes& operator=(Bytes&& o) noexcept { if (this != &o) { MemFree(p); p = o.p; n = o.n; cap = o.cap; o.p = nullptr; o.n = o.cap = 0; } return *this; }
    void Append(const void* d, int len);
};
Bytes ToUtf8(const Str& s);
Str FromUtf8(const char* s, int len);

// ---- 数字 ----
bool ParseNum(const wchar_t* s, int len, double* out, int* used);
i64 ParseInt(const Str& s, i64 def);

// ---- 时间：本地时间，单位 100ns，从 1601-01-01 起算（和 Windows 的 FILETIME 同一种编码，只用于比较和加减） ----
typedef i64 Time;
const Time TicksPerMinute = 600000000LL;
const Time TicksPerDay = TicksPerMinute * 60 * 24;
struct DateTime { int year, month, day, hour, minute, second, ms, dayOfWeek; }; // dayOfWeek：0 = 周日
void LocalNow(DateTime* out);                // 各平台实现
Time Now();
Time Today();
Time MakeTime(int y, int mo, int d, int h, int mi, int s); // 日子超出当月的往后顺延
void SplitTime(Time t, DateTime* out);
Str FormatTime(Time t, bool seconds);       // yyyy-MM-dd HH:mm[:ss]
bool ParseTime(const Str& s, Time* out);    // 接受 yyyy-MM-dd HH:mm[:ss]
u64 TickMs();                               // 各平台实现：开机以来的毫秒，只用于算间隔
void SleepMs(int ms);                       // 各平台实现

// ---- 文件（路径是本平台的格式；PathJoin 用本平台的分隔符） ----
bool ReadFileBytes(const wchar_t* path, Bytes* out);       // 各平台实现
bool ReadFileText(const wchar_t* path, Str* out);          // UTF-8（可带 BOM）
bool WriteFileBytes(const wchar_t* path, const void* d, int len); // 各平台实现
bool AppendFileBytes(const wchar_t* path, const void* d, int len); // 各平台实现
bool WriteAtomic(const wchar_t* path, const Str& text);    // 写 .tmp 再替换
bool ReplaceWithFile(const wchar_t* path, const wchar_t* tmp); // 各平台实现：用 tmp 原子替换 path
bool FileExists(const wchar_t* path);                      // 各平台实现
bool DirExists(const wchar_t* path);                       // 各平台实现
bool FileMTime(const wchar_t* path, Time* out);            // 各平台实现：最后修改时间（本地时间）
bool MakeDir(const wchar_t* path);                         // 各平台实现：只建一层
void EnsureDir(const wchar_t* path);                       // 逐层建
bool RemoveFile(const wchar_t* path);                      // 各平台实现
bool RemoveDir(const wchar_t* path);                       // 各平台实现：空目录
bool CopyFileTo(const wchar_t* from, const wchar_t* to);   // 各平台实现：覆盖已有的
bool MoveTo(const wchar_t* from, const wchar_t* to);       // 各平台实现：文件或目录改名，目标已存在就失败
struct DirEntry { Str name; bool isDir; Time mtime; };    // mtime：本地时间
bool ListDir(const wchar_t* dir, Vec<DirEntry>& out);      // 各平台实现：不含 . 和 ..
bool DeleteTree(const wchar_t* path);
Str PathJoin(const Str& a, const wchar_t* b);
Str PathFileName(const Str& p);
Str PathStem(const Str& p);

// ---- 杂项 ----
bool RandomBytes(void* out, int n);  // 各平台实现：系统的安全随机数
Str LowerCase(const Str& s);         // 各平台实现：按系统的 Unicode 规则转小写（搜索用）
u32 RandomU32();
Str HexOf(const u8* d, int n);
void Sha256(const void* d, int n, u8 out[32]);
Str Sha256Hex(const Str& s);         // UTF-8 编码后的 SHA-256，小写十六进制
void Log(const Str& msg); // 写 <数据目录>\error.log
extern Str g_logDir;
