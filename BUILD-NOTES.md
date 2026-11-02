# FileZilla 3.71.1 — MSYS2 打包 + 二次开发仓库

基于 MSYS2 `MINGW-packages` 中 `mingw-w64-filezilla` 的官方 PKGBUILD
（上游提交 `442fc22ddf2873a9c46360aa51b094691fa7cb01`）。

**本仓库已改为「源码树入库」模式**：源码直接放在 `filezilla/`（目录名不带版本号，
便于后续版本迭代），PKGBUILD 不再下载 tar.xz、也不重复打补丁，直接编译仓库内源码。

## 1. 仓库结构

```
PKGBUILD                  # 直接编译 ./filezilla 的打包脚本
BUILD-NOTES.md            # 本文件
.gitignore
filezilla/                # FileZilla 3.71.1 源码树（含 autoreconf 生成物，可二次开发）
patches/                  # 已应用到 filezilla/ 的两个 MSYS2 补丁（留档）
dist/                     # 打包产物 + 构建日志（不入库）
build-UCRT64/             # makepkg 的编译目录（不入库）
src/  pkg/                # makepkg 的 srcdir / pkgdir（不入库）
_work/                    # 临时脚本（不入库）
```

`filezilla/` 里已包含：

- 上游 `filezilla-3.71.1.tar.xz` 解包内容
- `patches/0002-fix-mingw-compiler-detection.patch`（改 `src/fzshellext/configure.ac`）
- `patches/0003-fix-building-po-files.patch`（改 `locales/Makefile.am`）
- `autoreconf -fiv` 的生成物：顶层 `configure`、`aclocal.m4`、各 `Makefile.in`

> 注意：源码树里的 `config/compile`、`config/install-sh`、`config/missing`、
> `config/depcomp`、`config/ltmain.sh`、`config.h.in`、`Makefile.in` 是**上游随源码
> 跟踪的文件**，`.gitignore` 特意没有忽略它们；只忽略 `autom4te.cache/` 与 `*~` 备份。

## 2. 日常开发 / 打包流程

在 MSYS2 **UCRT64** 终端里，仓库根目录执行：

```bash
MINGW_ARCH=ucrt64 makepkg-mingw -s --noconfirm --nocheck
```

- 改 `./filezilla/` 下的代码后直接重跑即可，`build-UCRT64/` 里是增量编译
- 产物：仓库根目录的 `mingw-w64-ucrt-x86_64-filezilla-<pkgver>-<pkgrel>-any.pkg.tar.zst`
- 想从零重编：删掉 `build-UCRT64/`（或给 makepkg 加 `-C`）再跑
- 改了 `configure.ac` / `Makefile.am`：先 `cd filezilla && autoreconf -fiv` 并提交生成物
- 升级到新版本：用新 tar.xz 覆盖 `filezilla/` 内容（保持目录名不变），
  同步 `PKGBUILD` 的 `pkgver`、必要时更新 `patches/`

安装 / 运行：

```bash
pacman -U mingw-w64-ucrt-x86_64-filezilla-3.71.1-1-any.pkg.tar.zst
/ucrt64/bin/filezilla.exe
```

## 3. 环境

- Windows 10 IoT LTSC 19044 x64，MSYS2 位于 `C:\msys64`，UCRT64 环境
- 构建前已 `pacman -Syu` 升级（`msys2-runtime` 升级会关掉终端，需重跑一次），并安装 `base-devel`
- 依赖由 `makepkg-mingw -s` 自动装：
  - 运行期 `ucrt-x86_64-{cc-libs,fzssh,gnutls,libfilezilla,sqlite3,wxwidgets3.2-msw-libs}`
  - 构建期 `ucrt-x86_64-{boost,cc(=gcc),autotools,wxwidgets3.2-msw}`
- 版本：gcc 16.2.0、wxWidgets 3.2.11、libfilezilla 0.58.0、fzssh 1.5.0、boost 1.92.0

## 4. 构建记录（要点）

1. 首次按原始 PKGBUILD 走「下载 tar.xz + 打补丁 + autoreconf」流程，
   sha256 三项校验全通过，编译到 `src/interface` 阶段一切正常。
2. 该次构建在 `src/fzshellext` 的 Makefile 再生步骤（aclocal/autoconf/automake +
   `./config.status`）**死锁**（make 进程 CPU 0、日志停滞十几分钟）。
   处理：终止后保留已编译树、串行 `make -j1` 续建成功。
   —— 这也是改成「源码树入库、生成物一并提交」的直接原因：生成物齐全后
   make 不再触发再生，并行编译（`-jN -Otarget`）顺畅完成，第二次全量构建未再卡。
3. 移入仓库后从零重建（`build-UCRT64/` 全新）一次通过，日志见
   `dist/build-vendored.log`；此前两阶段日志见 `dist/build-phase1.log`、`dist/build-phase2.log`。

## 5. 验证

- `pacman -Qip`：`mingw-w64-ucrt-x86_64-filezilla 3.71.1-1`；6 个 depends 全部满足
- 包内容 943 个文件，含 `ucrt64/bin/filezilla.exe`、`libfzclient-private-3-71-1.dll`、
  `libfzclient-commonui-private-3-71-1.dll`、`libfzshellext-0.dll`、图标资源、181 个语言包、
  `share/licenses/filezilla/COPYING`
- `filezilla.exe --version` → `FileZilla 3.71.1, compiled on 2026-11-02`
- 实际启动 GUI：窗口标题 `FileZilla`，进程响应正常
- 包 sha256 `0c2bb0289bdf21c1efe2924190124b5f1d815640388cadfaf3557a1c40a63546`（见 `dist/*.sha256`）

## 6. 备注

- 只构建 `ucrt64`；`clang64` / `clangarm64` 未构建（可用 `MINGW_ARCH="ucrt64 clang64"`）
- `check()` 未运行（`--nocheck`）；PKGBUILD 里它本身是 `|| true`
- 包 `Packager` 为 `Unknown Packager`（PKGBUILD 未设 `PACKAGER`），不影响安装

---

## 云存储协议（8 个，均已启用）

`PKGBUILD` 的 configure 现在带这些开关（默认全开）：

`--enable-s3`、`--enable-b2`、`--enable-azureblob`、`--enable-gcs`、
`--enable-gdrive`、`--enable-onedrive`、`--enable-dropbox`、`--enable-box`

对应的自测程序随 `make check` 构建（各自只链 libfilezilla，秒级）：

```bash
MINGW_ARCH=ucrt64 makepkg-mingw -s --noconfirm --nocheck   # 打包
# 或在 build 目录里单独跑协议自测：
cd build-UCRT64/tests && for t in s3 b2 azure gcs drive graph dropbox box; do make ${t}selftest.exe && ./${t}selftest.exe; done
```

各协议状态、已知限制见 `docs/PROTOCOL-STATUS.md` 与 `docs/PARKED.md`，
我们对上游源码的每一处改动见 `docs/upstream-hooks.md`。