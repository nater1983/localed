/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rcl-keymap-validation.h"
#include <glib/gstdio.h>
#include <unistd.h>
#include <string.h>

static void
test_names(void)
{
  gchar long_name[129];
  memset(long_name, 'a', 128);
  long_name[128] = 0;
  g_assert_true(rcl_keymap_name_is_valid("us"));
  g_assert_true(rcl_keymap_name_is_valid("ch-fr"));
  g_assert_false(rcl_keymap_name_is_valid(NULL));
  g_assert_false(rcl_keymap_name_is_valid(""));
  g_assert_false(rcl_keymap_name_is_valid("../us"));
  g_assert_false(rcl_keymap_name_is_valid("/tmp/us"));
  g_assert_false(rcl_keymap_name_is_valid("-C"));
  g_assert_false(rcl_keymap_name_is_valid("us;echo"));
  g_assert_false(rcl_keymap_name_is_valid(long_name));
}

static void
test_installed(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = g_dir_make_tmp("localed-keymap-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(dir);
  g_autofree gchar *nested = g_build_filename(dir, "i386", NULL);
  g_assert_cmpint(g_mkdir(nested, 0700), ==, 0);
  g_autofree gchar *plain = g_build_filename(nested, "us.map", NULL);
  g_autofree gchar *gz = g_build_filename(nested, "de.map.gz", NULL);
  g_autofree gchar *alias = g_build_filename(nested, "alias.map", NULL);
  g_autofree gchar *broken = g_build_filename(nested, "broken.map", NULL);
  g_autofree gchar *cycle = g_build_filename(nested, "cycle", NULL);
  g_autofree gchar *fake = g_build_filename(nested, "directory.map", NULL);
  const gchar *dirs[] = { "/nonexistent/localed-keymap-test", dir, NULL };

  /* File discovery only; no map is executed or loaded. */
  g_assert_true(g_file_set_contents(plain, "", 0, &error));
  g_assert_true(g_file_set_contents(gz, "", 0, &error));
  g_assert_no_error(error);
  g_assert_cmpint(symlink("us.map", alias), ==, 0);
  g_assert_cmpint(symlink("missing.map", broken), ==, 0);
  g_assert_cmpint(symlink("..", cycle), ==, 0);
  g_assert_cmpint(g_mkdir(fake, 0700), ==, 0);

  g_assert_true(rcl_keymap_is_installed("us", dirs));
  g_assert_true(rcl_keymap_is_installed("de", dirs));
  g_assert_true(rcl_keymap_is_installed("alias", dirs));
  g_assert_false(rcl_keymap_is_installed("broken", dirs));
  g_assert_false(rcl_keymap_is_installed("directory", dirs));
  g_assert_false(rcl_keymap_is_installed("missing", dirs));
  g_assert_false(rcl_keymap_is_installed("us.map", dirs));
  g_assert_true(rcl_keymap_validate("", dirs, &error));
  g_assert_true(rcl_keymap_validate("us", dirs, &error));
  g_assert_no_error(error);
  g_assert_false(rcl_keymap_validate("../us", dirs, &error));
  g_assert_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
  g_clear_error(&error);
  g_assert_false(rcl_keymap_validate("missing", dirs, &error));
  g_assert_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED);

  g_assert_cmpint(g_unlink(plain), ==, 0);
  g_assert_cmpint(g_unlink(gz), ==, 0);
  g_assert_cmpint(g_unlink(alias), ==, 0);
  g_assert_cmpint(g_unlink(broken), ==, 0);
  g_assert_cmpint(g_unlink(cycle), ==, 0);
  g_assert_cmpint(g_rmdir(fake), ==, 0);
  g_assert_cmpint(g_rmdir(nested), ==, 0);
  g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/keymap/names", test_names);
  g_test_add_func("/keymap/installed", test_installed);
  return g_test_run();
}
