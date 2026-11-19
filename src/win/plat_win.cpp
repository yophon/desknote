// 平台层的 Windows 实现：内存、文件、时钟、随机数、线程、HTTP（WinHTTP）、密钥保护（DPAPI）
#include "win.h"
#include "plat.h"
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>

// ---- 内存 ----
void* MemAlloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
void* MemRealloc(void* p, size_t n) { return p ? HeapReAlloc(GetProcessHeap(), 0, p, n ? n : 1) : MemAlloc(n); }
void MemFree(void* p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

// ---- 时间 ----
static void ToDateTime(const SYSTEMTIME& st, DateTime* d)
{
    d->year = st.wYear; d->month = st.wMonth; d->day = st.wDay;
    d->hour = st.wHour; d->minute = st.wMinute; d->second = st.wSecond; d->ms = st.wMilliseconds;
    d->dayOfWeek = st.wDayOfWeek;
}
void LocalNow(DateTime* out) { SYSTEMTIME st; GetLocalTime(&st); ToDateTime(st, out); }
u64 TickMs() { return GetTickCount64(); }
void SleepMs(int ms) { Sleep((DWORD)ms); }

void SplitTime(Time t, SYSTEMTIME* st)
{
    DateTime d;
    SplitTime(t, &d);
    st->wYear = (WORD)d.year; st->wMonth = (WORD)d.month; st->wDay = (WORD)d.day; st->wDayOfWeek = (WORD)d.dayOfWeek;
    st->wHour = (WORD)d.hour; st->wMinute = (WORD)d.minute; st->wSecond = (WORD)d.second; st->wMilliseconds = (WORD)d.ms;
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
bool AppendFileBytes(const wchar_t* path, const void* d, int len)
{
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, d, len, &wr, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}
bool ReplaceWithFile(const wchar_t* path, const wchar_t* tmp)
{
    if (FileExists(path) && ReplaceFileW(path, tmp, nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) return true;
    return MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
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
bool MakeDir(const wchar_t* path) { return CreateDirectoryW(path, nullptr) != FALSE; }
bool RemoveFile(const wchar_t* path) { return DeleteFileW(path) != FALSE; }
bool RemoveDir(const wchar_t* path) { return RemoveDirectoryW(path) != FALSE; }
bool CopyFileTo(const wchar_t* from, const wchar_t* to) { return CopyFileW(from, to, FALSE) != FALSE; }
bool MoveTo(const wchar_t* from, const wchar_t* to) { return MoveFileW(from, to) != FALSE; }
bool ListDir(const wchar_t* dir, Vec<DirEntry>& out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(PathJoin(dir, L"*").c(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do
    {
        if (WEq(fd.cFileName, L".") || WEq(fd.cFileName, L"..")) continue;
        DirEntry& e = out.PushDefault();
        e.name = fd.cFileName;
        e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        SYSTEMTIME utc, local;
        DateTime d;
        if (FileTimeToSystemTime(&fd.ftLastWriteTime, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        {
            ToDateTime(local, &d);
            e.mtime = MakeTime(d.year, d.month, d.day, d.hour, d.minute, d.second);
        }
        else e.mtime = 0;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

// ---- 随机数 ----
bool RandomBytes(void* out, int n)
{
    return BCryptGenRandom(nullptr, (PUCHAR)out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

namespace Plat
{
    // ---- 线程 ----
    struct ThreadStart { void (*fn)(void*); void* arg; };
    static DWORD WINAPI ThreadMain(LPVOID p)
    {
        ThreadStart ts = *(ThreadStart*)p;
        delete (ThreadStart*)p;
        ts.fn(ts.arg);
        return 0;
    }
    bool StartThread(void (*fn)(void*), void* arg)
    {
        ThreadStart* ts = new ThreadStart{ fn, arg };
        HANDLE th = CreateThread(nullptr, 0, ThreadMain, ts, 0, nullptr);
        if (!th) { delete ts; return false; }
        CloseHandle(th);
        return true;
    }
    int AtomicInc(volatile int* p) { return (int)InterlockedIncrement((volatile LONG*)p); }
    void AtomicSet(volatile int* p, int v) { InterlockedExchange((volatile LONG*)p, v); }
    int AtomicGet(volatile int* p) { return (int)InterlockedCompareExchange((volatile LONG*)p, 0, 0); }

    // ---- HTTP ----
    int Http(const HttpReq& q, Str* err)
    {
        URL_COMPONENTSW uc = { sizeof(uc) };
        wchar_t host[256], path[1024];
        uc.lpszHostName = host; uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
        if (!WinHttpCrackUrl(q.url.c(), 0, 0, &uc)) return HttpBadUrl;
        HINTERNET s = WinHttpOpen(L"DeskNote/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s) return HttpInitFailed;
        WinHttpSetTimeouts(s, q.timeoutMs, q.timeoutMs, q.timeoutMs, q.readTimeoutMs);
        HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
        HINTERNET r = c ? WinHttpOpenRequest(c, q.method, path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
        int result = 0;
        if (r)
        {
            if (WinHttpSendRequest(r, q.headers.c(), (DWORD)q.headers.n, (LPVOID)q.body, q.bodyLen, q.bodyLen, 0) && WinHttpReceiveResponse(r, nullptr))
            {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
                result = (int)status;
                if (!q.onStatus || q.onStatus(q.ctx, result))
                {
                    const DWORD bufSize = 8192;
                    char* buf = (char*)MemAlloc(bufSize); // 别放栈上：超过 4KB 的局部变量要 C 运行库的栈探测
                    while (true)
                    {
                        // 先等到有数据再只读这么多：直接 ReadData 会等到缓冲区读满才返回（推送一条才几十字节）
                        DWORD avail = 0, rd = 0;
                        if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
                        if (!WinHttpReadData(r, buf, Min(avail, bufSize), &rd) || !rd) break;
                        if (q.onData && !q.onData(q.ctx, buf, (int)rd)) break;
                    }
                    MemFree(buf);
                }
            }
            else if (err) { *err = L"错误 "; err->Int(GetLastError()); }
            WinHttpCloseHandle(r);
        }
        if (c) WinHttpCloseHandle(c);
        WinHttpCloseHandle(s);
        return result;
    }

    // ---- 密钥：DPAPI 加密，只有当前 Windows 用户能解开（和 WPF 版的 ProtectedData 兼容）----
    Str Protect(const Str& plain)
    {
        if (plain.Empty()) return Str();
        Bytes b = ToUtf8(plain);
        DATA_BLOB in = { (DWORD)b.n, (BYTE*)b.p }, out = {};
        if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return Str();
        DWORD len = 0;
        CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
        Str s;
        s.Reserve(len);
        CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s.p, &len);
        s.n = (int)len;
        s.p[s.n] = 0;
        LocalFree(out.pbData);
        return L"dpapi:" + s;
    }
    Str Unprotect(const Str& s)
    {
        if (!s.StartsWith(L"dpapi:")) return Str();
        Str b64 = s.Sub(6);
        DWORD len = 0;
        if (!CryptStringToBinaryW(b64.c(), b64.n, CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr)) return Str();
        BYTE* buf = (BYTE*)MemAlloc(len);
        CryptStringToBinaryW(b64.c(), b64.n, CRYPT_STRING_BASE64, buf, &len, nullptr, nullptr);
        DATA_BLOB in = { len, buf }, out = {};
        Str r;
        if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        {
            r = FromUtf8((const char*)out.pbData, (int)out.cbData);
            LocalFree(out.pbData);
        }
        else Log(L"解不开保存的同步密钥（换了电脑或用户？）");
        MemFree(buf);
        return r;
    }
}
