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
#include <unistd.h>

#include <gio/gio.h>
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
   * has_name   – TRUE once name_acquired has fired at least once.
   * reconnect_id – GSource id of a pending reconnect timer; 0 if none.
   * exiting    – set TRUE by signal handlers so name_lost does not reschedule.
   */
  guint       watch_id;
  gboolean    has_name;
  guint       reconnect_id;
  gboolean    exiting;
} RclState;

/* ───────────────────────────────────────────── */

static void
rcl_state_free(RclState *state)
{
  if (state->reconnect_id)
  {
    g_source_remove(state->reconnect_id);
    state->reconnect_id = 0;
  }

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
  if ((state)->debug || (state)->verbose) \
    g_debug("[localed] " fmt, ##__VA_ARGS__)

/* ───────────────────────────────────────────── */

static void rcl_acquire_name(RclState *state);   /* forward */

/*
 * rcl_main_reconnect_cb:
 *
 * Called by a one-shot GTimeout after RECONNECT_DELAY_MS.  Drops the old
 * g_bus_own_name() watch (which is in a failed state) and calls
 * rcl_acquire_name() to start a fresh ownership attempt.
 */
static gboolean
rcl_main_reconnect_cb(gpointer user_data)
{
  RclState *state = user_data;

  state->reconnect_id = 0;   /* source is self-removing (returns FALSE) */

  LOG(state, "attempting to re-acquire D-Bus name");

  if (state->watch_id)
  {
    g_bus_unown_name(state->watch_id);
    state->watch_id = 0;
  }

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
 * Fired in two distinct situations:
 *
 *   1. Startup failure – the name could not be obtained at all (e.g. another
 *      instance is running and --replace was not given).  has_name is FALSE.
 *      We exit cleanly rather than looping forever.
 *
 *   2. Runtime loss – we held the name and lost it (dbus-daemon restart, a
 *      --replace takeover, etc.).  has_name is TRUE.  We schedule a reconnect
 *      after RECONNECT_DELAY_MS so transient dbus-daemon restarts are
 *      survived without the service going permanently dark.
 *
 * In both cases, if --replace was active and we already owned the name once,
 * losing it a second time means another instance forcibly took over; exit.
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

  /* Case 1: never acquired – another instance is running */
  if (!state->has_name)
  {
    g_warning("could not acquire %s – is another instance running?", name);
    g_main_loop_quit(state->loop);
    return;
  }

  /* Case 2: forcibly replaced by another --replace instance */
  if (state->replace)
  {
    LOG(state, "replaced by another instance, exiting");
    g_main_loop_quit(state->loop);
    return;
  }

  /* Case 3: transient loss (dbus-daemon restart etc.) – schedule reconnect */
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
    time(&t);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", localtime(&t));
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

static gboolean
rcl_main_inotify_cb(gpointer user_data)
{
  RclState *state = user_data;
  RclLocaleDaemon *object = RCL_LOCALE_DAEMON(state->daemon);

  LOG(state, "file change detected → syncing DBus properties");

  rcl_daemon_sync_dbus_properties(object);

  return G_SOURCE_CONTINUE;
}

/* ───────────────────────────────────────────── */

/*
 * rcl_acquire_name:
 *
 * Registers a g_bus_own_name() watch and stores the watch_id in state.
 * Called once from main() and again from rcl_main_reconnect_cb() on
 * transient loss.
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
  GFile        *x11_keyboard_file;
  GFileMonitor *locale_mon;
  GFileMonitor *rc_keymap_mon;
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
   * When --debug is not set, GLib will still suppress G_LOG_LEVEL_DEBUG
   * messages before they reach the handler unless G_MESSAGES_DEBUG is set.
   */
  g_log_set_handler(NULL,
                    G_LOG_LEVEL_MASK |
                    G_LOG_FLAG_FATAL |
                    G_LOG_FLAG_RECURSION,
                    rcl_main_log_handler_cb,
                    NULL);

  if (debug)
  {
    /* Make GLib pass G_LOG_LEVEL_DEBUG through to our handler */
    g_log_set_fatal_mask(NULL, G_LOG_LEVEL_ERROR);
  }

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

  /* Acquire the D-Bus name for the first time */
  rcl_acquire_name(state);

  g_debug("localed started (%s)", PACKAGE_VERSION);

  g_main_loop_run(state->loop);

  /* ── Cleanup ───────────────────────────────────────────────────────────── */

  g_clear_object(&locale_mon);
  g_clear_object(&rc_keymap_mon);
  g_clear_object(&x11_keyboard_mon);
  g_clear_object(&locale_file);
  g_clear_object(&rc_keymap_file);
  g_clear_object(&x11_keyboard_file);

  /* rcl_state_free cancels any pending reconnect timer and unowns the name */
  rcl_state_free(state);

  closelog();

  return 0;
}
