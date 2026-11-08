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

input_sources_helper_sources = files('backends/input-sources-helper.c')

if get_option('desktop-backend') == 'gnome'
  input_sources_helper_sources += files(
    'backends/gnome/gnome-input-sources-helper.c',
  )
endif

executable(
  'localed-input-sources-helper',
  input_sources_helper_sources,
  dependencies      : [glib_dep, gio_dep],
  include_directories : [config_h_inc, include_directories('backends')],
  install           : true,
  install_dir       : get_option('libexecdir'),
)
