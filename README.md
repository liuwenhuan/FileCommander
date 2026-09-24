<div align="center">

<img src="resources/icons/FileCommander.svg" width="72" alt="FileCommander" />

# FileCommander · 文件指挥官

基于 Qt 5 / C++17 的 Windows、Linux 双栏文件管理器。

[下载发布版本](https://github.com/liuwenhuan/FileCommander/releases) · [开发过程记录](docs/development-history.md) · [English](#english)

</div>

## 主要功能

- **双栏操作：** 每栏有独立标签页和历史记录；支持键盘驱动的复制、移动、重命名、删除，以及目录比较和同步。
- **多种位置：** 浏览本地磁盘、压缩包（7z、ZIP、TAR、SquashFS、UDF）和 SFTP、FTP、WebDAV、SMB 网络位置。
- **预览与编辑：** 图片、文本、PDF、视频预览，缩略图缓存；`Ctrl+Q` 快速预览、`Ctrl+E` 内嵌编辑。Office 文档预览需要额外安装 `office-oxide`。
- **查找与自定义：** 文件名/内容搜索、可重绑定的功能键、浅色/深色/Green CRT 主题及多语言界面。
- **可选账号功能：** 设备间文件传输与云剪贴板；可使用官方或自定义服务器。

## 获取与构建

安装包和便携包以 [GitHub Releases](https://github.com/liuwenhuan/FileCommander/releases) 中的实际附件为准。仓库提供 Windows 安装/便携包及 Linux DEB、AppImage、RPM 的[打包脚本](packaging/)；单独复制 `FileCommander.exe` 不等于完整的 Windows 运行包。

Linux 开发环境需安装 Qt 5、libarchive、libssh2、libcurl、Poppler Qt5 等依赖；网络和媒体等功能的开关见 [`cmake/FileCommanderOptions.cmake`](cmake/FileCommanderOptions.cmake)。常规构建与测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows 使用 Qt 5/MSVC 及项目所需 SDK；发布包脚本见 [`packaging/build-windows.ps1`](packaging/build-windows.ps1) 和 [`packaging/build-windows-installer.ps1`](packaging/build-windows-installer.ps1)。Office 预览所需的 `office-oxide` 是独立工具，不随本项目构建。

## 进一步了解

- [开发过程、架构取舍与已知限制](docs/development-history.md)
- [自有网络后端与 gvfs 的实测对比](docs/why-not-gvfs.md)
- [账号服务器](server/README.md)

本项目采用 [GPL-3.0-or-later 许可证](LICENSE)。

## English

FileCommander is a Qt 5/C++17 dual-pane file manager for Windows and Linux. It supports local and network files, archives, previews, directory comparison and synchronization, customizable shortcuts, themes, and multiple languages. Office previews require the separate `office-oxide` tool. See [Releases](https://github.com/liuwenhuan/FileCommander/releases) for available packages and the [development record](docs/development-history.md) for design decisions.
