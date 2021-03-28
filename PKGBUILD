pkgname=('foot-git' 'foot-terminfo-git')
pkgver=1.7.0
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
  local compiler=other
  local do_pgo=no

  # makepkg uses -O2 by default, but we *really* want -O3
  CFLAGS+=" -O3"

  # Figure out which compiler we're using, and whether or not to do PGO
  case $(${CC-cc} --version) in
    *GCC*)
      compiler=gcc
      do_pgo=yes
      ;;

    *clang*)
      compiler=clang

      # We need llvm to be able to manage the profiling data
      if command -v llvm-profdata > /dev/null; then
        do_pgo=yes

        # Meson adds -fprofile-correction, which Clang doesn't
        # understand
        CFLAGS+=" -Wno-ignored-optimization-argument"
      fi
      ;;
  esac

  meson --prefix=/usr --buildtype=release --wrap-mode=nofallback -Db_lto=true ..

  if [[ ${do_pgo} == yes ]]; then
    find -name "*.gcda" -delete
    meson configure -Db_pgo=generate
    ninja

    local script_options="--scroll --scroll-region --colors-regular --colors-bright --colors-256 --colors-rgb --attr-bold --attr-italic --attr-underline --sixel"

    tmp_file=$(mktemp)

    if [[ -v WAYLAND_DISPLAY ]]; then
      ./footclient --version
      ./foot \
        --config /dev/null \
        --term=xterm \
        sh -c "../scripts/generate-alt-random-writes.py ${script_options} ${tmp_file} && cat ${tmp_file}"
    else
      ./footclient --version
      ./foot --version
      ../scripts/generate-alt-random-writes.py \
        --rows=67 \
        --cols=135 \
        ${script_options} \
        ${tmp_file}
      ./pgo ${tmp_file} ${tmp_file} ${tmp_file}
    fi

    rm "${tmp_file}"

    if [[ ${compiler} == clang ]]; then
      llvm-profdata merge default_*profraw --output=default.profdata
    fi

    meson configure -Db_pgo=use
  fi

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
