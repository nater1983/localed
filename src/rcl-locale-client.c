/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rcl-locale-client.h"
#define SERVICE "org.freedesktop.locale1"
#define PATH "/org/freedesktop/locale1"
static gboolean sync_enabled = FALSE;

void rcl_locale_client_set_sync(gboolean enabled) { sync_enabled = enabled; }

static GVariant *call(const gchar *interface, const gchar *method, GVariant *args,
                      const GVariantType *reply_type, GError **error)
{
  g_autoptr(GDBusConnection) connection = NULL;
  g_autoptr(GVariant) owned_args = g_variant_ref_sink(args);
  connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
  if (!connection)
    return NULL;
  return g_dbus_connection_call_sync(connection, SERVICE, PATH, interface, method,
      owned_args, reply_type, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, error);
}

static gboolean write_method(const gchar *method, GVariant *args, GError **error)
{
  g_autoptr(GVariant) reply = call(SERVICE, method, args, G_VARIANT_TYPE_UNIT, error);
  return reply != NULL;
}

gboolean rcl_locale_client_get_x11(gchar **layout, gchar **model, gchar **variant, gchar **options, GError **error)
{
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariant) dict = NULL;
  const gchar *l = "", *m = "", *v = "", *o = "";
  if (sync_enabled) {
    reply = call("org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", SERVICE),
                 G_VARIANT_TYPE("(a{sv})"), error);
    if (!reply) return FALSE;
    g_variant_get(reply, "(@a{sv})", &dict);
    if (!g_variant_lookup(dict, "X11Layout", "&s", &l) ||
        !g_variant_lookup(dict, "X11Model", "&s", &m) ||
        !g_variant_lookup(dict, "X11Variant", "&s", &v) ||
        !g_variant_lookup(dict, "X11Options", "&s", &o)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Incomplete locale1 XKB properties");
      return FALSE;
    }
  }
  *layout = g_strdup(l); *model = g_strdup(m); *variant = g_strdup(v); *options = g_strdup(o);
  return TRUE;
}

gboolean rcl_locale_client_set_x11_keyboard(const gchar *layout, const gchar *model,
    const gchar *variant, const gchar *options, GError **error)
{
  if (!sync_enabled) {
    g_message("Observed XKB layouts=%s variants=%s options=%s (no system write)", layout, variant, options);
    return TRUE;
  }
  return write_method("SetX11Keyboard", g_variant_new("(ssssbb)", layout, model, variant, options, FALSE, FALSE), error);
}

gboolean rcl_locale_client_set_vconsole_keyboard(const gchar *keymap, const gchar *toggle, GError **error)
{
  if (!sync_enabled) {
    g_message("Observed active keymap mapping=%s (no system write)", keymap);
    return TRUE;
  }
  return write_method("SetVConsoleKeyboard", g_variant_new("(ssbb)", keymap, toggle, FALSE, FALSE), error);
}

gboolean rcl_locale_client_set_language(const gchar *language, GError **error)
{
  if (!sync_enabled) {
    g_message("Observed account language=%s (no system write)", language);
    return TRUE;
  }
  /* Never fall back to replacement SetLocale: older implementations could
   * remove unrelated settings. UpdateLocale is a localed extension. */
  g_autofree gchar *assignment = g_strdup_printf("LANG=%s", language);
  const gchar *values[] = { assignment, NULL };
  return write_method("UpdateLocale", g_variant_new("(@asb)", g_variant_new_strv(values, -1), FALSE), error);
}
