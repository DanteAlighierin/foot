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
	Path to configuration file, see *foot.ini*(5) for details.

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
	and *ENVIRONMENT*). Default: _@default_terminfo@_.

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

*-s*,*--server*[=_PATH_|_FD_]
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

	If the argument is a number, foot will interpret it as the file descriptor
	of a socket provided by a supervision daemon (such as systemd or s6), and
	use that socket as it's own.

	Two systemd units (foot-server@.{service,socket}) are provided to use that
	feature with systemd. They need to be instantiated with the value of
	$WAYLAND_DISPLAY (multiples instances can co-exists).

	Note that starting *foot --server* as a systemd service will use
	the environment of the systemd user instance; thus, if you need specific
	environment variables, you'll need to import them using *systemctl --user
	import-environment* or use a drop-in for the foot-server service.

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

*-e*
	Ignored; for compatibility with *xterm -e*.

	This option was added in response to several program launchers
	passing *-e* to arbitrary terminals, under the assumption that
	they all implement the same semantics for it as *xterm*(1).
	Ignoring it allows foot to be invoked as e.g. *foot -e man foot*
	with the same results as with xterm, instead of producing an
	"invalid option" error.

# KEYBOARD SHORTCUTS

The following keyboard shortcuts are available by default. They can be
changed in *foot.ini*(5). There are also more actions (disabled by
default) available; see *foot.ini*(5).

## NORMAL MODE

*shift*+*page up*/*page down*
	Scroll up/down in history

*ctrl*+*shift*+*c*, *XF86Copy*
	Copy selected text to the _clipboard_

*ctrl*+*shift*+*v*, *XF86Paste*
	Paste from _clipboard_

*shift*+*insert*
	Paste from the _primary selection_.

*ctrl*+*shift*+*r*
	Start a scrollback search

*ctrl*+*+*, *ctrl*+*=*
	Increase font size by 0.5pt

*ctrl*+*-*
	Decrease font size by 0.5pt

*ctrl*+*0*
	Reset font size

*ctrl*+*shift*+*n*
	Spawn a new terminal. If the shell has been configured to emit the
	_OSC 7_ escape sequence, the new terminal will start in the
	current working directory.

*ctrl*+*shift*+*u*
	Activate URL mode, allowing you to "launch" URLs.

*ctrl*+*shift*+*z*
	Jump to the previous, currently not visible, prompt. Requires
	shell integration.

*ctrl*+*shift*+*x*
	Jump to the next prompt. Requires shell integration.

## SCROLLBACK SEARCH

*ctrl*+*r*
	Search _backward_ for the next match. If the search string is
	empty, the last searched-for string is used.

*ctrl*+*s*
	Search _forward_ for the next match. If the search string is
	empty, the last searched-for string is used.

*ctrl*+*w*
	Extend current selection (and thus the search criteria) to the end
	of the word, or the next word if currently at a word separating
	character.

*ctrl*+*shift*+*w*
	Same as *ctrl*+*w*, except that the only word separating
	characters are whitespace characters.

*ctrl*+*v*, *ctrl*+*shift*+*v*, *ctrl*+*y*, *XF86Paste*
	Paste from clipboard into the search buffer.

*shift*+*insert*
	Paste from primary selection into the search buffer.

*escape*, *ctrl*+*g*, *ctrl*+*c*
	Cancel the search

*return*
	Finish the search and copy the current match to the primary
	selection. The terminal selection is kept, allowing you to press
	*ctrl*+*shift*+*c* to copy it to the clipboard.

## URL MODE

*t*
	Toggle URL visibility in jump label.

*escape*, *ctrl*+*g*, *ctrl*+*c*, *ctrl*+*d*
	Exit URL mode without launching a URL.

## MOUSE SHORTCUTS

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
what activating a URL _does_; instead of opening it, it copies it to
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

foot will search for a configuration file in the following locations,
in this order:

	- *XDG_CONFIG_HOME/foot/foot.ini* (defaulting to
	  *$HOME/.config/foot/foot.ini* if unset)
	- *XDG_CONFIG_DIRS/foot/foot.ini* (defaulting to
	  */etc/xdg/foot/foot.ini* if unset)

An example configuration file containing all options with their default value
commented out will usually be installed to */etc/xdg/foot/foot.ini*.

For more information, see *foot.ini*(5).

# SHELL INTEGRATION

## Current working directory

New foot terminal instances (bound to *ctrl*+*shift*+*n* by default)
will open in the current working directory, if the shell in the
“parent” terminal reports directory changes.

This is done with the OSC-7 escape sequence. Most shells can be
scripted to do this, if they do not support it natively. See the wiki
(https://codeberg.org/dnkl/foot/wiki#user-content-spawning-new-terminal-instances-in-the-current-working-directory)
for details.


## Jumping between prompts

Foot can move the current viewport to focus prompts of already
executed commands (bound to *ctrl*+*shift*+*z*/*x* by default).

For this to work, the shell needs to emit an OSC-133;A
(*\\E]133;A\\E\\\\*) sequence before each prompt.

In zsh, one way to do this is to add a _precmd_ hook:

	*precmd() {
		print -Pn "\\e]133;A\\e\\\\"
	}*

See the wiki
(https://codeberg.org/dnkl/foot/wiki#user-content-jumping-between-prompts)
for details, and examples for other shells.

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
colors). Using *\*-direct* terminfo entries has been known to crash
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

# XTGETTCAP

*XTGETTCAP* is an escape sequence initially introduced by XTerm, and
also implemented (and extended, to some degree) by Kitty.

It allows querying the terminal for terminfo classic, file-based,
terminfo definition. For example, if all applications used this
feature, you would no longer have to install foot’s terminfo on remote
hosts you SSH into.

XTerm’s implementation (as of XTerm-370) only supports querying key
(as in keyboard keys) capabilities, and three custom capabilities:

- TN - terminal name
- Co -  number of colors (alias for the colors capability)
- RGB - number of bits per color channel (different semantics from
  the RGB capability in file-based terminfo definitions!).

Kitty has extended this, and also supports querying all integer and
string capabilities.

Foot supports this, and extends it even further, to also include
boolean capabilities. This means foot’s entire terminfo can be queried
via *XTGETTCAP*.

Note that both Kitty and foot handles responses to multi-capability
queries slightly differently, compared to XTerm.

XTerm will send a single DCS reply, with ;-separated
capability/value pairs. There are a couple of issues with this:

- The success/fail flag in the beginning of the response is always 1
  (success), unless the very first queried capability is invalid.
- XTerm will not respond at all to an invalid capability, unless it’s
  the first one in the XTGETTCAP query.
- XTerm will end the response at the first invalid capability.

In other words, if you send a large multi-capability query, you will
only get responses up to, but not including, the first invalid
capability. All subsequent capabilities will be dropped.

Kitty and foot on the other hand, send one DCS response for each
capability in the multi query. This allows us to send a proper
success/fail flag for each queried capability. Responses for all
queried capabilities are always sent. No queries are ever dropped.

# EXIT STATUS

Foot will exit with code 230 if there is a failure in foot itself.

In all other cases, the exit code is that of the client application
(i.e. the shell).

# ENVIRONMENT

## Variables used by foot

*SHELL*
	The default child process to run, when no _command_ argument is
	specified and the *shell* option in *foot.ini*(5) is not set.

*HOME*
	Used to determine the location of the configuration file, see
	*foot.ini*(5) for details.

*XDG\_CONFIG\_HOME*
	Used to determine the location of the configuration file, see
	*foot.ini*(5) for details.

*XDG\_CONFIG\_DIRS*
	Used to determine the location of the configuration file, see
	*foot.ini*(5) for details.

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

## Variables set in the child process

*TERM*
	terminfo/termcap identifier. This is used by client applications
	to determine which capabilities a terminal supports. The value is
	set according to either the *--term* command-line option or the
	*term* config option in *foot.ini*(5).

*COLORTERM*
	This variable is set to *truecolor*, to indicate to client
	applications that 24-bit RGB colors are supported.

In addition to the variables listed above, custom environment
variables may be defined in *foot.ini*(5).

# BUGS

Please report bugs to https://codeberg.org/dnkl/foot/issues

Before you open a new issue, please search existing bug reports, both
open and closed ones. Chances are someone else has already reported
the same issue.

The report should contain the following:

- Foot version (*foot --version*).
- Log output from foot (start foot from another terminal).
- Which Wayland compositor (and version) you are running.
- If reporting a crash, please try to provide a *bt full* backtrace
  with symbols.
- Steps to reproduce. The more details the better.

# IRC

\#foot on irc.libera.chat

# SEE ALSO

*foot.ini*(5), *footclient*(1)
