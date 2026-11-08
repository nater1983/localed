/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <gio/gio.h>
#include <glib.h>

#include "gnome-backend.h"

static void
test_input_sources_snapshot_format(void)
{
  const gchar *output =
      "(['us', 'es', 'jp'], ['', '', ''], @as [], 'us', '')";
  GError *error = NULL;
  GVariant *snapshot;

  snapshot = g_variant_parse(
      G_VARIANT_TYPE(RCL_GNOME_BACKEND_SNAPSHOT_TYPE),
      output, NULL, NULL, &error);

  g_assert_no_error(error);
  g_assert_nonnull(snapshot);
  g_assert_true(g_variant_is_of_type(
      snapshot, G_VARIANT_TYPE(RCL_GNOME_BACKEND_SNAPSHOT_TYPE)));

  g_variant_unref(snapshot);
}

static void
test_input_sources_start_stop(void)
{
  GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  RclBackend *backend;
  RclLocaleDaemon *daemon;
  GError *error = NULL;
  guint i;

  g_test_dbus_up(bus);
  g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(bus), TRUE);

  daemon = g_object_new(RCL_TYPE_LOCALE_DAEMON, NULL);
  g_assert_nonnull(daemon);

  /* The desktop backend must write through the in-process API without
   * requiring a locale1 D-Bus method call or Polkit authorization. */
  g_assert_true(rcl_locale_daemon_set_x11_keyboard_internal(
      daemon, "us,fr", "", "", "", &error));
  g_assert_no_error(error);
  g_assert_true(rcl_locale_daemon_set_vconsole_keyboard_internal(
      daemon, "fr", "", &error));
  g_assert_no_error(error);

  backend = rcl_gnome_backend_new(daemon);

  for (i = 0; i < 2; i++)
  {
    g_assert_true(rcl_backend_start(backend, &error));
    g_assert_no_error(error);
    rcl_backend_stop(backend);
  }

  g_object_unref(backend);
  g_object_unref(daemon);

  g_unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
  g_test_dbus_down(bus);
  g_object_unref(bus);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/input-sources/snapshot-format", test_input_sources_snapshot_format);
  g_test_add_func("/input-sources/start-stop", test_input_sources_start_stop);
  return g_test_run();
}
