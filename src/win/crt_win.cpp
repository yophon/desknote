// Windows 上不带 C 运行库：替代它的几个符号（memset/memcpy、new/delete、全局对象构造）
#include <string.h> // 只为 memset 等的声明
#include "rt.h"

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


void* __cdecl operator new(size_t n) { return MemAlloc(n); }
void* __cdecl operator new[](size_t n) { return MemAlloc(n); }
void __cdecl operator delete(void* p) noexcept { MemFree(p); }
void __cdecl operator delete(void* p, size_t) noexcept { MemFree(p); }
void __cdecl operator delete[](void* p) noexcept { MemFree(p); }
void __cdecl operator delete[](void* p, size_t) noexcept { MemFree(p); }
