# foot

**foot** is a Wayland terminal emulator.


## Requirements

* fontconfig
* freetype
* cairo
* wayland (_client_ and _cursor_ libraries)
* wayland protocols
* xkbcommon


## Fonts

**foot** supports all fonts that can be loaded by freetype, including
bitmap fonts and color emoji fonts.

Foot uses its own font fallback mechanism, rather than relying on
fontconfig's fallback. This is because fontconfig is quite bad at
selecting fallback fonts suitable for a terminal (i.e. monospaced
fonts).

Instead, foot allows you to specify a font fallback list, where _each_
font can be configured independently (for example, you can configure
the size for each font individually).

Note that _subpixel antialiasing_ is **not** supported at the moment
(but grayscale antialiasing is).


## Shortcuts

At the moment, all shortcuts are hard coded and cannot be changed. It
is **not** possible to define new key bindings.


### Keyboard

* `shift+page up/down` - scroll up/down in history
* `ctrl+shift+c` - copy selected text to the _clipboard_
* `ctrl+shift+v` - paste from _clipboard_


### Mouse

* `left` - single-click: drag to select; when released, the selected
  text is copied to the _primary_ selection. Note that this feature is
  normally disabled whenever the client has enabled mouse tracking,
  but can be forced by holding `shift`.
* `left` - double-click: selects the _word_ (separated by spaces,
  period, comma, parenthesis etc) under the pointer. Hold `ctrl` to
  select everything under the pointer up to, and until, the next space
  characters.
* `left` - triple-click: selects the entire row
