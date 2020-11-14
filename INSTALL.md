# Installing

1. [Overview](#overview)
1. [Requirements](#requirements)
   1. [Running](#running)
   1. [Building](#building)
1. [Arch Linux](#arch-linux)
1. [Other](#other)
   1. [Setup](#setup)
   1. [Release build](#release-build)
      1. [Profile Guided Optimization](#profile-guided-optimization)
   1. [Debug build](#debug-build)
   1. [Running the new build](#running-the-new-build)


## Overview

foot makes use of a couple of libraries I have developed:
[tllist](https://codeberg.org/dnkl/tllist) and
[fcft](https://codeberg.org/dnkl/fcft). As such, they will most likely
not have been installed already. You can either install them as system
libraries, or you can build them as _subprojects_ in foot.

When building foot, they will first be searched for as system
libraries. If **found**, foot will link dynamically against them.

If **not** found, they will be searched for as subprojects. In this
case you need to create the `subprojects` directory and clone
https://codeberg.org/dnkl/fcft.git and
https://codeberg.org/dnkl/tllist.git (see [Other](#other) below).


## Requirements

### Running

* fontconfig
* freetype
* pixman
* wayland (_client_ and _cursor_ libraries)
* xkbcommon
* [fcft](https://codeberg.org/dnkl/fcft) [^1]

[^1]: can also be built as subprojects, in which case they are
    statically linked.


### Building

In addition to the dev variant of the packages above, you need:

* meson
* ninja
* wayland protocols
* ncurses (needed to generate terminfo)
* scdoc (for man page generation)
* [tllist](https://codeberg.org/dnkl/tllist) [^1]

A note on compilers; in general, foot runs **much** faster when
compiled with gcc instead of clang. A profile-guided gcc build can be
more than twice as fast as a clang build.

**Note** GCC 10.1 has a performance regression that severely affects
foot when doing PGO builds and building with `-O2`; it is about 30-40%
slower compared to GCC 9.3.

The work around is simple: make sure you build with `-O3`. This is the
default with `meson --buildtype=release`, but e.g. `makepkg` can
override it (`makepkg` uses `-O2` by default).


## Arch Linux

Install from AUR:

* [foot](https://aur.archlinux.org/packages/foot/) +
  [foot-terminfo](https://aur.archlinux.org/packages/foot-terminfo/)
* [foot-git](https://aur.archlinux.org/packages/foot-git/) +
  [foot-terminfo-git](https://aur.archlinux.org/packages/foot-terminfo-git/)

Or use [makepkg](https://wiki.archlinux.org/index.php/Makepkg) to
build the bundled [PKGBUILD](PKGBUILD) (run `makepkg` in the source
root directory).

Unlike the AUR packages, the bundled PKGBUILD **requires**
[tllist](https://codeberg.org/dnkl/tllist) and
[fcft](https://codeberg.org/dnkl/fcft) to be installed as system
libraries. If you do not want this, please edit the PKGBUILD file, or
install manually (see [Other](#other) below).

Note that it will do a profiling-guided build, and that this requires
a running wayland session since it needs to run an intermediate build
of foot.


## Other

Foot uses _meson_. If you are unfamiliar with it, the official
[tutorial](https://mesonbuild.com/Tutorial.html) might be a good
starting point.

I also recommend taking a look at the bundled Arch
[PKGBUILD](PKGBUILD) file, to see how it builds foot. Especially so if
you intend to install a release build of foot, in which case you might
be interested in the compiler flags used there.


### Setup

If you have not installed [tllist](https://codeberg.org/dnkl/tllist)
and [fcft](https://codeberg.org/dnkl/fcft) as system libraries, clone
them into the `subprojects` directory:

```sh
mkdir -p subprojects
pushd subprojects
git clone https://codeberg.org/dnkl/tllist.git
git clone https://codeberg.org/dnkl/fcft.git
popd
```

To build, first, create a build directory, and switch to it:
```sh
mkdir -p bld/release && cd bld/release
```

### Release build

```sh
export CFLAGS="$CFLAGS -O3 -march=native"
meson --buildtype=release --prefix=/usr -Db_lto=true ../..
```
Both `-O3` (in `CFLAGS`) and `-Db_lto=true` are **highly** recommended.

For performance reasons, I strongly recommend doing a
[PGO](#profile-guided-optimization) (Profile Guided Optimization)
build. This requires a running Wayland session since we will be
executing an intermediate build of foot.

If you do not want this, just build:

```sh
ninja
```

and then skip to [Running the new build](#running-the-new-build).

**For packagers**: normally, you would configure compiler flags using
`-Dc_args`. This however "overwrites" `CFLAGS`. `makepkg` from Arch,
for example, uses `CFLAGS` to specify the default set of flags.

Thus, we do `export CFLAGS+="..."` to at least not throw away those
flags.

When packaging, you may want to use the default `CFLAGS` only, but
note this: foot is a performance critical application that relies on
compiler optimizations to perform well.

In particular, with GCC 10.1, it is **very** important `-O3` is used
(and not e.g. `-O2`) when doing a [PGO](#profile-guided-optimization)
build.


#### Profile Guided Optimization

First, make sure you have configured a [release](#release-build) build
directory, but:

If using Clang, make sure to add `-Wno-ignored-optimization-argument
-Wno-profile-instr-out-of-date` to `CFLAGS`.

If using GCC, make sure to add `-Wno-missing-profile` to `CFLAGS`.

Then, tell meson we want to _generate_ profile data, and build:

```sh
meson configure -Db_pgo=generate
ninja
```

Next, we need to execute the intermediate build of foot, and run a
payload inside it that will exercise the performance critical code
paths. To do this, we will use the script
`scripts/generate-alt-random-writes.py`:

```sh
foot_tmp_file=$(mktemp)
./foot --config=/dev/null --term=xterm sh -c "<path-to-generate-alt-random-writes.py> --scroll --scroll-region --colors-regular --colors-bright --colors-rgb ${foot_tmp_file} && cat ${foot_tmp_file}"
rm ${foot_tmp_file}
```

You should see a foot window open up, with random colored text. The
window should close after ~1-2s.

If using Clang, now do (this requires _llvm_ to have been installed):

```sh
llvm-profdata merge default_*profraw --output=default.profdata
```

Next, tell meson to _use_ the profile data we just generated, and rebuild:

```sh
meson configure -Db_pgo=use
ninja
```

Continue reading in [Running the new build](#running-the-new-build)

### Debug build

```sh
meson --buildtype=debug ../..
ninja
```

### Running the new build

You can now run it directly from the build directory:
```sh
./foot
```

But note that it will default to `TERM=foot`, and that this terminfo
has not been installed yet. However, most things should work with the
`xterm-256color` terminfo:
```sh
./foot --term xterm-256color
```

But, I **recommend** you install the `foot` and `foot-direct` terminfo
files. You can either copy them manually (typically to
`/usr/share/terminfo/f` - but this depends on the distro), or
just install everything:
```sh
ninja install
```
