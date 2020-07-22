# Changelog

* [1.4.1](#1-4-1)
* [1.4.0](#1-4-0)
* [1.3.0](#1-3-0)
* [1.2.3](#1-2-3)
* [1.2.2](#1-2-2)
* [1.2.1](#1-2-1)
* [1.2.0](#1-2-0)


## 1.4.1

### Fixed

* Compilation errors in release builds with some combinations of
  compilers and compiler flags.


## 1.4.0

### Added

* `Sync` to terminfo. This is a tmux extension that indicates
  _"Synchronized Updates"_ are supported.
* `--hold` command line option to `footclient`.
* Key mapping for `KP_Decimal`.
* Terminfo entries for keypad keys: `ka1`, `ka2`, `ka3`, `kb1`, `kb3`,
  `kc1`, `kc2`, `kc3`, `kp5`, `kpADD`, `kpCMA`, `kpDIV`, `kpDOT`,
  `kpMUL`, `kpSUB` and `kpZRO`.
* **blink** option to `footrc`; a boolean that lets you control
    whether the cursor should blink or not by default. Note that
    applications can override this.
* Multi-seat support
* Implemented `C0::FF` (form feed)
* **pipe-visible** and **pipe-scrollback** key bindings. These let you
  pipe either the currently visible text, or the entire scrollback to
  external tools (https://codeberg.org/dnkl/foot/issues/29). Example:
  `pipe-visible=[sh -c "xurls | bemenu | xargs -r firefox] Control+Print`


### Changed

* Background transparency to only be used with the default background
  color.
* Copy-to-clipboard/primary-selection to insert a line break if either
  the last cell on the previous line or the first cell on the next
  line is empty.
* Number of lines to scroll is now always clamped to the number of
  lines in the scrolling region..
* New terminal windows spawned with `ctrl`+`shift`+`n` are no longer
  double forked.
* Unicode combining character overflow errors are only logged when
  debug logging has been enabled.
* OSC 4 (_Set Color_) now updates already rendered cells, excluding
  scrollback.
* Mouse cursor from `hand2` to `left_ptr` when client is capturing the
  mouse.
* Sixel images are now removed when the font size is **decreased**.
* `DECSCUSR` (_Set Cursor Style_, `CSI Ps SP q`) now uses `Ps=0`
  instead of `Ps=2` to reset the style to the user configured default
  style. `Ps=2` now always configures a _Steady Block_ cursor.
* `Se` terminfo capability from `\E[2 q` to `\E[ q`.
* Hollow cursor to be drawn when window has lost _keyboard_ focus
  rather than _visual_ focus.


### Fixed

* Do not stop an ongoing selection when `shift` is released. When the
  client application is capturing the mouse, one must hold down
  `shift` to start a selection. This selection is now finalized only
  when the mouse button is released - not as soon as `shift` is
  released.
* Selected cells did not appear selected if programmatically modified.
* Rare crash when scrolling and the new viewport ended up **exactly**
  on the wrap around.
* Selection handling when viewport wrapped around.
* Restore signal mask in the client process.
* Set `IUTF8`.
* Selection of double-width characters. It is no longer possible to
  select half of a double-width character.
* Draw hollow block cursor on top of character.
* Set an initial `TIOCSWINSZ`. This ensures clients never read a
  `0x0` terminal size (https://codeberg.org/dnkl/foot/issues/20).
* Glyphs overflowing into surrounding cells
  (https://codeberg.org/dnkl/foot/issues/21).
* Crash when last rendered cursor cell had scrolled off screen and
  `\E[J3` was executed.
* Assert (debug builds) when an `\e]4` OSC escape was not followed by
  a `;`.
* Window title always being set to "foot" on reset.
* Terminfo entry `kb2` (center keypad key); it is now set to `\EOu`
  (which is what foot emits) instead of the incorrect value `\EOE`.
* Palette re-use in sixel images. Previously, the palette was reset
  after each image.
* Do not auto-resize a sixel image for which the cllent has specified
  a size. This fixes an issue where an image would incorrectly
  overflow into the cell row beneath.
* Text printed, or other sixel images drawn, on top of a sixel image
  no longer erases the entire image, only the part(s) covered by the
  new text or image.
* Sixel images being erased when printing text next to them.
* Sixel handling when resizing window.
* Sixel handling when scrollback wraps around.
* Foot now issues much fewer `wl_surface_damage_buffer()` calls
  (https://codeberg.org/dnkl/foot/issues/35).
* `C0::VT` to be processed as `C0::LF`. Previously, `C0::VT` would
  only move the cursor down, but never scroll.
* `C0::HT` (_Horizontal Tab_, or `\t`) no longer clears `LCF` (_Last
  Column Flag_).
* `C0::LF` now always clears `LCF`. Previously, it only cleared it
  when the cursor was **not** at the bottom of the scrolling region.
* `IND` and `RI` now clears `LCF`.
* `DECAWM` now clears `LCF`.
* A multi-column character that does not fit on the current line is
  now printed on the next line, instead of only printing half the
  character.
* Font size can no longer be reduced to negative values
  (https://codeberg.org/dnkl/foot/issues/38).


## 1.3.0

### Added

* User configurable key- and mouse bindings. See `man 5 foot` and the
  example `footrc` (https://codeberg.org/dnkl/foot/issues/1)
* **initial-window-mode** option to `footrc`, that lets you control
  the initial mode for each newly spawned window: _windowed_,
  _maximized_ or _fullscreen_.
* **app-id** option to `footrc` and `--app-id` command line option,
  that sets the _app-id_ property on the Wayland window.
* **title** option to `footrc` and `--title` command line option, that
  sets the initial window title.
* Right mouse button extends the current selection.
* `CSI Ps ; Ps ; Ps t` escape sequences for the following parameters:
  `11t`, `13t`, `13;2t`, `14t`, `14;2t`, `15t`, `19t`.
* Unicode combining characters.


### Changed

* Spaces no longer removed from zsh font name completions.
* Default key binding for _spawn-terminal_ to ctrl+shift+n.
* Renderer is now much faster with interactive scrolling
  (https://codeberg.org/dnkl/foot/issues/4)
* memfd sealing failures are no longer fatal errors.
* Selection to no longer be cleared on resize.
* The current monitor's subpixel order (RGB/BGR/V-RGB/V-BGR) is
  preferred over FontConfig's `rgba` property. Only if the monitor's
  subpixel order is `unknown` is FontConfig's `rgba` property used. If
  the subpixel order is `none`, then grayscale antialiasing is
  used. The subpixel order is ignored if antialiasing has been
  disabled.
* The four primary font variants (normal, bold, italic, bold italic)
  are now loaded in parallel. This speeds up both the initial startup
  time, as well as DPI changes.
* Command line parsing no longer tries to parse arguments following
  the command-to-execute. This means one can now write `foot sh -c
  true` instead of `foot -- sh -c true`.


### Removed

* Keyboard/pointer handler workarounds for Sway 1.2.


### Fixed

* Sixel images moved or deleted on window resize.
* Cursor sometimes incorrectly restored on exit from alternate screen.
* 'Underline' cursor being invisible on underlined text.
* Restored cursor position in 'normal' screen when window was resized
  while in 'alt' screen.
* Hostname in OSC 7 URI not being validated.
* OSC 4 with multiple `c;spec` pairs.
* Alt+Return to emit "ESC \r".
* Trackpad sloooow scrolling to eventually scroll a line.
* Memory leak in terminal reset.
* Translation of cursor coordinates on resize
* Scaling color specifiers in OSC sequences.
* `OSC 12 ?` to return the cursor color, not the cursor's text color.
* `OSC 12;#000000` to configure the cursor to use inverted
  foreground/background colors.
* Call `ioctl(TIOCSCTTY)` on the pts fd in the slave process.


## 1.2.3

### Fixed
* Forgot to version bump 1.2.2


## 1.2.2

### Changed

* Changed icon name in `foot.desktop` and `foot-server.desktop` from
  _terminal_ to _utilities-terminal_.
* `XDG_SESSION_ID` is now included in the server/daemon default socket
  path.


### Fixed

* Window size doubling when moving window between outputs with
  different scaling factors (https://codeberg.org/dnkl/foot/issues/3).
* Font being too small on monitors with fractional scaling
  (https://codeberg.org/dnkl/foot/issues/5).


## 1.2.1

### Fixed

* Building AUR package


## 1.2.0

### Added

* Run-time text resize using ctrl-+, ctrl+- and ctrl+0
* Font size adjusts dynamically to outputs' DPI
* Reflow text when resizing window
* **pad** option to `footrc`
* **login-shell** option to `footrc` and `--login-shell` command line
  option
* Client side decorations (CSDs). This finally makes foot usable on
  GNOME.
* Sixel graphics support
* OSC 12 and 112 escape sequences (set/reset text cursor color)
* REP CSI escape sequence
* `oc` to terminfo
* foot-server.desktop file
* Window and cell size reporting escape sequences
* `--hold` command line option
* `--print-pid=FILE|FD` command line option


### Changed

* Subpixel antialiasing is only enabled when background is opaque
* Meta/alt ESC prefix can now be disabled with `\E[?1036l`. In this
  mode, the 8:th bit is set and the result is UTF-8 encoded. This can
  also be disabled with `\E[1024l` (in which case the Alt key is
  effectively being ignored).
* terminfo now uses ST instead of BEL as OSC terminator
* Logging to print to stderr, not stdout
* Backspace now emits DEL (^?), and ctrl+backspace emits BS (^H)


### Removed

* '28' from DA response
