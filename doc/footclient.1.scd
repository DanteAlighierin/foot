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
that of the terminal (thus, *footclient* does not exit until the
terminal has terminated).

# OPTIONS

*-t*,*--term*=_TERM_
	Value to set the environment variable *TERM* to. Default: _foot_.

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

*-d*,*--log-level*={*info*,*warning*,*error*}
	Log level, used both for log output on stderr as well as
	syslog. Default: _info_.

*-l*,*--log-colorize*=[{*never*,*always*,*auto*}]
	Enables or disables colorization of log output on stderr.

*-v*,*--version*
	Show the version number and quit

# SEE ALSO

*foot*(1)
