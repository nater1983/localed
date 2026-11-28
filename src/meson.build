localed_sources = files(
  'main.c',
  'rcl-desktop-companion.c',
  'rcl-file-transaction.c',
  'rcl-xkb-config.c',
  'rcl-locale.c',
)

executable(
  'localed',
  localed_sources,
  dependencies          : [glib_dep, gio_dep, gio_unix],
  include_directories   : [config_h_inc, include_directories('.')],
  install               : true,
  install_dir           : get_option('libexecdir'),
  gnu_symbol_visibility : 'hidden',
)
