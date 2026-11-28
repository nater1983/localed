/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <gio/gio.h>

G_BEGIN_DECLS
gboolean rcl_keymap_name_is_valid(const gchar *name);
/* NULL directories selects the standard installed keymap locations. */
gboolean rcl_keymap_is_installed(const gchar *name, const gchar *const *directories);
/* Empty is an explicit reset, not a keymap name. */
gboolean rcl_keymap_validate(const gchar *name, const gchar *const *directories,
                             GError **error);
G_END_DECLS
