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

G_END_DECLS
