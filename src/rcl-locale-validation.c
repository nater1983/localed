/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rcl-locale-validation.h"

#include <errno.h>
#include <locale.h>
#include <string.h>

#define MAX_LOCALE_LITERAL_LEN 256
#define MAX_LOCALE_NAME_LEN 127
#define LOCALE_NAME_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-@"

gboolean
rcl_locale_value_is_safe(const gchar *value)
{
  return value && strlen(value) <= MAX_LOCALE_LITERAL_LEN &&
         strspn(value, LOCALE_NAME_CHARS ":,") == strlen(value);
}

gboolean
rcl_locale_name_is_valid(const gchar *name)
{
  return name && *name && strlen(name) <= MAX_LOCALE_NAME_LEN &&
         strspn(name, LOCALE_NAME_CHARS) == strlen(name) &&
         !g_str_equal(name, ".") && !g_str_equal(name, "..");
}

gboolean
rcl_locale_name_is_installed(const gchar *name, GError **error)
{
  locale_t locale;

  if (!rcl_locale_name_is_valid(name))
    return FALSE;
  if (g_str_equal(name, "C") || g_str_equal(name, "POSIX"))
    return TRUE;

  /* newlocale() validates all categories without touching setlocale().
   * Slackware uses glibc; its locale objects reflect installed locale data. */
  errno = 0;
  locale = newlocale(LC_ALL_MASK, name, (locale_t) 0);
  if (!locale)
  {
    if (errno == ENOMEM)
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          _("Not enough memory to validate locale"));
    return FALSE;
  }
  freelocale(locale);
  return TRUE;
}

static gboolean
language_list_is_valid(const gchar *value)
{
  if (!rcl_locale_value_is_safe(value))
    return FALSE;

  /* LANGUAGE is a gettext search order, not a glibc locale. Its entries
   * may be language-only codes for translations, e.g. es or en. */
  g_auto(GStrv) languages = g_strsplit(value, ":", -1);
  for (guint i = 0; languages[i]; i++)
    if (!rcl_locale_name_is_valid(languages[i]))
      return FALSE;
  return TRUE;
}

gboolean
rcl_locale_validate_assignment(const gchar *key, const gchar *value,
                               gboolean extension, GError **error)
{
  g_autoptr(GError) lookup_error = NULL;

  g_return_val_if_fail(key != NULL, FALSE);
  if (extension && value && !*value)
    return TRUE;

  if (extension && g_str_equal(key, "LANGUAGE"))
  {
    if (value && *value && language_list_is_valid(value))
      return TRUE;
  }
  else if (rcl_locale_name_is_valid(value))
  {
    if (rcl_locale_name_is_installed(value, &lookup_error))
      return TRUE;
    if (lookup_error)
      g_propagate_error(error, g_steal_pointer(&lookup_error));
    else
      g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                  _("Specified locale is not installed: %s"), value);
    return FALSE;
  }

  g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
              _("Invalid locale value for %s"), key);
  return FALSE;
}
