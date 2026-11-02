# 云存储协议兼容：开发计划（第一轮）

目标：在 AGPL-3.0-or-later 下，为 FileZilla 客户端补齐各云存储协议的**引擎实现**，
使其能像 FTP/SFTP 一样浏览、上传、下载、删除、建目录。**不做**协议兼容以外的任何功能。

## 0. 现状（源码事实）

- 协议枚举、端口/TLS 元数据、站点管理器 UI **都已在 GPL 树内**
  （`src/include/server.h` 的 `ServerProtocol`、`src/engine/server.cpp` 的协议表、
  `src/interface/sitemanager_controls.cpp` 的 S3 专属项）
- `src/engine/` 只有 `ftp/`、`http/`、`sftp/`、`storj/` 四个实现
- `src/engine/engineprivate.cpp` 的 `switch (server.GetProtocol())` 对其它协议走 `default:`
  → `'%s' is not a supported protocol.`

所以每个功能的固定工作量 = **协议客户端实现 + 四处钩子**：

| 钩子 | 文件 | 作用 |
|---|---|---|
| 1 | `configure.ac` | `--enable-<proto>` + `AC_DEFINE(ENABLE_<PROTO>)` + `AM_CONDITIONAL` |
| 2 | `src/engine/Makefile.am` | `if ENABLE_<PROTO>` 源文件段 |
| 3 | `src/engine/engineprivate.cpp` | `#if ENABLE_<PROTO>` `case <PROTO>:` → `make_unique<C...ControlSocket>` |
| 4 | 必要时 `src/engine/server.cpp` | 协议参数表修正（多数已存在） |

所有协议代码放**新目录** `src/engine/<proto>/`，只在上述钩子处碰上游文件 ——
这样升级上游时冲突面最小，符合 `docs/PRO-FEATURES-PLAN.md` 的工程约束。

## 1. 功能清单与顺序

按「实现代价 / 复用度 / 覆盖面」排序，逐个开发：

| # | 功能 | 协议常量 | 传输底座 | 认证 | 关键难点 |
|---|---|---|---|---|---|
| F1 | **Amazon S3 / S3 兼容** | `S3`, `CLOUDFLARE_R2` | REST+XML | SigV4（AK/SK） | 分页、multipart、path-style vs virtual-host |
| F2 | **Backblaze B2** | `B2` | REST+JSON | Native API（keyID/appKey），或 S3 API 复用 F1 | JSON 目录树、分页游标 |
| F3 | **Azure Blob / File** | `AZURE_BLOB` | REST+XML | 共享密钥（HMAC-SHA256 规范串）或 SAS | 规范字符串、block blob 分块 |
| F4 | **Google Cloud Storage** | `GOOGLE_CLOUD`, `GOOGLE_CLOUD_SVC_ACC` | REST+XML | OAuth2 / 服务账号 JWT | JWT 断言、刷新 |
| F5 | **Google Drive** | `GOOGLE_DRIVE` | REST+JSON | OAuth2 + PKCE | 文件/文件夹 ID 语义（无真实路径） |
| F6 | **Microsoft OneDrive** | `ONEDRIVE` | REST+JSON | OAuth2 + PKCE | Graph API、分片上传会话 |
| F7 | **Dropbox** | `DROPBOX` | REST+JSON | OAuth2 + PKCE | 游标分页、批量接口 |
| F8 | **Box** | `BOX` | REST+JSON | OAuth2 + PKCE | 文件夹 ID 语义、分片上传 |

> 每完成一个就提交一次；测试不过最多修 3 次，仍不过则按「搁置」记录
> （写入 `docs/PARKED.md`）并推进下一个。

## 2. 单个功能的开发模板（统一验收标准）

1. **纯逻辑层**（可脱离引擎单测）：请求构造、签名、响应解析、路径↔对象键映射
2. **控制套接字**：`C...ControlSocket : public CRealControlSocket`，实现
   `Connect / List / FileTransfer / Delete / Mkdir / RemoveDir / Rename`
3. **能力降级**：不支持的能力（如对象存储没有 chmod/原子重命名）返回
   `FZ_REPLY_NOTSUPPORTED`，不要假装成功
4. **自测**（每次必做）：
   - 单元测试：签名/解析/路径映射的确定性用例（含官方向量，例如 AWS SigV4 测试套件）
   - 编译验证：`makepkg-mingw` 全量构建通过
   - 冒烟：连接本地 mock 服务（S3 用 MinIO 或自写 mock），LIST/GET/PUT 走通
5. **提交**：`feat(engine/<proto>): ...`，一个功能一个提交，附自测结论

## 3. 测试策略（不依赖真实云账号）

- **单元测试**：`tests/s3selftest.cpp`（纯 `main()`，`if ENABLE_S3` 挂进 `make check`）
- **mock 服务**：S3 用 MinIO（MSYS2 有 `mingw-w64-ucrt-x86_64-minio` 可装），
  没有则用 Python 标准库写最小 mock（LIST/PUT/GET/DELETE + SigV4 不校验）
- **回归**：改动前基线构建日志 `dist/build-vendored.log`

## 4. 兼容矩阵（F1 的验收面）

同一份 S3 实现要覆盖：AWS S3、Cloudflare R2、MinIO、Backblaze B2（S3 API）、
DigitalOcean Spaces、阿里云 OSS / 腾讯 COS / 七牛（自定义 endpoint + path-style）。

## 5. 里程碑

- **M0** 钩子 + 空实现（全协议开关就位，可编译）——本轮
- **M1** F1 S3 读写打通 + 单测 + 构建 + 提交 ——本轮
- **M2** F2/F3（B2、Azure Blob）
- **M3** F4（GCS）
- **M4** F5–F8（OAuth 系）

---

**第一轮执行结果：F1–F8 全部落地，详见 [PROTOCOL-STATUS.md](PROTOCOL-STATUS.md) 与 [PARKED.md](PARKED.md)。**
