# DeskNote

Windows 桌面便签：半透明待办清单，可置底在桌面 / 置顶 / 普通窗口，可折叠成贴边胶囊。

- 构建：`build.cmd [输出路径]`，用系统自带的 .NET Framework 4 编译器（C# 5），无需安装 SDK
- 运行要求：Windows 10 1903+ / Windows 11（自带 .NET Framework 4.8）
- 数据：`%APPDATA%\DeskNote`（`notes\<id>.json`、`trash.json`、`settings.json`）
- 测试：`DeskNote.exe --data <目录>` 用独立数据目录运行；`--open trash|settings` 直接打开窗口
- 图标：`powershell -File tools\make-icon.ps1 DeskNote.ico`
