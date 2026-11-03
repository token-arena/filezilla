# 新增 Cloudflare R2 / 阿里云 OSS / 腾讯云 COS / 华为云 OBS —— 开发计划（已定型，待开工）

> 状态：**计划已按你确认的三项决定定稿，尚未写任何代码。**
> 开工后按既有自动化流程逐个推进：实现 → 自测 → 全量构建打包 → 提交推送；
> 单个功能自测 3 次修不好则记入 `docs/PARKED.md` 并推进下一个。

## 0. 已确认的决策

| 决策 | 选择 |
|---|---|
| 实现路线 | **R2 复用现有 S3 实现**；**OSS / COS / OBS 走原生实现**（各自签名 + REST） |
| UI 形态 | **做成站点管理器下拉里的独立协议条目**（接受改动 `server.h` / `server.cpp` / `sitemanager_controls.cpp`） |
| 上传范围 | **包含分片上传与断点续传**（不只是单请求上传） |

## 1. 现状（代码事实）

| 事实 | 位置 |
|---|---|
| `CLOUDFLARE_R2` 已在协议枚举 | `src/include/server.h:56`（`MAX_VALUE = CLOUDFLARE_R2`） |
| `CLOUDFLARE_R2` 已在协议描述表（名 “Cloudflare R2”、前缀 `r2`、443、TLS） | `src/engine/server.cpp:45` |
| `CLOUDFLARE_R2` 已派发到我们的 S3 实现 | `src/engine/engineprivate.cpp` `case CLOUDFLARE_R2:` |
| OSS / COS / OBS 均无枚举条目 | `server.h` 只有 S3 / S3_SSO / R2 / RACKSPACE 等 |
| 新协议要在 UI 可选，需动 4 处 | 枚举、描述表 + `defaultProtocols` + 登录方式 + 参数表、`sitemanager_controls.cpp` 分组、`engineprivate.cpp` 派发 |

## 2. 四家技术差异

| 厂商 | 端点形态 | 原生鉴权 | S3 兼容度 | 关键差异点 |
|---|---|---|---|---|
| **Cloudflare R2** | `https://<account>.r2.cloudflarestorage.com` | SigV4（同 AWS） | 高 | region 必须 `auto`；部分 ACL/策略 API 不支持 |
| **阿里云 OSS** | `https://<bucket>.oss-<region>.aliyuncs.com` | OSS V4 / V1（HMAC-SHA1 变体） | 子集 | 中文键编码、`CRC64` 校验、`x-oss-` 头、multipart 上传 |
| **腾讯云 COS** | `https://<bucket>-<appid>.cos.<region>.myqcloud.com` | COS 签名（`q-sign-algorithm=sha1&q-ak=...`） | 子集 | **bucket 名必须带 appid**、签名要覆盖 header/param 列表、分块上传 |
| **华为云 OBS** | `https://<bucket>.obs.<region>.myhuaweicloud.com` | OBS 签名（近似 S3 V2，HMAC-SHA1 + CanonicalizedResource） | 子集 | `x-obs-` 头、分段上传、并行文件系统语义 |

## 3. 每个协议的交付物（统一模板）

新目录 `src/engine/<proto>/`：

- `<proto>signer.{h,cpp}` 或 `<proto>json.{h,cpp}`：**纯逻辑层**（签名/请求构造/响应解析/路径↔键映射），零引擎依赖，可秒级单测
- `<proto>controlsocket.{h,cpp}`：`C...ControlSocket : public CRealControlSocket`，实现
  `Connect / List / FileTransfer / Delete / Mkdir / RemoveDir / Rename`
- `<proto>multipart.{h,cpp}`：分片上传状态机（见第 4 节）
- `tests/<proto>selftest.cpp`：签名向量（先由**独立 Python 参考实现**算出再对齐）+ XML/JSON 样本解析

上游钩子（每处都登记进 `docs/upstream-hooks.md`）：

1. `configure.ac`：`--enable-<proto>` + `AC_DEFINE([ENABLE_<PROTO>])` + `AM_CONDITIONAL`
2. `src/engine/Makefile.am`：`if ENABLE_<PROTO>` 源文件段
3. `src/engine/engineprivate.cpp`：`#include` + `case <PROTO>:` 派发
4. `src/include/server.h`：枚举条目（插在 `MAX_VALUE` 之前，**不得改动已有值**）
5. `src/engine/server.cpp`：描述表一行、`defaultProtocols` 一项、`GetSupportedLogonTypes`、
   必要时参数表（region / endpoint / appid 等）
6. `src/interface/sitemanager_controls.cpp`：`ProtocolGroup` 里加一项，使下拉框可见
7. `tests/Makefile.am`：自测程序
8. `PKGBUILD`：`--enable-<proto>`

## 4. 分片上传与续传设计（统一抽象）

引擎的 `FileTransfer` 是"一次命令一次传输"，所以分片全部在控制套接字内部完成，不动引擎：

```
init   → 计算分片大小（默认 8 MiB，可在站点参数覆盖）
       → 创建上传会话（各家不同，见下）
loop   → 读 reader 的一个分片 → 计算校验（MD5/SHA1/CRC64）→ PUT 分片 → 记录 {partNumber, etag}
       → 失败时对同一分片重试 N 次（指数退避）
commit → 完成上传（CompleteMultipartUpload / commit），带全部分片 etag
abort  → 用户取消时 AbortMultipartUpload，避免残留碎片
```

各家实现：

| 厂商 | 会话创建 | 分片 | 完成 |
|---|---|---|---|
| S3 / R2 | `POST /bucket/key?uploads` → UploadId | `PUT ?partNumber=N&uploadId=…` → ETag | `POST ?uploadId=…` + XML 清单 |
| OSS | `POST /key?uploads` → UploadId | `PUT ?partNumber&uploadId` | `POST ?uploadId` |
| COS | `POST /key?uploads` | `PUT ?partNumber&uploadId` | `POST ?uploadId` |
| OBS | `POST /key?uploads` | `PUT ?partNumber&uploadId` | `POST ?uploadId` |

**续传的两种含义**（见第 6 节问题 4）：

- *会话内续传*：某分片失败后重试、`FileTransfer` 被取消后可从已完成分片继续 —— **本轮实现**
- *跨会话续传*：进程重启后接着传 —— 需要引擎侧持久化分片状态，**本轮不做**（除非你要求）

## 5. 排期（1 人全职，含分片上传）

| 阶段 | 内容 | 估算 |
|---|---|---|
| M0 | R2：验证 S3 复用（region=auto、端点、path-style）+ S3 multipart 接入 + 测试 + 文档 | 1–1.5 天 |
| M1 | 阿里云 OSS：原生签名 + 六项操作 + multipart + mock 对拍 | 3–4 天 |
| M2 | 腾讯云 COS：COS 签名 + appid 语义 + 分块上传 + mock 对拍 | 3–4 天 |
| M3 | 华为云 OBS：OBS 签名 + 六项操作 + 分段上传 + mock 对拍 | 3–4 天 |
| M4 | 收尾：三处 UI 条目统一整理、参数预设、`PROTOCOL-STATUS.md` 与合规复核 | 1 天 |

每家完成即提交（一个协议一个 commit），并附自测结论。

## 6. 仍需你确认（不阻塞开工，但影响验收方式）

1. **测试账号**：能否提供四家各一个最小权限账号（单 bucket 读写）？
   - 能 → 做真机验证（签名/分页/分片都实测）
   - 不能 → 我按官方签名文档 + 本地 mock 对拍，提交信息里会注明"未做真机验证"
2. **是否还有别家**要一起排期（七牛 Kodo、金山 KS3、MinIO、AWS 中国区…）？
3. **下拉框显示名**：建议中文（“阿里云 OSS / 腾讯云 COS / 华为云 OBS”）或中英并列，你定。
4. **跨会话续传**是否需要（当前只做会话内续传）。

## 7. 验收标准（每个协议）

- 自测：签名向量全绿 + 解析样本全绿 + mock 服务的"列举/下载/上传/分片上传/删除/建目录/改名"全部走通
- 构建：`makepkg-mingw` 全量通过，引擎 DLL 中能查到该协议端点与签名相关字符串
- 兜底：不支持的能力（`chmod`、递归删目录）显式返回 `FZ_REPLY_NOTSUPPORTED`，不假装成功
---

## 8. 澄清结果（已确认，2026-11）

| 问题 | 结论 |
|---|---|
| 测试账号 | **没有**，全部按各家官方文档实现 + 本地 mock 对拍验证，提交信息注明"未做真机验证" |
| 协议范围 | 追加 **MinIO** 与 **AWS 中国区**（都在 S3 家族，复用现有 S3 实现） |
| 显示名 | **全英文**：`Cloudflare R2`、`MinIO - S3 compatible object storage`、`S3 - Amazon S3 China (Beijing and Ningxia)`、`Alibaba Cloud OSS`、`Tencent Cloud COS`、`Huawei Cloud OBS` |
| 跨会话续传 | **不需要**，只做会话内续传（分片级重试 + 取消后从已完成分片继续） |

调整后的功能清单与顺序：

1. **S3 分片上传**（R2 / MinIO / AWS 中国区都依赖它）— ✅ 已完成
2. **Cloudflare R2**（region=`auto`）+ **MinIO**（默认 9000 明文、`tls` 参数可覆盖）+ **AWS 中国区**（region 默认 `cn-north-1`）— ✅ 已完成（协议条目 + 派发 + 默认值）
3. 阿里云 OSS（原生签名 + 分片）
4. 腾讯云 COS（原生签名 + 分块）
5. 华为云 OBS（原生签名 + 分段）
