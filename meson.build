project('foot', 'c',
        version: '1.1.0',
        license: 'MIT',
        meson_version: '>=0.47.0',
        default_options: [
          'c_std=c11',
          'warning_level=1',
          'werror=true',
          'b_ndebug=if-release'])

is_debug_build = get_option('buildtype').startswith('debug')

add_project_arguments(
  ['-D_GNU_SOURCE=200809L'] + (is_debug_build ? ['-D_DEBUG'] : []),
  language: 'c',
)

cc = meson.get_compiler('c')
math = cc.find_library('m')
threads = dependency('threads')
pixman = dependency('pixman-1')
wayland_protocols = dependency('wayland-protocols')
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
xkb = dependency('xkbcommon')

tllist = dependency('tllist', version: '>=1.0.0', fallback: ['tllist', 'tllist'])
fcft = dependency('fcft', version: ['>=1.1.0', '<1.2.0'], fallback: ['fcft', 'fcft'])

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

executable(
  'foot',
  'async.c', 'async.h',
  'base64.c', 'base64.h',
  'config.c', 'config.h',
  'commands.c', 'commands.h',
  'csi.c', 'csi.h',
  'dcs.c', 'dcs.h',
  'fdm.c', 'fdm.h',
  'grid.c', 'grid.h',
  'input.c', 'input.h',
  'log.c', 'log.h',
  'main.c',
  'misc.c', 'misc.h',
  'osc.c', 'osc.h',
  'render.c', 'render.h',
  'search.c', 'search.h',
  'selection.c', 'selection.h',
  'server.c', 'server.h',
  'shm.c', 'shm.h',
  'slave.c', 'slave.h',
  'terminal.c', 'terminal.h',
  'tokenize.c', 'tokenize.h',
  'vt.c', 'vt.h',
  'wayland.c', 'wayland.h',
  wl_proto_src + wl_proto_headers, version,
  dependencies: [math, threads, pixman, wayland_client, wayland_cursor, xkb,
                tllist, fcft],
  install: true)

executable(
  'footclient',
  'client.c',
  'log.c', 'log.h', version,
  install: true)

custom_target(
  'terminfo',
  output: 'f',
  input: 'foot.info',
  command: ['tic', '-x', '-o', '@OUTDIR@', '-e', 'foot,foot-direct', '@INPUT@'],
  install: true,
  install_dir: join_paths(get_option('datadir'), 'terminfo'))

install_data('foot.desktop', 'foot-server.desktop', install_dir: join_paths(get_option('datadir'), 'applications'))
install_data('footrc', install_dir: join_paths(get_option('datadir'), 'foot'))

subdir('completions')
subdir('doc')
