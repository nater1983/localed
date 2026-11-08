/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 *
 * Licensed under the GNU General Public License Version 2 or later.
 */

#pragma once

#include <gio/gio.h>
#include "rcl-locale.h"

/* Helper snapshot: layouts, variants, options, active layout, active variant. */
#define RCL_GNOME_BACKEND_SNAPSHOT_TYPE "(asasasss)"

G_BEGIN_DECLS

#define RCL_TYPE_GNOME_BACKEND (rcl_gnome_backend_get_type())
G_DECLARE_FINAL_TYPE(RclGnomeBackend, rcl_gnome_backend,
                     RCL, INPUT_SOURCES, GObject)

RclGnomeBackend *rcl_gnome_backend_new(RclLocaleDaemon *daemon);
gboolean rcl_gnome_backend_start(RclGnomeBackend *self, GError **error);
void rcl_gnome_backend_stop(RclGnomeBackend *self);

/* Returns a snapshot of the one unambiguous active local GNOME session.
 * The caller owns the returned strings. Returns FALSE if there is no such
 * session or if more than one eligible session is active. */
gboolean rcl_gnome_backend_get_active_session(RclGnomeBackend *self,
                                               guint *uid,
                                               gchar **session_id,
                                               gchar **object_path,
                                               gchar **seat,
                                               gchar **session_type,
                                               gchar **desktop);

G_END_DECLS
