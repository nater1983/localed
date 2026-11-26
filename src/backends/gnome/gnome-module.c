/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "backends/backend-module.h"
#include "gnome-backend.h"

G_MODULE_EXPORT const RclBackendModuleDescriptor *
rcl_backend_module_query(void)
{
  static const RclBackendModuleDescriptor descriptor = {
    .abi_version = RCL_BACKEND_MODULE_ABI_VERSION,
    .struct_size = sizeof(RclBackendModuleDescriptor),
    .id = "gnome",
    .create = rcl_gnome_backend_new,
  };
  return &descriptor;
}
