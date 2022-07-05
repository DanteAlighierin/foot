sh = find_program('sh', native: true)

scdoc_prog = find_program(scdoc.get_variable('scdoc'), native: true)

conf_data = configuration_data(
  {
    'default_terminfo': get_option('default-terminfo'),
  }
)

foreach man_src : [{'name': 'foot', 'section' : 1},
                   {'name': 'foot.ini', 'section': 5},
                   {'name': 'footclient', 'section': 1},
                   {'name': 'foot-ctlseqs', 'section': 7}]
  name = man_src['name']
  section = man_src['section']
  out = '@0@.@1@'.format(name, section)

  preprocessed = configure_file(
    input: '@0@.@1@.scd'.format(name, section),
    output: '@0@.preprocessed'.format(out),
    configuration: conf_data,
  )

  custom_target(
    out,
    output: out,
    input: preprocessed,
    command: [sh, '-c', '@0@ < @INPUT@'.format(scdoc_prog.full_path())],
    capture: true,
    install: true,
    install_dir: join_paths(get_option('mandir'), 'man@0@'.format(section)))
endforeach
