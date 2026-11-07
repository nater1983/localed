/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rcl-file-transaction.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>

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
  fd = g_mkstemp( tmp_path );
  if( fd == -1 )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 "create rollback temporary file for %s: %s",
                 snapshot->path, g_strerror(errno) );
    g_free( tmp_path );
    return FALSE;
  }
  close( fd );

  if( !g_file_set_contents( tmp_path, snapshot->contents,
                            (gssize)snapshot->length, error ) )
  {
    g_prefix_error( error, "restore %s: ", snapshot->path );
    goto out;
  }

  if( g_chmod( tmp_path, snapshot->mode ) != 0 )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 "restore permissions on temporary file for %s: %s",
                 snapshot->path, g_strerror(errno) );
    goto out;
  }

  if( g_rename( tmp_path, snapshot->path ) != 0 )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 "rollback rename(%s, %s): %s",
                 tmp_path, snapshot->path, g_strerror(errno) );
    goto out;
  }

  ok = TRUE;

out:
  if( !ok )
    g_unlink( tmp_path );
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
    gchar *message = g_strdup_printf( "%s (rollback also failed: %s)",
                                      (*error)->message,
                                      rollback_error->message );
    g_clear_error( error );
    g_set_error_literal( error, G_IO_ERROR, G_IO_ERROR_FAILED, message );
    g_free( message );
    g_error_free( rollback_error );
  }
  else if( error )
    g_propagate_error( error, rollback_error );
  else
    g_error_free( rollback_error );
}
