/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include <gio/gio.h>
#include <glib-unix.h>
#include <signal.h>
#include <unistd.h>
#include "rcl-accounts-service.h"
#include "rcl-locale-client.h"
#include "backends/backend-interface.h"
#if HAVE_GNOME_BACKEND
#include "backends/gnome/gnome-backend.h"
#elif HAVE_PLASMA6_BACKEND
#include "backends/plasma6/plasma6-backend.h"
#endif

static gboolean stop_loop(gpointer data)
{
  g_main_loop_quit(data);
  return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
  gboolean sync = FALSE;
  gboolean debug = FALSE;
  GOptionEntry options[] = {
    { "sync-system-defaults", 0, 0, G_OPTION_ARG_NONE, &sync,
      "Explicitly enable automatic system-wide synchronization (administrator policy)", NULL },
    { "debug", 'd', 0, G_OPTION_ARG_NONE, &debug, "Enable debug logging", NULL },
    { NULL }
  };
  g_autoptr(GError) error = NULL;
  g_autoptr(GOptionContext) context = g_option_context_new("- optional desktop locale integration");
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(RclAccountsService) accounts = NULL;
  RclBackend *backend = NULL;
  guint sigint, sigterm;
  int status = 0;

  g_option_context_add_main_entries(context, options, NULL);
  if (!g_option_context_parse(context, &argc, &argv, &error) || argc != 1) {
    g_printerr("%s\n", error ? error->message : "Unexpected argument");
    return 2;
  }
  if (geteuid() != 0) {
    g_printerr("Administrator privileges required to launch helpers under the verified session UID.\n");
    return 1;
  }
  if (debug)
    g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
  rcl_locale_client_set_sync(sync);
  g_message("localed-desktop %s: %s", PACKAGE_VERSION,
            sync ? "explicit administrator synchronization enabled" : "observation only");
  loop = g_main_loop_new(NULL, FALSE);
  accounts = rcl_accounts_service_new();
  if (!rcl_accounts_service_start(accounts, &error)) {
    g_warning("AccountsService start failed; reconnect may be attempted: %s",
              error ? error->message : "system bus unavailable");
    g_clear_error(&error);
  }
#if HAVE_GNOME_BACKEND
  backend = rcl_gnome_backend_new();
#elif HAVE_PLASMA6_BACKEND
  backend = rcl_plasma6_backend_new();
#endif
  if (backend && !rcl_backend_start(backend, &error)) {
    g_warning("Desktop backend start failed: %s", error ? error->message : "unknown error");
    status = 1;
    goto out;
  }
  sigint = g_unix_signal_add(SIGINT, stop_loop, loop);
  sigterm = g_unix_signal_add(SIGTERM, stop_loop, loop);
  g_main_loop_run(loop);
  g_source_remove(sigint);
  g_source_remove(sigterm);
out:
  if (backend) {
    rcl_backend_stop(backend);
    g_object_unref(backend);
  }
  rcl_accounts_service_stop(accounts);
  return status;
}
