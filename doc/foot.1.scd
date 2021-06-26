foot(1)

# NAME
foot - Wayland terminal emulator

# SYNOPSIS
*foot* [_OPTIONS_]++
*foot* [_OPTIONS_] <_command_> [_COMMAND OPTIONS_]

All trailing (non-option) arguments are treated as a command, and its
arguments, to execute (instead of the default shell).

# DESCRIPTION

*foot* is a Wayland terminal emulator. Running it without arguments
will start a new terminal window with your default shell.

You can override the default shell by appending a custom command to
the foot command line

	*foot sh -c "echo hello world && sleep 5"*

# OPTIONS

*-c*,*--config*=_PATH_
	Path to configuration file. Default:
	*$XDG_CONFIG_HOME/foot/foot.ini*.

*-C*,*--check-config*
	Verify configuration and then exit with 0 if ok, otherwise exit
	with 230 (see *EXIT STATUS*).

*-o*,*--override*=[_SECTION_.]_KEY_=_VALUE_
	Override an option set in the configuration file. If _SECTION_ is not
	given, defaults to _main_.

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

*-w*,*--window-size-pixels*=_WIDTHxHEIGHT_
	Set initial window width and height, in pixels. Default: _700x500_.

*-W*,*--window-size-chars*=_WIDTHxHEIGHT_
	Set initial window width and height, in characters. Default: _not set_.

*-t*,*--term*=_TERM_
	Value to set the environment variable *TERM* to (see *TERMINFO*
	and *ENVIRONMENT*). Default: _foot_.

*-T*,*--title*=_TITLE_
	Initial window title. Default: _foot_.

*-a*,*--app-id*=_ID_
	Value to set the *app-id* property on the Wayland window
	to. Default: _foot_.

*-m*,*--maximized*
	Start in maximized mode. If both *--maximized* and *--fullscreen*
	are specified, the _last_ one takes precedence.

*-F*,*--fullscreen*
	Start in fullscreen mode. If both *--maximized* and *--fullscreen*
	are specified, the _last_ one takes precedence.

*-L*,*--login-shell*
	Start a login shell, by prepending a '-' to argv[0].

*-D*,*--working-directory*=_DIR_
	Initial working directory for the client application. Default:
	_CWD of foot_.

*-s*,*--server*[=_PATH_]
	Run as a server. In this mode, a single foot instance hosts
	multiple terminals (windows). Use *footclient*(1) to launch new
	terminals.
	
	This saves some memory since for example fonts and glyph caches
	can be shared between the terminals.
	
	It also saves upstart time since the config has already been
	loaded and parsed, and most importantly, fonts have already been
	loaded (and their glyph caches are likely to already have been
	populated).
	
	Each terminal will have its own rendering threads, but all Wayland
	communication, as well as input/output to the shell, is
	multiplexed in the main thread. Thus, this mode might result in
	slightly worse performance when multiple terminals are under heavy
	load.
	
	Also be aware that should one terminal crash, it will take all the
	others with it.
	
	The default path is
	*$XDG\_RUNTIME\_DIR/foot-$WAYLAND\_DISPLAY.sock*.
	
	If *$XDG\_RUNTIME\_DIR* is not set, the default path is instead
	*/tmp/foot.sock*.
	
	If *$XDG\_RUNTIME\_DIR* is set, but *$WAYLAND\_DISPLAY* is not,
	the default path is *$XDG\_RUNTIME\_DIR/foot.sock*.
	
	Note that if you change the default, you will also need to use the
	*--server-socket* option in *footclient*(1) and point it to your
	custom socket path.

*-H*,*--hold*
	Remain open after child process exits.

*-p*,*--print-pid*=_FILE_|_FD_
	Print PID to this file, or FD, when successfully started. The file
	(or FD) is closed immediately after writing the PID. When a _FILE_
	as been specified, the file is unlinked at exit.

	This option can only be used in combination with *-s*,*--server*.

*-d*,*--log-level*={*info*,*warning*,*error*,*none*}
	Log level, used both for log output on stderr as well as
	syslog. Default: _info_.

*-l*,*--log-colorize*=[{*never*,*always*,*auto*}]
	Enables or disables colorization of log output on stderr. Default:
	_auto_.

*-S*,*--log-no-syslog*
	Disables syslog logging. Logging is only done on stderr. This
	option can only be used in combination with *-s*,*--server*.

*-v*,*--version*
	Show the version number and quit.

# EXIT STATUS

Foot will exit with code 230 if there is a failure in foot itself.

In all other cases, the exit code is that of the client application
(i.e. the shell).

# KEYBOARD SHORTCUTS

The following keyboard shortcuts are available.

## NORMAL MODE

Note that these are just the defaults; they can be changed in
*foot.ini(5)*.

*shift*+*page up*/*page down*
	Scroll up/down in history

*ctrl*+*shift*+*c*
	Copy selected text to the _clipboard_

*ctrl*+*shift*+*v*
	Paste from _clipboard_

*shift*+*insert*
	Paste from the _primary selection_.

*ctrl*+*shift*+*r*
	Start a scrollback search

*ctrl*+*+*, *ctrl*+*=*
	Increase font size by 1pt

*ctrl*+*-*
	Decrease font size by 1pt

*ctrl*+*0*
	Reset font size

*ctrl*+*shift*+*n*
	Spawn a new terminal. If the shell has been configured to emit the
	_OSC 7_ escape sequence, the new terminal will start in the
	current working directory.

## SCROLLBACK SEARCH

*ctrl*+*r*
	Search _backward_ for the next match

*ctrl*+*s*
	Search _forward_ for the next match

*ctrl*+*w*
	Extend current selection (and thus the search criteria) to the end
	of the word, or the next word if currently at a word separating
	character.

*ctrl*+*shift*+*w*
	Same as *ctrl*+*w*, except that the only word separating
	characters are whitespace characters.

*ctrl*+*v*, *ctrl*+*y*
	Paste from clipboard into the searh buffer.

*shift*+*insert*
	Paste from primary selection into the search buffer.

*escape*, *ctrl*+*g*, *ctrl*+*c*
	Cancel the search

*return*
	Finish the search and copy the current match to the primary
	selection. The terminal selection is kept, allowing you to press
	*ctrl*+*shift*+*c* to copy it to the clipboard.

# MOUSE SHORTCUTS

*left*, single-click
	Drag to select; when released, the selected text is copied to the
	_primary_ selection. This feature is normally *disabled* whenever
	the client has enabled _mouse tracking_, but can be forced by
	holding *shift*.
	
	Holding *ctrl* will create a block selection.

*left*, double-click
	Selects the _word_ (separated by spaces, period, comma,
	parenthesis etc) under the pointer. Hold *ctrl* to select
	everything under the pointer up to, and until, the next space
	characters.

*left*, triple-click
	Selects the entire row

*middle*
	Paste from the _primary_ selection

*right*
	Extend current selection. Clicking immediately extends the
	selection, while hold-and-drag allows you to interactively resize
	the selection.

*wheel*
	Scroll up/down in history

# FONT FORMAT

The font is specified in FontConfig syntax. That is, a colon-separated
list of font name and font options.

_Examples_:
- Dina:weight=bold:slant=italic
- Courier New:size=12

# URLs

Foot supports URL detection. But, unlike many other terminal
emulators, where URLs are highlighted when they are hovered and opened
by clicking on them, foot uses a keyboard driven approach.

Pressing *ctrl*+*shift*+*u* enters _“URL mode”_, where all currently
visible URLs are underlined, and is associated with a
_“jump-label”_. The jump-label indicates the _key sequence_
(e.g. *”AF”*) to use to activate the URL.

The key binding can, of course, be customized, like all other key
bindings in foot. See *show-urls-launch* and *show-urls-copy* in
*foot.ini*(5).

*show-urls-launch* by default opens the URL with *xdg-open*. This can
be changed with the *url-launch* option.

*show-urls-copy* is an alternative to *show-urls-launch*, that changes
what activating an URL _does_; instead of opening it, it copies it to
the clipboard. It is unbound by default.

Jump label colors, the URL underline color, and the letters used in
the jump label key sequences can be configured.

# ALT/META CHARACTERS

By default, foot prefixes meta characters with *ESC*. This corresponds
to XTerm's *metaSendsEscape* option set to *true*.

This can be disabled programmatically with *\E[?1036l* (and enabled
again with *\E[?1036h*).

When disabled, foot will instead set the 8:th bit of meta character
and then UTF-8 encode it. This corresponds to XTerm's *eightBitMeta*
option set to *true*.

This can also be disabled programmatically with *rmm* (Reset Meta Mode,
*\E[?1034l*), and enabled again with *smm* (Set Meta Mode,
*\E[?1034h*).

# BACKSPACE

Foot transmits DEL (*^?*) on backspace. This corresponds to XTerm's
*backarrowKey* option set to *false*, and to DECBKM being _reset_.

To instead transmit BS (*^H*), press *ctrl*+*backspace*.

Note that foot does *not* implement DECBKM, and that the behavior
described above *cannot* be changed.

Finally, pressing *alt* will prefix the transmitted byte with ESC.

# KEYPAD

By default, *Num Lock* overrides the run-time configuration keypad
mode; when active, the keypad is always considered to be in
_numerical_ mode. This corresponds to XTerm's *numLock* option set to
*true*.

In this mode, the keypad keys always sends either numbers (Num Lock is
active) or cursor movement keys (up, down, left, right, page up, page
down etc).

This can be disabled programmatically with *\E[?1035l* (and enabled
again with *\E[?1035h*).

When disabled, the keypad sends custom escape sequences instead of
numbers, when in _application_ mode.

# CONFIGURATION

See *foot.ini*(5)

# TERMINFO

Client applications use the terminfo identifier specified by the
environment variable *TERM* (set by foot) to determine terminal
capabilities.

Foot has two terminfo definitions: *foot* and *foot-direct*, with
*foot* being the default.

The difference between the two is in the number of colors they
describe; *foot* describes 256 colors and *foot-direct* 16.7 million
colors (24-bit truecolor).

Note that using the *foot* terminfo does not limit the number of
usable colors to 256; applications can still use 24-bit RGB colors. In
fact, most applications work best with *foot* (including 24-bit
colors)). Using *\*-direct* terminfo entries has been known to crash
some ncurses applications even.

There are however applications that need a *\*-direct* terminfo entry
for 24-bit support. Emacs is one such example.

While using either *foot* or *foot-direct* is strongly recommended, it
is possible to use e.g. *xterm-256color* as well. This can be useful
when remoting to a system where foot's terminfo entries cannot easily
be installed.

Note that terminfo entries can be installed in the user's home
directory. I.e. if you do not have root access, or if there is no
distro package for foot's terminfo entries, you can install foot's
terminfo entries manually, by copying *foot* and *foot-direct* to
*~/.terminfo/f/*.

# ENVIRONMENT

The following environment variables are used by foot:

*SHELL*
	The default child process to run, when no _command_ argument is
	specified and the *shell* option in *foot.ini*(5) is not set.

*XDG\_CONFIG\_HOME*
	Used to determine the default location of *foot.ini*(5).

*XDG\_CONFIG\_DIRS*
	Used to determine the default fallback location of *foot.ini*(5),
	when not found in *${XDG\_CONFIG\_HOME:-~/.config}*.

*XDG\_RUNTIME\_DIR*
	Used to construct the default _PATH_ for the *--server*
	option, when no explicit argument is given (see above).

*WAYLAND\_DISPLAY*
	Used to construct the default _PATH_ for the *--server*
	option, when no explicit argument is given (see above).

*XCURSOR\_THEME*
	The name of the *Xcursor*(3) theme to use for pointers (typically
	set by the Wayland compositor).

*XCURSOR\_SIZE*
	The size to use for *Xcursor*(3) pointers (typically set by the
	Wayland compositor).

The following environment variables are set in the child process:

*TERM*
	terminfo/termcap identifier. This is used by client applications
	to determine which capabilities a terminal supports. The value is
	set according to either the *--term* command-line option or the
	*term* config option in *foot.ini*(5).

*COLORTERM*
	This variable is set to *truecolor*, to indicate to client
	applications that 24-bit RGB colors are supported.

# BUGS

Please report bugs to https://codeberg.org/dnkl/foot/issues

The report should contain the following:

- Which Wayland compositor (and version) you are running
- Foot version (*foot --version*)
- Log output from foot (start foot from another terminal)
- If reporting a crash, please try to provide a *bt full* backtrace
  with symbols
- Steps to reproduce. The more details the better

# IRC

\#foot on irc.libera.chat

# SEE ALSO

*foot.ini*(5), *footclient*(1)
