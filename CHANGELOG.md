# Changelog

* [Unreleased](#unreleased)
* [1.18.1](#1-18-1)
* [1.18.0](#1-18-0)
* [1.17.2](#1-17-2)
* [1.17.1](#1-17-1)
* [1.17.0](#1-17-0)
* [1.16.2](#1-16-2)
* [1.16.1](#1-16-1)
* [1.16.0](#1-16-0)
* [1.15.3](#1-15-3)
* [1.15.2](#1-15-2)
* [1.15.1](#1-15-1)
* [1.15.0](#1-15-0)
* [1.14.0](#1-14-0)
* [1.13.1](#1-13-1)
* [1.13.0](#1-13-0)
* [1.12.1](#1-12-1)
* [1.12.0](#1-12-0)
* [1.11.0](#1-11-0)
* [1.10.3](#1-10-3)
* [1.10.2](#1-10-2)
* [1.10.1](#1-10-1)
* [1.10.0](#1-10-0)
* [1.9.2](#1-9-2)
* [1.9.1](#1-9-1)
* [1.9.0](#1-9-0)
* [1.8.2](#1-8-2)
* [1.8.1](#1-8-1)
* [1.8.0](#1-8-0)
* [1.7.2](#1-7-2)
* [1.7.1](#1-7-1)
* [1.7.0](#1-7-0)
* [1.6.4](#1-6-4)
* [1.6.3](#1-6-3)
* [1.6.2](#1-6-2)
* [1.6.1](#1-6-1)
* [1.6.0](#1-6-0)
* [1.5.4](#1-5-4)
* [1.5.3](#1-5-3)
* [1.5.2](#1-5-2)
* [1.5.1](#1-5-1)
* [1.5.0](#1-5-0)
* [1.4.4](#1-4-4)
* [1.4.3](#1-4-3)
* [1.4.2](#1-4-2)
* [1.4.1](#1-4-1)
* [1.4.0](#1-4-0)
* [1.3.0](#1-3-0)
* [1.2.3](#1-2-3)
* [1.2.2](#1-2-2)
* [1.2.1](#1-2-1)
* [1.2.0](#1-2-0)


## Unreleased
### Added

* `resize-keep-grid` option, controlling whether the window is resized
  (and the grid reflowed) or not when e.g. zooming in/out
  ([#1807][1807]).
* `strikeout-thickness` option.
* Implemented the new `xdg-toplevel-icon-v1` protocol.
* Implemented `CSI 21 t`: report window title.

[1807]: https://codeberg.org/dnkl/foot/issues/1807


### Changed

* `cursor.unfocused-style` is now effective even when `cursor.style`
  is not `block`.
* Activating a notification triggered with OSC-777, or BEL, now
  focuses the foot window, if XDG activation tokens are supported by
  the compositor, the notification daemon, and the notification helper
  used by foot (i.e. `desktop-notifications.command`). This has been
  supported for OSC-99 since 1.18.0, and now we also support it for
  BEL and OSC-777 ([#1822][1822]).
* Sixel background color (when `P2=0|2`) is now set to the **sixel**
  color palette entry #0, instead of using the current ANSI background
  color. This is what a real VT340 does.
* The `.desktop` files no longer use the reverse DNS naming scheme,
  and their names now match the default app-ids used by foot (`foot`
  and `footclient`) ([#1607][1607]).

[1822]: https://codeberg.org/dnkl/foot/issues/1822
[1607]: https://codeberg.org/dnkl/foot/issues/1607


### Deprecated
### Removed
### Fixed

* Some invalid UTF-8 strings passing the validity check when setting
  the window title, triggering a Wayland protocol error which then
  caused foot to shutdown.
* "Too large" values for `scrollback.lines` causing an integer
  overflow, resulting in either visual glitches, crashes, or both
  ([#1828][1828]).
* Crash when trying to set an invalid cursor shape with OSC-22, when
  foot uses server-side cursor shapes.
* Occasional visual glitches when selecting text, when foot is running
  under a compositor that forces foot to double buffer
  (e.g. KDE/KWin) ([#1715][1715]).
* Sixels flickering when foot is running under a compositor that
  forces foot to double buffer (e.g. KDE, or Smithay based
  compositors) ([#1851][1851]).

[1828]: https://codeberg.org/dnkl/foot/issues/1828
[1715]: https://codeberg.org/dnkl/foot/issues/1715
[1851]: https://codeberg.org/dnkl/foot/issues/1851


### Security
### Contributors


## 1.18.1

### Added

* OSC-99: support for the `s` parameter. Supported keywords are
  `silent`, `system` and names from the freedesktop sound naming
  specification.
* `${muted}` and `${sound-name}` added to the
  `desktop-notifications.command` template.


### Changed

* CSD buttons now activate on mouse button **release**, rather than
  press ([#1787][1787]).

[1787]: https://codeberg.org/dnkl/foot/issues/1787


### Fixed

* Regression: OSC-111 not handling alpha changes correctly, causing
  visual glitches ([#1801][1801]).

[1801]: https://codeberg.org/dnkl/foot/issues/1801


### Contributors

* Craig Barnes
* Shogo Yamazaki


## 1.18.0

### Added

* `cursor.blink-rate` option, allowing you to configure the rate the
  cursor blinks with (when `cursor.blink=yes`) ([#1707][1707]);
* Support for `wp_single_pixel_buffer_v1`; certain overlay surfaces
  will now utilize the new single-pixel buffer protocol. This mainly
  reduces the memory usage, but should also be slightly faster.
* Support for high-res mouse wheel scroll events ([#1738][1738]).
* Styled and colored underlines ([#828][828]).
* Support for SGR 21 (double underline).
* Support for `XTPUSHCOLORS`, `XTPOPCOLORS` and `XTREPORTCOLORS`,
  i.e. color palette stack ([#856][856]).
* Log output now respects the [`NO_COLOR`](http://no-color.org/)
  environment variable ([#1771][1771]).
* Support for [in-band window resize
  notifications](https://gist.github.com/rockorager/e695fb2924d36b2bcf1fff4a3704bd83),
  private mode `2048`.
* Support for OSC-99 [_"Kitty desktop
  notifications"_](https://sw.kovidgoyal.net/kitty/desktop-notifications/).
* `desktop-notifications.command` option, replaces `notify`.
* `desktop-notifications.inhibit-when-focused` option, replaces
  `notify-focus-inhibit`.
* `${category}`, `${urgency}`, `${expire-time}`, `${replace-id}`,
  `${icon}` and `${action-argument}` added to the
  `desktop-notifications.command` template.
* `desktop-notifications.command-action-argument` option, defining how
  `${action-argument}` (in `desktop-notifications.command`) should be
  expanded.
* `desktop-notifications.close` option, defining what to execute when
  an application wants to close an existing notification (via an
  OSC-99 escape sequence).

[1707]: https://codeberg.org/dnkl/foot/issues/1707
[1738]: https://codeberg.org/dnkl/foot/issues/1738
[828]: https://codeberg.org/dnkl/foot/issues/828
[856]: https://codeberg.org/dnkl/foot/issues/856
[1771]: https://codeberg.org/dnkl/foot/issues/1771


### Changed

* All `XTGETTCAP` capabilities are now in the `tigetstr()` format:

  - parameterized string capabilities were previously "source
    encoded", meaning e.g. `\E` where not "decoded" into `\x1b`.
  - Control characters were also "source encoded", meaning they were
    returned as e.g. "^G" instead of `\x07` ([#1701][1701]).

  In other words, if, after this change, `XTGETTCAP` returns a string
  that is different compared to `tigetstr()`, then it is likely a bug
  in foot's implementation of `XTGETTCAP`.
* If the cursor foreground and background colors are identical (for
  example, when cursor uses inverted colors and the cell's foreground
  and background are the same), the cursor will instead be rendered
  using the default foreground and background colors, inverted
  ([#1761][1761]).
* Mouse wheel events now generate `BTN_WHEEL_BACK` and
  `BTN_WHEEL_FORWARD` "button presses", instead of `BTN_BACK` and
  `BTN_FORWARD`. The default bindings have been updated, and
  `scrollback-up-mouse`, `scrollback-down-mouse`, `font-increase` and
  `font-decrease` now use the new button names.

  This change allow users to separate physical mouse buttons that
  _also_ generates `BTN_BACK` and `BTN_FORWARD`, from wheel scrolling
  ([#1763][1763]).
* Replaced the old catppuccin theme with updated flavored themes
  pulled from [catppuccin/foot](https://github.com/catppuccin/foot)
* Mouse selections can now be started inside the margins
  ([#1702][1702]).

[1701]: https://codeberg.org/dnkl/foot/issues/1701
[1761]: https://codeberg.org/dnkl/foot/issues/1761
[1763]: https://codeberg.org/dnkl/foot/issues/1763
[1702]: https://codeberg.org/dnkl/foot/issues/1702


### Deprecated

* `notify` option; replaced by `desktop-notifications.command`.
* `notify-focus-inhibit` option; replaced by
  `desktop-notifications.inhibit-when-focused`.


### Fixed

* Crash when zooming in or out, with `dpi-aware=yes`, and the
  monitor's DPI is 0 (this is true for, for example, nested Wayland
  sessions, or in virtualized environments).
* No error response for empty `XTGETTCAP` request ([#1694][1694]).
* Unicode-mode in one foot client affecting other clients, in foot
  server mode ([#1717][1717]).
* IME interfering in URL-mode ([#1718][1718]).
* OSC-52 reply interleaved with other data sent to the client
  ([#1734][1734]).
* XKB compose state being reset when foot receives a new keymap
  ([#1744][1744]).
* Regression: alpha changes through OSC-11 sequences not taking effect
  until window is resized.
* VS15 being ignored ([#1742][1742]).
* VS16 being ignored for a subset of the valid VS16 sequences
  ([#1742][1742]).
* Crash in debug builds, when using OSC-12 to set the cursor color and
  foot config has not set any custom cursor colors (i.e. without
  OSC-12, inverted fg/bg would be used).
* Wrong color used when drawing the unfocused, hollow cursor.
* Encoding of `BTN_BACK` and `BTN_FORWARD`, when sending a mouse input
  escape sequence to the terminal application.

[1694]: https://codeberg.org/dnkl/foot/issues/1694
[1717]: https://codeberg.org/dnkl/foot/issues/1717
[1718]: https://codeberg.org/dnkl/foot/issues/1718
[1734]: https://codeberg.org/dnkl/foot/issues/1734
[1744]: https://codeberg.org/dnkl/foot/issues/1744
[1742]: https://codeberg.org/dnkl/foot/issues/1742


### Contributors

* abs3nt
* Artturin
* Craig Barnes
* Jan Beich
* Mariusz Bialonczyk
* Nicolas Kolling Ribas


## 1.17.2

### Changed

* Notifications with invalid UTF-8 strings are now ignored.


### Fixed

* Crash when changing aspect ratio of a sixel, in the middle of the
  sixel data (this is unsupported in foot, but should of course not
  result in a crash).
* Crash when printing double-width (or longer) characters to, or near,
  the last column, when auto-wrap (private mode 7) has been disabled.
* Dynamically sized sixel being trimmed to nothing.
* Flickering with `dpi-aware=yes` and window is unmapped/remapped
  (some compositors do this when window is minimized), in a
  multi-monitor setup with different monitor DPIs.


## 1.17.1

### Added

* `cursor.unfocused-style=unchanged|hollow|none` to `foot.ini`. The
  default is `hollow` ([#1582][1582]).
* New key binding: `quit` ([#1475][1475]).

[1582]: https://codeberg.org/dnkl/foot/issues/1582
[1475]: https://codeberg.org/dnkl/foot/issues/1475


### Fixed

* Log-level not respected by syslog.
* Regression: terminal shutting down when the PTY is closed by the
  client application, which may be earlier than when the client
  application exits ([#1666][1666]).
* When closing the window, send `SIGHUP` to the client application,
  before sending `SIGTERM`. The signal sequence is now `SIGHUP`, wait,
  `SIGTERM`, wait `SIGKILL`.
* Crash when receiving a `DECRQSS` request with more than 2 bytes in
  the `q` parameter.

[1666]: https://codeberg.org/dnkl/foot/issues/1666


### Contributors

* Holger Weiß
* izmyname
* Marcin Puc
* tunjan


## 1.17.0

### Added

- Support for opening an existing PTY, e.g. a VM console.
  ([#1564][1564])
* Unicode input mode now accepts input from the numpad as well,
  numlock is ignored.
* A new `resize-by-cells` option, enabled by default, allows the size
  of floating windows to be constrained to multiples of the cell size.
* Support for custom (i.e. other than ctrl/shift/alt/super) modifiers
  in key bindings ([#1348][1348]).
* `pipe-command-output` key binding.
* Support for OSC-176, _"Set App-ID"_
  (https://gist.github.com/delthas/d451e2cc1573bb2364839849c7117239).
* Support for `DECRQM` queries with ANSI/ECMA-48 modes (`CSI Ps $ p`).
* Rectangular edit functions: `DECCARA`, `DECRARA`, `DECCRA`, `DECFRA`
  and `DECERA` ([#1633][1633]).
* `Rect` capability to terminfo.
* `fe` and `fd` (focus in/out enable/disable) capabilities to
  terminfo.
* `nel` capability to terminfo.

[1348]: https://codeberg.org/dnkl/foot/issues/1348
[1633]: https://codeberg.org/dnkl/foot/issues/1633
[1564]: https://codeberg.org/dnkl/foot/pulls/1564
[`DECBKM`]: https://vt100.net/docs/vt510-rm/DECBKM.html


### Changed

* config: ARGB color values now default to opaque, rather than
  transparent, when the alpha component has been left out
  ([#1526][1526]).
* The `foot` process now changes CWD to `/` after spawning the shell
  process. This ensures the terminal itself does not "lock" a
  directory; for example, preventing a mount point from being
  unmounted ([#1528][1528]).
* Kitty keyboard protocol: updated behavior of modifiers bits during
  modifier key events, to match the (new [#6913][kitty-6913]) behavior
  in kitty >= 0.32.0 ([#1561][1561]).
* When changing font sizes or display scales in floating windows, the
  window will be resized as needed to preserve the same grid size.
* `smm` now disables private mode 1036 (_"send ESC when Meta modifies
  a key"_), and enables private mode 1034 (_"8-bit Meta mode"_). `rmm`
  does the opposite ([#1584][1584]).
* Grid is now always centered in the window, when either fullscreened
  or maximized.
* Ctrl+wheel up/down bound to `font-increase` and `font-decrease`
  respectively (in addition to the already existing default key
  bindings `ctrl-+` and `ctrl+-`).
* Use XRGB pixel format (instead of ARGB) when there is no
  transparency.
* Prefer CSS xcursor names, and fallback to legacy X11 names.
* Kitty keyboard protocol: use the `XKB` mode when retrieving locked
  modifiers, instead of the `GTK` mode. This fixes an issue where some
  key combinations (e.g. Shift+space) produces different results
  depending on the state of e.g. the NumLock key.
* Kitty keyboard protocol: filter out **all** locked modifiers (as
  reported by XKB), rather than hardcoding it to CapsLock only, when
  determining whether a key combination produces text or not.
* CSI-t queries now report pixel values **unscaled**, instead of
  **scaled** ([#1643][1643]).
* Sixel: text cursor is now placed on the last text row touched by the
  sixel, instead of the text row touched by the _upper_ pixel of the
  last sixel ([#chafa-192][chafa-192]).
* Sixel: trailing, fully transparent rows are now trimmed
  ([#chafa-192][chafa-192]).
* `1004` (enable focus in/out events) removed from the `XM` terminfo
  capability. To enable focus in/out, use the `fe` and `fd`
  capabilities instead.
* Tightened the regular expression in the `rv` terminfo capability.
* Tightened the regular expression in the `xr` terminfo capability.
* `DECRQM` queries for private mode 67 ([`DECBKM`]) now reply with mode
  value 4 ("permanently reset") instead of 0 ("not recognized").

[1526]: https://codeberg.org/dnkl/foot/issues/1526
[1528]: https://codeberg.org/dnkl/foot/issues/1528
[1561]: https://codeberg.org/dnkl/foot/issues/1561
[kitty-6913]: https://github.com/kovidgoyal/kitty/issues/6913
[1584]: https://codeberg.org/dnkl/foot/issues/1584
[1643]: https://codeberg.org/dnkl/foot/issues/1643
[chafa-192]: https://github.com/hpjansson/chafa/issues/192


### Fixed

* config: improved validation of color values.
* config: double close of file descriptor, resulting in a chain of
  errors ultimately leading to a startup failure ([#1531][1531]).
* Crash when using a desktop scaling factor > 1, on compositors that
  implements neither the `fractional-scale-v1`, nor the
  `cursor-shape-v1` Wayland protocols ([#1573][1573]).
* Crash in `--server` mode when one or more environment variables are
  set in `[environment]`.
* Environment variables normally set by foot lost with `footclient
  -E,--client-environment` ([#1568][1568]).
* XDG toplevel protocol violation, by trying to set a title that
  contains an invalid UTF-8 sequence ([#1552][1552]).
* Crash when erasing the scrollback, when scrollback history is
  exactly 0 rows. This happens when `[scrollback].line = 0`, and the
  window size (number of rows) is a power of two (i.e. 2, 4, 8, 16
  etc) ([#1610][1610]).
* VS16 (variation selector 16 - emoji representation) should only
  affect emojis.
* Pressing a modifier key while the kitty keyboard protocol is enabled
  no longer resets the viewport, or clears the selection.
* Crash when failing to load an xcursor image ([#1624][1624]).
* Crash when resizing a dynamically sized sixel (no raster
  attributes), with a non-1:1 aspect ratio.
* The default sixel color table is now initialized to the colors used
  by the VT340, instead of not being initialized at all (thus
  requiring the sixel escape sequence to explicitly set all colors it
  used).

[1531]: https://codeberg.org/dnkl/foot/issues/1531
[1573]: https://codeberg.org/dnkl/foot/issues/1573
[1568]: https://codeberg.org/dnkl/foot/issues/1568
[1552]: https://codeberg.org/dnkl/foot/issues/1552
[1610]: https://codeberg.org/dnkl/foot/issues/1610
[1624]: https://codeberg.org/dnkl/foot/issues/1624


### Contributors

* Alyssa Ross
* Andrew J. Hesford
* Artturin
* Craig Barnes
* delthas
* eugenrh
* Fazzi
* Gregory Anders
* Jan Palus
* Leonardo Hernández Hernández
* LmbMaxim
* Matheus Afonso Martins Moreira
* Sivecano
* Tim Culverhouse
* xnuk


## 1.16.2

### Fixed

* Last row and/or column of opaque sixels (not having a size that is a
  multiple of the cell size) being the wrong color ([#1520][1520]).

[1520]: https://codeberg.org/dnkl/foot/issues/1520


## 1.16.1

### Fixed

* Foot not starting on linux kernels before 6.3 ([#1514][1514]).
* Cells underneath erased sixels not being repainted ([#1515][1515]).

[1514]: https://codeberg.org/dnkl/foot/issues/1514
[1515]: https://codeberg.org/dnkl/foot/issues/1515


## 1.16.0

### Added

* Support for building with _wayland-protocols_ as a subproject.
* Mouse wheel scrolls can now be used in `mouse-bindings`
  ([#1077][1077]).
* New mouse bindings: `scrollback-up-mouse` and
  `scrollback-down-mouse`, bound to `BTN_BACK` and `BTN_FORWARD`
  respectively.
* New key binding: `select-quote`. This key binding selects text
  between quote characters, and falls back to selecting the entire
  row ([#1364][1364]).
* Support for DECSET/DECRST/DECRQM 2027 (_Grapheme cluster
  processing_).
* New **search mode** key bindings (along with their defaults)
  ([#419][419]):
  - `extend-char` (shift+right)
  - `extend-line-down` (shift+down)
  - `extend-backward-char` (shift+left)
  - `extend-backward-to-word-boundary` (ctrl+shift+left)
  - `extend-backward-to-next-whitespace` (none)
  - `extend-line-up` (shift+up)
  - `scrollback-up-page` (shift+page-up)
  - `scrollback-up-half-page` (none)
  - `scrollback-up-line` (none)
  - `scrollback-down-page` (shift+page-down)
  - `scrollback-down-half-page` (none)
  - `scrollback-down-line` (none)
* Support for visual bell which flashes the terminal window.
  ([#1337][1337]).

[1077]: https://codeberg.org/dnkl/foot/issues/1077
[1364]: https://codeberg.org/dnkl/foot/issues/1364
[419]: https://codeberg.org/dnkl/foot/issues/419
[1337]: https://codeberg.org/dnkl/foot/issues/1337


### Changed

* Minimum required version of _wayland-protocols_ is now 1.32
  ([#1391][1391]).
* `foot-server.service` systemd now checks for
  `ConditionEnvironment=WAYLAND_DISPLAY` for consistency with the
  socket unit ([#1448][1448])
* Default key binding for `select-row` is now `BTN_LEFT+4`. However,
  in many cases, triple clicking will still be enough to select the
  entire row; see the new key binding `select-quote` (mapped to
  `BTN_LEFT+3` by default) ([#1364][1364]).
* `file://` prefix from URI's are no longer stripped when
  opened/activated ([#1474][1474]).
* `XTGETTCAP` with capabilities that are not properly hex encoded will
  be ignored, instead of echo:ed back to the TTY in an error response.
* Command line configuration overrides are now applied even if the
  configuration file does not exist or can't be
  parsed. ([#1495][1495]).
* Wayland surface damage is now more fine-grained. This should result
  in lower latencies in many use cases, especially on high DPI
  monitors.

[1391]: https://codeberg.org/dnkl/foot/issues/1391
[1448]: https://codeberg.org/dnkl/foot/pulls/1448
[1474]: https://codeberg.org/dnkl/foot/pulls/1474
[1495]: https://codeberg.org/dnkl/foot/pulls/1495


### Removed

* `utempter` config option (was deprecated in 1.15.0).


### Fixed

* Race condition for systemd units start in GNOME and KDE
  ([#1436][1436]).
* One frame being rendered at the wrong scale after being hidden by
  another opaque, maximized window ([#1464][1464]).
* Double-width characters, and grapheme clusters breaking URL
  auto-detection ([#1465][1465]).
* Crash when `XDG_ACTIVATION_TOKEN` is set, but compositor does not
  support XDG activation ([#1493][1493]).
* Crash when compositor calls `fractional_scale::preferred_scale()`
  when there are no monitors (for example, after a monitor has been
  turned off and then back on again) ([#1498][1498]).
* Transparency in margins (padding) not being disabled in fullscreen
  mode ([#1503][1503]).
* Crash when a scrollback search match is in the last column.
* Scrollback search: grapheme clusters not matching correctly.
* Wrong baseline offset for some fonts ([#1511][1511]).

[1436]: https://codeberg.org/dnkl/foot/issues/1436
[1464]: https://codeberg.org/dnkl/foot/issues/1464
[1465]: https://codeberg.org/dnkl/foot/issues/1465
[1493]: https://codeberg.org/dnkl/foot/pulls/1493
[1498]: https://codeberg.org/dnkl/foot/issues/1498
[1503]: https://codeberg.org/dnkl/foot/issues/1503
[1511]: https://codeberg.org/dnkl/foot/issues/1511

### Contributors

* 6t8k
* Alyssa Ross
* CismonX
* Max Gautier
* raggedmyth
* Raimund Sacherer
* Sertonix


## 1.15.3

### Fixed

* `-f,--font` command line option not affecting `csd.font` (if unset).
* Vertical alignment in URL jump labels, and the scrollback position
  indicator. The fix in 1.15.2 was incorrect, and was reverted in the
  last minute. But we forgot to remove the entry from the changelog
  ([#1430][1430]).


## 1.15.2

### Added

* `[tweak].bold-text-in-bright-amount` option ([#1434][1434]).
* `-Dterminfo-base-name` meson option, allowing you to name the
  terminfo files to something other than `-Ddefault-terminfo`. Use
  case: have foot default to using the terminfo from ncurses (`foot`,
  `foot-direct`), while still packaging foot's terminfo files, but
  under a different name (e.g. `foot-extra`, `foot-extra-direct`).

[1434]: https://codeberg.org/dnkl/foot/issues/1434


### Fixed

* Crash when copying text that contains invalid UTF-8 ([#1423][1423]).
* Wrong font size after suspending the monitor ([#1431][1431]).
* Vertical alignment in URL jump labels, and the scrollback position
  indicator ([#1430][1430]).
* Regression: line- and box drawing characters not covering the full
  height of the line, when a custom `line-height` is being used
  ([#1430][1430]).
* Crash when compositor does not implement the _viewporter_ interface
  ([#1444][1444]).
* CSD rendering with fractional scaling ([#1441][1441]).
* Regression: crash with certain combinations of
  `--window-size-chars=NxM` and desktop scaling factors
  ([#1446][1446]).

[1423]: https://codeberg.org/dnkl/foot/issues/1423
[1431]: https://codeberg.org/dnkl/foot/issues/1431
[1430]: https://codeberg.org/dnkl/foot/issues/1430
[1444]: https://codeberg.org/dnkl/foot/issues/1444
[1441]: https://codeberg.org/dnkl/foot/issues/1441
[1446]: https://codeberg.org/dnkl/foot/issues/1446


## 1.15.1

### Changed

* When window is mapped, use metadata (DPI, scaling factor, subpixel
  configuration) from the monitor we were most recently mapped on,
  instead of the one least recently.
* Starlight theme (the default theme) updated to [V4][starlight-v4]
* Background transparency (alpha) is now disabled in fullscreened
  windows ([#1416][1416]).
* Foot server systemd units now use the standard
  graphical-session.target ([#1281][1281]).
* If `$XDG_RUNTIME_DIR/foot-$WAYLAND_DISPLAY.sock` does not exist,
  `footclient` now tries `$XDG_RUNTIME_DIR/foot.sock`, then
  `/tmp/foot.sock`, even if `$WAYLAND_DISPLAY` and/or
  `$XDG_RUNTIME_DIR` are defined ([#1281][1281]).
* Font baseline calculation: try to center the text within the line,
  instead of anchoring it at the top ([#1302][1302]).

[starlight-v4]: https://github.com/CosmicToast/starlight/blob/v4/CHANGELOG.md#v4
[1416]: https://codeberg.org/dnkl/foot/issues/1416
[1281]: https://codeberg.org/dnkl/foot/pulls/1281
[1302]: https://codeberg.org/dnkl/foot/issues/1302


### Fixed

* Use appropriate rounding when applying fractional scales.
* Xcursor not being scaled correctly on `fractional-scale-v1` capable
  compositors.
* `dpi-aware=yes` being broken on `fractional-scale-v1` capable
  compositors (and when a fractional scaling factor is being used)
  ([#1404][1404]).
* Initial font size being wrong on `fractional-scale-v1` capable
  compositors, with multiple monitors with different scaling factors
  connected ([#1404][1404]).
* Crash when _pointer capability_ is removed from a seat, on
  compositors without `cursor-shape-v1 support` ([#1411][1411]).
* Crash on exit, if the mouse is hovering over the foot window (does
  not happen on all compositors)
* Visual glitches when CSD titlebar is transparent.

[1404]: https://codeberg.org/dnkl/foot/issues/1404
[1411]: https://codeberg.org/dnkl/foot/pulls/1411


### Contributors

* Ayush Agarwal
* CismonX
* Max Gautier
* Ronan Pigott
* xdavidwu


## 1.15.0

### Added

* VT: implemented `XTQMODKEYS` query (`CSI ? Pp m`).
* Meson option `utmp-backend=none|libutempter|ulog|auto`. The default
  is `auto`, which will select `libutempter` on Linux, `ulog` on
  FreeBSD, and `none` for all others.
* Sixel aspect ratio.
* Support for the new `fractional-scale-v1` Wayland protocol. This
  brings true fractional scaling to Wayland in general, and with this
  release, to foot.
* Support for the new `cursor-shape-v1` Wayland protocol, i.e. server
  side cursor shapes ([#1379][1379]).
* Support for touchscreen input ([#517][517]).
* `csd.double-click-to-maximize` option to `foot.ini`. Defaults to
  `yes` ([#1293][1293]).

[1379]: https://codeberg.org/dnkl/foot/issues/1379
[517]: https://codeberg.org/dnkl/foot/issues/517
[1293]: https://codeberg.org/dnkl/foot/issues/1293


### Changed

* Default color theme is now
  [starlight](https://github.com/CosmicToast/starlight)
  ([#1321][1321]).
* Minimum required meson version is now 0.59 ([#1371][1371]).
* `Control+Shift+u` is now bound to `unicode-input` instead of
  `show-urls-launch`, to follow the convention established in GTK and
  Qt ([#1183][1183]).
* `show-urls-launch` now bound to `Control+Shift+o` ([#1183][1183]).
* Kitty keyboard protocol: F3 is now encoded as `CSI 13~` instead of
  `CSI R`. The kitty keyboard protocol originally allowed F3 to be
  encoded as `CSI R`, but this was removed from the specification
  since `CSI R` conflicts with the _"Cursor Position Report"_.
* `[main].utempter` renamed to `[main].utmp-helper`. The old option
  name is still recognized, but will log a deprecation warning.
* Meson option `default-utempter-path` renamed to
  `utmp-default-helper-path`.
* Opaque sixels now retain the background opacity (when current
  background color is the **default** background color)
  ([#1360][1360]).
* Text cursor's vertical position after emitting a sixel, when sixel
  scrolling is **enabled** (the default) has been updated to match
  XTerm's, and the VT382's behavior: the cursor is positioned **on**
  the last sixel row, rather than _after_ it. This allows printing
  sixels on the last row without scrolling up, but also means
  applications may have to explicitly emit a newline to ensure the
  sixel is visible. For example, `cat`:ing a sixel in the shell will
  typically result in the last row not being visible, unless a newline
  is explicitly added.
* Default sixel aspect ratio is now 2:1 instead of 1:1.
* Sixel images are no longer cropped to the last non-transparent row.
* Sixel images are now re-scaled when the font size is changed
  ([#1383][1383]).
* `dpi-aware` now defaults to `no`, and the `auto` value has been
  removed.
* When using custom cursor colors (`cursor.color` is set in
  `foot.ini`), the cursor is no longer inverted when the cell is
  selected, or when the cell has the `reverse` (SGR 7) attribute set
  ([#1347][1347]).

[1321]: https://codeberg.org/dnkl/foot/issues/1321
[1371]: https://codeberg.org/dnkl/foot/pulls/1371
[1183]: https://codeberg.org/dnkl/foot/issues/1183
[1360]: https://codeberg.org/dnkl/foot/issues/1360
[1383]: https://codeberg.org/dnkl/foot/issues/1383
[1347]: https://codeberg.org/dnkl/foot/issues/1347


### Deprecated

* `[main].utempter` option.


### Removed

* `auto` value for the `dpi-aware` option.


### Fixed

* Incorrect icon in dock and window switcher on Gnome ([#1317][1317])
* Crash when scrolling after resizing the window with non-zero
  scrolling regions.
* `XTMODKEYS` state not being reset on a terminal reset.
* In Gnome dock foot always groups under "foot client".  Change
  instances of footclient and foot to appear as "foot client" and
  "foot" respectively. ([#1355][1355]).
* Glitchy rendering when alpha (transparency) is changed between
  opaque and non-opaque at runtime (using OSC-11).
* Regression: crash when resizing the window when `resize-delay-ms >
  0` ([#1377][1377]).
* Crash when scrolling up while running something that generates a lot
  of output (for example, `yes`) ([#1380][1380]).
* Default key binding for URL mode conflicting with Unicode input on
  some DEs; `show-urls-launched` is now mapped to `Control+Shift+o` by
  default, instead of `Control+Shift+u` ([#1183][1183]).

[1317]: https://codeberg.org/dnkl/foot/issues/1317
[1355]: https://codeberg.org/dnkl/foot/issues/1355
[1377]: https://codeberg.org/dnkl/foot/issues/1377
[1380]: https://codeberg.org/dnkl/foot/issues/1380


### Contributors

* Antoine Beaupré
* CismonX
* Craig Barnes
* Dan Bungert
* jdevdevdev
* Kyle Gunger
* locture
* Phillip Susi
* sewn
* ShugarSkull
* Vivian Szczepanski
* Vladimir Bauer
* wout
* CosmicToast


## 1.14.0

### Added

* Support for adjusting the thickness of regular underlines ([#1136][1136]).
* Support (optional) for utmp logging with libutempter.
* `kxIN` and `kxOUT` (focus in/out events) to terminfo.
* `name` capability to `XTGETTCAP`.
* String values in `foot.ini` may now be quoted. This can be used to
  set a value to the empty string, for example.
* Environment variables can now be **unset**, by setting
  `[environment].<variable>=""` (quotes are required) ([#1225][1225]).
* `font-size-adjustment=N[px]` option, letting you configure how much
  to increment/decrement the font size when zooming in or out
  ([#1188][1188]).
* Bracketed paste terminfo entries (`BD`, `BE`, `PE` and `PS`, added
  to ncurses in 2022-12-24). Vim makes use of these.
* "Report version" terminfo entries (`XR`/`xr`).
* "Report DA2" terminfo entries (`RV`/`rv`).
* `XF` terminfo capability (focus in/out events available).
* `$TERM_PROGRAM` and `$TERM_PROGRAM_VERSION` environment variables
  unset in the slave process.

[1136]: https://codeberg.org/dnkl/foot/issues/1136
[1225]: https://codeberg.org/dnkl/foot/issues/1225
[1188]: https://codeberg.org/dnkl/foot/issues/1188


### Changed

* Default color theme from a variant of the Zenburn theme, to a
  variant of the Solarized dark theme.
* Default `pad` from 2x2 to 0x0 (i.e. no padding at all).
* Current working directory (as set by OSC-7) is now passed to the
  program executed by the `pipe-*` key bindings ([#1166][1166]).
* `DECRPM` replies (to `DECRQM` queries) now report a value of `4`
  ("permanently reset") instead of `2` ("reset") for DEC private
  modes that are known but unsupported.
* Set `PWD` environment variable in the slave process ([#1179][1179]).
* DPI is now forced to 96 when found to be unreasonably high.
* Set default log level to warning ([#1215][1215]).
* Default `grapheme-width-method` from `wcswidth` to `double-width`.
* When determining initial font size, do FontConfig config
  substitution if the user-provided font pattern has no {pixel}size
  option ([#1287][1287]).
* DECRST of DECCOLM and DECSCLM removed from terminfo.

[1166]: https://codeberg.org/dnkl/foot/issues/1166
[1179]: https://codeberg.org/dnkl/foot/issues/1179
[1215]: https://codeberg.org/dnkl/foot/pulls/1215
[1287]: https://codeberg.org/dnkl/foot/issues/1287


### Fixed

* Crash in `foot --server` on key press, after another `footclient`
  has terminated very early (for example, by trying to launch a
  non-existing shell/client).
* Glitchy rendering when scrolling in the scrollback, on compositors
  that does not allow Wayland buffer reuse (e.g. KDE/plasma)
  ([#1173][1173])
* Scrollback search matches not being highlighted correctly, on
  compositors that does not allow Wayland buffer reuse
  (e.g. KDE/plasma).
* Nanosecs "overflow" when calculating timeout value for
  `resize-delay-ms` option.
* Missing backslash in ST terminator in escape sequences in the
  built-in terminfo (accessed via XTGETTCAP).
* Crash when interactively resizing the window with a very large
  scrollback.
* Crash when a sixel image exceeds the current sixel max height.
* Crash after reverse-scrolling (`CSI Ps T`) in the 'normal'
  (non-alternate) screen ([#1190][1190]).
* Background transparency being applied to the text "behind" the
  cursor. Only applies to block cursor using inversed fg/bg
  colors. ([#1205][1205]).
* Crash when monitor's physical size is "too small" ([#1209][1209]).
* Line-height adjustment when incrementing/decrementing the font size
  with a user-set line-height ([#1218][1218]).
* Scaling factor not being correctly applied when converting pt-or-px
  config values (e.g. letter offsets, line height etc).
* Selection being stuck visually when `IL` and `DL`.
* URL underlines sometimes still being visible after exiting URL mode.
* Text-bindings, and pipe-* bindings, with multiple key mappings
  causing a crash (double-free) on exit ([#1259][1259]).
* Double-width glyphs glitching when surrounded by glyphs overflowing
  into the double-width glyph ([#1256][1256]).
* Wayland protocol violation when ack:ing a configure event for an
  unmapped surface ([#1249][1249]).
* `xdg_toplevel::set_min_size()` not being called.
* Key bindings with consumed modifiers masking other key bindings
  ([#1280][1280]).
* Multi-character compose sequences with the kitty keyboard protocol
  ([#1288][1288]).
* Crash when application output scrolls very fast, e.g. `yes`
  ([#1305][1305]).
* Crash when application scrolls **many** lines (> ~2³¹).
* DECCOLM erasing the screen ([#1265][1265]).

[1173]: https://codeberg.org/dnkl/foot/issues/1173
[1190]: https://codeberg.org/dnkl/foot/issues/1190
[1205]: https://codeberg.org/dnkl/foot/issues/1205
[1209]: https://codeberg.org/dnkl/foot/issues/1209
[1218]: https://codeberg.org/dnkl/foot/issues/1218
[1259]: https://codeberg.org/dnkl/foot/issues/1259
[1256]: https://codeberg.org/dnkl/foot/issues/1256
[1249]: https://codeberg.org/dnkl/foot/issues/1249
[1280]: https://codeberg.org/dnkl/foot/issues/1280
[1288]: https://codeberg.org/dnkl/foot/issues/1288
[1305]: https://codeberg.org/dnkl/foot/issues/1305
[1265]: https://codeberg.org/dnkl/foot/issues/1265


### Contributors

* Alexey Sakovets
* Andrea Pappacoda
* Antoine Beaupré
* argosatcore
* Craig Barnes
* EuCaue
* Grigory Kirillov
* Harri Nieminen
* Hugo Osvaldo Barrera
* jaroeichler
* Joakim Nohlgård
* Nick Hastings
* Soren A D
* Torsten Trautwein
* Vladimír Magyar
* woojiq
* Yorick Peterse


## 1.13.1

### Changed

* Window is now dimmed while in Unicode input mode.


### Fixed

* Compiling against wayland-protocols < 1.25
* Crash on buggy compositors (GNOME) that sometimes send pointer-enter
  events with a NULL surface. Foot now ignores these events, and the
  subsequent motion and leave events.
* Regression: "random" selected empty cells being highlighted as
  selected when they should not.
* Crash when either resizing the terminal window, or scrolling in the
  scrollback history ([#1074][1074])
* OSC-8 URLs with matching IDs, but mismatching URIs being incorrectly
  connected.

[1074]: https://codeberg.org/dnkl/foot/pulls/1074


## 1.13.0

### Added

* XDG activation support when opening URLs ([#1058][1058]).
* `-Dsystemd-units-dir=<path>` meson command line option.
* Support for custom environment variables in `foot.ini`
  ([#1070][1070]).
* Support for jumping to previous/next prompt (requires shell
  integration). By default bound to `ctrl`+`shift`+`z` and
  `ctrl`+`shift`+`x` respectively ([#30][30]).
* `colors.search-box-no-match` and `colors.search-box-match` options
  to `foot.ini` ([#1112][1112]).
* Very basic Unicode input mode via the new
  `key-bindings.unicode-input` and `search-bindings.unicode-input` key
  bindings. Note that there is no visual feedback, as the preferred
  way of entering Unicode characters is with an IME ([#1116][1116]).
* Support for `xdg_toplevel.wm_capabilities`, to adapt the client-side
  decoration buttons to the compositor capabilities ([#1061][1061]).

[1058]: https://codeberg.org/dnkl/foot/issues/1058
[1070]: https://codeberg.org/dnkl/foot/issues/1070
[30]: https://codeberg.org/dnkl/foot/issues/30
[1112]: https://codeberg.org/dnkl/foot/issues/1112
[1116]: https://codeberg.org/dnkl/foot/issues/1116
[1061]: https://codeberg.org/dnkl/foot/pulls/1061


### Changed

* Use `$HOME` instead of `getpwuid()` to retrieve the user's home
  directory when searching for `foot.ini`.
* HT, VT and FF are no longer stripped when pasting in non-bracketed
  mode ([#1084][1084]).
* NUL is now stripped when pasting in non-bracketed mode
  ([#1084][1084]).
* `alt`+`escape` now emits `\E\E` instead of a `CSI 27` sequence
  ([#1105][1105]).

[1084]: https://codeberg.org/dnkl/foot/issues/1084
[1105]: https://codeberg.org/dnkl/foot/issues/1105


### Fixed

* Graphical corruption when viewport is at the top of the scrollback,
  and the output is scrolling.
* Improved text reflow of logical lines with trailing empty cells
  ([#1055][1055])
* IME focus is now tracked independently from keyboard focus.
* Workaround for buggy compositors (e.g. some versions of GNOME)
  allowing drag-and-drops even though foot has reported it does not
  support the offered mime-types ([#1092][1092]).
* Keyboard enter/leave events being ignored if there is no keymap
  ([#1097][1097]).
* Crash when application emitted an invalid `CSI 38;5;<idx>m`, `CSI
  38:5:<idx>m`, `CSI 48;5;<idx>m` or `CSI 48:5:<idx>m` sequence
  ([#1111][1111]).
* Certain dead-key combinations resulting in different escape
  sequences compared to kitty, when the kitty keyboard protocol is
  used ([#1120][1120]).
* Search matches ending with a double-width character not being
  highlighted correctly.
* Selection not being cancelled correctly when scrolled out.
* Extending a multi-page selection behaving inconsistently.
* Poor performance when making very large selections ([#1114][1114]).
* Bogus error message when using systemd socket activation for server
  mode ([#1107][1107])
* Empty line at the bottom after a window resize ([#1108][1108]).

[1055]: https://codeberg.org/dnkl/foot/issues/1055
[1092]: https://codeberg.org/dnkl/foot/issues/1092
[1097]: https://codeberg.org/dnkl/foot/issues/1097
[1111]: https://codeberg.org/dnkl/foot/issues/1111
[1120]: https://codeberg.org/dnkl/foot/issues/1120
[1114]: https://codeberg.org/dnkl/foot/issues/1114
[1107]: https://codeberg.org/dnkl/foot/issues/1107
[1108]: https://codeberg.org/dnkl/foot/issues/1108


### Contributors

* Craig Barnes
* Lorenz
* Max Gautier
* Simon Ser
* Stefan Prosiegel


## 1.12.1

### Added

* Workaround for Sway bug [#6960][sway-6960]: scrollback search and
  the OSC-555 ("flash") escape sequence leaves dimmed (search) and
  yellow (flash) artifacts ([#1046][1046]).
* `Control+Shift+v` and `XF86Paste` have been added to the default set
  of key bindings that paste from the clipboard into the scrollback
  search buffer. This is in addition to the pre-existing `Control+v`
  and `Control+y` bindings.

[sway-6960]: https://github.com/swaywm/sway/issues/6960
[1046]: https://codeberg.org/dnkl/foot/issues/1046


### Changed

* Scrollback search's `extend-to-word-boundary` no longer stops at
  space-to-word boundaries, making selection extension feel more
  natural.


### Fixed

* build: missing symbols when linking the `pgo` helper binary.
* UI not refreshing when pasting something into the scrollback search
  box, that does not result in a grid update (for example, when the
  search criteria did not result in any matches) ([#1040][1040]).
* foot freezing in scrollback search mode, using 100% CPU
  ([#1036][1036], [#1047][1047]).
* Crash when extending a selection to the next word boundary in
  scrollback search mode ([#1036][1036]).
* Scrollback search mode not always highlighting all matches
  correctly.
* Sixel options not being reset on hard resets (`\Ec`)

[1040]: https://codeberg.org/dnkl/foot/issues/1040
[1036]: https://codeberg.org/dnkl/foot/issues/1036
[1047]: https://codeberg.org/dnkl/foot/issues/1047


## 1.12.0

### Added

* OSC-22 - set xcursor pointer.
* Add "xterm" as fallback cursor where "text" is not available.
* `[key-bindings].scrollback-home|end` options.
* Socket activation for `foot --server` and accompanying systemd unit
  files
* Support for re-mapping input, i.e. mapping input to custom escape
  sequences ([#325][325]).
* Support for [DECNKM](https://vt100.net/docs/vt510-rm/DECNKM.html),
  which allows setting/saving/restoring/querying the keypad mode.
* Sixel support can be disabled by setting `[tweak].sixel=no`
  ([#950][950]).
* footclient: `-E,--client-environment` command line option. When
  used, the child process in the new terminal instance inherits the
  environment from the footclient process instead of the server's
  ([#1004][1004]).
* `[csd].hide-when-maximized=yes|no` option ([#1019][1019]).
* Scrollback search mode now highlights all matches.
* `[key-binding].show-urls-persistent` action. This key binding action
  is similar to `show-urls-launch`, but does not automatically exit
  URL mode after activating an URL ([#964][964]).
* Support for `CSI > 4 n`, disable _modifyOtherKeys_. Note that since
  foot only supports level 1 and 2 (and not level 0), this sequence
  does not disable _modifyOtherKeys_ completely, but simply reverts it
  back to level 1 (the default).
* `-Dtests=false|true` meson command line option. When disabled, test
  binaries will neither be built, nor will `ninja test` attempt to
  execute them. Enabled by default ([#919][919]).

[325]: https://codeberg.org/dnkl/foot/issues/325
[950]: https://codeberg.org/dnkl/foot/issues/950
[1004]: https://codeberg.org/dnkl/foot/issues/1004
[1019]: https://codeberg.org/dnkl/foot/issues/1019
[964]: https://codeberg.org/dnkl/foot/issues/964
[919]: https://codeberg.org/dnkl/foot/issues/919


### Changed

* Minimum required meson version is now 0.58.
* Mouse selections are now finalized when the window is resized
  ([#922][922]).
* OSC-4 and OSC-11 replies now uses four digits instead of 2
  ([#971][971]).
* `\r` is no longer translated to `\n` when pasting clipboard data
  ([#980][980]).
* Use circles for rendering light arc box-drawing characters
  ([#988][988]).
* Example configuration is now installed to
  `${sysconfdir}/xdg/foot/foot.ini`, typically resolving to
  `/etc/xdg/foot/foot.ini` ([#1001][1001]).

[922]: https://codeberg.org/dnkl/foot/issues/922
[971]: https://codeberg.org/dnkl/foot/issues/971
[980]: https://codeberg.org/dnkl/foot/issues/980
[988]: https://codeberg.org/dnkl/foot/issues/988
[1001]: https://codeberg.org/dnkl/foot/issues/1001


### Removed

* DECSET mode 27127 (which was first added in release 1.6.0).
  The kitty keyboard protocol (added in release 1.10.3) can
  be used to similar effect.


### Fixed

* Build: missing `wayland_client` dependency in `test-config`
  ([#918][918]).
* "(null)" being logged as font-name (for some fonts) when warning
  about a non-monospaced primary font.
* Rare crash when the window is resized while a mouse selection is
  ongoing ([#922][922]).
* Large selections crossing the scrollback wrap-around ([#924][924]).
* Crash in `pipe-scrollback` ([#926][926]).
* Exit code being 0 when a foot server with no open windows terminate
  due to e.g. a Wayland connection failure ([#943][943]).
* Key binding collisions not detected for bindings specified as option
  overrides on the command line.
* Crash when seat has no keyboard ([#963][963]).
* Key presses with e.g. `AltGr` triggering key combinations with the
  base symbol ([#983][983]).
* Underline cursor sometimes being positioned too low, either making
  it look thinner than what it should be, or being completely
  invisible ([#1005][1005]).
* Fallback to `/etc/xdg` if `XDG_CONFIG_DIRS` is unset
  ([#1008][1008]).
* Improved compatibility with XTerm when `modifyOtherKeys=2`
  ([#1009][1009]).
* Window geometry when CSDs are enabled and CSD border width set to a
  non-zero value. This fixes window snapping in e.g. GNOME.
* Window size "jumping" when starting an interactive resize when CSDs
  are enabled, and CSD border width set to a non-zero value.
* Key binding overrides on the command line having no effect with
  `footclient` instances ([#931][931]).
* Search prev/next not updating the selection correctly when the
  previous and new match overlaps.
* Various minor fixes to scrollback search, and how it finds the
  next/prev match.

[918]: https://codeberg.org/dnkl/foot/issues/918
[922]: https://codeberg.org/dnkl/foot/issues/922
[924]: https://codeberg.org/dnkl/foot/issues/924
[926]: https://codeberg.org/dnkl/foot/issues/926
[943]: https://codeberg.org/dnkl/foot/issues/943
[963]: https://codeberg.org/dnkl/foot/issues/963
[983]: https://codeberg.org/dnkl/foot/issues/983
[1005]: https://codeberg.org/dnkl/foot/issues/1005
[1008]: https://codeberg.org/dnkl/foot/issues/1008
[1009]: https://codeberg.org/dnkl/foot/issues/1009
[931]: https://codeberg.org/dnkl/foot/issues/931


### Contributors

* Ashish SHUKLA
* Craig Barnes
* Enes Hecan
* Johannes Altmanninger
* L3MON4D3
* Leonardo Neumann
* Mariusz Bialonczyk
* Max Gautier
* Merlin Büge
* jvoisin
* merkix


## 1.11.0

### Added

* `[mouse-bindings].selection-override-modifiers` option, specifying
  which modifiers to hold to override mouse grabs by client
  applications and force selection instead.
* _irc://_ and _ircs://_ to the default set of protocols recognized
  when auto-detecting URLs.
* [SGR-Pixels (1016) mouse extended coordinates](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h3-Extended-coordinates) is now supported
  ([#762](https://codeberg.org/dnkl/foot/issues/762)).
* `XTGETTCAP` - builtin terminfo. See
  [README.md::XTGETTCAP](README.md#xtgettcap) for details
  ([#846](https://codeberg.org/dnkl/foot/issues/846)).
* `DECRQSS` - _Request Selection or Setting_
  ([#798](https://codeberg.org/dnkl/foot/issues/798)). Implemented settings
  are:
  - `DECSTBM` - _Set Top and Bottom Margins_
  - `SGR` - _Set Graphic Rendition_
  - `DECSCUSR` - _Set Cursor Style_
* Support for searching for the last searched-for string in scrollback
  search (search for next/prev match with an empty search string).


### Changed

* PaperColorDark and PaperColorLight themes renamed to
  paper-color-dark and paper-color-light, for consistency with other
  theme names.
* `[scrollback].multiplier` is now applied in "alternate scroll" mode,
  where scroll events are translated to fake arrow key presses on the
  alt screen ([#859](https://codeberg.org/dnkl/foot/issues/859)).
* The width of the block cursor's outline in an unfocused window is
  now scaled by the output scaling factor ("desktop
  scaling"). Previously, it was always 1px.
* Foot will now try to change the locale to either "C.UTF-8" or
  "en_US.UTF-8" if started with a non-UTF8 locale. If this fails, foot
  will start, but only to display a window with an error (user's shell
  is not executed).
* `gettimeofday()` has been replaced with `clock_gettime()`, due to it being
  marked as obsolete by POSIX.
* `alt+tab` now emits `ESC \t` instead of `CSI 27;3;9~`
  ([#900](https://codeberg.org/dnkl/foot/issues/900)).
* File pasted, or dropped, on the alt screen is no longer quoted
  ([#379](https://codeberg.org/dnkl/foot/issues/379)).
* Line-based selections now include a trailing newline when copied
  ([#869](https://codeberg.org/dnkl/foot/issues/869)).
* Foot now clears the signal mask and resets all signal handlers to
  their default handlers at startup
  ([#854](https://codeberg.org/dnkl/foot/issues/854)).
* `Copy` and `Paste` keycodes are supported by default for the
  clipboard. These are useful for keyboards with custom firmware like
  QMK to enable global copy/paste shortcuts that work inside and
  outside the terminal (https://codeberg.org/dnkl/foot/pulls/894).


### Removed

* Workaround for slow resize in Sway <= 1.5, when a foot window was
  hidden, for example, in a tabbed view
  (https://codeberg.org/dnkl/foot/pulls/507).


### Fixed

* Font size adjustment ("zooming") when font is configured with a
  **pixelsize**, and `dpi-aware=no`
  ([#842](https://codeberg.org/dnkl/foot/issues/842)).
* Key presses triggering keyboard layout switches also emitting CSI
  codes in the Kitty keyboard protocol.
* Assertion in `shm.c:buffer_release()`
  ([#844](https://codeberg.org/dnkl/foot/issues/844)).
* Crash when setting a key- or mouse binding to the empty string
  ([#851](https://codeberg.org/dnkl/foot/issues/851)).
* Crash when maximizing the window and `[csd].size=1`
  ([#857](https://codeberg.org/dnkl/foot/issues/857)).
* OSC-8 URIs not getting overwritten (erased) by double-width
  characters (e.g. emojis).
* Rendering of CSD borders when `csd.border-width > 0` and desktop
  scaling has been enabled.
* Failure to launch when `exec(3)`:ed with an empty argv.
* Pasting from the primary clipboard (mouse middle clicking) did not
  reset the scrollback view to the bottom.
* Wrong mouse binding triggered when doing two mouse selections in
  very quick (< 300ms) succession
  ([#883](https://codeberg.org/dnkl/foot/issues/883)).
* Bash completion giving an error when completing a list of short
  options
* Sixel: large image resizes (triggered by e.g. large repeat counts in
  `DECGRI`) are now truncated instead of ignored.
* Sixel: a repeat count of 0 in `DECGRI` now emits a single sixel.
* LIGHT ARC box drawing characters incorrectly rendered
  platforms ([#914](https://codeberg.org/dnkl/foot/issues/914)).


### Contributors

* [lamonte](https://codeberg.org/lamonte)
* Érico Nogueira
* feeptr
* Felix Lechner
* grtcdr
* Mark Stosberg
* Nicolai Dagestad
* Oğuz Ersen
* Pranjal Kole
* Simon Ser


## 1.10.3

### Added

* Kitty keyboard protocol ([#319](https://codeberg.org/dnkl/foot/issues/319)):
  - [Report event types](https://sw.kovidgoyal.net/kitty/keyboard-protocol/#report-events)
    (mode `0b10`)
  - [Report alternate keys](https://sw.kovidgoyal.net/kitty/keyboard-protocol/#report-alternates)
    (mode `0b100`)
  - [Report all keys as escape codes](https://sw.kovidgoyal.net/kitty/keyboard-protocol/#report-all-keys)
    (mode `0b1000`)
  - [Report associated text](https://sw.kovidgoyal.net/kitty/keyboard-protocol/#report-text)
    (mode `0b10000`)


### Fixed

* Crash when bitmap fonts are scaled down to very small font sizes
  ([#830](https://codeberg.org/dnkl/foot/issues/830)).
* Crash when overwriting/erasing an OSC-8 URL.


## 1.10.2

### Added

* New value, `max`, for `[tweak].grapheme-width-method`.
* Initial support for the [Kitty keyboard protocol](https://sw.kovidgoyal.net/kitty/keyboard-protocol/).
  Modes supported:
  - [Disambiguate escape codes](https://sw.kovidgoyal.net/kitty/keyboard-protocol/#disambiguate) (mode `0b1`)
* "Window menu" (compositor provided) on right clicks on the CSD title
  bar.


### Fixed

* An ongoing mouse selection is now finalized on a pointer leave event
  (for example by switching workspace while doing a mouse selection).
* OSC-8 URIs in the last column
* OSC-8 URIs sometimes being applied to too many, and seemingly
  unrelated cells ([#816](https://codeberg.org/dnkl/foot/issues/816)).
* OSC-8 URIs incorrectly being dropped when resizing the terminal
  window with the alternate screen active.
* CSD border not being dimmed when window is not focused.
* Visual corruption with large CSD borders
  ([#823](https://codeberg.org/dnkl/foot/issues/823)).
* Mouse cursor shape sometimes not being updated correctly.
* Color palette changes (via OSC 4/104) no longer affect RGB colors
  ([#678](https://codeberg.org/dnkl/foot/issues/678)).


### Contributors

* Jonas Ådahl


## 1.10.1

### Added

* `-Dthemes=false|true` meson command line option. When disabled,
  example theme files are **not** installed.
* XDG desktop file for footclient.


### Fixed

* Regression: `letter-spacing` resulting in a "not a valid option"
  error ([#795](https://codeberg.org/dnkl/foot/issues/795)).
* Regression: bad section name in configuration error messages.
* Regression: `pipe-*` key bindings not being parsed correctly,
  resulting in invalid error messages
  ([#809](https://codeberg.org/dnkl/foot/issues/809)).
* OSC-8 data not being cleared when cell is overwritten
  ([#804](https://codeberg.org/dnkl/foot/issues/804),
  [#801](https://codeberg.org/dnkl/foot/issues/801)).


### Contributors

* Arnavion
* Craig Barnes
* Soc Virnyl Silab Estela
* Xiretza


## 1.10.0

### Added

* `notify-focus-inhibit` boolean option, which can be used to control
  whether desktop notifications should be inhibited when the terminal
  has keyboard focus
* `[colors].scrollback-indicator` color-pair option, which specifies
  foreground and background colors for the scrollback indicator.
* `[key-bindings].noop` action. Key combinations assigned to this
  action will not be sent to the application
  ([#765](https://codeberg.org/dnkl/foot/issues/765)).
* Color schemes are now installed to `${datadir}/foot/themes`.
* `[csd].border-width` and `[csd].border-color`, allowing you to
  configure the width and color of the CSD border.
* Support for `XTMODKEYS` with `Pp=4` and `Pv=2` (_modifyOtherKeys=2_).
* `[colors].dim0-7` options, allowing you to configure custom "dim"
  colors ([#776](https://codeberg.org/dnkl/foot/issues/776)).


### Changed

* `[tweak].grapheme-shaping` is now enabled by default when both foot
  itself, and fcft has been compiled with support for it.
* Default value of `[tweak].grapheme-width-method` changed from
  `double-width` to `wcswidth`.
* INSTALL.md: `--override tweak.grapheme-shaping=no` added to PGO
  command line.
* Foot now terminates if there are no available seats - for example,
  due to the compositor not implementing a recent enough version of
  the `wl_seat` interface ([#779](https://codeberg.org/dnkl/foot/issues/779)).
* Boolean options in `foot.ini` are now limited to
  "yes|true|on|1|no|false|off|0", Previously, anything that did not
  match "yes|true|on", or a number greater than 0, was treated as
  "false".
* `[scrollback].multiplier` is no longer applied when the alternate
  screen is in use ([#787](https://codeberg.org/dnkl/foot/issues/787)).


### Removed

* The bundled PKGBUILD.
* Deprecated `bell` option (replaced with `[bell]` section in 1.8.0).
* Deprecated `url-launch`, `jump-label-letters` and `osc8-underline`
  options (moved to a dedicated `[url]` section in 1.8.0)


### Fixed

* 'Sticky' modifiers in input handling; when determining modifier
  state, foot was looking at **depressed** modifiers, not
  **effective** modifiers, like it should.
* Fix crashes after enabling CSD at runtime when `csd.size` is 0.
* Convert `\r` to `\n` when reading clipboard data
  ([#752](https://codeberg.org/dnkl/foot/issues/752)).
* Clipboard occasionally ceasing to work, until window has been
  re-focused ([#753](https://codeberg.org/dnkl/foot/issues/753)).
* Don't propagate window title updates to the Wayland compositor
  unless the new title is different from the old title.


### Contributors

* armin
* Craig Barnes
* Daniel Martí
* feeptr
* Mitja Horvat
* Ronan Pigott
* Stanislav Ochotnický


## 1.9.2

### Changed

* PGO helper scripts no longer set `LC_CTYPE=en_US.UTF-8`. But, note
  that "full" PGO builds still **require** a UTF-8 locale; you need
  to set one manually in your build script
  ([#728](https://codeberg.org/dnkl/foot/issues/728)).


## 1.9.1

### Added

* Warn when it appears the primary font is not monospaced. Can be
  disabled by setting `[tweak].font-monospace-warn=no`
  ([#704](https://codeberg.org/dnkl/foot/issues/704)).
* PGO build scripts, in the `pgo` directory. See INSTALL.md -
  _Performance optimized, PGO_, for details
  ([#701](https://codeberg.org/dnkl/foot/issues/701)).
* Braille characters (U+2800 - U+28FF) are now rendered by foot
  itself ([#702](https://codeberg.org/dnkl/foot/issues/702)).
* `-e` command-line option. This option is simply ignored, to appease
  program launchers that blindly pass `-e` to any terminal emulator
  ([#184](https://codeberg.org/dnkl/foot/issues/184)).


### Changed

* `-Ddefault-terminfo` is now also applied to the generated terminfo
  definitions when `-Dterminfo=enabled`.
* `-Dcustom-terminfo-install-location` no longer accepts `no` as a
  special value, to disable exporting `TERMINFO`. To achieve the same
  result, simply don't set it at all. If it _is_ set, `TERMINFO` is
  still exported, like before.
* The default install location for the terminfo definitions have been
  changed back to `${datadir}/terminfo`.
* `dpi-aware=auto`: fonts are now scaled using the monitor's DPI only
  when **all** monitors have a scaling factor of one
  ([#714](https://codeberg.org/dnkl/foot/issues/714)).
* fcft >= 3.0.0 in now required.


### Fixed

* Added workaround for GNOME bug where multiple button press events
  (for the same button) is sent to the CSDs without any release or
  leave events in between ([#709](https://codeberg.org/dnkl/foot/issues/709)).
* Line-wise selection not taking soft line-wrapping into account
  ([#726](https://codeberg.org/dnkl/foot/issues/726)).


### Contributors

* [craigbarnes](https://codeberg.org/craigbarnes)
* Arnavion


## 1.9.0

### Added

* Window title in the CSDs
  ([#638](https://codeberg.org/dnkl/foot/issues/638)).
* `-Ddocs=disabled|enabled|auto` meson command line option.
* Support for `~`-expansion in the `include` directive
  ([#659](https://codeberg.org/dnkl/foot/issues/659)).
* Unicode 13 characters U+1FB3C - U+1FB6F, U+1FB9A and U+1FB9B to list
  of box drawing characters rendered by foot itself (rather than using
  font glyphs) ([#474](https://codeberg.org/dnkl/foot/issues/474)).
* `XM`+`xm` to terminfo.
* Mouse buttons 6/7 (mouse wheel left/right).
* `url.uri-characters` option to `foot.ini`
  ([#654](https://codeberg.org/dnkl/foot/issues/654)).


### Changed

* Terminfo files can now co-exist with the foot terminfo files from
  ncurses. See `INSTALL.md` for more information
  ([#671](https://codeberg.org/dnkl/foot/issues/671)).
* `bold-text-in-bright=palette-based` now only brightens colors from palette
* Raised grace period between closing the PTY and sending `SIGKILL` (when
  terminating the client application) from 4 to 60 seconds.
* When terminating the client application, foot now sends `SIGTERM` immediately
  after closing the PTY, instead of waiting 2 seconds.
* Foot now sends `SIGTERM`/`SIGKILL` to the client application's process group,
  instead of just to the client application's process.
* `kmous` terminfo capability from `\E[M` to `\E[<`.
* pt-or-px values (`letter-spacing`, etc) and the line thickness
  (`tweak.box-drawing-base-thickness`) in box drawing characters are
  now translated to pixel values using the monitor's scaling factor
  when `dpi-aware=no`, or `dpi-aware=auto` and the scaling factor is
  larger than 1 ([#680](https://codeberg.org/dnkl/foot/issues/680)).
* Spawning a new terminal with a working directory that does not exist
  is no longer a fatal error.


### Removed

* `km`/`smm`/`rmm` from terminfo; foot prefixes Alt-key combinations
  with `ESC`, and not by setting the 8:th "meta" bit, regardless of
  `smm`/`rmm`. While this _can_ be disabled by, resetting private mode
  1036, the terminfo should reflect the **default** behavior
  ([#670](https://codeberg.org/dnkl/foot/issues/670)).
* Keypad application mode keys from terminfo; enabling the keypad
  application mode is not enough to make foot emit these sequences -
  you also need to disable private mode 1035
  ([#670](https://codeberg.org/dnkl/foot/issues/670)).


### Fixed

* Rendering into the right margin area with `tweak.overflowing-glyphs`
  enabled.
* PGO builds with clang ([#642](https://codeberg.org/dnkl/foot/issues/642)).
* Crash in scrollback search mode when selection has been canceled due
  to terminal content updates
  ([#644](https://codeberg.org/dnkl/foot/issues/644)).
* Foot process not terminating when the Wayland connection is broken
  ([#651](https://codeberg.org/dnkl/foot/issues/651)).
* Output scale being zero on compositors that does not advertise a
  scaling factor.
* Slow-to-terminate client applications causing other footclient instances to
  freeze when closing a footclient window.
* Underlying cell content showing through in the left-most column of
  sixels.
* `cursor.blink` not working in GNOME
  ([#686](https://codeberg.org/dnkl/foot/issues/686)).
* Blinking cursor stops blinking, or becoming invisible, when
  switching focus from, and then back to a terminal window on GNOME
  ([#686](https://codeberg.org/dnkl/foot/issues/686)).


### Contributors

* Nihal Jere
* [nowrep](https://codeberg.org/nowrep)
* [clktmr](https://codeberg.org/clktmr)


## 1.8.2

### Added

* `locked-title=no|yes` to `foot.ini`
  ([#386](https://codeberg.org/dnkl/foot/issues/386)).
* `tweak.overflowing-glyphs` option, which can be enabled to fix rendering
  issues with glyphs of any width that appear cut-off
  ([#592](https://codeberg.org/dnkl/foot/issues/592)).


### Changed

* Non-empty lines are now considered to have a hard linebreak,
  _unless_ an actual word-wrap is inserted.
* Setting `DECSDM` now _disables_ sixel scrolling, while resetting it
  _enables_ scrolling ([#631](https://codeberg.org/dnkl/foot/issues/631)).


### Removed

* The `tweak.allow-overflowing-double-width-glyphs` and
  `tweak.pua-double-width` options (which have been superseded by
  `tweak.overflowing-glyphs`).


### Fixed

* FD exhaustion when repeatedly entering/exiting URL mode with many
  URLs.
* Double free of URL while removing duplicated and/or overlapping URLs
  in URL mode ([#627](https://codeberg.org/dnkl/foot/issues/627)).
* Crash when an unclosed OSC-8 URL ran into un-allocated scrollback
  rows.
* Some box-drawing characters were rendered incorrectly on big-endian
  architectures.
* Crash when resizing the window to the smallest possible size while
  scrollback search is active.
* Scrollback indicator being incorrectly rendered when window size is
  very small.
* Reduced memory usage in URL mode.
* Crash when the `E3` escape (`\E[3J`) was executed, and there was a
  selection, or sixel image, in the scrollback
  ([#633](https://codeberg.org/dnkl/foot/issues/633)).


### Contributors

* [clktmr](https://codeberg.org/clktmr)


## 1.8.1

### Added

* `--log-level=none` command-line option.
* `Tc`, `setrgbf` and `setrgbb` capabilities in `foot` and `foot-direct`
  terminfo entries. This should make 24-bit RGB colors work in tmux and
  neovim, without the need for config hacks or detection heuristics
  ([#615](https://codeberg.org/dnkl/foot/issues/615)).


### Changed

* Grapheme cluster width is now limited to two cells by default. This
  may cause cursor synchronization issues with many applications. You
  can set `[tweak].grapheme-width-method=wcswidth` to revert to the
  behavior in foot-1.8.0.


### Fixed

* Grapheme cluster state being reset between codepoints.
* Regression: custom URL key bindings not working
  ([#614](https://codeberg.org/dnkl/foot/issues/614)).


### Contributors

* [craigbarnes](https://codeberg.org/craigbarnes)


## 1.8.0

### Grapheme shaping

This release adds _experimental, opt-in_ support for grapheme cluster
segmentation and grapheme shaping.

(note: several of the examples below may not render correctly in your
browser, viewer or editor).

Grapheme cluster segmentation is the art of splitting up text into
grapheme clusters, where a cluster may consist of more than one
Unicode codepoint. For example, 🙂 is a single codepoint, while 👩🏽‍🚀
consists of 4 codepoints (_Woman_ + _Medium skin tone_ + _Zero width
joiner_ + _Rocket_). The goal is to _cluster_ codepoints belonging to
the same grapheme in the same cell in the terminal.

Previous versions of foot implemented a simple grapheme cluster
segmentation technique that **only** handled zero-width
codepoints. This allowed us to cluster combining characters, like q́
(_q_ + _COMBINING ACUTE ACCENT_).

Once we have a grapheme cluster, we need to _shape_ it.

Combining characters are simple: they are typically rendered as
multiple glyphs layered on top of each other. This is why previous
versions of foot got away with it without any actual text shaping
support.

Beyond that, support from the font library is needed. Foot now depends
on fcft-2.4, which added support for grapheme and text shaping. When
rendering a cell, we ask the font library: give us the glyph(s) for
this sequence of codepoints.

Fancy emoji sequences aside, using libutf8proc for grapheme cluster
segmentation means **improved correctness**.

For full support, the following is required:

* fcft compiled with HarfBuzz support
* foot compiled with libutf8proc support
* `tweak.grapheme-shaping=yes` in `foot.ini`

If `tweak.grapheme-shaping` has **not** been enabled, foot will
neither use libutf8proc to do grapheme cluster segmentation, nor will
it use fcft's grapheme shaping capabilities to shape combining
characters.

This feature is _experimental_ mostly due to the "wcwidth" problem;
how many cells should foot allocate for a grapheme cluster? While the
answer may seem simple, the problem is that, whatever the answer is,
the client application **must** come up with the **same**
answer. Otherwise we get cursor synchronization issues.

In this release, foot simply adds together the `wcwidth()` of all
codepoints in the grapheme cluster. This is equivalent to running
`wcswidth()` on the entire cluster. **This is likely to change in the
future**.

Finally, note that grapheme shaping is not the same thing as text (or
text run) shaping. In this version, foot only shapes individual
graphemes, not entire text runs. That means e.g. ligatures are **not**
supported.


### Added

* Support for DECSET/DECRST 2026, as an alternative to the existing
  "synchronized updates" DCS sequences
  ([#459](https://codeberg.org/dnkl/foot/issues/459)).
* `cursor.beam-thickness` option to `foot.ini`
  ([#464](https://codeberg.org/dnkl/foot/issues/464)).
* `cursor.underline-thickness` option to `foot.ini`
  ([#524](https://codeberg.org/dnkl/foot/issues/524)).
* Unicode 13 characters U+1FB70 - U+1FB8B to list of box drawing
  characters rendered by foot itself (rather than using font glyphs)
  ([#471](https://codeberg.org/dnkl/foot/issues/471)).
* Dedicated `[bell]` section to config, supporting multiple actions
  and a new `command` action to run an arbitrary command.
  (https://codeberg.org/dnkl/foot/pulls/483)
* Dedicated `[url]` section to config.
* `[url].protocols` option to `foot.ini`
  ([#531](https://codeberg.org/dnkl/foot/issues/531)).
* Support for setting the full 256 color palette in foot.ini
  ([#489](https://codeberg.org/dnkl/foot/issues/489))
* XDG activation support, will be used by `[bell].urgent` when
  available (falling back to coloring the window margins red when
  unavailable) ([#487](https://codeberg.org/dnkl/foot/issues/487)).
* `ctrl`+`c` as a default key binding; to cancel search/url mode.
* `${window-title}` to `notify`.
* Support for including files in `foot.ini`
  ([#555](https://codeberg.org/dnkl/foot/issues/555)).
* `ENVIRONMENT` section in **foot**(1) and **footclient**(1) man pages
  ([#556](https://codeberg.org/dnkl/foot/issues/556)).
* `tweak.pua-double-width` option to `foot.ini`, letting you force
  _Private Usage Area_ codepoints to be treated as double-width
  characters.
* OSC 9 desktop notifications (iTerm2 compatible).
* Support for LS2 and LS3 (locking shift) escape sequences
  ([#581](https://codeberg.org/dnkl/foot/issues/581)).
* Support for overriding configuration options on the command line
  ([#554](https://codeberg.org/dnkl/foot/issues/554),
  [#600](https://codeberg.org/dnkl/foot/issues/600)).
* `underline-offset` option to `foot.ini`
  ([#490](https://codeberg.org/dnkl/foot/issues/490)).
* `csd.button-color` option to `foot.ini`.
* `-Dterminfo-install-location=disabled|<custom-path>` meson command
  line option ([#569](https://codeberg.org/dnkl/foot/issues/569)).


### Changed

* [fcft](https://codeberg.org/dnkl/fcft): required version bumped from
  2.3.x to 2.4.x.
* `generate-alt-random-writes.py --sixel`: width and height of emitted
  sixels has been adjusted.
* _Concealed_ text (`\E[8m`) is now revealed when highlighted.
* The background color of highlighted text is now adjusted, when the
  foreground and background colors are the same, making the
  highlighted text legible
  ([#455](https://codeberg.org/dnkl/foot/issues/455)).
* `cursor.style=bar` to `cursor.style=beam`. `bar` remains a
  recognized value, but will eventually be deprecated, and removed.
* Point values in `line-height`, `letter-spacing`,
  `horizontal-letter-offset` and `vertical-letter-offset` are now
  rounded, not truncated, when translated to pixel values.
* Foot's exit code is now -26/230 when foot itself failed to launch
  (due to invalid command line options, client application/shell not
  found etc). Footclient's exit code is -36/220 when it itself fails
  to launch (e.g. bad command line option) and -26/230 when the foot
  server failed to instantiate a new window
  ([#466](https://codeberg.org/dnkl/foot/issues/466)).
* Background alpha no longer applied to palette or RGB colors that
  matches the background color.
* Improved performance on compositors that does not release shm
  buffers immediately, e.g. KWin
  ([#478](https://codeberg.org/dnkl/foot/issues/478)).
* `ctrl + w` (_extend-to-word-boundary_) can now be used across lines
  ([#421](https://codeberg.org/dnkl/foot/issues/421)).
* Ignore auto-detected URLs that overlap with OSC-8 URLs.
* Default value for the `notify` option to use `-a ${app-id} -i
  ${app-id} ...` instead of `-a foot -i foot ...`.
* `scrollback-*`+`pipe-scrollback` key bindings are now passed through
  to the client application when the alt screen is active
  ([#573](https://codeberg.org/dnkl/foot/issues/573)).
* Reverse video (`\E[?5h`) now only swaps the default foreground and
  background colors. Cells with explicit foreground and/or background
  colors remain unchanged.
* Tabs (`\t`) are now preserved when the window is resized, and when
  copying text ([#508](https://codeberg.org/dnkl/foot/issues/508)).
* Writing a sixel on top of another sixel no longer erases the first
  sixel, but the two are instead blended
  ([#562](https://codeberg.org/dnkl/foot/issues/562)).
* Running foot without a configuration file is no longer an error; it
  has been demoted to a warning, and is no longer presented as a
  notification in the terminal window, but only logged on stderr.


### Deprecated

* `bell` option in `foot.ini`; set actions in the `[bell]` section
  instead.
* `url-launch` option in `foot.ini`; use `launch` in the `[url]`
  section instead.
* `jump-label-letters` option in `foot.ini`; use `label-letters` in
  the `[url]` section instead.
* `osc8-underline` option in `foot.ini`; use `osc8-underline` in the
  `[url]` section instead.


### Removed

* Buffer damage quirk for Plasma/KWin.


### Fixed

* `generate-alt-random-writes.py --sixel` sometimes crashing,
  resulting in PGO build failures.
* Wrong colors in the 256-color cube
  ([#479](https://codeberg.org/dnkl/foot/issues/479)).
* Memory leak triggered by "opening" an OSC-8 URI and then resetting
  the terminal without closing the URI
  ([#495](https://codeberg.org/dnkl/foot/issues/495)).
* Assertion when emitting a sixel occupying the entire scrollback
  history ([#494](https://codeberg.org/dnkl/foot/issues/494)).
* Font underlines being positioned below the cell (and thus being
  invisible) for certain combinations of fonts and font sizes
  ([#503](https://codeberg.org/dnkl/foot/issues/503)).
* Sixels with transparent bottom border being resized below the size
  specified in _"Set Raster Attributes"_.
* Fonts sometimes not being reloaded with the correct scaling factor
  when `dpi-aware=no`, or `dpi-aware=auto` with monitor(s) with a
  scaling factor > 1 ([#509](https://codeberg.org/dnkl/foot/issues/509)).
* Crash caused by certain CSI sequences with very large parameter
  values ([#522](https://codeberg.org/dnkl/foot/issues/522)).
* Rare occurrences where the window did not close when the shell
  exited. Only seen on FreeBSD
  ([#534](https://codeberg.org/dnkl/foot/issues/534))
* Foot process(es) sometimes remaining, using 100% CPU, when closing
  multiple foot windows at the same time
  ([#542](https://codeberg.org/dnkl/foot/issues/542)).
* Regression where `<mod>+shift+tab` always produced `\E[Z` instead of
  the correct `\E[27;<mod>;9~` sequence
  ([#547](https://codeberg.org/dnkl/foot/issues/547)).
* Crash when a line wrapping OSC-8 URI crossed the scrollback wrap
  around ([#552](https://codeberg.org/dnkl/foot/issues/552)).
* Selection incorrectly wrapping rows ending with an explicit newline
  ([#565](https://codeberg.org/dnkl/foot/issues/565)).
* Off-by-one error in markup of auto-detected URLs when the URL ends
  in the right-most column.
* Multi-column characters being cut in half when resizing the
  alternate screen.
* Restore `SIGHUP` in spawned processes.
* Text reflow performance ([#504](https://codeberg.org/dnkl/foot/issues/504)).
* IL+DL (`CSI Ps L` + `CSI Ps M`) now moves the cursor to column 0.
* SS2 and SS3 (single shift) escape sequences behaving like locking
  shifts ([#580](https://codeberg.org/dnkl/foot/issues/580)).
* `TEXT`+`STRING`+`UTF8_STRING` mime types not being recognized in
  clipboard offers ([#583](https://codeberg.org/dnkl/foot/issues/583)).
* Memory leak caused by custom box drawing glyphs not being completely
  freed when destroying a foot window instance
  ([#586](https://codeberg.org/dnkl/foot/issues/586)).
* Crash in scrollback search when current XKB layout is missing
  _compose_ definitions.
* Window title not being updated while window is hidden
  ([#591](https://codeberg.org/dnkl/foot/issues/591)).
* Crash on badly formatted URIs in e.g. OSC-8 URLs.
* Window being incorrectly resized on CSD/SSD run-time changes.


### Contributors
* [r\_c\_f](https://codeberg.org/r_c_f)
* [craigbarnes](https://codeberg.org/craigbarnes)


## 1.7.2

### Added

* URxvt OSC-11 extension to set background alpha
  ([#436](https://codeberg.org/dnkl/foot/issues/436)).
* OSC 17/117/19/119 - change/reset selection background/foreground
  color.
* `box-drawings-uses-font-glyphs=yes|no` option to `foot.ini`
  ([#430](https://codeberg.org/dnkl/foot/issues/430)).


### Changed

* Underline cursor is now rendered below text underline
  ([#415](https://codeberg.org/dnkl/foot/issues/415)).
* Foot now tries much harder to keep URL jump labels inside the window
  geometry ([#443](https://codeberg.org/dnkl/foot/issues/443)).
* `bold-text-in-bright` may now be set to `palette-based`, in which
  case it will use the corresponding bright palette color when the
  color to brighten matches one of the base 8 colors, instead of
  increasing the luminance
  ([#449](https://codeberg.org/dnkl/foot/issues/449)).


### Fixed

* Reverted _"Consumed modifiers are no longer sent to the client
  application"_ ([#425](https://codeberg.org/dnkl/foot/issues/425)).
* Crash caused by a double free originating in `XTSMGRAPHICS` - set
  number of color registers
  ([#427](https://codeberg.org/dnkl/foot/issues/427)).
* Wrong action referenced in error message for key binding collisions
  ([#432](https://codeberg.org/dnkl/foot/issues/432)).
* OSC 4/104 out-of-bounds accesses to the color table. This was the
  reason pywal turned foot windows transparent
  ([#434](https://codeberg.org/dnkl/foot/issues/434)).
* PTY not being drained when the client application terminates.
* `auto_left_margin` not being limited to `cub1`
  ([#441](https://codeberg.org/dnkl/foot/issues/441)).
* Crash in scrollback search mode when searching beyond the last output.


### Contributors

* [cglogic](https://codeberg.org/cglogic)


## 1.7.1

### Changed

* Update PGO build instructions in `INSTALL.md`
  ([#418](https://codeberg.org/dnkl/foot/issues/418)).
* In scrollback search mode, empty cells can now be matched by spaces.


### Fixed

* Logic that repairs invalid key bindings ended up breaking valid key
  bindings instead ([#407](https://codeberg.org/dnkl/foot/issues/407)).
* Custom `line-height` settings now scale when increasing or
  decreasing the font size at run-time.
* Newlines sometimes incorrectly inserted into copied text
  ([#410](https://codeberg.org/dnkl/foot/issues/410)).
* Crash when compositor send `text-input-v3::enter` events without
  first having sent a `keyboard::enter` event
  ([#411](https://codeberg.org/dnkl/foot/issues/411)).
* Deadlock when rendering sixel images.
* URL labels, scrollback search box or scrollback position indicator
  sometimes not showing up, caused by invalidly sized surface buffers
  when output scaling was enabled
  ([#409](https://codeberg.org/dnkl/foot/issues/409)).
* Empty sixels resulted in non-empty images.


## 1.7.0

### Added

* The `pad` option now accepts an optional third argument, `center`
  (e.g. `pad=5x5 center`), causing the grid to be centered in the
  window, with equal amount of padding of the left/right and
  top/bottom side ([#273](https://codeberg.org/dnkl/foot/issues/273)).
* `line-height`, `letter-spacing`, `horizontal-letter-offset` and
  `vertical-letter-offset` to `foot.ini`. These options let you tweak
  cell size and glyph positioning
  ([#244](https://codeberg.org/dnkl/foot/issues/244)).
* Key/mouse binding `select-extend-character-wise`, which forces the
  selection mode to 'character-wise' when extending a selection.
* `DECSET` `47`, `1047` and `1048`.
* URL detection and OSC-8 support. URLs are highlighted and activated
  using the keyboard (**no** mouse support). See **foot**(1)::URLs, or
  [README.md](README.md#urls) for details
  ([#14](https://codeberg.org/dnkl/foot/issues/14)).
* `-d,--log-level={info|warning|error}` to both `foot` and
  `footclient` ([#337](https://codeberg.org/dnkl/foot/issues/337)).
* `-D,--working-directory=DIR` to both `foot` and `footclient`
  ([#347](https://codeberg.org/dnkl/foot/issues/347))
* `DECSET 80` - sixel scrolling
  ([#361](https://codeberg.org/dnkl/foot/issues/361)).
* `DECSET 1070` - sixel private color palette
  ([#362](https://codeberg.org/dnkl/foot/issues/362)).
* `DECSET 8452` - position cursor to the right of sixels
  ([#363](https://codeberg.org/dnkl/foot/issues/363)).
* Man page **foot-ctlseqs**(7), documenting all supported escape
  sequences ([#235](https://codeberg.org/dnkl/foot/issues/235)).
* Support for transparent sixels (DCS parameter `P2=1`)
  ([#391](https://codeberg.org/dnkl/foot/issues/391)).
* `-N,--no-wait` to `footclient`
  ([#395](https://codeberg.org/dnkl/foot/issues/395)).
* Completions for Bash shell
  ([#10](https://codeberg.org/dnkl/foot/issues/10)).
* Implement `XTVERSION` (`CSI > 0q`). Foot will reply with
  `DCS>|foot(<major>.<minor>.<patch>)ST`
  ([#359](https://codeberg.org/dnkl/foot/issues/359)).


### Changed

* The fcft and tllist library subprojects are now handled via Meson
  [wrap files](https://mesonbuild.com/Wrap-dependency-system-manual.html)
  instead of needing to be manually cloned.
* Box drawing characters are now rendered by foot, instead of using
  font glyphs ([#198](https://codeberg.org/dnkl/foot/issues/198))
* Double- or triple clicking then dragging now extends the selection
  word- or line-wise ([#267](https://codeberg.org/dnkl/foot/issues/267)).
* The line thickness of box drawing characters now depend on the font
  size ([#281](https://codeberg.org/dnkl/foot/issues/281)).
* Extending a word/line-wise selection now uses the original selection
  mode instead of switching to character-wise.
* While doing an interactive resize of a foot window, foot now
  requires 100ms of idle time (where the window size does not change)
  before sending the new dimensions to the client application. The
  timing can be tweaked, or completely disabled, by setting
  `resize-delay-ms` ([#301](https://codeberg.org/dnkl/foot/issues/301)).
* `CSI 13 ; 2 t` now reports (0,0).
* Key binding matching logic; key combinations like `Control+Shift+C`
  **must** now be written as either `Control+C` or `Control+Shift+c`,
  the latter being the preferred
  variant. ([#376](https://codeberg.org/dnkl/foot/issues/376))
* Consumed modifiers are no longer sent to the client application
  ([#376](https://codeberg.org/dnkl/foot/issues/376)).
* The minimum version requirement for the libxkbcommon dependency is
  now 1.0.0.
* Empty pixel rows at the bottom of a sixel is now trimmed.
* Sixels with DCS parameter `P2=0|2` now use the _current_ ANSI
  background color for empty pixels instead of the default background
  color ([#391](https://codeberg.org/dnkl/foot/issues/391)).
* Sixel decoding optimized; up to 100% faster in some cases.
* Reported sixel "max geometry" from current window size, to the
  configured maximum size (defaulting to 10000x10000).


### Removed

* The `-g,--geometry` command-line option (which had been deprecated
  and superseded by `-w,--window-size-pixels` since 1.5.0).


### Fixed

* Some mouse bindings (_primary paste_, for example) did not require
  `shift` to be pressed while used in a mouse grabbing
  application. This meant the mouse event was never seen by the
  application.
* Terminals spawned with `ctrl`+`shift`+`n` not terminating when
  exiting shell ([#366](https://codeberg.org/dnkl/foot/issues/366)).
* Default value of `-t,--term` in `--help` output when foot was built
  without terminfo support.
* Drain PTY when the client application terminates.


### Contributors

* [craigbarnes](https://codeberg.org/craigbarnes)
* toast
* [l3mon4d3](https://codeberg.org/l3mon4d3)
* [Simon Schricker](mailto:s.schricker@sillage.at)


## 1.6.4

### Added

* `selection-target=none|primary|clipboard|both` to `foot.ini`. It can
  be used to configure which clipboard(s) selected text should be
  copied to. The default is `primary`, which corresponds to the
  behavior in older foot releases
  ([#288](https://codeberg.org/dnkl/foot/issues/288)).


### Changed

* The IME state no longer stays stuck in the terminal if the IME goes
  away during preedit.
* `-Dterminfo` changed from a `boolean` to a `feature` option.
* Use standard signals instead of a signalfd to handle
  `SIGCHLD`. Fixes an issue on FreeBSD where foot did not detect when
  the client application had terminated.


### Fixed

* `BS`, `HT` and `DEL` from being stripped in bracketed paste mode.


### Contributors

* [tdeo](https://codeberg.org/tdeo)
* jbeich


## 1.6.3

### Added

* Completions for fish shell
  ([#11](https://codeberg.org/dnkl/foot/issues/11))
* FreeBSD support ([#238](https://codeberg.org/dnkl/foot/issues/238)).
* IME popup location support: foot now sends the location of the cursor
  so any popup can be displayed near the text that is being typed.


### Changed

* Trailing comments in `foot.ini` must now be preceded by a space or tab
  ([#270](https://codeberg.org/dnkl/foot/issues/270))
* The scrollback search box no longer accepts non-printable characters.
* Non-formatting C0 control characters, `BS`, `HT` and `DEL` are now
  stripped from pasted text.


### Fixed

* Exit when the client application terminates, not when the TTY file
  descriptor is closed.
* Crash on compositors not implementing the _text input_ interface
  ([#259](https://codeberg.org/dnkl/foot/issues/259)).
* Erased, overflowing glyphs (when
  `tweak.allow-overflowing-double-width-glyphs=yes` - the default) not
  properly erasing the cell overflowed **into**.
* `word-delimiters` option ignores `#` and subsequent characters
  ([#270](https://codeberg.org/dnkl/foot/issues/270))
* Combining characters not being rendered when composed with colored
  bitmap glyphs (i.e. colored emojis).
* Pasting URIs from the clipboard when the source has not
  newline-terminated the last URI
  ([#291](https://codeberg.org/dnkl/foot/issues/291)).
* Sixel "current geometry" query response not being bounded by the
  current window dimensions (fixes `lsix` output)
* Crash on keyboard input when repeat rate was zero (i.e. no repeat).
* Wrong button encoding of mouse buttons 6 and 7 in mouse events.
* Scrollback search not matching composed characters.
* High CPU usage when holding down e.g. arrow keys while in scrollback
  search mode.
* Rendering of composed characters in the scrollback search box.
* IME pre-edit cursor when positioned at the end of the pre-edit
  string.
* Scrollback search not matching multi-column characters.


### Contributors

* [pc](https://codeberg.org/pc)
* [FollieHiyuki](https://codeberg.org/FollieHiyuki)
* jbeich
* [tdeo](https://codeberg.org/tdeo)


## 1.6.2

### Fixed

* Version number in `meson.build`.


## 1.6.1
### Added

* `--seed` to `generate-alt-random.py`, enabling deterministic PGO
  builds.


### Changed


* Use `-std=c11` instead of `-std=c18`.
* Added `-Wno-profile-instr-unprofiled` to Clang cflags in PGO builds
  ([INSTALL.md](https://codeberg.org/dnkl/foot/src/branch/releases/1.6/INSTALL.md#user-content-performance-optimized-pgo))


### Fixed

* Missing dependencies in meson, causing heavily parallelized builds
  to fail.
* Background color when alpha < 1.0 being wrong
  ([#249](https://codeberg.org/dnkl/foot/issues/249)).
* `generate-alt-random.py` failing in containers.


### Contributors

* [craigbarnes](https://codeberg.org/craigbarnes)
* [sterni](https://codeberg.org/sterni)


## 1.6.0

### For packagers

Starting with this release, foot can be PGO:d (compiled using profile
guided optimizations) **without** a running Wayland session. This
means foot can be PGO:d in e.g. sandboxed build scripts. See
[INSTALL.md](INSTALL.md#user-content-performance-optimized-pgo).


### Added

* IME support. This is compile-time optional, see
  [INSTALL.md](INSTALL.md#user-content-options)
  ([#134](https://codeberg.org/dnkl/foot/issues/134)).
* `DECSET` escape to enable/disable IME: `CSI ? 737769 h` enables IME
  and `CSI ? 737769 l` disables it. This can be used to
  e.g. enable/disable IME when entering/leaving insert mode in vim.
* `dpi-aware` option to `foot.ini`. The default, `auto`, sizes fonts
  using the monitor's DPI when output scaling has been
  **disabled**. If output scaling has been **enabled**, fonts are
  sized using the scaling factor. DPI-only font sizing can be forced
  by setting `dpi-aware=yes`. Setting `dpi-aware=no` forces font
  sizing to be based on the scaling factor.
  ([#206](https://codeberg.org/dnkl/foot/issues/206)).
* Implement reverse auto-wrap (_auto\_left\_margin_, _bw_, in
  terminfo). This mode can be enabled/disabled with `CSI ? 45 h` and
  `CSI ? 45 l`. It is **enabled** by default
  ([#150](https://codeberg.org/dnkl/foot/issues/150)).
* `bell` option to `foot.ini`. Can be set to `set-urgency` to make
  foot render the margins in red when receiving `BEL` while **not**
  having keyboard focus. Applications can dynamically enable/disable
  this with the `CSI ? 1042 h` and `CSI ? 1042 l` escape
  sequences. Note that Wayland does **not** implement an _urgency_
  hint like X11, but that there is a
  [proposal](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/9)
  to add support for this. The value `set-urgency` was chosen for
  forward-compatibility, in the hopes that this proposal eventualizes
  ([#157](https://codeberg.org/dnkl/foot/issues/157)).
* `bell` option can also be set to `notify`, in which case a desktop
  notification is emitted when foot receives `BEL` in an unfocused
  window.
* `word-delimiters` option to `foot.ini`
  ([#156](https://codeberg.org/dnkl/foot/issues/156)).
* `csd.preferred` can now be set to `none` to disable window
  decorations. Note that some compositors will render SSDs despite
  this option being used ([#163](https://codeberg.org/dnkl/foot/issues/163)).
* Terminal content is now auto-scrolled when moving the mouse above or
  below the window while selecting
  ([#149](https://codeberg.org/dnkl/foot/issues/149)).
* `font-bold`, `font-italic` `font-bold-italic` options to
  `foot.ini`. These options allow custom bold/italic fonts. They are
  unset by default, meaning the bold/italic version of the regular
  font is used ([#169](https://codeberg.org/dnkl/foot/issues/169)).
* Drag & drop support; text, files and URLs can now be dropped in a
  foot terminal window ([#175](https://codeberg.org/dnkl/foot/issues/175)).
* `clipboard-paste` and `primary-paste` scrollback search bindings. By
  default, they are bound to `ctrl+v ctrl+y` and `shift+insert`
  respectively, and lets you paste from the clipboard or primary
  selection into the search buffer.
* Support for `pipe-*` actions in mouse bindings. It was previously
  not possible to add a command to these actions when used in mouse
  bindings, making them useless
  ([#183](https://codeberg.org/dnkl/foot/issues/183)).
* `bold-text-in-bright` option to `foot.ini`. When enabled, bold text
  is rendered in a brighter color
  ([#199](https://codeberg.org/dnkl/foot/issues/199)).
* `-w,--window-size-pixels` and `-W,--window-size-chars` command line
  options to `footclient` ([#189](https://codeberg.org/dnkl/foot/issues/189)).
* Short command line options for `--title`, `--maximized`,
  `--fullscreen`, `--login-shell`, `--hold` and `--check-config`.
* `DECSET` escape to modify the `escape` key to send `\E[27;1;27~`
  instead of `\E`: `CSI ? 27127 h` enables the new behavior, `CSI ?
  27127 l` disables it (the default).
* OSC 777;notify: desktop notifications. Use in combination with the
  new `notify` option in `foot.ini`
  ([#224](https://codeberg.org/dnkl/foot/issues/224)).
* Status line terminfo capabilities `hs`, `tsl`, `fsl` and `dsl`. This
  enables e.g. vim to set the window title
  ([#242](https://codeberg.org/dnkl/foot/issues/242)).


### Changed

* Blinking text now uses the foreground color, but dimmed down in its
  off state, instead of the background color.
* Sixel default maximum size is now 10000x10000 instead of the current
  window size.
* Graphical glitches/flashes when resizing the window while running a
  fullscreen application, i.e. the 'alt' screen
  ([#221](https://codeberg.org/dnkl/foot/issues/221)).
* Cursor will now blink if **either** `CSI ? 12 h` or `CSI Ps SP q`
  has been used to enable blinking. **cursor.blink** in `foot.ini`
  controls the default state of `CSI Ps SP q`
  ([#218](https://codeberg.org/dnkl/foot/issues/218)).
* The sub-parameter versions of the SGR RGB color escapes (e.g
  `\E[38:2...m`) can now be used _without_ the color space ID
  parameter.
* SGR 21 no longer disables **bold**. According to ECMA-48, SGR 21 is
  _"double underline_". Foot does not (yet) implement that, but that's
  no reason to implement a non-standard behavior.
* `DECRQM` now returns actual state of the requested mode, instead of
  always returning `2`.


### Removed

* Support for loading configuration from `$XDG_CONFIG_HOME/footrc`.
* `scrollback` option from `foot.ini`.
* `geometry` from `foot.ini`.
* Key binding action `scrollback-up` and `scrollback-down`.


### Fixed

* Error when re-assigning a default key binding
  ([#233](https://codeberg.org/dnkl/foot/issues/233)).
* `\E[s`+`\E[u` (save/restore cursor) now saves and restores
  attributes and charset configuration, just like `\E7`+`\E8`.
* Report mouse motion events to the client application also while
  dragging the cursor outside the grid.
* Parsing of the sub-parameter versions of indexed SGR color escapes
  (e.g. `\E[38:5...m`)
* Frames occasionally being rendered while application synchronized
  updates is in effect.
* Handling of failures to parse the font specification string.
* Extra private/intermediate characters in escape sequences not being
  ignored.


### Contributors

* [kennylevinsen](https://codeberg.org/kennylevinsen)
* [craigbarnes](https://codeberg.org/craigbarnes)


## 1.5.4

### Changed


* Num Lock by default overrides the keypad mode. See
  **foot.ini**(5)::KEYPAD, or
  [README.md](README.md#user-content-keypad) for details
  ([#194](https://codeberg.org/dnkl/foot/issues/194)).
* Single-width characters with double-width glyphs are now allowed to
  overflow into neighboring cells by default. Set
  **tweak.allow-overflowing-double-width-glyphs** to 'no' to disable
  this.

### Fixed

* Resize very slow when window is hidden
  ([#190](https://codeberg.org/dnkl/foot/issues/190)).
* Key mappings for key combinations with `shift`+`tab`
  ([#210](https://codeberg.org/dnkl/foot/issues/210)).
* Key mappings for key combinations with `alt`+`return`.
* `footclient` `-m` (`--maximized`) flag being ignored.
* Crash with explicitly sized sixels with a height less than 6 pixels.
* Key mappings for `esc` with modifiers.


### Contributors

* [craigbarnes](https://codeberg.org/craigbarnes)


## 1.5.3

### Fixed

* Crash when libxkbcommon cannot find a suitable libX11 _compose_
  file. Note that foot will run, but without support for dead keys.
  ([#170](https://codeberg.org/dnkl/foot/issues/170)).
* Restored window size when window is un-tiled.
* XCursor shape in CSD corners when window is tiled.
* Error handling when processing keyboard input (maybe
  [#171](https://codeberg.org/dnkl/foot/issues/171)).
* Compilation error _"overflow in conversion from long 'unsigned int'
  to 'int' changes value... "_ seen on platforms where the `request`
  argument in `ioctl(3)` is an `int` (for example: linux/ppc64).
* Crash when using the mouse in alternate scroll mode in an unfocused
  window ([#179](https://codeberg.org/dnkl/foot/issues/179)).
* Character dropped from selection when "right-click-hold"-extending a
  selection ([#180](https://codeberg.org/dnkl/foot/issues/180)).


## 1.5.2

### Fixed

* Regression: middle clicking double pastes in e.g. vim
  ([#168](https://codeberg.org/dnkl/foot/issues/168))


## 1.5.1

### Changed

* Default value of the **scrollback.multiplier** option in `foot.ini`
  from `1.0` to `3.0`.
* `shift`+`insert` now pastes from the primary selection by
  default. This is in addition to middle-clicking with the mouse.


### Fixed

* Mouse bindings now match even if the actual click count is larger
  than specified in the binding. This allows you to, for example,
  quickly press the middle-button to paste multiple times
  ([#146](https://codeberg.org/dnkl/foot/issues/146)).
* Color flashes when changing the color palette with OSC 4,10,11
  ([#141](https://codeberg.org/dnkl/foot/issues/141)).
* Scrollback position is now retained when resizing the window
  ([#142](https://codeberg.org/dnkl/foot/issues/142)).
* Trackpad scrolling speed to better match the mouse scrolling speed,
  and to be consistent with other (Wayland) terminal emulators. Note
  that it is (much) slower compared to previous foot versions. Use the
  **scrollback.multiplier** option in `foot.ini` if you find the new
  speed too slow ([#144](https://codeberg.org/dnkl/foot/issues/144)).
* Crash when `foot.ini` contains an invalid section name
  ([#159](https://codeberg.org/dnkl/foot/issues/159)).
* Background opacity when in _reverse video_ mode.
* Crash when writing a sixel image that extends outside the terminal's
  right margin ([#151](https://codeberg.org/dnkl/foot/issues/151)).
* Sixel image at non-zero column positions getting sheared at
  seemingly random occasions
  ([#151](https://codeberg.org/dnkl/foot/issues/151)).
* Crash after either resizing a window or changing the font size if
  there were sixels present in the scrollback while doing so.
* _Send Device Attributes_ to only send a response if `Ps == 0`.
* Paste from primary when clipboard is empty.


### Contributors

* [craigbarnes](https://codeberg.org/craigbarnes)
* [zar](https://codeberg.org/zar)


## 1.5.0

### Deprecated

* `$XDG_CONFIG_HOME/footrc`/`~/.config/footrc`. Use
  `$XDG_CONFIG_HOME/foot/foot.ini`/`~/.config/foot/foot.ini` instead.
* **scrollback** option in `foot.ini`. Use **scrollback.lines**
  instead.
* **scrollback-up** key binding. Use **scrollback-up-page** instead.
* **scrollback-down** key binding. Use **scrollback-down-page**
  instead.


### Added

* Scrollback position indicator. This feature is optional and
  controlled by the **scrollback.indicator-position** and
  **scrollback.indicator-format** options in `foot.ini`
  ([#42](https://codeberg.org/dnkl/foot/issues/42)).
* Key bindings in _scrollback search_ mode are now configurable.
* `--check-config` command line option.
* **pipe-selected** key binding. Works like **pipe-visible** and
  **pipe-scrollback**, but only pipes the currently selected text, if
  any ([#51](https://codeberg.org/dnkl/foot/issues/51)).
* **mouse.hide-when-typing** option to `foot.ini`.
* **scrollback.multiplier** option to `foot.ini`
  ([#54](https://codeberg.org/dnkl/foot/issues/54)).
* **colors.selection-foreground** and **colors.selection-background**
  options to `foot.ini`.
* **tweak.render-timer** option to `foot.ini`.
* Modifier support in mouse bindings
  ([#77](https://codeberg.org/dnkl/foot/issues/77)).
* Click count support in mouse bindings, i.e double- and triple-click
  ([#78](https://codeberg.org/dnkl/foot/issues/78)).
* All mouse actions (begin selection, select word, select row etc) are
  now configurable, via the new **select-begin**,
  **select-begin-block**, **select-extend**, **select-word**,
  **select-word-whitespace** and **select-row** options in the
  **mouse-bindings** section in `foot.ini`
  ([#79](https://codeberg.org/dnkl/foot/issues/79)).
* Implement XTSAVE/XTRESTORE escape sequences, `CSI ? Ps s` and `CSI ?
  Ps r` ([#91](https://codeberg.org/dnkl/foot/issues/91)).
* `$COLORTERM` is now set to `truecolor` at startup, to indicate
  support for 24-bit RGB colors.
* Experimental support for rendering double-width glyphs with a
  character width of 1. Must be explicitly enabled with
  `tweak.allow-overflowing-double-width-glyphs`
  ([#116](https://codeberg.org/dnkl/foot/issues/116)).
* **initial-window-size-pixels** options to `foot.ini` and
  `-w,--window-size-pixels` command line option to `foot`. This option
  replaces the now deprecated **geometry** and `-g,--geometry`
  options.
* **initial-window-size-chars** option to `foot.ini` and
  `-W,--window-size-chars` command line option to `foot`. This option
  configures the initial window size in **characters**, and is an
  alternative to **initial-window-size-pixels**.
* **scrollback-up-half-page** and **scrollback-down-half-page** key
  bindings. They scroll up/down half of a page in the scrollback
  ([#128](https://codeberg.org/dnkl/foot/issues/128)).
* **scrollback-up-line** and **scrollback-down-line** key
  bindings. They scroll up/down a single line in the scrollback.
* **mouse.alternate-scroll-mode** option to `foot.ini`. This option
  controls the initial state of the _Alternate Scroll Mode_, and
  defaults to `yes`. When enabled, mouse scroll events are translated
  to up/down key events in the alternate screen, letting you scroll in
  e.g. `less` and other applications without enabling native mouse
  support in them ([#135](https://codeberg.org/dnkl/foot/issues/135)).


### Changed

* Renamed man page for `foot.ini` from **foot**(5) to **foot.ini**(5).
* Configuration errors are no longer fatal; foot will start and print
  an error inside the terminal (and of course still log errors on
  stderr).
* Default `--server` socket path to use `$WAYLAND_DISPLAY` instead of
  `$XDG_SESSION_ID` ([#55](https://codeberg.org/dnkl/foot/issues/55)).
* Trailing empty cells are no longer highlighted in mouse selections.
* Foot now searches for its configuration in
  `$XDG_DATA_DIRS/foot/foot.ini`, if no configuration is found in
  `$XDG_CONFIG_HOME/foot/foot.ini` or in `$XDG_CONFIG_HOME/footrc`.
* Minimum window size changed from four rows and 20 columns, to 1 row
  and 2 columns.
* **scrollback-up/down** renamed to **scrollback-up/down-page**.
* fcft >= 2.3.0 is now required.


### Fixed

* Command lines for **pipe-visible** and **pipe-scrollback** are now
  tokenized (i.e. syntax checked) when the configuration is loaded,
  instead of every time the key binding is executed.
* Incorrect multi-column character spacer insertion when reflowing
  text.
* Compilation errors in 32-bit builds.
* Mouse cursor style in top and left margins.
* Selection is now **updated** when the cursor moves outside the grid
  ([#70](https://codeberg.org/dnkl/foot/issues/70)).
* Viewport sometimes not moving when doing a scrollback search.
* Crash when canceling a scrollback search and the window had been
  resized while searching.
* Selection start point not moving when the selection changes
  direction.
* OSC 10/11/104/110/111 (modify colors) did not update existing screen
  content ([#94](https://codeberg.org/dnkl/foot/issues/94)).
* Extra newlines when copying empty cells
  ([#97](https://codeberg.org/dnkl/foot/issues/97)).
* Mouse events from being sent to client application when a mouse
  binding has consumed it.
* Input events from getting mixed with paste data
  ([#101](https://codeberg.org/dnkl/foot/issues/101)).
* Missing DPI values for "some" monitors on Gnome
  ([#118](https://codeberg.org/dnkl/foot/issues/118)).
* Handling of multi-column composed characters while reflowing.
* Escape sequences sent for key combinations with `Return`, that did
  **not** include `Alt`.
* Clipboard (or primary selection) is now cleared when receiving an
  OSC-52 command with an invalid base64 encoded payload.
* Cursor position being set outside the grid when reflowing text.
* CSD buttons to be hidden when window size becomes so small that they
  no longer fit.


### Contributors

* [craigbarnes](https://codeberg.org/craigbarnes)
* [birger](https://codeberg.org/birger)
* [Ordoviz](https://codeberg.org/Ordoviz)
* [cherti](https://codeberg.org/cherti)


## 1.4.4
### Changed

* Mouse cursor is now always a `left_ptr` when inside the margins, to
  indicate it is not possible to start a selection.


### Fixed

* Crash when starting a selection inside the margins.
* Improved font size consistency across multiple monitors with
  different DPI ([#47](https://codeberg.org/dnkl/foot/issues/47)).
* Handle trailing comments in `footrc`


## 1.4.3
### Added

* Section to [README.md](README.md) describing how to programmatically
  identify foot.
* [LICENSE](LICENSE), [README.md](README.md) and
  [CHANGELOG.md](CHANGELOG.md) are now installed to
  `${datadir}/doc/foot`.
* Support for escaping quotes in **pipe-visible** and
  **pipe-scrollback** commands.


### Changed

* Primary DA to no longer indicate support for _Selective Erase_,
  _Technical Characters_ and _Terminal State Interrogation_.
* Secondary DA to report foot as a VT220 instead of a VT420.
* Secondary DA to report foot's version number in parameter 2, the
  _Firmware Version_. The string is made up of foot's major, minor and
  patch version numbers, always using two digits for each version
  number and without any other separating characters. Thus, _1.4.2_
  would be reported as `010402` (i.e. the full response would be
  `\E[>1;010402;0c`).
* Scrollback search to only move the viewport if the match lies
  outside it.
* Scrollback search to focus match, that requires a viewport change,
  roughly in the center of the screen.
* Extending a selection with the right mouse button now works while
  dragging the mouse.


### Fixed

* Crash in scrollback search.
* Crash when a **pipe-visible** or **pipe-scrollback** command
  contained an unclosed quote
  ([#49](https://codeberg.org/dnkl/foot/issues/49)).


### Contributors

* [birger](https://codeberg.org/birger)
* [cherti](https://codeberg.org/cherti)


## 1.4.2

### Changed

* Maximum window title length from 100 to 2048.


### Fixed

* Crash when overwriting a sixel and the row being overwritten did not
  cover an entire cell.
* Assertion failure in debug builds when overwriting a sixel image.


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
  external tools ([#29](https://codeberg.org/dnkl/foot/issues/29)). Example:
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
  `0x0` terminal size ([#20](https://codeberg.org/dnkl/foot/issues/20)).
* Glyphs overflowing into surrounding cells
  ([#21](https://codeberg.org/dnkl/foot/issues/21)).
* Crash when last rendered cursor cell had scrolled off screen and
  `\E[J3` was executed.
* Assert (debug builds) when an `\e]4` OSC escape was not followed by
  a `;`.
* Window title always being set to "foot" on reset.
* Terminfo entry `kb2` (center keypad key); it is now set to `\EOu`
  (which is what foot emits) instead of the incorrect value `\EOE`.
* Palette reuse in sixel images. Previously, the palette was reset
  after each image.
* Do not auto-resize a sixel image for which the client has specified
  a size. This fixes an issue where an image would incorrectly
  overflow into the cell row beneath.
* Text printed, or other sixel images drawn, on top of a sixel image
  no longer erases the entire image, only the part(s) covered by the
  new text or image.
* Sixel images being erased when printing text next to them.
* Sixel handling when resizing window.
* Sixel handling when scrollback wraps around.
* Foot now issues much fewer `wl_surface_damage_buffer()` calls
  ([#35](https://codeberg.org/dnkl/foot/issues/35)).
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
  ([#38](https://codeberg.org/dnkl/foot/issues/38)).


## 1.3.0

### Added

* User configurable key- and mouse bindings. See `man 5 foot` and the
  example `footrc` ([#1](https://codeberg.org/dnkl/foot/issues/1))
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
  ([#4](https://codeberg.org/dnkl/foot/issues/4))
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
  different scaling factors ([#3](https://codeberg.org/dnkl/foot/issues/3)).
* Font being too small on monitors with fractional scaling
  ([#5](https://codeberg.org/dnkl/foot/issues/5)).


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
