/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <gio/gio.h>

G_BEGIN_DECLS

/* Locale synchronization accepts any active local user session, including TTY.
 * Keyboard backends additionally require a graphical session and verify the
 * desktop independently. Returned type is borrowed from properties. */
gboolean rcl_session_is_active_local_user(GVariant *properties,
                                          const gchar *seat);
gboolean rcl_session_is_graphical(GVariant *properties,
                                  const gchar **type);
gboolean rcl_session_uid_is_unique_active_local(GDBusConnection *connection,
                                                 guint64 uid);

/* Return full references to the typed D-Bus reply tuples, or NULL on error. */
GVariant *rcl_session_list(GDBusConnection *connection, gint timeout_ms,
                           GError **error);
GVariant *rcl_session_get_properties(GDBusConnection *connection,
                                     const gchar *path, gint timeout_ms,
                                     GError **error);
GVariant *rcl_session_get_by_pid(GDBusConnection *connection, guint pid,
                                 gint timeout_ms, GError **error);

G_END_DECLS
