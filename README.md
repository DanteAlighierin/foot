# ![Logo: a terminal with a foot shaped prompt](icons/hicolor/48x48/apps/foot.png) foot

The fast, lightweight and minimalistic Wayland terminal emulator.

## Index

1. [Features](#features)
1. [Installing](#installing)
1. [Configuration](#configuration)
1. [Troubleshooting](#troubleshooting)
1. [Why the name 'foot'?](#why-the-name-foot)
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
1. [Supported OSCs](#supported-oscs)
1. [Programmatically checking if running in foot](#programmatically-checking-if-running-in-foot)
1. [Credits](#Credits)
1. [Bugs](#bugs)
1. [Mastodon](#mastodon)
1. [License](#license)


## Features

* Fast (see [benchmarks](doc/benchmark.md), and
  [performance](https://codeberg.org/dnkl/foot/wiki/Performance))
* Lightweight, in dependencies, on-disk and in-memory
* Wayland native
* DE agnostic
* User configurable font fallback
* On-the-fly font resize
* On-the-fly DPI font size adjustment
* Scrollback search
* Color emoji support
* Server/daemon mode
* Multi-seat
* [Synchronized Updates](https://gitlab.freedesktop.org/terminal-wg/specifications/-/merge_requests/2) support
* [Sixel image support](https://en.wikipedia.org/wiki/Sixel)

  ![wow](doc/sixel-wow.png "Sixel screenshot")


# Installing

See [INSTALL.md](INSTALL.md).


## Configuration

**foot** can be configured by creating a file
`$XDG_CONFIG_HOME/foot/foot.ini` (defaulting to
`~/.config/foot/foot.ini`).  A template for that can usually be found
in `/usr/share/foot/foot.ini` or
[here](https://codeberg.org/dnkl/foot/src/branch/master/foot.ini).

Further information can be found in foot's man page `foot.ini(5)`.


## Troubleshooting

See the [wiki](https://codeberg.org/dnkl/foot/wiki#user-content-troubleshooting)


## Why the name 'foot'?

I'm bad at names. Most of my projects usually start out as _foo
something_ (for example, [yambar](https://codeberg.org/dnkl/yambar)
was _f00bar_ for a while).

So why _foot_?

_foo terminal_ → _footerm_ → _foot_

Pretty bad, I know.

As a side note, if you pronounce the _foo_ part of _foot_ the same way
you pronounce _foobar_, then _foot_ sounds a lot like the Swedish word
_fot_, which incidentally means (you guessed it) _foot_.


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

These are the default shortcuts. See `man foot.ini` and the example
`foot.ini` to see how these can be changed.


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
: Increase font size by 0,5pt

<kbd>ctrl</kbd>+<kbd>-</kbd>
: Decrease font size by 0,5pt

<kbd>ctrl</kbd>+<kbd>0</kbd>
: Reset font size

<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>n</kbd>
: Spawn a new terminal. If the shell has been [configured to emit the
  OSC 7 escape
  sequence](https://codeberg.org/dnkl/foot/wiki#user-content-how-to-configure-my-shell-to-emit-the-osc-7-escape-sequence),
  the new terminal will start in the current working directory.


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

<kbd>right</kbd>
: Extend current selection. Clicking immediately extends the
  selection, while hold-and-drag allows you to interactively resize
  the selection.

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

This can be disabled programmatically with `\E[?1036l` (and enabled
again with `\E[?1036h`).

When disabled, foot will instead set the 8:th bit of meta character
and then UTF-8 encode it. This corresponds to XTerm's `eightBitMeta`
option set to `true`.

This can also be disabled programmatically with `rmm` (_reset meta
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


## Supported OSCs

OSC, _Operating System Command_, are escape sequences that interacts
with the terminal emulator itself. Foot implements the following OSCs:

* `OSC 0` - change window icon + title (but only title is actually
  supported)
* `OSC 2` - change window title
* `OSC 4` - change color palette
* `OSC 7` - report CWD
* `OSC 10` - change (default) foreground color
* `OSC 11` - change (default) background color
* `OSC 12` - change cursor color
* `OSC 52` - copy/paste clipboard data
* `OSC 104` - reset color palette
* `OSC 110` - reset default foreground color
* `OSC 111` - reset default background color
* `OSC 112` - reset cursor color
* `OSC 555` - flash screen (**foot specific**)


## Programmatically checking if running in foot

Foot does **not** set any environment variables that can be used to
identify foot (reading `TERM` is not reliable since the user may have
chosen to use a different terminfo).

You can instead use the escape sequences to read the _Secondary_ and
_Tertiary Device Attributes_ (secondary/tertiary DA, for short).

The tertiary DA response is always `\EP!|464f4f54\E\\`. The `\EP!|` is
the standard tertiary DA response prefix, `DCS ! |`. The trailing
`\E\\` is of course the standard string terminator, `ST`.

In the response above, the interesting part is `464f4f54`; this is
the string _FOOT_ in hex.

The secondary DA response is `\E[>1;XXYYZZ;0c`, where XXYYZZ is foot's
major, minor and patch version numbers, in decimal, using two digits
for each number. For example, foot-1.4.2 would respond with
`\E[>1;010402;0c`.

**Note**: not all terminal emulators implement tertiary DA. Most
implement secondary DA, but not all. All _should_ however implement
_Primary DA_.

Thus, a safe way to query the terminal is to request the tertiary,
secondary and primary DA all at once, in that order. All terminals
should ignore escape sequences they do not recognize. You will have to
parse the response (which in foot will consist of all three DA
responses, all at once) to determine which requests the terminal
emulator actually responded to.


# Credits

* [Ordoviz](https://codeberg.org/Ordoviz), for designing and
contributing foot's [logo](icons/hicolor/48x48/apps/foot.png).


# BUGS

Please report bugs to https://codeberg.org/dnkl/foot/issues

The report should contain the following:

* Which Wayland compositor (and version) you are running
* Foot version (`foot --version`)
* Log output from foot (start foot from another terminal)
* If reporting a crash, please try to provide a `bt full` backtrace
  **with symbols** (i.e. use a debug build)
* Steps to reproduce. The more details the better


# Mastodon

Every now and then I post foot related updates on
[@dnkl@linuxrocks.online](https://linuxrocks.online/@dnkl)


# License

Foot is released under the [MIT license](LICENSE).
