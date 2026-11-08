localed_sources = files(
  'main.c',
  'rcl-accounts-service.c',
  'rcl-file-transaction.c',
  'rcl-xkb-config.c',
  'rcl-locale.c',
  'backends/backend-interface.c',
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

if get_option('desktop-backend') == 'gnome'
  localed_sources += files('backends/gnome/gnome-backend.c')
endif

executable(
  'localed',
  localed_sources,
  dependencies          : localed_deps,
  include_directories   : localed_inc,
  install               : true,
  install_dir           : get_option('libexecdir'),
  gnu_symbol_visibility : 'hidden',
)

if get_option('desktop-backend') == 'gnome'
  executable(
    'localed-gnome-input-sources-helper',
    'backends/gnome/gnome-input-sources-helper.c',
    dependencies : [glib_dep, gio_dep],
    install      : true,
    install_dir  : get_option('libexecdir'),
  )
endif
