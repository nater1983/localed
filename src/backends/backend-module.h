/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <gmodule.h>
#include "backend-interface.h"
#include "rcl-locale.h"

G_BEGIN_DECLS

/* Internal, native ABI for trusted desktop modules shipped with localed.
 * Bump the ABI version for incompatible descriptor or host API changes.
 * Modules using static GTypes must remain resident once their factory
 * has been called, even if the factory fails or the instance is stopped.
 */
#define RCL_BACKEND_MODULE_ABI_VERSION 2u
#define RCL_BACKEND_MODULE_QUERY_SYMBOL "rcl_backend_module_query"

typedef struct {
  guint32 abi_version;
  guint32 struct_size;
  const gchar *id;
  RclBackend *(*create)(RclLocaleDaemon *daemon);
} RclBackendModuleDescriptor;

typedef const RclBackendModuleDescriptor *(*RclBackendModuleQuery)(void);

/* Each module exports this single query entry point. Its descriptor is
 * immutable and valid for the lifetime of the module. Querying must not
 * register GTypes or perform other persistent initialization.
 * The factory returns a full reference; the daemon is borrowed and must
 * outlive the backend. All lifecycle calls run on the daemon main context.
 */
G_MODULE_EXPORT const RclBackendModuleDescriptor *
rcl_backend_module_query(void);

gboolean rcl_backend_module_validate(
    const RclBackendModuleDescriptor *descriptor,
    const gchar *expected_id, GError **error);

/* Load only from an absolute, trusted installation path. The returned
 * backend owns a full reference; the module remains resident.
 * The caller must stop and unref the backend before destroying daemon. */
RclBackend *rcl_backend_module_load(const gchar *path,
                                    const gchar *expected_id,
                                    RclLocaleDaemon *daemon,
                                    GError **error);

G_END_DECLS
