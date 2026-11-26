/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the real classifier with an injected Xorg verification result.
 * No root privileges, real graphical session or host bus are required. */
#include "../src/rcl-session.c"

#define TEST_PATH "/org/freedesktop/login1/session/_31"

static gboolean probe_found, probe_failed;
static guint probe_calls;

static gboolean
fake_xorg_probe(GDBusConnection *connection, guint uid, const gchar *path,
                gboolean *found, GError **error)
{
  g_assert_nonnull(connection);
  g_assert_cmpuint(uid, ==, 1000);
  g_assert_cmpstr(path, ==, TEST_PATH);
  probe_calls++;
  if (probe_failed)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "simulated inspection failure");
    return FALSE;
  }
  *found = probe_found;
  return TRUE;
}

static GVariant *
session_properties(const gchar *type)
{
  GVariantBuilder props;
  g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
  if (type)
    g_variant_builder_add(&props, "{sv}", "Type", g_variant_new_string(type));
  return g_variant_ref_sink(g_variant_builder_end(&props));
}

static void
test_classification(void)
{
  /* The fake probe never dereferences the connection. Use a real GObject
   * as a non-NULL opaque sentinel rather than contacting the host bus. */
  g_autoptr(GObject) sentinel = g_object_new(G_TYPE_OBJECT, NULL);
  GDBusConnection *connection = (GDBusConnection *) sentinel;
  g_autoptr(GVariant) tty = session_properties("tty");
  g_autoptr(GVariant) x11 = session_properties("x11");
  g_autoptr(GVariant) wayland = session_properties("wayland");
  g_autoptr(GVariant) missing = session_properties(NULL);
  g_autoptr(GVariant) other = session_properties("unspecified");
  const gchar *type = NULL;
  g_autoptr(GError) error = NULL;

  probe_calls = 0;
  g_assert_true(resolve_graphical_with_probe(connection, x11, 1000,
      TEST_PATH, &type, &error, fake_xorg_probe));
  g_assert_cmpstr(type, ==, "x11");
  g_assert_true(resolve_graphical_with_probe(connection, wayland, 1000,
      TEST_PATH, &type, &error, fake_xorg_probe));
  g_assert_cmpstr(type, ==, "wayland");
  g_assert_true(resolve_graphical_with_probe(connection, other, 1000,
      TEST_PATH, &type, &error, fake_xorg_probe));
  g_assert_null(type);
  g_assert_cmpuint(probe_calls, ==, 0);

  probe_found = FALSE;
  probe_failed = FALSE;
  g_assert_true(resolve_graphical_with_probe(connection, tty, 1000,
      TEST_PATH, &type, &error, fake_xorg_probe));
  g_assert_null(type);
  probe_found = TRUE;
  g_assert_true(resolve_graphical_with_probe(connection, tty, 1000,
      TEST_PATH, &type, &error, fake_xorg_probe));
  g_assert_cmpstr(type, ==, "x11");
  /* Xorg exit in the same TTY login must remove graphical eligibility. */
  probe_found = FALSE;
  g_assert_true(resolve_graphical_with_probe(connection, tty, 1000,
      TEST_PATH, &type, &error, fake_xorg_probe));
  g_assert_null(type);
  probe_failed = TRUE;
  g_assert_false(resolve_graphical_with_probe(connection, tty, 1000,
      TEST_PATH, &type, &error, fake_xorg_probe));
  g_assert_null(type);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error(&error);
  g_assert_false(resolve_graphical_with_probe(connection, missing, 1000,
      TEST_PATH, &type, &error, fake_xorg_probe));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);
  g_assert_false(resolve_graphical_with_probe(NULL, tty, 1000,
      TEST_PATH, &type, &error, fake_xorg_probe));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint(probe_calls, ==, 4);
}

static void
test_executable_trust(void)
{
  struct stat st = { 0 };
  st.st_mode = S_IFREG | 0755;
  st.st_uid = 0;
  g_assert_true(xorg_executable_is_trusted("Xorg", &st));
  g_assert_true(xorg_executable_is_trusted("Xorg (deleted)", &st));
  g_assert_false(xorg_executable_is_trusted("Xorg-wrapper", &st));
  st.st_uid = 1000;
  g_assert_false(xorg_executable_is_trusted("Xorg", &st));
  st.st_uid = 0;
  st.st_mode |= S_IWGRP;
  g_assert_false(xorg_executable_is_trusted("Xorg", &st));
  st.st_mode = S_IFREG | 0777;
  g_assert_false(xorg_executable_is_trusted("Xorg", &st));
  st.st_mode = S_IFDIR | 0755;
  g_assert_false(xorg_executable_is_trusted("Xorg", &st));
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/session/graphical-classification", test_classification);
  g_test_add_func("/session/xorg-executable-trust", test_executable_trust);
  return g_test_run();
}
