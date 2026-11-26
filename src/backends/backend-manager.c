/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "backend-manager.h"

struct _RclBackendManager {
  RclLocaleDaemon *daemon; /* borrowed */
  GHashTable *backends;   /* owned identifiers and instances */
  gboolean started;
  GHashTable *candidates; /* borrowed backend keys, owned session paths */
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
  self->backends = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, g_object_unref);
  self->candidates = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
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

static gboolean
reconcile_cb(gpointer data)
{
  RclBackendManager *self = data;
  GHashTableIter iter;
  gpointer backend;

  self->reconcile_id = 0;
  if (!self->started || g_hash_table_size(self->candidates) != 1)
  {
    set_owner(self, NULL);
    return G_SOURCE_REMOVE;
  }
  g_hash_table_iter_init(&iter, self->candidates);
  g_hash_table_iter_next(&iter, &backend, NULL);
  set_owner(self, backend);
  return G_SOURCE_REMOVE;
}

static void
active_session_changed(RclBackend *backend, gboolean active, guint uid,
                       const gchar *id, const gchar *path, const gchar *seat,
                       const gchar *type, const gchar *desktop, gpointer data)
{
  RclBackendManager *self = data;
  (void) uid;
  (void) id;
  (void) seat;
  (void) type;
  (void) desktop;

  if (!self->started)
    return;

  /* Revoke immediately, then coalesce reports from both monitors.
   * No candidate may write until ownership is reconciled. */
  set_owner(self, NULL);
  if (active && path && *path)
    g_hash_table_replace(self->candidates, backend, g_strdup(path));
  else
    g_hash_table_remove(self->candidates, backend);

  if (!self->reconcile_id)
    self->reconcile_id = g_timeout_add(250, reconcile_cb, self);
}

gboolean
rcl_backend_manager_load(RclBackendManager *self, const gchar *id,
                         GError **error)
{
  const gchar *filename;
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
  if (g_hash_table_contains(self->backends, id))
  {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                "Desktop backend %s is already loaded", id);
    return FALSE;
  }
  backend = rcl_backend_module_load(path, id, self->daemon, error);
  if (!backend)
    return FALSE;
  if (!g_signal_lookup("active-session-changed", G_OBJECT_TYPE(backend)))
  {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Desktop backend %s does not report active sessions", id);
    g_object_unref(backend);
    return FALSE;
  }
  g_signal_connect(backend, "active-session-changed",
                   G_CALLBACK(active_session_changed), self);
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
  g_hash_table_remove_all(self->candidates);
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
  g_hash_table_unref(self->candidates);
  g_hash_table_unref(self->backends);
  g_free(self);
}
