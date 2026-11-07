file_mode_test = executable(
  'test-file-mode',
  'test-file-mode.c',
  dependencies        : [glib_dep, gio_dep],
  include_directories : include_directories('../src'),
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
