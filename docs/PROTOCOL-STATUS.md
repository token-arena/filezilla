# 云存储协议落地情况

> 注：Box 已于本轮按要求**移除**（源码与钩子全部删除，见提交历史）。
> 下表只保留当前代码里真实存在的实现。

## 状态总览

| # | 协议 | 引擎目录 | 开关 | 自测 | 状态 |
|---|---|---|---|---|---|
| F1 | Amazon S3 / S3 兼容 | `src/engine/s3/` | `--enable-s3` | `s3selftest` 50 项（含分片上传） | ✅ 完成 |
| F2 | Backblaze B2（native API） | `src/engine/b2/` | `--enable-b2` | `b2selftest` 47 项 | ✅ 完成 |
| F3 | Azure Blob Storage | `src/engine/azure/` | `--enable-azureblob` | `azureselftest` 35 项 | ✅ 完成 |
| F4 | Google Cloud Storage（JSON API） | `src/engine/gcs/` | `--enable-gcs` | `gcsselftest` 32 项 | ✅ 完成 |
| F5 | Google Drive（API v3） | `src/engine/gdrive/` | `--enable-gdrive` | `driveselftest` 35 项 | ✅ 完成 |
| F6 | Microsoft OneDrive（Graph） | `src/engine/onedrive/` | `--enable-onedrive` | `graphselftest` 36 项 | ✅ 完成 |
| F7 | Dropbox（API v2） | `src/engine/dropbox/` | `--enable-dropbox` | `dropboxselftest` 41 项 | ✅ 完成 |
| F8 | Cloudflare R2 | `src/engine/s3/`（复用） | `--enable-s3` | 并入 `s3selftest` | ✅ 完成（region=auto） |
| F9 | MinIO | `src/engine/s3/`（复用） | `--enable-s3` | 并入 `s3selftest` | ✅ 完成（默认 9000、`tls` 可覆盖） |
| F10 | AWS 中国区（S3） | `src/engine/s3/`（复用） | `--enable-s3` | 并入 `s3selftest` | ✅ 完成（region 默认 cn-north-1） |
| F11 | 阿里云 OSS（原生） | `src/engine/oss/` | `--enable-oss` | `ossselftest` 56 项 | ✅ 完成（V4 默认 + V1 自动回退 + 分片） |
| F12 | 腾讯云 COS（原生） | `src/engine/cos/` | `--enable-cos` | `cosselftest` 46 项 | ✅ 完成（q-sign + 官方 SDK 头白名单 + 分片） |
| F13 | 华为云 OBS（原生） | `src/engine/obs/` | `--enable-obs` | `obsselftest` 41 项 | ✅ 完成（OBS 头签名 + 分片，5 组官方 golden 向量） |

合计 **429 项自测断言**，全部通过；每个协议一个独立提交。

## 每个协议都实现了什么

统一实现 `Connect / List / FileTransfer（下载+上传）/ Delete / Mkdir / Rename`，
并对对象存储不支持的语义显式返回 `FZ_REPLY_NOTSUPPORTED`（`chmod`、递归删目录）：

- **S3**：SigV4 签名、ListObjectsV2 分页、流式 PUT（`UNSIGNED-PAYLOAD`）、Rename=Copy+Delete；
  `CLOUDFLARE_R2` 也走这一实现（见 `engineprivate.cpp` 的 `case CLOUDFLARE_R2:`）
- **B2**：`b2_authorize_account` + 一次性上传 URL、`fileId` 查删、Rename=`b2_copy_file`+删源
- **Azure Blob**：SharedKey 规范化签名、`comp=list` XML 列举、BlockBlob 流式上传、Rename=`x-ms-copy-source`+删源
- **GCS**：JSON API、`prefix/delimiter/pageToken`、media 上传、Rename=`rewriteTo`+删源
- **Google Drive**：按 `name='…' in parents` 逐段解析 id 并缓存、`uploadType=media` PATCH 上传、Rename/Move=`PATCH addParents/removeParents`
- **OneDrive**：Graph 路径寻址（`root:/a/b:`）、`downloadUrl` 两步下载、`PUT …/content` 上传、Rename/Move=`PATCH parentReference`
- **Dropbox**：`list_folder` 游标分页、content 端点流式上下传（`Dropbox-API-Arg` 纯 ASCII 转义）、`move_v2` 一次完成改名

## 尚未覆盖

见 `docs/PARKED.md`：交互式 OAuth2 授权、分片上传、以及对象存储的权限/目录语义降级。
## 签名依据与验证范围

各协议签名对照的官方文档/官方 SDK、用到的 golden 向量，以及**无账号下仍无法验证的部分**，
统一记录在 [`SIGNATURE-NOTES.md`](SIGNATURE-NOTES.md)。