/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "rcl-desktop-companion.h"

#include <errno.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gstdio.h>

#define COMPANION_STOP_TIMEOUT_MS 1000

/* The daemon runs as root. Never search PATH or execute an untrusted
 * executable, symlink, or a file below an untrusted directory. */
static gboolean
trusted_path(const gchar *path, GError **error)
{
  GStatBuf st;
  g_autofree gchar *directory = NULL;

  if (!g_path_is_absolute(path))
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Desktop companion path must be absolute");
    return FALSE;
  }

  if (g_lstat(path, &st) < 0)
  {
    int saved_errno = errno;
    if (saved_errno != ENOENT && saved_errno != ENOTDIR)
      g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                  "Cannot inspect desktop companion %s: %s",
                  path, g_strerror(saved_errno));
    return FALSE;
  }

  if (!S_ISREG(st.st_mode) || st.st_uid != 0 ||
      (st.st_mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID)) ||
      !(st.st_mode & S_IXUSR))
    goto untrusted;

  directory = g_path_get_dirname(path);
  for (;;)
  {
    gchar *parent;

    if (g_lstat(directory, &st) < 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)))
      goto untrusted;

    parent = g_path_get_dirname(directory);
    if (g_str_equal(parent, directory))
    {
      g_free(parent);
      break;
    }
    g_free(directory);
    directory = parent;
  }

  return TRUE;

untrusted:
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
              "Refusing untrusted desktop companion path: %s", path);
  return FALSE;
}

/* Linux parent-death notification also covers abrupt daemon termination.
 * Check the parent after arming the signal to close the fork/exit race. */
static void
child_setup(gpointer user_data)
{
  pid_t parent = (pid_t) GPOINTER_TO_INT(user_data);

  if (prctl(PR_SET_PDEATHSIG, SIGTERM, 0L, 0L, 0L) < 0 ||
      getppid() != parent)
    _exit(125);
}

static GSubprocess *
spawn_companion(const gchar *path, gboolean debug, GError **error)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  const gchar *argv[] = { path, debug ? "--debug" : NULL, NULL };

  launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_set_child_setup(
    launcher, child_setup, GINT_TO_POINTER((gint) getpid()), NULL);

  /* Intentionally omit --sync-system-defaults. Discovery is not consent
   * to automatic changes to machine-wide locale or keyboard settings. */
  return g_subprocess_launcher_spawnv(launcher, argv, error);
}

GSubprocess *
rcl_desktop_companion_start(const gchar *path, gboolean debug, GError **error)
{
  if (!trusted_path(path, error))
    return NULL;

  return spawn_companion(path, debug, error);
}

static gboolean
force_stop(gpointer user_data)
{
  g_subprocess_force_exit(G_SUBPROCESS(user_data));
  return G_SOURCE_REMOVE;
}

static void
child_stopped(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);
  g_main_loop_quit(user_data);
}

void
rcl_desktop_companion_stop(GSubprocess *child)
{
  g_autoptr(GMainContext) context = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  GSource *timeout;

  if (!child)
    return;

  /* Use a private context to avoid dispatching unrelated locale1 callbacks
   * while stopping the child. GSubprocess handles child reaping and safe
   * signalling even if the child has already exited. */
  context = g_main_context_new();
  loop = g_main_loop_new(context, FALSE);
  g_main_context_push_thread_default(context);

  timeout = g_timeout_source_new(COMPANION_STOP_TIMEOUT_MS);
  g_source_set_callback(timeout, force_stop, child, NULL);
  g_source_attach(timeout, context);

  g_subprocess_send_signal(child, SIGTERM);
  g_subprocess_wait_async(child, NULL, child_stopped, loop);
  g_main_loop_run(loop);

  g_source_destroy(timeout);
  g_source_unref(timeout);
  g_main_context_pop_thread_default(context);
}

#ifdef RCL_DESKTOP_COMPANION_TESTING
/* Tests use an isolated temporary executable, never the installed companion. */
GSubprocess *
rcl_desktop_companion_spawn_for_test(const gchar *path, GError **error)
{
  return spawn_companion(path, FALSE, error);
}
#endif
