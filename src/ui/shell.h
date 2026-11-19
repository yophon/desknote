// 系统外壳（跨平台接口）：托盘、通知、全局快捷键、开机自启、程序级定时器、文件夹监视、从后台线程回到主线程。
// 系统那边发生的事交给 App 的 OnTrayClick / OnTrayMenu / OnNoticeClicked / OnTimer / OnFolderChanged / OnHotkey。
// Windows 实现在 win/app_win.cpp。
#pragma once
#include "rt.h"

namespace Shell
{
    void TrayShow();
    void TrayHide();
    void Notify(const Str& title, const Str& text);  // 系统通知；用户点了交给 App::OnNoticeClicked
    bool HotkeyRegister(int id, int mods, int key);  // Mod* + Key*；按下交给 App::OnHotkey(id)
    void HotkeyUnregister(int id);
    bool AutoStartGet();                             // 开机自启（只管当前用户）
    void AutoStartSet(bool on);
    void AutoStartFix();                             // exe 挪了位置：已开自启的话指到现在这个 exe
    void SetTimer(int id, int ms);                   // 程序级定时器：到点交给 App::OnTimer
    void KillTimer(int id);
    void WatchFolder(const Str& dir);                // 文件夹里的文件有增删改：交给 App::OnFolderChanged
    void StopWatching();
    void PostSyncDone(void* plan);                   // 后台线程里调：回到主线程交给 SyncOnDone
    void PostRemoteChange();                         // 后台线程里调：回到主线程交给 App::OnRemoteChange
    void Post(void (*fn)(void* ctx), void* ctx);     // 后台线程里调：回到主线程调 fn(ctx)
    void* MainHandle();                              // 托盘菜单的主人（平台句柄）
    void Quit();                                     // 退出主循环
}
