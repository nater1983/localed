/*
 * Copyright (C) 2026 Nathaniel Russell
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exercise AccountsService name monitoring and repeated lifecycle cleanup
 * against an isolated in-process D-Bus daemon. No host system bus is touched.
 */
#include <gio/gio.h>
#include <glib.h>

#include "rcl-accounts-service.h"
#include "rcl-locale.h"

#define TEST_ACCOUNTS_NAME "org.freedesktop.Accounts"

typedef struct
{
  GTestDBus *bus;
  gchar *address;
  gchar *previous_system_bus_address;
  GDBusConnection *client;
} Fixture;

static GDBusConnection *
connect_test_bus(const gchar *address)
{
  GError *error = NULL;
  GDBusConnection *connection =
    g_dbus_connection_new_for_address_sync(
      address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
      G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, &error);

  g_assert_no_error(error);
  g_assert_nonnull(connection);
  g_dbus_connection_set_exit_on_close(connection, FALSE);
  return connection;
}

static void
fixture_setup(Fixture *fixture, gconstpointer user_data)
{
  GError *error = NULL;

  fixture->bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(fixture->bus);

  fixture->address = g_strdup(g_test_dbus_get_bus_address(fixture->bus));
  g_assert_nonnull(fixture->address);
  fixture->previous_system_bus_address =
    g_strdup(g_getenv("DBUS_SYSTEM_BUS_ADDRESS"));

  /* The production monitor deliberately connects to the system bus. */
  g_setenv("DBUS_SYSTEM_BUS_ADDRESS", fixture->address, TRUE);

  fixture->client = connect_test_bus(fixture->address);
}

static void
fixture_teardown(Fixture *fixture, gconstpointer user_data)
{
  if (fixture->client) {
    g_dbus_connection_close_sync(fixture->client, NULL, NULL);
    g_clear_object(&fixture->client);
  }

  g_test_dbus_down(fixture->bus);
  g_clear_object(&fixture->bus);
  if (fixture->previous_system_bus_address)
    g_setenv("DBUS_SYSTEM_BUS_ADDRESS",
             fixture->previous_system_bus_address, TRUE);
  else
    g_unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
  g_free(fixture->previous_system_bus_address);
  g_free(fixture->address);
}

static void
iterate_for(guint milliseconds)
{
  gint64 deadline = g_get_monotonic_time() + ((gint64) milliseconds * 1000);

  while (g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(1000);
  }
}

static void
request_accounts_name(Fixture *fixture, gboolean acquire)
{
  GError *error = NULL;
  GVariant *reply;
  guint result;

  reply = g_dbus_connection_call_sync(
    fixture->client,
    "org.freedesktop.DBus",
    "/org/freedesktop/DBus",
    "org.freedesktop.DBus",
    acquire ? "RequestName" : "ReleaseName",
    acquire
      ? g_variant_new("(su)", TEST_ACCOUNTS_NAME, 0u)
      : g_variant_new("(s)", TEST_ACCOUNTS_NAME),
    G_VARIANT_TYPE(acquire ? "(u)" : "(u)"),
    G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);

  g_assert_no_error(error);
  g_assert_nonnull(reply);
  g_variant_get(reply, "(u)", &result);
  g_variant_unref(reply);

  if (acquire)
    g_assert_true(result == 1u || result == 4u); /* PRIMARY_OWNER / ALREADY_OWNER */
  else
    g_assert_cmpuint(result, ==, 1u); /* RELEASED */
}

static void
test_accounts_name_lifecycle(Fixture *fixture, gconstpointer user_data)
{
  RclLocaleDaemon *daemon;
  RclAccountsService *service;

  daemon = g_object_new(RCL_TYPE_LOCALE_DAEMON, NULL);
  g_assert_nonnull(daemon);
  service = rcl_accounts_service_new(daemon);
  g_assert_nonnull(service);

  /* Appearance triggers a PropertiesChanged subscription; disappearance
   * removes it. Repeat to catch stale watch/subscription identifiers. */
  g_assert_true(rcl_accounts_service_start(service, NULL));
  iterate_for(50);
  request_accounts_name(fixture, TRUE);
  iterate_for(100);
  request_accounts_name(fixture, FALSE);
  iterate_for(100);

  rcl_accounts_service_stop(service);
  rcl_accounts_service_stop(service); /* stop is intentionally idempotent */

  g_object_unref(service);
  g_object_unref(daemon);
}

static void
test_repeated_start_stop_cleanup(Fixture *fixture, gconstpointer user_data)
{
  for (guint i = 0; i < 20; i++) {
    RclLocaleDaemon *daemon =
      g_object_new(RCL_TYPE_LOCALE_DAEMON, NULL);
    RclAccountsService *service =
      rcl_accounts_service_new(daemon);

    g_assert_nonnull(daemon);
    g_assert_nonnull(service);

    /* Exercise real object registration and repeated shutdown cleanup. */
    g_assert_true(rcl_daemon_startup(RCL_DAEMON(daemon),
                                     fixture->client));
    g_assert_true(rcl_accounts_service_start(service, NULL));
    iterate_for(5);

    rcl_accounts_service_stop(service);
    rcl_daemon_shutdown(RCL_DAEMON(daemon));
    rcl_daemon_shutdown(RCL_DAEMON(daemon));
    g_object_unref(service);
    g_object_unref(daemon);

    /* Drain queued name-watch/closed callbacks after each disposal. */
    iterate_for(5);
  }
}

static void
test_system_bus_restart(Fixture *fixture, gconstpointer user_data)
{
  RclLocaleDaemon *daemon =
    g_object_new(RCL_TYPE_LOCALE_DAEMON, NULL);
  RclAccountsService *service = rcl_accounts_service_new(daemon);

  g_assert_nonnull(daemon);
  g_assert_nonnull(service);
  g_assert_true(rcl_accounts_service_start(service, NULL));
  iterate_for(100);

  /* Stop the private test bus. The monitor must discard its closed
   * connection and retry against a newly started bus daemon. */
  g_test_dbus_down(fixture->bus);
  iterate_for(100);

  if (fixture->client) {
    g_clear_object(&fixture->client);
  }

  g_test_dbus_up(fixture->bus);
  g_free(fixture->address);
  fixture->address = g_strdup(g_test_dbus_get_bus_address(fixture->bus));
  g_assert_nonnull(fixture->address);
  g_setenv("DBUS_SYSTEM_BUS_ADDRESS", fixture->address, TRUE);
  fixture->client = connect_test_bus(fixture->address);

  /* The production retry interval is 2.5 seconds. */
  iterate_for(2800);

  request_accounts_name(fixture, TRUE);
  iterate_for(100);
  request_accounts_name(fixture, FALSE);
  iterate_for(100);

  rcl_accounts_service_stop(service);
  g_object_unref(service);
  g_object_unref(daemon);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add("/dbus/accounts-name-lifecycle", Fixture, NULL,
             fixture_setup, test_accounts_name_lifecycle, fixture_teardown);
  g_test_add("/dbus/repeated-start-stop-cleanup", Fixture, NULL,
             fixture_setup, test_repeated_start_stop_cleanup, fixture_teardown);
  g_test_add("/dbus/system-bus-restart", Fixture, NULL,
             fixture_setup, test_system_bus_restart, fixture_teardown);

  return g_test_run();
}
