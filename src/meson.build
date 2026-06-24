localed_sources = files(
  'main.c',
  'rcl-locale.c',
)

localed_deps = [
  glib_dep,
  gio_dep,
  gio_unix,
]

if polkit_found
  localed_deps += polkit
endif

localed_inc = [
  config_h_inc,
  include_directories('.'),
]

executable(
  'localed',
  localed_sources,
  dependencies          : localed_deps,
  include_directories   : localed_inc,
  install               : true,
  install_dir           : get_option('libexecdir'),
  gnu_symbol_visibility : 'hidden',
  c_args                : [
    '-DGLIB_DISABLE_DEPRECATION_WARNINGS',
  ],
)
