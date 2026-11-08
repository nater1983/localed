/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <gio/gio.h>
#include <glib.h>

#include "rcl-input-sources.h"

static void
test_input_sources_start_stop(void)
{
  GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  RclInputSources *backend;
  GError *error = NULL;
  guint i;

  g_test_dbus_up(bus);
  g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(bus), TRUE);

  backend = rcl_input_sources_new();

  for (i = 0; i < 2; i++)
  {
    g_assert_true(rcl_input_sources_start(backend, &error));
    g_assert_no_error(error);
    rcl_input_sources_stop(backend);
  }

  g_object_unref(backend);

  g_unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
  g_test_dbus_down(bus);
  g_object_unref(bus);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/input-sources/start-stop", test_input_sources_start_stop);
  return g_test_run();
}
