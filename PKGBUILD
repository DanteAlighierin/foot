pkgname=('foot-git' 'foot-terminfo-git')
pkgver=1.5.4
pkgrel=1
arch=('x86_64' 'aarch64')
url=https://codeberg.org/dnkl/foot
license=(mit)
depends=('libxkbcommon' 'wayland' 'pixman' 'fontconfig' 'fcft>=2.3.0')
makedepends=('meson' 'ninja' 'scdoc' 'python' 'ncurses' 'wayland-protocols' 'tllist>=1.0.4')
source=()

pkgver() {
  cd ../.git &> /dev/null && git describe --tags --long | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g' ||
      head -3 ../meson.build | grep version | cut -d "'" -f 2
}

build() {
  # makepkg uses -O2 by default, but we *really* want -O3
  # -Wno-missing-profile since we're not exercising everything when doing PGO builds
  export CFLAGS+=" -O3 -Wno-missing-profile"

  meson --prefix=/usr --buildtype=release --wrap-mode=nofallback -Db_lto=true ..

  find -name "*.gcda" -delete
  meson configure -Db_pgo=generate
  ninja

  script_options="--scroll --scroll-region --colors-regular --colors-bright --colors-256 --colors-rgb --attr-bold --attr-italic --attr-underline --sixel"

  tmp_file=$(mktemp)

  if [[ -v WAYLAND_DISPLAY ]]; then
    ./foot \
      --config /dev/null \
      --term=xterm \
      sh -c "../scripts/generate-alt-random-writes.py ${script_options} ${tmp_file} && cat ${tmp_file}"
  else
    ../scripts/generate-alt-random-writes.py \
      --rows=67 \
      --cols=135 \
      ${script_options} \
      ${tmp_file}
    ./pgo ${tmp_file} ${tmp_file} ${tmp_file}
  fi

  rm "${tmp_file}"

  meson configure -Db_pgo=use
  ninja
}

check() {
  ninja test
}

package_foot-git() {
  pkgdesc="A wayland native terminal emulator"
  changelog=CHANGELOG.md
  depends+=('foot-terminfo')
  conflicts=('foot')
  provides=('foot')

  DESTDIR="${pkgdir}/" ninja install
  rm -rf "${pkgdir}/usr/share/terminfo"
}

package_foot-terminfo-git() {
  pkgdesc="Terminfo files for the foot terminal emulator"
  depends=('ncurses')
  conflicts=('foot-terminfo')
  provides=('foot-terminfo')

  install -dm 755 "${pkgdir}/usr/share/terminfo/f/"
  cp f/* "${pkgdir}/usr/share/terminfo/f/"
}
