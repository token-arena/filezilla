# Maintainer: Andrew Sun <adsun701@gmail.com>
#
# 本仓库变体（二次开发用）：
#   * 源码树直接入库在 ./filezilla/（目录名不带版本号，便于长期迭代）
#   * 上游 MSYS2 的两个补丁已应用在 ./filezilla/ 里，原文件留档在 ./patches/
#   * PKGBUILD 不再从网上下载 tar.xz，也不再重复打补丁；prepare() 为空
#   * 构建产物仍由 makepkg 管理（./build-${MSYSTEM}/、./src/、./pkg/）
#
# 日常流程：
#   改 ./filezilla/ 下的代码 -> 在 MSYS2 UCRT64 终端执行
#     MINGW_ARCH=ucrt64 makepkg-mingw -s --noconfirm --nocheck
#   生成的包在仓库根目录（.pkg.tar.zst）。
#   改了 configure.ac / Makefile.am 时先手动 `cd filezilla && autoreconf -fiv`。

_realname=filezilla
_wx_basever=3.2
pkgbase="mingw-w64-${_realname}"
pkgname=("${MINGW_PACKAGE_PREFIX}-${_realname}")
# 与 ./filezilla/configure.ac 中的 FILEZILLA_VERSION 保持一致
pkgver=3.71.1
pkgrel=1
pkgdesc="Fast and reliable FTP, FTPS and SFTP client (mingw-w64)"
arch=('any')
mingw_arch=('ucrt64' 'clang64' 'clangarm64')
url="https://filezilla-project.org/"
msys2_repository_url="https://svn.filezilla-project.org/filezilla/FileZilla3/"
msys2_documentation_url="https://wiki.filezilla-project.org/Documentation"
msys2_references=(
  'archlinux: filezilla'
  "cpe: cpe:/a:filezilla:filezilla"
  "cpe: cpe:/a:filezilla-project:filezilla_client"
)
license=('spdx:GPL-2.0-or-later')
depends=(
  "${MINGW_PACKAGE_PREFIX}-cc-libs"
  "${MINGW_PACKAGE_PREFIX}-fzssh"
  "${MINGW_PACKAGE_PREFIX}-gnutls"
  "${MINGW_PACKAGE_PREFIX}-libfilezilla"
  "${MINGW_PACKAGE_PREFIX}-sqlite3"
  "${MINGW_PACKAGE_PREFIX}-wxwidgets${_wx_basever}-msw-libs"
)
makedepends=(
  "${MINGW_PACKAGE_PREFIX}-boost"
  "${MINGW_PACKAGE_PREFIX}-cc"
  "${MINGW_PACKAGE_PREFIX}-autotools"
  "${MINGW_PACKAGE_PREFIX}-wxwidgets${_wx_basever}-msw"
)

# 源码树来自仓库本身，无需下载
source=()
sha256sums=()

# 仓库内的源码树（makepkg 把 build()/package() 的 cwd 设为 $srcdir，故用绝对路径）
_srctree="${startdir}/${_realname}"

prepare() {
  # 两个 MSYS2 补丁与 autoreconf 生成物（configure、Makefile.in 等）均已入库，
  # 这里无需任何操作。若修改了 configure.ac / Makefile.am，请自行重新生成：
  #   ( cd "${_srctree}" && autoreconf -fiv )
  :
}

build() {
  cd "${startdir}"
  mkdir -p "build-${MSYSTEM}" && cd "build-${MSYSTEM}"

  if [[ ${MINGW_PACKAGE_PREFIX} != *-clang-* ]]; then
    CFLAGS+=" -Wno-incompatible-pointer-types"
    CXXFLAGS+=" -Wno-incompatible-pointer-types"
  fi

  "${_srctree}/configure" \
    --prefix="${MINGW_PREFIX}" \
    --build="${MINGW_CHOST}" \
    --host="${MINGW_CHOST}" \
    --target="${MINGW_CHOST}" \
    --disable-manualupdatecheck \
    --disable-autoupdatecheck \
    --with-pugixml=builtin \
    --enable-s3 \
    --enable-b2 \
    --enable-azureblob \
    --enable-gcs \
    --enable-gdrive \
    --enable-onedrive \
    --enable-dropbox \
    --enable-oss \
    --with-wx-config="${MINGW_PREFIX}/bin/wx-config-${_wx_basever}"

  make
}

check() {
  cd "${startdir}/build-${MSYSTEM}"

  # LANG needed to fix string_test::test_conversion2
  LANG=en_US.UTF-8 make check || true
}

package() {
  cd "${startdir}/build-${MSYSTEM}"

  make install DESTDIR="${pkgdir}"

  install -Dm644 "${_srctree}/COPYING" -t "${pkgdir}${MINGW_PREFIX}/share/licenses/${_realname}"
}
