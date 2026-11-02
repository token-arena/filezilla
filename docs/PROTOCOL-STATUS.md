# 第一轮收尾：8 个云存储协议落地情况

## 状态总览

| # | 协议 | 引擎目录 | 开关 | 自测 | 状态 |
|---|---|---|---|---|---|
| F1 | Amazon S3 / S3 兼容（R2、MinIO、B2-S3、OSS/COS…） | `src/engine/s3/` | `--enable-s3` | `s3selftest` 44 项 | ✅ 完成 |
| F2 | Backblaze B2（native API） | `src/engine/b2/` | `--enable-b2` | `b2selftest` 47 项 | ✅ 完成 |
| F3 | Azure Blob Storage | `src/engine/azure/` | `--enable-azureblob` | `azureselftest` 35 项 | ✅ 完成 |
| F4 | Google Cloud Storage（JSON API） | `src/engine/gcs/` | `--enable-gcs` | `gcsselftest` 32 项 | ✅ 完成 |
| F5 | Google Drive（API v3） | `src/engine/gdrive/` | `--enable-gdrive` | `driveselftest` 35 项 | ✅ 完成 |
| F6 | Microsoft OneDrive（Graph） | `src/engine/onedrive/` | `--enable-onedrive` | `graphselftest` 36 项 | ✅ 完成 |
| F7 | Dropbox（API v2） | `src/engine/dropbox/` | `--enable-dropbox` | `dropboxselftest` 41 项 | ✅ 完成 |
| F8 | Box（API v2） | `src/engine/box/` | `--enable-box` | `boxselftest` 33 项 | ⚠️ 完成，见下方限制 |

合计 **303 项自测断言**，全部通过；每个协议一个独立提交，均已推送到 `main`。

## 每个协议都实现了什么

统一实现 `Connect / List / FileTransfer（下载+上传）/ Delete / Mkdir / Rename`，
并对对象存储不支持的语义显式返回 `FZ_REPLY_NOTSUPPORTED`（`chmod`、递归删目录）：

- **S3**：SigV4 签名、ListObjectsV2 分页、流式 PUT（`UNSIGNED-PAYLOAD`）、Rename=Copy+Delete
- **B2**：`b2_authorize_account` + 一次性上传 URL、`fileId` 查删、Rename=`b2_copy_file`+删源
- **Azure Blob**：SharedKey 规范化签名、`comp=list` XML 列举、BlockBlob 流式上传、Rename=`x-ms-copy-source`+删源
- **GCS**：JSON API、`prefix/delimiter/pageToken`、media 上传、Rename=`rewriteTo`+删源
- **Google Drive**：按 `name='…' in parents` 逐段解析 id 并缓存、`uploadType=media` PATCH 上传、Rename/Move=`PATCH addParents/removeParents`
- **OneDrive**：Graph 路径寻址（`root:/a/b:`）、`downloadUrl` 两步下载、`PUT …/content` 上传、Rename/Move=`PATCH parentReference`
- **Dropbox**：`list_folder` 游标分页、content 端点流式上下传（`Dropbox-API-Arg` 纯 ASCII 转义）、`move_v2` 一次完成改名
- **Box**：`folders/{id}/items` offset 分页、id 逐段解析并缓存、跟随 302 到 `dl.boxcloud.com` 下载、`PUT`/`POST` 改名与移动

## 已知限制与"搁置"记录

见 `docs/PARKED.md`：
- **Box 新建文件上传**（multipart / upload session，流式 reader 抽象无法表达）→ 当前返回"不支持"

## 尚未覆盖（下一轮候选，均不在本轮范围内）

- 各家的 OAuth2 **交互式授权流程**（临时用 access token 作为密码接入，UI 侧弹窗与 PKCE 未做）
- 断点续传的分片上传（S3 multipart、Azure block、Box session、Graph 上传会话）
- 跨云复制、服务端加密参数、版本浏览等增强能力
