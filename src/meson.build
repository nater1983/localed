localed_sources = files(
  'main.c',
  'rcl-accounts-service.c',
  'rcl-input-sources.c',
  'rcl-file-transaction.c',
  'rcl-xkb-config.c',
  'rcl-locale.c',
)

localed_deps = [
  glib_dep,
  gio_dep,
  gio_unix,
]

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
)

executable(
  'localed-input-sources-helper',
  'rcl-input-sources-helper.c',
  dependencies : [glib_dep, gio_dep],
  install      : true,
  install_dir  : get_option('libexecdir'),
)
