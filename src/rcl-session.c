/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rcl-session.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOGIN_NAME "org.freedesktop.login1"
#define LOGIN_PATH "/org/freedesktop/login1"
#define LOGIN_MANAGER "org.freedesktop.login1.Manager"

GVariant *
rcl_session_list(GDBusConnection *connection, gint timeout_ms, GError **error)
{
  return g_dbus_connection_call_sync(connection, LOGIN_NAME, LOGIN_PATH,
      LOGIN_MANAGER, "ListSessions", NULL, G_VARIANT_TYPE("(a(susso))"),
      G_DBUS_CALL_FLAGS_NONE, timeout_ms, NULL, error);
}

GVariant *
rcl_session_get_properties(GDBusConnection *connection, const gchar *path,
                           gint timeout_ms, GError **error)
{
  return g_dbus_connection_call_sync(connection, LOGIN_NAME, path,
      "org.freedesktop.DBus.Properties", "GetAll",
      g_variant_new("(s)", "org.freedesktop.login1.Session"),
      G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE,
      timeout_ms, NULL, error);
}

GVariant *
rcl_session_get_by_pid(GDBusConnection *connection, guint pid,
                       gint timeout_ms, GError **error)
{
  return g_dbus_connection_call_sync(connection, LOGIN_NAME, LOGIN_PATH,
      LOGIN_MANAGER, "GetSessionByPID", g_variant_new("(u)", pid),
      G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE,
      timeout_ms, NULL, error);
}


gboolean
rcl_session_is_local_user(GVariant *properties, const gchar *seat)
{
  gboolean remote = TRUE;
  const gchar *klass = NULL;

  return seat && *seat &&
         g_variant_lookup(properties, "Remote", "b", &remote) && !remote &&
         g_variant_lookup(properties, "Class", "&s", &klass) &&
         g_strcmp0(klass, "user") == 0;
}

gboolean
rcl_session_is_active_local_user(GVariant *properties, const gchar *seat)
{
  gboolean active = FALSE;

  return rcl_session_is_local_user(properties, seat) &&
         g_variant_lookup(properties, "Active", "b", &active) && active;
}

gboolean
rcl_session_is_graphical(GVariant *properties, const gchar **type)
{
  const gchar *value = NULL;

  if (type)
    *type = NULL;
  if (!g_variant_lookup(properties, "Type", "&s", &value) ||
      (g_strcmp0(value, "x11") != 0 && g_strcmp0(value, "wayland") != 0))
    return FALSE;
  if (type)
    *type = value;
  return TRUE;
}

/* A console login may retain Type=tty even after startx launches Xorg.
 * Require a real, root-owned Xorg executable and exact logind membership;
 * neither a process name nor a user-controlled DISPLAY is sufficient.
 * Return a scan failure separately from a successful negative result. */
static gboolean
session_has_xorg(GDBusConnection *connection, guint uid,
                 const gchar *session_path, gboolean *found, GError **error)
{
  g_autoptr(GDir) proc = g_dir_open("/proc", 0, error);
  const gchar *entry;

  *found = FALSE;
  if (!proc)
    return FALSE;

  while ((entry = g_dir_read_name(proc)) != NULL)
  {
    gchar *end = NULL;
    guint64 pid;
    struct stat process_stat, executable_stat;
    g_autofree gchar *proc_path = NULL;
    g_autofree gchar *exe_path = NULL;
    g_autofree gchar *target = NULL;
    g_autofree gchar *basename = NULL;
    g_autoptr(GVariant) reply = NULL;
    g_autoptr(GError) lookup_error = NULL;
    const gchar *path = NULL;

    if (!g_ascii_isdigit(entry[0]))
      continue;
    errno = 0;
    pid = g_ascii_strtoull(entry, &end, 10);
    if (errno || !end || *end || pid == 0 || pid > G_MAXUINT)
      continue;

    proc_path = g_build_filename("/proc", entry, NULL);
    if (stat(proc_path, &process_stat) != 0 ||
        (process_stat.st_uid != (uid_t) uid && process_stat.st_uid != 0))
      continue;
    exe_path = g_build_filename(proc_path, "exe", NULL);
    target = g_file_read_link(exe_path, NULL);
    if (!target)
      continue;
    basename = g_path_get_basename(target);
    if (g_strcmp0(basename, "Xorg") != 0 ||
        stat(exe_path, &executable_stat) != 0 ||
        !S_ISREG(executable_stat.st_mode) || executable_stat.st_uid != 0 ||
        (executable_stat.st_mode & (S_IWGRP | S_IWOTH)))
      continue;

    reply = rcl_session_get_by_pid(connection, (guint) pid, 2000, &lookup_error);
    if (!reply)
    {
      /* A disappearing process is harmless; an unresolved live Xorg is not
       * evidence that the candidate is the only graphical session. */
      if (stat(proc_path, &process_stat) != 0)
        continue;
      if (lookup_error &&
          (g_error_matches(lookup_error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT) ||
           g_dbus_error_is_remote_error(lookup_error)))
      {
        g_autofree gchar *remote = g_dbus_error_get_remote_error(lookup_error);
        if (g_strcmp0(remote, "org.freedesktop.login1.NoSessionForPID") == 0)
          continue;
      }
      g_propagate_error(error, g_steal_pointer(&lookup_error));
      return FALSE;
    }
    g_variant_get(reply, "(&o)", &path);
    if (g_strcmp0(path, session_path) != 0)
      continue;
    /* Recheck executable identity after the synchronous PID lookup. */
    {
      struct stat current;
      if (stat(exe_path, &current) != 0 ||
          current.st_dev != executable_stat.st_dev ||
          current.st_ino != executable_stat.st_ino)
        continue;
    }
    *found = TRUE;
    return TRUE;
  }
  return TRUE;
}

gboolean
rcl_session_resolve_graphical(GDBusConnection *connection, GVariant *properties,
                              guint uid, const gchar *session_path,
                              const gchar **type, GError **error)
{
  const gchar *value = NULL;
  gboolean found = FALSE;

  *type = NULL;
  if (!g_variant_lookup(properties, "Type", "&s", &value))
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Missing logind session type");
    return FALSE;
  }
  if (rcl_session_is_graphical(properties, type))
    return TRUE;
  if (g_strcmp0(value, "tty") != 0)
    return TRUE;
  if (!connection || !session_path)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Console session verification requires logind");
    return FALSE;
  }
  if (!session_has_xorg(connection, uid, session_path, &found, error))
    return FALSE;
  if (found)
    *type = "x11";
  return TRUE;
}

/* Only an unambiguous active local user session may drive
 * automatic system locale synchronization. Fail closed if logind cannot
 * establish the session state. */
gboolean
rcl_session_uid_is_unique_active_local(GDBusConnection *connection, guint64 uid)
{
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariant) sessions = NULL;
  GVariantIter iter;
  const gchar *id, *user, *seat, *path;
  guint session_uid, eligible = 0;
  gboolean matches = FALSE;

  reply = rcl_session_list(connection, 2000, NULL);
  if (!reply)
    return FALSE;

  g_variant_get(reply, "(@a(susso))", &sessions);
  g_variant_iter_init(&iter, sessions);
  while (g_variant_iter_next(&iter, "(&su&s&s&o)", &id, &session_uid,
                             &user, &seat, &path)) {
    g_autoptr(GVariant) result = NULL;
    g_autoptr(GVariant) props = NULL;

    if (!*seat)
      continue;
    result = rcl_session_get_properties(connection, path, 2000, NULL);
    if (!result)
      return FALSE;
    g_variant_get(result, "(@a{sv})", &props);
    if (!rcl_session_is_active_local_user(props, seat))
      continue;
    eligible++;
    matches = session_uid == uid;
    if (eligible > 1)
      return FALSE;
  }
  return eligible == 1 && matches;
}


gboolean
rcl_session_is_unique_active_graphical(GDBusConnection *connection,
                                      guint uid, const gchar *session_path)
{
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariant) sessions = NULL;
  GVariantIter iter;
  const gchar *id, *user, *seat, *path;
  guint session_uid, eligible = 0;
  gboolean matches = FALSE;

  if (!connection || !session_path)
    return FALSE;

  reply = rcl_session_list(connection, 2000, NULL);
  if (!reply)
    return FALSE;

  g_variant_get(reply, "(@a(susso))", &sessions);
  g_variant_iter_init(&iter, sessions);
  while (g_variant_iter_next(&iter, "(&su&s&s&o)", &id, &session_uid,
                             &user, &seat, &path))
  {
    g_autoptr(GVariant) result = NULL;
    g_autoptr(GVariant) props = NULL;

    if (!*seat)
      continue;
    result = rcl_session_get_properties(connection, path, 2000, NULL);
    if (!result)
      return FALSE;
    g_variant_get(result, "(@a{sv})", &props);
    if (!rcl_session_is_active_local_user(props, seat))
      continue;
    {
      const gchar *type = NULL;
      if (!rcl_session_resolve_graphical(connection, props, session_uid, path,
                                         &type, NULL))
        return FALSE;
      if (!type)
        continue;
    }
    if (++eligible > 1)
      return FALSE;
    matches = session_uid == uid && g_strcmp0(path, session_path) == 0;
  }

  return eligible == 1 && matches;
}

gchar *
rcl_session_find_bus_address(GDBusConnection *connection, guint uid,
                             const gchar *session_path,
                             RclSessionEnvironmentMatches matches,
                             gboolean allow_abstract)
{
  GDir *proc_dir;
  const gchar *entry;
  gchar *result = NULL;
  GError *error = NULL;

  proc_dir = g_dir_open("/proc", 0, &error);
  if (!proc_dir)
  {
    g_debug("could not scan /proc for the session bus: %s",
            error ? error->message : "unknown error");
    g_clear_error(&error);
    return NULL;
  }

  while (!result && (entry = g_dir_read_name(proc_dir)) != NULL)
  {
    gchar *endptr = NULL;
    guint64 pid_value;
    gchar *proc_path;
    gchar *environment_path;
    gchar *environment = NULL;
    gsize environment_length = 0;
    struct stat st;
    GVariant *reply;
    const gchar *process_session_path = NULL;
    gsize offset = 0;
    gchar *address = NULL;

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

    environment_path = g_build_filename(proc_path, "environ", NULL);
    g_free(proc_path);
    if (!g_file_get_contents(environment_path, &environment,
                             &environment_length, NULL))
    {
      g_free(environment_path);
      continue;
    }
    g_free(environment_path);

    if (!matches(environment, environment_length))
      goto next_process;

    while (offset < environment_length)
    {
      const gchar *item = environment + offset;
      const gchar *end = memchr(item, '\0', environment_length - offset);
      gsize item_length = end ? (gsize) (end - item) :
                                environment_length - offset;

      if (g_str_has_prefix(item, "DBUS_SESSION_BUS_ADDRESS="))
      {
        address = g_strndup(item + strlen("DBUS_SESSION_BUS_ADDRESS="),
                            item_length -
                            strlen("DBUS_SESSION_BUS_ADDRESS="));
        break;
      }

      offset += item_length + (end ? 1 : 0);
    }

    /*
     * Slackware 15's dbus-run-session uses an abstract Unix socket.
     * Also accept the conventional filesystem socket used by other
     * session-bus launchers. In either case, the process UID and exact
     * logind session are verified below before accepting the address.
     */
    if (!address ||
        (!(allow_abstract && g_str_has_prefix(address, "unix:abstract=")) &&
         !g_str_has_prefix(address, "unix:path=")))
      goto next_process;

    reply = rcl_session_get_by_pid(connection, (guint) pid_value,
                                   2000, NULL);
    if (!reply)
      goto next_process;

    g_variant_get(reply, "(&o)", &process_session_path);
    if (g_strcmp0(process_session_path, session_path) == 0)
    {
      if (g_str_has_prefix(address, "unix:abstract="))
      {
        const gchar *abstract_name = address + strlen("unix:abstract=");
        const gchar *address_end = strpbrk(abstract_name, ",;");

        if (*abstract_name && (!address_end || address_end > abstract_name))
        {
          result = g_strdup(address);
          g_debug("using abstract session bus from logind-verified "
                  "process %u", (guint) pid_value);
        }
      }
      else
      {
        const gchar *path_start = address + strlen("unix:path=");
        const gchar *path_end = strpbrk(path_start, ",;");
        gchar *socket_path = path_end ?
            g_strndup(path_start, (gsize) (path_end - path_start)) :
            g_strdup(path_start);
        struct stat socket_stat;

        if (g_path_is_absolute(socket_path) &&
            stat(socket_path, &socket_stat) == 0 &&
            S_ISSOCK(socket_stat.st_mode) &&
            socket_stat.st_uid == (uid_t) uid)
        {
          result = g_strdup(address);
          g_debug("using session bus from process %u: %s",
                  (guint) pid_value, socket_path);
        }
        else
        {
          g_debug("ignoring session bus from process %u: socket is "
                  "missing, not a Unix socket, or not owned by uid %u",
                  (guint) pid_value, uid);
        }

        g_free(socket_path);
      }
    }
    g_variant_unref(reply);

next_process:
    g_free(address);
    g_free(environment);
  }

  g_dir_close(proc_dir);
  return result;
}

