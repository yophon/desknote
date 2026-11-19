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
3. **窗口和输入接口**（进行中）：`src/ui/window.h`。
   - 窗口：创建（对话框 / 透明无边框）、位置大小（DIP 和物理像素）、显示隐藏、层级（置底 / 普通 / 置顶）、任务栏、激活和焦点、重绘、交给系统拖动和改大小、屏幕工作区。
   - 输入：一个事件结构体（鼠标、滚轮、键盘、字符、输入法组字、定时器、改大小、缩放变化、主题变化、关闭），按键用平台无关的键码（数值沿用 Windows 的虚拟键码）。
   - 其他：光标形状、剪贴板、打开链接、确认框、选颜色、原生输入框（对话框里的三个输入框继续用系统控件，输入法和读屏才可靠）。
   - 顺序：回收站 → 便签管理 → 设置 → 设置提醒 → 弹出菜单 → 便签窗口（含胶囊、预览）。每改完一个就提交。
   - 已完成：四个对话框（aa91a6c、ce12573）、弹出菜单（0c23d45）。对话框和菜单的窗口、消息、GDI 都在 `win/window_win.cpp`。
   - 剩下：便签窗口（`note.cpp`：创建、窗口模式和贴桌面、拖动改大小、胶囊吸边和滑动、悬停预览、输入法、剪贴板、选颜色、消息处理）。
4. **程序主体和窗口分开**：`App`、`NoteWin` 里的 `HWND` 换成 `Window*`；托盘、通知、全局快捷键、开机自启、文件监视、单实例收进平台接口；`app.h` 不再引 `windows.h`。
5. 把不再依赖 Windows 的界面代码从 `src/win` 搬到 `src/ui`，Linux 上能编译（还不能运行）。

之后才是写 macOS / Linux 外壳。

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
