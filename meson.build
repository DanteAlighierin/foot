project('foot', 'c',
        version: '0.9.0',
        license: 'MIT',
        default_options: [
          'c_std=c11',
          'warning_level=1',
          'werror=true',
          'b_ndebug=if-release'])

is_debug_build = get_option('buildtype').startswith('debug')

version = '"@0@"'.format(meson.project_version())

sh = find_program('sh', native: true)
git = find_program('git', required: false, native: true)

if git.found()
  commit_hash = run_command(
    [sh.path(), '-c',
     '@0@ --git-dir="$MESON_SOURCE_ROOT/.git" describe --always --tags'.format(
       git.path())])

  branch = run_command(
    [sh.path(), '-c',
     '@0@ --git-dir="$MESON_SOURCE_ROOT/.git" rev-parse --abbrev-ref HEAD'.format(
       git.path())])

  if commit_hash.returncode() == 0 and branch.returncode() == 0
    version = '"@0@ (" __DATE__ ", branch \'@1@\')"'.format(
      commit_hash.stdout().strip(), branch.stdout().strip())
  endif
endif

add_project_arguments(
  ['-D_GNU_SOURCE=200809L',
   '-DFOOT_VERSION=@0@'.format(version)] +
  (is_debug_build ? ['-D_DEBUG'] : []),
  language: 'c',
)

cc = meson.get_compiler('c')
math = cc.find_library('m')

threads = dependency('threads')
freetype = dependency('freetype2')
fontconfig = dependency('fontconfig')
pixman = dependency('pixman-1')
wayland_protocols = dependency('wayland-protocols')
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
xkb = dependency('xkbcommon')

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

executable(
  'foot',
  'base64.c', 'base64.h',
  'config.c', 'config.h',
  'commands.c', 'commands.h',
  'csi.c', 'csi.h',
  'font.c', 'font.h',
  'grid.c', 'grid.h',
  'input.c', 'input.h',
  'log.c', 'log.h',
  'main.c',
  'osc.c', 'osc.h',
  'render.c', 'render.h',
  'search.c', 'search.h',
  'selection.c', 'selection.h',
  'shm.c', 'shm.h',
  'slave.c', 'slave.h',
  'terminal.c', 'terminal.h',
  'tokenize.c', 'tokenize.h',
  'tllist.h',
  'vt.c', 'vt.h',
  wl_proto_src + wl_proto_headers,
  dependencies: [threads, math, freetype, fontconfig, pixman, wayland_client, wayland_cursor, xkb],
  install: true)

custom_target(
  'terminfo',
  output: 'f',
  input: 'foot.info',
  command: ['tic', '-x', '-o', '@OUTDIR@', '-e', 'foot,foot-direct', '@INPUT@'],
  install: true,
  install_dir: join_paths(get_option('datadir'), 'terminfo'))

install_data('foot.desktop', install_dir: join_paths(get_option('datadir'), 'applications'))
install_data('footrc', install_dir: join_paths(get_option('datadir'), 'foot'))

subdir('doc')
