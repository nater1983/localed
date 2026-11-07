/*
 * Copyright (C) 2026 Nathaniel Russell
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/*
 * Parse the localed-owned InputClass section from an Xorg configuration.
 *
 * Only a Section "InputClass" whose Identifier is "system-keyboard" is
 * considered.  Options outside that section (including ServerFlags and
 * other InputClass sections) are ignored.  If the section occurs more than
 * once, the last matching section wins.  Within a section, the last
 * occurrence of an option wins, matching Xorg's usual override semantics.
 *
 * All output strings are newly allocated.  Missing options are returned as
 * empty strings.  Malformed quoting or unbalanced Section/EndSection
 * directives return G_IO_ERROR_INVALID_DATA.
 */
gboolean rcl_xkb_config_parse ( const gchar *contents,
                                gchar **out_layout,
                                gchar **out_model,
                                gchar **out_variant,
                                gchar **out_options,
                                GError **error );

G_END_DECLS
