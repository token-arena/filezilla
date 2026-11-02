# 在开源 FileZilla 上开发「Pro 功能」：开发计划与许可证合规

> 本文的许可证结论来自**本仓库源码实测**（`filezilla/COPYING`、`filezilla/README`、
> `aboutdialog.cpp`、`configure.ac` 等），不是凭印象。功能边界同样以源码中的枚举/元数据为准。
> 本文不构成法律意见；对外分发或商用前建议请律师复核。

---

## 一、许可证：你必须遵守什么

### 1.1 实测结论

| 组件 | 许可证 | 证据位置 |
|---|---|---|
| **FileZilla Client**（本仓库 `filezilla/`） | **GPL-3.0-or-later** | `filezilla/COPYING` 为 GPLv3 全文；`filezilla/README:12-15` "either version 3 of the License, or (at your option) any later version" |
| **fzssh**（SFTP 实现，Windows 构建链接它） | **AGPL-3.0-or-later** | `filezilla/README:69`、`src/interface/aboutdialog.cpp:104` |
| libfilezilla | GPL-2.0-or-later | `README:68` |
| wxWidgets 3.2 | wxWindows Library Licence 3.1+（LGPL 类，含静态链接豁免条款） | `README:70` |
| GnuTLS | LGPL-2.1-or-later | `README:71` |
| Nettle / GMP | GPL-2.0-or-later 或 LGPL-3.0-or-later（双许可，取其一） | `README:72-73` |
| pugixml | MIT | `aboutdialog.cpp:109` |
| SQLite | Public Domain | — |
| Boost | BSL-1.0 | 依赖包元数据 |

> ⚠️ 顺手发现的问题：本仓库 `PKGBUILD` 的 `license=('spdx:GPL-2.0-or-later')` 与事实不符
> （上游已升到 GPLv3+，且链接了 AGPLv3+ 的 fzssh）。建议改成
> `spdx:AGPL-3.0-or-later`（因为最终二进制不可分割地包含 AGPL 代码）。

### 1.2 四条硬结论

1. **你的修改必须开源。** GPLv3/AGPLv3 是强著佐权：一旦你**分发**（发布二进制、卖给客户、
   放进产品、提供给第三方）基于 FileZilla 的衍生作品，整件作品必须以 GPLv3+ / AGPLv3+ 授权，
   并向接收者提供**完整对应源码**（含你的改动 + 构建脚本）。
2. **AGPL 把"网络服务"也管住了。** 因为构建链了 fzssh（AGPLv3+），你的产物整体按 AGPL 分发。
   如果你把它做成对外提供的网络服务（例如浏览器里的中转/网关/托管的传输服务），
   使用该服务的人有权拿到源码（AGPL §13）。
3. **可以收费，但不能封闭。** 买家拿到二进制即获得再分发权，你无法限制他们转手。
   可行的商业模式：付费支持/定制、托管服务（须开源）、集成实施、培训、私有部署交付
   （交付时仍须给源码）。不可行：闭源"Pro 版"、按许可证限制功能。
4. **商标不随 GPL 授权。** "FileZilla" 名称与图标是上游作者（Tim Kosse）的商标，
   GPL 只给代码许可，不给商标许可。对外发行必须改名、换图标，不能暗示官方背书。

### 1.3 分发合规检查清单

- [ ] 附上 `COPYING`（GPLv3）与 AGPLv3 全文，保留全部版权声明，明确标注"本程序基于 FileZilla 修改"
- [ ] 提供完整对应源码：`filezilla/` + 你的改动 + 构建脚本（本仓库的 `PKGBUILD` 正好满足）
- [ ] 不改动许可证、不增加额外限制（GPLv3 §10）；专利按 §11 授予；不搞反规避/反 Tivoization（§6 安装信息）
- [ ] 逐项列出第三方库许可（`aboutdialog.cpp` + `README` 的 Notices 段是现成模板，需补上你的新增依赖）
- [ ] 品牌替换：窗口标题/About 对话框、`share/appdata/*.appdata.xml`、`share/applications/*.desktop`、
      NSIS 安装器（`data/`）、图标资源（`src/interface/resources/`）、可执行文件名与 `filezilla.rc`
- [ ] 若提供网络服务：在服务界面显著位置提供源码获取方式
- [ ] 不要逆向/复制 Pro 版二进制里的实现（既非许可授权，也可能违反当地法律）

### 1.4 如果你不能接受开源

- **只自用、不分发**：GPL 义务基本不触发（注意"公司内部"边界——给关联公司/外包方可能构成分发）。
- **另起炉灶**：用宽松许可的组件（libcurl、libssh2、SQLite、Dear ImGui/wxWidgets）自研客户端，
  不复用 FileZilla 代码，则可自由闭源。
- **进程隔离的"插件"**：把专有部分做成独立可执行程序、通过**文档化的外部协议**通信，通常不被视为
  衍生作品（但要真隔离：不链接、不共享内部头文件、不复制其代码），边界由律师判断。

---

## 二、Pro 功能到底是什么：源码里能看到的边界

本仓库源码中，**协议名、元数据、站点管理器 UI 都在 GPL 侧**，只有引擎实现缺失：

- `src/include/server.h:18-56` `enum ServerProtocol` 已列出：
  `S3, STORJ, WEBDAV, AZURE_FILE, AZURE_BLOB, SWIFT, GOOGLE_CLOUD, GOOGLE_DRIVE, DROPBOX,
  ONEDRIVE, B2, BOX, INSECURE_WEBDAV, RACKSPACE, STORJ_GRANT, S3_SSO, GOOGLE_CLOUD_SVC_ACC, CLOUDFLARE_R2`
- `src/engine/server.cpp` 有每种协议的元数据表（默认端口 443、是否 TLS、参数表含 `oauth_identity`、
  登录方式 profile/ADC 等），并已有 `case S3:` 分支
- `src/interface/sitemanager_controls.cpp` 已有 S3 专属 UI（`ID_S3_AES256`、profile 登录、endpoint 等）
- `src/include/server.h:241` `enum class LogonType` 已含 `profile`（S3 profile）、`adc`（Google ADC）
- **但** `src/engine/` 只有 `ftp/`、`http/`、`sftp/`、`storj/` 四个实现；
  `src/engine/engineprivate.cpp:514` 的 `switch (server.GetProtocol())` 对其它协议走 `default:`，
  直接报 `'%s' is not a supported protocol.`

**结论**：你要做的核心工作 = 为这些协议补 `CControlSocket` 实现（客户端侧），
外加 OAuth 流程、分片上传等配套设施。协议族之外，Pro 版的其它差异化功能（团队协作、
审计、企业管理等）请以官方 Pro 实际功能为准做一次对照表，再决定取舍。

---

## 三、架构：新增一个协议要动哪些文件

照着 `src/engine/storj/`（最接近的现成范例）抄：

1. **新建 `src/engine/s3/`**
   - `s3controlsocket.h/.cpp`：继承 `CControlSocket`，实现 `Connect/List/FileTransfer/Delete/Chmod/Rename/Mkdir`
   - `request.cpp`：SigV4 签名、XML 解析、错误码映射（用 libfilezilla 的 `fz::http::client`，
     `src/engine/http/` 已有用法可抄，不必引入 AWS SDK）
   - `filetransfer.cpp`：`CFileTransferOpData` 子类，支持 multipart + 断点续传
2. **`src/engine/Makefile.am`**：`if ENABLE_S3` 段 + `noinst_HEADERS`（照 `ENABLE_STORJ` 写法）
3. **`configure.ac`**：新增 `--enable-s3`（抄 647-690 行的 ftp/sftp/storj）+ `AM_CONDITIONAL`
   （默认 `no`，避免破坏默认构建，与 `--enable-storj` 一致）
4. **`src/engine/engineprivate.cpp:514`**：`#if ENABLE_S3` `case S3:`（+ `S3_SSO`、`CLOUDFLARE_R2` 视实现范围）
5. **路径语义**：S3 用现有 `ServerType::UNIX` 即可，bucket 作第一级路径；注意 delimiter/前缀、
   `continuation-token` 分页、`CommonPrefixes` → `CDirectoryListing`（目录项用 `<DIR>` 标记占位对象）
6. **能力协商**：`CServerCapabilities` / `CDirectoryListing::unsure_flags` —— 没有 mtime/权限/
   重命名原子性的地方要显式降级，否则 UI 会显示错误信息
7. **认证与凭据**：`LogonType::profile`（access key / secret / region / endpoint / path-style）
   + 系统凭据存储（与现有 site manager 一致）
8. **OAuth 类**（Google Drive / OneDrive / Dropbox / Box）：新增授权弹窗 + 本地回环回调 + PKCE。
   ⚠️ 开源客户端不能内置 client_secret（会被公开），只能用 public client + PKCE，
   需确认各 provider 允许这种注册方式
9. **i18n**：新字符串进 `locales/*.po`，`make -C locales` 生成 .mo
10. **测试**：`tests/` 加单测；用 MinIO / LocalStack 做对拍集成测试（`--enable-s3` 下 CI 起容器）

---

## 四、里程碑与排期（按 1 人全职估算）

| 里程碑 | 内容 | 验收标准 | 估算 |
|---|---|---|---|
| **M0 骨架** | `configure.ac` 开关、`src/engine/s3/` 空实现、`Makefile.am`、`engineprivate` 分派 | 能编译、能连 endpoint、能 LIST 根 bucket（哪怕只支持 path-style + 匿名） | 3–5 天 |
| **M1 S3 只读** | SigV4、LIST（前缀/分页/CommonPrefixes）、下载、断点续传、错误映射 | MinIO + AWS S3 真机浏览/下载大文件不断 | 1.5–2 周 |
| **M2 S3 写操作** | 上传（含 multipart）、建/删目录、重命名（Copy+Delete）、删除、元数据 | 与 s3cmd/aws cli 对拍一致 | 1 周 |
| **M3 站点管理器打磨** | profile 登录、region/endpoint/path-style、AES256、从 `~/.aws/credentials` 导入 | UI 全流程可用 | 3–4 天 |
| **M4 S3 兼容矩阵** | Cloudflare R2、MinIO、B2（S3 API）、阿里 OSS / 腾讯 COS / 七牛（自定义 endpoint） | 每家至少跑通 M1+M2 验收 | 1.5–2 周 |
| **M5 OAuth 全家桶** | Google Drive、OneDrive/SharePoint、Dropbox、Box | 授权+刷新+浏览+传输，token 过期自愈 | 每家 1–2 周 |
| **M6 差异化** | 并行分片、传输后校验、服务端加密、加密容器、CLI/自动化、审计日志、团队共享书签 | 按商业需求排 | 按需 |

每个里程碑都要在 **Windows(MSYS2/UCRT64) + Linux** 双平台跑通构建（我们已有 Windows 打包流程）。

---

## 五、工程实践

- **分支**：`main` = 上游 3.71.1 + 我们的改动；每个功能独立分支 + PR。
- **少改上游文件**：改动尽量集中在新目录 `src/engine/s3/`，上游文件只碰 4 处钩子
  （`configure.ac`、`src/engine/Makefile.am`、`engineprivate.cpp`、必要时 `server.h/server.cpp`）。
- **维护 `docs/upstream-hooks.md`**：记录"改了上游哪些文件的哪几行"，升级时逐条重放。
- **升级上游**：新版本 tarball 直接覆盖 `filezilla/` 内容（目录名不带版本就是为此），
  然后按 hooks 清单重放改动 + 重跑 `autoreconf -fiv`，再全量构建。
- **CI**：Linux 跑 build + `make check`；Windows 用 `msys2/setup-msys2` + `makepkg-mingw`。
- **代码风格**：跟随现有风格（`C` 前缀类、`m_` 成员、`log(logmsg::debug_*)`、`fz::` 工具库）。
- **回归验证**：改动前先跑一次基线构建留档（当前 `dist/build-vendored.log` 即基线）。

---

## 六、立刻可做的下一步

1. 修 `PKGBUILD` 的 `license` 字段（`AGPL-3.0-or-later`）。
2. 起 M0 骨架分支：`--enable-s3` + `src/engine/s3/` 空实现 + 分派 + Makefile.am，先让它编译通过。
3. 建 `docs/upstream-hooks.md` 记录钩子点。
4. 确认商业目标（自用 / 对外分发 / SaaS），这直接决定 1.2 里的义务是否符合你的预期。
