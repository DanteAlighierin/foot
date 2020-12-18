project('foot', 'c',
        version: '1.6.0',
        license: 'MIT',
        meson_version: '>=0.53.0',
        default_options: [
          'c_std=c18',
          'warning_level=1',
          'werror=true',
          'b_ndebug=if-release'])

is_debug_build = get_option('buildtype').startswith('debug')

cc = meson.get_compiler('c')

add_project_arguments(
  ['-D_POSIX_C_SOURCE=200809L', '-D_GNU_SOURCE=200809L'] +
  (is_debug_build
    ? ['-D_DEBUG']
    : [cc.get_supported_arguments('-fno-asynchronous-unwind-tables')]) +
  (get_option('ime')
    ? ['-DFOOT_IME_ENABLED=1']
    : []) +
  cc.get_supported_arguments(
    ['-pedantic',
     '-fstrict-aliasing',
     '-Wstrict-aliasing']),
  language: 'c',
)

# Compute the relative path used by compiler invocations.
source_root = meson.current_source_dir().split('/')
build_root = meson.build_root().split('/')
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
threads = dependency('threads')
pixman = dependency('pixman-1')
wayland_protocols = dependency('wayland-protocols')
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
xkb = dependency('xkbcommon')
fontconfig = dependency('fontconfig')

tllist = dependency('tllist', version: '>=1.0.4', fallback: 'tllist')
fcft = dependency('fcft', version: ['>=2.3.0', '<3.0.0'], fallback: 'fcft')

wayland_protocols_datadir = wayland_protocols.get_pkgconfig_variable('pkgdatadir')

wscanner = dependency('wayland-scanner', native: true)
wscanner_prog = find_program(
  wscanner.get_pkgconfig_variable('wayland_scanner'), native: true)

wl_proto_headers = []
wl_proto_src = []
foreach prot : [
  wayland_protocols_datadir + '/stable/xdg-shell/xdg-shell.xml',
  wayland_protocols_datadir + '/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
  wayland_protocols_datadir + '/unstable/xdg-output/xdg-output-unstable-v1.xml',
  wayland_protocols_datadir + '/unstable/primary-selection/primary-selection-unstable-v1.xml',
  wayland_protocols_datadir + '/stable/presentation-time/presentation-time.xml',
  wayland_protocols_datadir + '/unstable/text-input/text-input-unstable-v3.xml',
  ]

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

generate_version_sh = files('generate-version.sh')
version = custom_target(
  'generate_version',
  build_always_stale: true,
  output: 'version.h',
  command: [generate_version_sh, meson.project_version(), '@SOURCE_DIR@', '@OUTPUT@'])

misc = static_library(
  'misc',
  'hsl.c', 'hsl.h',
  'log.c', 'log.h',
  'macros.h',
  'misc.c', 'misc.h',
  'uri.c', 'uri.h',
  'xmalloc.c', 'xmalloc.h',
)

vtlib = static_library(
  'vtlib',
  'base64.c', 'base64.h',
  'csi.c', 'csi.h',
  'dcs.c', 'dcs.h',
  'osc.c', 'osc.h',
  'sixel.c', 'sixel.h',
  'vt.c', 'vt.h',
  version,
  dependencies: [pixman, fcft, tllist],
  link_with: misc,
)

pgolib = static_library(
  'pgolib',
  'grid.c', 'grid.h',
  'selection.c', 'selection.h',
  'terminal.c', 'terminal.h',
  dependencies: [pixman, fcft, tllist],
  link_with: vtlib,
)

executable(
  'pgo',
  'pgo/pgo.c',
  wl_proto_src,
  dependencies: [math, threads, pixman, wayland_client, fcft, tllist],
  link_with: pgolib,
)

executable(
  'foot',
  'async.c', 'async.h',
  'config.c', 'config.h',
  'commands.c', 'commands.h',
  'extract.c', 'extract.h',
  'fdm.c', 'fdm.h',
  'foot-features.h',
  'ime.c', 'ime.h',
  'input.c', 'input.h',
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
  'user-notification.h',
  'wayland.c', 'wayland.h',
  wl_proto_src + wl_proto_headers, version,
  dependencies: [math, threads, pixman, wayland_client, wayland_cursor, xkb, fontconfig,
                 tllist, fcft],
  link_with: pgolib,
  install: true)

executable(
  'footclient',
  'client.c', 'client-protocol.h',
  'foot-features.h',
  'log.c', 'log.h',
  'macros.h',
  'xmalloc.c', 'xmalloc.h',
  version,
  install: true)

tic = find_program('tic', native: true)
custom_target(
  'terminfo',
  output: 'f',
  input: 'foot.info',
  command: [tic, '-x', '-o', '@OUTDIR@', '-e', 'foot,foot-direct', '@INPUT@'],
  install: true,
  install_dir: join_paths(get_option('datadir'), 'terminfo'))

install_data(
  'LICENSE', 'README.md', 'CHANGELOG.md',
  install_dir: join_paths(get_option('datadir'), 'doc', 'foot'))
install_data(
  'foot.desktop', 'foot-server.desktop',
  install_dir: join_paths(get_option('datadir'), 'applications'))
install_data('foot.ini', install_dir: join_paths(get_option('datadir'), 'foot'))

subdir('completions')
subdir('doc')
subdir('icons')

summary(
  {
    'IME': get_option('ime'),
  },
  bool_yn: true
)
