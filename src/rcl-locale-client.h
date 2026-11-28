/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <gio/gio.h>
G_BEGIN_DECLS
void rcl_locale_client_set_sync(gboolean enabled);
gboolean rcl_locale_client_get_x11(gchar **layout, gchar **model, gchar **variant, gchar **options, GError **error);
gboolean rcl_locale_client_set_x11_keyboard(const gchar *layout, const gchar *model, const gchar *variant, const gchar *options, GError **error);
gboolean rcl_locale_client_set_vconsole_keyboard(const gchar *keymap, const gchar *toggle, GError **error);
gboolean rcl_locale_client_set_language(const gchar *language, GError **error);
G_END_DECLS
