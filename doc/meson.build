sh = find_program('sh', native: true)

scdoc = dependency('scdoc', native: true)
scdoc_prog = find_program(scdoc.get_pkgconfig_variable('scdoc'), native: true)

foreach man_src : [{'name': 'foot', 'section' : 1},
                   {'name': 'foot.ini', 'section': 5},
                   {'name': 'footclient', 'section': 1},
                   {'name': 'foot-ctlseqs', 'section': 5}]
  name = man_src['name']
  section = man_src['section']
  out = '@0@.@1@'.format(name, section)

  custom_target(
    out,
    output: out,
    input: '@0@.@1@.scd'.format(name, section),
    command: [sh, '-c', '@0@ < @INPUT@'.format(scdoc_prog.path())],
    capture: true,
    install: true,
    install_dir: join_paths(get_option('mandir'), 'man@0@'.format(section)))
endforeach
