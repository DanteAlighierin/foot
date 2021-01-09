foot-ctlseqs(5)

# DESCRIPTION

This document describes all the control sequences supported by foot.

- CC0 - 7-bit control sequences
- CSI
- OSC
- DCS

# CC0 - 7-bit control sequences

[[ *Sequence*
:[ *Name*
:[ *Description*
|  *\\a*
:  BEL
:  Depends on what *bell* in *foot.ini*(5)
|  *\\b*
:  BS
:  Backspace; moves the cursor left one step. Wraps is _bw_ is enabled.
|  *\\t*
:  HT
:  Horizontal tab; moves the cursor to the next tab stop.
|  *\\n*
:  LF
:  Line feed; moves the cursor down one step, or scrolls content up if
   at the bottom line.
|  *\\v*
:  VT
:  Vertical tab; identical to _LF_.
|  *\\f*
:  FF
:  Form feed; identical to _LF_.
|  *\\r*
:  CR
:  Carriage ret; moves the cursor to the left most column.
|  *\\x0E*
:  SS0
:  Shift out; selects the _G1_ character set.
|  *\\x0F*
:  SS1
:  Shift in; selects the _G0_ character set.

