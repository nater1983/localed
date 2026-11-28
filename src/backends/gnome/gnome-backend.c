/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 *
 * Licensed under the GNU General Public License Version 2 or later.
 *
 * GNOME input-source backend. It discovers the active local GNOME session,
 * launches a per-user GSettings monitor, validates its XKB snapshot, and
 * synchronizes the resulting configuration through localed's trusted
 * in-process keyboard configuration API.
 */

#include "config.h"

#include <errno.h>
#include <string.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib.h>

#include "gnome-backend.h"
#include "rcl-locale-client.h"
#include "rcl-xkb-config.h"
#include "rcl-session.h"
#include "rcl-session-monitor.h"

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

struct _RclGnomeBackend
{
  GObject parent_instance;

  RclSessionMonitor *monitor;
  guint helper_restart_id;

  GSubprocess *settings_helper;
  GDataInputStream *settings_stream;
  GCancellable *settings_cancellable;

  gboolean stopping;

  ActiveSession *active;
};

static void rcl_gnome_backend_iface_init(RclBackendInterface *iface);
G_DEFINE_TYPE_WITH_CODE(RclGnomeBackend, rcl_gnome_backend, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(RCL_TYPE_BACKEND,
                                              rcl_gnome_backend_iface_init))

enum
{
  SIGNAL_ACTIVE_SESSION_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static gboolean
lookup_session_user(guint uid, gid_t *primary_gid, gchar **home)
{
  struct passwd *pw = getpwuid((uid_t) uid);

  if (!pw)
    return FALSE;

  if (primary_gid)
    *primary_gid = pw->pw_gid;
  if (home)
    *home = g_strdup(pw->pw_dir);

  return TRUE;
}

static gboolean process_environment_is_gnome(const gchar *environment,
                                                gsize length);
static void settings_helper_read_next(RclGnomeBackend *self);
static gboolean helper_restart_cb(gpointer user_data);
static void restart_settings_helper(RclGnomeBackend *self);


typedef struct
{
  const gchar *layout;
  const gchar *variant;
  const gchar *keymap;
} GnomeConsoleKeymap;

static const GnomeConsoleKeymap gnome_console_keymaps[] =
{
  { "us", "", "us" },
  { "gb", "", "uk" },
  { "de", "", "de" },
  { "fr", "", "fr" },
  { "es", "", "es" },
  { "it", "", "it" },
  { "pt", "", "pt-latin1" },
  { "ru", "", "ru" },
  { "se", "", "se-latin1" },
  { "no", "", "no-latin1" },
  { "dk", "", "dk-latin1" },
  { "nl", "", "nl" },
  { "tr", "", "trq" },
  { "pl", "", "pl" },
  { "cz", "", "cz-lat2" },
  { "fi", "", "fi" },
  { "gr", "", "gr" },
  { "il", "", "il" },
  { "jp", "", "jp106" },
  { "ua", "", "ua" },
  { "ch", "de_nodeadkeys", "ch" },
  { "ch", "fr", "ch-fr" },
  { NULL, NULL, NULL }
};

static const gchar *
gnome_console_keymap_lookup(const gchar *layout, const gchar *variant)
{
  guint i;

  for (i = 0; gnome_console_keymaps[i].layout != NULL; i++)
    if (g_strcmp0(layout, gnome_console_keymaps[i].layout) == 0 &&
        g_strcmp0(variant ? variant : "", gnome_console_keymaps[i].variant) == 0)
      return gnome_console_keymaps[i].keymap;

  return NULL;
}

static void
apply_gnome_settings(RclGnomeBackend *self, const gchar *line)
{
  GVariant *snapshot;
  GVariant *layouts_v = NULL;
  GVariant *variants_v = NULL;
  GVariant *options_v = NULL;
  const gchar *active_layout = NULL;
  const gchar *active_variant = NULL;
  const gchar *console_keymap = NULL;
  gchar **layouts = NULL;
  gchar **variants = NULL;
  gchar **options = NULL;
  gchar *layout = NULL;
  gchar *variant = NULL;
  gchar *option = NULL;
  gchar *current_layout = NULL;
  gchar *current_model = NULL;
  gchar *current_variant = NULL;
  gchar *current_options = NULL;
  gsize n_layouts = 0, n_variants = 0;
  GError *error = NULL;

  g_debug("received GNOME input-sources snapshot: %s", line);

  snapshot = g_variant_parse(G_VARIANT_TYPE(RCL_GNOME_BACKEND_SNAPSHOT_TYPE),
                             line, NULL, NULL,
                             &error);
  if (!snapshot)
  {
    g_warning("invalid input-sources helper output: %s",
              error ? error->message : "invalid GVariant");
    g_clear_error(&error);
    return;
  }

  g_variant_get(snapshot, "(@as@as@as&s&s)",
                &layouts_v, &variants_v, &options_v,
                &active_layout, &active_variant);
  g_debug("GNOME input-sources snapshot parsed successfully: "
          "active=%s variant=%s",
          active_layout, active_variant);
  layouts = g_variant_dup_strv(layouts_v, &n_layouts);
  variants = g_variant_dup_strv(variants_v, &n_variants);
  options = g_variant_dup_strv(options_v, NULL);

  if (n_layouts == 0 || n_layouts != n_variants)
    goto out;

  layout = g_strjoinv(",", layouts);
  variant = g_strjoinv(",", variants);
  if (strspn(variant, ",") == strlen(variant))
    variant[0] = '\0';
  option = g_strjoinv(",", options);

  if (!rcl_xkb_value_is_valid(RCL_XKB_VALUE_LAYOUT, layout) ||
      !rcl_xkb_value_is_valid(RCL_XKB_VALUE_VARIANT, variant) ||
      !rcl_xkb_value_is_valid(RCL_XKB_VALUE_OPTIONS, option))
  {
    g_warning("ignoring invalid XKB values from GNOME input-sources");
    goto out;
  }

  /* GNOME does not expose an XKB model setting. Preserve the model already
   * configured by localed instead of clearing it on every source update. */
  if (!rcl_locale_client_get_x11(&current_layout, &current_model,
                                 &current_variant, &current_options, &error))
  {
    g_debug("could not read system XKB properties: %s", error->message);
    g_clear_error(&error);
    goto out;
  }

  g_debug("synchronizing GNOME input sources: layouts=%s variants=%s",
          layout, variant);

  if (!rcl_locale_client_set_x11_keyboard(
          layout, current_model ? current_model : "",
          variant, option, &error))
  {
    g_warning("could not synchronize GNOME XKB source list: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
  }


  /* The X11 source list is the complete GNOME list. The console keymap,
   * however, follows only the currently selected XKB source. Do not ask
   * localed to convert this call: that conversion would rewrite X11 settings
   * and lose the complete per-user source list. */
  console_keymap = gnome_console_keymap_lookup(active_layout, active_variant);
  if (console_keymap)
  {
    g_debug("synchronizing active GNOME input source: %s+%s -> console keymap %s",
            active_layout, active_variant, console_keymap);
    if (!rcl_locale_client_set_vconsole_keyboard(
            console_keymap, "", &error))
    {
      g_warning("could not synchronize active GNOME console keymap: %s",
                error ? error->message : "unknown error");
      g_clear_error(&error);
    }
  }
  else if (active_layout && *active_layout)
    g_debug("no console keymap mapping for active GNOME source %s+%s; "
            "leaving rc.keymap unchanged", active_layout, active_variant);

out:
  g_free(layout);
  g_free(variant);
  g_free(option);
  g_free(current_layout);
  g_free(current_model);
  g_free(current_variant);
  g_free(current_options);
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
  RclGnomeBackend *self = RCL_GNOME_BACKEND(user_data);
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
      g_warning("input-sources helper stopped: %s", error->message);

    if (self->settings_helper)
      g_subprocess_force_exit(self->settings_helper);

    g_clear_object(&self->settings_stream);
    g_clear_object(&self->settings_helper);
    g_clear_object(&self->settings_cancellable);

    if (!self->stopping && self->active && !self->helper_restart_id)
      self->helper_restart_id =
        g_timeout_add(HELPER_RESTART_DELAY_MS,
                      helper_restart_cb, self);
  }

out:
  g_free(line);
  g_clear_error(&error);
  g_object_unref(self);
}

static void
settings_helper_read_next(RclGnomeBackend *self)
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
stop_settings_helper(RclGnomeBackend *self)
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
start_settings_helper(RclGnomeBackend *self)
{
  struct stat runtime_stat;
  struct passwd *pw;
  gid_t primary_gid = 0;
  gchar *home = NULL;
  gchar *config_home = NULL;
  gchar *data_home = NULL;
  gchar *uid_text = NULL;
  gchar *gid_text = NULL;
  gchar *runtime_dir = NULL;
  gchar *bus_address = NULL;
  gchar *setpriv = NULL;
  const gchar *argv[12];
  guint argc = 0;
  GSubprocessLauncher *launcher = NULL;
  GError *error = NULL;
  gboolean ok = FALSE;

  if (!self->active || self->stopping || !rcl_session_monitor_get_connection(self->monitor))
    return FALSE;

  if (!lookup_session_user(self->active->uid, &primary_gid, &home))
  {
    g_warning("could not look up GNOME session user with uid %u",
              self->active->uid);
    return FALSE;
  }

  pw = getpwuid((uid_t) self->active->uid);
  if (!pw)
    goto out;

  uid_text = g_strdup_printf("%u", self->active->uid);
  gid_text = g_strdup_printf("%u", (guint) primary_gid);
  runtime_dir = g_strdup_printf("/run/user/%u", self->active->uid);

  if (stat(runtime_dir, &runtime_stat) != 0 ||
      !S_ISDIR(runtime_stat.st_mode) ||
      runtime_stat.st_uid != self->active->uid ||
      (runtime_stat.st_mode & 0077) != 0)
  {
    g_debug("GNOME session uid %u has no secure owned runtime directory",
            self->active->uid);
    goto out;
  }

  bus_address = rcl_session_find_bus_address(rcl_session_monitor_get_connection(self->monitor), self->active->uid,
                                             self->active->object_path,
                                             process_environment_is_gnome,
                                             FALSE);
  if (!bus_address)
  {
    g_debug("could not find a verified D-Bus socket for GNOME session %s",
            self->active->id);
    goto out;
  }

  setpriv = g_find_program_in_path("setpriv");
  if (!setpriv)
  {
    g_warning("setpriv is required to run the input-sources helper "
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

  launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  config_home = g_build_filename(home, ".config", NULL);
  data_home = g_build_filename(home, ".local", "share", NULL);
  g_subprocess_launcher_setenv(launcher, "HOME", home, TRUE);
  g_subprocess_launcher_setenv(launcher, "XDG_CONFIG_HOME", config_home, TRUE);
  g_subprocess_launcher_setenv(launcher, "XDG_DATA_HOME", data_home, TRUE);
  g_subprocess_launcher_setenv(launcher, "GSETTINGS_BACKEND", "dconf", TRUE);
  g_subprocess_launcher_unsetenv(launcher, "GSETTINGS_SCHEMA_DIR");
  g_subprocess_launcher_setenv(launcher, "USER", pw->pw_name, TRUE);
  g_subprocess_launcher_setenv(launcher, "LOGNAME", pw->pw_name, TRUE);
  g_subprocess_launcher_setenv(launcher, "XDG_RUNTIME_DIR", runtime_dir, TRUE);
  g_subprocess_launcher_setenv(launcher, "DBUS_SESSION_BUS_ADDRESS",
                               bus_address, TRUE);
  g_subprocess_launcher_unsetenv(launcher, "DBUS_SYSTEM_BUS_ADDRESS");

  self->settings_helper = g_subprocess_launcher_spawnv(launcher, argv, &error);
  if (!self->settings_helper)
  {
    g_warning("could not start input-sources helper: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
    goto out;
  }

  self->settings_stream = g_data_input_stream_new(
      g_subprocess_get_stdout_pipe(self->settings_helper));
  self->settings_cancellable = g_cancellable_new();
  settings_helper_read_next(self);

  g_debug("monitoring GNOME input sources for uid %u (%s)",
          self->active->uid, pw->pw_name);
  ok = TRUE;

out:
  if (launcher)
    g_object_unref(launcher);
  g_free(setpriv);
  g_free(config_home);
  g_free(data_home);
  g_free(bus_address);
  g_free(runtime_dir);
  g_free(uid_text);
  g_free(gid_text);
  g_free(home);
  return ok;
}

static gboolean
helper_restart_cb(gpointer user_data)
{
  RclGnomeBackend *self = RCL_GNOME_BACKEND(user_data);

  self->helper_restart_id = 0;
  if (!self->stopping && self->active)
  {
    stop_settings_helper(self);
    if (!start_settings_helper(self))
      self->helper_restart_id =
        g_timeout_add(HELPER_RESTART_DELAY_MS, helper_restart_cb, self);
  }

  return G_SOURCE_REMOVE;
}

static void
restart_settings_helper(RclGnomeBackend *self)
{
  stop_settings_helper(self);
  if (self->active && !self->stopping && !start_settings_helper(self))
    self->helper_restart_id =
      g_timeout_add(HELPER_RESTART_DELAY_MS, helper_restart_cb, self);
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
environment_value_is_gnome(const gchar *value)
{
  gchar **desktops;
  guint i;
  gboolean result = FALSE;

  if (!value || !*value)
    return FALSE;

  desktops = g_strsplit(value, ":", -1);
  for (i = 0; desktops[i]; i++)
  {
    if (desktop_is_gnome(desktops[i]))
    {
      result = TRUE;
      break;
    }
  }
  g_strfreev(desktops);
  return result;
}

static gboolean
process_environment_is_gnome(const gchar *environment, gsize length)
{
  gsize offset = 0;

  while (offset < length)
  {
    const gchar *entry = environment + offset;
    const gchar *end = memchr(entry, '\0', length - offset);
    gsize entry_length = end ? (gsize) (end - entry) : length - offset;
    const gchar *value = NULL;

    if (g_str_has_prefix(entry, "XDG_CURRENT_DESKTOP="))
      value = entry + strlen("XDG_CURRENT_DESKTOP=");
    else if (g_str_has_prefix(entry, "XDG_SESSION_DESKTOP="))
      value = entry + strlen("XDG_SESSION_DESKTOP=");
    else if (g_str_has_prefix(entry, "DESKTOP_SESSION="))
      value = entry + strlen("DESKTOP_SESSION=");

    if (value)
    {
      gchar *copy = g_strndup(value, entry_length - (gsize) (value - entry));
      gboolean is_gnome = environment_value_is_gnome(copy);
      g_free(copy);
      if (is_gnome)
        return TRUE;
    }

    offset += entry_length + (end ? 1 : 0);
  }

  return FALSE;
}

/*
 * elogind may leave Session.Desktop empty for GDM sessions. The session
 * leader is then gdm-session-worker (root), so its environment cannot identify
 * the desktop. Find a GNOME process owned by the session UID and ask logind
 * which session owns that PID. Only accept a process mapped to this exact
 * session object path.
 */
static gboolean
session_has_gnome_process(RclGnomeBackend *self,
                          guint uid,
                          const gchar *session_path)
{
  GDir *proc_dir;
  const gchar *entry;
  gboolean found = FALSE;
  GError *error = NULL;

  proc_dir = g_dir_open("/proc", 0, &error);
  if (!proc_dir)
  {
    g_debug("could not scan /proc for GNOME session processes: %s",
            error ? error->message : "unknown error");
    g_clear_error(&error);
    return FALSE;
  }

  while (!found && (entry = g_dir_read_name(proc_dir)) != NULL)
  {
    gchar *endptr = NULL;
    guint64 pid_value;
    gchar *proc_path;
    gchar *environment = NULL;
    gsize environment_length = 0;
    struct stat st;
    GVariant *reply;
    const gchar *process_session_path = NULL;

    if (!g_ascii_isdigit(entry[0]))
      continue;

    errno = 0;
    pid_value = g_ascii_strtoull(entry, &endptr, 10);
    if (errno || !endptr || *endptr || pid_value == 0 ||
        pid_value > G_MAXUINT)
      continue;

    proc_path = g_build_filename("/proc", entry, NULL);
    if (stat(proc_path, &st) != 0 || st.st_uid != (uid_t) uid)
    {
      g_free(proc_path);
      continue;
    }

    {
      gchar *environment_path = g_build_filename(proc_path, "environ", NULL);
      gboolean read_ok = g_file_get_contents(environment_path,
                                             &environment,
                                             &environment_length, NULL);
      g_free(environment_path);
      if (!read_ok)
      {
        g_free(proc_path);
        continue;
      }
    }
    g_free(proc_path);

    if (!process_environment_is_gnome(environment, environment_length))
    {
      g_free(environment);
      continue;
    }
    g_free(environment);

    reply = rcl_session_get_by_pid(rcl_session_monitor_get_connection(self->monitor), (guint) pid_value,
                                   2000, NULL);
    if (!reply)
      continue;

    g_variant_get(reply, "(&o)", &process_session_path);
    found = g_strcmp0(process_session_path, session_path) == 0;
    if (found)
      g_debug("identified GNOME session %s from process %u",
              session_path, (guint) pid_value);
    g_variant_unref(reply);
  }

  g_dir_close(proc_dir);
  return found;
}



static gboolean
session_properties_are_eligible(RclGnomeBackend *self,
                                GVariant *properties,
                                const gchar *seat_from_manager,
                                const gchar *session_path,
                                guint uid,
                                ActiveSession *session)
{
  const gchar *session_type = NULL;
  const gchar *desktop = NULL;

  if (!rcl_session_is_active_local_user(properties, seat_from_manager) ||
      !rcl_session_is_graphical(properties, &session_type))
    return FALSE;

  if (g_variant_lookup(properties, "Desktop", "&s", &desktop) &&
      desktop && *desktop)
  {
    if (!desktop_is_gnome(desktop))
      return FALSE;
  }
  else
  {
    if (!session_has_gnome_process(self, uid, session_path))
    {
      g_debug("rejecting session %s: Desktop is empty and no GNOME process "
              "could be verified in that session", session_path);
      return FALSE;
    }
    desktop = "GNOME (verified by session process)";
  }

  session->seat = g_strdup(seat_from_manager);
  session->session_type = g_strdup(session_type);
  session->desktop = g_strdup(desktop);
  return TRUE;
}

static ActiveSession *
query_session(RclGnomeBackend *self,
              const gchar *id,
              guint uid,
              const gchar *seat,
              const gchar *object_path)
{
  GVariant *reply = NULL;
  GVariant *properties = NULL;
  GError *error = NULL;
  ActiveSession *session = NULL;

  reply = rcl_session_get_properties(rcl_session_monitor_get_connection(self->monitor), object_path,
                                     3000, &error);

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

  if (!session_properties_are_eligible(self, properties, seat,
                                         object_path, uid, session))
  {
    active_session_free(session);
    session = NULL;
  }

  g_variant_unref(properties);
  g_variant_unref(reply);
  return session;
}

static void
emit_active_session_changed(RclGnomeBackend *self)
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
refresh_sessions(RclGnomeBackend *self)
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

  if (self->stopping || !rcl_session_monitor_is_available(self->monitor) || !rcl_session_monitor_get_connection(self->monitor))
    return;

  reply = rcl_session_list(rcl_session_monitor_get_connection(self->monitor), 5000, &error);

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

static void
monitor_refresh(gpointer user_data)
{
  refresh_sessions(RCL_GNOME_BACKEND(user_data));
}

static void
monitor_lost(gpointer user_data)
{
  RclGnomeBackend *self = RCL_GNOME_BACKEND(user_data);

  stop_settings_helper(self);
  if (self->active)
  {
    active_session_free(self->active);
    self->active = NULL;
    emit_active_session_changed(self);
  }
}

void
rcl_gnome_backend_stop(RclGnomeBackend *self)
{
  g_return_if_fail(RCL_IS_GNOME_BACKEND(self));
  self->stopping = TRUE;
  rcl_session_monitor_stop(self->monitor);
  stop_settings_helper(self);
  active_session_free(self->active);
  self->active = NULL;
}

gboolean
rcl_gnome_backend_start(RclGnomeBackend *self, GError **error)
{
  g_return_val_if_fail(RCL_IS_GNOME_BACKEND(self), FALSE);
  (void) error;
  self->stopping = FALSE;
  return rcl_session_monitor_start(self->monitor);
}

gboolean
rcl_gnome_backend_get_active_session(RclGnomeBackend *self,
                                     guint *uid,
                                     gchar **session_id,
                                     gchar **object_path,
                                     gchar **seat,
                                     gchar **session_type,
                                     gchar **desktop)
{
  g_return_val_if_fail(RCL_IS_GNOME_BACKEND(self), FALSE);

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
rcl_gnome_backend_dispose(GObject *object)
{
  RclGnomeBackend *self = RCL_GNOME_BACKEND(object);

  rcl_gnome_backend_stop(self);
  g_clear_pointer(&self->monitor, rcl_session_monitor_free);
  G_OBJECT_CLASS(rcl_gnome_backend_parent_class)->dispose(object);
}

static void
rcl_gnome_backend_class_init(RclGnomeBackendClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = rcl_gnome_backend_dispose;

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
rcl_gnome_backend_init(RclGnomeBackend *self)
{
  self->monitor = rcl_session_monitor_new(monitor_refresh, monitor_lost, self);
}
static gboolean
rcl_gnome_backend_iface_start(RclBackend *backend, GError **error)
{
  return rcl_gnome_backend_start(RCL_GNOME_BACKEND(backend), error);
}
static void
rcl_gnome_backend_iface_stop(RclBackend *backend)
{
  rcl_gnome_backend_stop(RCL_GNOME_BACKEND(backend));
}
static void
rcl_gnome_backend_iface_init(RclBackendInterface *iface)
{
  iface->start = rcl_gnome_backend_iface_start;
  iface->stop = rcl_gnome_backend_iface_stop;
}
RclBackend *
rcl_gnome_backend_new(void)
{
  RclGnomeBackend *self;

  self = g_object_new(RCL_TYPE_GNOME_BACKEND, NULL);
  return RCL_BACKEND(self);
}
