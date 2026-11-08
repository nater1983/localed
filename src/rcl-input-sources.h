/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 *
 * Licensed under the GNU General Public License Version 2 or later.
 */

#pragma once

#include <gio/gio.h>
#include "rcl-locale.h"

/* Helper snapshot: layouts, variants, options, active layout, active variant. */
#define RCL_INPUT_SOURCES_SNAPSHOT_TYPE "(asasasss)"

G_BEGIN_DECLS

#define RCL_TYPE_INPUT_SOURCES (rcl_input_sources_get_type())
G_DECLARE_FINAL_TYPE(RclInputSources, rcl_input_sources,
                     RCL, INPUT_SOURCES, GObject)

RclInputSources *rcl_input_sources_new(RclLocaleDaemon *daemon);
gboolean rcl_input_sources_start(RclInputSources *self, GError **error);
void rcl_input_sources_stop(RclInputSources *self);

/* Returns a snapshot of the one unambiguous active local GNOME session.
 * The caller owns the returned strings. Returns FALSE if there is no such
 * session or if more than one eligible session is active. */
gboolean rcl_input_sources_get_active_session(RclInputSources *self,
                                               guint *uid,
                                               gchar **session_id,
                                               gchar **object_path,
                                               gchar **seat,
                                               gchar **session_type,
                                               gchar **desktop);

G_END_DECLS
