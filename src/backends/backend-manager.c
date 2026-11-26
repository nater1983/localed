/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "backend-manager.h"

/* A session claim is separate from the long-lived desktop monitor.
 * The monitor is borrowed from backends; identity strings are owned. */
typedef struct {
  RclBackend *monitor;
  RclBackend *instance; /* owned, bound to this identity */
  gboolean ready;
  gboolean active; /* reported activity, independent of instance readiness */
  gchar *module_id;
  gchar *id;
  gchar *path;
  guint uid;
} RclSessionClaim;

static void
session_claim_free(gpointer data)
{
  RclSessionClaim *claim = data;
  if (claim->instance)
  {
    rcl_backend_stop(claim->instance);
    g_object_unref(claim->instance);
  }
  g_free(claim->module_id);
  g_free(claim->id);
  g_free(claim->path);
  g_free(claim);
}

struct _RclBackendManager {
  RclLocaleDaemon *daemon; /* borrowed */
  GHashTable *modules;   /* owned identifiers, borrowed resident descriptors */
  GHashTable *backends;  /* owned identifiers and monitor instances */
  gboolean started;
  gboolean reconciling;
  gboolean invalidated_during_reconcile;
  GHashTable *sessions; /* owned composite keys and session claims */
  GHashTable *reported; /* borrowed monitors with valid discovery reports */
  RclBackend *owner; /* borrowed */
  guint reconcile_id;
};

RclBackendManager *
rcl_backend_manager_new(RclLocaleDaemon *daemon)
{
  RclBackendManager *self;
  g_return_val_if_fail(RCL_IS_LOCALE_DAEMON(daemon), NULL);
  self = g_new0(RclBackendManager, 1);
  self->daemon = daemon;
  self->modules = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->backends = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, g_object_unref);
  self->sessions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, session_claim_free);
  self->reported = g_hash_table_new(g_direct_hash, g_direct_equal);
  return self;
}

static void
set_owner(RclBackendManager *self, RclBackend *owner)
{
  if (self->owner == owner)
    return;
  if (self->owner)
    rcl_backend_set_write_enabled(self->owner, FALSE);
  self->owner = owner;
  if (owner)
    rcl_backend_set_write_enabled(owner, TRUE);
  g_debug("desktop keyboard write owner: %s",
          owner ? G_OBJECT_TYPE_NAME(owner) : "none");
}

static gboolean reconcile_cb(gpointer data);

static void
schedule_reconcile(RclBackendManager *self)
{
  if (self->started && !self->reconciling && !self->reconcile_id)
    self->reconcile_id = g_timeout_add(250, reconcile_cb, self);
}

static void
instance_session_changed(RclBackend *backend, gboolean active, guint uid,
                         const gchar *id, const gchar *path, const gchar *seat,
                         const gchar *type, const gchar *desktop, gpointer data)
{
  RclBackendManager *self = data;
  GHashTableIter iter;
  gpointer value;
  (void) seat;
  (void) type;
  (void) desktop;

  if (!self->started)
    return;
  g_hash_table_iter_init(&iter, self->sessions);
  while (g_hash_table_iter_next(&iter, NULL, &value))
  {
    RclSessionClaim *claim = value;
    if (claim->instance != backend)
      continue;
    claim->ready = active && uid == claim->uid &&
                   g_strcmp0(id, claim->id) == 0 &&
                   g_strcmp0(path, claim->path) == 0;
    if (!claim->ready && self->owner == backend)
      set_owner(self, NULL);
    schedule_reconcile(self);
    return;
  }
}

static gboolean
ensure_session_instance(RclBackendManager *self, RclSessionClaim *claim)
{
  const RclBackendModuleDescriptor *descriptor;
  g_autoptr(GError) error = NULL;
  RclBackend *instance;

  if (claim->instance)
    return TRUE;
  descriptor = g_hash_table_lookup(self->modules, claim->module_id);
  if (!descriptor)
    return FALSE;
  instance = rcl_backend_module_create(descriptor, self->daemon, &error);
  if (!instance || !rcl_backend_bind_session(instance, claim->uid, claim->id,
                                            claim->path, &error))
  {
    g_warning("could not create session backend %s: %s", claim->path,
              error ? error->message : "unknown error");
    g_clear_object(&instance);
    return FALSE;
  }
  claim->instance = instance;
  g_signal_connect(instance, "active-session-changed",
                   G_CALLBACK(instance_session_changed), self);
  if (!rcl_backend_start(instance, &error))
    g_debug("session backend %s initial connection pending: %s", claim->path,
            error ? error->message : "monitor reconnect pending");
  g_debug("created %s backend instance for session %s uid=%u",
          claim->module_id, claim->id, claim->uid);
  return TRUE;
}

static void
sessions_invalidated(RclBackend *backend, gpointer data)
{
  RclBackendManager *self = data;
  (void) backend;

  if (!self->started)
    return;
  /* Invalidate the whole discovery round, without deleting instances. */
  set_owner(self, NULL);
  g_hash_table_remove_all(self->reported);
  if (self->reconciling)
    self->invalidated_during_reconcile = TRUE;
  else
    schedule_reconcile(self);
}

static gboolean
reconcile_cb(gpointer data)
{
  RclBackendManager *self = data;
  GHashTableIter iter;
  gpointer value;
  RclSessionClaim *candidate = NULL;
  guint active_count = 0;

  self->reconcile_id = 0;
  if (!self->started)
    return G_SOURCE_REMOVE;

  /* A debounce only coalesces events; it cannot certify that the
   * independent discovery reports are current. Refresh every configured
   * monitor in this round, with writes disabled until it completes. */
  self->reconciling = TRUE;
  self->invalidated_during_reconcile = FALSE;
  set_owner(self, NULL);
  g_hash_table_remove_all(self->reported);
  g_hash_table_iter_init(&iter, self->backends);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    rcl_backend_refresh(value);

  g_hash_table_iter_init(&iter, self->sessions);
  while (g_hash_table_iter_next(&iter, NULL, &value))
  {
    RclSessionClaim *claim = value;
    ensure_session_instance(self, claim);
    if (claim->active)
    {
      active_count++;
      candidate = claim;
    }
  }

  if (active_count == 1 &&
      !self->invalidated_during_reconcile &&
      g_hash_table_size(self->reported) == g_hash_table_size(self->backends) &&
      g_hash_table_size(self->backends) == (HAVE_GNOME_BACKEND + HAVE_PLASMA6_BACKEND) &&
      candidate->instance)
  {
    /* A readiness report from a previous transition cannot grant writes. */
    candidate->ready = FALSE;
    rcl_backend_refresh(candidate->instance);
    if (candidate->ready && !self->invalidated_during_reconcile)
      set_owner(self, candidate->instance);
  }

  self->reconciling = FALSE;
  if (self->invalidated_during_reconcile)
    schedule_reconcile(self);
  return G_SOURCE_REMOVE;
}

static void
sessions_changed(RclBackend *backend, GVariant *snapshot, gpointer data)
{
  RclBackendManager *self = data;
  GHashTableIter iter;
  GVariantIter entries;
  gpointer key, value;
  const gchar *module_id = NULL;
  const gchar *id, *path;
  guint uid;
  gboolean changed = FALSE;
  gboolean valid, active;
  g_autoptr(GHashTable) seen =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  if (!self->started)
    return;

  g_hash_table_iter_init(&iter, self->backends);
  while (g_hash_table_iter_next(&iter, &key, &value))
    if (value == backend)
    {
      module_id = key;
      break;
    }
  if (!module_id)
    return;

  /* Validate the entire batch before altering any claims. NULL indicates
   * discovery failure, not a successful report of zero sessions. */
  valid = snapshot && g_variant_is_of_type(snapshot, G_VARIANT_TYPE("a(usob)"));
  if (valid)
  {
    g_variant_iter_init(&entries, snapshot);
    while (g_variant_iter_next(&entries, "(u&s&ob)", &uid, &id, &path, &active))
    {
      if (!*id || !g_str_has_prefix(path, "/org/freedesktop/login1/session/") ||
          !g_hash_table_add(seen, g_strdup(path)))
      {
        valid = FALSE;
        break;
      }
    }
    g_hash_table_remove_all(seen);
  }
  if (!valid)
  {
    g_hash_table_remove(self->reported, backend);
    set_owner(self, NULL);
  }
  else
    g_hash_table_add(self->reported, backend);

  if (valid)
  {
    g_variant_iter_init(&entries, snapshot);
    while (g_variant_iter_next(&entries, "(u&s&ob)", &uid, &id, &path, &active))
    {
      g_autofree gchar *session_key = NULL;
      RclSessionClaim *claim;

      session_key = g_strdup_printf("%s:%s", module_id, path);
      g_hash_table_add(seen, g_strdup(session_key));
      claim = g_hash_table_lookup(self->sessions, session_key);

      if (claim && claim->uid == uid && g_strcmp0(claim->id, id) == 0)
      {
        if (claim->active != active)
        {
          set_owner(self, NULL);
          changed = TRUE;
          claim->active = active;
        }
        continue;
      }

      set_owner(self, NULL);
      changed = TRUE;
      if (claim && claim->instance)
        g_signal_handlers_disconnect_by_data(claim->instance, self);

      claim = g_new0(RclSessionClaim, 1);
      claim->monitor = backend;
      claim->module_id = g_strdup(module_id);
      claim->id = g_strdup(id);
      claim->path = g_strdup(path);
      claim->uid = uid;
      claim->active = active;
      g_hash_table_replace(self->sessions, g_steal_pointer(&session_key), claim);
    }
  }

  /* Preserve unchanged instances, removing only claims absent from
   * this monitor's latest complete report. */
  g_hash_table_iter_init(&iter, self->sessions);
  while (g_hash_table_iter_next(&iter, &key, &value))
  {
    RclSessionClaim *claim = value;
    if (claim->monitor != backend || g_hash_table_contains(seen, key))
      continue;
    set_owner(self, NULL);
    changed = TRUE;
    if (claim->instance)
      g_signal_handlers_disconnect_by_data(claim->instance, self);
    g_hash_table_iter_remove(&iter);
  }

  if (changed || !self->owner)
    schedule_reconcile(self);
}

gboolean
rcl_backend_manager_load(RclBackendManager *self, const gchar *id,
                         GError **error)
{
  const gchar *filename;
  const RclBackendModuleDescriptor *descriptor;
  RclBackend *backend;
  g_autofree gchar *path = NULL;

  g_return_val_if_fail(self != NULL, FALSE);
  if (self->started)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "Cannot load modules after backend startup");
    return FALSE;
  }

  /* Explicit allowlist: never construct a privileged module path from
   * arbitrary desktop names or environment variables. */
  if (g_strcmp0(id, "gnome") == 0)
    filename = "gnome.so";
  else if (g_strcmp0(id, "plasma6") == 0)
    filename = "plasma.so";
  else
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Unknown desktop backend identifier");
    return FALSE;
  }

  path = g_build_filename(RCL_BACKEND_MODULE_DIR, filename, NULL);
  if (g_hash_table_contains(self->modules, id))
  {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                "Desktop backend %s is already loaded", id);
    return FALSE;
  }
  descriptor = rcl_backend_module_open(path, id, error);
  if (!descriptor)
    return FALSE;
  backend = rcl_backend_module_create(descriptor, self->daemon, error);
  if (!backend)
    return FALSE;
  if (!g_signal_lookup("active-session-changed", G_OBJECT_TYPE(backend)) ||
      !g_signal_lookup("sessions-changed", G_OBJECT_TYPE(backend)) ||
      !g_signal_lookup("sessions-invalidated", G_OBJECT_TYPE(backend)) ||
      !RCL_BACKEND_GET_IFACE(backend)->refresh)
  {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Desktop backend %s does not report active sessions", id);
    g_object_unref(backend);
    return FALSE;
  }
  g_signal_connect(backend, "sessions-changed",
                   G_CALLBACK(sessions_changed), self);
  g_signal_connect(backend, "sessions-invalidated",
                   G_CALLBACK(sessions_invalidated), self);
  g_hash_table_insert(self->modules, g_strdup(id), (gpointer) descriptor);
  g_hash_table_insert(self->backends, g_strdup(id), backend);
  return TRUE;
}

gboolean
rcl_backend_manager_start(RclBackendManager *self, GError **error)
{
  GHashTableIter iter;
  gpointer backend;
  gboolean ok = TRUE;

  g_return_val_if_fail(self != NULL, FALSE);
  if (self->started)
    return TRUE;
  self->started = TRUE;
  g_hash_table_iter_init(&iter, self->backends);
  while (g_hash_table_iter_next(&iter, NULL, &backend))
  {
    g_autoptr(GError) local_error = NULL;
    if (!rcl_backend_start(backend, &local_error))
    {
      ok = FALSE;
      if (error && !*error)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Could not start %s: %s", G_OBJECT_TYPE_NAME(backend),
                    local_error ? local_error->message : "monitor reconnect pending");
    }
  }
  return ok;
}

void
rcl_backend_manager_stop(RclBackendManager *self)
{
  if (!self)
    return;
  GHashTableIter iter;
  gpointer backend;
  self->started = FALSE;
  if (self->reconcile_id)
  {
    g_source_remove(self->reconcile_id);
    self->reconcile_id = 0;
  }
  set_owner(self, NULL);
  g_hash_table_iter_init(&iter, self->sessions);
  while (g_hash_table_iter_next(&iter, NULL, &backend))
  {
    RclSessionClaim *claim = backend;
    if (claim->instance)
      g_signal_handlers_disconnect_by_data(claim->instance, self);
  }
  g_hash_table_remove_all(self->sessions);
  g_hash_table_remove_all(self->reported);
  g_hash_table_iter_init(&iter, self->backends);
  while (g_hash_table_iter_next(&iter, NULL, &backend))
    rcl_backend_stop(backend);
}

void
rcl_backend_manager_free(RclBackendManager *self)
{
  if (!self)
    return;
  rcl_backend_manager_stop(self);
  GHashTableIter iter;
  gpointer backend;
  g_hash_table_iter_init(&iter, self->backends);
  while (g_hash_table_iter_next(&iter, NULL, &backend))
    g_signal_handlers_disconnect_by_data(backend, self);
  g_hash_table_unref(self->sessions);
  g_hash_table_unref(self->reported);
  g_hash_table_unref(self->backends);
  g_hash_table_unref(self->modules);
  g_free(self);
}
