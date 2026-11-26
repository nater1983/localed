/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "backend-manager.h"

struct _RclBackendManager {
  RclLocaleDaemon *daemon; /* borrowed */
  RclBackend *backend;    /* owned */
  gboolean started;
};

RclBackendManager *
rcl_backend_manager_new(RclLocaleDaemon *daemon)
{
  RclBackendManager *self;
  g_return_val_if_fail(RCL_IS_LOCALE_DAEMON(daemon), NULL);
  self = g_new0(RclBackendManager, 1);
  self->daemon = daemon;
  return self;
}

static gboolean
can_attach(RclBackendManager *self, GError **error)
{
  if (self->backend || self->started)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "Only one desktop backend is currently supported");
    return FALSE;
  }
  return TRUE;
}

gboolean
rcl_backend_manager_load(RclBackendManager *self, const gchar *id,
                         GError **error)
{
  const gchar *filename;
  g_autofree gchar *path = NULL;

  g_return_val_if_fail(self != NULL, FALSE);
  if (!can_attach(self, error))
    return FALSE;

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
  self->backend = rcl_backend_module_load(path, id, self->daemon, error);
  return self->backend != NULL;
}

gboolean
rcl_backend_manager_start(RclBackendManager *self, GError **error)
{
  g_return_val_if_fail(self != NULL, FALSE);
  if (!self->backend || self->started)
    return TRUE;

  /* Current backends may schedule recovery even when start returns FALSE.
   * Always stop them on shutdown; allow subsequent start calls to retry. */
  self->started = rcl_backend_start(self->backend, error);
  return self->started;
}

void
rcl_backend_manager_stop(RclBackendManager *self)
{
  if (!self)
    return;
  if (self->backend)
    rcl_backend_stop(self->backend);
  self->started = FALSE;
}

void
rcl_backend_manager_free(RclBackendManager *self)
{
  if (!self)
    return;
  rcl_backend_manager_stop(self);
  g_clear_object(&self->backend);
  g_free(self);
}
