scdoc = dependency('scdoc', native: true)
scdoc_prog = find_program(scdoc.get_pkgconfig_variable('scdoc'), native: true)

foreach man_src : ['foot.1.scd', 'foot.5.scd']
  parts = man_src.split('.')
  name = parts[-3]
  section = parts[-2]
  out = '@0@.@1@'.format(name, section)

  custom_target(
    out,
    output: out,
    input: man_src,
    command: [sh, '-c', '@0@ < @INPUT@'.format(scdoc_prog.path())],
    capture: true,
    install: true,
    install_dir: join_paths(get_option('mandir'), 'man@0@'.format(section)))
endforeach
