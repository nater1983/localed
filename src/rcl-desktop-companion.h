/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* A missing executable is normal: return NULL without setting an error. */
GSubprocess *rcl_desktop_companion_start(const gchar *path,
                                       gboolean debug,
                                       GError **error);

/* Stop the owned child, allowing it to clean up before escalating. */
void rcl_desktop_companion_stop(GSubprocess *child);

#ifdef RCL_DESKTOP_COMPANION_TESTING
GSubprocess *rcl_desktop_companion_spawn_for_test(const gchar *path,
                                                GError **error);
#endif

G_END_DECLS
