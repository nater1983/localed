/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "backend-manager.h"

struct _RclBackendManager {
  RclLocaleDaemon *daemon; /* borrowed */
  GHashTable *backends;   /* owned identifiers and instances */
  gboolean started;
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
  return self;
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
  g_hash_table_insert(self->backends, g_strdup(id), backend);
  return TRUE;
}

gboolean
rcl_backend_manager_start(RclBackendManager *self, GError **error)
{
  g_return_val_if_fail(self != NULL, FALSE);
  GHashTableIter iter;
  gpointer backend;

  if (self->started)
    return TRUE;

#if HAVE_GNOME_BACKEND && HAVE_PLASMA6_BACKEND
  /* Fail closed in dual builds even if only one module loaded.
   * Session arbitration must precede activation. */
  return TRUE;
#endif
  if (g_hash_table_size(self->backends) == 0)
    return TRUE;
  if (g_hash_table_size(self->backends) != 1)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "Multiple backend activation requires session arbitration");
    return FALSE;
  }
  g_hash_table_iter_init(&iter, self->backends);
  g_hash_table_iter_next(&iter, NULL, &backend);
  self->started = rcl_backend_start(backend, error);
  return self->started;
}

void
rcl_backend_manager_stop(RclBackendManager *self)
{
  if (!self)
    return;
  GHashTableIter iter;
  gpointer backend;
  g_hash_table_iter_init(&iter, self->backends);
  while (g_hash_table_iter_next(&iter, NULL, &backend))
    rcl_backend_stop(backend);
  self->started = FALSE;
}

void
rcl_backend_manager_free(RclBackendManager *self)
{
  if (!self)
    return;
  rcl_backend_manager_stop(self);
  g_hash_table_unref(self->backends);
  g_free(self);
}
