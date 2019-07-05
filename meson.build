project('f00ter', 'c',
        version: '0.9.0',
        license: 'MIT',
        default_options: [
          'c_std=c11',
          'warning_level=1',
          'werror=true',
          'b_ndebug=if-release'])

is_debug_build = get_option('buildtype').startswith('debug')

add_project_arguments(
  ['-D_GNU_SOURCE',
   #'-DF00SEL_VERSION=@0@'.format(version)] +
   ] +
  (is_debug_build ? ['-D_DEBUG'] : []),
  language: 'c',
)

cc = meson.get_compiler('c')
math = cc.find_library('m')

threads = dependency('threads')
fontconfig = dependency('fontconfig')
cairo = dependency('cairo')
cairo_ft = dependency('cairo-ft')
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
  #'external/wlr-layer-shell-unstable-v1.xml',
  wayland_protocols_datadir + '/stable/xdg-shell/xdg-shell.xml',
  #wayland_protocols_datadir + '/unstable/xdg-output/xdg-output-unstable-v1.xml']
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
  'f00ter',
  'csi.c', 'csi.h',
  'font.c', 'font.h',
  'grid.c', 'grid.h',
  'input.c', 'input.h',
  'log.c', 'log.h',
  'main.c',
  'osc.c', 'osc.h',
  'render.c', 'render.h',
  'shm.c', 'shm.h',
  'slave.c', 'slave.h',
  'terminal.c', 'terminal.h',
  'tllist.h',
  'vt.c', 'vt.h',
  wl_proto_src + wl_proto_headers,
  dependencies: [threads, math, cairo, cairo_ft, fontconfig, wayland_client, wayland_cursor, xkb],
  install: true)
