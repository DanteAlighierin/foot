foot(5)

# NAME
foot - configuration file

# DESCRIPTION

*foot* uses the standard _unix configuration format_, with section based
key/value pairs. The default section is unnamed (i.e. not prefixed
with a _[section]_).

foot will search for a configuration file in the following locations,
in this order:

- _XDG_CONFIG_HOME/footrc_
- _~/.config/footrc_

# SECTION: default

*font*
	Font and style to use, in fontconfig format. See *FONT
	FORMAT*. Default: _monospace_.

*shell*
	Executable to launch. Typically a shell. Default: the user's
	default shell (as specified in _/etc/passwd_).

*term*
	Value to set the environment variable *TERM* to. Default: _foot_.

*scrollback*
	Number of scrollback lines. Default: _1000_.

*workers*
	Number of threads to use for rendering. Default: the number of
	available logical CPUs (including SMT). Note that this is not
	always the best value. In some cases, the number of physical
	_cores_ is better.


# SECTION: cursor

*style*
	Configures the default cursor style, and is One of: _block_, _bar_
	or _underline_. Default: _block_.

*color*
	Two RRGGBB values specifying the foreground (text) and background
	(cursor) colors for the cursor. Default: inversed foreground and
	background colors. Note that this value only applies to the block
	cursor. The other cursor styles are always rendered with the
	foreground color.

# SECTION: colors

*foreground*
	Default RRGGBB foreground color. This is the color used when no
	ANSI color is being used. Default: _dcdccc_.

*background*
	Default RRGGBB background color. This is the color used when no
	ANSI color is being used. Default: _111111_.

*regular0*, *regular1* *..* *regular7*
	The eight basic ANSI colors. Default: _000000_, _cc9393_,
	_7f9f7f_, _d0bf8f_, _6ca0a3_, _dc8cc3_, _93e0e3_ and _dcdccc_ (a
	variant of the _zenburn_ theme).

*bright0*, *bright1* *..* *bright7*
	The eight bright ANSI colors. Default: _000000_, _dca3a3_,
	_bfebbf_, _f0dfaf_, _8cd0d3_, _dc8cc3_, _93e0e3_ and _ffffff_ (a
	variant of the _zenburn_ theme).

# FONT FORMAT

The font is specified in FontConfig syntax. That is, a colon-separated
list of font name and font options.

_Examples_:
- Dina:weight=bold:slant=italic
- Arial:size=12
