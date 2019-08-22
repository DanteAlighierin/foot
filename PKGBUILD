pkgname=foot
pkgver=0.0.r136.g90d357b
pkgrel=1
pkgdesc="A wayland native terminal emulator"
arch=('x86_64')
url=https://gitlab.com/dnkl/foot
license=(mit)
makedepends=('meson' 'ninja' 'scdoc')
depends=(
  'libxkbcommon'
  'wayland'
  'freetype2' 'fontconfig' 'pixman')
source=()

pkgver() {
  [ -d ../.git ] && git describe --tags --long | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g'
  [ ! -d ../.git ] && head -3 ../meson.build | grep version | cut -d "'" -f 2
}

build() {
  meson --prefix=/usr --buildtype=release -Db_lto=true -Dc_args="-fno-stack-protector" ..

  meson configure -Db_pgo=generate
  ninja

  tmp_file=$(mktemp)
  ./foot -- sh -c "../scripts/generate-alt-random-writes.py --scroll --scroll-region --colors-regular --colors-bright --colors-rgb ${tmp_file} && cat ${tmp_file}"
  rm "${tmp_file}"

  meson configure -Db_pgo=use
  ninja
}

package() {
  DESTDIR="${pkgdir}/" ninja install
}
