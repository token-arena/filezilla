# 签名与协议实现：依据与验证记录

本项目所有云存储签名实现都对照了**官方文档**或**官方 SDK 源码**，并用官方文档里给出的
示例值做 golden 测试。本文记录每家的依据、验证到什么程度、以及**在没有账号的情况下仍然
无法验证的部分**（不能想当然地说"已兼容"）。

## 1. Amazon S3 / R2 / MinIO / AWS 中国区 —— AWS Signature V4

- **依据**：AWS S3 官方文档 "Signature Version 4 for header-based authentication"。
- **验证**：用文档中 GET Object 示例（含 Range 头）的**公布签名**
  `f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41` 作为 golden 值，
  已通过。另有 4 组自建向量由独立 Python 实现交叉生成。
- **覆盖**：ListObjectsV2、GetObject（含 Range 续传）、PutObject、分片上传
  （initiate / part / complete / abort）、DeleteObject、CopyObject（改名）、
  ListBuckets。

## 2. 阿里云 OSS —— 签名 V4（默认）与 V1（回退）

- **依据**
  - V4：官方文档《在 Header 中包含 V4 签名》（`OSS4-HMAC-SHA256`），以及官方 Python SDK
    v2 的 `signer/v4.py` 源码。
  - V1：官方文档《签名版本 1》（`OSS <ak>:<base64(HMAC-SHA1)>`）。
- **验证**
  - V4 的 canonical request 与文档示例**逐字节一致**；`StringToSign` 里的
    `Hex(SHA256(CanonicalRequest))` 与文档公布的 `c46d9639…` 一致。
  - 文档公布的最末签名值 `053edbf5…` **无法复现**，因为文档示例的 SK 只给了占位值
    `yourAccessKeySecret`（真实密钥未公开）。这一项我用独立 Python 实现算出期望值
    `d3694c2d…` 对齐，并在测试里注明原因。
  - V1 用 10 组向量验证（服务级 / bucket / object / 分片 / STS / 自定义头）。
- **策略**：`signature_version` 站点参数取 `v4`（默认）、`v1` 或 `auto`。
  `auto` 先用 V4，若服务端返回 400/403 且错误码属于
  `SignatureDoesNotMatch` / `InvalidAccessKeyId` / `AccessDenied` / `InvalidArgument` /
  `InvalidRequest` / `OperationNotSupported`，**自动用 V1 重发一次**。
  带请求体的请求（上传）无法重放（reader 已消费），失败时直接报错，需要用户改设 `v1`。
- **region**：优先取 `region` 站点参数，否则从端点主机名推导
  （`oss-cn-hangzhou.aliyuncs.com` → `cn-hangzhou`）。

## 3. 腾讯云 COS —— q-sign-algorithm=sha1

- **依据**：官方文档《请求签名》，以及官方 C++ SDK
  `cos-cpp-sdk-v5/src/util/auth_tool.cpp` 源码。
- **验证**：签名结构（`SignKey = HMAC-SHA1(SK, KeyTime)`、`HttpString`、
  `StringToSign`、`q-header-list`/`q-url-param-list` 的编码与排序）逐项对照 SDK 源码实现；
  7 组向量由独立 Python 实现交叉生成。
- **头过滤**：按 SDK 的白名单，仅以下头参与签名——
  cache-control、content-disposition、content-encoding、content-length、content-md5、
  content-type、expect、expires、host、if-match、if-modified-since、if-none-match、
  if-unmodified-since、origin、range、transfer-encoding、pic-operations，以及
  `x-cos*`、`x-ci*`。其它头（如 User-Agent）不进入签名，已有测试锁定该行为。

## 4. 华为云 OBS —— Authorization 头签名（S3 V2 风格）

- **依据**：官方 API 参考《Using an Authorization Header》（含 StringToSign 结构与多组示例）。
- **验证**：文档表 4–8 给出的 5 组 `StringToSign` **全部作为 golden 值通过**，覆盖：
  仅 Date；`x-obs-date` + `x-obs-security-token` + content-type；额外 `x-obs-acl`；
  子资源 `?acl`；`Content-MD5`。
- **实现要点**：`CanonicalizedHeaders` 每行以 `\n` 结尾（为空则不占行），
  子资源以 `?a&b=c` 形式按字典序拼接。

## 5. 其它已实现协议

B2（native API）、Azure Blob（SharedKey）、GCS（JSON API）、Google Drive（v3）、
OneDrive（Graph）、Dropbox（v2）的依据与验证方式见各自提交信息与
`docs/PROTOCOL-STATUS.md`。

## 6. 仍然无法验证的部分（必须如实说明）

没有真实账号，以下都无法端到端确认，只能保证"按文档/官方 SDK 构造请求 + 编译可运行"：

1. 各家服务端是否接受我们构造的请求（尤其是签名）。
2. 分页/`marker`/`NextMarker` 的真实返回形态与边界情况。
3. 中文/特殊字符对象名的编码与回显。
4. 分片上传的完整流程（initiate/part/complete）在真实服务上的行为与最小分片限制。
5. 各家的权限错误码是否与文档一致（影响 `auto` 回退的触发条件）。

建议：拿到任意一家的最小权限账号后，优先跑一条"列举 → 上传小文件 → 下载 → 删除"的
冒烟链路；失败时消息日志里的 `Server error: <Code> (<Message>)` 可直接定位到具体环节。
