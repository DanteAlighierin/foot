foot-ctlseqs(5)

# DESCRIPTION

This document describes all the control sequences supported by foot.

- CC0 - 7-bit control sequences
- CC1 - 8-bit control sequences (*not supported*)
- Sequences beginning with ESC
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

# Sequences beginning with ESC

Note: this table excludes sequences where ESC is part of a 7-bit
equivalent to 8-bit C1 controls.

[[ *Sequence*
:[ *Name*
:[ *Origin*
:[ *Description*
|  \\E 7
:  DECSC
:  VT100
:  Save cursor position.
|  \\E 8
:  DECRC
:  VT100
:  Restore cursor position.
|  \\E c
:  RIS
:  VT100
:  Reset terminal to initial state.
|  \\E D
:  IND
:  VT100
:  Line feed; move the cursor down one step, or scroll content up if
   at the bottom margin.
|  \\E E
:  NEL
:  VT100
:  Next line; move the cursor down one step, and to the first
   column. Content is scrolled up if at the bottom line.
|  \\E H
:  HTS
:  VT100
:  Set one horizontal tab stop at the current position.
|  \\E M
:  RI
:  VT100
:  Reverse index; move the cursor up one step, or scroll content down
   if at the top margin.
|  \\E N
:  SS2
:  VT220
:  Single shift G2; select the _G2_ character set.
|  \\E O
:  SS3
:  VT220
:  Single shift G3; select the _G3_ character set.
|  \\E =
:  DECKPAM
:  VT100
:  Switch keypad to _application_ mode.
|  \\E >
:  DECKPNM
:  VT100
:  Switch keypad to _numeric_ mode.
|  \\E ( _C_
:  SCS
:  VT100
:  Designate G0 character set. Supported values for _C_ are: *0* (_DEC
   Special Character and Line Drawing Set_), and *B* (_USASCII_).
|  \\E ) _C_
:  SCS
:  VT100
:  Designate G1 character set. Same supported values for _C_ as in _G0_.
|  \\E \* _C_
:  SCS
:  VT220
:  Designate G2 character set. Same supported values for _C_ as in _G0_.
|  \\E + _C_
:  SCS
:  VT220
:  Designate G3 character set. Same supported values for _C_ as in _G0_.

# CSI

All sequences begin with *\\E[*, sometimes abbreviated _CSI_. Spaces
are used in the sequence strings to make them easier to read, but are
not actually part of the string. I.e. *\\E[ 1 m* is really *\\E1m*.

## SGR

All SGR sequences are on the form *\\E[ <N> m*, where _N_ is a decimal
number - the _parameter_. Multiple parameters can be combined in a
single CSI sequence by separating them with semicolons: *\\E[ 1;2;3
m*.

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

- *\\E[ 38:5:<idx> m*
- *\\E[ 38;5;<idx> m*

RGB colors:

- *\\E[ 38:2:<cs>:<r>:<g>:<b> m*
- *\\E[ 38:2:<r>:<g>:<b> m*
- *\\E[ 38;2;<r>;<g>;<b> m*

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
|  \\E[ ? _Pm_ *h*
:  DECSET
:  Enable private mode
|  \\E[ ? _Pm_ *l*
:  DECRST
:  Disable private mode
|  \\E[ ? _Pm_ *s*
:  XTSAVE
:  Save private mode
|  \\E[ ? _Pm_ *r*
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

[[ *Parameter*
:[ *Name*
:[ *Origin*
:[ *Description*
|  \\E[ _Ps_ b
:  REP
:  ECMA-48
:  Repeat the preceding printable character _Ps_ times.
|  \\E[ _Ps_ c
:  DA
:  VT100
:  Send primary device attributes. Foot responds with "I'm a VT220 with
   sixel and ANSI color support".
|  \\E[ _Pm_ d
:  VPA
:  ECMA-48
:  Line position absolute - move cursor to line _Pm_.
|  \\E[ _Ps_ A
:  CUU
:  VT100
:  Cursor up - move cursor up _Ps_ times.
|  \\E[ _Pm_ e
:  VPR
:  ECMA-48
:  Line position relative - move cursor down _Pm_ lines.
|  \\E[ _Ps_ B
:  CUD
:  VT100
:  Cursor down - move cursor down _Ps_ times.
|  \\E[ _Pm_ a
:  HPR
:  ECMA-48
:  Character position relative - move cursor to the right _Pm_ times.
|  \\E[ _Ps_ C
:  CUF
:  VT100
:  Cursor forward - move cursor to the right _Ps_ times.
|  \\E[ _Ps_ D
:  CUB
:  VT100
:  Cursor backward - move cursor to the left _Ps_ times.
|  \\E[ _Ps_ E
:  CNL
:  ECMA-48
:  Cursor next line - move the cursor down _Ps_ times.
|  \\E[ _Ps_ F
:  CPL
:  ECMA-48
:  Cursor preceding line  - move the cursor up _Ps_ times.
|  \\E[ _Ps_ g
:  TBC
:  VT100
:  Tab clear. _Ps_=0 -> clear current column. _Ps_=3 -> clear all.
|  \\E[ _Pm_ `
:  HPA
:  ECMA-48
:  Character position absolute - move cursor to column _Pm_.
|  \\E[ _Ps_ G
:  CHA
:  ECMA-48
:  Cursor character absolute - move cursor to column _Ps_.
|  \\E[ _Ps_ ; _Ps_ f
:  HVP
:  VT100
:  Horizontal and vertical position - move cursor to _row_ ; _column_.
|  \\E[ _Ps_ ; _Ps_ H
:  CUP
:  VT100
:  Cursor position - move cursor to _row_ ; _column_.
|  \\E[ _Ps_ J
:  ED
:  VT100
:  Erase in display. _Ps_=0 -> below cursor. _Ps_=1 -> above
   cursor. _Ps_=2 -> all. _Ps_=3 -> saved lines.
|  \\E[ _Ps_ K
:  EL
:  VT100
:  Erase in line. _Ps_=0 -> right of cursor. _Ps_=1 -> left of
   cursor. _Ps_=2 -> all.
|  \\E[ _Ps_ L
:  IL
:  VT220
:  Insert _Ps_ lines.
|  \\E[ _Ps_ M
:  DL
:  VT220
:  Delete _Ps_ lines.
|  \\E[ _Ps_ P
:  DCH
:  VT220
:  Delete _Ps_ characters.
|  \\E[ _Ps_ @
:  ICH
:  VT220
:  Insert _Ps_ blank characters.
|  \\E[ _Ps_ S
:  SU
:  ECMA-48
:  Scroll up _Ps_ lines.
|  \\E[ _Ps_ T
:  SD
:  VT420
:  Scroll down _Ps_ lines.
|  \\E[ _Ps_ X
:  ECH
:  VT220
:  Erase _Ps_ characters.
|  \\E[ _Ps_ I
:  CHT
:  ECMA-48
:  Cursor forward tabulation _Ps_ tab stops.
|  \\E[ _Ps_ Z
:  CBT
:  ECMA-48
:  Cursor backward tabulation _Ps_ tab stops.
|  \\E[ _Pm_ h
:  SM
:  VT100
:  Set mode. _Pm_=4 -> enable IRM (insert mode). All other values of
   _Pm_ are unsupported.
|  \\E[ _Pm_ l
:  RM
:  VT100
:  Reset mode. _Pm_=4 -> disable IRM (insert mode). All other values of
   _Pm_ are unsupported.
|  \\E[ s
:  SCOSC
:  SCO, VT510
:  Save cursor position.
|  \\E[ u
:  SCORC
:  SCO, VT510
:  Restore cursor position.
|  \\E[ _Ps_ n
:  DSR
:  VT100
:  Device status report. _Ps_=5 -> device status. _Ps_=6 -> cursor
   position.
|  \\E[ > c
:  DA2
:  VT220
:  Send secondary device attributes. Foot responds with "I'm a VT220
   and here's my version number".
|  \\E[ _Ps_ SP q
:  DECSCUSR
:  VT510
:  Set cursor style. In foot, _Ps_=0 means "use style from foot.ini".
|  \\E[ ! p
:  DECSTR
:  VT220
:  Soft terminal reset.
|  \\E[ = _Ps_ c
:  DA3
:  VT510
:  send tertiary device attributes. Foot responds with "FOOT", in
   hexadecimal.
|  \\E[ ? _Ps_ $ p
:  DECRQM
:  VT320
:  Request DEC private mode.

TODO

\\E[ ? S   sixels (separate table?) (xterm?)
\\E[ t     window operations - use a custom table for this

# OSC

All _OSC_ sequences begin with *\\E]*, sometimes abbreviated _OSC_.

[[ *Sequence*
:[ *Origin*
:[ *Description*
|  \\E] 0 ; _Pt_ ST
:  xterm
:  Set window icon and title to _Pt_ (foot does not support setting the
   icon)
|  \\E] 2 ; _Pt_ ST
:  xterm
:  Set window title to _Pt_
|  \\E] 4 ; _c_ ; _spec_ ST
:  xterm
:  Change color number _c_ to _spec_, where _spec_ is a color in
   *XParseColor* format. foot only supports RGB colors; either
   *rgb:<red>/<green>/<blue>*, or the legacy format (*#rgb*).
|  \\E] 7 ; _Uri_ ST
:  iTerm2
:  Update the terminal's current working directory. Newly spawned
   terminals will launch in this directory. _Uri_ must be on the format
   *file://<hostname>/<path>*. *hostname* must refer to your local host.
|  \\E] 10 ; _spec_ ST
:  xterm
:  Change the default foreground color to _spec_, a color in
   *XParseColor* format.
|  \\E] 11 ; _spec_ ST
:  xterm
:  Change the default background color to _spec_, a color in
   *XParseColor* format.
|  \\E] 12 ; _spec_ ST
:  xterm
:  Change cursor color to _spec_, a color in *XParseColor* format.
|  \\E] 52 ; _Pc_ ; ? ST
:  xterm
:  Send clipboard data. _Pc_ can be either *c*, *s* or *p*. *c* uses
   the clipboard as source, and *s* and *p* uses the primary
   selection. The response is *OSC 52 ; Pc ; <base64-encoded data>*,
   where _Pc_ indicates the source used.
|  \\E] 52 ; _Pc_ ; _Pd_ ST
:  xterm
:  Copy _Pd_ (base64 encoded text) to the clipboard. _Pc_ indicates the
   target: *c* targets the clipboard and *s* and *p* the primary
   selection.
|  \\E] 104 [; _c_] ST
:  xterm
:  Reset color number _c_, or all colors (excluding the default
   foreground/background colors) if _c_ is omitted.
|  \\E] 110 ST
:  xterm
:  Reset default foreground color
|  \\E] 111 ST
:  xterm
:  Reset default background color
|  \\E] 112 ST
:  xterm
:  Reset cursor color
|  \\E] 555 ST
:  foot
:  Flash the entire terminal (foot extension)
|  \\E] 777;notify;_title_;_msg_ ST
:  urxvt
:  Desktop notification, uses *notify* in *foot.ini*(5).

# DCS

All _DCS_ sequences begin with *\\EP* (from here denoted _DCS_), and
are terminated by *\\E\\* (ST).

[[ *Sequence*
:[ *Description*
|  \\EP q <sixel data> ST
:  Emit a sixel image at the current cursor position
|  \\EP = 1 s ST
:  Begin application synchronized updates
|  \\EP = 2 s ST
:  End application synchronized updates

