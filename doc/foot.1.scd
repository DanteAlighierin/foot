foot(1)

# NAME
foot - Wayland terminal emulator

# SYNOPSIS
*foot* [_OPTIONS_]++
*foot* [_OPTIONS_] -- <command>

All trailing (non-option) arguments are treated as a command to
execute (instead of the shell).

# OPTIONS

*-f*,*--font*=_FONT_
	Comma separated list of fonts to use, in fontconfig format (see
	*FONT FORMAT*).
	
	The first font is the primary font. The remaining fonts are
	fallback fonts that will be used whenever a glyph cannot be found
	in the primary font.
	
	The fallback fonts are searched in the order they appear. If a
	glyph cannot be found in any of the fallback fonts, the dynamic
	fallback list from fontconfig (for the primary font) is
	searched.
	
	Default: _monospace_.

*-g*,*--geometry*=_WIDTHxHEIGHT_
	Set initial window width and height.

*-t*,*--term*=_TERM_
	Value to set the environment variable _TERM_ to. Default: _foot_.

*-v*,*--version*
	Show the version number and quit

# KEYBOARD SHORTCUTS

The following keyboard shortcuts are available. Note that they cannot
be changed.

*shift*+*page up*/*page down*
	Scroll up/down in history

*ctrl*+*shift*+*c*
	Copy selected text to the _clipboard_

*ctrl*+*shift*+*v*
	Paste from _clipboard_

*ctrl*+*shift*+*r*
	Start a scrollback search

While doing a scrollback search, the following shortcuts are
available:

*ctrl*+*r*
	Search _backward_ for the next match

*ctrl*+*s*
	Search _forward_ for the next match

*escape*, *ctrl*+*g*
	Cancel the search

*return*
	Finish the search and copy the current match to the primary
	selection

# MOUSE SHORTCUTS

*left*, single-click
	Drag to select; when released, the selected text is copied to the
	_primary_ selection. Note that this feature is normally
	*disabled* whenever the client has enabled _mouse tracking_, but
	can be forced by holding *shift*.

*left*, double-click
	Selects the _word_ (separated by spaces, period, comma,
	parenthesis etc) under the pointer. Hold *ctrl* to select
	everything under the pointer up to, and until, the next space
	characters.

*left*, triple-click
	Selects the entire row

*middle*
	Paste from the _primary_ selection


# FONT FORMAT

The font is specified in FontConfig syntax. That is, a colon-separated
list of font name and font options.

_Examples_:
- Dina:weight=bold:slant=italic
- Courier New:size=12


# CONFIGURATION

See *foot*(5)

# SEE ALSO

*foot*(5)
