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

All sequences begin with *\\E[*, denoted _CSI_ in the tables
below. Spaces are used in the sequence strings to make them easier to
read, but are not actually part of the string. I.e. *CSI 1 m* is
really *\\E1m*.

## SGR

All SGR sequences are on the form *CSI <N> m*, where _N_ is a decimal
number - the _parameter_. Multiple parameters can be combined in a
single CSI sequence by separating them with semicolons: *CSI 1;2;3 m*

[[ *Sequence*
:[ *Description*
|  0
:  Reset all attributes.
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
:  Conceal; text is not visible, but is copyable
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
:  See indexed and RGB colors below
|  39
:  Use the default foreground color (*foreground* in *foot.ini*(5))
|  40-47
:  Select background color (using *regularN* in *foot.ini*(5))
|  48
:  See indexed and RGB colors below
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


## DECSET/DECRST

*TODO*

## Other

*TODO*: all other CSI sequences that does not fall into any of the
above categories.

# OSC

*TODO*
All _OSC_ sequences begin with *\\E]*.

# DCS

*TODO*
All _DCS_ sequences begin with *\\EP*, and are terminated by *\\E\\*
(ST).
