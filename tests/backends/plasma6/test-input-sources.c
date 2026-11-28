/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include "config.h"

#include "plasma6-backend.h"

static void
test_plasma6_layout_names_signature(void)
{
  GVariant *reply;
  GVariant *layouts;
  GVariantIter iter;
  const gchar *short_name;
  const gchar *display_name;
  const gchar *long_name;
  guint count = 0;

  /*
   * This is the exact LayoutNames tuple returned by
   * org.kde.KeyboardLayouts.getLayoutsList(): shortName, displayName,
   * longName. The second field is not an XKB variant.
   */
  reply = g_variant_parse(G_VARIANT_TYPE("(a(sss))"),
                          "([('us', 'us', 'English (US)'), "
                          "('es', 'es', 'Spanish')],)",
                          NULL, NULL, NULL);
  g_assert_nonnull(reply);
  g_variant_get(reply, "(@a(sss))", &layouts);
  g_variant_iter_init(&iter, layouts);

  while (g_variant_iter_next(&iter, "(&s&s&s)",
                             &short_name, &display_name, &long_name))
  {
    if (count == 0)
    {
      g_assert_cmpstr(short_name, ==, "us");
      g_assert_cmpstr(display_name, ==, "us");
      g_assert_cmpstr(long_name, ==, "English (US)");
    }
    else
    {
      g_assert_cmpstr(short_name, ==, "es");
      g_assert_cmpstr(display_name, ==, "es");
      g_assert_cmpstr(long_name, ==, "Spanish");
    }
    count++;
  }

  g_assert_cmpuint(count, ==, 2);
  g_variant_unref(layouts);
  g_variant_unref(reply);
}

static void
test_plasma6_snapshot_format(void)
{
  const gchar *output =
      "(['us', 'es'], ['', ''], @as [], 'es', '')";
  GError *error = NULL;
  GVariant *snapshot;

  snapshot = g_variant_parse(
      G_VARIANT_TYPE(RCL_PLASMA6_BACKEND_SNAPSHOT_TYPE),
      output, NULL, NULL, &error);

  g_assert_no_error(error);
  g_assert_nonnull(snapshot);
  g_assert_true(g_variant_is_of_type(
      snapshot, G_VARIANT_TYPE(RCL_PLASMA6_BACKEND_SNAPSHOT_TYPE)));

  g_variant_unref(snapshot);
}

static void
test_observation_does_not_write(void)
{
  const gchar *paths[] = { RCL_X11_KEYBOARD_FILE, RCL_RC_KEYMAP_FILE,
                           RCL_RC_KEYMAP_TOGGLE_FILE };
  const gchar *contents[] = {
    "# Existing Xorg configuration\n",
    "#!/bin/bash\n/usr/bin/loadkeys \"us\"\n",
    "uk\n"
  };
  GStatBuf before[G_N_ELEMENTS(paths)];
  guint i;

  g_assert_cmpstr(paths[0], !=, "/etc/X11/xorg.conf.d/00-keyboard.conf");
  g_assert_cmpstr(paths[1], !=, "/etc/rc.d/rc.keymap");
  g_assert_cmpstr(paths[2], !=, "/etc/rc.d/rc.keymap.toggle");
  for (i = 0; i < G_N_ELEMENTS(paths); i++)
  {
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_set_contents(paths[i], contents[i], -1, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_stat(paths[i], &before[i]), ==, 0);
  }

  rcl_plasma6_backend_test_observe("(['us', 'es'], ['', ''], @as [], 'es', '')");
  rcl_plasma6_backend_test_observe("(['de'], [''], @as [], 'de', '')");

  for (i = 0; i < G_N_ELEMENTS(paths); i++)
  {
    GStatBuf after;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *actual = NULL;
    g_assert_cmpint(g_stat(paths[i], &after), ==, 0);
    g_assert_cmpuint(after.st_ino, ==, before[i].st_ino);
    g_assert_cmpint(after.st_mtim.tv_sec, ==, before[i].st_mtim.tv_sec);
    g_assert_cmpint(after.st_mtim.tv_nsec, ==, before[i].st_mtim.tv_nsec);
    g_assert_true(g_file_get_contents(paths[i], &actual, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(actual, ==, contents[i]);
    g_assert_cmpint(g_unlink(paths[i]), ==, 0);
  }

  /* Observation must not create missing system defaults either. */
  rcl_plasma6_backend_test_observe("(['us'], [''], @as [], 'us', '')");
  for (i = 0; i < G_N_ELEMENTS(paths); i++)
    g_assert_false(g_file_test(paths[i], G_FILE_TEST_EXISTS));
}

static void
test_plasma6_backend_start_stop(void)
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

  backend = rcl_plasma6_backend_new(daemon);
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
  g_test_add_func("/plasma6/layout-names-signature",
                  test_plasma6_layout_names_signature);
  g_test_add_func("/plasma6/snapshot-format",
                  test_plasma6_snapshot_format);
  g_test_add_func("/plasma6/backend-start-stop",
                  test_plasma6_backend_start_stop);
  g_test_add_func("/plasma6/observation-does-not-write",
                  test_observation_does_not_write);
  return g_test_run();
}
