/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Per-user GNOME input-source settings monitor.
 *
 * This process is launched by localed after dropping to the selected
 * session user's UID and GID. It never writes system configuration. It
 * emits a single GVariant tuple per settings update on stdout:
 *   (as layouts, as variants, as options)
 */

#include <gio/gio.h>
#include <glib.h>

static void
emit_settings(GSettings *settings)
{
  GVariant *sources;
  GVariant *options;
  GVariantIter iter;
  const gchar *type;
  const gchar *id;
  GPtrArray *layouts = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *variants = g_ptr_array_new_with_free_func(g_free);
  gchar **option_strv;
  GVariant *snapshot;
  gchar *printed;

  sources = g_settings_get_value(settings, "sources");
  g_variant_iter_init(&iter, sources);

  while (g_variant_iter_next(&iter, "(&s&s)", &type, &id))
  {
    if (g_strcmp0(type, "xkb") != 0)
      continue;

    g_ptr_array_add(layouts, g_strdup(id));
    /* GNOME stores layout and variant together in the source identifier:
     * a source ID may be "layout+variant". */
    {
      gchar **parts = g_strsplit(id, "+", 2);
      gchar *layout = g_strdup(parts[0]);
      gchar *variant = g_strdup(parts[1] ? parts[1] : "");

      g_free(g_ptr_array_index(layouts, layouts->len - 1));
      g_ptr_array_index(layouts, layouts->len - 1) = layout;
      g_ptr_array_add(variants, variant);
      g_strfreev(parts);
    }
  }

  options = g_settings_get_value(settings, "xkb-options");
  option_strv = g_variant_dup_strv(options, NULL);

  g_ptr_array_add(layouts, NULL);
  g_ptr_array_add(variants, NULL);

  snapshot = g_variant_new("(^as^as^as)",
                           (gchar **) layouts->pdata,
                           (gchar **) variants->pdata,
                           option_strv);
  printed = g_variant_print(snapshot, TRUE);

  g_print("%s\n", printed);
  fflush(stdout);

  g_free(printed);
  g_strfreev(option_strv);
  g_variant_unref(options);
  g_variant_unref(sources);
  g_ptr_array_unref(layouts);
  g_ptr_array_unref(variants);
}

static void
settings_changed(GSettings *settings, const gchar *key, gpointer user_data)
{
  (void) user_data;

  if (g_strcmp0(key, "sources") == 0 ||
      g_strcmp0(key, "xkb-options") == 0)
    emit_settings(settings);
}

int
main(void)
{
  GSettingsSchemaSource *source;
  GSettingsSchema *schema;
  GSettings *settings;

  source = g_settings_schema_source_get_default();
  if (!source)
  {
    g_printerr("no GSettings schema source available\n");
    return 1;
  }

  schema = g_settings_schema_source_lookup(
      source, "org.gnome.desktop.input-sources", TRUE);
  if (!schema)
  {
    g_printerr("GNOME input-sources GSettings schema is unavailable\n");
    return 1;
  }

  settings = g_settings_new_full(schema, NULL, NULL);
  g_signal_connect(settings, "changed",
                   G_CALLBACK(settings_changed), NULL);

  emit_settings(settings);
  g_main_loop_run(g_main_loop_new(NULL, FALSE));

  g_object_unref(settings);
  g_settings_schema_unref(schema);
  return 0;
}
