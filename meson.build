project('localed-desktop', 'c', version: '0.1.0', license: 'GPL-2.0-or-later',
  meson_version: '>= 0.58.0', default_options: ['c_std=gnu11', 'warning_level=2'])
glib_dep = dependency('glib-2.0', version: '>= 2.56')
gio_dep = dependency('gio-2.0', version: '>= 2.56')
gio_unix = dependency('gio-unix-2.0', version: '>= 2.56')
add_project_arguments('-DGETTEXT_PACKAGE="localed-desktop"', '-include', 'glib/gi18n-lib.h', language: 'c')
conf = configuration_data()
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('LOCALEDIR', join_paths(get_option('prefix'), get_option('localedir')))
conf.set_quoted('PRIVILEGED_GROUP', get_option('privileged-group'))
backend = get_option('desktop-backend')
conf.set10('HAVE_GNOME_BACKEND', backend == 'gnome')
conf.set10('HAVE_PLASMA6_BACKEND', backend == 'plasma6')
conf.set_quoted('RCL_INPUT_SOURCES_HELPER', join_paths(get_option('prefix'), get_option('libexecdir'), 'localed-desktop-input-sources-helper'))
configure_file(output: 'config.h', configuration: conf)
inc = include_directories('.', 'src')
deps = [glib_dep, gio_dep, gio_unix]
common = files('src/rcl-locale-client.c', 'src/rcl-accounts-service.c', 'src/rcl-session.c',
  'src/rcl-session-monitor.c', 'src/rcl-xkb-config.c', 'src/backends/backend-interface.c')
if backend == 'gnome'
  backend_sources = files('src/backends/gnome/gnome-backend.c')
  helper_sources = files('src/backends/gnome/gnome-input-sources-helper.c')
  backend_test = files('tests/backends/gnome/test-input-sources.c')
  backend_inc = include_directories('src/backends/gnome')
elif backend == 'plasma6'
  backend_sources = files('src/backends/plasma6/plasma6-backend.c')
  helper_sources = files('src/backends/plasma6/plasma6-input-sources-helper.c')
  backend_test = files('tests/backends/plasma6/test-input-sources.c')
  backend_inc = include_directories('src/backends/plasma6')
else
  backend_sources = []
  helper_sources = []
endif
executable('localed-desktop', files('src/main.c') + common + backend_sources,
  include_directories: inc, dependencies: deps, install: true, install_dir: get_option('libexecdir'))
if backend != 'none'
  executable('localed-desktop-input-sources-helper', files('src/backends/input-sources-helper.c') + helper_sources,
    include_directories: inc, dependencies: deps, install: true, install_dir: get_option('libexecdir'))
  if get_option('tests')
    t = executable('test-input-sources', backend_test + common + backend_sources,
      include_directories: [inc, backend_inc], dependencies: deps)
    test('desktop backend lifecycle', t, timeout: 30)
  endif
endif
if get_option('tests')
  t = executable('test-locale-client', 'tests/test-locale-client.c', 'src/rcl-locale-client.c',
    include_directories: inc, dependencies: deps)
  test('observation mode does not write system configuration', t)
endif
