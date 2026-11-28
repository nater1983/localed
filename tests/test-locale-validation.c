/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rcl-locale-validation.h"
#include <locale.h>
#include <string.h>

static void
test_names(void)
{
  gchar long_name[129];
  memset(long_name, 'a', sizeof(long_name) - 1);
  long_name[sizeof(long_name) - 1] = '\0';

  g_assert_true(rcl_locale_name_is_valid("en_US.UTF-8"));
  g_assert_true(rcl_locale_name_is_valid("sr_RS@latin"));
  g_assert_false(rcl_locale_name_is_valid(NULL));
  g_assert_false(rcl_locale_name_is_valid(""));
  g_assert_false(rcl_locale_name_is_valid("."));
  g_assert_false(rcl_locale_name_is_valid(".."));
  g_assert_false(rcl_locale_name_is_valid("../C"));
  g_assert_false(rcl_locale_name_is_valid("es:en"));
  g_assert_false(rcl_locale_name_is_valid("C;echo"));
  g_assert_false(rcl_locale_name_is_valid("C UTF-8"));
  g_assert_false(rcl_locale_name_is_valid("é"));
  g_assert_false(rcl_locale_name_is_valid(long_name));
  long_name[127] = '\0';
  g_assert_true(rcl_locale_name_is_valid(long_name));
}

static void
test_availability(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *before = g_strdup(setlocale(LC_ALL, NULL));
  g_assert_true(rcl_locale_name_is_installed("C", &error));
  g_assert_no_error(error);
  g_assert_true(rcl_locale_name_is_installed("POSIX", &error));
  g_assert_no_error(error);
  g_assert_false(rcl_locale_name_is_installed("zz_ZZ.localed_nonexistent_987654321", &error));
  g_assert_no_error(error);

  /* Exercise the real newlocale/freelocale path when C.UTF-8 is available. */
  locale_t installed = newlocale(LC_ALL_MASK, "C.UTF-8", (locale_t) 0);
  if (installed)
  {
    freelocale(installed);
    g_assert_true(rcl_locale_name_is_installed("C.UTF-8", &error));
    g_assert_no_error(error);
  }
  else
    g_test_message("C.UTF-8 is not installed; optional positive lookup omitted");

  g_assert_cmpstr(setlocale(LC_ALL, NULL), ==, before);
}

static void
test_assignments(void)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(rcl_locale_validate_assignment("LANG", "C", FALSE, &error));
  g_assert_true(rcl_locale_validate_assignment("LC_TIME", "POSIX", TRUE, &error));
  g_assert_true(rcl_locale_validate_assignment("LANGUAGE", "es:en_US:en", TRUE, &error));
  g_assert_true(rcl_locale_validate_assignment("LC_COLLATE", "", TRUE, &error));
  g_assert_no_error(error);

  const struct { const gchar *key; const gchar *value; gboolean extension; } invalid[] = {
    { "LANG", "zz_ZZ.localed_nonexistent_987654321", FALSE },
    { "LC_TIME", "zz_ZZ.localed_nonexistent_987654321", TRUE },
    { "LANG", "", FALSE },
    { "LANG", "en:es", TRUE },
    { "LANGUAGE", "es:en", FALSE },
    { "LANGUAGE", "es::en", TRUE },
    { "LANGUAGE", ":es", TRUE },
    { "LANGUAGE", "es:", TRUE },
    { "LANGUAGE", "es,en", TRUE },
    { "LANGUAGE", "es:../en", TRUE }
  };
  for (guint i = 0; i < G_N_ELEMENTS(invalid); i++)
  {
    g_assert_false(rcl_locale_validate_assignment(invalid[i].key, invalid[i].value,
                                                    invalid[i].extension, &error));
    g_assert_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
    g_clear_error(&error);
  }
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/locale-validation/names", test_names);
  g_test_add_func("/locale-validation/availability", test_availability);
  g_test_add_func("/locale-validation/assignments", test_assignments);
  return g_test_run();
}
