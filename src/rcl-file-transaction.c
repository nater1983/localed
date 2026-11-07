/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rcl-file-transaction.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>
#ifdef RCL_FILE_TRANSACTION_TESTING
static RclFileTransactionTestOps test_ops;
void rcl_file_transaction_set_test_ops(const RclFileTransactionTestOps *ops){if(ops)test_ops=*ops;else memset(&test_ops,0,sizeof(test_ops));}
#define TM(p) (test_ops.mkstemp_fn?test_ops.mkstemp_fn(p):g_mkstemp(p))
#define TC(fd) (test_ops.close_fn?test_ops.close_fn(fd):close(fd))
#define TW(p,d,n,e) (test_ops.write_fn?test_ops.write_fn(p,d,n,e):g_file_set_contents(p,d,n,e))
#define TO(p,u,g) (test_ops.chown_fn?test_ops.chown_fn(p,u,g):chown(p,u,g))
#define TP(p,m) (test_ops.chmod_fn?test_ops.chmod_fn(p,m):g_chmod(p,m))
#define TR(a,b) (test_ops.rename_fn?test_ops.rename_fn(a,b):g_rename(a,b))
#define TU(p) (test_ops.unlink_fn?test_ops.unlink_fn(p):g_unlink(p))
#else
#define TM(p) g_mkstemp(p)
#define TC(fd) close(fd)
#define TW(p,d,n,e) g_file_set_contents(p,d,n,e)
#define TO(p,u,g) chown(p,u,g)
#define TP(p,m) g_chmod(p,m)
#define TR(a,b) g_rename(a,b)
#define TU(p) g_unlink(p)
#endif

/*
 * Remove a staged file without hiding a prior failure.  A failed unlink is
 * reported alongside the original error because leaving a named temporary
 * file behind is itself an operational failure.
 */
void
rcl_file_cleanup_temporary ( const gchar *path, GError **error )
{
  GError *cleanup_error = NULL;

  if( TU( path ) == 0 || errno == ENOENT )
    return;

  g_set_error( &cleanup_error, G_IO_ERROR, g_io_error_from_errno(errno),
               "remove temporary file %s: %s", path, g_strerror(errno) );

  if( error && *error )
  {
    GQuark domain = (*error)->domain;
    gint code = (*error)->code;
    gchar *message = g_strdup_printf(
      "%s (temporary-file cleanup also failed: %s)",
      (*error)->message, cleanup_error->message );
    g_clear_error( error );
    g_set_error_literal( error, domain, code, message );
    g_free( message );
    g_error_free( cleanup_error );
  }
  else if( error )
    g_propagate_error( error, cleanup_error );
  else
  {
    g_warning( "%s", cleanup_error->message );
    g_error_free( cleanup_error );
  }
}

gboolean
rcl_file_close_temporary ( int fd, const gchar *temporary_path,
                           const gchar *destination_path, GError **error )
{
  int saved_errno;

  g_return_val_if_fail( temporary_path != NULL, FALSE );
  g_return_val_if_fail( destination_path != NULL, FALSE );

  /*
   * On Linux, close() releases the descriptor even when it reports EINTR.
   * Never retry it: the descriptor number may already have been reused.
   */
  if( TC( fd ) == 0 )
    return TRUE;

  saved_errno = errno;
  {
    GError *close_error = NULL;

    g_set_error( &close_error, G_IO_ERROR,
                 g_io_error_from_errno(saved_errno),
                 "close temporary file for %s: %s",
                 destination_path, g_strerror(saved_errno) );

    /*
     * Keep the close failure even when the caller does not request an
     * error object.  Cleanup may append a secondary failure to this error.
     */
    rcl_file_cleanup_temporary( temporary_path, &close_error );

    if( error )
      g_propagate_error( error, close_error );
    else
    {
      g_warning( "%s", close_error->message );
      g_error_free( close_error );
    }
  }

  return FALSE;
}

void
rcl_file_snapshot_clear ( RclFileSnapshot *snapshot )
{
  if( !snapshot )
    return;

  g_free( snapshot->path );
  g_free( snapshot->contents );
  memset( snapshot, 0, sizeof(*snapshot) );
}

gboolean
rcl_file_snapshot_take ( RclFileSnapshot *snapshot,
                         const gchar *path,
                         GError **error )
{
  GError *local_error = NULL;
  GStatBuf st;

  g_return_val_if_fail( snapshot != NULL, FALSE );
  g_return_val_if_fail( path != NULL, FALSE );

  memset( snapshot, 0, sizeof(*snapshot) );
  snapshot->path = g_strdup( path );

  if( !g_file_get_contents( path, &snapshot->contents,
                            &snapshot->length, &local_error ) )
  {
    if( g_error_matches( local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT ) )
    {
      g_clear_error( &local_error );
      snapshot->existed = FALSE;
      return TRUE;
    }

    g_prefix_error( &local_error, "snapshot %s: ", path );
    g_propagate_error( error, local_error );
    rcl_file_snapshot_clear( snapshot );
    return FALSE;
  }

  if( g_stat( path, &st ) != 0 )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 "stat %s: %s", path, g_strerror(errno) );
    rcl_file_snapshot_clear( snapshot );
    return FALSE;
  }

  snapshot->mode = st.st_mode & 07777;
  snapshot->uid = st.st_uid;
  snapshot->gid = st.st_gid;
  snapshot->existed = TRUE;
  return TRUE;
}

gboolean
rcl_file_snapshot_restore ( const RclFileSnapshot *snapshot,
                            GError **error )
{
  gchar *tmp_path;
  int fd;
  gboolean ok = FALSE;

  g_return_val_if_fail( snapshot != NULL, FALSE );
  g_return_val_if_fail( snapshot->path != NULL, FALSE );

  if( !snapshot->existed )
  {
    if( g_unlink( snapshot->path ) != 0 && errno != ENOENT )
    {
      g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                   "remove %s during rollback: %s",
                   snapshot->path, g_strerror(errno) );
      return FALSE;
    }
    return TRUE;
  }

  /*
   * Restore through a sibling temporary file.  Never write directly to the
   * destination: readers must see either the failed replacement or the
   * complete restored file, never truncated intermediate contents.
   */
  tmp_path = g_strdup_printf( "%s.XXXXXX", snapshot->path );
  fd = TM( tmp_path );
  if( fd == -1 )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 "create rollback temporary file for %s: %s",
                 snapshot->path, g_strerror(errno) );
    g_free( tmp_path );
    return FALSE;
  }
  /*
   * close() can report delayed write/metadata errors.  Do not retry close()
   * after EINTR: on Linux the descriptor has already been released.
   */
  if( TC( fd ) != 0 )
  {
    int saved_errno = errno;
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                 "close rollback temporary file %s: %s",
                 tmp_path, g_strerror(saved_errno) );
    rcl_file_cleanup_temporary( tmp_path, error );
    g_free( tmp_path );
    return FALSE;
  }

  if( !TW( tmp_path, snapshot->contents,
             (gssize)snapshot->length, error ) )
  {
    g_prefix_error( error, "restore %s: ", snapshot->path );
    goto out;
  }

  /* chown may clear setuid/setgid bits: restore ownership before mode. */
  if( TO( tmp_path, snapshot->uid, snapshot->gid ) != 0 )
  {
    int saved_errno = errno;
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                 "restore ownership on temporary file for %s: %s", snapshot->path, g_strerror(saved_errno) );
    goto out;
  }
  if( TP( tmp_path, snapshot->mode ) != 0 )
  {
    int saved_errno = errno;
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                 "restore permissions on temporary file for %s: %s", snapshot->path, g_strerror(saved_errno) );
    goto out;
  }
  if( TR( tmp_path, snapshot->path ) != 0 )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 "rollback rename(%s, %s): %s",
                 tmp_path, snapshot->path, g_strerror(errno) );
    goto out;
  }

  ok = TRUE;

out:
  if( !ok )
    rcl_file_cleanup_temporary( tmp_path, error );
  g_free( tmp_path );
  return ok;
}

void
rcl_report_rollback_failure ( GError **error, GError *rollback_error )
{
  if( !rollback_error )
    return;

  if( error && *error )
  {
    GQuark domain = (*error)->domain;
    gint code = (*error)->code;
    gchar *message = g_strdup_printf( "%s (rollback also failed: %s)", (*error)->message, rollback_error->message );
    g_clear_error( error );
    g_set_error_literal( error, domain, code, message );
    g_free( message );
    g_error_free( rollback_error );
  }
  else if( error )
    g_propagate_error( error, rollback_error );
  else
  {
    g_warning( "%s", rollback_error->message );
    g_error_free( rollback_error );
  }
}
