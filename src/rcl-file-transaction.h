/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef RCL_FILE_TRANSACTION_H
#define RCL_FILE_TRANSACTION_H

#include <gio/gio.h>
#include <glib.h>
#include <sys/types.h>

G_BEGIN_DECLS

/*
 * Snapshot one configuration file before a multi-file update.  A snapshot
 * records whether the file existed, its exact contents, and its permission
 * bits so a later write failure can restore the previous state.
 */
typedef struct
{
  gchar   *path;
  gchar   *contents;
  gsize    length;
  mode_t   mode;
  gboolean existed;
} RclFileSnapshot;

void rcl_file_snapshot_clear ( RclFileSnapshot *snapshot );

gboolean rcl_file_snapshot_take ( RclFileSnapshot *snapshot,
                                  const gchar *path,
                                  GError **error );

gboolean rcl_file_snapshot_restore ( const RclFileSnapshot *snapshot,
                                     GError **error );

void rcl_report_rollback_failure ( GError **error,
                                   GError *rollback_error );

G_END_DECLS

#endif /* RCL_FILE_TRANSACTION_H */
