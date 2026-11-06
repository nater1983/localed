#include "config.h"
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <gio/gio.h>
#include "rcl-accounts-service.h"

#define ACCOUNTS_NAME "org.freedesktop.Accounts"
#define USER_PREFIX "/org/freedesktop/Accounts/User"
#define RECONNECT_DELAY_MS 2500

struct _RclAccountsService {
  GObject parent_instance;
  RclLocaleDaemon *daemon;
  GDBusConnection *connection;
  guint watch_id;
  guint subscription_id;
  gulong connection_closed_id;
  guint reconnect_id;
  gboolean stopping;
};
G_DEFINE_TYPE(RclAccountsService, rcl_accounts_service, G_TYPE_OBJECT)

static gboolean uid_in_privileged_group(guint64 uid)
{
  struct passwd *pw;
  struct group *gr;
  gid_t *groups;
  int ngroups = 16;
  gboolean found = FALSE;
  if (uid > G_MAXINT || !(pw = getpwuid((uid_t) uid)) ||
      !(gr = getgrnam(PRIVILEGED_GROUP)))
    return FALSE;
  groups = g_new(gid_t, ngroups);
  if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0) {
    groups = g_renew(gid_t, groups, ngroups);
    if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0) {
      g_free(groups);
      return FALSE;
    }
  }
  for (int i = 0; i < ngroups; i++)
    if (groups[i] == gr->gr_gid) { found = TRUE; break; }
  g_free(groups);
  return found;
}

static gboolean parse_uid(const gchar *text, guint64 *uid)
{
  const gchar *p;
  gchar *end = NULL;
  guint64 value;
  if (!text || !*text) return FALSE;
  for (p = text; *p; p++)
    if (!g_ascii_isdigit(*p)) return FALSE;
  errno = 0;
  value = g_ascii_strtoull(text, &end, 10);
  if (errno == ERANGE || !end || *end || value > G_MAXINT) return FALSE;
  *uid = value;
  return TRUE;
}

static void properties_changed(GDBusConnection *connection, const gchar *sender,
  const gchar *path, const gchar *interface, const gchar *signal,
  GVariant *parameters, gpointer user_data)
{
  RclAccountsService *self = RCL_ACCOUNTS_SERVICE(user_data);
  const gchar *changed_interface, *language;
  guint64 uid;
  GVariant *changed = NULL, *invalidated = NULL, *value = NULL;
  GError *error = NULL;
  if (!g_str_has_prefix(path, USER_PREFIX) ||
      g_strcmp0(interface, "org.freedesktop.DBus.Properties") ||
      g_strcmp0(signal, "PropertiesChanged"))
    return;
  if (!parse_uid(path + strlen(USER_PREFIX), &uid)) return;
  g_variant_get(parameters, "(&s@a{sv}@as)", &changed_interface, &changed, &invalidated);
  if (g_strcmp0(changed_interface, "org.freedesktop.Accounts.User")) goto out;
  value = g_variant_lookup_value(changed, "Language", G_VARIANT_TYPE_STRING);
  if (!value) goto out;
  language = g_variant_get_string(value, NULL);
  if (!*language || !g_utf8_validate(language, -1, NULL)) goto out;
  /* Signals have no original caller credentials. This group check applies
   * to the affected account and is not caller-based Polkit authorization. */
  if (!uid_in_privileged_group(uid)) {
    g_debug("ignoring AccountsService language change for uid %" G_GUINT64_FORMAT
            ": account is not in group '%s'", uid, PRIVILEGED_GROUP);
    goto out;
  }
  if (!rcl_locale_daemon_set_language_from_accounts(self->daemon, language, &error)) {
    g_warning("AccountsService locale bridge: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
  } else {
    g_debug("applied AccountsService language change");
  }
out:
  g_clear_pointer(&value, g_variant_unref);
  g_clear_pointer(&changed, g_variant_unref);
  g_clear_pointer(&invalidated, g_variant_unref);
}

static void name_appeared(GDBusConnection *connection, const gchar *name,
  const gchar *owner, gpointer user_data)
{
  RclAccountsService *self = RCL_ACCOUNTS_SERVICE(user_data);
  if (self->subscription_id) return;
  self->subscription_id = g_dbus_connection_signal_subscribe(connection,
    ACCOUNTS_NAME, "org.freedesktop.DBus.Properties", "PropertiesChanged",
    NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, properties_changed, self, NULL);
  g_debug("subscribed to AccountsService at %s", owner);
}

static void name_vanished(GDBusConnection *connection, const gchar *name,
  gpointer user_data)
{
  RclAccountsService *self = RCL_ACCOUNTS_SERVICE(user_data);
  if (self->subscription_id) {
    g_dbus_connection_signal_unsubscribe(connection, self->subscription_id);
    self->subscription_id = 0;
  }
  g_debug("AccountsService disappeared; waiting for it to return");
}

static gboolean rcl_accounts_service_connect(RclAccountsService *self);

static gboolean
rcl_accounts_service_reconnect_cb(gpointer user_data)
{
  RclAccountsService *self = RCL_ACCOUNTS_SERVICE(user_data);

  self->reconnect_id = 0;
  if (!self->stopping)
    rcl_accounts_service_connect(self);

  return G_SOURCE_REMOVE;
}

static void
rcl_accounts_service_connection_closed(GDBusConnection *connection,
                                       gboolean remote_peer_vanished,
                                       GError *error,
                                       gpointer user_data)
{
  RclAccountsService *self = RCL_ACCOUNTS_SERVICE(user_data);

  if (connection != self->connection || self->stopping)
    return;

  g_warning("system D-Bus connection for AccountsService closed: %s",
            error ? error->message : "connection closed");

  /* The closed connection has already invalidated both registrations. */
  self->watch_id = 0;
  self->subscription_id = 0;
  self->connection_closed_id = 0;
  g_clear_object(&self->connection);

  if (!self->reconnect_id)
    self->reconnect_id =
      g_timeout_add(RECONNECT_DELAY_MS,
                    rcl_accounts_service_reconnect_cb, self);
}

static gboolean
rcl_accounts_service_connect(RclAccountsService *self)
{
  GError *error = NULL;
  gchar *address;

  if (self->stopping || self->connection)
    return self->connection != NULL;

  /*
   * Use a private connection rather than g_bus_get_sync(). The latter caches
   * a shared connection, which may remain closed after dbus-daemon restarts.
   */
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
                     G_CALLBACK(rcl_accounts_service_connection_closed),
                     self);
  self->watch_id =
    g_bus_watch_name_on_connection(self->connection, ACCOUNTS_NAME,
                                   G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   name_appeared, name_vanished, self, NULL);
  g_debug("watching for AccountsService on the system bus");
  return TRUE;

failed:
  g_warning("could not connect to system D-Bus for AccountsService: %s",
            error ? error->message : "unknown error");
  g_clear_error(&error);

  if (!self->stopping && !self->reconnect_id)
    self->reconnect_id =
      g_timeout_add(RECONNECT_DELAY_MS,
                    rcl_accounts_service_reconnect_cb, self);
  return FALSE;
}

void
rcl_accounts_service_stop(RclAccountsService *self)
{
  g_return_if_fail(RCL_IS_ACCOUNTS_SERVICE(self));

  self->stopping = TRUE;

  if (self->reconnect_id) {
    g_source_remove(self->reconnect_id);
    self->reconnect_id = 0;
  }
  if (self->watch_id) {
    g_bus_unwatch_name(self->watch_id);
    self->watch_id = 0;
  }
  if (self->subscription_id && self->connection) {
    g_dbus_connection_signal_unsubscribe(self->connection,
                                         self->subscription_id);
    self->subscription_id = 0;
  }
  if (self->connection && self->connection_closed_id) {
    g_signal_handler_disconnect(self->connection,
                                self->connection_closed_id);
    self->connection_closed_id = 0;
  }
  g_clear_object(&self->connection);
}

gboolean
rcl_accounts_service_start(RclAccountsService *self, GError **error)
{
  g_return_val_if_fail(RCL_IS_ACCOUNTS_SERVICE(self), FALSE);
  (void) error;

  if (self->connection || self->reconnect_id)
    return TRUE;

  self->stopping = FALSE;
  return rcl_accounts_service_connect(self);
}

static void rcl_accounts_service_dispose(GObject *object)
{
  RclAccountsService *self = RCL_ACCOUNTS_SERVICE(object);
  rcl_accounts_service_stop(self);
  g_clear_object(&self->daemon);
  G_OBJECT_CLASS(rcl_accounts_service_parent_class)->dispose(object);
}
static void rcl_accounts_service_class_init(RclAccountsServiceClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = rcl_accounts_service_dispose;
}
static void rcl_accounts_service_init(RclAccountsService *self) { (void) self; }

RclAccountsService *rcl_accounts_service_new(RclLocaleDaemon *daemon)
{
  RclAccountsService *self;
  g_return_val_if_fail(RCL_IS_LOCALE_DAEMON(daemon), NULL);
  self = g_object_new(RCL_TYPE_ACCOUNTS_SERVICE, NULL);
  self->daemon = g_object_ref(daemon);
  return self;
}
