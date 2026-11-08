/* Copyright (C) 2026 Nathaniel Russell; SPDX-License-Identifier: GPL-2.0-or-later */
#include "backend-interface.h"
G_DEFINE_INTERFACE(RclBackend, rcl_backend, G_TYPE_OBJECT)
static void rcl_backend_default_init(RclBackendInterface *iface) { (void) iface; }
gboolean rcl_backend_start(RclBackend *self, GError **error) {
  RclBackendInterface *iface;
  g_return_val_if_fail(RCL_IS_BACKEND(self), FALSE);
  iface = RCL_BACKEND_GET_IFACE(self);
  g_return_val_if_fail(iface->start != NULL, FALSE);
  return iface->start(self, error);
}
void rcl_backend_stop(RclBackend *self) {
  RclBackendInterface *iface;
  g_return_if_fail(RCL_IS_BACKEND(self));
  iface = RCL_BACKEND_GET_IFACE(self);
  if (iface->stop) iface->stop(self);
}
