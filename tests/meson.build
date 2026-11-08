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

if get_option('desktop-backend') == 'gnome'
  input_sources_test = executable(
    'test-input-sources',
    [
      'backends/gnome/test-input-sources.c',
      '../src/backends/gnome/gnome-backend.c',
      '../src/backends/backend-interface.c',
      '../src/rcl-file-transaction.c',
      '../src/rcl-xkb-config.c',
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
