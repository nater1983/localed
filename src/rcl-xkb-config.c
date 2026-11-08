/*
 * Copyright (C) 2026 Nathaniel Russell
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rcl-xkb-config.h"

#include <gio/gio.h>
#include <string.h>

typedef struct
{
  gchar *identifier;
  gchar *layout;
  gchar *model;
  gchar *variant;
  gchar *options;
} RclXkbSection;

static void
rcl_xkb_section_clear ( RclXkbSection *section )
{
  g_clear_pointer( &section->identifier, g_free );
  g_clear_pointer( &section->layout, g_free );
  g_clear_pointer( &section->model, g_free );
  g_clear_pointer( &section->variant, g_free );
  g_clear_pointer( &section->options, g_free );
}

static void
rcl_xkb_section_init ( RclXkbSection *section )
{
  memset( section, 0, sizeof( *section ) );
  section->layout = g_strdup( "" );
  section->model = g_strdup( "" );
  section->variant = g_strdup( "" );
  section->options = g_strdup( "" );
}

/*
 * Tokenize one Xorg configuration line.  Backslashes are literal inside
 * double-quoted strings; an unquoted # begins a comment.  The caller owns
 * the returned string vector.
 */
static gchar **
rcl_xkb_tokenize_line ( const gchar *line, GError **error )
{
  GPtrArray *tokens = g_ptr_array_new_with_free_func( g_free );
  const gchar *p = line;

  while( *p )
  {
    GString *token;
    gboolean quoted = FALSE;
    gboolean was_quoted = FALSE;

    while( g_ascii_isspace( *p ) )
      p++;

    if( *p == '\0' || *p == '#' )
      break;

    token = g_string_new( NULL );
    if( *p == '"' )
    {
      quoted = TRUE;
      was_quoted = TRUE;
      p++;
    }

    while( *p )
    {
      if( quoted )
      {
        if( *p == '"' )
        {
          p++;
          quoted = FALSE;
          break;
        }
        /*
         * Xorg's configuration lexer does not interpret C-style backslash
         * escapes inside quoted strings.  A backslash is ordinary data;
         * the next double quote still terminates the string.
         */
        if( *p == '\n' || *p == '\r' )
        {
          g_set_error_literal( error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "unterminated quoted string in Xorg configuration" );
          g_string_free( token, TRUE );
          g_ptr_array_unref( tokens );
          return NULL;
        }
        g_string_append_c( token, *p++ );
      }
      else
      {
        if( g_ascii_isspace( *p ) || *p == '#' )
          break;
        if( *p == '"' )
        {
          g_set_error_literal( error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "unexpected quote in unquoted Xorg token" );
          g_string_free( token, TRUE );
          g_ptr_array_unref( tokens );
          return NULL;
        }
        g_string_append_c( token, *p++ );
      }
    }

    if( quoted )
    {
      g_set_error_literal( error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "unterminated quoted string in Xorg configuration" );
      g_string_free( token, TRUE );
      g_ptr_array_unref( tokens );
      return NULL;
    }

    g_ptr_array_add( tokens, g_string_free( token, FALSE ) );

    /*
     * A quoted Xorg token must end at whitespace, a comment, or end of line.
     * Do not silently split a malformed token such as "value"suffix.
     */
    if( was_quoted && *p != '\0' && !g_ascii_isspace( *p ) && *p != '#' )
    {
      g_set_error_literal( error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "unexpected character after quoted Xorg token" );
      g_ptr_array_unref( tokens );
      return NULL;
    }

    while( g_ascii_isspace( *p ) )
      p++;
    if( *p == '#' )
      break;
  }

  g_ptr_array_add( tokens, NULL );
  return (gchar **) g_ptr_array_free( tokens, FALSE );
}

static gboolean
rcl_xkb_identifier_is_valid ( const gchar *value )
{
  const gchar *p;

  if( !value || value[0] == '\0' )
    return FALSE;

  for( p = value; *p; p++ )
  {
    if( g_ascii_isalnum( *p ) || *p == '_' || *p == '-' ||
        *p == '.' || *p == '+' )
      continue;
    return FALSE;
  }

  return TRUE;
}

static gboolean
rcl_xkb_list_is_valid ( const gchar *value,
                        gboolean allow_empty_items )
{
  gchar **items;
  guint i, count;
  gboolean valid = TRUE;

  if( !value || value[0] == '\0' )
    return TRUE;

  items = g_strsplit( value, ",", -1 );
  count = g_strv_length( items );

  for( i = 0; i < count; i++ )
  {
    if( items[i][0] == '\0' )
    {
      if( !allow_empty_items )
        valid = FALSE;
      continue;
    }

    if( !rcl_xkb_identifier_is_valid( items[i] ) )
    {
      valid = FALSE;
      break;
    }
  }

  g_strfreev( items );
  return valid;
}

static gboolean
rcl_xkb_options_are_valid ( const gchar *value )
{
  gchar **items;
  guint i;
  gboolean valid = TRUE;

  if( !value || value[0] == '\0' )
    return TRUE;

  items = g_strsplit( value, ",", -1 );

  for( i = 0; items[i] != NULL; i++ )
  {
    gchar *colon = strchr( items[i], ':' );

    if( !colon || colon == items[i] || colon[1] == '\0' ||
        strchr( colon + 1, ':' ) != NULL )
    {
      valid = FALSE;
      break;
    }

    *colon = '\0';
    if( !rcl_xkb_identifier_is_valid( items[i] ) ||
        !rcl_xkb_identifier_is_valid( colon + 1 ) )
    {
      valid = FALSE;
      break;
    }
  }

  g_strfreev( items );
  return valid;
}

gboolean
rcl_xkb_value_is_valid ( RclXkbValueType type,
                         const gchar *value )
{
  if( !value || strlen( value ) > 256 )
    return FALSE;

  switch( type )
  {
    case RCL_XKB_VALUE_LAYOUT:
      return rcl_xkb_list_is_valid( value, FALSE );

    case RCL_XKB_VALUE_MODEL:
      return value[0] == '\0' || rcl_xkb_identifier_is_valid( value );

    case RCL_XKB_VALUE_VARIANT:
      /*
       * Empty list elements preserve positional correspondence with layouts,
       * e.g. ",dvorak" selects the default first layout and dvorak for the
       * second.  A comma-only value is not meaningful.
       */
      if( value[0] == '\0' )
        return TRUE;
      if( strspn( value, "," ) == strlen( value ) )
        return FALSE;
      return rcl_xkb_list_is_valid( value, TRUE );

    case RCL_XKB_VALUE_OPTIONS:
      return rcl_xkb_options_are_valid( value );

    default:
      return FALSE;
  }
}

static gboolean
rcl_xkb_option_is_managed ( const gchar *name )
{
  return g_strcmp0( name, "XkbLayout" ) == 0 ||
         g_strcmp0( name, "XkbModel" ) == 0 ||
         g_strcmp0( name, "XkbVariant" ) == 0 ||
         g_strcmp0( name, "XkbOptions" ) == 0;
}

static void
rcl_xkb_section_set_option ( RclXkbSection *section,
                             const gchar *name,
                             const gchar *value )
{
  gchar **slot = NULL;

  if( g_strcmp0( name, "XkbLayout" ) == 0 )
    slot = &section->layout;
  else if( g_strcmp0( name, "XkbModel" ) == 0 )
    slot = &section->model;
  else if( g_strcmp0( name, "XkbVariant" ) == 0 )
    slot = &section->variant;
  else if( g_strcmp0( name, "XkbOptions" ) == 0 )
    slot = &section->options;

  if( slot )
  {
    g_free( *slot );
    *slot = g_strdup( value );
  }
}

gboolean
rcl_xkb_config_parse ( const gchar *contents,
                       gchar **out_layout,
                       gchar **out_model,
                       gchar **out_variant,
                       gchar **out_options,
                       GError **error )
{
  gchar **lines;
  guint i;
  gboolean in_section = FALSE;
  gboolean in_input_class = FALSE;
  RclXkbSection current;
  gchar *layout = g_strdup( "" );
  gchar *model = g_strdup( "" );
  gchar *variant = g_strdup( "" );
  gchar *options = g_strdup( "" );

  g_return_val_if_fail( contents != NULL, FALSE );
  g_return_val_if_fail( out_layout != NULL, FALSE );
  g_return_val_if_fail( out_model != NULL, FALSE );
  g_return_val_if_fail( out_variant != NULL, FALSE );
  g_return_val_if_fail( out_options != NULL, FALSE );
  g_return_val_if_fail( error == NULL || *error == NULL, FALSE );

  *out_layout = NULL;
  *out_model = NULL;
  *out_variant = NULL;
  *out_options = NULL;
  rcl_xkb_section_init( &current );

  lines = g_strsplit( contents, "\n", -1 );
  for( i = 0; lines[i] != NULL; i++ )
  {
    gchar **tokens = rcl_xkb_tokenize_line( lines[i], error );
    guint n = tokens ? g_strv_length( tokens ) : 0;

    if( !tokens )
      goto fail;

    if( n == 0 )
    {
      g_strfreev( tokens );
      continue;
    }

    if( g_ascii_strcasecmp( tokens[0], "Section" ) == 0 )
    {
      if( in_section )
      {
        g_set_error_literal( error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "nested Section in Xorg configuration" );
        g_strfreev( tokens );
        goto fail;
      }
      if( n != 2 )
      {
        g_set_error_literal( error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "malformed Section directive in Xorg configuration" );
        g_strfreev( tokens );
        goto fail;
      }
      in_section = TRUE;
      in_input_class = g_ascii_strcasecmp( tokens[1], "InputClass" ) == 0;
      rcl_xkb_section_clear( &current );
      rcl_xkb_section_init( &current );
    }
    else if( g_ascii_strcasecmp( tokens[0], "EndSection" ) == 0 )
    {
      if( !in_section || n != 1 )
      {
        g_set_error_literal( error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "unmatched or malformed EndSection directive" );
        g_strfreev( tokens );
        goto fail;
      }

      if( in_input_class &&
          g_strcmp0( current.identifier, "system-keyboard" ) == 0 )
      {
        g_free( layout );  layout = g_strdup( current.layout );
        g_free( model );   model = g_strdup( current.model );
        g_free( variant ); variant = g_strdup( current.variant );
        g_free( options ); options = g_strdup( current.options );
      }
      in_section = FALSE;
      in_input_class = FALSE;
      rcl_xkb_section_clear( &current );
      rcl_xkb_section_init( &current );
    }
    else if( in_section && in_input_class &&
             g_ascii_strcasecmp( tokens[0], "Identifier" ) == 0 )
    {
      if( n != 2 )
      {
        g_set_error_literal( error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "malformed Identifier in Xorg InputClass" );
        g_strfreev( tokens );
        goto fail;
      }
      g_free( current.identifier );
      current.identifier = g_strdup( tokens[1] );
    }
    else if( in_section && in_input_class &&
             g_ascii_strcasecmp( tokens[0], "Option" ) == 0 )
    {
      if( n < 2 || n > 3 ||
          ( rcl_xkb_option_is_managed( tokens[1] ) && n != 3 ) )
      {
        g_set_error_literal( error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "malformed Option in Xorg InputClass" );
        g_strfreev( tokens );
        goto fail;
      }
      if( n == 3 )
        rcl_xkb_section_set_option( &current, tokens[1], tokens[2] );
    }

    g_strfreev( tokens );
  }

  if( in_section )
  {
    g_set_error_literal( error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "unterminated Section in Xorg configuration" );
    goto fail;
  }

  g_strfreev( lines );
  rcl_xkb_section_clear( &current );
  *out_layout = layout;
  *out_model = model;
  *out_variant = variant;
  *out_options = options;
  return TRUE;

fail:
  g_strfreev( lines );
  rcl_xkb_section_clear( &current );
  g_free( layout );
  g_free( model );
  g_free( variant );
  g_free( options );
  return FALSE;
}
