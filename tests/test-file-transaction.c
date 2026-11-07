/*
 * Copyright (C) 2026 Nathaniel Russell
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>

#include "rcl-file-transaction.h"

typedef struct
{
  gchar *directory;
  gchar *first;
  gchar *second_directory;
  gchar *second;
} Fixture;

static void
fixture_setup ( Fixture *fixture, gconstpointer user_data )
{
  GError *error = NULL;

  fixture->directory = g_dir_make_tmp( "localed-transaction-XXXXXX", &error );
  g_assert_no_error( error );
  g_assert_nonnull( fixture->directory );
  fixture->first = g_build_filename( fixture->directory, "first", NULL );
  fixture->second_directory = g_build_filename( fixture->directory, "second-dir", NULL );
  fixture->second = g_build_filename( fixture->second_directory, "second", NULL );
}

static void
fixture_teardown ( Fixture *fixture, gconstpointer user_data )
{
  g_unlink( fixture->first );
  g_unlink( fixture->second );
  g_rmdir( fixture->second_directory );
  g_rmdir( fixture->directory );
  g_free( fixture->first );
  g_free( fixture->second );
  g_free( fixture->second_directory );
  g_free( fixture->directory );
}

static void
test_restore_existing_file ( Fixture *fixture, gconstpointer user_data )
{
  RclFileSnapshot snapshot = { 0 };
  GError *error = NULL;
  gchar *contents = NULL;
  GStatBuf st;

  g_assert_true( g_file_set_contents( fixture->first, "original", -1, &error ) );
  g_assert_no_error( error );
  g_assert_cmpint( g_chmod( fixture->first, 0640 ), ==, 0 );

  g_assert_true( rcl_file_snapshot_take( &snapshot, fixture->first, &error ) );
  g_assert_no_error( error );
  g_assert_true( g_file_set_contents( fixture->first, "replacement", -1, &error ) );
  g_assert_no_error( error );

  g_assert_true( rcl_file_snapshot_restore( &snapshot, &error ) );
  g_assert_no_error( error );
  g_assert_true( g_file_get_contents( fixture->first, &contents, NULL, &error ) );
  g_assert_no_error( error );
  g_assert_cmpstr( contents, ==, "original" );
  g_assert_cmpint( g_stat( fixture->first, &st ), ==, 0 );
  g_assert_cmpuint( st.st_mode & 07777, ==, 0640 );

  g_free( contents );
  rcl_file_snapshot_clear( &snapshot );
}

static void
test_restore_removes_new_file ( Fixture *fixture, gconstpointer user_data )
{
  RclFileSnapshot snapshot = { 0 };
  GError *error = NULL;

  g_assert_true( rcl_file_snapshot_take( &snapshot, fixture->first, &error ) );
  g_assert_no_error( error );
  g_assert_false( snapshot.existed );

  g_assert_true( g_file_set_contents( fixture->first, "new", -1, &error ) );
  g_assert_no_error( error );
  g_assert_true( rcl_file_snapshot_restore( &snapshot, &error ) );
  g_assert_no_error( error );
  g_assert_false( g_file_test( fixture->first, G_FILE_TEST_EXISTS ) );

  rcl_file_snapshot_clear( &snapshot );
}

static void
test_multi_file_rollback_after_second_write_failure (
    Fixture *fixture, gconstpointer user_data )
{
  RclFileSnapshot first_snapshot = { 0 }, second_snapshot = { 0 };
  GError *error = NULL;
  gchar *contents = NULL;

  g_assert_true( g_file_set_contents( fixture->first, "first-before", -1, &error ) );
  g_assert_no_error( error );
  g_assert_cmpint( g_mkdir( fixture->second_directory, 0755 ), ==, 0 );

  g_assert_true( rcl_file_snapshot_take( &first_snapshot, fixture->first, &error ) );
  g_assert_no_error( error );
  g_assert_true( rcl_file_snapshot_take( &second_snapshot, fixture->second, &error ) );
  g_assert_no_error( error );
  g_assert_false( second_snapshot.existed );

  /* First write succeeds.  Removing the second target's parent directory
   * deterministically makes the second write fail with ENOENT, including
   * when the test suite runs as root. */
  g_assert_true( g_file_set_contents( fixture->first, "first-after", -1, &error ) );
  g_assert_no_error( error );
  g_assert_cmpint( g_rmdir( fixture->second_directory ), ==, 0 );

  g_assert_false( g_file_set_contents( fixture->second, "second-after", -1, &error ) );
  g_assert_nonnull( error );
  g_clear_error( &error );

  g_assert_true( rcl_file_snapshot_restore( &first_snapshot, &error ) );
  g_assert_no_error( error );
  g_assert_true( g_file_get_contents( fixture->first, &contents, NULL, &error ) );
  g_assert_no_error( error );
  g_assert_cmpstr( contents, ==, "first-before" );
  g_assert_false( g_file_test( fixture->second, G_FILE_TEST_EXISTS ) );

  g_free( contents );
  rcl_file_snapshot_clear( &first_snapshot );
  rcl_file_snapshot_clear( &second_snapshot );
}


static void
test_restore_rename_failure_cleans_temporary_file (
    Fixture *fixture, gconstpointer user_data )
{
  RclFileSnapshot snapshot = { 0 };
  GError *error = NULL;
  gchar *contents = NULL;
  gchar *pattern = g_strdup_printf( "%s.XXXXXX", fixture->first );
  gchar *directory = NULL;
  GDir *dir;
  const gchar *name;
  guint leftovers = 0;

  g_assert_true( g_file_set_contents( fixture->first, "before", -1, &error ) );
  g_assert_no_error( error );
  g_assert_true( rcl_file_snapshot_take( &snapshot, fixture->first, &error ) );
  g_assert_no_error( error );

  /* Force the final rename to fail after the rollback temporary file has
   * been created and populated. */
  g_assert_cmpint( g_unlink( fixture->first ), ==, 0 );
  g_assert_cmpint( g_mkdir( fixture->first, 0755 ), ==, 0 );

  g_assert_false( rcl_file_snapshot_restore( &snapshot, &error ) );
  g_assert_nonnull( error );
  g_assert_nonnull( strstr( error->message, "rollback rename" ) );
  g_clear_error( &error );

  directory = g_path_get_dirname( fixture->first );
  dir = g_dir_open( directory, 0, &error );
  g_assert_no_error( error );
  g_assert_nonnull( dir );

  while( ( name = g_dir_read_name( dir ) ) != NULL )
    if( g_str_has_prefix( name, "first." ) )
      leftovers++;

  g_dir_close( dir );
  g_assert_cmpuint( leftovers, ==, 0 );
  g_assert_true( g_file_test( fixture->first, G_FILE_TEST_IS_DIR ) );

  g_rmdir( fixture->first );
  g_free( directory );
  g_free( pattern );
  g_free( contents );
  rcl_file_snapshot_clear( &snapshot );
}

static void
test_report_rollback_failure_preserves_both_errors (
    Fixture *fixture, gconstpointer user_data )
{
  GError *primary_error = NULL;
  GError *rollback_error = NULL;

  g_set_error_literal( &primary_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "configuration write failed" );
  g_set_error_literal( &rollback_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                       "rollback restore failed" );

  rcl_report_rollback_failure( &primary_error, rollback_error );

  g_assert_nonnull( primary_error );
  g_assert_nonnull( strstr( primary_error->message,
                            "configuration write failed" ) );
  g_assert_nonnull( strstr( primary_error->message,
                            "rollback also failed: rollback restore failed" ) );
  g_clear_error( &primary_error );
}

static void
test_snapshot_read_error ( Fixture *fixture, gconstpointer user_data )
{
  RclFileSnapshot snapshot = { 0 };
  GError *error = NULL;

  g_assert_cmpint( g_mkdir( fixture->second_directory, 0755 ), ==, 0 );
  g_assert_false( rcl_file_snapshot_take( &snapshot,
                                          fixture->second_directory, &error ) );
  g_assert_nonnull( error );
  g_clear_error( &error );
  rcl_file_snapshot_clear( &snapshot );
}

int
main ( int argc, char **argv )
{
  g_test_init( &argc, &argv, NULL );

  g_test_add( "/file-transaction/restore-existing", Fixture, NULL,
              fixture_setup, test_restore_existing_file, fixture_teardown );
  g_test_add( "/file-transaction/restore-removes-new", Fixture, NULL,
              fixture_setup, test_restore_removes_new_file, fixture_teardown );
  g_test_add( "/file-transaction/multi-file-rollback", Fixture, NULL,
              fixture_setup, test_multi_file_rollback_after_second_write_failure,
              fixture_teardown );
  g_test_add( "/file-transaction/snapshot-read-error", Fixture, NULL,
              fixture_setup, test_snapshot_read_error, fixture_teardown );
  g_test_add( "/file-transaction/restore-rename-failure-cleans-temp", Fixture, NULL,
              fixture_setup, test_restore_rename_failure_cleans_temporary_file,
              fixture_teardown );
  g_test_add( "/file-transaction/report-rollback-failure", Fixture, NULL,
              fixture_setup, test_report_rollback_failure_preserves_both_errors,
              fixture_teardown );

  return g_test_run();
}
