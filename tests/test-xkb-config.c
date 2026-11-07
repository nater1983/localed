/*
 * Copyright (C) 2026 Nathaniel Russell
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <gio/gio.h>
#include <glib.h>

#include "rcl-xkb-config.h"

static void
assert_parse ( const gchar *contents,
               const gchar *layout,
               const gchar *model,
               const gchar *variant,
               const gchar *options )
{
  gchar *actual_layout = NULL;
  gchar *actual_model = NULL;
  gchar *actual_variant = NULL;
  gchar *actual_options = NULL;
  GError *error = NULL;

  g_assert_true( rcl_xkb_config_parse( contents,
                                       &actual_layout, &actual_model,
                                       &actual_variant, &actual_options,
                                       &error ) );
  g_assert_no_error( error );
  g_assert_cmpstr( actual_layout, ==, layout );
  g_assert_cmpstr( actual_model, ==, model );
  g_assert_cmpstr( actual_variant, ==, variant );
  g_assert_cmpstr( actual_options, ==, options );

  g_free( actual_layout );
  g_free( actual_model );
  g_free( actual_variant );
  g_free( actual_options );
}

static void
test_owned_input_class ( void )
{
  assert_parse(
    "Section \"ServerFlags\"\n"
    "    Option \"XkbLayout\" \"bad\"\n"
    "EndSection\n"
    "Section \"InputClass\"\n"
    "    Identifier \"other-keyboard\"\n"
    "    Option \"XkbLayout\" \"de\"\n"
    "EndSection\n"
    "Section \"InputClass\"\n"
    "    # comment mentioning Option \"XkbLayout\" \"fr\"\n"
    "    Identifier \"system-keyboard\"\n"
    "    MatchIsKeyboard \"on\"\n"
    "    Option \"XkbLayout\" \"us,de\" # trailing comment\n"
    "    Option \"XkbModel\" \"pc105\"\n"
    "    Option \"XkbVariant\" \"intl,nodeadkeys\"\n"
    "    Option \"XkbOptions\" \"grp:win_space_toggle\"\n"
    "EndSection\n",
    "us,de", "pc105", "intl,nodeadkeys", "grp:win_space_toggle" );
}

static void
test_last_duplicate_option_wins ( void )
{
  assert_parse(
    "Section \"InputClass\"\n"
    "Identifier \"system-keyboard\"\n"
    "Option \"XkbLayout\" \"us\"\n"
    "Option \"XkbLayout\" \"de\"\n"
    "EndSection\n",
    "de", "", "", "" );
}

static void
test_last_owned_section_wins ( void )
{
  assert_parse(
    "Section \"InputClass\"\n"
    "Identifier \"system-keyboard\"\n"
    "Option \"XkbLayout\" \"us\"\n"
    "EndSection\n"
    "Section \"InputClass\"\n"
    "Identifier \"system-keyboard\"\n"
    "Option \"XkbLayout\" \"fr\"\n"
    "EndSection\n",
    "fr", "", "", "" );
}

static void
test_escaped_quotes ( void )
{
  /* The lexer decodes escapes; validation in the caller rejects unsafe
   * resulting values before they can become D-Bus properties. */
  assert_parse(
    "Section \"InputClass\"\n"
    "Identifier \"system-keyboard\"\n"
    "Option \"XkbLayout\" \"us\\\"bad\"\n"
    "EndSection\n",
    "us\"bad", "", "", "" );
}

static void
assert_invalid ( const gchar *contents )
{
  gchar *layout = NULL, *model = NULL, *variant = NULL, *options = NULL;
  GError *error = NULL;

  g_assert_false( rcl_xkb_config_parse( contents, &layout, &model,
                                        &variant, &options, &error ) );
  g_assert_error( error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA );
  g_assert_null( layout );
  g_assert_null( model );
  g_assert_null( variant );
  g_assert_null( options );
  g_clear_error( &error );
}

static void
test_unterminated_quote ( void )
{
  assert_invalid(
    "Section \"InputClass\"\n"
    "Identifier \"system-keyboard\"\n"
    "Option \"XkbLayout\" \"us\n"
    "EndSection\n" );
}

static void
test_unbalanced_section ( void )
{
  assert_invalid( "Section \"InputClass\"\nIdentifier \"system-keyboard\"\n" );
  assert_invalid( "EndSection\n" );
  assert_invalid( "Section \"InputClass\"\nSection \"InputClass\"\nEndSection\n" );
}

static void
test_malformed_owned_option ( void )
{
  assert_invalid(
    "Section \"InputClass\"\n"
    "Identifier \"system-keyboard\"\n"
    "Option \"XkbLayout\"\n"
    "EndSection\n" );
}

int
main ( int argc, char **argv )
{
  g_test_init( &argc, &argv, NULL );
  g_test_add_func( "/xkb-config/owned-input-class", test_owned_input_class );
  g_test_add_func( "/xkb-config/duplicate-option", test_last_duplicate_option_wins );
  g_test_add_func( "/xkb-config/duplicate-section", test_last_owned_section_wins );
  g_test_add_func( "/xkb-config/escaped-quotes", test_escaped_quotes );
  g_test_add_func( "/xkb-config/unterminated-quote", test_unterminated_quote );
  g_test_add_func( "/xkb-config/unbalanced-section", test_unbalanced_section );
  g_test_add_func( "/xkb-config/malformed-option", test_malformed_owned_option );
  return g_test_run();
}
