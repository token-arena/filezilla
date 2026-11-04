# HTTP 与 HTTPS：所有云协议的端点配置

企业私有化部署（MinIO、Ceph RGW、Azure Stack、私有 OSS/COS/OBS、网关代理、LocalStack 之类
的本地模拟器）经常只提供明文 HTTP。因此**所有**云存储协议都支持在 HTTP 与 HTTPS 之间选择，
并且固定域名类协议允许把 API 主机指向自建网关。

共享实现：`src/engine/cloudendpoint.{h,cpp}`（自测 `tests/endpointselftest.cpp`，49 项断言）。

## 1. 站点参数

| 参数 | 适用协议 | 取值 | 说明 |
|---|---|---|---|
| `tls` | 全部云协议 | `1`/`0`、`true`/`false`、`yes`/`no`、`on`/`off`、`https`/`http` | 强制选择 HTTPS 或 HTTP，优先级最高 |
| `api_endpoint` | B2、GCS、Google Drive、OneDrive、Dropbox（API 主机写死的协议） | `host`、`host:port` 或完整的 `http(s)://host:port` | 把 API 指向私有网关/模拟器；可自带 scheme 与端口 |
| `region` | OSS（V4 需要）、S3 家族 | 区域 ID | 不填时 OSS 从端点主机名推导 |
| `signature_version` | OSS | `v4`（默认）/`v1`/`auto` | 见 `SIGNATURE-NOTES.md` |

这些参数都在站点管理器的参数区可填（`ExtraServerParameterTraits` 已声明）。

## 2. 端点解析规则（不填 `tls` 时）

| 站点里填的 Host | 结果 |
|---|---|
| `oss-cn-hangzhou.aliyuncs.com`（无端口） | 协议默认端口：HTTPS/443 |
| `minio.local`（MinIO，默认端口 9000） | HTTP/9000（MinIO 的协议默认端口是 9000，非安全端口 → HTTP） |
| `192.168.1.10:8080` | HTTP/8080 |
| `example.com:8443` | HTTPS/8443（443/8443 视为安全端口） |
| `http://gateway:8080/path` | HTTP/8080（URL 里的 path 被忽略） |
| `[::1]:9000` | HTTP/9000，主机保持 IPv6 字面量 |

规则：**显式 `tls` > URL 里的 scheme > 端口（443/8443 = HTTPS，其它 = HTTP）> 协议默认端口**。
端口等于 scheme 默认值（HTTPS:443、HTTP:80）时不会写进 URL 与 `Host` 头；否则会带上
（例如 `minio.local:9000`），COS 这类会签名 `Host` 的协议因此也能正确工作。

## 3. 各协议的落点

| 协议 | 端点来源 | 改动 |
|---|---|---|
| S3 / Cloudflare R2 / MinIO / AWS 中国区 | 站点 Host + `tls` | scheme/端口/Host 头统一由 `cloud::resolve` 决定；MinIO 默认 9000 走 HTTP |
| Azure Blob / File | 站点 Host + `tls` | 同上（Azure Stack 常用 HTTP） |
| 阿里云 OSS / 腾讯云 COS / 华为云 OBS | 站点 Host + `tls` | 同上；COS 的 `Host` 头使用 `authority()`（含非默认端口） |
| Backblaze B2 | `api_endpoint`（默认 `api.backblazeb2.com`）+ `tls` | 原来的 `https://api.backblazeb2.com/...` 硬编码改为可配置 |
| Google Cloud Storage | `api_endpoint`（默认 `storage.googleapis.com`）+ `tls` | 生成的 URL 前缀会被替换为配置的端点 |
| Google Drive | `api_endpoint`（默认 `www.googleapis.com`）+ `tls` | 同上 |
| OneDrive | `api_endpoint`（默认 `graph.microsoft.com`）+ `tls` | 同上 |
| Dropbox | `api_endpoint`（默认 `api.dropboxapi.com`）+ `tls` | api 与 content 两个主机都会指向该端点（自建反代需同时代理两者） |

## 4. 备注与限制

- 明文 HTTP 下不再有传输加密，凭据与数据都是裸传 —— 仅建议在受信内网或本机模拟器上使用。
- TLS 证书校验仍由 FileZilla 的证书对话框负责（不是"忽略证书"，那是另一个开关）。
- 重定向类流程（Box 那种 302、OneDrive 的 downloadUrl）不在本项目范围；
  Dropbox 的 content 端点若与 api 端点不同源，自建网关需要同时代理两者。
- 这些改动只能保证"请求按配置发往正确的 scheme/端口/主机"，
  服务端是否接受签名仍取决于 `SIGNATURE-NOTES.md` 里列出的验证范围。
