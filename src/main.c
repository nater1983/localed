/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>
#include <polkit/polkit.h>
#include <glib.h>
#include <glib-unix.h>
#include <glib-object.h>
#include <glib/gi18n-lib.h>
#include <locale.h>

#include "rcl-locale.h"

#define LOCALE_SERVICE_NAME  "org.freedesktop.locale1"

/*
 * Reconnect delay after losing the D-Bus name (milliseconds).
 * Long enough that a dbus-daemon restart has time to settle, short enough
 * that the service is not visibly absent.
 */
#define RECONNECT_DELAY_MS     2500

/* ───────────────────────────────────────────── */

typedef struct RclState
{
  RclDaemon  *daemon;
  GMainLoop  *loop;

  gboolean    debug;
  gboolean    verbose;
  gboolean    replace;

  /*
   * Ownership state.
   *
   * watch_id   – handle returned by g_bus_own_name(); 0 when not owning.
   * has_name     – TRUE once name_acquired has fired.
   * reconnect_id – pending reconnect timer; 0 if none.
   * exiting      – set TRUE by signal handlers so name_lost does not reschedule.
   * sync_id      – pending debounce timer for file-change resync; 0 if none.
   */
  guint       watch_id;
  gboolean    has_name;
  guint       reconnect_id;
  gboolean    exiting;
  guint       sync_id;
  GDBusConnection *system_bus;
  guint       accounts_subscription_id;
} RclState;

/* ───────────────────────────────────────────── */

static void
rcl_state_free(RclState *state)
{
  if (state->sync_id)
  {
    g_source_remove(state->sync_id);
    state->sync_id = 0;
  }

  if (state->reconnect_id)
  {
    g_source_remove(state->reconnect_id);
    state->reconnect_id = 0;
  }

  if (state->accounts_subscription_id && state->system_bus)
    g_dbus_connection_signal_unsubscribe(state->system_bus,
                                         state->accounts_subscription_id);
  g_clear_object(&state->system_bus);

  if (state->watch_id)
  {
    g_bus_unown_name(state->watch_id);
    state->watch_id = 0;
  }

  rcl_daemon_shutdown(state->daemon);
  g_clear_object(&state->daemon);
  g_clear_pointer(&state->loop, g_main_loop_unref);
  g_free(state);
}

static RclState *
rcl_state_new(void)
{
  RclState *state = g_new0(RclState, 1);

  state->daemon = rcl_daemon_new();
  state->loop   = g_main_loop_new(NULL, FALSE);

  return state;
}

/* ───────────────────────────────────────────── */

#define LOG(state, fmt, ...) \
  do { \
    if ((state)->debug || (state)->verbose) \
      g_message("[localed] " fmt, ##__VA_ARGS__); \
  } while (0)

/* ───────────────────────────────────────────── */

static void rcl_acquire_name(RclState *state);   /* forward */

/*
 * Retry name acquisition after a transient D-Bus disconnect. Clear the old
 * daemon registration first so startup does not retain the previous
 * connection or introspection data.
 */
static gboolean
rcl_main_reconnect_cb(gpointer user_data)
{
  RclState *state = user_data;

  state->reconnect_id = 0;

  LOG(state, "attempting to re-acquire D-Bus name");

  if (state->watch_id)
  {
    g_bus_unown_name(state->watch_id);
    state->watch_id = 0;
  }

  rcl_daemon_shutdown(state->daemon);
  state->has_name = FALSE;
  rcl_acquire_name(state);

  return G_SOURCE_REMOVE;
}

/* ───────────────────────────────────────────── */

static void
rcl_main_bus_acquired(GDBusConnection *connection,
                      const gchar     *name,
                      gpointer         user_data)
{
  RclState *state = user_data;

  LOG(state, "bus acquired");

  if (!rcl_daemon_startup(state->daemon, connection))
  {
    g_warning("Could not startup daemon");
    g_main_loop_quit(state->loop);
    return;
  }

  LOG(state, "daemon startup complete");
}

/* ───────────────────────────────────────────── */

static void
rcl_main_name_acquired(GDBusConnection *connection,
                       const gchar     *name,
                       gpointer         user_data)
{
  RclState *state = user_data;

  state->has_name = TRUE;

  LOG(state, "name acquired: %s", name);
}

/* ───────────────────────────────────────────── */

/*
 * rcl_main_name_lost:
 *
 * If the name was never acquired, another instance is likely running and
 * startup fails. If it was lost after successful startup, retry after a
 * short delay unless this instance is exiting or was explicitly replaced.
 */
static void
rcl_main_name_lost(GDBusConnection *connection,
                   const gchar     *name,
                   gpointer         user_data)
{
  RclState *state = user_data;

  g_warning("D-Bus name lost: %s", name);

  if (state->exiting)
    return;

  if (!state->has_name)
  {
    g_warning("could not acquire %s – is another instance running?", name);
    g_main_loop_quit(state->loop);
    return;
  }

  if (state->replace)
  {
    LOG(state, "replaced by another instance, exiting");
    g_main_loop_quit(state->loop);
    return;
  }

  if (!state->reconnect_id)
  {
    g_warning("scheduling reconnect in %d ms", RECONNECT_DELAY_MS);
    state->reconnect_id = g_timeout_add(RECONNECT_DELAY_MS,
                                        rcl_main_reconnect_cb,
                                        state);
  }
}

/* ───────────────────────────────────────────── */

static gboolean
rcl_main_sigint_cb(gpointer user_data)
{
  RclState *state = user_data;
  LOG(state, "SIGINT received");
  state->exiting = TRUE;
  g_main_loop_quit(state->loop);
  return G_SOURCE_REMOVE;
}

static gboolean
rcl_main_sigterm_cb(gpointer user_data)
{
  RclState *state = user_data;
  LOG(state, "SIGTERM received");
  state->exiting = TRUE;
  g_main_loop_quit(state->loop);
  return G_SOURCE_REMOVE;
}

/* ───────────────────────────────────────────── */

static void
rcl_main_log_handler_cb(const gchar    *log_domain,
                        GLogLevelFlags  log_level,
                        const gchar    *message,
                        gpointer        user_data)
{
  if (isatty(STDERR_FILENO))
  {
    /*
     * Interactive use: colourised output to stderr.
     * Errors/warnings in red, everything else in blue,
     * timestamp prefix in green.
     */
    gchar timebuf[32];
    time_t t;
    struct tm tm_buf;
    time(&t);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", localtime_r(&t, &tm_buf));
    const char *color = (log_level & (G_LOG_LEVEL_ERROR    |
                                      G_LOG_LEVEL_CRITICAL |
                                      G_LOG_LEVEL_WARNING))
                          ? "\033[31m" : "\033[34m";
    g_printerr("\033[32mTI:%s\t%s%s\033[0m\n", timebuf, color, message);
  }
  else
  {
    /*
     * Daemon context (launched by dbus-daemon, rc script, etc.):
     * route through syslog(3) using the LOG_DAEMON facility so
     * messages land in /var/log/syslog (or wherever daemon.* is
     * directed in /etc/syslog.conf).
     *
     * Note: LOG_DEBUG messages are filtered by most syslog.conf
     * defaults.  To see them add "daemon.debug /var/log/debug" to
     * /etc/syslog.conf and send syslogd a SIGHUP.
     */
    int priority;
    switch (log_level & G_LOG_LEVEL_MASK)
    {
      case G_LOG_LEVEL_ERROR:
      case G_LOG_LEVEL_CRITICAL: priority = LOG_CRIT;    break;
      case G_LOG_LEVEL_WARNING:  priority = LOG_WARNING; break;
      case G_LOG_LEVEL_MESSAGE:  priority = LOG_NOTICE;  break;
      case G_LOG_LEVEL_INFO:     priority = LOG_INFO;    break;
      case G_LOG_LEVEL_DEBUG:    priority = LOG_DEBUG;   break;
      default:                   priority = LOG_INFO;    break;
    }
    syslog(priority, "%s%s%s",
           log_domain ? log_domain : "",
           log_domain ? ": "       : "",
           message);
  }
}

/* ───────────────────────────────────────────── */

/*
 * GNOME Control Center currently writes the selected language to
 * AccountsService instead of calling locale1.SetLocale. Bridge that change
 * to localed only when the affected Unix user is already authorised by
 * Polkit for system-wide locale changes.
 */
static void
rcl_main_accounts_properties_changed(GDBusConnection *connection,
                                    const gchar *sender_name,
                                    const gchar *object_path,
                                    const gchar *interface_name,
                                    const gchar *signal_name,
                                    GVariant *parameters,
                                    gpointer user_data)
{
  RclState *state = user_data;
  const gchar *changed_interface = NULL;
  GVariant *changed = NULL;
  GVariant *invalidated = NULL;
  GVariant *language_value = NULL;
  const gchar *language = NULL;
  const gchar *uid_text;
  gchar *end = NULL;
  gchar *locale_assignment = NULL;
  guint64 uid;
  PolkitAuthority *authority = NULL;
  PolkitSubject *subject = NULL;
  PolkitAuthorizationResult *auth_result = NULL;
  GError *error = NULL;
  GVariantBuilder locale;
  GVariant *reply = NULL;

  if (g_strcmp0(sender_name, "org.freedesktop.Accounts") != 0 ||
      !g_str_has_prefix(object_path, "/org/freedesktop/Accounts/User") ||
      g_strcmp0(interface_name, "org.freedesktop.DBus.Properties") != 0 ||
      g_strcmp0(signal_name, "PropertiesChanged") != 0)
    return;

  uid_text = object_path + strlen("/org/freedesktop/Accounts/User");
  if (!uid_text[0])
    return;
  uid = g_ascii_strtoull(uid_text, &end, 10);
  if (!end || *end != '\0' || uid > G_MAXINT)
    return;

  g_variant_get(parameters, "(&s@a{sv}@as)",
                &changed_interface, &changed, &invalidated);
  if (g_strcmp0(changed_interface, "org.freedesktop.Accounts.User") != 0)
    goto out;

  language_value = g_variant_lookup_value(changed, "Language",
                                           G_VARIANT_TYPE_STRING);
  if (!language_value)
    goto out;

  language = g_variant_get_string(language_value, NULL);
  if (!language[0] || !g_utf8_validate(language, -1, NULL))
    goto out;

  authority = polkit_authority_get_sync(NULL, &error);
  if (!authority)
  {
    g_warning("AccountsService locale bridge: cannot connect to Polkit: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
    goto out;
  }

  subject = polkit_unix_user_new((gint)uid);
  auth_result = polkit_authority_check_authorization_sync(
      authority, subject, RCL_LOCALE_POLKIT_SET_LOCALE, NULL,
      POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE, NULL, &error);
  if (!auth_result)
  {
    g_warning("AccountsService locale bridge: Polkit check failed: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
    goto out;
  }

  if (!polkit_authorization_result_get_is_authorized(auth_result))
  {
    LOG(state, "ignoring AccountsService language change for uid %" G_GUINT64_FORMAT
        ": user is not authorised for system locale changes", uid);
    goto out;
  }

  locale_assignment = g_strdup_printf("LANG=%s", language);
  g_variant_builder_init(&locale, G_VARIANT_TYPE("as"));
  g_variant_builder_add(&locale, "s", locale_assignment);
  reply = g_dbus_connection_call_sync(
      state->system_bus, LOCALE_SERVICE_NAME, "/org/freedesktop/locale1",
      "org.freedesktop.locale1", "SetLocale",
      g_variant_new("(@asb)", g_variant_builder_end(&locale), FALSE),
      NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
  if (!reply)
  {
    g_warning("AccountsService locale bridge: SetLocale failed: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
  }
  else
  {
    LOG(state, "applied AccountsService language '%s' as system locale", language);
    g_variant_unref(reply);
  }

out:
  g_free(locale_assignment);
  g_clear_pointer(&language_value, g_variant_unref);
  g_clear_pointer(&changed, g_variant_unref);
  g_clear_pointer(&invalidated, g_variant_unref);
  g_clear_object(&auth_result);
  g_clear_object(&subject);
  g_clear_object(&authority);
}

/* ───────────────────────────────────────────── */

#define SYNC_DEBOUNCE_MS 100

static gboolean
rcl_main_sync_timeout_cb(gpointer user_data)
{
  RclState *state = user_data;

  state->sync_id = 0;
  LOG(state, "file change detected → syncing DBus properties");
  rcl_daemon_sync_dbus_properties(RCL_LOCALE_DAEMON(state->daemon));
  return G_SOURCE_REMOVE;
}

/* Coalesce bursts of file events (and our own writes) into one resync. */
static void
rcl_main_inotify_cb(gpointer user_data)
{
  RclState *state = user_data;

  if (!state->sync_id)
    state->sync_id = g_timeout_add(SYNC_DEBOUNCE_MS,
                                   rcl_main_sync_timeout_cb, state);
}

/* ───────────────────────────────────────────── */

/*
 * rcl_acquire_name:
 *
 * Registers a g_bus_own_name() watch and stores the watch_id in state.
 * Called initially from main() and again after a transient D-Bus loss.
 *
 * G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT is always set so that a
 * subsequent invocation with --replace can take over cleanly.
 */
static void
rcl_acquire_name(RclState *state)
{
  GBusNameOwnerFlags flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;

  if (state->replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  state->watch_id = g_bus_own_name(G_BUS_TYPE_SYSTEM,
                                   LOCALE_SERVICE_NAME,
                                   flags,
                                   rcl_main_bus_acquired,
                                   rcl_main_name_acquired,
                                   rcl_main_name_lost,
                                   state,
                                   NULL);
}

/* ───────────────────────────────────────────── */

gint main(gint argc, gchar **argv)
{
  GError         *error = NULL;
  GOptionContext *context;

  gboolean debug   = FALSE;
  gboolean verbose = FALSE;
  gboolean replace = FALSE;

  RclState *state;

  GFile        *locale_file;
  GFile        *rc_keymap_file;
  GFile        *toggle_file;
  GFile        *x11_keyboard_file;
  GFileMonitor *locale_mon;
  GFileMonitor *rc_keymap_mon;
  GFileMonitor *toggle_mon;
  GFileMonitor *x11_keyboard_mon;

  const GOptionEntry options[] =
  {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace, "Replace existing daemon",        NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Verbose output",                 NULL },
    { "debug",   'd', 0, G_OPTION_ARG_NONE, &debug,   "Debug output (implies verbose)", NULL },
    { NULL }
  };

  setlocale(LC_ALL, "");

  context = g_option_context_new("");
  g_option_context_add_main_entries(context, options, NULL);

  if (!g_option_context_parse(context, &argc, &argv, &error))
  {
    g_warning("arg parse failed: %s", error->message);
    g_error_free(error);
    g_option_context_free(context);
    return 1;
  }

  g_option_context_free(context);

  if (debug)
    verbose = TRUE;

  state          = rcl_state_new();
  state->debug   = debug;
  state->verbose = verbose;
  state->replace = replace;

  rcl_daemon_set_debug(state->daemon, debug);

  /*
   * Open the syslog connection immediately so any subsequent g_warning() /
   * g_debug() calls that reach rcl_main_log_handler_cb in daemon context
   * have a valid syslog handle.  LOG_PID stamps every line with our PID;
   * LOG_NDELAY opens the socket now rather than on the first message.
   * LOG_DAEMON is the conventional facility for system daemons.
   */
  openlog("localed", LOG_PID | LOG_NDELAY, LOG_DAEMON);

  /*
   * Install the log handler unconditionally for all domains and levels,
   * including fatal flags.  The handler itself decides whether to write
   * to syslog or to the terminal based on isatty(STDERR_FILENO).
   * G_LOG_LEVEL_DEBUG messages are passed to syslog, as in hostnamed.
   */
  g_log_set_handler(NULL,
                    G_LOG_LEVEL_MASK |
                    G_LOG_FLAG_FATAL |
                    G_LOG_FLAG_RECURSION,
                    rcl_main_log_handler_cb,
                    NULL);

  /* signals */
  g_unix_signal_add(SIGINT,  rcl_main_sigint_cb,  state);
  g_unix_signal_add(SIGTERM, rcl_main_sigterm_cb, state);

  /*
   * File monitoring.
   *
   * RCL_LANG_SH_FILE / RCL_RC_KEYMAP_FILE / RCL_X11_KEYBOARD_FILE come from
   * config.h (meson options locale_conf / rc_keymap / x11_keyboard_conf).
   * G_FILE_MONITOR_WATCH_MOVES catches both direct writes and atomic
   * rename-based writes (which is how localed itself, and the bundled
   * localectl client, persist changes).
   */
  locale_file = g_file_new_for_path(RCL_LANG_SH_FILE);
  locale_mon  = g_file_monitor_file(locale_file,
                                    G_FILE_MONITOR_WATCH_MOVES,
                                    NULL, &error);
  if (locale_mon)
    g_signal_connect_swapped(locale_mon, "changed",
                             G_CALLBACK(rcl_main_inotify_cb), state);
  else
  {
    g_warning("could not watch %s: %s", RCL_LANG_SH_FILE, error->message);
    g_clear_error(&error);
  }

  rc_keymap_file = g_file_new_for_path(RCL_RC_KEYMAP_FILE);
  rc_keymap_mon  = g_file_monitor_file(rc_keymap_file,
                                       G_FILE_MONITOR_WATCH_MOVES,
                                       NULL, &error);
  if (rc_keymap_mon)
    g_signal_connect_swapped(rc_keymap_mon, "changed",
                             G_CALLBACK(rcl_main_inotify_cb), state);
  else
  {
    g_warning("could not watch %s: %s", RCL_RC_KEYMAP_FILE, error->message);
    g_clear_error(&error);
  }

  toggle_file = g_file_new_for_path(RCL_RC_KEYMAP_TOGGLE_FILE);
  toggle_mon  = g_file_monitor_file(toggle_file,
                                    G_FILE_MONITOR_WATCH_MOVES,
                                    NULL, &error);
  if (toggle_mon)
    g_signal_connect_swapped(toggle_mon, "changed",
                             G_CALLBACK(rcl_main_inotify_cb), state);
  else
  {
    g_warning("could not watch %s: %s", RCL_RC_KEYMAP_TOGGLE_FILE, error->message);
    g_clear_error(&error);
  }

  x11_keyboard_file = g_file_new_for_path(RCL_X11_KEYBOARD_FILE);
  x11_keyboard_mon  = g_file_monitor_file(x11_keyboard_file,
                                          G_FILE_MONITOR_WATCH_MOVES,
                                          NULL, &error);
  if (x11_keyboard_mon)
    g_signal_connect_swapped(x11_keyboard_mon, "changed",
                             G_CALLBACK(rcl_main_inotify_cb), state);
  else
  {
    g_warning("could not watch %s: %s", RCL_X11_KEYBOARD_FILE, error->message);
    g_clear_error(&error);
  }

  /*
   * Watch AccountsService language updates. The affected UID is checked
   * against the locale1 Polkit action before a system-wide change is made.
   */
  state->system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
  if (state->system_bus)
  {
    state->accounts_subscription_id =
      g_dbus_connection_signal_subscribe(
        state->system_bus, "org.freedesktop.Accounts",
        "org.freedesktop.DBus.Properties", "PropertiesChanged", NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, rcl_main_accounts_properties_changed,
        state, NULL);
  }
  else
  {
    g_warning("could not subscribe to AccountsService language changes: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
  }

  /* Acquire the D-Bus name for the first time */
  rcl_acquire_name(state);

  g_debug("localed started (%s)", PACKAGE_VERSION);

  g_main_loop_run(state->loop);

  /* ── Cleanup ───────────────────────────────────────────────────────────── */

  g_clear_object(&locale_mon);
  g_clear_object(&rc_keymap_mon);
  g_clear_object(&toggle_mon);
  g_clear_object(&x11_keyboard_mon);
  g_clear_object(&locale_file);
  g_clear_object(&rc_keymap_file);
  g_clear_object(&toggle_file);
  g_clear_object(&x11_keyboard_file);

  /* rcl_state_free cancels any pending resync timer and unowns the name */
  rcl_state_free(state);

  closelog();

  return 0;
}
