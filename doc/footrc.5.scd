footrc(5)

# NAME
footrc - configuration file

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
	Initial window width and height in pixels (subject to output
	scaling), on the form _WIDTHxHEIGHT_. Default: _700x500_.

*pad*
	Padding between border and glyphs, in pixels (subject to output
	scaling), on the form _XxY_. Default: _2x2_.

*initial-window-mode*
	Initial window mode for each newly spawned window: *windowed*,
	*maximized* or *fullscreen*. Default: _windowed_.

*shell*
	Executable to launch. Typically a shell. Default: _$SHELL_ if set,
	otherwise the user's default shell (as specified in
	_/etc/passwd_). You can also pass arguments. For example
	"/bin/bash --norc".

*login-shell*
	Boolean. If enabled, the shell will be launched as a login shell,
	by prepending a '-' to argv[0]. Default: _no_.

*term*
	Value to set the environment variable *TERM* to. Default: _foot_.

*title*
	Initial window title. Default: _foot_.

*app-id*
	Value to set the *app-id* property on the Wayland window to. The
	compositor can use this value to e.g. group multiple windows, or
	apply window management rules. Default: _foot_.

*workers*
	Number of threads to use for rendering. Set to 0 to disable
	multithreading. Default: the number of available logical CPUs
	(including SMT). Note that this is not always the best value. In
	some cases, the number of physical _cores_ is better.


# SECTION: scrollback

*lines*
	Number of scrollback lines. Default: _1000_.

*multiplier*
	Amount to multiply mouse scrolling with. It is a decimal number,
	i.e. fractions are allowed. Default: _1.0_.

*indicator-position*
	Configures the style of the scrollback position indicator. One of
	*none*, *fixed* or *relative*. *none* disables the indicator
	completely. *fixed* always renders the indicator near the top at
	the window, and *relative* renders the indicator at the position
	corresponding to the current scrollback position. Default:
	_relative_.

*indicator-format*
	Which format to use when displaying the scrollback position
	indicator. Either _percentage_, _line_, or a custom fixed
	string. This option is ignored if
	*indicator-position=none*. Default: _empty string_.


# SECTION: cursor

This section controls the cursor style and color. Note that
applications can change these at runtime.

*style*
	Configures the default cursor style, and is one of: _block_, _bar_
	or _underline_. Note that this can be overridden by
	applications. Default: _block_.

*blink*
	Boolean. Enables blinking cursor. Note that this can be overridden
	by applications. Default: _no_.

*color*
	Two RRGGBB values specifying the foreground (text) and background
	(cursor) colors for the cursor. Default: inversed foreground and
	background colors. Note that this value only applies to the block
	cursor. The other cursor styles are always rendered with the
	foreground color.


# SECTION: mouse

*hide-when-typing*
	Boolean. When enabled, the mouse cursor is hidden while typing.


# SECTION: colors

This section controls the 16 ANSI colors and the default foreground
and background colors. Note that applications can change these at
runtime.

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
	Minimize button's AARRGGBB color. Default: use the default
	_regular4_ color (blue).

*button-maximize-color*
	Maximize button's AARRGGBB color. Default: use the default
	_regular2_ color (green).

*button-close-color*
	Close button's AARRGGBB color. Default: use the default _regular1_
	color (red).


# SECTION: key-bindings

This section lets you override the default key bindings.

The general format is _action=combo1...comboN_. That is, each action
may have one or more key combinations, space separated. Each
combination is on the form _mod1+mod2+key_. The names of the modifiers
and the key *must* be valid XKB key names.

Note that if *Shift* is one of the modifiers, the _key_ *must* be in
upper case. For example, *Control+Shift+v* will never trigger -
*Control+Shift+V* is the correct way to write it.

Note that *Alt* is usually called *Mod1*.

A key combination can only be mapped to *one* action. Lets say you
want to bind *Control+Shift+R* to *fullscreen*. Since this is the
default shortcut for *search-start*, you first need to unmap the
default binding. This can be done by setting _action=none_;
e.g. *search-start=none*.

*scrollback-up*
	Scrolls up/back in history. Default: _Shift+Page\_Up_.

*scrollback-down*
	Scroll down/forward in history. Default: _Shift+Page\_Down_.

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
	Spawns a new terminal. If the shell has been configured to emit
	the OSC 7 escape sequence, the new terminal will start in the
	current working directory. Default: _Control+Shift+N_.

*minimize*
	Minimizes the window. Default: _not bound_.

*maximize*
	Toggle the maximized state. Default: _not bound_.

*fullscreen*
	Toggles the fullscreen state. Default: _not bound_.

*pipe-visible*, *pipe-scrollback*, *pipe-selected*
	Pipes the currently visible text, the entire scrollback, or the
	currently selected text to an external tool. The syntax for this
	option is a bit special; the first part of the value is the
	command to execute enclosed in "[]", followed by the binding(s).
	
	You can configure multiple pipes as long as the command strings
	are different and the key bindings are unique.
	
	Note that the command is *not* automatically run inside a shell;
	use *sh -c "command line"* if you need that.
	
	Example:
		*pipe-visible=[sh -c "xurls | uniq | tac | bemenu | xargs -r
		firefox"] Control+Print*
	
	Default: _not bound_


# SECTION: search-bindings

This section lets you override the default key bindings used in
scrollback search mode. The syntax is exactly the same as the regular
**key-bindings**.

*cancel*
	Aborts the search. The viewport is restored and the _primary
	selection_ is **not** updated. Default: _Control+g Escape_.

*commit*
	Exit search mode and copy current selection into the _primary
	selection_. Viewport is **not** restored. To copy the selection to
	the regular _clipboard_, use *Control+Shift+C*. Default: _Return_.

*find-prev*
	Search **backwards** in the scrollback history for the next
	match. Default: _Control+r_.

*find-next*
	Searchs **forwards** in the scrollback history for the next
	match. Default: _Control+s_.

*cursor-left*
	Moves the cursor in the search box one **character** to the
	left. Default: _Left Control+b_.

*cursor-left-word*
	Moves the cursor in the search box one **word** to the
	left. Default: _Control+Left Mod1+b_.

*cursor-right*
	Moves the cursor in the search box one **character** to the
	right. Default: _Right Control+f_.

*cursor-right-word*
	Moves the cursor in the search box one **word** to the
	right. Default: _Control+Left Mod1+b_.

*cursor-home*
	Moves the cursor in the search box to the beginning of the
	input. Default: _Home Control+a_.

*cursor-end*
	Moves the cursor in the search box to the end of the
	input. Default: _End Control+e_.

*delete-prev*
	Deletes the **character before** the cursor. Default: _BackSpace_.

*delete-prev-word*
	Deletes the **word before** the cursor. Default: _Mod1+BackSpace
	Control+BackSpace_.

*delete-next*
	Deletes the **character after** the cursor. Default: _Delete_.

*delete-next-word*
	Deletes the **word after** the cursor. Default: _Mod1+b
	Control+Delete_.

*extend-to-word-boundary*
	Extend current selection to the next word boundary. Default:
	_Control+w_.

*extend-to-next-whitespace*
	Extend the current selection to the next whitespace. Default:
	_Control+Shift+W_.


# SECTION: mouse-bindings

This section lets you override the default mouse bindings.

The general format is _action=BTN\_<name>_, where _BTN\_<name>_ is
the name of the event code (e.g. *BTN\_LEFT*, *BTN\_RIGHT*). You can
find the event names using *libinput debug-events*.

A button can only be mapped to *one* action. Lets say you want to bind
*BTN\_MIDDLE* to *fullscreen*. Since *BTN\_MIDDLE* is the default
binding for *primary-paste*, you first need to unmap the default
binding. This can be done by setting _action=none_;
e.g. *primary-paste=none*.

All actions listed under *key-bindings* can be user here as well.

*primary-paste*
	Pastes from the _primary selection_. Default: _BTN\_MIDDLE_.


# FONT FORMAT

The font is specified in FontConfig syntax. That is, a colon-separated
list of font name and font options.

_Examples_:
- Dina:weight=bold:slant=italic
- Courier New:size=12

# TWEAKS

This section is for advanced users and describes configuration options
that can be used to tweak foot's low-level behavior.

These options are *not* included in the example configuration. You
should not change these unless you understand what they do and note
that changing the default values *will* print a warning when launching
foot.

When reporting bugs, please mention if, and to what, you have changed
any of these options.

*delayed-render-lower*, *delayed-render-upper*
	These two values control the timeouts (in nanoseconds) that are
	used to mitigate screen flicker caused by clients writing large,
	non-atomic screen updates.
	
	If a client splits up a screen update over multiple *write*(3)
	calls, we may end up rendering an intermediate frame, quickly
	followed by another frame with the final screen content. For
	example, the client may erase part of the screen (or scroll) in
	one write, and then write new content in one or more subsequent
	writes. Rendering the frame when the screen has been erased, but
	not yet filled with new content will be perceived as screen
	flicker.
	
	The *real* solution to this is _Application Synchronized Updates_
	(https://gitlab.freedesktop.org/terminal-wg/specifications/-/merge_requests/2).
	
	The problem with this is twofold - first, it has not yet been
	standardized, and thus there are not many terminal emulators that
	implement it (foot *does* implement it), and second, applications
	must be patched to use it.
	
	Until this has happened, foot offers an interim workaround; an
	attempt to mitigate the screen flicker *without* affecting neither
	performance nor latency.
	
	It is based on the fact that the screen is updated at a fixed
	interval (typically 60Hz). For us, this means it does not matter
	if we render a new frame at the *beginning* of a frame interval,
	or at the *end*. Thus, the goal is to introduce a delay between
	receiving client data and rendering the resulting state, but
	without causing a frame skip.
	
	While it should be possible to estimate the amount of time left
	until the next frame, foot's algorithm is currently not that
	advanced, but is based on statistics I guess you could say - the
	delay we introduce is so small that the risk of pushing the frame
	over to the next frame interval is also very small.
	
	Now, that was a lof of text. But what is it foot actually does?
	
	When receiving client data, it schedules a timer, the
	*delayed-render-lower*. If we do not receive any more client data
	before the timer has run out, we render the frame. If however, we
	do receive more data, the timer is re-scheduled. That is, each
	time we receive client data, frame rendering is delayed another
	*delayed-render-lower* nanoseconds.
	
	Now, while this works very well with most clients, it would be
	possible to construct a malicious client that keeps writing data
	at a slow pace. To the user, this would look like foot has frozen
	as we never get to render a new frame. To prevent this, an upper
	limit is set - *delayed-render-upper*. If this timer runs out, we
	render the frame regardless of what the client is doing.
	
	If changing these values, note that the lower timeout *must* be
	set lower than the upper timeout, but that this is not verified by
	foot. Furthermore, both values must be less than 16ms (that is,
	16000000 nanoseconds).
	
	You can disable the feature altoghether by setting either value to
	0. In this case, frames are rendered "as soon as possible".
	
	Default: lower=_500000_ (0.5ms), upper=_8333333_ (8.3ms - half a
	frame interval).

*max-shm-pool-size-mb*

	This option controls the amount of *virtual* memory used by the
	pixmap memory to which the terminal screen content is rendered.
	
	It does *not* change how much physical memory foot uses.
	
	Foot uses a memory mapping trick to implement fast rendering of
	interactive scrolling (typically, but applies to "slow" scrolling
	in general). Example: holding down the 'up' or 'down' arrow key to
	scroll in a text editor.
	
	For this to work, it needs a large amount of virtual address
	space. Again, note that this is *not* physical memory.
	
	On a normal x64 based computer, each process has 128TB of virtual
	address space, and newer ones have 64PB. This is an insane amount
	and most applications do not use anywhere near that amount.
	
	Each foot terminal window can allocate up to 2GB of virtual
	address space. With 128TB of address space, that means a maximum
	of 65536 windows in server/daemon mode (for 2GB). That should be
	enough, yes?
	
	However, the Wayland compositor *also* needs to allocate the same
	amount of virtual address space. Thus, it has a slightly higher
	chance of running out of address space since it needs to host
	*all* running Wayland clients in the same way, at the same time.
	
	In the off chance that this becomes a problem for you, you can
	reduce the amount used with this option.
	
	Or, for optimal performance, you can increase it to the maximum
	allowed value, 2GB (but note that you most likely will not notice
	any difference compared to the default value).
	
	Note: this feature is always disabled in 32-bit.
	
	Default: _512_. Maximum allowed: _2048_ (2GB).

# SEE ALSO

*foot*(1), *footclient*(1)
