localed_sources = files(
  'main.c',
  'rcl-accounts-service.c',
  'rcl-session.c',
  'rcl-session-monitor.c',
  'rcl-file-transaction.c',
  'rcl-xkb-config.c',
  'rcl-locale.c',
  'backends/backend-interface.c',
  'backends/backend-module.c',
  'backends/backend-manager.c',
)

localed_deps = [
  glib_dep,
  gio_dep,
  gio_unix,
  gmodule_dep,
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
  gnu_symbol_visibility : 'default',
  export_dynamic        : true,
)

if have_gnome_backend
  gnome_module = shared_module(
    'gnome',
    'backends/gnome/gnome-backend.c',
    'backends/gnome/gnome-module.c',
    dependencies          : localed_deps,
    include_directories   : localed_inc,
    name_prefix           : '',
    gnu_symbol_visibility : 'hidden',
    override_options      : ['b_lundef=false'],
    install               : true,
    install_dir           : backend_module_dir,
  )
endif

if have_plasma6_backend
  plasma_module = shared_module(
    'plasma',
    'backends/plasma6/plasma6-backend.c',
    'backends/plasma6/plasma6-module.c',
    dependencies          : localed_deps,
    include_directories   : localed_inc,
    name_prefix           : '',
    gnu_symbol_visibility : 'hidden',
    override_options      : ['b_lundef=false'],
    install               : true,
    install_dir           : backend_module_dir,
  )
endif

input_sources_helper_sources = files('backends/input-sources-helper.c')

if have_gnome_backend
  input_sources_helper_sources += files(
    'backends/gnome/gnome-input-sources-helper.c',
  )
endif

if have_plasma6_backend
  input_sources_helper_sources += files(
    'backends/plasma6/plasma6-input-sources-helper.c',
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
