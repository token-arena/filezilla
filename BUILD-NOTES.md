# FileZilla 3.71.1 — MSYS2 从零编译打包记录

依据工作目录根下的 `PKGBUILD`（MSYS2 `MINGW-packages` 中 `mingw-w64-filezilla` 的官方 PKGBUILD，
对应上游提交 `442fc22ddf2873a9c46360aa51b094691fa7cb01`）完成取源码、编译、打包。

## 1. 产物

| 文件 | 说明 |
|---|---|
| `dist/mingw-w64-ucrt-x86_64-filezilla-3.71.1-1-any.pkg.tar.zst` | 可安装的 pacman 包，5.40 MiB 压缩 / 17.42 MiB 安装后 |
| `dist/*.pkg.tar.zst.sha256` | `9335b4c250799820ae7d37b5b9ec5393045d52eff89095ae2c6f02177ea231fc` |
| `dist/build-phase1.log` | 第一次 `makepkg-mingw` 全程日志（含依赖安装、取源码、校验、编译） |
| `dist/build-phase2.log` | 续建 + 打包日志 |

安装方式：

```
# 在 MSYS2 UCRT64 终端里
pacman -U mingw-w64-ucrt-x86_64-filezilla-3.71.1-1-any.pkg.tar.zst
# 然后运行 /ucrt64/bin/filezilla.exe
```

## 2. 环境

- Windows 10 IoT LTSC 19044 x64，已装 MSYS2（`C:\msys64`），UCRT64 环境
- 构建前 `pacman -Syu` 升级（`msys2-runtime` 升级会关闭终端，需重跑一次）
- 系统侧工具：`base-devel`
- 依赖包由 `makepkg-mingw -s` 自动解析安装：
  - 运行期：`ucrt-x86_64-{cc-libs,fzssh,gnutls,libfilezilla,sqlite3,wxwidgets3.2-msw-libs}`
  - 构建期：`ucrt-x86_64-{boost,cc(=gcc),autotools,wxwidgets3.2-msw}`
- 关键版本：gcc 16.2.0、wxWidgets 3.2.11、libfilezilla 0.58.0、fzssh 1.5.0、boost 1.92.0

## 3. 步骤

1. 建立打包目录 `build/mingw-w64-filezilla/`，放入 `PKGBUILD`。
2. 补齐 PKGBUILD 引用的两个补丁（PKGBUILD 只列出文件名，需与上游同版本）：
   从 GitHub `msys2/MINGW-packages@442fc22` 取
   `0002-fix-mingw-compiler-detection.patch`、`0003-fix-building-po-files.patch`，
   sha256 与 PKGBUILD 内 `sha256sums` 完全一致。
3. `MSYSTEM=UCRT64 MINGW_ARCH=ucrt64 makepkg-mingw -s --noconfirm --nocheck`
   - 源码：`https://sources.archlinux.org/other/filezilla/filezilla-3.71.1.tar.xz`（sha256 校验通过）
   - `prepare()`：打两个补丁 + `autoreconf -fiv`
   - `build()`：`configure --prefix=/ucrt64 ... --with-wx-config=/ucrt64/bin/wx-config-3.2` + `make`
4. 第一次构建在 `src/fzshellext` 的 Makefile 再生步骤（`aclocal/autoconf/automake` +
   `./config.status`）死锁：`make` 进程 CPU 占用 0、日志停止十几分钟。
   处理：终止后保留已编译树，改用串行 `make -j1` 续建
   （续建用 `PKGBUILD.resume` + `makepkg-mingw -e`，`prepare()` 置空以免重复 autoreconf）。
   该文件保留在 `build/mingw-w64-filezilla/PKGBUILD.resume` 供复现；`PKGBUILD` 已还原为原始内容。
5. `package()`：`make install DESTDIR=$pkgdir` + 安装 COPYING，由 makepkg 生成
   `.PKGINFO`/`.BUILDINFO`/`.MTREE` 并 zstd 压缩。

## 4. 验证

- `pacman -Qip`：`mingw-w64-ucrt-x86_64-filezilla 3.71.1-1`，6 个 depends 全部满足（`pacman -T` 无输出）
- 包内容 943 个文件，含 `ucrt64/bin/filezilla.exe`、`libfzclient-private-3-71-1.dll`、
  `libfzclient-commonui-private-3-71-1.dll`、`libfzshellext-0.dll`、资源图标与 181 个语言包
- `filezilla.exe --version` → `FileZilla 3.71.1, compiled on 2026-11-02`
- 实际启动 GUI：窗口标题 `FileZilla`，进程响应正常（随后手动结束）

## 5. 备注

- 按 PKGBUILD 只构建了 `ucrt64`；`clang64`/`clangarm64` 未构建（可用 `MINGW_ARCH="ucrt64 clang64"` 再跑）。
- `check()` 未运行（`--nocheck`）；PKGBUILD 里它本身是 `|| true`，可另行执行。
- 构建树 `build/mingw-w64-filezilla/` 约 1.26 GB，确认无误后可整目录删除。
- 包内 `Packager` 为 `Unknown Packager`（PKGBUILD 未设 `PACKAGER`），不影响安装。
