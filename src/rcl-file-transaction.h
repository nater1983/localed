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
  uid_t    uid;
  gid_t    gid;
  gboolean existed;
} RclFileSnapshot;

/* Close a staged temporary file, checking delayed errors and removing its
 * pathname on failure without discarding the close error. */
gboolean rcl_file_close_temporary ( int fd,
                                    const gchar *temporary_path,
                                    const gchar *destination_path,
                                    GError **error );

void rcl_file_snapshot_clear ( RclFileSnapshot *snapshot );

gboolean rcl_file_snapshot_take ( RclFileSnapshot *snapshot,
                                  const gchar *path,
                                  GError **error );

gboolean rcl_file_snapshot_restore ( const RclFileSnapshot *snapshot,
                                     GError **error );

void rcl_report_rollback_failure ( GError **error,
                                   GError *rollback_error );

#ifdef RCL_FILE_TRANSACTION_TESTING
typedef struct { int (*mkstemp_fn)(gchar*); int (*close_fn)(int); gboolean (*write_fn)(const gchar*,const gchar*,gssize,GError**); int (*chown_fn)(const gchar*,uid_t,gid_t); int (*chmod_fn)(const gchar*,mode_t); int (*rename_fn)(const gchar*,const gchar*); int (*unlink_fn)(const gchar*); } RclFileTransactionTestOps;
void rcl_file_transaction_set_test_ops(const RclFileTransactionTestOps *ops);
#endif

G_END_DECLS

#endif /* RCL_FILE_TRANSACTION_H */
