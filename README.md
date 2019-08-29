# foot

**foot** is a fast Wayland terminal emulator.


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


## Fonts

**foot** supports all fonts that can be loaded by freetype, including
**bitmap** fonts and **color emoji** fonts.

Foot uses its own font fallback mechanism, rather than relying on
fontconfig's fallback. This is because fontconfig is quite bad at
selecting fallback fonts suitable for a terminal (i.e. monospaced
fonts).

Instead, foot allows you to specify a font fallback list, where _each_
font can be configured independently (for example, you can configure
the size for each font individually).

If a glyph cannot be found in _any_ of the user configured fallback
fonts, _then_ fontconfig's list is used.


## Shortcuts

At the moment, all shortcuts are hard coded and cannot be changed. It
is **not** possible to define new key bindings.


### Keyboard

* <kbd>shift</kbd>+<kbd>page up</kbd>/<kbd>page down</kbd>

  scroll up/down in history

* <kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>c</kbd>

  copy selected text to the _clipboard_

* <kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>v</kbd>

  paste from _clipboard_

* <kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>r</kbd>

  start a scrollback search

While doing a scrollback search, the following shortcuts are
available:

* <kbd>ctrl</kbd>+<kbd>r</kbd>

  search for next match

* <kbd>escape</kbd>

  cancel the search

* <kbd>ctrl</kbd>+<kbd>g</kbd>

  cancel the search (same as `esc`)

* <kbd>return</kbd>

  finish the search and put the current match to the primary selection

### Mouse

* <kbd>left</kbd> - **single-click**

  drag to select; when released, the selected text is copied to the
  _primary_ selection. Note that this feature is normally disabled
  whenever the client has enabled mouse tracking, but can be forced by
  holding `shift`.

* <kbd>left</kbd> - **double-click**

  selects the _word_ (separated by spaces, period, comma, parenthesis
  etc) under the pointer. Hold `ctrl` to select everything under the
  pointer up to, and until, the next space characters.

* <kbd>left</kbd> - **triple-click**

  selects the entire row

* <kbd>middle</kbd>

  paste from _primary_ selection
