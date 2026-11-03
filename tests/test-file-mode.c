/*
 * Copyright (C) 2026 Nathaniel Russell
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>

#include "rcl-file-utils.h"

typedef struct
{
  gchar *directory;
  gchar *destination;
  gchar *temporary;
} Fixture;

static void
fixture_setup( Fixture *fixture, gconstpointer user_data )
{
  GError *error = NULL;
  fixture->directory = g_dir_make_tmp( "localed-file-mode-XXXXXX", &error );
  g_assert_no_error( error );
  g_assert_nonnull( fixture->directory );
  fixture->destination = g_build_filename( fixture->directory, "config", NULL );
  fixture->temporary = g_build_filename( fixture->directory, "config.new", NULL );
}

static void
fixture_teardown( Fixture *fixture, gconstpointer user_data )
{
  g_unlink( fixture->temporary );
  g_unlink( fixture->destination );
  g_rmdir( fixture->directory );
  g_free( fixture->temporary );
  g_free( fixture->destination );
  g_free( fixture->directory );
}

static void
test_existing_mode( Fixture *fixture, gconstpointer user_data )
{
  mode_t expected = GPOINTER_TO_UINT( user_data );
  GError *error = NULL;
  GStatBuf st;

  g_assert_true( g_file_set_contents( fixture->destination, "old", -1, &error ) );
  g_assert_no_error( error );
  g_assert_cmpint( g_chmod( fixture->destination, expected ), ==, 0 );
  g_assert_true( g_file_set_contents( fixture->temporary, "new", -1, &error ) );
  g_assert_no_error( error );

  g_assert_true( rcl_file_apply_mode_for_write( fixture->temporary,
                                                 fixture->destination,
                                                 0755, &error ) );
  g_assert_no_error( error );
  g_assert_cmpint( g_rename( fixture->temporary, fixture->destination ), ==, 0 );
  g_assert_cmpint( g_stat( fixture->destination, &st ), ==, 0 );
  g_assert_cmpuint( st.st_mode & 07777, ==, expected );
}

static void
test_new_file_default_mode( Fixture *fixture, gconstpointer user_data )
{
  mode_t expected = GPOINTER_TO_UINT( user_data );
  GError *error = NULL;
  GStatBuf st;

  g_assert_true( g_file_set_contents( fixture->temporary, "new", -1, &error ) );
  g_assert_no_error( error );
  g_assert_true( rcl_file_apply_mode_for_write( fixture->temporary,
                                                 fixture->destination,
                                                 expected, &error ) );
  g_assert_no_error( error );
  g_assert_cmpint( g_rename( fixture->temporary, fixture->destination ), ==, 0 );
  g_assert_cmpint( g_stat( fixture->destination, &st ), ==, 0 );
  g_assert_cmpuint( st.st_mode & 07777, ==, expected );
}

static void
test_stat_error_is_fatal( Fixture *fixture, gconstpointer user_data )
{
  gchar *not_directory = g_build_filename( fixture->directory, "not-a-dir", NULL );
  gchar *invalid_destination = g_build_filename( not_directory, "config", NULL );
  GError *error = NULL;

  g_assert_true( g_file_set_contents( not_directory, "file", -1, &error ) );
  g_assert_no_error( error );
  g_assert_true( g_file_set_contents( fixture->temporary, "new", -1, &error ) );
  g_assert_no_error( error );

  g_assert_false( rcl_file_apply_mode_for_write( fixture->temporary,
                                                  invalid_destination,
                                                  0644, &error ) );
  g_assert_error( error, G_IO_ERROR, g_io_error_from_errno( ENOTDIR ) );
  g_clear_error( &error );

  g_unlink( fixture->temporary );
  g_unlink( not_directory );
  g_free( invalid_destination );
  g_free( not_directory );
}

int
main( int argc, char **argv )
{
  g_test_init( &argc, &argv, NULL );

  g_test_add( "/file-mode/existing/0600", Fixture, GUINT_TO_POINTER(0600),
              fixture_setup, test_existing_mode, fixture_teardown );
  g_test_add( "/file-mode/existing/0640", Fixture, GUINT_TO_POINTER(0640),
              fixture_setup, test_existing_mode, fixture_teardown );
  g_test_add( "/file-mode/existing/0750", Fixture, GUINT_TO_POINTER(0750),
              fixture_setup, test_existing_mode, fixture_teardown );
  g_test_add( "/file-mode/new/default-0755", Fixture, GUINT_TO_POINTER(0755),
              fixture_setup, test_new_file_default_mode, fixture_teardown );
  g_test_add( "/file-mode/new/default-0644", Fixture, GUINT_TO_POINTER(0644),
              fixture_setup, test_new_file_default_mode, fixture_teardown );
  g_test_add( "/file-mode/stat-error", Fixture, NULL,
              fixture_setup, test_stat_error_is_fatal, fixture_teardown );

  return g_test_run();
}
