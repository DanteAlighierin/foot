# Installing

1. [Overview](#overview)
1. [Requirements](#requirements)
   1. [Running](#running)
   1. [Building](#building)
1. [Arch Linux](#arch-linux)
1. [Other](#other)
   1. [Setup](#setup)
   1. [Options](#options)
   1. [Release build](#release-build)
      1. [Size optimized](#size-optimized)
      1. [Performance optimized, non-PGO](#performance-optimized-non-pgo)
      1. [Performance optimized, PGO](#performance-optimized-pgo)
         1. [Partial PGO](#partial-pgo)
         1. [Full PGO](#full-pgo)
         1. [Use the generated PGO data](#use-the-generated-pgo-data)
      1. [Profile Guided Optimization](#profile-guided-optimization)
   1. [Debug build](#debug-build)
   1. [Running the new build](#running-the-new-build)


## Overview

foot makes use of a couple of libraries I have developed:
[tllist](https://codeberg.org/dnkl/tllist) and
[fcft](https://codeberg.org/dnkl/fcft). As such, they will most likely
not have been installed already. You can either install them as system
libraries or build them as _subprojects_ in foot.

When building foot, they will first be searched for as system
libraries. If **found**, foot will link dynamically against them.
If **not** found, meson will attempt to download and build them as
subprojects.


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

To build, first, create a build directory, and switch to it:
```sh
mkdir -p bld/release && cd bld/release
```

### Options

Available compile-time options:

| Option   | Type | Default | Description         | Extra dependencies |
|----------|------|---------|---------------------|--------------------|
| `-Dime`  | bool | `true`  | Enables IME support | None               |


### Release build

Below are instructions for building foot either [size
optimized](#size-optimized), [performance
optimized](performance-optimized-non-pgo), or performance
optimized using [PGO](#performance-optimized-pgo).

PGO - _Profile Guided Optimization_ - is a way to optimize a program
better than `-O3` can, and is done by compiling foot twice: first to
generate an instrumented version which is used to run a payload that
exercises the performance critical parts of foot, and then a second
time to rebuild foot using the generated profiling data to guide
optimization.

In addition to being faster, PGO builds also tend to be smaller than
regular `-O3` builds.


#### Size optimized

To optimize for size (i.e. produce a small binary):

```sh
export CFLAGS="$CFLAGS -Os -march=native"
meson --buildtype=release --prefix=/usr -Db_lto=true ../..
ninja
ninja test
ninja install
```

#### Performance optimized, non-PGO

To do a regular, non-PGO build optimized for performance:

```sh
export CFLAGS="$CFLAGS -O3 -march=native"
meson --buildtype=release --prefix=/usr -Db_lto=true ../..
ninja
ninja test
ninja install
```

Use `-O2` instead of `-O3` if you prefer a slightly smaller (and
slower!) binary.


#### Performance optimized, PGO

First, configure the build directory:

```sh
export CFLAGS="$CFLAGS -O3 -march=native -Wno-missing-profile"
meson --buildtype=release --prefix=/usr -Db_lto=true ../..
```

It is **very** important `-O3` is being used here, as GCC-10.1.x and
later have a regression where PGO with `-O2` is **much** slower.

If you are using Clang instead of GCC, use the following `CFLAGS` instead:

```sh
export CFLAGS="$CFLAGS -O3 -march=native \
    -Wno-ignored-optimization-argument \
    -Wno-profile-instr-out-of-date \
    -Wno-profile-instr-unprofiled"
```

Then, tell meson we want to _generate_ profiling data, and build:

```sh
meson configure -Db_pgo=generate
ninja
```

Next, we need to actually generate the profiling data.

There are two ways to do this: a [partial PGO build using a PGO
helper](#partial-pgo) binary, or a [full PGO build](#full-pgo) by
running the real foot binary. The latter has slightly better results
(i.e. results in a faster binary), but must be run in a Wayland
session.

A full PGO build also tends to be smaller than a partial build.


##### Partial PGO

This method uses a PGO helper binary that links against the VT parser
only. It is similar to a mock test; it instantiates a dummy terminal
instance and then directly calls the VT parser with stimuli.

It explicitly does **not** include the Wayland backend and as such, it
does not require a running Wayland session. The downside is that not
all code paths in foot is exercised. In particular, the **rendering**
code is not. As a result, the final binary built using this method is
slightly slower than when doing a [full PGO](#full-pgo) build.

We will use the `pgo` binary along with input corpus generated by
`scripts/generate-alt-random-writes.py`:

```sh
tmp_file=$(mktemp)
../../scripts/generate-alt-random-writes \
    --rows=67 \
    --cols=135 \
    --scroll \
    --scroll-region \
    --colors-regular \
    --colors-bright \
    --colors-256 \
    --colors-rgb \
    --attr-bold \
    --attr-italic \
    --attr-underline \
    --sixel \
    ${tmp_file}
./pgo ${tmp_file} ${tmp_file} ${tmp_file}
rm ${tmp_file}
```

The snippet above first creates an (empty) temporary file. Then, it
runs a script that generates random escape sequences (if you cat
`${tmp_file}` in a terminal, you’ll see random colored characters all
over the screen). Finally, we feed the randomly generated escape
sequences to the PGO helper. This is what generates the profiling data
used in the next step.

You are now ready to [use the generated PGO
data](#use-the-generated-pgo-data).


##### Full PGO

This method requires a running Wayland session.

We will use the script `scripts/generate-alt-random-writes.py`:

```sh
foot_tmp_file=$(mktemp)
./foot --config=/dev/null --term=xterm sh -c "<path-to-generate-alt-random-writes.py> --scroll --scroll-region --colors-regular --colors-bright --colors-256 --colors-rgb --attr-bold --attr-italic --attr-underline ${foot_tmp_file} && cat ${foot_tmp_file}"
rm ${foot_tmp_file}
```

You should see a foot window open up, with random colored text. The
window should close after ~1-2s.


##### Use the generated PGO data

Now that we have _generated_ PGO data, we need to rebuild foot. This
time telling meson (and ultimately gcc/clang) to _use_ the PGO data.

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
