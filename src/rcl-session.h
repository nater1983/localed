/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <gio/gio.h>

G_BEGIN_DECLS

/* Locale synchronization accepts any active local user session, including TTY.
 * Keyboard backends additionally require a graphical session and verify the
 * desktop independently. Returned type is borrowed from properties. */
gboolean rcl_session_is_local_user(GVariant *properties, const gchar *seat);
gboolean rcl_session_is_active_local_user(GVariant *properties,
                                          const gchar *seat);
gboolean rcl_session_is_graphical(GVariant *properties,
                                  const gchar **type);
/* Resolve x11/wayland, including a verified Xorg server in a TTY session.
 * TRUE with *type == NULL means non-graphical; FALSE means inspection failed.
 * The returned type is borrowed or a static string. */
gboolean rcl_session_resolve_graphical(GDBusConnection *connection,
                                       GVariant *properties, guint uid,
                                       const gchar *session_path,
                                       const gchar **type, GError **error);
gboolean rcl_session_uid_is_unique_active_local(GDBusConnection *connection,
                                                 guint64 uid);

/* Authorize automatic keyboard writes only for the sole active local
 * graphical session, matching both UID and logind object path. */
gboolean rcl_session_is_unique_active_graphical(GDBusConnection *connection,
                                                 guint uid,
                                                 const gchar *session_path);

/* Return full references to the typed D-Bus reply tuples, or NULL on error. */
GVariant *rcl_session_list(GDBusConnection *connection, gint timeout_ms,
                           GError **error);
GVariant *rcl_session_get_properties(GDBusConnection *connection,
                                     const gchar *path, gint timeout_ms,
                                     GError **error);
GVariant *rcl_session_get_by_pid(GDBusConnection *connection, guint pid,
                                 gint timeout_ms, GError **error);

/* The backend supplies its desktop environment predicate. The returned
 * address is owned by the caller. Abstract sockets cannot be stat-checked. */
typedef gboolean (*RclSessionEnvironmentMatches)(const gchar *environment,
                                                  gsize length);
gchar *rcl_session_find_bus_address(GDBusConnection *connection, guint uid,
                                    const gchar *session_path,
                                    RclSessionEnvironmentMatches matches,
                                    gboolean allow_abstract);

G_END_DECLS
