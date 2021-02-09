foot-ctlseqs(5)

# DESCRIPTION

This document describes all the control sequences supported by foot.

- CC0 - 7-bit control sequences
- CC1 - 8-bit control sequences (*not supported*)
- CSI - Control Sequence Introducer
- OSC - Operating System Command
- DCS - Device Control String

# CC0 - 7-bit control sequences

[[ *Sequence*
:[ *Name*
:[ *Description*
|  *\\a*
:  BEL
:  Depends on what *bell* in *foot.ini*(5) is set to.
|  *\\b*
:  BS
:  Backspace; move the cursor left one step. Wrap if _bw_ is enabled.
|  *\\t*
:  HT
:  Horizontal tab; move the cursor to the next tab stop.
|  *\\n*
:  LF
:  Line feed; move the cursor down one step, or scroll content up if
   at the bottom line.
|  *\\v*
:  VT
:  Vertical tab; identical to _LF_.
|  *\\f*
:  FF
:  Form feed; identical to _LF_.
|  *\\r*
:  CR
:  Carriage ret; move the cursor to the leftmost column.
|  *\\x0E*
:  SS0
:  Shift out; select the _G1_ character set.
|  *\\x0F*
:  SS1
:  Shift in; select the _G0_ character set.

# CC1 - 8-bit control sequences

Foot does not support any CC1 control sequences.

# CSI

All sequences begin with *\\E[*, from here on denoted _CSI_. Spaces
are used in the sequence strings to make them easier to read, but are
not actually part of the string. I.e. *CSI 1 m* is really *\\E1m*.

## SGR

All SGR sequences are on the form *CSI <N> m*, where _N_ is a decimal
number - the _parameter_. Multiple parameters can be combined in a
single CSI sequence by separating them with semicolons: *CSI 1;2;3 m*

[[ *Sequence*
:[ *Description*
|  0
:  Reset all attributes
|  1
:  Bold
|  2
:  Dim
|  3
:  Italic
|  4
:  Underline
|  5
:  Blink
|  7
:  Reverse video; swap foreground and background colors
|  8
:  Conceal; text is not visible, but is copiable
|  9
:  Crossed-out/strike
|  22
:  Disable *bold* and *dim*
|  23
:  Disable italic
|  24
:  Disable underline
|  25
:  Disable blink
|  27
:  Disable reverse video
|  28
:  Disable conceal
|  29
:  Disable crossed-out
|  30-37
:  Select foreground color (using *regularN* in *foot.ini*(5))
|  38
:  See "indexed and RGB colors" below
|  39
:  Use the default foreground color (*foreground* in *foot.ini*(5))
|  40-47
:  Select background color (using *regularN* in *foot.ini*(5))
|  48
:  See "indexed and RGB colors" below
|  49
:  Use the default background color (*background* in *foot.ini*(5))
|  90-97
:  Select foreground color (using *brightN* in *foot.ini*(5))
|  100-107
:  Select background color (using *brightN* in *foot.ini*(5))

## Indexed and RGB colors (256-color palette and 24-bit colors)

Foot supports both the new sub-parameter based variants, and the older
parameter based variants for setting foreground and background colors.

Indexed colors:

- *CSI 38:5:<idx> m*
- *CSI 38;5;<idx> m*

RGB colors:

- *CSI 38:2:<cs>:<r>:<g>:<b> m*
- *CSI 38:2:<r>:<g>:<b> m*
- *CSI 38;2;<r>;<g>;<b> m*

The first variant is the "correct" one (and foot also recognizes, but
ignores, the optional _tolerance_ parameters).

The second one is allowed since many programs "forget" the color space
ID, _cs_.

The sub-parameter based variants are preferred, and what foot's
terminfo uses.

## Private Modes

There are several Boolean-like "modes" that affect certain aspects
of the terminal's behavior. These modes can be manipulated with the
following 4 escape sequences:

[[ *Sequence*
:[ *Name*
:[ *Description*
|  CSI ? _Pm_ *h*
:  DECSET
:  Enable private mode
|  CSI ? _Pm_ *l*
:  DECRST
:  Disable private mode
|  CSI ? _Pm_ *s*
:  XTSAVE
:  Save private mode
|  CSI ? _Pm_ *r*
:  XTRESTORE
:  Restore private mode


The _Pm_ parameter in the above sequences denotes a numerical ID
that corresponds to one of the following modes:

[[ *Parameter*
:[ *Origin*
:[ *Description*
|  1
:  VT100
:  Cursor keys mode (DECCKM)
|  5
:  VT100
:  Reverse video (DECSCNM)
|  6
:  VT100
:  Origin mode (DECOM)
|  7
:  VT100
:  Auto-wrap mode (DECAWM)
|  12
:  AT&T 610
:  Cursor blink
|  25
:  VT220
:  Cursor visibility (DECTCEM)
|  45
:  xterm
:  Reverse-wraparound mode
|  47
:  xterm
:  Same as 1047 (see below)
|  1000
:  xterm
:  Send mouse x/y on button press/release
|  1001
:  xterm
:  Use hilite mouse tracking
|  1002
:  xterm
:  Use cell motion mouse tracking
|  1003
:  xterm
:  Use all motion mouse tracking
|  1004
:  xterm
:  Send FocusIn/FocusOut events
|  1006
:  xterm
:  SGR mouse mode
|  1007
:  xterm
:  Alternate scroll mode
|  1015
:  urxvt
:  urxvt mouse mode
|  1034
:  xterm
:  8-bit Meta mode
|  1035
:  xterm
:  Num Lock modifier (see xterm numLock option)
|  1036
:  xterm
:  Send ESC when Meta modifies a key (see xterm metaSendsEscape option)
|  1042
:  xterm
:  Perform action for BEL character (see *bell* in *foot.ini*(5))
|  1047
:  xterm
:  Use alternate screen buffer
|  1048
:  xterm
:  Save/restore cursor (DECSET=save, DECRST=restore)
|  1049
:  xterm
:  Equivalent to 1048 and 1047 combined
|  2004
:  xterm
:  Wrap pasted text with start/end delimiters (bracketed paste mode)
|  737769
:  foot
:  Input Method Editor (IME) mode

## Other

*TODO*: all other CSI sequences that does not fall into any of the
above categories.

# OSC

All _OSC_ sequences begin with *\\E]*, from here on denoted _OSC_.

[[ *Sequence*
:[ *Description*
|  OSC 0 ; _Pt_ ST
:  Set window icon and title to _Pt_ (foot does not support setting the
   icon)
|  OSC 2 ; _Pt_ ST
:  Set window title to _Pt_
|  OSC 4 ; _c_ ; _spec_ ST
:  Change color number _c_ to _spec_, where _spec_ is a color in
   *XParseColor* format. foot only supports RGB colors; either
   *rgb:<red>/<green>/<blue>*, or the legacy format (*#rgb*).
|  OSC 7 ; _Uri_ ST
:  Update the terminal's current working directory. Newly spawned
   terminals will launch in this directory. _Uri_ must be on the format
   *file://<hostname>/<path>*. *hostname* must refer to your local host.
|  OSC 10 ; _spec_ ST
:  Change the default foreground color to _spec_, a color in
   *XParseColor* format.
|  OSC 11 ; _spec_ ST
:  Change the default background color to _spec_, a color in
   *XParseColor* format.
|  OSC 12 ; _spec_ ST
:  Change cursor color to _spec_, a color in *XParseColor* format.
|  OSC 52 ; _Pc_ ; ? ST
:  Send clipboard data. _Pc_ can be either *c*, *s* or *p*. *c* uses
   the clipboard as source, and *s* and *p* uses the primary
   selection. The response is *OSC 52 ; Pc ; <base64-encoded data>*,
   where _Pc_ indicates the source used.
|  OSC 52 ; _Pc_ ; _Pd_ ST
:  Copy _Pd_ (base64 encoded text) to the clipboard. _Pc_ indicates the
   target: *c* targets the clipboard and *s* and *p* the primary
   selection.
|  OSC 104 [; _c_] ST
:  Reset color number _c_, or all colors (excluding the default
   foreground/background colors) if _c_ is omitted.
|  OSC 110 ST
:  Reset default foreground color
|  OSC 111 ST
:  Reset default background color
|  OSC 112 ST
:  Reset cursor color
|  OSC 555 ST
:  Flash the entire terminal (foot extension)
|  OSC 777 ; notify ; _summary_ ; _message_ ST
:  Desktop notification, uses *notify* in *foot.ini*(5).

# DCS

All _DCS_ sequences begin with *\\EP* (from here denoted _DCS_), and
are terminated by *\\E\\* (ST).

[[ *Sequence*
:[ *Description*
|  DCS q <sixel data> ST
:  Emit a sixel image at the current cursor position
|  DCS = 1 s ST
:  Begin application synchronized updates
|  DCS = 2 s ST
:  End application synchronized updates

