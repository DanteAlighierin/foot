footclient(1)

# NAME
footclient - start new terminals in a foot server

# SYNOPSIS
*footclient* [_OPTIONS_]++
*footclient* [_OPTIONS_] <_command_> [_COMMAND OPTIONS_]

All trailing (non-option) arguments are treated as a command, and its
arguments, to execute (instead of the default shell).

# DESCRIPTION

*footclient* is used together with *foot*(1) in *--server*
mode.

Running it without arguments will open a new terminal window (hosted
in the foot server), with your default shell. The exit code will be
that of the terminal. I.e *footclient* does not exit until the
terminal has terminated.

# OPTIONS

*-t*,*--term*=_TERM_
	Value to set the environment variable *TERM* to (see *TERMINFO*
	and *ENVIRONMENT*). Default: _@default_terminfo@_.

*-T*,*--title*=_TITLE_
	Initial window title. Default: _foot_.

*-a*,*--app-id*=_ID_
	Value to set the *app-id* property on the Wayland window
	to. Default: _foot_.

*-w*,*--window-size-pixels*=_WIDTHxHEIGHT_
	Set initial window width and height, in pixels. Default: _700x500_.

*-W*,*--window-size-chars*=_WIDTHxHEIGHT_
	Set initial window width and height, in characters. Default: _not set_.

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
	_CWD of footclient_.

*-s*,*--server-socket*=_PATH_
	Connect to _PATH_ instead of
	*$XDG\_RUNTIME\_DIR/foot-$WAYLAND\_DISPLAY.sock*.

*-H*,*--hold*
	Remain open after child process exits.

*-N*,*--no-wait*
	Detach the client process from the running terminal, exiting
	immediately.

*-o*,*--override*=[_SECTION_.]_KEY_=_VALUE_
	Override an option set in the configuration file. If _SECTION_ is not
	given, defaults to _main_.

*-E*,*--client-environment*
	The child process in the new terminal instance will use
	footclient's environment, instead of the server's.

*-d*,*--log-level*={*info*,*warning*,*error*,*none*}
	Log level, used both for log output on stderr as well as
	syslog. Default: _info_.

*-l*,*--log-colorize*=[{*never*,*always*,*auto*}]
	Enables or disables colorization of log output on stderr.

*-v*,*--version*
	Show the version number and quit

*-e*
	Ignored; for compatibility with *xterm -e*. See *foot*(1) for more
	details.

# EXIT STATUS

Footlient will exit with code 220 if there is a failure in footclient
itself (for example, the server socket does not exist).

If *-N*,*--no-wait* is used, footclient exits with code 0 as soon as
the foot server has been instructed to open a new window.

If not, footclient may also exit with code 230. This indicates a
failure in the foot server.

In all other cases the exit code is that of the client application
(i.e. the shell).

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

## Variables used by footclient

*XDG\_RUNTIME\_DIR*
	Used to construct the default _PATH_ for the *--server-socket*
	option, when no explicit argument is given (see above).

*WAYLAND\_DISPLAY*
	Used to construct the default _PATH_ for the *--server-socket*
	option, when no explicit argument is given (see above).

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

# SEE ALSO

*foot*(1)
