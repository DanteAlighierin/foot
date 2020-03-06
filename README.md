# foot

**foot** is a fast Wayland terminal emulator.

## Index

1. [Features](#features)
1. [Non-features](#non-features)
1. [What does not work?](#what-does-not-work)
1. [Fonts](#fonts)
1. [Shortcuts](#shortcuts)
   1. [Keyboard](#keyboard)
      1. [Normal mode](#normal-mode)
      1. [Scrollback search](#scrollback-search)
   1. [Mouse](#mouse)
1. [Server (daemon) mode](#server-daemon-mode)
1. [Alt/meta](#alt-meta)
1. [Backspace](#backspace)
1. [DPI and font size](#dpi-and-font-size)
1. [Requirements](#requirements)
   1. [Running](#running)
   1. [Building](#building)
1. [Installing](#installing)
   1. [Arch Linux](#arch-linux)
   1. [Other](#other)


## Features

* Fast
* Lightweight, both on-disk and in-memory
* Wayland native
* DE agnostic
* User configurable font fallback
* On-the-fly font resize
* On-the-fly DPI font size adjustment
* Scrollback search
* Color emoji support
* Server/daemon mode (one master process, many windows)
* [Sixel image support](https://en.wikipedia.org/wiki/Sixel)

  ![wow](doc/sixel-wow.png "Sixel screenshot")



## Non-features

This is a non-exhaustive list of things some people might consider
being important features (i.e. _"must-haves"_), that are unlikely to
ever be supported by foot.

* Tabs
* Graphical elements (menu, buttons etc)


## What does not work?

This is a list of known, but probably not all, issues:

* Unicode combining characters

    Examples: á (`LATIN SMALL LETTER A` + `COMBINING ACUTE ACCENT`)


## Fonts

**foot** supports all fonts that can be loaded by _freetype_,
including **bitmap** fonts and **color emoji** fonts.

Foot uses _fontconfig_ to locate and configure the font(s) to
use. Since fontconfig's fallback mechanism is imperfect, especially
for monospace fonts (it doesn't prefer monospace fonts even though the
requested font is one), foot allows you, the user, to configure the
fallback fonts to use.

This also means you can configure _each_ fallback font individually;
you want _that_ fallback font to use _this_ size, and you want that
_other_ fallback font to be _italic_?  No problem!

If a glyph cannot be found in _any_ of the user configured fallback
fonts, _then_ fontconfig's list is used.


## Shortcuts

At the moment, all shortcuts are hard coded and cannot be changed
(this is planned for a later release).. It is **not** possible to
define new key bindings.


### Keyboard

#### Normal mode

<kbd>shift</kbd>+<kbd>page up</kbd>/<kbd>page down</kbd>
: Scroll up/down in history

<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>c</kbd>
: Copy selected text to the _clipboard_

<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>v</kbd>
: Paste from _clipboard_

<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>r</kbd>
: Start a scrollback search

<kbd>ctrl</kbd>+<kbd>+</kbd>, <kbd>ctrl</kbd>+<kbd>=</kbd>
: Increase font size by 1pt

<kbd>ctrl</kbd>+<kbd>-</kbd>
: Decrease font size by 1pt

<kbd>ctrl</kbd>+<kbd>0</kbd>
: Reset font size


#### Scrollback search

<kbd>ctrl</kbd>+<kbd>r</kbd>
: Search _backward_ for next match

<kbd>ctrl</kbd>+<kbd>s</kbd>
: Search _forward_ for next match

<kbd>ctrl</kbd>+<kbd>w</kbd>
: Extend current selection (and thus the search criteria) to the end
  of the word, or the next word if currently at a word separating
  character.

<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>w</kbd>
: Same as <kbd>ctrl</kbd>+<kbd>w</kbd>, except that the only word
  separating characters are whitespace characters.

<kbd>escape</kbd>, <kbd>ctrl</kbd>+<kbd>g</kbd>
: Cancel the search

<kbd>return</kbd>
: Finish the search and copy the current match to the primary
  selection

### Mouse

<kbd>left</kbd> - **single-click**
: Drag to select; when released, the selected text is copied to the
  _primary_ selection. This feature is **disabled** when client has
  enabled _mouse tracking_.
: Holding <kbd>shift</kbd> enables selection in mouse tracking enabled
  clients.
: Holding <kbd>ctrl</kbd> will create a block selection.

<kbd>left</kbd> - **double-click**
: Selects the _word_ (separated by spaces, period, comma, parenthesis
  etc) under the pointer. Hold <kbd>ctrl</kbd> to select everything
  under the pointer up to, and until, the next space characters.

<kbd>left</kbd> - **triple-click**
: Selects the entire row

<kbd>middle</kbd>
: Paste from _primary_ selection

<kbd>wheel</kbd>
: Scroll up/down in history


## Server (daemon) mode

When run normally, **foot** is a single-window application; if you
want another window, start another foot process.

However, foot can also be run in a _server_ mode. In this mode, one
process hosts multiple windows. Note that this is **nothing** like
tabs. When first started in server mode, **no** windows are available.

You open new windows by running `footclient`. This is a small process
that instructs the foot server to open a new terminal window. The
client process remains running until the terminal window is
closed. The exit value of the client process is that of the shell that
was running in the terminal window.

The point of this mode is **a)** reduced memory footprint - all
terminal windows will share fonts and glyph cache, and **b)** reduced
startup time - loading fonts and populating the glyph cache takes
time, but in server mode it only happens once.

The downside is a performance penalty; all windows' input and output
are multiplexed in the same thread (but each window will have its own
set of rendering threads). This means that if one window is very busy
with, for example, producing output, then other windows will suffer.

And of course, should the server process crash, **all** windows will
be gone.

Typical usage would be to start the server process (`foot --server`)
when starting your Wayland compositor (i.e. logging in to your
desktop), and then run `footclient` instead of `foot` whenever you
want to launch a new terminal.


## Alt/meta

By default, foot prefixes _Meta characters_ with ESC. This corresponds
to XTerm's `metaSendsEscape` option set to `true`.

This can be disabled programatically with `\E[?1036l` (and enabled
again with `\E[?1036h`).

When disabled, foot will instead set the 8:th bit of meta character
and then UTF-8 encode it. This corresponds to XTerm's `eightBitMeta`
option set to `true`.

This can also be disabled programatically with `rmm` (_reset meta
mode_, `\E[?1034l`), and enabled again with `smm` (_set meta mode_,
`\E[?1034h`).


## Backspace

Foot transmits DEL (`^?`) on <kbd>backspace</kbd>. This corresponds to
XTerm's `backarrowKey` option set to `false`, and to DECBKM being
_reset_.

To instead transmit BS (`^H`), press
<kbd>ctrl</kbd>+<kbd>backspace</kbd>.

Note that foot does **not** implement DECBKM, and that the behavior
described above **cannot** be changed.

Finally, pressing <kbd>alt</kbd> will prefix the transmitted byte with
ESC.


## DPI and font size

Font sizes are apparently a complex thing. Many applications use a
fixed DPI of 96. They may also multiply it with the monitor's scale
factor.

This results in fonts with different **physical** sizes (i.e. if
measured by a ruler) when rendered on screens with different DPI
values. Even if the configured font size is the same.

This is not how it is meant to be. Fonts are measured in _point sizes_
**for a reason**; a given point size should have the same height on
all mediums, be it printers or monitors, regardless of their DPI.

Foot will always use the monitor's physical DPI value. Scale factors
are irrelevant (well, they affect e.g. padding, but not the font
size). This means the glyphs rendered by foot should always have the
same physical height, regardless of monitor.

Foot will re-size the fonts on-the-fly when the window is moved
between screens with different DPIs values. If the window covers
multiple screens, with different DPIs, the highest DPI will be used.

_Tip_: QT applications can be configured to work this way too, by
exporting the environment variable `QT_WAYLAND_FORCE_DPI=physical`.

_Note_: if you configure **pixelsize**, rather than **size**, then DPI
changes will **not** change the font size. Pixels are always pixels.


## Requirements

### Running

* fontconfig
* freetype
* pixman
* wayland (_client_ and _cursor_ libraries)
* xkbcommon
* [tllist](https://codeberg.org/dnkl/tllist) [^1]
* [fcft](https://codeberg.org/dnkl/fcft) [^1]

[^1]: can also be built as subprojects, in which case they are
    statically linked.


### Building

In addition to the dev variant of the packages above, you need:

* meson
* ninja
* wayland protocols
* ncurses
* scdoc


## Installing

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


### Arch Linux

Use [makepkg](https://wiki.archlinux.org/index.php/Makepkg) to build
the bundled [PKGBUILD](PKGBUILD) (run `makepkg` in the source root
directory).

It **requires** [tllist](https://codeberg.org/dnkl/tllist) and
[fcft](https://codeberg.org/dnkl/fcft) to be installed as system
libraries. If you do not want this, please edit the PKGBUILD file, or
install manually (see [Other](#other) below).

Note that it will do a profiling-guided build, and that this requires
a running wayland session since it needs to run an intermediate build
of foot.


### Other

Foot uses _meson_. If you are unfamiliar with it, the official
[tutorial](https://mesonbuild.com/Tutorial.html) might be a good
starting point.

I also recommend taking a look at the bundled Arch
[PKGBUILD](PKGBUILD) file, to see how it builds foot. Especially so if
you intend to install a release build of foot, in which case you might
be interested in the compiler flags used there.

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

Second, configure[^2] the build (if you intend to install it globally, you
might also want `--prefix=/usr`):
```sh
meson --buildtype=release ../..
```

[^2]: for advanced users: a profile guided build will have
    significantly better performance; take a look at
    [PKDBUILD](PKGBUILD) to see how this can be done.

Three, build it:
```sh
ninja
```

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
`/usr/share/terminfo/f` - but this is dependens on the distro), or
just install everything:
```sh
ninja install
```
