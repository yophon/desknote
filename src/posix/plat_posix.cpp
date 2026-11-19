// 平台层的 macOS / Linux 实现（POSIX）：内存、文件、时钟、随机数、线程。
// 还没做：HTTP（打算用 libcurl 或系统的网络库）、密钥保护（macOS 钥匙串 / Linux libsecret），见文件末尾。
#include "rt.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif

// ---- 内存 ----
void* MemAlloc(size_t n) { return malloc(n ? n : 1); }
void* MemRealloc(void* p, size_t n) { return realloc(p, n ? n : 1); }
void MemFree(void* p) { free(p); }

// 路径：内部是 wchar_t，交给系统前转成 UTF-8
struct Path8
{
    Bytes b;
    explicit Path8(const wchar_t* p) { b = ToUtf8(Str(p)); }
    const char* c() const { return b.p ? b.p : ""; }
};

// ---- 时间 ----
static void ToDateTime(const struct tm& t, int ms, DateTime* d)
{
    d->year = t.tm_year + 1900; d->month = t.tm_mon + 1; d->day = t.tm_mday;
    d->hour = t.tm_hour; d->minute = t.tm_min; d->second = t.tm_sec; d->ms = ms;
    d->dayOfWeek = t.tm_wday;
}
void LocalNow(DateTime* out)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    localtime_r(&ts.tv_sec, &t);
    ToDateTime(t, (int)(ts.tv_nsec / 1000000), out);
}
u64 TickMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000;
}
void SleepMs(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

// ---- 文件 ----
bool ReadFileBytes(const wchar_t* path, Bytes* out)
{
    int fd = open(Path8(path).c(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size > 64 * 1024 * 1024) { close(fd); return false; }
    int len = (int)st.st_size;
    out->n = 0;
    out->p = (char*)MemRealloc(out->p, len + 1);
    out->cap = len + 1;
    int got = 0;
    while (got < len)
    {
        ssize_t r = read(fd, out->p + got, len - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (int)r;
    }
    close(fd);
    out->n = got;
    out->p[got] = 0;
    return got == len;
}
static bool WriteAll(int fd, const void* d, int len)
{
    const char* p = (const char*)d;
    while (len > 0)
    {
        ssize_t w = write(fd, p, len);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        len -= (int)w;
    }
    return true;
}
bool WriteFileBytes(const wchar_t* path, const void* d, int len)
{
    int fd = open(Path8(path).c(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = WriteAll(fd, d, len);
    fsync(fd);
    close(fd);
    return ok;
}
bool AppendFileBytes(const wchar_t* path, const void* d, int len)
{
    int fd = open(Path8(path).c(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = WriteAll(fd, d, len);
    close(fd);
    return ok;
}
bool ReplaceWithFile(const wchar_t* path, const wchar_t* tmp) { return rename(Path8(tmp).c(), Path8(path).c()) == 0; }
bool FileExists(const wchar_t* path)
{
    struct stat st;
    return stat(Path8(path).c(), &st) == 0 && S_ISREG(st.st_mode);
}
bool DirExists(const wchar_t* path)
{
    struct stat st;
    return stat(Path8(path).c(), &st) == 0 && S_ISDIR(st.st_mode);
}
static Time LocalTimeOf(time_t secs);
bool FileMTime(const wchar_t* path, Time* out)
{
    struct stat st;
    if (stat(Path8(path).c(), &st) != 0) return false;
    *out = LocalTimeOf(st.st_mtime);
    return true;
}
bool MakeDir(const wchar_t* path) { return mkdir(Path8(path).c(), 0755) == 0; }
bool RemoveFile(const wchar_t* path) { return unlink(Path8(path).c()) == 0; }
bool RemoveDir(const wchar_t* path) { return rmdir(Path8(path).c()) == 0; }
bool CopyFileTo(const wchar_t* from, const wchar_t* to)
{
    Bytes b;
    return ReadFileBytes(from, &b) && WriteFileBytes(to, b.p, b.n);
}
bool MoveTo(const wchar_t* from, const wchar_t* to)
{
    // 和 Windows 的 MoveFile 一样：目标已存在就失败（rename 会覆盖文件、空目录）
    struct stat st;
    if (lstat(Path8(to).c(), &st) == 0) return false;
    return rename(Path8(from).c(), Path8(to).c()) == 0;
}
static Time LocalTimeOf(time_t secs)
{
    struct tm t;
    localtime_r(&secs, &t);
    return MakeTime(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}
bool ListDir(const wchar_t* dir, Vec<DirEntry>& out)
{
    Path8 d8(dir);
    DIR* d = opendir(d8.c());
    if (!d) return false;
    while (struct dirent* e = readdir(d))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        DirEntry& de = out.PushDefault();
        de.name = FromUtf8(e->d_name, (int)strlen(e->d_name));
        struct stat st;
        Str full = PathJoin(Str(dir), de.name.c());
        if (stat(Path8(full.c()).c(), &st) == 0)
        {
            de.isDir = S_ISDIR(st.st_mode);
            de.mtime = LocalTimeOf(st.st_mtime);
        }
        else { de.isDir = false; de.mtime = 0; }
    }
    closedir(d);
    return true;
}

Str LowerCase(const Str& s)
{
    Str t(s);
    for (int i = 0; i < t.n; i++) t.p[i] = (wchar_t)towlower((wint_t)t.p[i]);
    return t;
}

// ---- 随机数 ----
bool RandomBytes(void* out, int n)
{
    // getentropy 一次最多 256 字节
    u8* p = (u8*)out;
    while (n > 0)
    {
        int k = n > 256 ? 256 : n;
        if (getentropy(p, k) != 0) return false;
        p += k;
        n -= k;
    }
    return true;
}

namespace Plat
{
    // ---- 线程 ----
    struct ThreadStart { void (*fn)(void*); void* arg; };
    static void* ThreadMain(void* p)
    {
        ThreadStart ts = *(ThreadStart*)p;
        delete (ThreadStart*)p;
        ts.fn(ts.arg);
        return nullptr;
    }
    bool StartThread(void (*fn)(void*), void* arg)
    {
        ThreadStart* ts = new ThreadStart{ fn, arg };
        pthread_t th;
        if (pthread_create(&th, nullptr, ThreadMain, ts) != 0) { delete ts; return false; }
        pthread_detach(th);
        return true;
    }
    int AtomicInc(volatile int* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
    void AtomicSet(volatile int* p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
    int AtomicGet(volatile int* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

    // ---- 还没做 ----
    // HTTP：同步要用。打算 Linux 用 libcurl、macOS 用系统的 NSURLSession（或同样用 libcurl）
    int Http(const HttpReq&, Str* err)
    {
        if (err) *err = L"这个平台还没接上网络";
        return 0;
    }
    // 密钥：macOS 放钥匙串，Linux 放 libsecret（GNOME 钥匙环 / KWallet）。没接上之前不保存密钥
    Str Protect(const Str&) { return Str(); }
    Str Unprotect(const Str&) { return Str(); }
}
