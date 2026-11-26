/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rcl-session.h"

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
rcl_session_is_active_local_user(GVariant *properties, const gchar *seat)
{
  gboolean active = FALSE, remote = TRUE;
  const gchar *klass = NULL;

  return seat && *seat &&
         g_variant_lookup(properties, "Active", "b", &active) && active &&
         g_variant_lookup(properties, "Remote", "b", &remote) && !remote &&
         g_variant_lookup(properties, "Class", "&s", &klass) &&
         g_strcmp0(klass, "user") == 0;
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

