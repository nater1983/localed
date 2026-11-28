/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "rcl-desktop-companion.h"

#include <glib/gstdio.h>
#include <signal.h>

static gchar *
make_program(const gchar *directory, const gchar *script)
{
  g_autoptr(GError) error = NULL;
  gchar *path = g_build_filename(directory, "localed-input-sources-helper", NULL);

  g_assert_true(g_file_set_contents(path, script, -1, &error));
  g_assert_no_error(error);
  g_assert_cmpint(g_chmod(path, 0700), ==, 0);
  return path;
}

static void
test_missing(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *directory = g_dir_make_tmp("localed-companion-XXXXXX", &error);
  g_autofree gchar *path = NULL;

  g_assert_no_error(error);
  g_assert_nonnull(directory);
  path = g_build_filename(directory, "missing", NULL);

  g_assert_null(rcl_desktop_companion_start(path, FALSE, &error));
  g_assert_no_error(error);
  rcl_desktop_companion_stop(NULL);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void
test_untrusted(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *directory = g_dir_make_tmp("localed-companion-XXXXXX", &error);
  g_autofree gchar *path = NULL;

  g_assert_no_error(error);
  g_assert_nonnull(directory);
  path = make_program(directory, "#!/bin/sh\nexit 0\n");
  /* An executable writable by other users must never be started as root. */
  g_assert_cmpint(g_chmod(path, 0777), ==, 0);
  g_assert_null(rcl_desktop_companion_start(path, FALSE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);

  g_assert_cmpint(g_unlink(path), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void
test_child_exit(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *directory = g_dir_make_tmp("localed-companion-XXXXXX", &error);
  g_autofree gchar *path = NULL;
  g_autoptr(GSubprocess) child = NULL;

  g_assert_no_error(error);
  g_assert_nonnull(directory);
  path = make_program(directory,
    "#!/bin/sh\n[ \"$#\" -eq 1 ] && [ \"$1\" = --managed-companion ] || exit 99\nexit 7\n");

  child = rcl_desktop_companion_spawn_for_test(path, &error);
  g_assert_no_error(error);
  g_assert_nonnull(child);
  g_assert_true(g_subprocess_wait(child, NULL, &error));
  g_assert_no_error(error);
  g_assert_cmpint(g_subprocess_get_exit_status(child), ==, 7);
  rcl_desktop_companion_stop(child);

  g_assert_cmpint(g_unlink(path), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void
test_child_stop(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *directory = g_dir_make_tmp("localed-companion-XXXXXX", &error);
  g_autofree gchar *path = NULL;
  g_autofree gchar *ready = NULL;
  g_autoptr(GSubprocess) child = NULL;
  guint i;

  g_assert_no_error(error);
  g_assert_nonnull(directory);
  /* The shell ignores TERM to exercise the forced-stop path. It has no
   * descendants and creates a readiness marker before entering its loop. */
  path = make_program(directory,
    "#!/bin/sh\ntrap '' TERM\n: > \"$0.ready\"\nwhile :; do :; done\n");
  ready = g_strconcat(path, ".ready", NULL);
  child = rcl_desktop_companion_spawn_for_test(path, &error);
  g_assert_no_error(error);
  g_assert_nonnull(child);

  for (i = 0; i < 200 && !g_file_test(ready, G_FILE_TEST_EXISTS); i++)
    g_usleep(10000);
  g_assert_true(g_file_test(ready, G_FILE_TEST_EXISTS));

  rcl_desktop_companion_stop(child);
  g_assert_true(g_subprocess_get_if_signaled(child));
  g_assert_cmpint(g_subprocess_get_term_sig(child), ==, SIGKILL);

  g_assert_cmpint(g_unlink(ready), ==, 0);
  g_assert_cmpint(g_unlink(path), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/desktop-companion/missing", test_missing);
  g_test_add_func("/desktop-companion/untrusted", test_untrusted);
  g_test_add_func("/desktop-companion/child-exit", test_child_exit);
  g_test_add_func("/desktop-companion/child-stop", test_child_stop);

  return g_test_run();
}
