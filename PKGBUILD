pkgname=('foot' 'foot-terminfo')
pkgver=0.0.r386.gce4d2a0
pkgrel=1
arch=('x86_64')
url=https://gitlab.com/dnkl/foot
license=(mit)
makedepends=('meson' 'ninja' 'scdoc')
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

package_foot() {
  pkgdesc="A wayland native terminal emulator"
  depends=(
    'libxkbcommon'
    'wayland'
    'freetype2' 'fontconfig' 'pixman')

  DESTDIR="${pkgdir}/" ninja install
  rm -rf "${pkgdir}/usr/share/terminfo"
}

package_foot-terminfo() {
  pkgdesc="Terminfo files for the foot terminal emulator"

  install -dm 755 "${pkgdir}/usr/share/terminfo/f/"
  cp f/* "${pkgdir}/usr/share/terminfo/f/"
}
