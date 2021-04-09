foot.ini(5)

# NAME
foot.ini - configuration file for *foot*(1)

# DESCRIPTION

*foot* uses the standard _unix configuration format_, with section based
key/value pairs. The default section is unnamed (i.e. not prefixed
with a _[section]_).

foot will search for a configuration file in the following locations,
in this order:

- *XDG_CONFIG_HOME/foot/foot.ini*
- *~/.config/foot/foot.ini*
- *XDG_CONFIG_DIRS/foot/foot.ini*

# SECTION: default

*shell*
	Executable to launch. Typically a shell. Default: _$SHELL_ if set,
	otherwise the user's default shell (as specified in
	_/etc/passwd_). You can also pass arguments. For example
	*/bin/bash --norc*.

*login-shell*
	Boolean. If enabled, the shell will be launched as a login shell,
	by prepending a '-' to argv[0]. Default: _no_.

*term*
	Value to set the environment variable *TERM* to. Default: _foot_
	or _xterm-256color_ if built with _-Dterminfo=disabled_

*font*, *font-bold*, *font-italic*, *font-bold-italic*
	Comma separated list of fonts to use, in fontconfig format. That
	is, a font name followed by a list of colon-separated
	options. Most noteworthy is *:size=n*, which is used to set the
	font size.
	
	Examples:
	- Dina:weight=bold:slant=italic
	- Courier New:size=12
	- Fantasque Sans Mono:fontfeatures=ss01
	
	For each option, the first font is the primary font. The remaining
	fonts are fallback fonts that will be used whenever a glyph cannot
	be found in the primary font.
	
	The fallback fonts are searched in the order they appear. If a
	glyph cannot be found in any of the fallback fonts, the dynamic
	fallback list from fontconfig (for the primary font) is
	searched.
	
	*font-bold*, *font-italic* and *font-bold-italic* allow custom
	fonts to be used for bold/italic/bold+italic fonts. If left
	unconfigured, the bold/italic variants of the regular font(s)
	specified in *font* are used. *Note*: you _may_ have to tweak the
	size(s) of the custom bold/italic fonts to match the regular font.
	
	To disable bold and/or italic fonts, set e.g. *font-bold* to
	_exactly_ the same value as *font*.
	
	Default: _monospace:size=8_ (*font*), _not set_ (*font-bold*,
	*font-italic*, *font-bold-italic*).

*line-height*
	An absolute value, in _points_, that override line height from the
	font metrics.
	
	You can specify a height in _pixels_ by using the _px_ suffix:
	e.g. *line-height=12px*.
	
	See also: *vertical-letter-offset*.
	
	Default: _not set_.

*letter-spacing*
	Spacing between letters, in _points_. A positive value will
	increase the cell size, and a negative value shrinks it.
	
	You can specify a letter spacing in _pixels_ by using the _px_
	suffix: e.g. *letter-spacing=2px*.
	
	See also: *horizontal-letter-offset*.
	
	Default: _0_.

*horizontal-letter-offset*, *vertical-letter-offset*
	Configure the horizontal and vertical offsets used when
	positioning glyphs within cells, in _points_, relative to the top
	left corner.
	
	To specify an offset in _pixels_, append _px_:
	e.g. *horizontal-letter-offset=2px*.
	
	Default: _0_.

*box-drawings-uses-font-glyphs*
	Boolean. When disabled, foot generates box/line drawing characters
	itself. The are several advantages to doing this instead of using
	font glyphs:
	
		- No antaliasing effects where e.g. line endpoints appear dimmer.
		- Line- and box characters are guaranteed to span the entire cell,
		  resulting in a gap-less appearance.
		- No alignment issues, i.e. lines are centered when they should be.
		- Many fonts lack some, or all, of the line- and box drawing
		  characters, causing fallback fonts to be used, which results
		  in out-of-place (for example, badly sized) looking glyphs.
	
	When enabled, box/line drawing characters are rendered using font
	glyphs. This may result in a more uniform look, in some use cases.
	
	Default: _no_.

*dpi-aware*
	*auto*, *yes*, or *no*. When set to *yes*, fonts are sized using
	the monitor's DPI, making a font of a given size have the same
	physical size, regardless of monitor. In other words, if you drag
	a foot window between different monitors, the font size remains
	the same.
	
	In this mode, the monitor's scaling factor is ignored; doubling
	the scaling factor will *not* double the font size.
	
	When set to *no*, the monitor's DPI is ignored. The font is
	instead sized using the monitor's scaling factor; doubling the
	scaling factor *does* double the font size.
	
	Finally, if set to *auto*, fonts will be sized using the monitor's
	DPI on monitors with a scaling factor of 1, but otherwise using
	the scaling factor.
	
	Default: _auto_

*pad*
	Padding between border and glyphs, in pixels (subject to output
	scaling), on the form _XxY_.
	
	This will add _at least_ X pixels on both the left and right
	sides, and Y pixels on the top and bottom sides. The grid content
	will be anchored in the top left corner. I.e. if the window
	manager forces an odd window size on foot, the additional pixels
	will be added to the right and bottom sides.
	
	To instead center the grid content, append *center* (e.g. *pad=5x5
	center*).
	
	Default: _2x2_.

*resize-delay-ms*
	Time, in milliseconds, of "idle time" "before foot sends the new
	window dimensions to the client application while doing an
	interactive resize of a foot window. Idle time in this context is
	a period of time where the window size is not changing.
	
	In other words, while you are fiddling with the window size, foot
	does not send the updated dimensions to the client. Only when you
	pause the fiddling for *relay-size-ms* milliseconds is the client
	updated.
	
	Emphasis is on _while_ here; as soon as the interactive resize
	ends (i.e. when you let go of the window border), the final
	dimensions is sent to the client, without any delays.
	
	Setting it to 0 disables the delay completely.
	
	Default: _100_.

*initial-window-size-pixels*
	Initial window width and height in _pixels_ (subject to output
	scaling), on the form _WIDTHxHEIGHT_. The height _includes_ the
	titlebar when using CSDs. Mutually exclusive to
	*initial-window-size-chars*. Default: _700x500_.

*initial-window-size-chars*
	Initial window width and height in _characters_, on the form
	_WIDTHxHEIGHT_. Mutually exclusive to
	*initial-window-size-pixels*.'
	
	Note that if you have a multi-monitor setup, with different
	scaling factors, there is a possibility the window size will not
	be set correctly. If that is the case, use
	*initial-window-size-pixels* instead.
	
	Default: _not set_.

*initial-window-mode*
	Initial window mode for each newly spawned window: *windowed*,
	*maximized* or *fullscreen*. Default: _windowed_.

*title*
	Initial window title. Default: _foot_.

*app-id*
	Value to set the *app-id* property on the Wayland window to. The
	compositor can use this value to e.g. group multiple windows, or
	apply window management rules. Default: _foot_.

*bold-text-in-bright*
	Boolean. When enabled, bold text is rendered in a brighter color
	(in addition to using a bold font). Default: _no_.

*bell*
	Action to perform when receiving a *BEL* character. Can be set to
	either *set-urgency*, *notify* or *none*.
	
	When set to *set-urgency*, the margins will be painted in red
	whenever *BEL* is received while the window does *not* have
	keyboard focus. Note that Wayland currently does not have an
	_urgency_ hint like X11. The value *set-urgency* was chosen for
	forward-compatibility in the hopes that a corresponding Wayland
	protocol is added in the future (in which case foot will use that
	instead of painting its margins red).
	
	Applications can enable/disable this feature programmatically with
	the *CSI ? 1042 h* and *CSI ? 1042 l* escape sequences.
	
	_Note_: expect this feature to be *replaced* with proper
	compositor urgency support once/if that gets implemented.
	
	When set to *notify*, foot will emit a desktop notification using
	the command specified in the *notify* option, but only if the foot
	window does *not* have keyboard focus.
	
	When set to *none*, no special action is taken when receiving *BEL*.
	
	Default: _none_.

*word-delimiters*
	String of characters that act as word delimiters when selecting
	text. Note that whitespace characters are _always_ word
	delimiters, regardless of this setting. Default: _,│`|:"'()[]{}<>_

*jump-label-letters*
	String of characters that will be when generating key sequences
	for URL jump labels. Default: _sadfjklewcmpgh_.

*notify*
	Command to execute to display a notification. _${title}_ and
	_${body}_ will be replaced with the notification's actual _title_
	and _body_ (message content).
	
	Applications can trigger notifications in the following ways:
	
	- OSC 777: *\\e]777;notify;<title>;<body>\\e\\\\*
	
	Notifications are *inhibited* if the foot window has keyboard
	focus.
	
	Default: _notify-send -a foot -i foot ${title} ${body}_.

*url-launch*
	Command to execute when opening URLs. _${url}_ will be replaced
	with the actual URL. Default: _xdg-open ${url}_.

*selection-target*
	Clipboard target to automatically copy selected text to. One of
	*none*, *primary*, *clipboard* or *both*. Default: _primary_.

*workers*
	Number of threads to use for rendering. Set to 0 to disable
	multithreading. Default: the number of available logical CPUs
	(including SMT). Note that this is not always the best value. In
	some cases, the number of physical _cores_ is better.

*osc8-underline*
	When to underline OSC-8 URLs. Possible values are *url-mode* and
	*always*.
	
	When set to *url-mode*, OSC-8 URLs are only highlighted in URL
	mode, just like auto-detected URLs.
	
	When set to *always*, OSC-8 URLs are always highlighted,
	regardless of their other attributes (bold, italic etc). Note that
	this does _not_ make them clickable.
	
	Default: _url-mode_


# SECTION: scrollback

*lines*
	Number of scrollback lines. The maximum number of allocated lines
	will be this value plus the number of visible lines, rounded up to
	the nearest power of 2. Default: _1000_.

*multiplier*
	Amount to multiply mouse scrolling with. It is a decimal number,
	i.e. fractions are allowed. Default: _3.0_.

*indicator-position*
	Configures the style of the scrollback position indicator. One of
	*none*, *fixed* or *relative*. *none* disables the indicator
	completely. *fixed* always renders the indicator near the top of
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
	Boolean. When enabled, the mouse cursor is hidden while
	typing. Default: _no_.

*alternate-scroll-mode*
	Boolean. This option controls the initial value for the _alternate
	scroll mode_. When this mode is enabled, mouse scroll events are
	translated to _up_/_down_ key events when displaying the alternate
	screen.
	
	This lets you scroll with the mouse in e.g. pagers (like _less_)
	without enabling native mouse support in them.
	
	Alternate scrolling is *not* used if the application enables
	native mouse support.
	
	This option can be modified by applications at run-time using the
	escape sequences *CSI ? 1007 h* (enable) and *CSI ? 1007 l*
	(disable).
	
	Default: _yes_.


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

*selection-foreground*, *selection-background*
	Foreground (text) and background color to use in selected
	text. Note that *both* options must be set, or the default will be
	used. Default: _inverse foreground/background_.

*jump-labels*
	To RRGGBB values specifying the foreground (text) and background
	colors to use when rendering jump labels in URL mode. Default:
	_regular0 regular3_.

*urls*
	Color to use for the underline used to highlight URLs in URL
	mode. Default: _regular3_.


# SECTION: csd

This section controls the look of the _CSDs_ (Client Side
Decorations). Note that the default is to *not* use CSDs, but instead
to use _SSDs_ (Server Side Decorations) when the compositor supports
it.

Note that unlike the colors defined in the _colors_ section, the color
values here are in AARRGGBB format. I.e. they contain an alpha
component.

*preferred*
	Which type of window decorations to prefer: *client* (CSD),
	*server* (SSD) or *none*.
	
	Note that this is only a hint to the compositor. Depending on
	compositor support, and how it has been configured, it may
	instruct foot to use CSDs even though this option has been set to
	*server*, or render SSDs despite *client* or *none* being set.
	
	Default: _server_.

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

Note that if *Shift* is one of the modifiers, the _key_ *must not* be
in upper case. For example, *Control+Shift+V* will never trigger, but
*Control+Shift+v* will.

Note that *Alt* is usually called *Mod1*.

A key combination can only be mapped to *one* action. Lets say you
want to bind *Control+Shift+R* to *fullscreen*. Since this is the
default shortcut for *search-start*, you first need to unmap the
default binding. This can be done by setting _action=none_;
e.g. *search-start=none*.

*scrollback-up-page*
	Scrolls up/back one page in history. Default: _Shift+Page\_Up_.

*scrollback-up-half-page*
	Scrolls up/back half of a page in history. Default: _not set_.

*scrollback-up-line*
	Scrolls up/back a single line in history. Default: _not set_.

*scrollback-down-page*
	Scroll down/forward one page in history. Default:
	_Shift+Page\_Down_.

*scrollback-down-half-page*
	Scroll down/forward half of a page in history. Default: _not set_.

*scrollback-down-line*
	Scroll down/forward a single line in history. Default: _not set_.

*clipboard-copy*
	Copies the current selection into the _clipboard_. Default: _Control+Shift+c_.

*clipboard-paste*
	Pastes from the _clipboard_. Default: _Control+Shift+v_.

*primary-paste*
	Pastes from the _primary selection_. Default: _Shift+Insert_ (also
	defined in *mouse-bindings*).

*search-start*
	Starts a scrollback/history search. Default: _Control+Shift+r_.

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
	current working directory. Default: _Control+Shift+n_.

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
		*pipe-visible=[sh -c "xurls | uniq | tac | fuzzel | xargs -r
		firefox"] Control+Print*
	
	Default: _not bound_

*show-urls-launch*
	Enter URL mode, where all currently visible URLs are tagged with a
	jump label with a key sequence that will open the URL. Default:
	_Control+Shift+u_.

*show-urls-copy*
	Enter URL mode, where all currently visible URLs are tagged with a
	jump label with a key sequence that will place the URL in the
	clipboard. Default: _none_.


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
	the regular _clipboard_, use *Control+Shift+c*. Default: _Return_.

*find-prev*
	Search **backwards** in the scrollback history for the next
	match. Default: _Control+r_.

*find-next*
	Searches **forwards** in the scrollback history for the next
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
	Deletes the **word after** the cursor. Default: _Mod1+d
	Control+Delete_.

*extend-to-word-boundary*
	Extend current selection to the next word boundary. Default:
	_Control+w_.

*extend-to-next-whitespace*
	Extend the current selection to the next whitespace. Default:
	_Control+Shift+w_.

*clipboard-paste*
	Paste from the _clipboard_ into the search buffer. Default:
	_Control+v Control+y_.

*primary-paste*
	Paste from the _primary selection_ into the search
	buffer. Default: _Shift+Insert_.


# SECTION: url-bindings

This section lets you override the default key bindings used in URL
mode. The syntax is exactly the same as the regular **key-bindings**.

*cancel*
	Exits URL mode without opening an URL. Default: _Control+g
	Control+d Escape_.

*toggle-url-visible*
	By default, the jump label only shows the key sequence required to
	activate it. This is fine as long as the URL is visible in the
	original text.
	
	But with e.g. OSC-8 URLs (the terminal version of HTML anchors,
	i.e. "links"), the text on the screen can be something completey
	different than the URL.
	
	This action toggles between showing and hiding the URL on the jump
	label.
	
	Default: _t_.


# SECTION: mouse-bindings

This section lets you override the default mouse bindings.

The general format is _action=combo1...comboN_. That is, each action
may have one or more key combinations, space separated. Each
combination is on the form _mod1+mod2+BTN\_<name>[-COUNT]_. The names
of the modifiers *must* be valid XKB key names, and the button name
*must* be a valid libinput name. You can find the button names using
*libinput debug-events*.

Note that *Shift* cannot be used as a modifier in mouse bindings since
it is used to enable selection when the client application is grabbing
the mouse.

The trailing *COUNT* is optional and specifies the click count
required to trigger the binding. The default if *COUNT* is omitted is
_1_.

A modifier+button combination can only be mapped to *one* action. Lets
say you want to bind *BTN\_MIDDLE* to *fullscreen*. Since
*BTN\_MIDDLE* is the default binding for *primary-paste*, you first
need to unmap the default binding. This can be done by setting
_action=none_; e.g. *primary-paste=none*.

All actions listed under *key-bindings* can be user here as well.

*select-begin*
	Begin an interactive selection. The selection is finalized, and
	copied to the _primary selection_, when the button is
	released. Default: _BTN\_LEFT_.

*select-begin-block*
	Begin an interactive block selection. The selection is finalized,
	and copied to the _primary selection_, when the button is
	released. Default: _Control+BTN\_LEFT_.

*select-word*
	Begin an interactive word-wise selection, where words are
	separated by whitespace and all characters defined by the
	*word-delimiters* option. The selection is finalized, and copied
	to the _primary selection_, when the button is released. Default:
	_BTN\_LEFT-2_.

*select-word-whitespace*
	Same as *select-word*, but the characters in the *word-delimiters*
	option are ignored. I.e only whitespace characters act as
	delimiters. The selection is finalized, and copied to the _primary
	selection_, when the button is released. Default:
	_Control+_BTN\_LEFT-2_.

*select-row*
	Begin an interactive row-wise selection. The selection is
	finalized, and copied to the _primary selection_, when the button
	is released. Default: _BTN\_LEFT-3_.

*select-extend*
	Interactively extend an existing selection, using the original
	selection mode (normal, block, word-wise or row-wise). The
	selection is finalized, and copied to the _primary selection_,
	when the button is released. Default: _BTN\_RIGHT_.

*select-extend-character-wise*
	Same as *select-extend*, but forces the selection mode to _normal_
	(i.e. character wise). Note that this causes subsequent
	*select-extend* operations to be character wise. This action is
	ignored for block selections. Default: _Control+BTN\_RIGHT_.


*primary-paste*
	Pastes from the _primary selection_. Default: _BTN\_MIDDLE_.


# TWEAK

This section is for advanced users and describes configuration options
that can be used to tweak foot's low-level behavior.

These options are *not* included in the example configuration. You
should not change these unless you understand what they do and note
that changing the default values *will* print a warning when launching
foot.

Note that these options may change, or be removed at any time, without
prior notice.

When reporting bugs, please mention if, and to what, you have changed
any of these options.

*scaling-filter*
	Overrides the default scaling filter used when down-scaling bitmap
	fonts (e.g. emoji fonts). Possible values are *none*, *nearest*,
	*bilinear*, *cubic* or *lanczos3*. *cubic* and *lanczos3* produce
	the best results, but are slower (with *lanczos3* being the best
	_and_ slowest).
	
	Default: _lanczos3_.

*allow-overflowing-double-width-glyphs*
	Boolean. when enabled, double width glyphs with a character width
	of 1 are allowed to overflow into the neighbouring cell.
	
	One use case for this is fonts "icon" characters in the Unicode
	private usage area, e.g. Nerd Fonts, or Powerline Fonts. Without
	this option, such glyphs will appear "cut off".
	
	Another use case are legacy emoji characters like *WHITE FROWNING
	FACE*.
	
	Note: this feature uses _heuristics_ to determine *which* glyphs
	should be allowed to overflow.
	
	Default: _yes_.

*render-timer*
	Enables a frame rendering timer, that prints the time it takes to
	render each frame, in microseconds, either on-screen, to stderr,
	or both. Valid values are *none*, *osd*, *log* and
	*both*. Default: _none_.

*box-drawing-base-thickness*
	Line thickness to use for *LIGHT* box drawing line characters, in
	points. This value is converted to pixels using the monitor's DPI,
	and then multiplied with the cell size. The end result is that a
	larger font (and thus larger cells) result in thicker
	lines. Default: _0.04_.

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
	
	Now, that was a lot of text. But what is it foot actually does?
	
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
	
	You can disable the feature altogether by setting either value to
	0. In this case, frames are rendered "as soon as possible".
	
	Default: lower=_500000_ (0.5ms), upper=_8333333_ (8.3ms - half a
	frame interval).

*damage-whole-window*
	Boolean. When enabled, foot will 'damage' the entire window each
	time a frame has been rendered. This forces the compositor to
	redraw the entire window. If disabled, foot will only 'damage'
	updated rows.
	
	There is normally *no* reason to enable this. However, it has been
	seen to workaround an issue with _fractional scaling_ in _Gnome_.
	
	Note that enabling this option is likely to increase CPU and/or
	GPU usage (by the compositor, not by foot), and may have a
	negative impact on battery life.
	
	Default: _no_.

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
	
	Setting it to 0 disables the feature.
	
	Note: this feature is always disabled in 32-bit.
	
	Default: _512_. Maximum allowed: _2048_ (2GB).

# SEE ALSO

*foot*(1), *footclient*(1)
