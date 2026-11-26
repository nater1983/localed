/* Copyright (C) 2026 Nathaniel Russell; SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <gio/gio.h>
G_BEGIN_DECLS
#define RCL_TYPE_BACKEND (rcl_backend_get_type())
G_DECLARE_INTERFACE(RclBackend, rcl_backend, RCL, BACKEND, GObject)
struct _RclBackendInterface { GTypeInterface parent_iface; gboolean (*start)(RclBackend *, GError **); void (*stop)(RclBackend *); void (*set_write_enabled)(RclBackend *, gboolean); gboolean (*bind_session)(RclBackend *, guint, const gchar *, const gchar *, GError **); void (*refresh)(RclBackend *); };
gboolean rcl_backend_start(RclBackend *self, GError **error);
void rcl_backend_stop(RclBackend *self);
void rcl_backend_set_write_enabled(RclBackend *self, gboolean enabled);
/* Bind once before first start. The session identity is copied. */
gboolean rcl_backend_bind_session(RclBackend *self, guint uid,
                                  const gchar *id, const gchar *path,
                                  GError **error);
/* Request an immediate discovery/readiness report on the main context. */
void rcl_backend_refresh(RclBackend *self);
G_END_DECLS
