# DeskNote

Windows 桌面便签：半透明待办清单，可置底在桌面 / 置顶 / 普通窗口，可折叠成贴边胶囊。单个 exe（约 240KB），不依赖 .NET 和 C 运行库，空闲时私有内存约 3～5MB。

## 功能

- 便签：双击新增或编辑，拖左边距排序，勾选框，撤销/重做（Ctrl+Z / Ctrl+Y），条目里的链接（Ctrl+单击打开），彩色 emoji
- 提醒：条目右侧的提醒标签，到点弹系统通知并变红；程序没开着时错过的提醒下次打开补上
- 外观：右键菜单「外观」里直接调背景/文字颜色、不透明度、字号、粗细、文字阴影，实时生效
- 胶囊：折叠后贴边自动缩进，鼠标移上去滑出并显示预览卡片
- 全局快捷键（默认 Ctrl+Alt+N）把所有便签临时提到最前；托盘菜单
- 回收站（不自动清理）、每日自动备份（保留 14 天）、开机自启（exe 挪位置后打开一次即自动修正）
- 同步到 gopan（MCP 密钥；两边同时改时本地另存「冲突副本」）
- 界面跟随系统深浅色和强调色（Win11 样式）

## 构建与运行

- 构建：`build.cmd [输出路径]`，默认输出到仓库根目录的 `DeskNote.exe`。需要 VS 2022 Build Tools 的 C++ 工具和 Windows SDK
- 运行要求：Windows 10 1903+ / Windows 11
- 数据：`%APPDATA%\DeskNote`（`notes\<id>.json`、`trash.json`、`settings.json`、`sync.json`、`backups\`）

## 代码

C 风格 C++，不带 C 运行库（自己的内存、字符串、JSON），Direct2D + DirectWrite 软件渲染，绘图对象空闲几秒后整体释放。

代码分层，为出 macOS、Linux 版做准备（计划和进度见 `docs/PORTING.md`）：`src/core` 和 `src/ui` 不引用任何系统头文件，Linux 上能编译；系统相关的都在平台目录里。

| 文件 | 内容 |
|---|---|
| `src/core/rt.*` | 最小运行时：内存、动态数组、字符串（wchar_t，Windows 上 UTF-16、其他系统 UTF-32）、UTF-8、时间换算、文件、SHA-256 |
| `src/core/plat.h` | 平台层接口：线程、原子操作、HTTP、密钥保护（rt.h 里标着「各平台实现」的文件、时钟函数也算） |
| `src/core/json.*` | JSON 解析和写出 |
| `src/core/store.*` | 便签、回收站、设置的读写，每日备份 |
| `src/core/syncengine.*` | 同步引擎：gopan MCP 客户端、同步状态、一轮同步的计算、三方合并、实时推送连接 |
| `src/core/links.*` | 条目里的链接识别 |
| `src/ui/gfx.h` | 绘图接口：图形、文字排版、光标和选区的命中测试 |
| `src/ui/window.h` | 窗口和输入接口：对话框、弹出窗口、便签窗口、事件、输入法、剪贴板、原生输入框、屏幕 |
| `src/ui/shell.h` | 系统外壳接口：托盘、通知、全局快捷键、开机自启、定时器、文件夹监视 |
| `src/ui/app.*` | 程序主体：便签增删、回收站、提醒、同步联动、外部修改、提到最前、托盘菜单 |
| `src/ui/note.cpp` | 便签窗口：绘制、编辑、排序、撤销、胶囊、右键菜单 |
| `src/ui/menu.*` | 主题和仿 Win11 的弹出菜单 |
| `src/ui/dialogs.cpp` | 设置、回收站、设置提醒、便签管理 |
| `src/ui/sync.cpp` | 同步调度：什么时候同步、同步完怎么改便签 |
| `src/win/` | Windows 外壳：`app_win.cpp`（入口、托盘、通知、快捷键、开机自启、主循环）、`window_win.cpp`（窗口和输入）、`gfx_d2d.cpp`（Direct2D + DirectWrite）、`ui_win.cpp`（画布、系统主题）、`plat_win.cpp`（WinHTTP、DPAPI 等）、`crt_win.cpp`（替代 C 运行库）、`selftest.cpp` |
| `src/posix/plat_posix.cpp` | 平台层的 macOS / Linux 实现（还没接 HTTP 和密钥保存） |

## 测试与调试

- 回归测试：`powershell -NoProfile -ExecutionPolicy Bypass -File tests\selftest.ps1`（编译到临时目录，用临时数据跑两轮自测；有 python 时连 `tests\mock_gopan.py` 测同步）
- 核心测试（只测 `src/core`，各平台同一套）：Windows `tests\coretest.cmd`；Linux / macOS `sh tests/coretest.sh`（要 C++17 编译器；还会检查 `src/ui` 能不能编译）
- 独立数据目录：`DeskNote.exe --data <目录>`（不影响正在用的便签，也不改开机自启）
- 内存诊断：`DeskNote.exe --data <测试目录> --memdiag <结果文件>`
- 直接打开界面：`--open trash|settings|reminder|edit|capsule|preview|hover|menu|menu1|menu2|traymenu`；`--theme light|dark` 强制深浅色
- 图标：`powershell -File tools\make-icon.ps1 DeskNote.ico`

## 历史

1.x 是 WPF（.NET Framework 4.8）版本，2.0 起换成现在的原生版，数据格式不变。旧版源码在标签 `wpf-1.2.1`。
