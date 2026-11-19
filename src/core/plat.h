// 平台层：核心代码（同步引擎等）要用、但每个系统写法不一样的功能。
// rt.h 里标着「各平台实现」的文件、时钟函数也算这一层。
// Windows 在 win/plat_win.cpp；macOS、Linux 在 posix/plat_posix.cpp。
#pragma once
#include "rt.h"

namespace Plat
{
    // ---- 后台线程和原子操作 ----
    bool StartThread(void (*fn)(void* arg), void* arg); // 线程自己结束，不用等
    int AtomicInc(volatile int* p);                     // 返回加一之后的值
    void AtomicSet(volatile int* p, int v);
    int AtomicGet(volatile int* p);

    // ---- HTTP（阻塞调用，在后台线程里用）----
    struct HttpReq
    {
        const wchar_t* method = L"GET";
        Str url;
        Str headers;                 // 每行「名字: 值\r\n」
        const void* body = nullptr;
        int bodyLen = 0;
        int timeoutMs = 20000;       // 连接、发送
        int readTimeoutMs = 20000;   // 两次收到数据之间最多等多久
        void* ctx = nullptr;
        // 收到响应头时调用；返回 false 就不读正文了
        bool (*onStatus)(void* ctx, int status) = nullptr;
        // 收到一段正文（来一点给一点，不等缓冲区满）；返回 false 中止
        bool (*onData)(void* ctx, const char* d, int n) = nullptr;
    };
    enum { HttpBadUrl = -1, HttpInitFailed = -2 };
    // 返回 HTTP 状态；0 = 没拿到响应（*err 写上系统给的原因，可能为空）；负数见上面
    int Http(const HttpReq& r, Str* err);

    // ---- 保存在本机的密钥：只有当前用户在这台电脑上能解开 ----
    Str Protect(const Str& plain);   // 返回可以写进设置文件的文本
    Str Unprotect(const Str& saved); // 解不开返回空
}
