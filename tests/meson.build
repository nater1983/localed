file_mode_test = executable(
  'test-file-mode',
  'test-file-mode.c',
  dependencies        : [glib_dep, gio_dep],
  include_directories : include_directories('../src', '../src/backends/gnome'),
)

test('configuration file mode preservation', file_mode_test)

dbus_lifecycle_test = executable(
  'test-dbus-lifecycle',
  [
    'test-dbus-lifecycle.c',
    '../src/rcl-accounts-service.c',
    '../src/rcl-file-transaction.c',
    '../src/rcl-xkb-config.c',
    '../src/rcl-session.c',
    '../src/rcl-session-monitor.c',
    '../src/rcl-locale.c',
  ],
  dependencies        : [glib_dep, gio_dep, gio_unix],
  include_directories : [config_h_inc, include_directories('../src')],
  c_args              : ['-DRCL_LOCALE_TESTING'],
)

test(
  'D-Bus lifecycle and resource cleanup',
  dbus_lifecycle_test,
  timeout : 30,
  env : ['G_DEBUG=fatal-warnings', 'G_SLICE=always-malloc'],
)

file_transaction_test = executable(
  'test-file-transaction',
  [
    'test-file-transaction.c',
    '../src/rcl-file-transaction.c',
  ],
  dependencies        : [glib_dep, gio_dep],
  include_directories : include_directories('../src'),
  c_args              : ['-DRCL_FILE_TRANSACTION_TESTING'],
)

test('configuration file transaction rollback', file_transaction_test)

xkb_config_test = executable(
  'test-xkb-config',
  [
    'test-xkb-config.c',
    '../src/rcl-xkb-config.c',
  ],
  dependencies        : [glib_dep, gio_dep],
  include_directories : include_directories('../src'),
)

test('Xorg XKB configuration parsing', xkb_config_test)

if have_gnome_backend
  input_sources_test = executable(
    'test-input-sources',
    [
      'backends/gnome/test-input-sources.c',
      '../src/backends/gnome/gnome-backend.c',
      '../src/backends/backend-interface.c',
      '../src/rcl-file-transaction.c',
      '../src/rcl-xkb-config.c',
      '../src/rcl-session.c',
      '../src/rcl-session-monitor.c',
      '../src/rcl-locale.c',
    ],
    dependencies        : [glib_dep, gio_dep, gio_unix],
    include_directories : [config_h_inc, include_directories('../src', '../src/backends/gnome')],
  )
  
  test(
    'GNOME input-sources backend lifecycle',
    input_sources_test,
    timeout : 30,
    env : ['G_DEBUG=fatal-warnings', 'G_SLICE=always-malloc'],
  )
  
endif

if have_plasma6_backend
  plasma6_input_sources_test = executable(
    'test-plasma6-input-sources',
    [
      'backends/plasma6/test-input-sources.c',
      '../src/backends/plasma6/plasma6-backend.c',
      '../src/backends/backend-interface.c',
      '../src/rcl-file-transaction.c',
      '../src/rcl-xkb-config.c',
      '../src/rcl-session.c',
      '../src/rcl-session-monitor.c',
      '../src/rcl-locale.c',
    ],
    dependencies        : [glib_dep, gio_dep, gio_unix],
    include_directories : [
      config_h_inc,
      include_directories('../src', '../src/backends/plasma6'),
    ],
  )

  test(
    'Plasma 6 input-sources backend lifecycle',
    plasma6_input_sources_test,
    timeout : 30,
    env : ['G_DEBUG=fatal-warnings', 'G_SLICE=always-malloc'],
  )
endif

if have_gnome_backend or have_plasma6_backend
  backend_manager_test = executable(
    'test-backend-manager',
    [
      'backends/test-backend-manager.c',
      '../src/backends/backend-interface.c',
      '../src/backends/backend-module.c',
      '../src/rcl-file-transaction.c',
      '../src/rcl-xkb-config.c',
      '../src/rcl-locale.c',
    ],
    dependencies        : [glib_dep, gio_dep, gio_unix, gmodule_dep],
    include_directories : [config_h_inc, include_directories('../src')],
  )

  test(
    'desktop backend manager arbitration and lifecycle',
    backend_manager_test,
    timeout : 30,
    env : ['G_DEBUG=fatal-warnings', 'G_SLICE=always-malloc'],
  )
endif

# ELF regression test also covers the no-backend build (no rcl_* exports).
host_export_modules = []
if have_gnome_backend
  host_export_modules += [gnome_module]
endif
if have_plasma6_backend
  host_export_modules += [plasma_module]
endif
nm_program = find_program('nm')
test(
  'desktop module host symbol visibility',
  find_program('sh'),
  args : [files('backends/test-host-exports.sh'), nm_program.full_path(),
          localed_binary] + host_export_modules,
)

session_classification_test = executable(
  'test-session',
  'test-session.c',
  dependencies : [glib_dep, gio_dep],
  include_directories : include_directories('../src'),
)
test('console graphical session classification', session_classification_test)
