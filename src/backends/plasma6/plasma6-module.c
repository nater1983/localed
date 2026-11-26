/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "backends/backend-module.h"
#include "plasma6-backend.h"

G_MODULE_EXPORT const RclBackendModuleDescriptor *
rcl_backend_module_query(void)
{
  static const RclBackendModuleDescriptor descriptor = {
    .abi_version = RCL_BACKEND_MODULE_ABI_VERSION,
    .struct_size = sizeof(RclBackendModuleDescriptor),
    .id = "plasma6",
    .create = rcl_plasma6_backend_new,
  };
  return &descriptor;
}
