/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "backend-module.h"

G_BEGIN_DECLS
typedef struct _RclBackendManager RclBackendManager;

/* Main-context-owned manager. The daemon must outlive the manager.
 * Until session arbitration is implemented, only one backend is allowed. */
RclBackendManager *rcl_backend_manager_new(RclLocaleDaemon *daemon);
gboolean rcl_backend_manager_load(RclBackendManager *self,
                                  const gchar *id, GError **error);
/* Transitional adapter: takes its own reference to a built-in backend. */
gboolean rcl_backend_manager_set_builtin(RclBackendManager *self,
                                         RclBackend *backend,
                                         GError **error);
gboolean rcl_backend_manager_start(RclBackendManager *self, GError **error);
void rcl_backend_manager_stop(RclBackendManager *self);
void rcl_backend_manager_free(RclBackendManager *self);
G_END_DECLS
