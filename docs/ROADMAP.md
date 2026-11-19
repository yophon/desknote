# 后续方向（2026-11-19 记录，未排期）

## 已完成（2026-11-19）
- **用真实 gopan 跑通同步**：write_text_file 会返回 sha256；轮询带来的审计日志由 gopan 3.25.0 起只保留 90 天。
- **手机上看/改便签**：gopan 3.25.0 网页和安卓把便签 json 画成清单（gopan doc/39）。
- **实时同步**：gopan 3.25.1 的推送接口收 MCP 密钥；DeskNote 连着推送，有改动就同步，定时拉取放慢到 30 分钟只当兜底。
- **重复提醒**和**快速添加**（DeskNote 2.1.0）。
- **CI**：`.github/workflows/ci.yml`，Windows 编译并跑核心测试、回归测试；Linux、macOS 跑核心测试并检查 `src/ui` 能编译。
- **更新通道**：GitHub Releases（`.github/workflows/release.yml`，打 `v*` 标签发布）；程序每天问一次 releases/latest，有新版提醒一次。
- **提醒推到手机、手机上设提醒**（gopan 3.26.0，gopan doc/39 七）：gopan 服务器每分钟看便签里的提醒，到点走通知渠道推到手机，电脑关着也推，重复的服务器自己顺延；网页和安卓清单里能设置、修改、移除提醒和重复。

## 以后可以做
- **手机上新建便签**：gopan 清单目前能改已有的便签，不能新建一张（新建要定文件名和位置，和桌面端的 id 规则一致）。
- **代码签名**：exe 没签名，从网上下载时 SmartScreen 会拦一下。
- **macOS / Linux 版**：代码已经分好层（`docs/PORTING.md`），有需要时写各平台的外壳。
