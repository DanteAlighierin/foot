# Installing

1. [Overview](#overview)
1. [Requirements](#requirements)
   1. [Running](#running)
   1. [Building](#building)
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
   1. [Terminfo](#terminfo)
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

* UTF-8 locale
* fontconfig
* freetype
* pixman
* wayland (_client_ and _cursor_ libraries)
* xkbcommon
* [fcft](https://codeberg.org/dnkl/fcft) [^1]

[^1]: can also be built as subprojects, in which case they are
    statically linked.

If you are packaging foot, you may also want to consider adding the
following **optional** dependencies:

* libnotify: desktop notifications by default uses `notify-send`.
* xdg-utils: URLs are by default launched with `xdg-open`.
* bash-completion: If you want completion for positional arguments.

### Building

In addition to the dev variant of the packages above, you need:

* meson
* ninja
* wayland protocols
* ncurses (needed to generate terminfo)
* scdoc (for man page generation, not needed if documentation is disabled)
* llvm (for PGO builds with Clang)
* [tllist](https://codeberg.org/dnkl/tllist) [^1]
* systemd (optional, foot will install systemd unit files if detected)

A note on compilers; in general, foot runs **much** faster when
compiled with gcc instead of clang. A profile-guided gcc build can be
more than twice as fast as a clang build.

**Note** GCC 10.1 has a performance regression that severely affects
foot when doing PGO builds and building with `-O2`; it is about 30-40%
slower compared to GCC 9.3.

The work around is simple: make sure you build with `-O3`. This is the
default with `meson --buildtype=release`, but e.g. `makepkg` can
override it (`makepkg` uses `-O2` by default).

## Other

Foot uses _meson_. If you are unfamiliar with it, the official
[tutorial](https://mesonbuild.com/Tutorial.html) might be a good
starting point.

A note on terminfo; the terminfo database exposes terminal
capabilities to the applications running inside the terminal. As such,
it is important that the terminfo used reflects the actual
terminal. Using the `xterm-256color` terminfo will, in many cases,
work, but I still recommend using foot’s own terminfo. There are two
reasons for this:

* foot’s terminfo contains a couple of non-standard capabilities,
  used by e.g. tmux.
* New capabilities added to the `xterm-256color` terminfo could
  potentially break foot.
* There may be future additions or changes to foot’s terminfo.

As of ncurses 2021-07-31, ncurses includes a version of foot’s
terminfo. **The recommendation is to use those**, and only install the
terminfo definitions from this git repo if the system’s ncurses
predates 2021-07-31.

But, note that the foot terminfo definitions in ncurses’ lack the
non-standard capabilities. This mostly affects tmux; without them,
`terminal-overrides` must be configured to enable truecolor
support. For this reason, it _is_ possible to install “our” terminfo
definitions as well, either in a non-default location, or under a
different name.

Both have their set of issues. When installing to a non-default
location, foot will set the environment variable `TERMINFO` in the
child process. However, there are many situations where this simply
does not work. See https://codeberg.org/dnkl/foot/issues/695 for
details.

Installing them under a different name generally works well, but will
break applications that check if `$TERM == foot`.

Hence the recommendation to simply use ncurses’ terminfo definitions
if available.

If packaging “our” terminfo definitions, I recommend doing that as a
separate package, to allow them to be installed on remote systems
without having to install foot itself.


### Setup

To build, first, create a build directory, and switch to it:
```sh
mkdir -p bld/release && cd bld/release
```

### Options

Available compile-time options:

| Option                               | Type    | Default                 | Description                                           | Extra dependencies |
|--------------------------------------|---------|-------------------------|-------------------------------------------------------|--------------------|
| `-Ddocs`                             | feature | `auto`                  | Builds and install documentation                      | scdoc              |
| `-Dtests`                            | bool    | `true`                  | Build tests (adds a `ninja test` build target)        | none               |
| `-Dime`                              | bool    | `true`                  | Enables IME support                                   | None               |
| `-Dgrapheme-clustering`              | feature | `auto`                  | Enables grapheme clustering                           | libutf8proc        |
| `-Dterminfo`                         | feature | `enabled`               | Build and install terminfo files                      | tic (ncurses)      |
| `-Ddefault-terminfo`                 | string  | `foot`                  | Default value of `TERM`                               | none               |
| `-Dcustom-terminfo-install-location` | string  | `${datadir}/terminfo`   | Value to set `TERMINFO` to                            | None               |
| `-Dsystemd-units-dir`                | string  | `${systemduserunitdir}` | Where to install the systemd service files (absolute) | None               |

Documentation includes the man pages, readme, changelog and license
files.

`-Ddefault-terminfo`: I strongly recommend leaving the default
value. Use this option if you plan on installing the terminfo files
under a different name. Setting this changes the default value of
`$TERM`, and the names of the terminfo files (if
`-Dterminfo=enabled`).

`-Dcustom-terminfo-install-location` enables foot’s terminfo to
co-exist with ncurses’ version, without changing the terminfo
names. The idea is that you install foot’s terminfo to a non-standard
location, for example `/usr/share/foot/terminfo`. Use
`-Dcustom-terminfo-install-location` to tell foot where the terminfo
is. Foot will set the environment variable `TERMINFO` to this value
(with `${prefix}` added). The value is **relative to ${prefix}**.

Note that there are several issues with this approach:
https://codeberg.org/dnkl/foot/issues/695.

If left unset, foot will **not** set or modify `TERMINFO`.

`-Dterminfo` can be used to disable building the terminfo definitions
in the meson build. It does **not** change the default value of
`TERM`, and it does **not** disable `TERMINFO`, if
`-Dcustom-terminfo-install-location` has been set. Use this if
packaging the terminfo definitions in a separate package (and the
build script isn’t shared with the ‘foot’ package).

Example:

```sh
meson --prefix=/usr -Dcustom-terminfo-install-location=lib/foot/terminfo
```

The above tells foot its terminfo definitions will be installed to
`/usr/lib/foot/terminfo`. This is the value foot will set the
`TERMINFO` environment variable to.

If `-Dterminfo` is enabled (the default), then the terminfo files will
be built as part of the regular build process, and installed to the
specified location.

Packagers may want to set `-Dterminfo=disabled`, and manually build
and [install the terminfo](#terminfo) files instead.


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
export CFLAGS="$CFLAGS -Os"
meson --buildtype=release --prefix=/usr -Db_lto=true ../..
ninja
ninja test
ninja install
```

#### Performance optimized, non-PGO

To do a regular, non-PGO build optimized for performance:

```sh
export CFLAGS="$CFLAGS -O3"
meson --buildtype=release --prefix=/usr -Db_lto=true ../..
ninja
ninja test
ninja install
```

Use `-O2` instead of `-O3` if you prefer a slightly smaller (and
slower!) binary.


#### Performance optimized, PGO

There are a lot more steps involved in a PGO build, and for this
reason there are a number of helper scripts available.

`pgo/pgo.sh` is a standalone script that pieces together the other
scripts in the `pgo` directory to do a complete PGO build. This script
is intended to be used when doing manual builds.

Note that all “full” PGO builds (which `auto` will prefer, if
possible) **require** `LC_CTYPE` to be set to an UTF-8 locale. This is
**not** done automatically.

Example:

```sh
cd foot
./pgo/pgo.sh auto . /tmp/foot-pgo-build-output
```

(run `./pgo/pgo.sh` to get help on usage)

It supports a couple of different PGO builds; partial (covered in
detail below), full (also covered in detail below), and (full)
headless builds using Sway or cage.

Packagers may want to use it as inspiration, but may choose to support
only a specific build type; e.g. full/headless with Sway.

To do a manual PGO build, instead of using the script(s) mentioned
above, detailed instructions follows:

First, configure the build directory:

```sh
export CFLAGS="$CFLAGS -O3"
meson --buildtype=release --prefix=/usr -Db_lto=true ../..
```

It is **very** important `-O3` is being used here, as GCC-10.1.x and
later have a regression where PGO with `-O2` is **much** slower.

Clang users **must** add `-Wno-ignored-optimization-argument` to
`CFLAGS`.

Then, tell meson we want to _generate_ profiling data, and build:

```sh
meson configure -Db_pgo=generate
ninja
ninja test
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
./footclient --version
./foot --version
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

The first step, running `./foot --version` and `./footclient
--version` might seem unnecessary, but is needed to ensure we have
_some_ profiling data for functions not covered by the PGO helper
binary. Without this, the final link phase will fail.

The snippet above then creates an (empty) temporary file. Then, it
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
./footclient --version
foot_tmp_file=$(mktemp)
./foot \
    --config=/dev/null \
    --override tweak.grapheme-shaping=no \
    --term=xterm \
    sh -c "<path-to-generate-alt-random-writes.py> --scroll --scroll-region --colors-regular --colors-bright --colors-256 --colors-rgb --attr-bold --attr-italic --attr-underline --sixel ${foot_tmp_file} && cat ${foot_tmp_file}"
rm ${foot_tmp_file}
```

You should see a foot window open up, with random colored text. The
window should close after ~1-2s.

The first step, `./footclient --version` might seem unnecessary, but
is needed to ensure we have _some_ profiling data for
`footclient`. Without this, the final link phase will fail.


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
ninja test
```

Continue reading in [Running the new build](#running-the-new-build)


### Debug build

```sh
meson --buildtype=debug ../..
ninja
ninja test
```

### Terminfo

By default, building foot also builds the terminfo files. If packaging
the terminfo files in a separate package, it might be easier to simply
disable the terminfo files in the regular build, and compile the
terminfo files manually instead.

To build the terminfo files, run:

```sh
sed 's/@default_terminfo@/foot/g' foot.info | \
    tic -o <output-directory> -x -e foot,foot-direct -
```

Where _”output-directory”_ **must** match the value passed to
`-Dcustom-terminfo-install-location` in the foot build. If
`-Dcustom-terminfo-install-location` has not been set, `-o
<output-directoty>` can simply be omitted.

Or, if packaging:

```sh
tic -o ${DESTDIR}/usr/share/terminfo ...
```


### Running the new build

You can now run it directly from the build directory:
```sh
./foot
```

Or, if you did not install the terminfo definitions:

```sh
./foot --term xterm-256color
```
