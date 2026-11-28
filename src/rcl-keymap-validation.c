/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rcl-keymap-validation.h"
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static const gchar *const default_dirs[] = {
  "/usr/share/keymaps", "/usr/share/kbd/keymaps", "/usr/lib/kbd/keymaps", NULL
};

gboolean
rcl_keymap_name_is_valid(const gchar *name)
{
  /* Also reject option-like names: rc.keymap passes the name to loadkeys. */
  return name && g_ascii_isalnum(name[0]) && strlen(name) < 128 &&
         strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-+:,")
           == strlen(name);
}

static gboolean
directory_has_keymap(const gchar *directory, const gchar *plain,
                     const gchar *compressed, guint depth)
{
  if (depth > 32)
    return FALSE;
  g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
  const gchar *entry;
  if (!dir)
    return FALSE;

  while ((entry = g_dir_read_name(dir)))
  {
    if (entry[0] == '.')
      continue;
    g_autofree gchar *path = g_build_filename(directory, entry, NULL);
    /* Accept readable regular maps and aliases to regular maps. Do not
     * recurse through symlink directories, which could form cycles. */
    if ((g_str_equal(entry, plain) || g_str_equal(entry, compressed)) &&
        g_file_test(path, G_FILE_TEST_IS_REGULAR) && g_access(path, R_OK) == 0)
      return TRUE;
    if (!g_file_test(path, G_FILE_TEST_IS_SYMLINK) &&
        g_file_test(path, G_FILE_TEST_IS_DIR) &&
        directory_has_keymap(path, plain, compressed, depth + 1))
      return TRUE;
  }
  return FALSE;
}

gboolean
rcl_keymap_is_installed(const gchar *name, const gchar *const *directories)
{
  if (!rcl_keymap_name_is_valid(name))
    return FALSE;
  g_autofree gchar *plain = g_strconcat(name, ".map", NULL);
  g_autofree gchar *compressed = g_strconcat(name, ".map.gz", NULL);
  if (!directories)
    directories = default_dirs;
  for (guint i = 0; directories[i]; i++)
    if (directory_has_keymap(directories[i], plain, compressed, 0))
      return TRUE;
  return FALSE;
}

gboolean
rcl_keymap_validate(const gchar *name, const gchar *const *directories,
                     GError **error)
{
  if (name && !*name)
    return TRUE;
  if (!rcl_keymap_name_is_valid(name))
  {
    g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                        _("Invalid console keymap name"));
    return FALSE;
  }
  if (!rcl_keymap_is_installed(name, directories))
  {
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                _("Keymap %s is not installed"), name);
    return FALSE;
  }
  return TRUE;
}
