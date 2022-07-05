foot-ctlseqs(7)

# NAME
foot-ctlseqs - terminal control sequences supported by foot

# DESCRIPTION

This document describes all the control sequences supported by foot.

- Control characters
- Sequences beginning with ESC
- CSI - Control Sequence Introducer
    - SGR
    - Indexed and RGB colors (256-color palette and 24-bit colors)
    - Private modes
    - Window manipulation
    - Other
- OSC - Operating System Command
- DCS - Device Control String

# Control characters

[[ *Sequence*
:[ *Name*
:[ *Description*
|  \\a
:  BEL
:  Depends on what *bell* in *foot.ini*(5) is set to.
|  \\b
:  BS
:  Backspace; move the cursor left one step. Wrap if _bw_ is enabled.
|  \\t
:  HT
:  Horizontal tab; move the cursor to the next tab stop.
|  \\n
:  LF
:  Line feed; move the cursor down one step, or scroll content up if
   at the bottom line.
|  \\v
:  VT
:  Vertical tab; identical to _LF_.
|  \\f
:  FF
:  Form feed; identical to _LF_.
|  \\r
:  CR
:  Carriage ret; move the cursor to the leftmost column.
|  \\x0E
:  SO
:  Shift out; select the _G1_ character set.
|  \\x0F
:  SI
:  Shift in; select the _G0_ character set.

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
:  Single shift select of G2 character set (affects next character only).
|  \\E O
:  SS3
:  VT220
:  Single shift select of G3 character set (affects next character only).
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
:  Designate G0 character set. Supported values for _C_ are: *0* (DEC
   Special Character and Line Drawing Set), and *B* (USASCII).
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

All sequences begin with *\\E[*, sometimes abbreviated "CSI". Spaces
are used in the sequence strings to make them easier to read, but are
not actually part of the string (i.e. *\\E[ 1 m* is really *\\E[1m*).

## SGR

All SGR sequences are in the form *\\E[* _N_ *m*, where _N_ is a decimal
number - the _parameter_. Multiple parameters can be combined in a
single CSI sequence by separating them with semicolons: *\\E[ 1;2;3
m*.

[[ *Parameter*
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

- *\\E[ 38 : 5 :* _idx_ *m*
- *\\E[ 38 ; 5 ;* _idx_ *m*

RGB colors:

- *\\E[ 38 : 2 :* _cs_ *:* _r_ *:* _g_ *:* _b_ *m*
- *\\E[ 38 : 2 :* _r_ *:* _g_ *:* _b_ *m*
- *\\E[ 38 ; 2 ;* _r_ *;* _g_ *;* _b_ *m*

The first variant is the "correct" one (and foot also recognizes, but
ignores, the optional _tolerance_ parameters).

The second one is allowed since many programs "forget" the color space
ID, _cs_.

The sub-parameter based variants are preferred, and are what foot's
*terminfo*(5) entry uses.

## Private Modes

There are several Boolean-like "modes" that affect certain aspects
of the terminal's behavior. These modes can be manipulated with the
following 4 escape sequences:

[[ *Sequence*
:[ *Name*
:[ *Description*
|  \\E[ ? _Pm_ h
:  DECSET
:  Enable private mode
|  \\E[ ? _Pm_ l
:  DECRST
:  Disable private mode
|  \\E[ ? _Pm_ s
:  XTSAVE
:  Save private mode
|  \\E[ ? _Pm_ r
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
|  66
:  VT320
:  Numeric keypad mode (DECNKM); same as DECKPAM/DECKPNM when enabled/disabled
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
|  1016
:  xterm
:  SGR-Pixels mouse mode
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
|  1070
:  xterm
:  Use private color registers for each sixel
|  2004
:  xterm
:  Wrap pasted text with start/end delimiters (bracketed paste mode)
|  2026
:  terminal-wg
:  Application synchronized updates mode
|  8452
:  xterm
:  Position cursor to the right of sixels, instead of on the next line
|  737769
:  foot
:  Input Method Editor (IME) mode

## Window manipulation

Foot implements a sub-set of XTerm's (originally dtterm's) window
manipulation sequences. The generic format is:

*\\E[ *_Ps_* ; *_Ps_* ; *_Ps_* t*

[[ *Parameter 1*
:[ *Parameter 2*
:[ *Description*
|  11
:  -
:  Report if window is iconified. Foot always reports *1* - not iconified.
|  13
:  -
:  Report window position. Foot always reports (0,0), due to Wayland
   limitations.
|  13
:  2
:  Report text area position. Foot always reports (0,0) due to Wayland
   limitations.
|  14
:  -
:  Report text area size, in pixels. Foot reports the grid size,
   excluding the margins.
|  14
:  2
:  Report window size, in pixels. Foot reports the grid size plus the
   margins.
|  15
:  -
:  Report the screen size, in pixels.
|  16
:  -
:  Report the cell size, in pixels.
|  18
:  -
:  Report text area size, in characters.
|  19
:  -
:  Report screen size, in characters.
|  22
:  -
:  Push window title+icon. Foot does not support pushing the icon.
|  22
:  2
:  Push window title.
|  23
:  -
:  Pop window title+icon. Foot does not support popping the icon.
|  23
:  2
:  Pop window title.

## Other

[[ *Parameter*
:[ *Name*
:[ *Origin*
:[ *Description*
|  \\E[ _Ps_ c
:  DA
:  VT100
:  Send primary device attributes. Foot responds with "I'm a VT220 with
   sixel and ANSI color support".
|  \\E[ _Ps_ A
:  CUU
:  VT100
:  Cursor up - move cursor up _Ps_ times.
|  \\E[ _Ps_ B
:  CUD
:  VT100
:  Cursor down - move cursor down _Ps_ times.
|  \\E[ _Ps_ C
:  CUF
:  VT100
:  Cursor forward - move cursor to the right _Ps_ times.
|  \\E[ _Ps_ D
:  CUB
:  VT100
:  Cursor backward - move cursor to the left _Ps_ times.
|  \\E[ _Ps_ g
:  TBC
:  VT100
:  Tab clear. _Ps_=0 -> clear current column. _Ps_=3 -> clear all.
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
|  \\E[ _Ps_ K
:  EL
:  VT100
:  Erase in line. _Ps_=0 -> right of cursor. _Ps_=1 -> left of
   cursor. _Ps_=2 -> all.
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
|  \\E[ _Ps_ n
:  DSR
:  VT100
:  Device status report. _Ps_=5 -> device status. _Ps_=6 -> cursor
   position.
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
|  \\E[ _Ps_ X
:  ECH
:  VT220
:  Erase _Ps_ characters.
|  \\E[ > c
:  DA2
:  VT220
:  Send secondary device attributes. Foot responds with "I'm a VT220
   and here's my version number".
|  \\E[ ! p
:  DECSTR
:  VT220
:  Soft terminal reset.
|  \\E[ ? _Ps_ $ p
:  DECRQM
:  VT320
:  Request DEC private mode.
|  \\E[ _Ps_ T
:  SD
:  VT420
:  Scroll down _Ps_ lines.
|  \\E[ s
:  SCOSC
:  SCO, VT510
:  Save cursor position.
|  \\E[ u
:  SCORC
:  SCO, VT510
:  Restore cursor position.
|  \\E[ _Ps_ SP q
:  DECSCUSR
:  VT510
:  Set cursor style. In foot, _Ps_=0 means "use style from foot.ini".
|  \\E[ = _Ps_ c
:  DA3
:  VT510
:  send tertiary device attributes. Foot responds with "FOOT", in
   hexadecimal.
|  \\E[ _Pm_ d
:  VPA
:  ECMA-48
:  Line position absolute - move cursor to line _Pm_.
|  \\E[ _Pm_ e
:  VPR
:  ECMA-48
:  Line position relative - move cursor down _Pm_ lines.
|  \\E[ _Pm_ a
:  HPR
:  ECMA-48
:  Character position relative - move cursor to the right _Pm_ times.
|  \\E[ _Ps_ E
:  CNL
:  ECMA-48
:  Cursor next line - move the cursor down _Ps_ times.
|  \\E[ _Ps_ F
:  CPL
:  ECMA-48
:  Cursor preceding line  - move the cursor up _Ps_ times.
|  \\E[ _Pm_ `
:  HPA
:  ECMA-48
:  Character position absolute - move cursor to column _Pm_.
|  \\E[ _Ps_ G
:  CHA
:  ECMA-48
:  Cursor character absolute - move cursor to column _Ps_.
   cursor. _Ps_=2 -> all. _Ps_=3 -> saved lines.
|  \\E[ _Ps_ S
:  SU
:  ECMA-48
:  Scroll up _Ps_ lines.
|  \\E[ _Ps_ I
:  CHT
:  ECMA-48
:  Cursor forward tabulation _Ps_ tab stops.
|  \\E[ _Ps_ Z
:  CBT
:  ECMA-48
:  Cursor backward tabulation _Ps_ tab stops.
|  \\E[ _Ps_ b
:  REP
:  ECMA-48
:  Repeat the preceding printable character _Ps_ times.
|  \\E[ ? _Pi_ ; _Pa_ ; _Pv_ S
:  XTSMGRAPHICS
:  xterm
:  Set or request sixel attributes.
|  \\E[ > _Ps_ q
:  XTVERSION
:  xterm
:  _Ps_=0 -> report terminal name and version, in the form
   *\\EP>|foot(version)\\E\\*.
|  \\E[ > 4 ; _Pv_ m
:  XTMODKEYS
:  xterm
:  Set level of the _modifyOtherKeys_ property to _Pv_. Note that foot
   only supports level 1 and 2, where level 1 is the default setting.
|  \\E[ > 4 n
:  <unnamed>
:  xterm
:  Resets the _modifyOtherKeys_ property to level 1. Note that in foot,
   this sequence does not completely disable _modifyOtherKeys_, since
   foot only supports level 1 and level 2 (and not level 0).
|  \\E[ ? u
:  <unnamed>
:  kitty
:  Query current values of the Kitty keyboard flags.
|  \\E[ > _flags_ u
:  <unnamed>
:  kitty
:  Push a new entry, _flags_, to the Kitty keyboard stack.
|  \\E[ < _number_ u
:  <unnamed>
:  kitty
:  Pop _number_ of entries from the Kitty keyboard stack.
|  \\E[ = _flags_ ; _mode_ u
:  <unnamed>
:  kitty
:  Update current Kitty keyboard flags, according to _mode_.


# OSC

All _OSC_ sequences begin with *\\E]*, sometimes abbreviated _OSC_.

[[ *Sequence*
:[ *Origin*
:[ *Description*
|  \\E] 0 ; _Pt_ \\E\\ 
:  xterm
:  Set window icon and title to _Pt_ (foot does not support setting the
   icon)
|  \\E] 2 ; _Pt_ \\E\\ 
:  xterm
:  Set window title to _Pt_
|  \\E] 4 ; _c_ ; _spec_ \\E\\ 
:  xterm
:  Change color number _c_ to _spec_, where _spec_ is a color in
   XParseColor format. foot only supports RGB colors; either
   *rgb:<red>/<green>/<blue>*, or the legacy format (*#rgb*).
|  \\E] 7 ; _Uri_ \\E\\ 
:  iTerm2
:  Update the terminal's current working directory. Newly spawned
   terminals will launch in this directory. _Uri_ must be in the format
   *file://<hostname>/<path>*. *hostname* must refer to your local host.
|  \\E] 8 ; id=_ID_ ; _Uri_ \\E\\ 
:  VTE+iTerm2
:  Hyperlink (a.k.a HTML-like anchors). id=_ID_ is optional; if assigned,
   all URIs with the same _ID_ will be treated as a single
   hyperlink. An empty URI closes the hyperlink.
|  \\E] 9 ; _msg_ \\E\\ 
:  iTerm2
:  Desktop notification, uses *notify* in *foot.ini*(5).
|  \\E] 10 ; _spec_ \\E\\ 
:  xterm
:  Change the default foreground color to _spec_, a color in
   XParseColor format.
|  \\E] 11 ; _spec_ \\E\\ 
:  xterm
:  Change the default background color to _spec_, a color in
   XParseColor format. Foot implements URxvt's transparency extension;
   e.g. _spec_=*[75]#ff00ff* or _spec_=*rgba:ff/00/ff/bf* (pink with
   75% alpha).
|  \\E] 12 ; _spec_ \\E\\ 
:  xterm
:  Change cursor color to _spec_, a color in XParseColor format.
|  \\E] 17 ; _spec_ \\E\\ 
:  xterm
:  Change selection background color to _spec_, a color in
   XParseColor format.
|  \\E] 19 ; _spec_ \\E\\ 
:  xterm
:  Change selection foreground color to _spec_, a color in XParseColor
   format.
|  \\E] 22 ; _xcursor-pointer-name_ \\E\\ 
:  xterm
:  Sets the xcursor pointer. An empty name, or an invalid name resets
   it.
|  \\E] 52 ; _Pc_ ; ? \\E\\ 
:  xterm
:  Send clipboard data. _Pc_ can be either *c*, *s* or *p*. *c* uses
   the clipboard as source, and *s* and *p* uses the primary
   selection. The response is *\\E] 52 ; Pc ; <base64-encoded data>
   \E\\*, where _Pc_ denotes the source used.
|  \\E] 52 ; _Pc_ ; _Pd_ \\E\\ 
:  xterm
:  Copy _Pd_ (base64 encoded text) to the clipboard. _Pc_ denotes the
   target: *c* targets the clipboard and *s* and *p* the primary
   selection.
|  \\E] 104 ; _c_ \\E\\ 
:  xterm
:  Reset color number _c_ (multiple semicolon separated _c_ values may
   be provided), or all colors (excluding the default
   foreground/background colors) if _c_ is omitted.
|  \\E] 110 \\E\\ 
:  xterm
:  Reset default foreground color
|  \\E] 111 \\E\\ 
:  xterm
:  Reset default background color
|  \\E] 112 \\E\\ 
:  xterm
:  Reset cursor color
|  \\E] 117 \\E\\ 
:  xterm
:  Reset selection background color
|  \\E] 119 \\E\\ 
:  xterm
:  Reset selection foreground color
|  \\E] 133 ; A \\E\\ 
:  FinalTerm
:  Mark start of shell prompt
|  \\E] 555 \\E\\ 
:  foot
:  Flash the entire terminal (foot extension)
|  \\E] 777;notify;_title_;_msg_ \\E\\ 
:  urxvt
:  Desktop notification, uses *notify* in *foot.ini*(5).

# DCS

All _DCS_ sequences begin with *\\EP* (sometimes abbreviated _DCS_),
and are terminated by *\\E\\* (ST).

[[ *Sequence*
:[ *Description*
|  \\EP q <sixel data> \\E\\ 
:  Emit a sixel image at the current cursor position
|  \\P $ q <query> \\E\\ 
:  Request selection or setting (DECRQSS). Implemented queries:
   DECSTBM, SGR and DECSCUSR.
|  \\EP = _C_ s \\E\\ 
:  Begin (_C_=*1*) or end (_C_=*2*) application synchronized updates.
   This sequence is supported for compatibility reasons, but it's
   recommended to use private mode 2026 (see above) instead.
|  \\EP + q <hex encoded capability name> \\E\\
:  Query builtin terminfo database (XTGETTCAP)


# FOOTNOTE

Foot does not support 8-bit control characters ("C1").
