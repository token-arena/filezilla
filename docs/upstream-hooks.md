# 我们对上游源码树做的改动（升级上游时必须逐条重放）

本仓库把 FileZilla 源码树放进 `filezilla/`（见 `BUILD-NOTES.md`），并在此之上开发
云存储协议。为了让「升级上游」可重复，这里记录我们对**上游原有文件**的每一处改动。
新增文件（`src/engine/s3/`、`tests/s3selftest.cpp`、`docs/`）不在本清单内。

## 1. `configure.ac`

- 新增协议开关（紧跟 storj 块之后）：

```sh
  AC_ARG_ENABLE(s3, AS_HELP_STRING([--enable-s3@<:@=ARG@:>@],[Enable support for Amazon S3 and S3-compatible object storage. Default: no]), [], [ enable_s3="no" ])
  if test "$enable_s3" != "no"; then
    AC_DEFINE([ENABLE_S3], [1], [Define if building with Amazon S3 support.])
    enable_s3="yes"
  fi
```

- 条件变量（`AM_CONDITIONAL(ENABLE_STORJ, ...)` 之后）：

```sh
AM_CONDITIONAL(ENABLE_S3, [test "x$enable_s3" = "xyes"])
```

## 2. `src/engine/Makefile.am`

在 `if ENABLE_STORJ` 之前插入：

```make
if ENABLE_S3
libfzclient_private_la_SOURCES += \
		s3/s3controlsocket.cpp \
		s3/s3response.cpp \
		s3/s3signer.cpp

noinst_HEADERS += \
		s3/s3controlsocket.h \
		s3/s3response.h \
		s3/s3signer.h
endif
```

## 3. `src/engine/engineprivate.cpp`

- 头文件区，`sftp` 之前：

```cpp
#if ENABLE_S3
#include "s3/s3controlsocket.h"
#endif
```

- `switch (server.GetProtocol())` 里，`default:` 之前：

```cpp
#if ENABLE_S3
	case S3:
	case CLOUDFLARE_R2:
		controlSocket_ = std::make_unique<CS3ControlSocket>(*this);
		break;
#endif
```

## 4. `tests/Makefile.am`

- 顶部加 `AUTOMAKE_OPTIONS = subdir-objects`（自测程序要编译 `../src/engine/s3/*.cpp`）
- 新增 `if ENABLE_S3 … MAYBE_S3_SELFTEST = s3selftest`，并入 `TESTS`
- 新增 `s3selftest` 程序（只链接 libfilezilla，秒级构建）

## 5. `locales/Makefile.am`（Windows 构建必须）

`filezilla.pot` 规则原来把所有源文件路径铺在命令行上，Win32 有 32 KB 参数上限，
新增文件后会报 `Argument list too long`。改为经 `--files-from` 传文件列表：

```make
	FILES="`find $(srcdir)/../src -name '*.h' -o -name '*.cpp'`"; \
	echo "$$FILES" > $@.files; \
	$(XGETTEXT) $(XGETTEXT_ARGS) -f $@.files -o $@.new || exit 1; \
	rm -f $@.files
```

## 6. GPU/其它

暂无。`src/engine/server.cpp` 的协议元数据表上游已包含所有云协议，无需改动。

## 7. Backblaze B2（F2）

- configure.ac：新增 `--enable-b2`（默认 no）→ `AC_DEFINE([ENABLE_B2])`，以及
  `AM_CONDITIONAL(ENABLE_B2, [test "x$enable_b2" = "xyes"])`
- `src/engine/Makefile.am`：`if ENABLE_B2` 段（`b2/b2controlsocket.cpp`、`b2/b2json.cpp`）
- `src/engine/engineprivate.cpp`：`#include "b2/b2controlsocket.h"`（ENABLE_B2 保护）+
  `switch` 里 `case B2:` → `CB2ControlSocket`
- `tests/Makefile.am`：`b2selftest` 程序（纯逻辑，只链 libfilezilla）
