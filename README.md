<div align="center">

<img src="resources/icons/FileCommander.svg" width="72" alt="FileCommander" />

# FileCommander · 文件指挥官

**又酷又快的文件管理器**

一个双栏文件管理器。绿色荧光主题不是彩蛋，是一等公民。

[![release](https://img.shields.io/badge/release-v0.1.0-33ff88?style=flat-square)](https://github.com/liuwenhuan/FileCommander/releases)
[![platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-1fa85c?style=flat-square)](#安装)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-12602f?style=flat-square)](CMakeLists.txt)
[![Qt5](https://img.shields.io/badge/Qt-5-12602f?style=flat-square)](https://www.qt.io/)
[![license](https://img.shields.io/badge/license-GPL--3.0--or--later-12602f?style=flat-square)](LICENSE)

[English](#english) · [安装](#安装) · [性能实测](#性能实测) · [从源码构建](#从源码构建)

</div>

<!-- 截图：三个主题各一张。占位，替换为真实截图。 -->
<p align="center">
  <img src="docs/images/screenshot-crt.png" width="880" alt="Green CRT 主题下的双栏主界面" />
</p>

---

## 它为什么存在

**双栏，不是两个窗口。** 左边源、右边目标，F5 复制、F6 移动，手不离键盘。从 Total
Commander / Double Commander 过来的人不需要重新学。

**彻底统一的配色。** 浅色、深色、Green CRT 三套主题覆盖每一个对话框、每一根分隔线、
每一个滚动条——包括文件列表的图标和缩略图会跟着主题走。CRT 主题里连扫描线的相位都是
对齐的：相邻的行和控件不会各自重新起一遍纹理。

**自己实现的网络后端。** SFTP / FTP / WebDAV / SMB 不走 gvfs 的 FUSE 挂载。原因不是
情怀，是测出来的——见下方[性能实测](#性能实测)。

---

## 功能

| | |
|---|---|
| **双栏浏览** | 每栏独立标签页、独立目录树、独立历史；面板互换、目录同步 |
| **压缩包** | 7z / zip / tar / squashfs / UDF 直接当目录进去浏览；加密与 solid 7z 走内置 LZMA SDK |
| **网络** | SFTP、FTP、WebDAV、SMB。同后端移动走服务端 rename（SMB 实测 ~191× 快于下载再上传）；传输支持断点续传 |
| **快速预览** | 图片、PDF、视频、Office 文档（docx/pptx/xlsx）在侧栏直接看，不开外部程序 |
| **缩略图** | 磁盘缓存 + 按滚动位置渐进抓取，先出当前屏；远程视频用字节范围规划只取关键帧那一段 |
| **同步与比较** | 目录对比、双向同步，逐条选择动作 |
| **搜索** | 文件名与内容搜索 |
| **主题** | Light / Dark / Green CRT，覆盖全部界面 |
| **多语言** | 简中、繁中、EN、DE、ES、FR、JA、KO、PT-BR、RU |
| **可改键** | F3–F8 六个槽位随便换命令；底栏按钮和键位联动 |

---

## 主题

三套主题，覆盖全部界面，不是只换菜单颜色。

| Light | Dark | Green CRT |
|---|---|---|
| ![](docs/images/theme-light.png) | ![](docs/images/theme-dark.png) | ![](docs/images/theme-crt.png) |

Green CRT 的设计约束写在 `resources/themes/green.qss` 的文件头里，一条条列着：
只用一个色相（h=140）的不同明度；`#33ff88` 是点亮的荧光、`#1fa85c` 是半亮、
`#12602f` 是未点亮的余迹、`#071108` 是玻璃；圆角全部为 0，因为荫罩画的是方格；
文字永远不用纯白，因为荧光屏上最亮的东西就是荧光本身，所以选中态反转成暗玻璃色。

---

## 性能实测

> 下面的数字来自开发过程中的实机测量，对同一台服务器、同一个目录反复测得。
> 环境不同结果会不同，欢迎自己跑一遍。

### 目录列表：1408 个条目，同一台 SMB 服务器

| 路径 | 耗时 |
|---|---|
| FileCommander 自带 `SmbProvider`（`smbc_readdirplus2`） | **88–142 ms** |
| `smbclient` 协议层 | 115 ms |
| `gio list -l`（GIO API → D-Bus → gvfsd-smb） | 100 ms |
| 通过 gvfs 的 FUSE 挂载 | **1347–1416 ms** |

慢的不是 gvfs 的 SMB 后端，而是 `gvfsd-fuse` 没有实现 FUSE 的 readdirplus——
内核只好退回「先 readdir 拿名字，再对每个条目单独 getattr」，后端攒好的批量请求
在这一层被拆散了。代价是每个条目一次往返，所以它随延迟线性放大：在 120 ms RTT 的
主机上，SFTP 实测 **139.7 ms / 条目**，1408 个条目外推到 **3 分 17 秒**。

### 文件传输

| | gvfs FUSE | FileCommander | |
|---|---|---|---|
| **千兆局域网 SMB，400 MB** | | | |
| 写入 | 103 MB/s | 107.5 MB/s | −4% |
| 读取 | 87.6 MB/s | 107.9 MB/s | −19% |
| 同 share 跨目录移动（热连接） | 4.4–5.5 ms | 1.0–1.3 ms | 4–5× |
| **广域网 SFTP，117 ms RTT，20 MB** | | | |
| 上传 | 193 kB/s | 1104 kB/s | **5.7×** |
| 下载 | 464 kB/s | 2415 kB/s | **5.2×** |

局域网上 FUSE 并不是瓶颈，写入相差 4%，线基本跑满。真正拉开差距的是延迟：FUSE 请求
同步且不流水线化，每 128 KB 一次往返，117 ms RTT 下就是 5–6 倍的损失。

---

## 安装

> v0.1.0 的安装包正在准备，稍后统一挂到对象存储上分发。现在请[从源码构建](#从源码构建)。

**Debian / Deepin / UOS**

```bash
sudo apt install ./FileCommander_0.1.0_amd64.deb
```

**AppImage（任意发行版）**

```bash
chmod +x FileCommander-0.1.0-x86_64.AppImage
./FileCommander-0.1.0-x86_64.AppImage
```

**Windows** — MSIX 包，即将推出。

Office 文档预览需要外部 CLI `office-oxide`（单独的项目）。装了才有 docx / pptx /
xlsx 预览；没装的话这个功能静默跳过，其余一切正常。

---

## 从源码构建

```bash
# Debian / Deepin 的依赖包名
sudo apt install qtbase5-dev qtbase5-dev-tools libqt5x11extras5-dev qttools5-dev-tools \
                 libarchive-dev libssh2-1-dev libsecret-1-dev libcurl4-openssl-dev \
                 libsmbclient-dev libmpv-dev libpoppler-qt5-dev zlib1g-dev libxcb1-dev

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/FileCommander
```

跑测试（需要 `libgtest-dev`）：

```bash
ctest --test-dir build
```

打包：

```bash
packaging/build-deb.sh        # → dist/FileCommander_<ver>_amd64.deb
packaging/build-appimage.sh   # → dist/FileCommander-<ver>-x86_64.AppImage
```

版本号只在 `CMakeLists.txt` 的 `project(FileCommander VERSION X.Y.Z)` 一处，别处不用改。

---

## 快捷键

底栏 F3–F8 是六个可重新绑定的槽位，右键任意一个按钮就能换成别的命令。

| 键 | 默认命令 |
|---|---|
| `F2` | 重命名 |
| `F3` | 查看 |
| `F4` | 编辑 |
| `F5` | 复制 |
| `Shift+F5` | 在当前目录复制一份 |
| `F6` | 移动 |
| `F7` | 新建目录 |
| `F8` | 删除 |
| `F10` | 退出 |
| `Ctrl+Q` | 快速预览面板 |
| `Ctrl+F1` | 选择显示模式（沿用 TC 的 `Ctrl+F<n>` 约定） |
| `Shift+F10` | 上下文菜单 |
| `Tab` | 切换面板 |

地址栏最右的 `✳` 会列出全部命令和它们当前的键位，点一行就执行。

---

## 架构

各个 CMake 子目录是独立的静态库，依赖方向严格单向，下层永不依赖上层：

```
core  →  widgets  →  viewer, archive, search  →  ui  →  FileCommander
```

- `src/core` — 文件系统抽象、文件操作、设置与会话持久化、目录同步比较、设备挂载监控、
  自更新检查，以及全部网络后端
- `src/widgets` — 无依赖的共享窗体（无边框对话框基类、标题栏、进度条）
- `src/viewer` — 文本编辑器、图片查看器、`office-oxide` 子进程封装
- `src/archive` — libarchive 压缩包浏览，外加内置的公有领域 LZMA SDK
- `src/search` — 文件名与内容搜索
- `src/ui` — `MainWindow`、双面板、目录树、全部对话框、主题、i18n、缩略图流水线
- `src/smbhelper` — 一个刻意不依赖 Qt 也不依赖本项目的独立可执行文件。libsmbclient
  无法在同一进程内并发驱动，所以 SMB 读取由一组这样的子进程完成：既能并行，
  也让 libsmbclient 崩溃时不会带走主进程

每一个后端都实现 `FileProvider`（`src/core/filesystem/FileProvider.h`）——本地、
SFTP、FTP、WebDAV、SMB、压缩包，模型和文件操作只对着这个接口写。

---

## 贡献

Issue 和 PR 都欢迎。改动之前值得先读 `CLAUDE.md`：里面记着若干「看起来该改、其实不要改」
的既有约定（比如内部标识符仍叫 `ttc`），以及几处踩过的坑。

---

<a name="english"></a>

## English

**FileCommander — a file manager that is fast, and looks it.**

A dual-pane file manager for Linux and Windows, in the Total Commander tradition.
Three themes (Light, Dark, Green CRT) that cover every dialog, divider and
scrollbar rather than just the menus. Archives (7z/zip/tar/squashfs/UDF) browse as
directories. SFTP, FTP, WebDAV and SMB are implemented in-process rather than
through gvfs — because listing a 1408-entry directory takes **88–142 ms** through
our own SMB provider and **1347–1416 ms** through the gvfs FUSE mount, and on a
117 ms-RTT link SFTP transfers measured **5.2–5.7× faster**. Quick-view for images,
PDF, video and Office documents. F3–F8 are rebindable.

Build: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)`.
See [从源码构建](#从源码构建) for dependencies, and [性能实测](#性能实测) for the full
measurement tables.
