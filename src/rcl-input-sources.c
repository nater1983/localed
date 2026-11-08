/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 *
 * Licensed under the GNU General Public License Version 2 or later.
 *
 * GNOME session discovery for the per-user input-sources backend.
 *
 * This module deliberately does not read GSettings or write system
 * configuration yet. It discovers and tracks the active, local GNOME
 * graphical session through logind/elogind. The next backend stage can use
 * the selected session's UID and session metadata to establish a correctly
 * credentialed per-user settings monitor.
 */

#include "config.h"

#include <string.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib.h>

#include "rcl-input-sources.h"
#include "rcl-xkb-config.h"

#define LOGIND_NAME             "org.freedesktop.login1"
#define LOGIND_MANAGER_PATH     "/org/freedesktop/login1"
#define LOGIND_MANAGER_IFACE    "org.freedesktop.login1.Manager"
#define LOGIND_SESSION_IFACE    "org.freedesktop.login1.Session"
#define DBUS_PROPERTIES_IFACE   "org.freedesktop.DBus.Properties"
#define RECONNECT_DELAY_MS      2500
#define REFRESH_DELAY_MS        100
#define HELPER_RESTART_DELAY_MS 2500

typedef struct
{
  gchar *id;
  gchar *object_path;
  gchar *seat;
  gchar *session_type;
  gchar *desktop;
  guint uid;
} ActiveSession;

struct _RclInputSources
{
  GObject parent_instance;

  GDBusConnection *connection;
  guint name_watch_id;
  guint manager_subscription_id;
  guint properties_subscription_id;
  gulong connection_closed_id;
  guint reconnect_id;
  guint refresh_id;
  guint helper_restart_id;

  GSubprocess *settings_helper;
  GDataInputStream *settings_stream;
  GCancellable *settings_cancellable;

  gboolean stopping;
  gboolean logind_available;

  ActiveSession *active;
};

G_DEFINE_TYPE(RclInputSources, rcl_input_sources, G_TYPE_OBJECT)

enum
{
  SIGNAL_ACTIVE_SESSION_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static gboolean
uid_in_privileged_group(guint uid, gid_t *primary_gid, gchar **home)
{
  struct passwd *pw = getpwuid((uid_t) uid);
  struct group *gr = getgrnam(PRIVILEGED_GROUP);
  gid_t *groups;
  int ngroups = 16;
  gboolean found = FALSE;
  int i;

  if (!pw || !gr)
    return FALSE;

  if (primary_gid)
    *primary_gid = pw->pw_gid;
  if (home)
    *home = g_strdup(pw->pw_dir);

  groups = g_new(gid_t, ngroups);
  if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0)
  {
    groups = g_renew(gid_t, groups, ngroups);
    if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0)
    {
      g_free(groups);
      return FALSE;
    }
  }

  for (i = 0; i < ngroups; i++)
    if (groups[i] == gr->gr_gid)
    {
      found = TRUE;
      break;
    }

  g_free(groups);
  return found;
}

static void settings_helper_read_next(RclInputSources *self);

static void
settings_apply_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GError *error = NULL;
  GVariant *reply;

  reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                        &error);
  if (!reply)
  {
    g_warning("could not synchronize GNOME XKB settings with localed: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
  }
  else
    g_variant_unref(reply);

  g_object_unref(user_data);
}

static void
apply_gnome_settings(RclInputSources *self, const gchar *line)
{
  GVariant *snapshot;
  GVariant *layouts_v = NULL;
  GVariant *variants_v = NULL;
  GVariant *options_v = NULL;
  gchar **layouts = NULL;
  gchar **variants = NULL;
  gchar **options = NULL;
  gchar *layout = NULL;
  gchar *variant = NULL;
  gchar *option = NULL;
  gsize n_layouts = 0, n_variants = 0, n_options = 0;
  GError *error = NULL;

  snapshot = g_variant_parse(G_VARIANT_TYPE("(asasas)"), line, NULL, NULL,
                             &error);
  if (!snapshot)
  {
    g_warning("invalid GNOME input-sources helper output: %s",
              error ? error->message : "invalid GVariant");
    g_clear_error(&error);
    return;
  }

  g_variant_get(snapshot, "(@as@as@as)",
                &layouts_v, &variants_v, &options_v);
  layouts = g_variant_dup_strv(layouts_v, &n_layouts);
  variants = g_variant_dup_strv(variants_v, &n_variants);
  options = g_variant_dup_strv(options_v, &n_options);

  if (n_layouts == 0 || n_layouts != n_variants)
    goto out;

  layout = g_strjoinv(",", layouts);
  variant = g_strjoinv(",", variants);
  option = g_strjoinv(",", options);

  if (!rcl_xkb_value_is_valid(RCL_XKB_VALUE_LAYOUT, layout) ||
      !rcl_xkb_value_is_valid(RCL_XKB_VALUE_VARIANT, variant) ||
      !rcl_xkb_value_is_valid(RCL_XKB_VALUE_OPTIONS, option))
  {
    g_warning("ignoring invalid XKB values from GNOME input-sources");
    goto out;
  }

  g_debug("synchronizing GNOME input sources: layouts=%s variants=%s",
          layout, variant);

  /* localed calls its own public method over the system bus. The call is
   * made by the privileged daemon, not by the untrusted session process. */
  g_dbus_connection_call(self->connection,
                         "org.freedesktop.locale1",
                         "/org/freedesktop/locale1",
                         "org.freedesktop.locale1",
                         "SetX11Keyboard",
                         g_variant_new("(ssssbb)", layout, "", variant,
                                       option, FALSE, FALSE),
                         NULL,
                         G_DBUS_CALL_FLAGS_NONE,
                         -1,
                         NULL,
                         settings_apply_done,
                         g_object_ref(self));

out:
  g_free(layout);
  g_free(variant);
  g_free(option);
  g_strfreev(layouts);
  g_strfreev(variants);
  g_strfreev(options);
  g_clear_pointer(&layouts_v, g_variant_unref);
  g_clear_pointer(&variants_v, g_variant_unref);
  g_clear_pointer(&options_v, g_variant_unref);
  g_variant_unref(snapshot);
}

static void
settings_helper_read_cb(GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
  RclInputSources *self = RCL_INPUT_SOURCES(user_data);
  GDataInputStream *stream = G_DATA_INPUT_STREAM(source);
  gsize length = 0;
  GError *error = NULL;
  gchar *line = g_data_input_stream_read_line_finish(stream, result,
                                                      &length, &error);

  if (self->stopping || stream != self->settings_stream)
    goto out;

  if (line)
  {
    if (length > 0)
      apply_gnome_settings(self, line);
    settings_helper_read_next(self);
  }
  else
  {
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning("GNOME input-sources helper stopped: %s", error->message);

    if (self->settings_helper)
      g_subprocess_force_exit(self->settings_helper);

    g_clear_object(&self->settings_stream);
    g_clear_object(&self->settings_helper);
    g_clear_object(&self->settings_cancellable);

    if (!self->stopping && self->active && !self->helper_restart_id)
      self->helper_restart_id =
        g_timeout_add(HELPER_RESTART_DELAY_MS,
                      (GSourceFunc) settings_helper_read_next, self);
  }

out:
  g_free(line);
  g_clear_error(&error);
  g_object_unref(self);
}

static void
settings_helper_read_next(RclInputSources *self)
{
  if (!self->settings_stream || !self->settings_cancellable ||
      self->stopping)
    return;

  g_data_input_stream_read_line_async(self->settings_stream,
                                      G_PRIORITY_DEFAULT,
                                      self->settings_cancellable,
                                      settings_helper_read_cb,
                                      g_object_ref(self));
}

static void
stop_settings_helper(RclInputSources *self)
{
  if (self->helper_restart_id)
  {
    g_source_remove(self->helper_restart_id);
    self->helper_restart_id = 0;
  }

  if (self->settings_cancellable)
    g_cancellable_cancel(self->settings_cancellable);

  if (self->settings_helper)
    g_subprocess_force_exit(self->settings_helper);

  g_clear_object(&self->settings_stream);
  g_clear_object(&self->settings_helper);
  g_clear_object(&self->settings_cancellable);
}

static gboolean
start_settings_helper(RclInputSources *self)
{
  struct stat runtime_stat, bus_stat;
  struct passwd *pw;
  gid_t primary_gid = 0;
  gchar *home = NULL;
  gchar *uid_text = NULL;
  gchar *gid_text = NULL;
  gchar *runtime_dir = NULL;
  gchar *bus_path = NULL;
  gchar *bus_address = NULL;
  gchar *setpriv = NULL;
  const gchar *argv[12];
  guint argc = 0;
  GSubprocessLauncher *launcher = NULL;
  GError *error = NULL;
  gboolean ok = FALSE;

  if (!self->active || self->stopping || !self->connection)
    return FALSE;

  if (!uid_in_privileged_group(self->active->uid, &primary_gid, &home))
  {
    g_debug("not monitoring GNOME input sources for uid %u: account is not "
            "in privileged group '%s'", self->active->uid, PRIVILEGED_GROUP);
    g_free(home);
    return FALSE;
  }

  pw = getpwuid((uid_t) self->active->uid);
  if (!pw)
    goto out;

  uid_text = g_strdup_printf("%u", self->active->uid);
  gid_text = g_strdup_printf("%u", (guint) primary_gid);
  runtime_dir = g_strdup_printf("/run/user/%u", self->active->uid);
  bus_path = g_build_filename(runtime_dir, "bus", NULL);
  bus_address = g_strdup_printf("unix:path=%s", bus_path);

  if (g_stat(runtime_dir, &runtime_stat) != 0 ||
      !S_ISDIR(runtime_stat.st_mode) ||
      runtime_stat.st_uid != self->active->uid ||
      g_stat(bus_path, &bus_stat) != 0 ||
      !S_ISSOCK(bus_stat.st_mode) ||
      bus_stat.st_uid != self->active->uid)
  {
    g_debug("GNOME session uid %u has no owned runtime D-Bus socket",
            self->active->uid);
    goto out;
  }

  setpriv = g_find_program_in_path("setpriv");
  if (!setpriv)
  {
    g_warning("setpriv is required to run the GNOME input-sources helper "
              "as the logged-in user");
    goto out;
  }

  argv[argc++] = setpriv;
  argv[argc++] = "--reuid";
  argv[argc++] = uid_text;
  argv[argc++] = "--regid";
  argv[argc++] = gid_text;
  argv[argc++] = "--init-groups";
  argv[argc++] = "--no-new-privs";
  argv[argc++] = "--";
  argv[argc++] = RCL_INPUT_SOURCES_HELPER;
  argv[argc] = NULL;

  launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                        G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_setenv(launcher, "HOME", home, TRUE);
  g_subprocess_launcher_setenv(launcher, "USER", pw->pw_name, TRUE);
  g_subprocess_launcher_setenv(launcher, "LOGNAME", pw->pw_name, TRUE);
  g_subprocess_launcher_setenv(launcher, "XDG_RUNTIME_DIR", runtime_dir, TRUE);
  g_subprocess_launcher_setenv(launcher, "DBUS_SESSION_BUS_ADDRESS",
                               bus_address, TRUE);
  g_subprocess_launcher_unsetenv(launcher, "DBUS_SYSTEM_BUS_ADDRESS");

  self->settings_helper = g_subprocess_launcher_spawnv(launcher, argv, &error);
  if (!self->settings_helper)
  {
    g_warning("could not start GNOME input-sources helper: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
    goto out;
  }

  self->settings_stream = g_data_input_stream_new(
      g_subprocess_get_stdout_pipe(self->settings_helper));
  self->settings_cancellable = g_cancellable_new();
  settings_helper_read_next(self);

  g_debug("monitoring GNOME input sources for uid %u", self->active->uid);
  ok = TRUE;

out:
  if (launcher)
    g_object_unref(launcher);
  g_free(setpriv);
  g_free(bus_address);
  g_free(bus_path);
  g_free(runtime_dir);
  g_free(uid_text);
  g_free(gid_text);
  g_free(home);
  return ok;
}

static gboolean
helper_restart_cb(gpointer user_data)
{
  RclInputSources *self = RCL_INPUT_SOURCES(user_data);

  self->helper_restart_id = 0;
  if (!self->stopping && self->active)
    start_settings_helper(self);

  return G_SOURCE_REMOVE;
}

static void
restart_settings_helper(RclInputSources *self)
{
  stop_settings_helper(self);
  if (self->active && !self->stopping)
    start_settings_helper(self);
}

static void
active_session_free(ActiveSession *session)
{
  if (!session)
    return;

  g_free(session->id);
  g_free(session->object_path);
  g_free(session->seat);
  g_free(session->session_type);
  g_free(session->desktop);
  g_free(session);
}

static ActiveSession *
active_session_copy(const ActiveSession *session)
{
  ActiveSession *copy;

  if (!session)
    return NULL;

  copy = g_new0(ActiveSession, 1);
  copy->id = g_strdup(session->id);
  copy->object_path = g_strdup(session->object_path);
  copy->seat = g_strdup(session->seat);
  copy->session_type = g_strdup(session->session_type);
  copy->desktop = g_strdup(session->desktop);
  copy->uid = session->uid;
  return copy;
}

static gboolean
active_session_equal(const ActiveSession *a, const ActiveSession *b)
{
  if (a == b)
    return TRUE;
  if (!a || !b)
    return FALSE;

  return a->uid == b->uid &&
         g_strcmp0(a->id, b->id) == 0 &&
         g_strcmp0(a->object_path, b->object_path) == 0 &&
         g_strcmp0(a->seat, b->seat) == 0 &&
         g_strcmp0(a->session_type, b->session_type) == 0 &&
         g_strcmp0(a->desktop, b->desktop) == 0;
}

static gboolean
desktop_is_gnome(const gchar *desktop)
{
  gchar *lower;
  gboolean result;

  if (!desktop || !*desktop)
    return FALSE;

  lower = g_ascii_strdown(desktop, -1);
  result = strstr(lower, "gnome") != NULL;
  g_free(lower);

  return result;
}

static gboolean
session_properties_are_eligible(GVariant *properties,
                                const gchar *seat_from_manager,
                                ActiveSession *session)
{
  gboolean active = FALSE;
  gboolean remote = TRUE;
  const gchar *session_class = NULL;
  const gchar *session_type = NULL;
  const gchar *desktop = NULL;

  if (!g_variant_lookup(properties, "Active", "b", &active) || !active)
    return FALSE;

  if (!g_variant_lookup(properties, "Remote", "b", &remote) || remote)
    return FALSE;

  if (!g_variant_lookup(properties, "Class", "&s", &session_class) ||
      g_strcmp0(session_class, "user") != 0)
    return FALSE;

  if (!g_variant_lookup(properties, "Type", "&s", &session_type) ||
      (g_strcmp0(session_type, "x11") != 0 &&
       g_strcmp0(session_type, "wayland") != 0))
    return FALSE;

  if (!g_variant_lookup(properties, "Desktop", "&s", &desktop) ||
      !desktop_is_gnome(desktop))
    return FALSE;

  /* A seat-less session is not a local physical-seat desktop session. */
  if (!seat_from_manager || !*seat_from_manager)
    return FALSE;

  session->seat = g_strdup(seat_from_manager);
  session->session_type = g_strdup(session_type);
  session->desktop = g_strdup(desktop);
  return TRUE;
}

static ActiveSession *
query_session(RclInputSources *self,
              const gchar *id,
              guint uid,
              const gchar *seat,
              const gchar *object_path)
{
  GVariant *reply = NULL;
  GVariant *properties = NULL;
  GError *error = NULL;
  ActiveSession *session = NULL;

  reply = g_dbus_connection_call_sync(
      self->connection,
      LOGIND_NAME,
      object_path,
      DBUS_PROPERTIES_IFACE,
      "GetAll",
      g_variant_new("(s)", LOGIND_SESSION_IFACE),
      G_VARIANT_TYPE("(a{sv})"),
      G_DBUS_CALL_FLAGS_NONE,
      3000,
      NULL,
      &error);

  if (!reply)
  {
    g_debug("could not inspect logind session %s: %s",
            id, error ? error->message : "unknown error");
    g_clear_error(&error);
    return NULL;
  }

  g_variant_get(reply, "(@a{sv})", &properties);

  session = g_new0(ActiveSession, 1);
  session->id = g_strdup(id);
  session->object_path = g_strdup(object_path);
  session->uid = uid;

  if (!session_properties_are_eligible(properties, seat, session))
  {
    active_session_free(session);
    session = NULL;
  }

  g_variant_unref(properties);
  g_variant_unref(reply);
  return session;
}

static void
emit_active_session_changed(RclInputSources *self)
{
  if (self->active)
  {
    g_signal_emit(self, signals[SIGNAL_ACTIVE_SESSION_CHANGED], 0,
                  TRUE,
                  self->active->uid,
                  self->active->id,
                  self->active->object_path,
                  self->active->seat,
                  self->active->session_type,
                  self->active->desktop);
  }
  else
  {
    g_signal_emit(self, signals[SIGNAL_ACTIVE_SESSION_CHANGED], 0,
                  FALSE, 0u, "", "", "", "", "");
  }
}

static void
refresh_sessions(RclInputSources *self)
{
  GVariant *reply = NULL;
  GVariant *sessions = NULL;
  GVariantIter iter;
  const gchar *id;
  const gchar *user;
  const gchar *seat;
  const gchar *object_path;
  guint uid;
  ActiveSession *candidate = NULL;
  guint eligible_count = 0;
  GError *error = NULL;

  if (self->stopping || !self->logind_available || !self->connection)
    return;

  reply = g_dbus_connection_call_sync(
      self->connection,
      LOGIND_NAME,
      LOGIND_MANAGER_PATH,
      LOGIND_MANAGER_IFACE,
      "ListSessions",
      NULL,
      G_VARIANT_TYPE("(a(susso))"),
      G_DBUS_CALL_FLAGS_NONE,
      5000,
      NULL,
      &error);

  if (!reply)
  {
    g_warning("could not enumerate logind sessions: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
    return;
  }

  g_variant_get(reply, "(@a(susso))", &sessions);
  g_variant_iter_init(&iter, sessions);

  while (g_variant_iter_next(&iter, "(&su&s&s&o)",
                             &id, &uid, &user, &seat, &object_path))
  {
    ActiveSession *found;

    (void) user;

    found = query_session(self, id, uid, seat, object_path);
    if (!found)
      continue;

    eligible_count++;
    if (eligible_count == 1)
      candidate = found;
    else
      active_session_free(found);
  }

  if (eligible_count != 1)
  {
    if (eligible_count > 1)
      g_warning("multiple active local GNOME sessions found; refusing "
                "to choose a system-wide keyboard configuration source");
    active_session_free(candidate);
    candidate = NULL;
  }

  if (!active_session_equal(self->active, candidate))
  {
    active_session_free(self->active);
    self->active = active_session_copy(candidate);
    emit_active_session_changed(self);

    if (self->active)
      g_debug("active local GNOME session: id=%s uid=%u seat=%s type=%s "
              "desktop=%s",
              self->active->id, self->active->uid, self->active->seat,
              self->active->session_type, self->active->desktop);
    else
      g_debug("no unambiguous active local GNOME session");

    restart_settings_helper(self);
  }

  active_session_free(candidate);
  g_variant_unref(sessions);
  g_variant_unref(reply);
}

static gboolean
refresh_timeout_cb(gpointer user_data)
{
  RclInputSources *self = RCL_INPUT_SOURCES(user_data);

  self->refresh_id = 0;
  refresh_sessions(self);
  return G_SOURCE_REMOVE;
}

static void
schedule_refresh(RclInputSources *self)
{
  if (self->stopping || self->refresh_id)
    return;

  self->refresh_id = g_timeout_add(REFRESH_DELAY_MS,
                                   refresh_timeout_cb, self);
}

static void
manager_signal_cb(GDBusConnection *connection,
                  const gchar *sender_name,
                  const gchar *object_path,
                  const gchar *interface_name,
                  const gchar *signal_name,
                  GVariant *parameters,
                  gpointer user_data)
{
  RclInputSources *self = RCL_INPUT_SOURCES(user_data);

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) parameters;

  if (g_strcmp0(signal_name, "SessionNew") == 0 ||
      g_strcmp0(signal_name, "SessionRemoved") == 0)
    schedule_refresh(self);
}

static void
properties_changed_cb(GDBusConnection *connection,
                      const gchar *sender_name,
                      const gchar *object_path,
                      const gchar *interface_name,
                      const gchar *signal_name,
                      GVariant *parameters,
                      gpointer user_data)
{
  RclInputSources *self = RCL_INPUT_SOURCES(user_data);
  const gchar *changed_interface;
  GVariant *changed = NULL;
  GVariant *invalidated = NULL;

  (void) connection;
  (void) sender_name;
  (void) interface_name;
  (void) signal_name;

  if (!object_path ||
      !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sa{sv}as)")))
    return;

  g_variant_get(parameters, "(&s@a{sv}@as)",
                &changed_interface, &changed, &invalidated);

  if (g_strcmp0(changed_interface, LOGIND_SESSION_IFACE) == 0)
    schedule_refresh(self);

  g_clear_pointer(&changed, g_variant_unref);
  g_clear_pointer(&invalidated, g_variant_unref);
}

static void
unsubscribe_signals(RclInputSources *self)
{
  if (!self->connection)
    return;

  if (self->manager_subscription_id)
  {
    g_dbus_connection_signal_unsubscribe(self->connection,
                                         self->manager_subscription_id);
    self->manager_subscription_id = 0;
  }

  if (self->properties_subscription_id)
  {
    g_dbus_connection_signal_unsubscribe(self->connection,
                                         self->properties_subscription_id);
    self->properties_subscription_id = 0;
  }
}

static void
name_appeared_cb(GDBusConnection *connection,
                 const gchar *name,
                 const gchar *owner,
                 gpointer user_data)
{
  RclInputSources *self = RCL_INPUT_SOURCES(user_data);

  (void) name;
  (void) owner;

  if (self->stopping || connection != self->connection)
    return;

  self->logind_available = TRUE;

  if (!self->manager_subscription_id)
    self->manager_subscription_id =
      g_dbus_connection_signal_subscribe(
          connection, LOGIND_NAME, LOGIND_MANAGER_IFACE, NULL,
          LOGIND_MANAGER_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
          manager_signal_cb, self, NULL);

  if (!self->properties_subscription_id)
    self->properties_subscription_id =
      g_dbus_connection_signal_subscribe(
          connection, LOGIND_NAME, DBUS_PROPERTIES_IFACE,
          "PropertiesChanged", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
          properties_changed_cb, self, NULL);

  g_debug("watching logind for GNOME session changes");
  schedule_refresh(self);
}

static void
name_vanished_cb(GDBusConnection *connection,
                 const gchar *name,
                 gpointer user_data)
{
  RclInputSources *self = RCL_INPUT_SOURCES(user_data);

  (void) name;

  if (connection != self->connection)
    return;

  self->logind_available = FALSE;
  stop_settings_helper(self);
  unsubscribe_signals(self);

  if (self->refresh_id)
  {
    g_source_remove(self->refresh_id);
    self->refresh_id = 0;
  }

  if (self->active)
  {
    active_session_free(self->active);
    self->active = NULL;
    emit_active_session_changed(self);
  }

  g_debug("logind disappeared; waiting for it to return");
}

static gboolean rcl_input_sources_connect(RclInputSources *self);

static gboolean
reconnect_timeout_cb(gpointer user_data)
{
  RclInputSources *self = RCL_INPUT_SOURCES(user_data);

  self->reconnect_id = 0;
  if (!self->stopping)
    rcl_input_sources_connect(self);

  return G_SOURCE_REMOVE;
}

static void
connection_closed_cb(GDBusConnection *connection,
                     gboolean remote_peer_vanished,
                     GError *error,
                     gpointer user_data)
{
  RclInputSources *self = RCL_INPUT_SOURCES(user_data);

  (void) remote_peer_vanished;

  if (connection != self->connection || self->stopping)
    return;

  g_debug("system D-Bus connection for GNOME session discovery closed: %s",
          error ? error->message : "connection closed");

  if (self->name_watch_id)
  {
    g_bus_unwatch_name(self->name_watch_id);
    self->name_watch_id = 0;
  }

  unsubscribe_signals(self);
  self->logind_available = FALSE;
  stop_settings_helper(self);
  self->connection_closed_id = 0;
  g_clear_object(&self->connection);

  if (self->active)
  {
    active_session_free(self->active);
    self->active = NULL;
    emit_active_session_changed(self);
  }

  if (!self->reconnect_id)
    self->reconnect_id =
      g_timeout_add(RECONNECT_DELAY_MS, reconnect_timeout_cb, self);
}

static gboolean
rcl_input_sources_connect(RclInputSources *self)
{
  gchar *address;
  GError *error = NULL;

  if (self->stopping || self->connection)
    return self->connection != NULL;

  address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
  if (!address)
    goto failed;

  self->connection = g_dbus_connection_new_for_address_sync(
      address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
      G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, &error);
  g_free(address);

  if (!self->connection)
    goto failed;

  g_dbus_connection_set_exit_on_close(self->connection, FALSE);
  self->connection_closed_id =
    g_signal_connect(self->connection, "closed",
                     G_CALLBACK(connection_closed_cb), self);

  self->name_watch_id =
    g_bus_watch_name_on_connection(self->connection, LOGIND_NAME,
                                   G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   name_appeared_cb, name_vanished_cb,
                                   self, NULL);

  g_debug("watching for logind on the system bus");
  return TRUE;

failed:
  g_warning("could not connect to system D-Bus for GNOME session discovery: %s",
            error ? error->message : "unknown error");
  g_clear_error(&error);

  if (!self->stopping && !self->reconnect_id)
    self->reconnect_id =
      g_timeout_add(RECONNECT_DELAY_MS, reconnect_timeout_cb, self);

  return FALSE;
}

void
rcl_input_sources_stop(RclInputSources *self)
{
  g_return_if_fail(RCL_IS_INPUT_SOURCES(self));

  self->stopping = TRUE;
  stop_settings_helper(self);

  if (self->reconnect_id)
  {
    g_source_remove(self->reconnect_id);
    self->reconnect_id = 0;
  }

  if (self->refresh_id)
  {
    g_source_remove(self->refresh_id);
    self->refresh_id = 0;
  }

  if (self->name_watch_id)
  {
    g_bus_unwatch_name(self->name_watch_id);
    self->name_watch_id = 0;
  }

  unsubscribe_signals(self);

  if (self->connection && self->connection_closed_id)
  {
    g_signal_handler_disconnect(self->connection,
                                self->connection_closed_id);
    self->connection_closed_id = 0;
  }

  g_clear_object(&self->connection);
  self->logind_available = FALSE;
  active_session_free(self->active);
  self->active = NULL;
}

gboolean
rcl_input_sources_start(RclInputSources *self, GError **error)
{
  g_return_val_if_fail(RCL_IS_INPUT_SOURCES(self), FALSE);
  (void) error;

  if (self->connection || self->reconnect_id)
    return TRUE;

  self->stopping = FALSE;
  return rcl_input_sources_connect(self);
}

gboolean
rcl_input_sources_get_active_session(RclInputSources *self,
                                     guint *uid,
                                     gchar **session_id,
                                     gchar **object_path,
                                     gchar **seat,
                                     gchar **session_type,
                                     gchar **desktop)
{
  g_return_val_if_fail(RCL_IS_INPUT_SOURCES(self), FALSE);

  if (!self->active)
    return FALSE;

  if (uid)
    *uid = self->active->uid;
  if (session_id)
    *session_id = g_strdup(self->active->id);
  if (object_path)
    *object_path = g_strdup(self->active->object_path);
  if (seat)
    *seat = g_strdup(self->active->seat);
  if (session_type)
    *session_type = g_strdup(self->active->session_type);
  if (desktop)
    *desktop = g_strdup(self->active->desktop);

  return TRUE;
}

static void
rcl_input_sources_dispose(GObject *object)
{
  RclInputSources *self = RCL_INPUT_SOURCES(object);

  rcl_input_sources_stop(self);
  G_OBJECT_CLASS(rcl_input_sources_parent_class)->dispose(object);
}

static void
rcl_input_sources_class_init(RclInputSourcesClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = rcl_input_sources_dispose;

  signals[SIGNAL_ACTIVE_SESSION_CHANGED] =
    g_signal_new("active-session-changed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 7,
                 G_TYPE_BOOLEAN,
                 G_TYPE_UINT,
                 G_TYPE_STRING,
                 G_TYPE_STRING,
                 G_TYPE_STRING,
                 G_TYPE_STRING,
                 G_TYPE_STRING);
}

static void
rcl_input_sources_init(RclInputSources *self)
{
  (void) self;
}

RclInputSources *
rcl_input_sources_new(void)
{
  return g_object_new(RCL_TYPE_INPUT_SOURCES, NULL);
}
