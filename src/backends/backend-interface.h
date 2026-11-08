/* Copyright (C) 2026 Nathaniel Russell; SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <gio/gio.h>
G_BEGIN_DECLS
#define RCL_TYPE_BACKEND (rcl_backend_get_type())
G_DECLARE_INTERFACE(RclBackend, rcl_backend, RCL, BACKEND, GObject)
struct _RclBackendInterface { GTypeInterface parent_iface; gboolean (*start)(RclBackend *, GError **); void (*stop)(RclBackend *); };
gboolean rcl_backend_start(RclBackend *self, GError **error);
void rcl_backend_stop(RclBackend *self);
G_END_DECLS
