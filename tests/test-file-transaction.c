/*
 * Copyright (C) 2026 Nathaniel Russell
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <string.h>

#include "rcl-file-transaction.h"

typedef enum { F_NONE,F_CREATE,F_CLOSE,F_WRITE,F_CHOWN,F_CHMOD,F_RENAME,F_CLEANUP,F_CLOSE_CLEANUP } FStage;
static FStage fstage;
static gchar operation_order[8];
static guint operation_count;
static int imkstemp(gchar*p){if(fstage==F_CREATE){errno=EACCES;return -1;}return g_mkstemp(p);}
static int iclose(int fd){int r=close(fd);if(fstage==F_CLOSE||fstage==F_CLOSE_CLEANUP){errno=EIO;return -1;}return r;}
static gboolean iwrite(const gchar*p,const gchar*d,gssize n,GError**e){if(fstage==F_WRITE){g_set_error_literal(e,G_IO_ERROR,G_IO_ERROR_NO_SPACE,"injected write failure");return FALSE;}return g_file_set_contents(p,d,n,e);}
static int ichown(const gchar*p,uid_t u,gid_t g){if(operation_count<sizeof(operation_order)-1)operation_order[operation_count++]='C';if(fstage==F_CHOWN){errno=EPERM;return -1;}return chown(p,u,g);}
static int ichmod(const gchar*p,mode_t m){if(operation_count<sizeof(operation_order)-1)operation_order[operation_count++]='M';if(fstage==F_CHMOD||fstage==F_CLEANUP){errno=EPERM;return -1;}return g_chmod(p,m);}
static int iunlink(const gchar*p){if(fstage==F_CLEANUP||fstage==F_CLOSE_CLEANUP){errno=EACCES;return -1;}return g_unlink(p);}
static int irename(const gchar*a,const gchar*b){if(fstage==F_RENAME){errno=EIO;return -1;}return g_rename(a,b);}
static guint temp_count(const gchar*d){GDir*x=g_dir_open(d,0,NULL);const gchar*n;guint c=0;while((n=g_dir_read_name(x)))if(g_str_has_prefix(n,"first."))c++;g_dir_close(x);return c;}
typedef struct _Fixture Fixture;
struct _Fixture
{
  gchar *directory;
  gchar *first;
  gchar *second_directory;
  gchar *second;
};

static void
test_checked_temporary_close_failure ( Fixture *f, gconstpointer user_data )
{
  gchar *tmp_path = g_strdup_printf( "%s.XXXXXX", f->first );
  int fd;
  GError *error = NULL;
  RclFileTransactionTestOps ops = {
    imkstemp, iclose, iwrite, ichown, ichmod, irename, iunlink
  };

  fd = g_mkstemp( tmp_path );
  g_assert_cmpint( fd, >=, 0 );

  fstage = F_CLOSE;
  rcl_file_transaction_set_test_ops( &ops );
  g_assert_false( rcl_file_close_temporary( fd, tmp_path, f->first, &error ) );
  rcl_file_transaction_set_test_ops( NULL );
  fstage = F_NONE;

  g_assert_error( error, G_IO_ERROR, G_IO_ERROR_FAILED );
  g_assert_nonnull( strstr( error->message, "close temporary file" ) );
  g_assert_false( g_file_test( tmp_path, G_FILE_TEST_EXISTS ) );
  g_assert_cmpuint( temp_count( f->directory ), ==, 0 );

  g_clear_error( &error );
  g_free( tmp_path );
}

static void
test_checked_temporary_close_cleanup_failure ( Fixture *f,
                                               gconstpointer user_data )
{
  gchar *tmp_path = g_strdup_printf( "%s.XXXXXX", f->first );
  int fd;
  GError *error = NULL;
  GDir *directory;
  const gchar *name;
  RclFileTransactionTestOps ops = {
    imkstemp, iclose, iwrite, ichown, ichmod, irename, iunlink
  };

  fd = g_mkstemp( tmp_path );
  g_assert_cmpint( fd, >=, 0 );

  fstage = F_CLOSE_CLEANUP;
  rcl_file_transaction_set_test_ops( &ops );
  g_assert_false( rcl_file_close_temporary( fd, tmp_path, f->first, &error ) );
  rcl_file_transaction_set_test_ops( NULL );
  fstage = F_NONE;

  g_assert_nonnull( error );
  g_assert_nonnull( strstr( error->message, "close temporary file" ) );
  g_assert_nonnull( strstr( error->message,
                            "temporary-file cleanup also failed" ) );

  g_clear_error( &error );

  /* The injected unlink failure deliberately leaves the staged file behind. */
  directory = g_dir_open( f->directory, 0, NULL );
  g_assert_nonnull( directory );
  while( ( name = g_dir_read_name( directory ) ) != NULL )
    if( g_str_has_prefix( name, "first." ) )
    {
      gchar *leftover = g_build_filename( f->directory, name, NULL );
      g_unlink( leftover );
      g_free( leftover );
    }
  g_dir_close( directory );
  g_assert_cmpuint( temp_count( f->directory ), ==, 0 );
  g_free( tmp_path );
}

static void test_injected_failures(Fixture*f,gconstpointer u){FStage ss[]={F_CREATE,F_CLOSE,F_WRITE,F_CHOWN,F_CHMOD,F_RENAME};const gchar*msg[]={"create rollback temporary","close rollback temporary","restore ","restore ownership","restore permissions","rollback rename"};guint i;for(i=0;i<G_N_ELEMENTS(ss);i++){RclFileSnapshot s={0};GError*e=NULL;RclFileTransactionTestOps o={imkstemp,iclose,iwrite,ichown,ichmod,irename,NULL};g_assert_true(g_file_set_contents(f->first,"before",-1,&e));g_assert_no_error(e);g_assert_true(rcl_file_snapshot_take(&s,f->first,&e));g_assert_no_error(e);fstage=ss[i];rcl_file_transaction_set_test_ops(&o);g_assert_false(rcl_file_snapshot_restore(&s,&e));rcl_file_transaction_set_test_ops(NULL);fstage=F_NONE;g_assert_nonnull(e);g_assert_nonnull(strstr(e->message,msg[i]));g_clear_error(&e);g_assert_cmpuint(temp_count(f->directory),==,0);rcl_file_snapshot_clear(&s);g_unlink(f->first);}}

static void test_cleanup_failure_preserves_restore_error(Fixture*f,gconstpointer u){
 RclFileSnapshot s={0};GError*e=NULL;RclFileTransactionTestOps o={imkstemp,iclose,iwrite,ichown,ichmod,irename,iunlink};
 g_assert_true(g_file_set_contents(f->first,"before",-1,&e));g_assert_no_error(e);
 g_assert_true(rcl_file_snapshot_take(&s,f->first,&e));g_assert_no_error(e);
 fstage=F_CLEANUP;rcl_file_transaction_set_test_ops(&o);
 g_assert_false(rcl_file_snapshot_restore(&s,&e));
 rcl_file_transaction_set_test_ops(NULL);fstage=F_NONE;
 g_assert_nonnull(e);g_assert_nonnull(strstr(e->message,"restore permissions"));
 g_assert_nonnull(strstr(e->message,"temporary-file cleanup also failed"));
 g_assert_cmpuint(e->domain,==,G_IO_ERROR);g_assert_cmpint(e->code,==,G_IO_ERROR_PERMISSION_DENIED);
 g_clear_error(&e);
 /* The injected unlink failure intentionally leaves one file; remove it now. */
 GDir*d=g_dir_open(f->directory,0,NULL);const gchar*n;while((n=g_dir_read_name(d)))if(g_str_has_prefix(n,"first.")){gchar*q=g_build_filename(f->directory,n,NULL);g_unlink(q);g_free(q);}g_dir_close(d);
 g_assert_cmpuint(temp_count(f->directory),==,0);rcl_file_snapshot_clear(&s);
}
static void test_ownership_before_mode(Fixture*f,gconstpointer u){
 RclFileSnapshot s={0};GError*e=NULL;RclFileTransactionTestOps o={imkstemp,iclose,iwrite,ichown,ichmod,irename,iunlink};
 g_assert_true(g_file_set_contents(f->first,"before",-1,&e));g_assert_no_error(e);
 g_assert_cmpint(g_chmod(f->first,04750),==,0);
 g_assert_true(rcl_file_snapshot_take(&s,f->first,&e));g_assert_no_error(e);
 g_assert_true(g_file_set_contents(f->first,"after",-1,&e));g_assert_no_error(e);
 operation_count=0;memset(operation_order,0,sizeof(operation_order));
 rcl_file_transaction_set_test_ops(&o);
 g_assert_true(rcl_file_snapshot_restore(&s,&e));
 rcl_file_transaction_set_test_ops(NULL);
 g_assert_no_error(e);g_assert_cmpstr(operation_order,==,"CM");
 rcl_file_snapshot_clear(&s);
}

static void test_owner_group(Fixture*f,gconstpointer u){RclFileSnapshot s={0};GError*e=NULL;GStatBuf a,b;g_assert_true(g_file_set_contents(f->first,"before",-1,&e));g_assert_no_error(e);g_chmod(f->first,04750);g_stat(f->first,&a);g_assert_true(rcl_file_snapshot_take(&s,f->first,&e));g_assert_no_error(e);g_file_set_contents(f->first,"after",-1,&e);g_assert_no_error(e);g_assert_true(rcl_file_snapshot_restore(&s,&e));g_assert_no_error(e);g_stat(f->first,&b);g_assert_cmpuint(b.st_uid,==,a.st_uid);g_assert_cmpuint(b.st_gid,==,a.st_gid);g_assert_cmpuint(b.st_mode&07777,==,a.st_mode&07777);rcl_file_snapshot_clear(&s);}

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
  rcl_report_rollback_failure(&primary_error,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_FAILED,"second rollback failed"));
  g_assert_nonnull(strstr(primary_error->message,"configuration write failed"));
  g_assert_nonnull(strstr(primary_error->message,"rollback restore failed"));
  g_assert_nonnull(strstr(primary_error->message,"second rollback failed"));
  g_assert_cmpuint(primary_error->domain,==,G_IO_ERROR);
  g_assert_cmpint(primary_error->code,==,G_IO_ERROR_FAILED);
  g_clear_error(&primary_error);
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


static void
test_checked_temporary_close_without_error_receiver (
    Fixture *fixture, gconstpointer user_data )
{
  gchar *tmp_path = g_strdup_printf( "%s.XXXXXX", fixture->first );
  int fd;
  RclFileTransactionTestOps ops = {
    imkstemp, iclose, iwrite, ichown, ichmod, irename, iunlink
  };

  fd = g_mkstemp( tmp_path );
  g_assert_cmpint( fd, >=, 0 );

  fstage = F_CLOSE;
  rcl_file_transaction_set_test_ops( &ops );
  g_test_expect_message( NULL, G_LOG_LEVEL_WARNING,
                         "*close temporary file*" );
  g_assert_false( rcl_file_close_temporary( fd, tmp_path,
                                            fixture->first, NULL ) );
  g_test_assert_expected_messages();
  rcl_file_transaction_set_test_ops( NULL );
  fstage = F_NONE;

  g_assert_false( g_file_test( tmp_path, G_FILE_TEST_EXISTS ) );
  g_assert_cmpuint( temp_count( fixture->directory ), ==, 0 );
  g_free( tmp_path );
}

static void
test_checked_temporary_close_and_cleanup_without_error_receiver (
    Fixture *fixture, gconstpointer user_data )
{
  gchar *tmp_path = g_strdup_printf( "%s.XXXXXX", fixture->first );
  int fd;
  RclFileTransactionTestOps ops = {
    imkstemp, iclose, iwrite, ichown, ichmod, irename, iunlink
  };

  fd = g_mkstemp( tmp_path );
  g_assert_cmpint( fd, >=, 0 );

  fstage = F_CLOSE_CLEANUP;
  rcl_file_transaction_set_test_ops( &ops );
  g_test_expect_message( NULL, G_LOG_LEVEL_WARNING,
                         "*close temporary file*" );
  g_test_expect_message( NULL, G_LOG_LEVEL_WARNING,
                         "*temporary-file cleanup also failed*" );
  g_assert_false( rcl_file_close_temporary( fd, tmp_path,
                                            fixture->first, NULL ) );
  g_test_assert_expected_messages();
  rcl_file_transaction_set_test_ops( NULL );
  fstage = F_NONE;

  /* The injected unlink failure deliberately leaves the staged file behind. */
  g_assert_true( g_file_test( tmp_path, G_FILE_TEST_EXISTS ) );
  g_assert_cmpint( g_unlink( tmp_path ), ==, 0 );
  g_assert_cmpuint( temp_count( fixture->directory ), ==, 0 );
  g_free( tmp_path );
}

static void
test_report_rollback_failure_without_error_receiver (
    Fixture *fixture, gconstpointer user_data )
{
  GError *rollback_error = NULL;

  g_set_error_literal( &rollback_error, G_IO_ERROR,
                       G_IO_ERROR_PERMISSION_DENIED,
                       "rollback restore failed" );

  g_test_expect_message( NULL, G_LOG_LEVEL_WARNING,
                         "*rollback restore failed*" );
  rcl_report_rollback_failure( NULL, rollback_error );
  g_test_assert_expected_messages();
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
  g_test_add("/file-transaction/checked-temporary-close",Fixture,NULL,fixture_setup,test_checked_temporary_close_failure,fixture_teardown);
  g_test_add( "/file-transaction/checked-temporary-close-no-error",
              Fixture, NULL, fixture_setup,
              test_checked_temporary_close_without_error_receiver,
              fixture_teardown );
  g_test_add( "/file-transaction/checked-temporary-close-cleanup-no-error",
              Fixture, NULL, fixture_setup,
              test_checked_temporary_close_and_cleanup_without_error_receiver,
              fixture_teardown );
  g_test_add( "/file-transaction/rollback-failure-no-error",
              Fixture, NULL, fixture_setup,
              test_report_rollback_failure_without_error_receiver,
              fixture_teardown );
  g_test_add("/file-transaction/checked-temporary-close-cleanup-failure",Fixture,NULL,fixture_setup,test_checked_temporary_close_cleanup_failure,fixture_teardown);
  g_test_add("/file-transaction/failure-injection",Fixture,NULL,fixture_setup,test_injected_failures,fixture_teardown);
  g_test_add("/file-transaction/cleanup-failure-preserves-error",Fixture,NULL,fixture_setup,test_cleanup_failure_preserves_restore_error,fixture_teardown);
  g_test_add("/file-transaction/ownership-before-mode",Fixture,NULL,fixture_setup,test_ownership_before_mode,fixture_teardown);
  g_test_add("/file-transaction/owner-group",Fixture,NULL,fixture_setup,test_owner_group,fixture_teardown);

  return g_test_run();
}
