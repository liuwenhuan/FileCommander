# WPS 格式预览兼容方案

> 技术调研报告 + 开发计划
> 日期:2026-07-23 · 目标:让 ttc 支持金山 WPS 私有格式(`.wps`/`.et`/`.dps` 及模板)的只读预览

---

## 一、目标与结论

**目标**:在 ttc 的文件预览中支持金山 WPS Office 的三种私有格式,复用现有 office 预览管线,不引入重依赖。

**结论(已实测验证)**:路线**完全走通**。核心做法是 **"扩展名重映射 + 复用 office-oxide"**——把 WPS 私有格式在调用前当作对应的 OOXML 类型(`.dps`→pptx、`.et`→xlsx、`.wps`→docx)喂给现有的 `office-oxide` CLI,即可解析出内容,无需新增任何解析库。

---

## 二、背景调研

### 2.1 WPS 私有格式家族

| 扩展名 | 含义 | 对应微软格式 | ttc 归类 |
|--------|------|--------------|----------|
| `.wps` / `.wpt` | WPS 文字 / 模板 | `.docx` / `.doc` | Document(文字) |
| `.et` / `.ett` | WPS 表格 / 模板 | `.xlsx` / `.xls` | Spreadsheet |
| `.dps` / `.dpt` | WPS 演示 / 模板 | `.pptx` / `.ppt` | Document(演示) |

### 2.2 两个必须澄清的坑

1. **`.wps` 同名歧义**:该扩展名同时被"金山 WPS 文字"和"微软 Works 文字处理器"使用,两者毫无关系。网上大量提到的开源库 **libwps 是给微软 Works 用的**,对金山 `.et`/`.dps` 支持很差——不可采用。
2. **"改名 .zip 就能解"不可靠**:网上资料称现代 WPS 存的是 OOXML(zip)、改扩展名即可解压。**实测证伪**——见 2.3。

### 2.3 关键发现:真实 WPS 文件是 OLE2 复合文档,不是纯 zip

对本机 WPS **12.1** 保存的真实文档做魔数检测:

| 文件 | `file` 魔数 |
|------|-------------|
| `立项建议书.wps` | Composite Document File V2 · Creating Application: **WPS Office_12.1.2** |
| `议题2关联方.et` | Composite Document File V2 · Creating Application: WPS |
| `smartart.dps` | Composite Document File V2 |

即:**这台机的 WPS 12.1 默认保存为 OLE2 复合文档(CFB)**,而非纯 OOXML zip。系统出厂模板(`/opt/apps/cn.wps.wps-office/.../templates/*`)同样是 OLE2 封装,其中 `.et`/`.dps` 内部又嵌了一层 OOXML 部件,`.wps` 则是纯二进制。

**推论**:任何"检测 zip 魔数 → 解压 → 复用"的方案都会漏掉大量真实文件。必须依赖能读 OLE2 封装的解析器。好在 `office-oxide` 对 OLE2 封装和纯 OOXML **都能读**。

---

## 三、技术验证

### 3.1 验证环境

- `office-oxide` 已安装:`~/.local/bin/office-oxide`(codework 补丁 fork)
- 测试样本:用户提供的 WPS 12.1 真实文档 + 系统出厂模板

### 3.2 决定性发现:office-oxide 只按扩展名判断格式

把 pptx/xlsx/docx **改名**成 `.dps/.et/.wps` 后直接喂 office-oxide:

```
office-oxide html sample.dps  → error: unsupported format: dps
office-oxide text sample.et   → error: unsupported format: et
office-oxide html sample.wps  → error: unsupported format: wps
```

→ **office-oxide 靠后缀识别,不看文件内容**。因此 ttc 集成必须做"扩展名重映射":调用前把源文件呈现为带标准后缀(`.pptx/.xlsx/.docx`)的临时文件。

### 3.3 端到端实测(重映射后)

对用户提供的真实 WPS 文件,复制成标准后缀再解析:

| 真实文件 | 魔数 | 重映射 | 子命令 | 结果 |
|---------|------|--------|--------|------|
| `smartart.dps` | OLE2 | →`.pptx` | `html` | ✅ 输出母版标题/正文 HTML |
| `公司PPT模板.dps` | OLE2 | →`.pptx` | `html` | ✅ "中国操作系统领创者…" 全文 |
| `议题2关联方.et` | OLE2 | →`.xlsx` | `text` | ✅ TSV 表格(华为/维沃/金额…) |
| `立项建议书.wps` | OLE2 | →`.docx` | `html` | ✅ 标题+表格+全文,329 KB 完整 |

**三种格式 100% 解析成功,exit=0。**

> 注:验证中一度出现 `.wps` `exit=101`,经排查为测试命令用 `head -c` 截断管道触发 BrokenPipe 的假象;单独运行退出码为 0、stdout 329 KB 完整、stderr 为空。

### 3.4 已知边界

- **老式纯二进制 `.wps` 文字**(如系统出厂 `templates/WPS文字文档.wps`,内部无 OOXML 部件):office-oxide 输出**空**(exit=0)。用户日常用现代 WPS 保存的文件不受影响,但需对"成功但空输出"做兜底提示。
- `.et`/`.dps` 出厂模板即使是 OLE2 封装也能正常读出,未见空输出问题。

---

## 四、技术决策

| # | 决策 | 理由 |
|---|------|------|
| D1 | 复用 `office-oxide`,不引入新解析库 | 已实测可读 WPS 的 OLE2 封装与 OOXML;避免 libwps(错库)、LibreOffice(300MB+ 重依赖) |
| D2 | 采用"扩展名重映射到临时文件"策略 | office-oxide 只认后缀;不可只改 `kindFor` |
| D3 | 临时文件用**硬链接**优先、失败回退**复制** | 硬链零拷贝、最快;跨文件系统或权限受限时回退 `QFile::copy` |
| D4 | `.dps`/`.dpt` 走演示分支(`stripImgTags`) | 与 pptx 同源,图片按幻灯片几何定位、与扁平文本不对齐 |
| D5 | "成功但空输出"识别为老式二进制,给友好提示 | 避免用户看到空白预览却无解释 |

### 映射表(集成核心)

| WPS 后缀 | Kind | 临时后缀 | 子命令 | 演示分支 |
|----------|------|----------|--------|----------|
| `wps` `wpt` | Document | `.docx` | `html` | 否 |
| `dps` `dpt` | Document | `.pptx` | `html` | **是**(strip img) |
| `et` `ett` | Spreadsheet | `.xlsx` | `text` | — |

---

## 五、开发计划

### 阶段 1 — OfficeConverter 核心改造(`src/viewer/OfficeConverter.{h,cpp}`)

1. **`kindFor()`**:新增 `wps/wpt/dps/dpt`→`Document`,`et/ett`→`Spreadsheet`。
2. **重映射工具**:新增内部函数,判断某后缀是否 WPS 私有格式并返回其目标标准后缀;返回空表示原生格式(走现有路径)。
3. **临时文件桥接**:在 `convert()` 里,若源为 WPS 格式,先把它硬链/复制到 `QTemporaryDir` 下的 `preview.<std-suffix>`,后续所有 office-oxide 调用改用该临时路径;`convert()` 返回前自动清理(RAII/`QTemporaryDir`)。
4. **演示分支判定**:`convertDocument()` 里 `isPresentation` 的判断从"`ppt/pptx`"扩展为"`ppt/pptx/dps/dpt`"(基于**原始**后缀,而非临时后缀)。
5. **空输出兜底**:文档/表格转换成功但输出为空时,若原始后缀是 `wps` 系,置 `ok=false` 且 `error` 提示"疑似老式 WPS 二进制格式,office-oxide 暂不支持,请用 WPS 另存为 .docx/.xlsx/.pptx"。

### 阶段 2 — 预览分发接入

6. 检查 `QuickView` / `FileSystemModel` 等处对"是否可预览"的判断(`isOfficeFile`),确认新后缀被纳入 F3/Ctrl+Q 预览与文件列表图标逻辑。
7. 加密路径(`OFFICE_OXIDE_PASSWORD` + `classifyEncryption`)天然复用,无需改动;验证加密 WPS(若有样本)。

### 阶段 3 — 构建与回归

8. 在 `build/` 目录编译(遵循项目约定)。
9. 用本次三个真实样本 + 系统模板回归:
   - `smartart.dps` / `公司PPT模板.dps` → 有 HTML 正文
   - `议题2关联方.et` → 有表格网格
   - `立项建议书.wps` → 有正文+表格
   - 老式 `templates/WPS文字文档.wps` → 命中空输出兜底提示
10. GUI 冒烟:Xvfb 下打开预览,确认渲染正常(参照既有 GUI 测试流程)。

### 阶段 4 — 收尾

11. 更新用户可见的支持格式说明/图标资源(如有)。
12. 提交:`feat: WPS 私有格式(.wps/.et/.dps)预览支持`。

---

## 六、风险与限制

| 风险 | 影响 | 缓解 |
|------|------|------|
| 老式纯二进制 `.wps` 文字读出空 | 该类文件无法预览 | 空输出兜底提示;引导另存为 docx |
| office-oxide 未安装 | 所有 office 预览不可用(既有约束) | 沿用现有 `resolveBinary()` 缺失提示 |
| 不同 WPS 版本内部结构差异 | 少数文件解析不全 | 本次覆盖 WPS 12.1;后续按样本补测 |
| 临时文件磁盘/权限 | 极端情况桥接失败 | 硬链→复制回退;失败填 `error` 不崩溃 |

---

## 七、验收标准

- [ ] `.dps/.dpt` 现代文件预览出正文(演示分支,去图)
- [ ] `.et/.ett` 现代文件预览出表格网格
- [ ] `.wps/.wpt` 现代文件预览出正文+表格
- [ ] 老式纯二进制 `.wps` 给出明确的"请另存为"提示而非空白
- [ ] 加密 WPS(若有)走密码提示流程
- [ ] 临时文件在 `convert()` 结束后无残留
- [ ] `build/` 编译通过,GUI 冒烟正常
