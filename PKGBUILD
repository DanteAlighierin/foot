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
  meson --prefix=/usr --buildtype=release -Db_lto=true -Dc_args="-fno-stack-protector" -Db_pgo=generate ..
  ninja

  # TODO: run something to actually create a profile

  meson --prefix=/usr --buildtype=release -Db_lto=true -Dc_args="-fno-stack-protector" -Db_pgo=use ..
  ninja
}

package() {
  DESTDIR="${pkgdir}/" ninja install
}
