/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* Safe literal values read from existing Slackware configuration. This is
 * intentionally not an availability check: old settings must remain readable. */
gboolean rcl_locale_value_is_safe(const gchar *value);

/* Locale-name syntax used by upstream systemd v262. */
gboolean rcl_locale_name_is_valid(const gchar *name);

/* Check a real glibc locale without changing the process locale.
 * Returns FALSE for unknown/invalid names; reports allocation errors separately. */
gboolean rcl_locale_name_is_installed(const gchar *name, GError **error);

/* Validate a requested assignment. Only the localed extension allows removal
 * and gettext LANGUAGE preference chains. */
gboolean rcl_locale_validate_assignment(const gchar *key, const gchar *value,
                                        gboolean extension, GError **error);

G_END_DECLS
