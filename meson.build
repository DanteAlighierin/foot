project('foot', 'c',
        version: '1.18.1',
        license: 'MIT',
        meson_version: '>=0.59.0',
        default_options: [
          'c_std=c11',
          'warning_level=1',
          'werror=true',
          'b_ndebug=if-release'])

is_debug_build = get_option('buildtype').startswith('debug')

cc = meson.get_compiler('c')

if cc.has_function('memfd_create',
                   args: ['-D_GNU_SOURCE'],
                   prefix: '#include <sys/mman.h>')
  add_project_arguments('-DMEMFD_CREATE', language: 'c')
endif

# Missing on DragonFly, FreeBSD < 14.1
if cc.has_function('execvpe',
                   args: ['-D_GNU_SOURCE'],
                   prefix: '#include <unistd.h>')
  add_project_arguments('-DEXECVPE', language: 'c')
endif

utmp_backend = get_option('utmp-backend')
if utmp_backend == 'auto'
  host_os = host_machine.system()
  if host_os == 'linux'
    utmp_backend = 'libutempter'
  elif host_os == 'freebsd'
    utmp_backend = 'ulog'
  else
    utmp_backend = 'none'
  endif
endif

utmp_default_helper_path = get_option('utmp-default-helper-path')

if utmp_backend == 'none'
  utmp_add = ''
  utmp_del = ''
  utmp_del_have_argument = false
  utmp_default_helper_path = ''
elif utmp_backend == 'libutempter'
  utmp_add = 'add'
  utmp_del = 'del'
  utmp_del_have_argument = true
  if utmp_default_helper_path == 'auto'
    utmp_default_helper_path = join_paths('/usr', get_option('libdir'), 'utempter', 'utempter')
  endif
elif utmp_backend == 'ulog'
  utmp_add = 'login'
  utmp_del = 'logout'
  utmp_del_have_argument = false
  if utmp_default_helper_path == 'auto'
    utmp_default_helper_path = join_paths('/usr', get_option('libexecdir'), 'ulog-helper')
  endif
else
    error('invalid utmp backend')
endif

add_project_arguments(
  ['-D_GNU_SOURCE=200809L',
   '-DFOOT_DEFAULT_TERM="@0@"'.format(get_option('default-terminfo'))] +
  (utmp_backend != 'none'
  ? ['-DUTMP_ADD="@0@"'.format(utmp_add),
     '-DUTMP_DEL="@0@"'.format(utmp_del),
     '-DUTMP_DEFAULT_HELPER_PATH="@0@"'.format(utmp_default_helper_path)]
  : []) +
  (utmp_del_have_argument
   ? ['-DUTMP_DEL_HAVE_ARGUMENT=1']
   : []) +
  (is_debug_build
    ? ['-D_DEBUG']
   : [cc.get_supported_arguments('-fno-asynchronous-unwind-tables')]) +
  (get_option('ime')
    ? ['-DFOOT_IME_ENABLED=1']
    : []) +
  (get_option('b_pgo') == 'use'
    ? ['-DFOOT_PGO_ENABLED=1']
    : []) +
  cc.get_supported_arguments(
    ['-pedantic',
     '-fstrict-aliasing',
     '-Wstrict-aliasing']),
  language: 'c',
)

terminfo_install_location = get_option('custom-terminfo-install-location')

if terminfo_install_location != ''
  add_project_arguments(
    ['-DFOOT_TERMINFO_PATH="@0@"'.format(
      join_paths(get_option('prefix'), terminfo_install_location))],
    language: 'c')
else
  terminfo_install_location = join_paths(get_option('datadir'), 'terminfo')
endif

# Compute the relative path used by compiler invocations.
source_root = meson.current_source_dir().split('/')
build_root = meson.global_build_root().split('/')
relative_dir_parts = []
i = 0
in_prefix = true
foreach p : build_root
  if i >= source_root.length() or not in_prefix or p != source_root[i]
    in_prefix = false
    relative_dir_parts += '..'
  endif
  i += 1
endforeach
i = 0
in_prefix = true
foreach p : source_root
  if i >= build_root.length() or not in_prefix or build_root[i] != p
    in_prefix = false
    relative_dir_parts += p
  endif
  i += 1
endforeach
relative_dir = join_paths(relative_dir_parts) + '/'

if cc.has_argument('-fmacro-prefix-map=/foo=')
  add_project_arguments('-fmacro-prefix-map=@0@='.format(relative_dir), language: 'c')
endif

math = cc.find_library('m')
threads = [dependency('threads'), cc.find_library('stdthreads', required: false)]
libepoll = dependency('epoll-shim', required: false)
pixman = dependency('pixman-1')
wayland_protocols = dependency('wayland-protocols', version: '>=1.32',
                               fallback: 'wayland-protocols',
                               default_options: ['tests=false'])
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
xkb = dependency('xkbcommon', version: '>=1.0.0')
fontconfig = dependency('fontconfig')
utf8proc = dependency('libutf8proc', required: get_option('grapheme-clustering'))

if utf8proc.found()
  add_project_arguments('-DFOOT_GRAPHEME_CLUSTERING=1', language: 'c')
endif

tllist = dependency('tllist', version: '>=1.1.0', fallback: 'tllist')
fcft = dependency('fcft', version: ['>=3.0.1', '<4.0.0'], fallback: 'fcft')

wayland_protocols_datadir = wayland_protocols.get_variable('pkgdatadir')

wscanner = dependency('wayland-scanner', native: true)
wscanner_prog = find_program(
  wscanner.get_variable('wayland_scanner'), native: true)

wl_proto_headers = []
wl_proto_src = []
wl_proto_xml = [
  wayland_protocols_datadir / 'stable/xdg-shell/xdg-shell.xml',
  wayland_protocols_datadir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
  wayland_protocols_datadir / 'unstable/xdg-output/xdg-output-unstable-v1.xml',
  wayland_protocols_datadir / 'unstable/primary-selection/primary-selection-unstable-v1.xml',
  wayland_protocols_datadir / 'stable/presentation-time/presentation-time.xml',
  wayland_protocols_datadir / 'unstable/text-input/text-input-unstable-v3.xml',
  wayland_protocols_datadir / 'staging/xdg-activation/xdg-activation-v1.xml',
  wayland_protocols_datadir / 'stable/viewporter/viewporter.xml',
  wayland_protocols_datadir / 'staging/fractional-scale/fractional-scale-v1.xml',
  wayland_protocols_datadir / 'unstable/tablet/tablet-unstable-v2.xml',  # required by cursor-shape-v1
  wayland_protocols_datadir / 'staging/cursor-shape/cursor-shape-v1.xml',
  wayland_protocols_datadir / 'staging/single-pixel-buffer/single-pixel-buffer-v1.xml',
  ]

if wayland_protocols.version().version_compare('>=1.37')
  add_project_arguments('-DHAVE_XDG_TOPLEVEL_ICON', language: 'c')
  wl_proto_xml += [wayland_protocols_datadir / 'staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml']
  xdg_toplevel_icon = true
else
  xdg_toplevel_icon = false
endif

foreach prot : wl_proto_xml
  wl_proto_headers += custom_target(
    prot.underscorify() + '-client-header',
    output: '@BASENAME@.h',
    input: prot,
    command: [wscanner_prog, 'client-header', '@INPUT@', '@OUTPUT@'])

  wl_proto_src += custom_target(
    prot.underscorify() + '-private-code',
    output: '@BASENAME@.c',
    input: prot,
    command: [wscanner_prog, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach

env = find_program('env', native: true)
generate_version_sh = files('generate-version.sh')
version = custom_target(
  'generate_version',
  build_always_stale: true,
  output: 'version.h',
  command: [env, 'LC_ALL=C', generate_version_sh, meson.project_version(), '@CURRENT_SOURCE_DIR@', '@OUTPUT@'])

python = find_program('python3', native: true)
generate_builtin_terminfo_py = files('scripts/generate-builtin-terminfo.py')
foot_terminfo = files('foot.info')
builtin_terminfo = custom_target(
  'generate_builtin_terminfo',
  output: 'foot-terminfo.h',
  command: [python, generate_builtin_terminfo_py,
            '@default_terminfo@', foot_terminfo, 'foot', '@OUTPUT@']
)

generate_emoji_variation_sequences = files('scripts/generate-emoji-variation-sequences.py')
emoji_variation_sequences = custom_target(
  'generate_emoji_variation_sequences',
  input: 'unicode/emoji-variation-sequences.txt',
  output: 'emoji-variation-sequences.h',
  command: [python, generate_emoji_variation_sequences, '@INPUT@', '@OUTPUT@']
)

common = static_library(
  'common',
  'log.c', 'log.h',
  'char32.c', 'char32.h',
  'debug.c', 'debug.h',
  'macros.h',
  'xmalloc.c', 'xmalloc.h',
  'xsnprintf.c', 'xsnprintf.h'
)

misc = static_library(
  'misc',
  'hsl.c', 'hsl.h',
  'macros.h',
  'misc.c', 'misc.h',
  'uri.c', 'uri.h'
)

vtlib = static_library(
  'vtlib',
  'base64.c', 'base64.h',
  'composed.c', 'composed.h',
  'cursor-shape.c', 'cursor-shape.h',
  'csi.c', 'csi.h',
  'dcs.c', 'dcs.h',
  'macros.h',
  'osc.c', 'osc.h',
  'sixel.c', 'sixel.h',
  'vt.c', 'vt.h',
  builtin_terminfo, emoji_variation_sequences,
  wl_proto_src + wl_proto_headers,
  version,
  dependencies: [libepoll, pixman, fcft, tllist, wayland_client, xkb, utf8proc],
  link_with: [common, misc],
)

pgolib = static_library(
  'pgolib',
  'grid.c', 'grid.h',
  'selection.c', 'selection.h',
  'terminal.c', 'terminal.h',
  wl_proto_src + wl_proto_headers,
  dependencies: [libepoll, pixman, fcft, tllist, wayland_client, xkb, utf8proc],
  link_with: vtlib,
)

tokenize = static_library(
  'tokenizelib',
  'tokenize.c',
  link_with: [common],
)

if get_option('b_pgo') == 'generate'
  executable(
    'pgo',
    'pgo/pgo.c',
    wl_proto_src + wl_proto_headers,
    dependencies: [math, threads, libepoll, pixman, wayland_client, xkb, utf8proc, fcft, tllist],
    link_with: pgolib,
  )
endif

executable(
  'foot',
  'async.c', 'async.h',
  'box-drawing.c', 'box-drawing.h',
  'config.c', 'config.h',
  'commands.c', 'commands.h',
  'extract.c', 'extract.h',
  'fdm.c', 'fdm.h',
  'foot-features.h',
  'ime.c', 'ime.h',
  'input.c', 'input.h',
  'key-binding.c', 'key-binding.h',
  'main.c',
  'notify.c', 'notify.h',
  'quirks.c', 'quirks.h',
  'reaper.c', 'reaper.h',
  'render.c', 'render.h',
  'search.c', 'search.h',
  'server.c', 'server.h', 'client-protocol.h',
  'shm.c', 'shm.h',
  'slave.c', 'slave.h',
  'spawn.c', 'spawn.h',
  'tokenize.c', 'tokenize.h',
  'unicode-mode.c', 'unicode-mode.h',
  'url-mode.c', 'url-mode.h',
  'user-notification.c', 'user-notification.h',
  'wayland.c', 'wayland.h', 'shm-formats.h',
  wl_proto_src + wl_proto_headers, version,
  dependencies: [math, threads, libepoll, pixman, wayland_client, wayland_cursor, xkb, fontconfig, utf8proc,
                 tllist, fcft],
  link_with: pgolib,
  install: true)

executable(
  'footclient',
  'client.c', 'client-protocol.h',
  'foot-features.h',
  'macros.h',
  'util.h',
  version,
  dependencies: [tllist],
  link_with: common,
  install: true)

install_data(
  'foot.desktop', 'foot-server.desktop', 'footclient.desktop',
  install_dir: join_paths(get_option('datadir'), 'applications'))

systemd = dependency('systemd', required: false)
custom_systemd_units_dir = get_option('systemd-units-dir')

if systemd.found() or custom_systemd_units_dir != ''
  configuration = configuration_data()
  configuration.set('bindir', join_paths(get_option('prefix'), get_option('bindir')))

  if (custom_systemd_units_dir == '')
    systemd_units_dir = systemd.get_variable('systemduserunitdir')
  else
    systemd_units_dir = custom_systemd_units_dir
  endif

  configure_file(
    configuration: configuration,
    input: 'foot-server.service.in',
    output: '@BASENAME@',
    install_dir: systemd_units_dir
    )

  install_data(
    'foot-server.socket',
    install_dir: systemd_units_dir)
endif

scdoc = dependency('scdoc', native: true, required: get_option('docs'))
install_data('foot.ini', install_dir: join_paths(get_option('sysconfdir'), 'xdg', 'foot'))
if scdoc.found()
  install_data(
    'LICENSE', 'README.md', 'CHANGELOG.md',
    install_dir: join_paths(get_option('datadir'), 'doc', 'foot'))
  subdir('doc')
endif

if get_option('themes')
  install_subdir('themes', install_dir: join_paths(get_option('datadir'), 'foot'))
endif

terminfo_base_name = get_option('terminfo-base-name')
if terminfo_base_name == ''
  terminfo_base_name = get_option('default-terminfo')
endif

tic = find_program('tic', native: true, required: get_option('terminfo'))
if tic.found()
  conf_data = configuration_data(
    {
      'default_terminfo': terminfo_base_name
    }
  )

  preprocessed = configure_file(
    input: 'foot.info',
    output: 'foot.info.preprocessed',
    configuration: conf_data,
  )
  custom_target(
    'terminfo',
    output: terminfo_base_name[0],
    input: preprocessed,
    command: [tic, '-x', '-o', '@OUTDIR@', '-e', '@0@,@0@-direct'.format(terminfo_base_name), '@INPUT@'],
    install: true,
    install_dir: terminfo_install_location
  )
endif

subdir('completions')
subdir('icons')
subdir('utils')

if (get_option('tests'))
  subdir('tests')
endif

summary(
  {
    'Documentation': scdoc.found(),
    'Themes': get_option('themes'),
    'IME': get_option('ime'),
    'Grapheme clustering': utf8proc.found(),
    'Wayland: xdg-toplevel-icon-v1': xdg_toplevel_icon,
    'utmp backend': utmp_backend,
    'utmp helper default path': utmp_default_helper_path,
    'Build terminfo': tic.found(),
    'Terminfo base name': terminfo_base_name,
    'Terminfo install location': terminfo_install_location,
    'Default TERM': get_option('default-terminfo'),
    'Set TERMINFO': get_option('custom-terminfo-install-location') != '',
    'Build tests': get_option('tests'),
  },
  bool_yn: true
)
