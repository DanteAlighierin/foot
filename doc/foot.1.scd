foot(1)

# NAME
foot - Wayland terminal emulator

# SYNOPSIS
*foot* [_OPTIONS_]...

# OPTIONS

*-f*,*--font*=_FONT_
	Font and style to use, in fontconfig format. See *FONT
	FORMAT*. Default: _monospace_.

*-t*,*--term*=_TERM_
	Value to set the environment variable _TERM_ to. Default: _foot_.

*-v*,*--version*
	Show the version number and quit


# FONT FORMAT

The font is specified in FontConfig syntax. That is, a colon-separated
list of font name and font options.

_Examples_:
- Dina:weight=bold:slant=italic
- Arial:size=12


# CONFIGURATION

See *foot*(5)

# SEE ALSO

*foot*(5)
