# foot

**foot** is a fast Wayland terminal emulator.

## Index

1. [Features](#features)
1. [Non-features](#non-features)
1. [What does not work?](#what-does-not-work)
1. [Fonts](#fonts)
1. [Shortcuts](#shortcuts)
   1. [Keyboard](#keyboard)
   1. [Mouse](#mouse)
1. [Requirements](#requirements)
   1. [Running](#running)
   1. [Building](#building)
1. [Installing](#installing)
   1. [Arch Linux](#arch-linux)
   1. [Other](#other)


## Features

* Fast (**TODO** insert benchmark results here)
* Wayland native
* DE agnostic
* User configurable font fallback
* Scrollback search
* Color emoji support


## Non-features

This is a non-exhaustive list of things some people might consider
being important features (i.e. _"must-haves"_), that are unlikely to
ever be supported by foot.

* Tabs
* Multiple windows


## What does not work?

This is a list of known, but probably not all, issues:

* **Unicode combining characters**

  Examples: á, 👪🏼 (_may not be displayed correctly in your
  browser/editor_)

* **Reflow text on window resize**


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

At the moment, all shortcuts are hard coded and cannot be changed. It
is **not** possible to define new key bindings.


### Keyboard

* <kbd>shift</kbd>+<kbd>page up</kbd>/<kbd>page down</kbd>

  Scroll up/down in history

* <kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>c</kbd>

  Copy selected text to the _clipboard_

* <kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>v</kbd>

  Paste from _clipboard_

* <kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>r</kbd>

  Start a scrollback search

While doing a scrollback search, the following shortcuts are
available:

* <kbd>ctrl</kbd>+<kbd>r</kbd>

  Search _backward_ for next match

* <kbd>ctrl</kbd>+<kbd>s</kbd>

  Search _forward_ for next match

* <kbd>escape</kbd>, <kbd>ctrl</kbd>+<kbd>g</kbd>

  Cancel the search

* <kbd>return</kbd>

  Finish the search and copy the current match to the primary
  selection

### Mouse

* <kbd>left</kbd> - **single-click**

  Drag to select; when released, the selected text is copied to the
  _primary_ selection. Note that this feature is normally **disabled**
  whenever the client has enabled _mouse tracking_, but can be forced
  by holding <kbd>shift</kbd>.

* <kbd>left</kbd> - **double-click**

  Selects the _word_ (separated by spaces, period, comma, parenthesis
  etc) under the pointer. Hold <kbd>ctrl</kbd> to select everything
  under the pointer up to, and until, the next space characters.

* <kbd>left</kbd> - **triple-click**

  Selects the entire row

* <kbd>middle</kbd>

  Paste from _primary_ selection


## Requirements

### Running

* fontconfig
* freetype
* pixman
* wayland (_client_ and _cursor_ libraries)
* xkbcommon


### Building

In addition to the dev variant of the packages above, you need:

* meson
* ninja
* wayland protocols
* ncurses
* scdoc


## Installing

### Arch Linux

Use [makepkg](https://wiki.archlinux.org/index.php/Makepkg) to build
the bundled `PKGBUILD` (just run `makepkg` in the source root
directory)..

Note that it will do a profiling-guided build, and that this requires
a running wayland session since it needs to run an intermediate build
of foot.


### Other

Foot uses _meson_. If you are unfamiliar with it, the official
[tutorial](https://mesonbuild.com/Tutorial.html) might be a good
starting point.

I also recommend taking a look at that bundled Arch `PKGBUILD` file,
to see how it builds foot.
