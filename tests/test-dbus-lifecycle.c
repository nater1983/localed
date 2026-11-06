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


/* A minimal PolicyKit Authority used to exercise the daemon's direct D-Bus
 * authorization path without invoking the host polkitd or modifying files. */
#define TEST_POLKIT_NAME "org.freedesktop.PolicyKit1"
#define TEST_POLKIT_PATH "/org/freedesktop/PolicyKit1/Authority"

typedef struct
{
  gboolean hold_check;
  gboolean check_seen;
  gboolean cancel_seen;
  gboolean client_done;
  guint check_flags;
  gchar *check_cancellation_id;
  gchar *cancel_cancellation_id;
  GError *client_error;
  GDBusMethodInvocation *pending_check;
} PolkitFixture;

static const gchar test_polkit_xml[] =
  "<node>"
  " <interface name='org.freedesktop.PolicyKit1.Authority'>"
  "  <method name='CheckAuthorization'>"
  "   <arg type='(sa{sv})' direction='in'/>"
  "   <arg type='s' direction='in'/>"
  "   <arg type='a{ss}' direction='in'/>"
  "   <arg type='u' direction='in'/>"
  "   <arg type='s' direction='in'/>"
  "   <arg type='(bba{ss})' direction='out'/>"
  "  </method>"
  "  <method name='CancelCheckAuthorization'>"
  "   <arg type='s' direction='in'/>"
  "  </method>"
  " </interface>"
  "</node>";

static void
polkit_method_call(GDBusConnection *connection,
                   const gchar *sender,
                   const gchar *object_path,
                   const gchar *interface_name,
                   const gchar *method_name,
                   GVariant *parameters,
                   GDBusMethodInvocation *invocation,
                   gpointer user_data)
{
  PolkitFixture *fixture = user_data;

  if (g_str_equal(method_name, "CheckAuthorization")) {
    GVariant *id_value = g_variant_get_child_value(parameters, 4);
    GVariant *flags_value = g_variant_get_child_value(parameters, 3);
    const gchar *id = g_variant_get_string(id_value, NULL);

    fixture->check_flags = g_variant_get_uint32(flags_value);
    g_variant_unref(flags_value);
    fixture->check_seen = TRUE;
    g_free(fixture->check_cancellation_id);
    fixture->check_cancellation_id = g_strdup(id);
    g_variant_unref(id_value);

    if (fixture->hold_check) {
      fixture->pending_check = g_object_ref(invocation);
      return;
    }

    g_dbus_method_invocation_return_value(
      invocation,
      g_variant_new("(bb@a{ss})", FALSE, FALSE,
                    g_variant_new_array(G_VARIANT_TYPE("{ss}"), NULL, 0)));
    return;
  }

  if (g_str_equal(method_name, "CancelCheckAuthorization")) {
    GVariant *id_value = g_variant_get_child_value(parameters, 0);
    const gchar *id = g_variant_get_string(id_value, NULL);

    fixture->cancel_seen = TRUE;
    g_free(fixture->cancel_cancellation_id);
    fixture->cancel_cancellation_id = g_strdup(id);
    g_variant_unref(id_value);
    if (fixture->pending_check) {
      g_dbus_method_invocation_return_dbus_error(
        fixture->pending_check,
        "org.freedesktop.PolicyKit1.Error.Cancelled",
        "Test authorization was cancelled");
      g_clear_object(&fixture->pending_check);
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }

  g_dbus_method_invocation_return_dbus_error(
    invocation, "org.freedesktop.DBus.Error.UnknownMethod",
    "Unknown test PolicyKit method");
}

static const GDBusInterfaceVTable test_polkit_vtable = {
  polkit_method_call, NULL, NULL, { 0 }
};

static void
polkit_client_call_done(GObject *source,
                        GAsyncResult *result,
                        gpointer user_data)
{
  PolkitFixture *fixture = user_data;
  GVariant *reply;

  reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                        result, &fixture->client_error);
  if (reply)
    g_variant_unref(reply);
  fixture->client_done = TRUE;
}

static gboolean
polkit_condition(gpointer user_data)
{
  PolkitFixture *fixture = user_data;
  return fixture->client_done;
}

static gboolean
polkit_check_condition(gpointer user_data)
{
  PolkitFixture *fixture = user_data;
  return fixture->check_seen;
}

static gboolean
polkit_cancel_condition(gpointer user_data)
{
  PolkitFixture *fixture = user_data;
  return fixture->cancel_seen && fixture->client_done;
}

static void
iterate_until(PolkitFixture *fixture,
              gboolean (*condition)(gpointer),
              guint timeout_ms)
{
  gint64 deadline = g_get_monotonic_time() + ((gint64) timeout_ms * 1000);

  while (!condition(fixture) && g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(1000);
  }
  g_assert_true(condition(fixture));
}

static void
polkit_fixture_free(PolkitFixture *fixture)
{
  g_free(fixture->check_cancellation_id);
  g_free(fixture->cancel_cancellation_id);
  g_clear_error(&fixture->client_error);
  g_clear_object(&fixture->pending_check);
}

static void
test_polkit_denial(Fixture *fixture, gconstpointer user_data)
{
  GError *error = NULL;
  GDBusConnection *daemon_bus = connect_test_bus(fixture->address);
  GDBusConnection *polkit_bus = connect_test_bus(fixture->address);
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(test_polkit_xml, &error);
  PolkitFixture polkit_fixture = { 0 };
  RclDaemon *daemon = rcl_daemon_new();
  guint registration_id;
  guint name_result;
  const gchar *locale[] = { "LANG=en_US.UTF-8", NULL };

  g_assert_no_error(error);
  g_assert_nonnull(node);
  g_assert_true(rcl_daemon_startup(daemon, daemon_bus));

  GVariant *name_reply = g_dbus_connection_call_sync(
    polkit_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
    "org.freedesktop.DBus", "RequestName",
    g_variant_new("(su)", TEST_POLKIT_NAME, 0u),
    G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
  g_assert_no_error(error);
  g_variant_get(name_reply, "(u)", &name_result);
  g_variant_unref(name_reply);
  g_assert_true(name_result == 1u || name_result == 4u);

  registration_id = g_dbus_connection_register_object(
    polkit_bus, TEST_POLKIT_PATH,
    g_dbus_node_info_lookup_interface(node,
                                      "org.freedesktop.PolicyKit1.Authority"),
    &test_polkit_vtable, &polkit_fixture, NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(registration_id, >, 0);

  g_dbus_connection_call(
    fixture->client, g_dbus_connection_get_unique_name(daemon_bus),
    "/org/freedesktop/locale1", "org.freedesktop.locale1", "SetLocale",
    g_variant_new("(@asb)", g_variant_new_strv(locale, -1), TRUE),
    NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
    polkit_client_call_done, &polkit_fixture);

  iterate_until(&polkit_fixture, polkit_condition, 5000);
  g_assert_true(polkit_fixture.check_seen);
  g_assert_nonnull(polkit_fixture.check_cancellation_id);
  g_assert_cmpstr(polkit_fixture.check_cancellation_id, !=, "");
  g_assert_cmpuint(polkit_fixture.check_flags, ==, 1u);
  g_assert_nonnull(polkit_fixture.client_error);
  {
    gchar *remote_error =
      g_dbus_error_get_remote_error(polkit_fixture.client_error);
    g_assert_cmpstr(remote_error, ==, "org.freedesktop.DBus.Error.AuthFailed");
    g_free(remote_error);
  }

  polkit_fixture_free(&polkit_fixture);
  rcl_daemon_shutdown(daemon);
  g_object_unref(daemon);
  g_dbus_connection_unregister_object(polkit_bus, registration_id);
  g_dbus_node_info_unref(node);
  g_dbus_connection_close_sync(polkit_bus, NULL, NULL);
  g_dbus_connection_close_sync(daemon_bus, NULL, NULL);
  g_object_unref(polkit_bus);
  g_object_unref(daemon_bus);
}

static void
test_polkit_cancellation(Fixture *fixture, gconstpointer user_data)
{
  GError *error = NULL;
  GDBusConnection *daemon_bus = connect_test_bus(fixture->address);
  GDBusConnection *polkit_bus = connect_test_bus(fixture->address);
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(test_polkit_xml, &error);
  PolkitFixture polkit_fixture = { .hold_check = TRUE };
  RclDaemon *daemon = rcl_daemon_new();
  guint registration_id;
  guint name_result;
  const gchar *locale[] = { "LANG=en_US.UTF-8", NULL };

  g_assert_no_error(error);
  g_assert_nonnull(node);
  g_assert_true(rcl_daemon_startup(daemon, daemon_bus));

  GVariant *name_reply = g_dbus_connection_call_sync(
    polkit_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
    "org.freedesktop.DBus", "RequestName",
    g_variant_new("(su)", TEST_POLKIT_NAME, 0u),
    G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
  g_assert_no_error(error);
  g_variant_get(name_reply, "(u)", &name_result);
  g_variant_unref(name_reply);
  g_assert_true(name_result == 1u || name_result == 4u);

  registration_id = g_dbus_connection_register_object(
    polkit_bus, TEST_POLKIT_PATH,
    g_dbus_node_info_lookup_interface(node,
                                      "org.freedesktop.PolicyKit1.Authority"),
    &test_polkit_vtable, &polkit_fixture, NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(registration_id, >, 0);

  g_dbus_connection_call(
    fixture->client, g_dbus_connection_get_unique_name(daemon_bus),
    "/org/freedesktop/locale1", "org.freedesktop.locale1", "SetLocale",
    g_variant_new("(@asb)", g_variant_new_strv(locale, -1), TRUE),
    NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
    polkit_client_call_done, &polkit_fixture);

  iterate_until(&polkit_fixture, polkit_check_condition, 5000);
  g_assert_nonnull(polkit_fixture.check_cancellation_id);
  g_assert_cmpuint(polkit_fixture.check_flags, ==, 1u);

  /* Shutting down the daemon cancels the pending authorization. */
  rcl_daemon_shutdown(daemon);
  iterate_until(&polkit_fixture, polkit_cancel_condition, 5000);

  g_assert_cmpstr(polkit_fixture.cancel_cancellation_id, ==,
                  polkit_fixture.check_cancellation_id);
  g_assert_nonnull(polkit_fixture.client_error);

  polkit_fixture_free(&polkit_fixture);
  g_object_unref(daemon);
  g_dbus_connection_unregister_object(polkit_bus, registration_id);
  g_dbus_node_info_unref(node);
  g_dbus_connection_close_sync(polkit_bus, NULL, NULL);
  g_dbus_connection_close_sync(daemon_bus, NULL, NULL);
  g_object_unref(polkit_bus);
  g_object_unref(daemon_bus);
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
  g_test_add("/polkit/denial", Fixture, NULL,
             fixture_setup, test_polkit_denial, fixture_teardown);
  g_test_add("/polkit/cancellation", Fixture, NULL,
             fixture_setup, test_polkit_cancellation, fixture_teardown);

  return g_test_run();
}
