/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rcl-session-monitor.h"

#define LOGIND_NAME "org.freedesktop.login1"
#define LOGIND_MANAGER_PATH "/org/freedesktop/login1"
#define LOGIND_MANAGER_IFACE "org.freedesktop.login1.Manager"
#define LOGIND_SESSION_IFACE "org.freedesktop.login1.Session"
#define DBUS_PROPERTIES_IFACE "org.freedesktop.DBus.Properties"
#define RECONNECT_DELAY_MS 2500
#define REFRESH_DELAY_MS 100

struct _RclSessionMonitor {
  GDBusConnection *connection;
  guint name_watch_id, manager_subscription_id, properties_subscription_id;
  gulong connection_closed_id;
  guint reconnect_id, refresh_id;
  gboolean stopping, logind_available;
  RclSessionMonitorCallback refresh, lost;
  gpointer user_data;
};

static gboolean
refresh_timeout_cb(gpointer user_data)
{
  RclSessionMonitor *self = user_data;

  self->refresh_id = 0;
  self->refresh(self->user_data);
  return G_SOURCE_REMOVE;
}

static void
schedule_refresh(RclSessionMonitor *self)
{
  if (self->stopping || self->refresh_id)
    return;

  self->refresh_id = g_timeout_add(REFRESH_DELAY_MS,
                                   refresh_timeout_cb, self);
}

static void
manager_signal_cb(GDBusConnection *connection,
                  const gchar *sender_name,
                  const gchar *object_path,
                  const gchar *interface_name,
                  const gchar *signal_name,
                  GVariant *parameters,
                  gpointer user_data)
{
  RclSessionMonitor *self = user_data;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) parameters;

  if (g_strcmp0(signal_name, "SessionNew") == 0 ||
      g_strcmp0(signal_name, "SessionRemoved") == 0)
    schedule_refresh(self);
}

static void
properties_changed_cb(GDBusConnection *connection,
                      const gchar *sender_name,
                      const gchar *object_path,
                      const gchar *interface_name,
                      const gchar *signal_name,
                      GVariant *parameters,
                      gpointer user_data)
{
  RclSessionMonitor *self = user_data;
  const gchar *changed_interface;
  GVariant *changed = NULL;
  GVariant *invalidated = NULL;

  (void) connection;
  (void) sender_name;
  (void) interface_name;
  (void) signal_name;

  if (!object_path ||
      !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sa{sv}as)")))
    return;

  g_variant_get(parameters, "(&s@a{sv}@as)",
                &changed_interface, &changed, &invalidated);

  if (g_strcmp0(changed_interface, LOGIND_SESSION_IFACE) == 0)
    schedule_refresh(self);

  g_clear_pointer(&changed, g_variant_unref);
  g_clear_pointer(&invalidated, g_variant_unref);
}

static void
unsubscribe_signals(RclSessionMonitor *self)
{
  if (!self->connection)
    return;

  if (self->manager_subscription_id)
  {
    g_dbus_connection_signal_unsubscribe(self->connection,
                                         self->manager_subscription_id);
    self->manager_subscription_id = 0;
  }

  if (self->properties_subscription_id)
  {
    g_dbus_connection_signal_unsubscribe(self->connection,
                                         self->properties_subscription_id);
    self->properties_subscription_id = 0;
  }
}

static void
name_appeared_cb(GDBusConnection *connection,
                 const gchar *name,
                 const gchar *owner,
                 gpointer user_data)
{
  RclSessionMonitor *self = user_data;

  (void) name;
  (void) owner;

  if (self->stopping || connection != self->connection)
    return;

  self->logind_available = TRUE;

  if (!self->manager_subscription_id)
    self->manager_subscription_id =
      g_dbus_connection_signal_subscribe(
          connection, LOGIND_NAME, LOGIND_MANAGER_IFACE, NULL,
          LOGIND_MANAGER_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
          manager_signal_cb, self, NULL);

  if (!self->properties_subscription_id)
    self->properties_subscription_id =
      g_dbus_connection_signal_subscribe(
          connection, LOGIND_NAME, DBUS_PROPERTIES_IFACE,
          "PropertiesChanged", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
          properties_changed_cb, self, NULL);

  g_debug("watching logind for desktop session changes");
  schedule_refresh(self);
}

static void
name_vanished_cb(GDBusConnection *connection,
                 const gchar *name,
                 gpointer user_data)
{
  RclSessionMonitor *self = user_data;

  (void) name;

  if (connection != self->connection)
    return;

  self->logind_available = FALSE;
    unsubscribe_signals(self);

  if (self->refresh_id)
  {
    g_source_remove(self->refresh_id);
    self->refresh_id = 0;
  }

  self->lost(self->user_data);

  g_debug("logind disappeared; waiting for it to return");
}

static gboolean monitor_connect(RclSessionMonitor *self);

static gboolean
reconnect_timeout_cb(gpointer user_data)
{
  RclSessionMonitor *self = user_data;

  self->reconnect_id = 0;
  if (!self->stopping)
    monitor_connect(self);

  return G_SOURCE_REMOVE;
}

static void
connection_closed_cb(GDBusConnection *connection,
                     gboolean remote_peer_vanished,
                     GError *error,
                     gpointer user_data)
{
  RclSessionMonitor *self = user_data;

  (void) remote_peer_vanished;

  if (connection != self->connection || self->stopping)
    return;

  g_debug("system D-Bus connection for desktop session discovery closed: %s",
          error ? error->message : "connection closed");

  if (self->name_watch_id)
  {
    g_bus_unwatch_name(self->name_watch_id);
    self->name_watch_id = 0;
  }

  unsubscribe_signals(self);
  self->logind_available = FALSE;
    if (self->refresh_id)
  {
    g_source_remove(self->refresh_id);
    self->refresh_id = 0;
  }
  g_signal_handler_disconnect(connection, self->connection_closed_id);
  self->connection_closed_id = 0;
  g_clear_object(&self->connection);

  self->lost(self->user_data);

  if (!self->stopping && !self->reconnect_id)
    self->reconnect_id =
      g_timeout_add(RECONNECT_DELAY_MS, reconnect_timeout_cb, self);
}

static gboolean
monitor_connect(RclSessionMonitor *self)
{
  gchar *address;
  GError *error = NULL;

  if (self->stopping || self->connection)
    return self->connection != NULL;

  address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
  if (!address)
    goto failed;

  self->connection = g_dbus_connection_new_for_address_sync(
      address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
      G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, &error);
  g_free(address);

  if (!self->connection)
    goto failed;

  g_dbus_connection_set_exit_on_close(self->connection, FALSE);
  self->connection_closed_id =
    g_signal_connect(self->connection, "closed",
                     G_CALLBACK(connection_closed_cb), self);

  self->name_watch_id =
    g_bus_watch_name_on_connection(self->connection, LOGIND_NAME,
                                   G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   name_appeared_cb, name_vanished_cb,
                                   self, NULL);

  g_debug("watching for logind on the system bus");
  return TRUE;

failed:
  g_warning("could not connect to system D-Bus for desktop session discovery: %s",
            error ? error->message : "unknown error");
  g_clear_error(&error);

  if (!self->stopping && !self->reconnect_id)
    self->reconnect_id =
      g_timeout_add(RECONNECT_DELAY_MS, reconnect_timeout_cb, self);

  return FALSE;
}

void
rcl_session_monitor_stop(RclSessionMonitor *self)
{
  if (!self)
    return;

  self->stopping = TRUE;

  if (self->reconnect_id)
  {
    g_source_remove(self->reconnect_id);
    self->reconnect_id = 0;
  }

  if (self->refresh_id)
  {
    g_source_remove(self->refresh_id);
    self->refresh_id = 0;
  }

  if (self->name_watch_id)
  {
    g_bus_unwatch_name(self->name_watch_id);
    self->name_watch_id = 0;
  }

  unsubscribe_signals(self);

  if (self->connection && self->connection_closed_id)
  {
    g_signal_handler_disconnect(self->connection,
                                self->connection_closed_id);
    self->connection_closed_id = 0;
  }

  g_clear_object(&self->connection);
  self->logind_available = FALSE;
}


RclSessionMonitor *
rcl_session_monitor_new(RclSessionMonitorCallback refresh,
                        RclSessionMonitorCallback lost, gpointer user_data)
{
  RclSessionMonitor *self = g_new0(RclSessionMonitor, 1);
  self->refresh = refresh;
  self->lost = lost;
  self->user_data = user_data;
  self->stopping = TRUE;
  return self;
}

gboolean
rcl_session_monitor_start(RclSessionMonitor *self)
{
  if (self->connection || self->reconnect_id)
    return TRUE;
  self->stopping = FALSE;
  return monitor_connect(self);
}

void
rcl_session_monitor_free(RclSessionMonitor *self)
{
  rcl_session_monitor_stop(self);
  g_free(self);
}

GDBusConnection *
rcl_session_monitor_get_connection(RclSessionMonitor *self)
{
  return self ? self->connection : NULL;
}

gboolean
rcl_session_monitor_is_available(RclSessionMonitor *self)
{
  return self && !self->stopping && self->logind_available;
}
