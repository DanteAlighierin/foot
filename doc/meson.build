scdoc_prog = find_program(scdoc.get_variable('scdoc'), native: true)

if utmp_backend != 'none'
  utmp_add_args = '@0@ $WAYLAND_DISPLAY'.format(utmp_add)
  utmp_del_args = (utmp_del_have_argument
                   ? '@0@ $WAYLAND_DISPLAY'.format(utmp_del)
                   : '@0@'.format(utmp_del))
  utmp_path = utmp_default_helper_path
else
  utmp_add_args = '<no utmp support in foot>'
  utmp_del_args = '<no utmp support in foot>'
  utmp_path = 'none'
endif


conf_data = configuration_data(
  {
    'default_terminfo': get_option('default-terminfo'),
    'utmp_backend': utmp_backend,
    'utmp_add_args': utmp_add_args,
    'utmp_del_args': utmp_del_args,
    'utmp_helper_path': utmp_path,
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
    command: scdoc_prog.full_path(),
    capture: true,
    feed: true,
    install: true,
    install_dir: join_paths(get_option('mandir'), 'man@0@'.format(section)))
endforeach
