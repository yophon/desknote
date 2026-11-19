# 后续方向（2026-11-19 记录，未排期）

## 收尾
1. **用真实 gopan 跑通同步**：确认 write_text_file 返回 sha256（没有时会多调一次 get_file_info）；观察 5 分钟轮询带来的审计日志增长（gopan 源码里没找到审计表清理）。需要先在 gopan「Agent 接入」建密钥。

## 和 gopan 联动
2. **手机上看/改便签**：gopan 网页/安卓认出 DeskNote 文件夹，把 json 渲染成清单，可勾选、添加；写回带 base_sha256，DeskNote 已能处理外部修改和冲突。改动在 gopan。
3. **提醒推到手机**：
   - 简单版：DeskNote 到点时调 ntfy / Bark，几十行；电脑关着时收不到。
   - 彻底版：gopan 服务端加「便签提醒」事件，到点由服务器推送；可与 gopan doc/37 定时任务一起做。
4. **实时同步**：gopan 的 SSE（/api/events）放开给限定目录的 MCP 密钥，替代 5 分钟轮询，同时去掉轮询带来的审计日志。

## 日常使用
5. **重复提醒**（每天 / 每周 / 工作日）和**快速添加**（全局快捷键弹小输入框，直接加到默认便签）。

## 分发与维护
6. **更新通道**：exe + 版本文件放 gopan 分享链接，启动时检查一次，有新版提示下载（仓库私有，朋友看不到 GitHub 发布）。
7. **CI**：GitHub Actions Windows 机器每次推送编译并跑 tests\selftest.ps1。
