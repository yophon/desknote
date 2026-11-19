// 不带 C 运行库的最小运行时：内存、动态数组、宽字符串、数字格式化、时间、文件
#pragma once
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;

// ---- 内存 ----
void* MemAlloc(size_t n);
void* MemRealloc(void* p, size_t n);
void MemFree(void* p);

#ifndef __PLACEMENT_NEW_INLINE
#define __PLACEMENT_NEW_INLINE
inline void* __cdecl operator new(size_t, void* p) noexcept { return p; }
inline void __cdecl operator delete(void*, void*) noexcept {}
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

// ---- 时间：本地时间，单位 100ns（按 FILETIME 编码，只用于比较和加减） ----
typedef i64 Time;
const Time TicksPerMinute = 600000000LL;
const Time TicksPerDay = TicksPerMinute * 60 * 24;
Time Now();
Time Today();
Time MakeTime(int y, int mo, int d, int h, int mi, int s);
void SplitTime(Time t, SYSTEMTIME* st);
Str FormatTime(Time t, bool seconds);       // yyyy-MM-dd HH:mm[:ss]
bool ParseTime(const Str& s, Time* out);    // 接受 yyyy-MM-dd HH:mm[:ss]

// ---- 文件 ----
bool ReadFileBytes(const wchar_t* path, Bytes* out);
bool ReadFileText(const wchar_t* path, Str* out); // UTF-8（可带 BOM）
bool WriteFileBytes(const wchar_t* path, const void* d, int len);
bool WriteAtomic(const wchar_t* path, const Str& text); // 写 .tmp 再替换
bool FileExists(const wchar_t* path);
bool DirExists(const wchar_t* path);
void EnsureDir(const wchar_t* path);
bool DeleteTree(const wchar_t* path);
Str PathJoin(const Str& a, const wchar_t* b);
Str PathFileName(const Str& p);
Str PathStem(const Str& p);

// ---- 杂项 ----
u32 RandomU32();
Str HexOf(const u8* d, int n);
void Log(const Str& msg); // 写 <数据目录>\error.log
extern Str g_logDir;
