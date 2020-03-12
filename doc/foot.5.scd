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

*geometry*
	Initial window width and height in pixels, on the form
	_WIDTHxHEIGHT_.

*pad*
	Padding between border and glyphs, in pixels, on the form
	_XxY_ (-padding).

*shell*
	Executable to launch. Typically a shell. Default: _$SHELL_ if set,
	otherwise the user's default shell (as specified in
	_/etc/passwd_). You can also pass arguments. For example
	"/bin/bash --norc".

*login-shell*
	Start a login shell, by prepending a '-' to argv[0]. Default: _no_.

*term*
	Value to set the environment variable *TERM* to. Default: _foot_.

*scrollback*
	Number of scrollback lines. Default: _1000_.

*workers*
	Number of threads to use for rendering. Set to 0 to disable
	multithreading. Default: the number of available logical CPUs
	(including SMT). Note that this is not always the best value. In
	some cases, the number of physical _cores_ is better.


# SECTION: cursor

This section controls the cursor style and color. Note that
applications can change these runtime.

*style*
	Configures the default cursor style, and is one of: _block_, _bar_
	or _underline_. Default: _block_.

*color*
	Two RRGGBB values specifying the foreground (text) and background
	(cursor) colors for the cursor. Default: inversed foreground and
	background colors. Note that this value only applies to the block
	cursor. The other cursor styles are always rendered with the
	foreground color.

# SECTION: colors

This section controls the 16 ANSI colors and the default foreground
and background colors. Note that applications can change these runtime.

The colors are in RRGGBB format. That is, they do *not* have an alpha
component. You can configure the background transparency with the
_alpha_ option.

*foreground*
	Default RRGGBB foreground color. This is the color used when no
	ANSI color is being used. Default: _dcdccc_.

*background*
	Default RRGGBB background color. This is the color used when no
	ANSI color is being used. Default: _111111_.

*regular0*, *regular1* *..* *regular7*
	The eight basic ANSI colors. Default: _222222_, _cc9393_,
	_7f9f7f_, _d0bf8f_, _6ca0a3_, _dc8cc3_, _93e0e3_ and _dcdccc_ (a
	variant of the _zenburn_ theme).

*bright0*, *bright1* *..* *bright7*
	The eight bright ANSI colors. Default: _666666_, _dca3a3_,
	_bfebbf_, _f0dfaf_, _8cd0d3_, _fcace3_, _b3ffff_ and _ffffff_ (a
	variant of the _zenburn_ theme).

*alpha*
	Background translucency. A value in the range 0.0-1.0, where 0.0
	means completely transparent, and 1.0 is opaque. Default: _1.0_.

# SECTION: csd

This section controls the look of the _CSDs_ (Client Side
Decorations). Note that the default is to *not* use CSDs, but instead
to use _SSDs_ (Server Side Decorations) when the compositor supports
it.

Note that unlike the colors defined in the _colors_ section, the color
values here are in AARRGGBB format. I.e. they contain an alpha
component.

*preferred*
	Which type of window decorations to prefer: *client* (CSD) or
	*server* (SSD). Note that this is only a hint to the
	compositor. Depending on the compositor's configuration and
	capabilities, it may not have any effect. Default: _server_.

*size*
	Height, in pixels (subject to output scaling), of the
	titlebar. Default: _26_.

*color*
	Titlebar AARRGGBB color. Default: use the default _foreground_
	color.

*button-width*
	Width, in pixels (subject to output scaling), of the
	minimize/maximize/close buttons. Default: _26_.

*button-minimize-color*
	Minimize button's AARRGGBB color. Default: _ff1e90ff_.

*button-maximize-color*
	Maximize button's AARRGGBB color. Default: _ff30ff30_.

*button-close-color*
	Close button's AARRGGBB color. Default: _ffff3030_.

# SECTION: key-bindings

This section lets you override the default key bindings.

The general format is _action_=_combo1_..._comboN_. That is, each
action may have one or more key combinations, space separated. Each
combination is on the form _mod1_+_mod2_+_key_. The names of the
modifiers and the key *must* be a valid XKB key name.

Note that if *Shift* is one of the modifiers, the _key_ *must* be in
upper case. For example, *Control*+*Shift*+*v* will never trigger -
*Control*+*Shift*+*V* is the correct way to write it.

Note that *Alt* is usually called *Mod1*.

A key combination can only be mapped to *one* action. Lets say you
want to bind *Control*+*Shift*+*R* to *fullscreen*. Since this is the
default shortcut for *search-start*, you first need to unmap the
default binding. This can be done by setting _action_=*NONE*;
e.g. *search-start*=*NONE*.

*scrollback-up*
	Scrolls up/back in history. Default: _Shift+Page_Up_.

*scrollback-down*
	Scroll down/forward in history. Default: _Shift+Page_Down_.

*clipboard-copy*
	Copies the current selection into the _clipboard_. Default: _Control+Shift+C_.

*clipboard-paste*
	Pastes from the _clipboard_. Default: _Control+Shift+V_.

*primary-paste*
	Pastes from the _primary selection_. Default: not bound (to a
	_key_, see *mouse-bindings*).

*search-start*
	Starts a scrollback/history search. Default: _Control+Shift+R_.

*font-increase*
	Increases the font size by 0.5pt. Default: _Control+plus
	Control+equal Control+KP\_Add_.

*font-decrease*
	Decreases the font size by 0.5pt. Default: _Control+minus
	Control+KP\_Subtract_.

*font-reset*
	Resets the font size to the default. Default: _Control+0 Control+KP\_0_.

*spawn-terminal*
	Spawns a new terminal. Default: _Control+Shift+Return_.

*minimize*
	Minimizes the window. Default: _not bound_.

*maximize*
	Toggle the maximized state. Default: _not bound_.

*fullscreen*
	Toggles the fullscreen state. Default: _not bound_.

# SECTION: mouse-bindings

This section lets you override the default mouse bindings.

The general format is _action_=_BTN\_<name>_, where _BTN\_<name>_ is
the name of the event code (e.g. *BTN\_LEFT*, *BTN\_RIGHT*). You can
find the event names using *libinput debug-events*.

A button can only be mapped to *one* action. Lets say you want to bind
*BTN\_MIDDLE* to *fullscreen*. Since *BTN\_MIDDLE* is the default
binding for *primary-paste*, you first need to unmap the default
binding. This can be done by setting _action_=*NONE*;
e.g. *primary-paste*=*NONE*.

All actions listed under *key-bindings* can be user here as well.

*primary-paste*
	Pastes from the _primary selection_. Default: _BTN_MIDDLE_.


# FONT FORMAT

The font is specified in FontConfig syntax. That is, a colon-separated
list of font name and font options.

_Examples_:
- Dina:weight=bold:slant=italic
- Courier New:size=12
