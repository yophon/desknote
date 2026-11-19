# 移植到 macOS / Linux：代码整理计划

目标：界面逻辑（便签、对话框、菜单、程序主体）只写一份，放在 `src/ui`；每个平台只写一层薄外壳（窗口、绘图、系统服务）。
整理期间 Windows 版的行为不变：每一步都要回归测试全过、截图逐像素对比，才提交。

## 分层

| 目录 | 内容 | 能引用 |
|---|---|---|
| `src/core` | 数据、存储、同步引擎（无界面） | 只有自己 |
| `src/ui` | 界面层：绘图接口 `gfx.h`、窗口接口 `window.h`，以后还有便签、对话框、菜单、程序主体的逻辑 | core、ui |
| `src/win` | Windows 外壳：`plat_win.cpp`、`gfx_d2d.cpp`、`window_win.cpp`、托盘、通知、快捷键 | 全部 + Windows 头文件 |
| `src/posix` | macOS / Linux 共用的平台层 | core |
| （以后）`src/mac`、`src/gtk` | macOS（AppKit + CoreGraphics/CoreText）、Linux（GTK4 + Cairo/Pango）外壳 | |

## 步骤

1. ~~核心拆出来~~（3d47379）：`src/core` 不引用系统头文件，Linux 上编译测试通过（`tests/coretest.sh`）。
2. ~~绘图接口~~（8770a10）：`src/ui/gfx.h`；界面代码不再直接用 Direct2D / DirectWrite。
3. ~~窗口和输入接口~~：`src/ui/window.h`，Windows 实现在 `src/win/window_win.cpp`。
   - 对话框（aa91a6c、ce12573）、弹出菜单（0c23d45）、便签窗口（含胶囊、悬停预览）都改成了 `WinHandler`；`dialogs.cpp`、`note.cpp` 里不再有 Win32 调用。
   - 平台拿不准的事反过来问界面代码：光标形状、要不要压在最下面、拖动时保持多大、最小多大、要不要自己画输入法组字。
   - 截图和回归测试都和改造前一致；拖动、改大小、输入法、胶囊吸边、置底这些交互要手动试。
4. ~~程序主体和窗口分开~~：`src/ui/shell.h`（托盘、通知、全局快捷键、开机自启、定时器、文件夹监视、后台线程回主线程）；Windows 外壳在 `src/win/app_win.cpp`。日期显示改用核心的 `DateTime`，文件修改时间、转小写进了平台层。
5. ~~界面代码搬到 `src/ui`~~：`app`、`note`、`dialogs`、`menu`、`sync` 不引用任何系统头文件，`tests/coretest.sh` 在 Linux 上检查它们能编译（零警告）。

整理到此完成。接下来写 macOS / Linux 外壳，每个平台要实现：

- `src/core/plat.h` 剩下的 HTTP、密钥保存（`src/posix/plat_posix.cpp` 里还是空的）。
- `src/ui/gfx.h`：macOS 用 CoreGraphics + CoreText，Linux 用 Cairo + Pango。
- `src/ui/window.h`：macOS 用 AppKit（NSWindow / NSView / NSTextInputClient），Linux 用 GTK4。
- `src/ui/shell.h`：菜单栏图标 / 托盘、通知、全局快捷键、开机自启、定时器、文件夹监视。
- 入口和主循环，以及 `Ui::LoadTheme`（读系统深浅色和强调色）。

## 验证方法

- `tests\selftest.ps1`：回归测试（目前 67 项）。
- `tests\coretest.cmd` / `tests/coretest.sh`：核心测试，Windows、Linux 各跑一遍。
- 截图对比：改动前后的程序用同一份测试数据打开各个界面（`--open trash|settings|reminder|edit1|edit2|edit3|capsule|preview|hover|menu|menu1|menu2|traymenu|manage|managemenu`），逐像素比较。第一张便签不能是隐藏或胶囊状态，否则这些入口测不到它。

## 已知要在各平台外壳里处理的事

- 图标：界面里的图标是 Segoe Fluent Icons 的码位，macOS / Linux 要换成自带的图标字体或矢量图。
- 字体：便签正文 Windows 用微软雅黑 UI；macOS 用苹方，Linux 用 Noto Sans CJK。
- 快捷键：macOS 上复制粘贴等用 Cmd，外壳把 Cmd 映射成界面层的 Ctrl。
- Linux 的 Wayland 下程序不能自己定窗口位置、不能置顶置底（GNOME）；KDE、wlroots 系可以用 layer-shell 做桌面挂件。
- HTTP：Linux 用 libcurl，macOS 用 NSURLSession 或 libcurl。密钥：macOS 钥匙串，Linux libsecret。
