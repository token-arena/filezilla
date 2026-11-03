# 新增 Cloudflare R2 / 阿里云 OSS / 腾讯云 COS / 华为云 OBS —— 开发计划（待澄清，暂不动手）

> 本文只是计划。等你确认第 5 节的几个问题后，才按既有自动化流程逐个开发
> （每个功能：实现 → 自测 → 全量构建 → 提交推送；自测 3 次修不好则记入 `docs/PARKED.md` 并推进下一个）。

## 1. 现状（代码事实，不是推测）

| 事实 | 位置 |
|---|---|
| `CLOUDFLARE_R2` **已在**协议枚举里 | `src/include/server.h:56`（且 `MAX_VALUE = CLOUDFLARE_R2`） |
| `CLOUDFLARE_R2` **已在**协议描述表里（名字 “Cloudflare R2”、前缀 `r2`、端口 443、TLS） | `src/engine/server.cpp:45` |
| `CLOUDFLARE_R2` **已经**派发到我们的 S3 实现 | `src/engine/engineprivate.cpp` 的 `case CLOUDFLARE_R2:` |
| 没有 OSS / COS / OBS 的枚举条目 | `server.h` 里只有 S3、S3_SSO、R2、RACKSPACE 等 |
| 新协议要“在站点管理器里可选”，需要动 4 处上游文件 | `server.h`(枚举) + `server.cpp`(描述表、`defaultProtocols`、登录方式、参数表) + `src/interface/sitemanager_controls.cpp`(下拉分组) + `engineprivate.cpp`(派发) |
| 我们的 S3 实现是 **path-style + SigV4**，region 默认 `us-east-1`，上传用 `UNSIGNED-PAYLOAD` | `src/engine/s3/` |

所以：**R2 基本等于“接线 + 验证 + 补测试/文档”**，OSS/COS/OBS 才需要真正的新实现。

## 2. 四家的技术差异（决定工作量）

| 厂商 | 端点形态 | 原生鉴权 | S3 兼容度 | 需要特别注意 |
|---|---|---|---|---|
| **Cloudflare R2** | `https://<account>.r2.cloudflarestorage.com` | SigV4（与 AWS 一致） | 高 | region 必须填 `auto`；不支持部分 ACL/策略类 API |
| **阿里云 OSS** | `https://<bucket>.oss-<region>.aliyuncs.com` | 原生 OSS 签名（V4 / V1，HMAC-SHA1 变体） | 中等（子集） | 中文/特殊字符键的编码规则、`CRC64` 校验、`x-oss-` 头 |
| **腾讯云 COS** | `https://<bucket>-<appid>.cos.<region>.myqcloud.com` | 原生 COS 签名（`q-sign-algorithm=sha1&q-ak=...`） | 中等（子集） | **bucket 名必须带 appid**；签名要覆盖 header 列表与 param 列表 |
| **华为云 OBS** | `https://<bucket>.obs.<region>.myhuaweicloud.com` | 原生 OBS 签名（近似 S3 V2：HMAC-SHA1 + CanonicalizedResource） | 中等（子集） | 有并行文件系统语义；请求头 `x-obs-` |

## 3. 两条路线（需要你拍板）

### 路线 A：S3 复用（最低成本）

把四家都当 “S3 兼容服务”用现有实现：加端点预设 + region + path-style 开关，必要时加 4 个枚举条目只为了让下拉框能选。

- 成本：每家 **0.5–1 天**（端点/签名验证 + 单测 + 文档）
- 风险：
  - 各家 S3 兼容层是**子集**，分块上传、ACL、中文键、分页行为容易踩坑；
  - 部分地域/老账号不支持 S3 兼容接口；
  - COS 的 `bucket-appid` 命名和 OSS 的编码规则与 AWS 不完全一致，容易出现 “SignatureDoesNotMatch”。

### 路线 B：原生实现（像 Azure/B2 那样）

每家写自己的签名 + REST 客户端（列举/下载/上传/删除/建目录/改名）。

- 成本：每家 **2–4 天**
- 优点：错误码准确、能覆盖各家特性、不依赖 S3 兼容层的“运气”
- 代价：四份独立代码，维护面变大

### 我的建议

- **R2 → 路线 A**（它本来就是 S3 兼容，且枚举已存在，只需验证 + 补测试）
- **OSS / COS / OBS → 路线 B**（原生签名与 AWS 有实质差异，走兼容层会留下长期隐患）

## 4. 每个功能的开发与自测模板（沿用前 8 个协议的做法）

1. **纯逻辑层**（可脱离引擎秒级单测）：签名/请求构造/响应解析/路径↔对象键映射
   - 签名向量先由**独立的 Python 参考实现**算出再对齐（S3 SigV4、Azure SharedKey 都是这么验的）
2. **控制套接字**：`C...ControlSocket : public CRealControlSocket`，实现
   `Connect / List / FileTransfer / Delete / Mkdir / RemoveDir / Rename`，
   不支持的能力返回 `FZ_REPLY_NOTSUPPORTED`
3. **钩子**：`configure.ac`、`src/engine/Makefile.am`、`engineprivate.cpp`、
   （若要在 UI 里可选）`server.h` + `server.cpp` + `sitemanager_controls.cpp`、`tests/Makefile.am`、`PKGBUILD`
   —— 全部登记进 `docs/upstream-hooks.md`
4. **自测**：
   - 协议逻辑单测（签名向量、XML/JSON 样本、URL 构造）
   - 本地 **mock 服务**（Python 起最小 HTTP 服务，校验签名头格式、返回固定响应）做“请求↔响应”对拍
   - 全量 `makepkg-mingw` 构建 + 打包，确认代码进了引擎 DLL
5. **提交**：一个功能一个提交；3 次修不好 → 记 `docs/PARKED.md` → 推进下一个

## 5. 需要你澄清的问题

**Q1（最关键）路线**：R2 走 A、OSS/COS/OBS 走 B —— 同意吗？
还是四家全部走 A（快，但兼容性风险高）／全部走 B（慢但干净）？

**Q2（UI 形态）**：OSS/COS/OBS 要不要做成站点管理器下拉里的**独立条目**？
- 要（推荐）：需改 `server.h` / `server.cpp` / `sitemanager_controls.cpp` 三处上游文件，改动可控但要逐条登记
- 不要：用户选 “S3” 后手填端点 —— 零上游改动，但体验差（显示名仍叫 Amazon S3）

**Q3（测试账号）**：能否提供四家各一个**最小权限**测试账号（只读写一个测试 bucket）？
- 能：可以真机验证签名与分页、避免“上线才发现差异”
- 不能：我按各家官方签名文档 + 本地 mock 对拍实现，会在提交信息里注明“未做真机验证”

**Q4（上传范围）**：先只做**单请求流式上传**，还是同时做**分片/断点续传**？
（S3 multipart、OSS multipart、COS 分块、OBS 分段；分片会显著增加工作量）

**Q5（还有别家吗）**：除这四家，是否还要排期七牛 Kodo、金山 KS3、MinIO、Backblaze（S3 API）、AWS 中国区？

**Q6（显示名）**：下拉框里用中文名（“阿里云 OSS / 腾讯云 COS / 华为云 OBS”）还是英文名（“Alibaba Cloud OSS / Tencent Cloud COS / Huawei Cloud OBS”）？

## 6. 排期预览（按建议方案，1 人全职）

| 阶段 | 内容 | 估算 |
|---|---|---|
| 0 | R2：验证 S3 复用（region=auto、端点、path-style）+ 单测 + 文档 | 0.5–1 天 |
| 1 | 阿里云 OSS：原生签名 + 六项操作 + mock 对拍 | 2–3 天 |
| 2 | 腾讯云 COS：原生签名（q-sign）+ appid 语义 + mock 对拍 | 2–3 天 |
| 3 | 华为云 OBS：原生签名 + 六项操作 + mock 对拍 | 2–3 天 |
| 4 | （可选）UI 收尾、参数预设、文档与合规复核 | 1 天 |

每家完成后各写一个提交 + 自测结论；全部完成后更新 `docs/PROTOCOL-STATUS.md`。
