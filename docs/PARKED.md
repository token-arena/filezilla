# 搁置清单 / 已知限制

按开发计划约定：单个功能自测失败自动修复最多 3 次，仍不通过则记录在此并推进下一个功能。
另外，**未实现但有意保留**的能力也记录在这里，避免以后误以为"已经支持"。

## 搁置（自测 3 次未通过）

暂无。第一轮的 8 个协议全部通过各自的自测与全量构建。

## 已知限制（实现层面的取舍，不是失败）

### 1. Box：新建文件上传

- **现象**：向 Box 上传一个**新的**文件名时返回 `FZ_REPLY_NOTSUPPORTED`，并在日志里给出明确提示；
  对**已存在**的文件名上传可以成功（走 `POST /files/{id}/content` 新版本上传）。
- **原因**：Box 新建文件只能走 `multipart/form-data`（`POST upload.box.com/api/2.0/files/content`）
  或分片上传会话（`/files/upload_sessions` + 带 `Digest` 头的分段 PUT）。
  这两者都需要把"前缀字节 + 文件流 + 后缀字节"拼成一个请求体，
  而引擎现在给我们的只有单个 `fz::reader_base`，libfilezilla 没有可用的拼接 reader；
  multipart 又要求每段算 SHA1。要么自己实现 `reader_base` 子类，要么改成分片会话。
- **后续方案**（下轮可做）：实现一个 `concat_reader`（组合多个 view/file reader），
  再补一个 multipart 上传 op；或直接实现 Box 的分片上传会话（需要 per-part SHA1）。

### 2. 所有 OAuth2 类协议（Drive / OneDrive / Dropbox / Box / GCS）

- **现象**：当前把 **OAuth2 access token 当作密码**填入站点管理器即可使用。
- **原因**：交互式授权（浏览器打开 + 本地回环回调 + PKCE + refresh token 续期）需要 UI 侧配合，
  而 UI 侧（`src/interface`）的授权弹窗不在本轮"只做协议兼容"的范围内。
- **注意**：开源客户端不能内置 client_secret，必须用 public client + PKCE。

### 3. 分片/断点续传上传

- 目前上传都是"单请求流式上传"。S3 >5GB、Azure block blob 分块、Box 上传会话、
  Graph 上传会话尚未实现；下载侧已支持 `Range` 续传。

### 4. 对象存储语义降级

- `chmod`、递归删目录：对 S3 / B2 / Azure / GCS / Drive / OneDrive / Dropbox / Box 一律返回
  `FZ_REPLY_NOTSUPPORTED`（对象存储没有 POSIX 权限与原子目录删除）。
- 目录用"占位对象"表示（S3/B2/Azure/GCS 用尾部 `/` 的零字节对象），
  因此某些第三方工具创建的"隐式目录"可能显示为空目录。
