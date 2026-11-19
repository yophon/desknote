// 最小运行时里跨平台的部分；内存、文件、时钟这些要问系统的在 win/plat_win.cpp、posix/plat_posix.cpp
#include "rt.h"

// ---- 字符串 ----
int WLen(const wchar_t* s) { int n = 0; if (s) while (s[n]) n++; return n; }
bool WEq(const wchar_t* a, const wchar_t* b)
{
    if (!a) a = L"";
    if (!b) b = L"";
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
int WCmp(const wchar_t* a, const wchar_t* b)
{
    if (!a) a = L"";
    if (!b) b = L"";
    while (*a && *a == *b) { a++; b++; }
    return (u32)*a < (u32)*b ? -1 : (u32)*a > (u32)*b ? 1 : 0;
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
Str& Str::CodePoint(u32 cp)
{
    if (sizeof(wchar_t) == 2 && cp >= 0x10000)
    {
        cp -= 0x10000;
        Ch((wchar_t)(0xD800 + (cp >> 10)));
        return Ch((wchar_t)(0xDC00 + (cp & 0x3FF)));
    }
    return Ch((wchar_t)cp);
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
    for (int i = 0; i < len; i++) p[n + i] = ((const char*)d)[i];
    n += len;
    p[n] = 0;
}

// UTF-8 编解码自己写（各平台一致）：坏的字节、落单的代理都换成 U+FFFD，和 Windows 的转换函数一样
Bytes ToUtf8(const Str& s)
{
    Bytes b;
    b.cap = s.n * 3 + 1;
    b.p = (char*)MemAlloc(b.cap);
    for (int i = 0; i < s.n; i++)
    {
        u32 c = (u32)s.p[i];
        if (sizeof(wchar_t) == 2 && c >= 0xD800 && c <= 0xDBFF && i + 1 < s.n && (u32)s.p[i + 1] >= 0xDC00 && (u32)s.p[i + 1] <= 0xDFFF)
        {
            c = 0x10000 + ((c - 0xD800) << 10) + ((u32)s.p[i + 1] - 0xDC00);
            i++;
        }
        else if ((c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) c = 0xFFFD;
        char* o = b.p + b.n;
        if (c < 0x80) { o[0] = (char)c; b.n += 1; }
        else if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); b.n += 2; }
        else if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); b.n += 3; }
        else { o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63)); b.n += 4; }
    }
    b.p[b.n] = 0;
    return b;
}
Str FromUtf8(const char* s0, int len)
{
    const u8* s = (const u8*)s0;
    Str r;
    if (len >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) { s += 3; len -= 3; }
    r.Reserve(len);
    int i = 0;
    while (i < len)
    {
        u32 c = s[i];
        int need = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (need == 0) { r.Ch((wchar_t)c); i++; continue; }
        if (need < 0) { r.Ch((wchar_t)0xFFFD); i++; continue; }
        u32 cp = c & (0x3F >> need);
        int k = 1;
        while (k <= need && i + k < len && (s[i + k] & 0xC0) == 0x80) { cp = (cp << 6) | (s[i + k] & 63); k++; }
        static const u32 minCp[4] = { 0, 0x80, 0x800, 0x10000 };
        if (k <= need || cp < minCp[need] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { r.Ch((wchar_t)0xFFFD); i += k; continue; }
        r.CodePoint(cp);
        i += k;
    }
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

// ---- 时间：自己按公历换算（各平台一致），只有「现在几点」要问系统 ----
// 从 1601-01-01 起的天数（Howard Hinnant 的 days_from_civil，换成 1601 起算）
static i64 DaysFrom1601(i64 y, int m, int d)
{
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400;
    i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468 + 134774; // 719468：0000-03-01 到 1970；134774：1601 到 1970
}
Time MakeTime(int y, int mo, int d, int h, int mi, int s)
{
    i64 days = DaysFrom1601(y, mo, 1) + (d - 1);
    return ((days * 24 + h) * 60 + mi) * TicksPerMinute + (Time)s * 10000000LL;
}
void SplitTime(Time t, DateTime* out)
{
    i64 secs = t / 10000000LL;
    out->ms = (int)((t / 10000) % 1000);
    i64 days = secs / 86400, rem = secs % 86400;
    out->hour = (int)(rem / 3600);
    out->minute = (int)(rem / 60 % 60);
    out->second = (int)(rem % 60);
    out->dayOfWeek = (int)((days + 1) % 7); // 1601-01-01 是周一
    // civil_from_days
    i64 z = days - 134774 + 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    i64 doe = z - era * 146097;
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    i64 mp = (5 * doy + 2) / 153;
    out->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    out->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    out->year = (int)(yoe + era * 400 + (out->month <= 2));
}
Time Now()
{
    DateTime d;
    LocalNow(&d);
    return MakeTime(d.year, d.month, d.day, d.hour, d.minute, d.second) + (Time)d.ms * 10000;
}
Time Today()
{
    DateTime d;
    LocalNow(&d);
    return MakeTime(d.year, d.month, d.day, 0, 0, 0);
}
Str FormatTime(Time t, bool seconds)
{
    DateTime st;
    SplitTime(t, &st);
    Str s;
    s.Int(st.year).Ch(L'-').Pad2(st.month).Ch(L'-').Pad2(st.day).Ch(L' ').Pad2(st.hour).Ch(L':').Pad2(st.minute);
    if (seconds) s.Ch(L':').Pad2(st.second);
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
bool ReadFileText(const wchar_t* path, Str* out)
{
    Bytes b;
    if (!ReadFileBytes(path, &b)) return false;
    *out = FromUtf8(b.p, b.n);
    return true;
}
bool WriteAtomic(const wchar_t* path, const Str& text)
{
    Str tmp = Str(path) + L".tmp";
    Bytes b = ToUtf8(text);
    if (!WriteFileBytes(tmp.c(), b.p, b.n)) return false;
    return ReplaceWithFile(path, tmp.c());
}
void EnsureDir(const wchar_t* path)
{
    if (DirExists(path)) return;
    Str s(path);
#ifdef _WIN32
    int from = 3; // 跳过盘符和它后面的分隔符
#else
    int from = 1; // 跳过开头的 /
#endif
    for (int i = from; i < s.n; i++)
        if (s.p[i] == PATH_SEP)
        {
            s.p[i] = 0;
            MakeDir(s.p);
            s.p[i] = PATH_SEP;
        }
    MakeDir(path);
}
bool DeleteTree(const wchar_t* path)
{
    Vec<DirEntry> list;
    ListDir(path, list);
    for (int i = 0; i < list.n; i++)
    {
        Str full = PathJoin(path, list[i].name.c());
        if (list[i].isDir) DeleteTree(full.c());
        else RemoveFile(full.c());
    }
    return RemoveDir(path);
}
Str PathJoin(const Str& a, const wchar_t* b)
{
    Str r(a);
    if (r.n && r.p[r.n - 1] != PATH_SEP) r.Ch(PATH_SEP);
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
    if (!RandomBytes(&v, sizeof(v))) v = (u32)TickMs() * 2654435761u;
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

// SHA-256（FIPS 180-4）：同步时比对内容用
static u32 Ror(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
void Sha256(const void* data, int len, u8 out[32])
{
    static const u32 K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    u32 h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const u8* p = (const u8*)data;
    u64 bits = (u64)len * 8;
    int total = ((len + 9 + 63) / 64) * 64; // 补位后的长度
    u8 block[64];
    u32 w[64];
    for (int off = 0; off < total; off += 64)
    {
        for (int i = 0; i < 64; i++)
        {
            int k = off + i;
            block[i] = k < len ? p[k] : k == len ? 0x80 : k >= total - 8 ? (u8)(bits >> (8 * (total - 1 - k))) : 0;
        }
        for (int i = 0; i < 16; i++) w[i] = (u32)block[i * 4] << 24 | (u32)block[i * 4 + 1] << 16 | (u32)block[i * 4 + 2] << 8 | block[i * 4 + 3];
        for (int i = 16; i < 64; i++)
        {
            u32 s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            u32 s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++)
        {
            u32 t1 = hh + (Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            u32 t2 = (Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (int i = 0; i < 8; i++) { out[i * 4] = (u8)(h[i] >> 24); out[i * 4 + 1] = (u8)(h[i] >> 16); out[i * 4 + 2] = (u8)(h[i] >> 8); out[i * 4 + 3] = (u8)h[i]; }
}
Str Sha256Hex(const Str& s)
{
    Bytes b = ToUtf8(s);
    u8 out[32];
    Sha256(b.p, b.n, out);
    return HexOf(out, 32);
}

Str g_logDir;
void Log(const Str& msg)
{
    if (g_logDir.Empty()) return;
    Str line = FormatTime(Now(), true) + L" " + msg + L"\r\n";
    Bytes b = ToUtf8(line);
    AppendFileBytes(PathJoin(g_logDir, L"error.log").c(), b.p, b.n);
}
