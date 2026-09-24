<div align="center">

<img src="resources/icons/FileCommander.svg" width="72" alt="FileCommander" />

# FileCommander · 文件指挥官

面向 Windows 和 Linux 的双栏文件管理器：熟悉的高效操作，连接不同设备。

[下载](https://github.com/liuwenhuan/FileCommander/releases) · [English](#english)

</div>

## 为什么选择 FileCommander

- **高效双栏文件管理。** 延续 Total Commander 用户熟悉的双栏操作习惯：键盘快捷键、独立标签页与历史记录、复制和移动，以及目录比较与同步。
- **跨设备文件发送和剪贴板同步。** 登录账号后，可向另一台设备发送文件：网络可达时设备间直连，否则通过中继传输。还可主动发送文字或图片到其他设备。支持官方服务器和自定义服务器；普通文件管理无需登录。
- **本地、网络与压缩包统一浏览。** 在同一工作区管理本地磁盘、SFTP、FTP、WebDAV、SMB，以及 7z、ZIP、TAR 等压缩包。
- **列表旁即时预览与编辑。** `Ctrl+Q` 在另一栏预览图片、文本、PDF、视频等文件；`Ctrl+E` 打开内嵌编辑，切换文件时不必反复开关窗口。Office 预览需另装 `office-oxide`。
- **Windows 与 Linux 双平台。** 提供可重绑定快捷键、多语言界面，以及浅色、深色和 Green CRT 主题。

## 获取与构建

可用安装包以 [GitHub Releases](https://github.com/liuwenhuan/FileCommander/releases) 的实际附件为准。仓库提供 Windows 安装/便携包及 Linux DEB、AppImage、RPM 的[打包脚本](packaging/)；Windows 下不要单独复制 `FileCommander.exe`，它还需要随包提供的运行库。

Linux 开发环境需安装 Qt 5、libarchive、libssh2、libcurl、Poppler Qt5 等依赖；网络和媒体等功能的开关见 [`cmake/FileCommanderOptions.cmake`](cmake/FileCommanderOptions.cmake)。常规构建与测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows 使用 Qt 5/MSVC 及项目所需 SDK；发布包脚本见 [`packaging/build-windows.ps1`](packaging/build-windows.ps1) 和 [`packaging/build-windows-installer.ps1`](packaging/build-windows-installer.ps1)。Office 预览所需的 `office-oxide` 是独立工具，不随本项目构建。

## 更多信息

- [开发过程、架构取舍与已知限制](docs/development-history.md)
- [自有网络后端与 gvfs 的实测对比](docs/why-not-gvfs.md)
- [账号服务器](server/README.md)

本项目采用 [GPL-3.0-or-later 许可证](LICENSE)。

## English

FileCommander is a dual-pane file manager for Windows and Linux. It pairs a familiar, keyboard-friendly workflow with file transfers and clipboard sharing across devices.

### Why FileCommander

- **Efficient dual-pane file management.** A workflow familiar to Total Commander users, with keyboard shortcuts, independent tabs and history in each pane, copy and move, and directory comparison and synchronization.
- **Cross-device file transfers and clipboard sharing.** Sign in to send files directly between devices when reachable, with relay fallback, or explicitly send text and images through Cloud Clipboard. Use the official or a custom server; local file management needs no account.
- **Local, network, and archive browsing in one workspace.** Work with local drives, SFTP, FTP, WebDAV, SMB, and archives including 7z, ZIP, and TAR.
- **Preview and edit alongside the file list.** Use `Ctrl+Q` for embedded previews of images, text, PDFs, videos, and more, or `Ctrl+E` for embedded editing while navigating files. Office previews require a separate `office-oxide` installation.
- **Windows and Linux support.** Rebind shortcuts, choose from multiple languages, and switch between light, dark, and Green CRT themes.

### Download and build

See [GitHub Releases](https://github.com/liuwenhuan/FileCommander/releases) for available packages. Windows packages must include the bundled runtime libraries; the EXE alone is not a complete distribution. Packaging scripts are in [`packaging/`](packaging/), and build options are in [`cmake/FileCommanderOptions.cmake`](cmake/FileCommanderOptions.cmake). The project uses Qt 5 and C++17; build and test with CMake and CTest as shown above.

Read the [development record](docs/development-history.md) for design decisions and known limitations. FileCommander is licensed under [GPL-3.0-or-later](LICENSE).
