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

const RclBackendModuleDescriptor *
rcl_backend_module_open(const gchar *path, const gchar *expected_id,
                       GError **error)
{
  GModule *module;
  gpointer symbol = NULL;
  RclBackendModuleQuery query;
  const RclBackendModuleDescriptor *descriptor;

  if (!path || !g_path_is_absolute(path) || !expected_id ||
      !*expected_id)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid desktop backend module arguments");
    return NULL;
  }

  if (!g_module_supported())
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "Dynamic desktop backend modules are not supported");
    return NULL;
  }

  module = g_module_open(path, G_MODULE_BIND_LOCAL);
  if (!module)
  {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Cannot load desktop backend module %s: %s",
                path, g_module_error());
    return NULL;
  }

  if (!g_module_symbol(module, RCL_BACKEND_MODULE_QUERY_SYMBOL, &symbol) ||
      !symbol)
  {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Missing desktop backend module entry point in %s: %s",
                path, g_module_error() ? g_module_error() : "NULL symbol");
    g_module_close(module);
    return NULL;
  }

  query = (RclBackendModuleQuery) symbol;
  descriptor = query();
  if (!rcl_backend_module_validate(descriptor, expected_id, error))
  {
    g_prefix_error(error, "%s: ", path);
    g_module_close(module);
    return NULL;
  }

  /* Factories may register static GTypes even when creation fails. */
  g_module_make_resident(module);
  g_module_close(module);
  return descriptor;
}

RclBackend *
rcl_backend_module_create(const RclBackendModuleDescriptor *descriptor,
                          RclLocaleDaemon *daemon, GError **error)
{
  RclBackend *backend;
  if (!descriptor || !RCL_IS_LOCALE_DAEMON(daemon) ||
      !rcl_backend_module_validate(descriptor, descriptor->id, error))
    return NULL;
  backend = descriptor->create(daemon);

  if (!backend || !RCL_IS_BACKEND(backend))
  {
    if (backend)
      g_object_unref(backend);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Desktop backend factory failed for %s", descriptor->id);
    return NULL;
  }

  return backend;
}

RclBackend *
rcl_backend_module_load(const gchar *path, const gchar *expected_id,
                        RclLocaleDaemon *daemon, GError **error)
{
  const RclBackendModuleDescriptor *descriptor;
  if (!RCL_IS_LOCALE_DAEMON(daemon))
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid desktop backend daemon");
    return NULL;
  }
  descriptor = rcl_backend_module_open(path, expected_id, error);
  return descriptor ? rcl_backend_module_create(descriptor, daemon, error) : NULL;
}
