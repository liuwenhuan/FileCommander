# 开发过程记录

> 截至 2026-09-24。本文由原 `CLAUDE.md` 整理，记录已经发生的设计取舍、验证结果和遗留问题；具体行为以当前代码和测试为准。

## 迭代脉络

- **2026-07：双栏基础。** 从本地文件浏览起步，陆续加入标签页、编辑与预览、压缩包、搜索、会话恢复、快捷键、目录比较和同步。文件操作逐步补上取消、字节级进度、吞吐量与剩余时间。
- **2026-07 至 08：网络与性能。** SFTP、FTP、WebDAV、SMB 接入统一的 `FileProvider`；缩略图改为磁盘缓存和可见区域优先的渐进抓取。对比 gvfs 后，保留自有网络后端，同时承认 gvfs 适合向外部程序提供单个文件的本地挂载路径。
- **2026-08：跨平台与发布。** 完成 Windows 构建和平台功能拆分；Linux 保留 libsmbclient 辅助进程，Windows 使用本地/UNC 路径处理 SMB。逐步补齐 Windows 便携包、安装包、运行库部署及双平台 CI。
- **2026-08 至 09：协作与稳定性。** 增加账号、设备间传输和云剪贴板；完善目录变更监控、文件操作错误反馈、预览/编辑切换和各主题下的窗口布局。2026-09 的测试整理重点处理异步 UI、平台差异和翻译资源一致性。

## 关键取舍

### 文件系统边界

本地、网络和压缩包后端共享 [`FileProvider`](../src/core/filesystem/FileProvider.h) 接口，模型及文件操作通过提供者访问条目。实现过程中形成了几条重要约束：

- `RenameResult::Unsupported` 表示可以回退；`Failed` 表示尝试过但失败，不能当作“不支持”。
- `moveTo()` 是同后端移动的快速路径；失败时必须保留源文件，调用方才能安全回退。跨后端传输使用流式读写，提交结果可能要到 `closeHandleStatus()` 才能确认。
- `maxReadChannels()` 由后端报告实际并发能力；`setModifiedTime()` 是尽力而为，不应让已完成的传输倒退为失败。

面板路径不一定是本地路径。网络标签页、压缩包条目和“计算机”视图的虚拟条目不能直接交给 `QFile`、`QDir` 或按本地路径工作的子进程。早期依靠 `displayName()` 是否为空判断本地性会漏掉压缩包；后来改动集中在后端能力和虚拟视图的明确边界。离开压缩包/计算机视图时，还要保留暂存的网络连接，并把真实路径而非 `computer://` 写入会话状态。

### 网络后端与 gvfs

2026-07 的实测中，同一 SMB 目录的 1408 个条目通过自有后端列出耗时 **88–142 ms**，经 gvfs FUSE 挂载耗时 **1347–1416 ms**。高延迟 SFTP 传输也出现约 **5.2–5.7 倍**差距。瓶颈主要在 FUSE 桥接和往返次数，不等于 gvfs 协议后端本身慢。完整环境、数据和适用边界见[测量记录](why-not-gvfs.md)。

因此，列表和批量操作继续走自有提供者；外部应用需要真实文件路径时，gvfs 挂载仍有价值。Linux SMB 使用独立的 `FileCommander-smb-helper` 进程池，隔离 libsmbclient 的并发限制和崩溃风险；Windows 不依赖这套 Linux 专属辅助进程。

### 预览与可选组件

缩略图路径由缓存、可见区域优先的扫描、远程抓取和视频字节范围规划组成。远程视频只取所需范围和关键帧，避免为了一个缩略图下载整段文件。图片、文本、PDF 和视频预览分别使用适合平台的组件；Windows 视频采用 Media Foundation，PDF 仍采用 Poppler。

Office 预览通过独立的 `office-oxide` 命令行程序完成，未随本仓库一起提供；缺少它不影响其他功能。仓库根目录的 [`wps兼容方案.md`](../wps兼容方案.md) 是对 WPS 私有格式的验证和计划，不能据此认为该兼容方案已经交付。

## 工程与验证

项目按 `src/core`、`src/widgets`、`src/viewer`、`src/archive`、`src/search`、`src/ui` 分层，主程序负责组装；平台差异集中在 `src/platform` 和构建选项。测试拆为 core、ui、archive、viewer 等套件，UI 测试使用真实 `QApplication`。构建、翻译和打包入口分别见 [`CMakeLists.txt`](../CMakeLists.txt)、[`resources/translations`](../resources/translations/README.md) 与 [`packaging`](../packaging/)；发布版本号由 CMake 的 `project()` 统一定义。

产品已改名为 FileCommander，但部分内部名称仍为 `ttc`/`TTC_*`，包括构建选项和翻译文件前缀。这是历史兼容约定，不代表还有两个产品。Windows 构建需要将 Qt、Poppler 等运行库与可执行文件一起部署；曾出现单独运行 EXE 时缺少 DLL 的问题，因此发布包验证与源码编译成功是两件事。

## 相关记录

- [目录变更监控方案](superpowers/specs/2026-09-04-directory-change-monitoring.md)
- [云剪贴板改造方案](superpowers/specs/2026-08-23-cloud-clipboard-redesign.md)
- [更新服务器说明](UPDATE_SERVER.md)
