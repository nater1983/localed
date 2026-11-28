/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rcl-locale-client.h"
static void test_observe(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *l = NULL, *m = NULL, *v = NULL, *o = NULL;
  /* Deliberately invalid address: observation must never contact locale1. */
  g_setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/localed-desktop-test", TRUE);
  rcl_locale_client_set_sync(FALSE);
  g_assert_true(rcl_locale_client_get_x11(&l, &m, &v, &o, &error));
  g_assert_true(rcl_locale_client_set_language("en_US.UTF-8", &error));
  g_assert_true(rcl_locale_client_set_x11_keyboard("us", "", "", "", &error));
  g_assert_true(rcl_locale_client_set_vconsole_keyboard("us", "", &error));
  g_assert_no_error(error);
}
int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/client/observe-no-bus", test_observe);
  return g_test_run();
}
