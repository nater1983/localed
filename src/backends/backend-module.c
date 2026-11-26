/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "backend-module.h"

gboolean
rcl_backend_module_validate(const RclBackendModuleDescriptor *descriptor,
                            const gchar *expected_id, GError **error)
{
  if (!descriptor ||
      descriptor->abi_version != RCL_BACKEND_MODULE_ABI_VERSION ||
      descriptor->struct_size < sizeof(RclBackendModuleDescriptor))
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "Unsupported desktop backend module ABI");
    return FALSE;
  }

  if (!expected_id || !*expected_id || !descriptor->id ||
      g_strcmp0(descriptor->id, expected_id) != 0 || !descriptor->create)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Invalid desktop backend module descriptor");
    return FALSE;
  }

  return TRUE;
}
