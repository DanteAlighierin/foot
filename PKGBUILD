pkgname=('foot' 'foot-terminfo')
pkgver=1.2.1
pkgrel=1
arch=('x86_64')
url=https://codeberg.org/dnkl/foot
license=(mit)
makedepends=('meson' 'ninja' 'scdoc' 'python' 'ncurses' 'wayland-protocols' 'tllist>=1.0.0')
depends=('libxkbcommon' 'wayland' 'pixman' 'fcft>=1.1.1')
source=()

pkgver() {
  cd ../.git &> /dev/null && git describe --tags --long | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g' ||
      head -3 ../meson.build | grep version | cut -d "'" -f 2
}

build() {
  meson --prefix=/usr --buildtype=release --wrap-mode=nofallback -Db_lto=true ..

  if [[ -v WAYLAND_DISPLAY ]]; then
    meson configure -Db_pgo=generate
    find -name "*.gcda" -delete
    ninja

    tmp_file=$(mktemp)
    ./foot --config /dev/null --term=xterm -- sh -c "../scripts/generate-alt-random-writes.py --scroll --scroll-region --colors-regular --colors-bright --colors-rgb ${tmp_file} && cat ${tmp_file}"
    rm "${tmp_file}"

    meson configure -Db_pgo=use
  fi

  ninja
}

check() {
  ninja test
}

package_foot() {
  pkgdesc="A wayland native terminal emulator"
  optdepends=('foot-terminfo: terminfo for foot')

  DESTDIR="${pkgdir}/" ninja install
  rm -rf "${pkgdir}/usr/share/terminfo"
}

package_foot-terminfo() {
  pkgdesc="Terminfo files for the foot terminal emulator"
  depends=('ncurses')

  install -dm 755 "${pkgdir}/usr/share/terminfo/f/"
  cp f/* "${pkgdir}/usr/share/terminfo/f/"
}
