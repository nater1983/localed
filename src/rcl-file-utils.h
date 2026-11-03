/*
 * Copyright (C) 2026 Nathaniel Russell
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef RCL_FILE_UTILS_H
#define RCL_FILE_UTILS_H

#include <errno.h>
#include <sys/stat.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

/* Apply the destination's current permission bits to a staged replacement.
 * If the destination does not exist, use its writer-specific default mode.
 * Any other stat error is fatal: silently falling back could change access
 * permissions on a file that already exists but cannot be inspected. */
static inline gboolean
rcl_file_apply_mode_for_write( const gchar *temporary_path,
                               const gchar *destination_path,
                               mode_t default_mode,
                               GError **error )
{
  GStatBuf st;
  mode_t mode;

  if( g_stat( destination_path, &st ) == 0 )
    mode = st.st_mode & 07777;
  else if( errno == ENOENT )
    mode = default_mode;
  else
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 "stat %s: %s", destination_path, g_strerror(errno) );
    return FALSE;
  }

  if( g_chmod( temporary_path, mode ) != 0 )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 "chmod(%s): %s", temporary_path, g_strerror(errno) );
    return FALSE;
  }

  return TRUE;
}

#endif /* RCL_FILE_UTILS_H */
