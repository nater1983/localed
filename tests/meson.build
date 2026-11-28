file_mode_test = executable(
  'test-file-mode',
  'test-file-mode.c',
  dependencies        : [glib_dep, gio_dep],
  include_directories : include_directories('../src'),
)

test('configuration file mode preservation', file_mode_test)

# Give only the D-Bus test harness a private configuration root. Each
# fixture creates a unique directory here; production config.h is unchanged.
test_config = configuration_data()
test_config.set_quoted('RCL_TEST_CONFIG_ROOT', meson.current_build_dir())
configure_file(output: 'rcl-test-config.h', configuration: test_config)

dbus_lifecycle_test = executable(
  'test-dbus-lifecycle',
  [
    'test-dbus-lifecycle.c',
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


# Exercise companion launch and cleanup without using the installed binary.
desktop_companion_test = executable(
  'test-desktop-companion',
  [
    'test-desktop-companion.c',
    '../src/rcl-desktop-companion.c',
  ],
  dependencies        : [glib_dep, gio_dep],
  include_directories : include_directories('../src'),
  c_args              : ['-DRCL_DESKTOP_COMPANION_TESTING'],
)

test('optional desktop companion lifecycle', desktop_companion_test, timeout : 15)
