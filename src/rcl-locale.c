/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <errno.h>
#include <string.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glib-object.h>
#include <glib/gi18n-lib.h>
#include <polkit/polkit.h>

#include "rcl-locale.h"

/* --------------------------------------------------------------------------
   The org.freedesktop.locale1 introspection XML.

   Generated once at startup by g_dbus_node_info_new_for_xml() and kept for
   the lifetime of the process.  Property/method names here must match the
   handler and getter/setter functions below, and mirror the upstream
   systemd-localed interface so existing clients (gnome-control-center,
   localectl-alikes) work unmodified.
   -------------------------------------------------------------------------- */
static const gchar locale1_introspection_xml[] =
  "<node>"
  "  <interface name='org.freedesktop.locale1'>"
  /* ---- read-only properties ---- */
  "    <property name='Locale'               type='as' access='read'/>"
  "    <property name='VConsoleKeymap'       type='s'  access='read'/>"
  "    <property name='VConsoleKeymapToggle' type='s'  access='read'/>"
  "    <property name='X11Layout'            type='s'  access='read'/>"
  "    <property name='X11Model'             type='s'  access='read'/>"
  "    <property name='X11Variant'           type='s'  access='read'/>"
  "    <property name='X11Options'           type='s'  access='read'/>"
  /* ---- methods ---- */
  "    <method name='SetLocale'>"
  "      <arg name='locale'      type='as' direction='in'/>"
  "      <arg name='interactive' type='b'  direction='in'/>"
  "    </method>"
  "    <method name='SetVConsoleKeyboard'>"
  "      <arg name='keymap'        type='s' direction='in'/>"
  "      <arg name='keymap_toggle' type='s' direction='in'/>"
  "      <arg name='convert'       type='b' direction='in'/>"
  "      <arg name='interactive'   type='b' direction='in'/>"
  "    </method>"
  "    <method name='SetX11Keyboard'>"
  "      <arg name='layout'      type='s' direction='in'/>"
  "      <arg name='model'       type='s' direction='in'/>"
  "      <arg name='variant'     type='s' direction='in'/>"
  "      <arg name='options'     type='s' direction='in'/>"
  "      <arg name='convert'     type='b' direction='in'/>"
  "      <arg name='interactive' type='b' direction='in'/>"
  "    </method>"
  "  </interface>"
  "</node>";

/* --------------------------------------------------------------------------
   Private data
   -------------------------------------------------------------------------- */
struct _RclLocaleDaemonPrivate
{
  GDBusConnection  *connection;
  GDBusNodeInfo    *introspection;
  guint             registration_id;
  gboolean          debug;

  /* Cached PolKit authority handle, fetched once at startup.  The blocking
     part of authorisation (the agent dialog) is driven asynchronously so it
     never stalls the single-threaded main loop. */
  PolkitAuthority  *authority;

  /* Cached property values.  Refreshed by rcl_daemon_sync_dbus_properties(). */
  gchar  **locale;                 /* GStrv, "KEY=value" entries, never NULL */
  gchar   *vconsole_keymap;
  gchar   *vconsole_keymap_toggle;
  gchar   *x11_layout;
  gchar   *x11_model;
  gchar   *x11_variant;
  gchar   *x11_options;
};

/* --------------------------------------------------------------------------
   GObject boilerplate
   -------------------------------------------------------------------------- */
G_DEFINE_TYPE_WITH_PRIVATE( RclLocaleDaemon, rcl_locale_daemon, RCL_TYPE_DAEMON )

/* Also define the abstract RclDaemon base just enough that GObject is happy.
   In a real multi-daemon codebase this would live in its own rcl-daemon.c. */
G_DEFINE_TYPE( RclDaemon, rcl_daemon, G_TYPE_OBJECT )

static void rcl_daemon_class_init( RclDaemonClass *klass ) {}
static void rcl_daemon_init( RclDaemon *self ) {}

/* --------------------------------------------------------------------------
   Known locale variables, in the canonical order systemd-localed and
   `localectl` expect when printing/comparing the Locale array.
   -------------------------------------------------------------------------- */
static const gchar *const locale_variable_names[] =
{
  "LANG",
  "LANGUAGE",
  "LC_CTYPE",
  "LC_NUMERIC",
  "LC_TIME",
  "LC_COLLATE",
  "LC_MONETARY",
  "LC_MESSAGES",
  "LC_PAPER",
  "LC_NAME",
  "LC_ADDRESS",
  "LC_TELEPHONE",
  "LC_MEASUREMENT",
  "LC_IDENTIFICATION",
  NULL
};

static gboolean
rcl_locale_variable_is_known( const gchar *name )
{
  guint i;
  for( i = 0; locale_variable_names[i] != NULL; i++ )
    if( g_strcmp0( name, locale_variable_names[i] ) == 0 )
      return TRUE;
  return FALSE;
}

/* --------------------------------------------------------------------------
   Small built-in console-keymap <-> X11 layout conversion table.

   This is intentionally a short, conservative subset (not the full kbd(4)
   keymap list) covering common single-layout console keymaps.  It is only
   consulted when a Set* call passes convert=TRUE; entries with no match
   simply leave the other side unset rather than guessing.
   -------------------------------------------------------------------------- */
typedef struct
{
  const gchar *vconsole_keymap;
  const gchar *x11_layout;
  const gchar *x11_variant;
} RclKeymapMapEntry;

static const RclKeymapMapEntry keymap_map[] =
{
  { "us",         "us", ""        },
  { "uk",         "gb", ""        },
  { "de",         "de", ""        },
  { "de-latin1",  "de", ""        },
  { "fr",         "fr", ""        },
  { "fr-latin1",  "fr", ""        },
  { "es",         "es", ""        },
  { "it",         "it", ""        },
  { "pt-latin1",  "pt", ""        },
  { "ru",         "ru", ""        },
  { "ru4",        "ru", ""        },
  { "se-latin1",  "se", ""        },
  { "no-latin1",  "no", ""        },
  { "dk-latin1",  "dk", ""        },
  { "nl",         "nl", ""        },
  { "trq",        "tr", ""        },
  { "pl",         "pl", ""        },
  { "cz-lat2",    "cz", ""        },
  { "fi",         "fi", ""        },
  { "gr",         "gr", ""        },
  { "il",         "il", ""        },
  { "jp106",      "jp", ""        },
  { "ua",         "ua", ""        },
  { "be-latin1",  "be", ""        },
  { "ch",         "ch", "de_nodeadkeys" },
  { "ch-fr",      "ch", "fr"      },
  { NULL,         NULL, NULL      }
};

/* --------------------------------------------------------------------------
   Language fallback table for LANGUAGE auto-derivation.

   This is a verbatim copy of the upstream systemd language-fallback-map
   (src/locale/language-fallback-map in the systemd source tree, installed
   at ${pkgdatadir}/language-fallback-map).

   When SetLocale is called with LANG= but without LANGUAGE=, the new LANG
   value is looked up here (exact match on the bare locale code, e.g.
   "en_AU" rather than "en_AU.UTF-8") and, if found, LANGUAGE is set to
   the corresponding colon-separated fallback chain.

   The table has only 13 entries and changes very infrequently in upstream,
   so we embed it directly rather than reading a runtime data file.
   -------------------------------------------------------------------------- */
typedef struct
{
  const gchar *lang;          /* bare locale code, e.g. "en_AU"    */
  const gchar *language;      /* LANGUAGE chain, e.g. "en_AU:en_GB" */
} RclLanguageFallbackEntry;

static const RclLanguageFallbackEntry language_fallback_map[] =
{
  { "csb_PL",  "csb:pl"     },
  { "en_AU",   "en_AU:en_GB" },
  { "en_IE",   "en_IE:en_GB" },
  { "en_NZ",   "en_NZ:en_GB" },
  { "en_ZA",   "en_ZA:en_GB" },
  { "fr_BE",   "fr_BE:fr_FR" },
  { "fr_CA",   "fr_CA:fr_FR" },
  { "fr_CH",   "fr_CH:fr_FR" },
  { "fr_LU",   "fr_LU:fr_FR" },
  { "it_CH",   "it_CH:it_IT" },
  { "mai_IN",  "mai:hi"      },
  { "nds_DE",  "nds:de"      },
  { "szl_PL",  "szl:pl"      },
  { NULL,      NULL           }
};

/*
 * rcl_find_language_fallback:
 *
 * Look up 'lang' in language_fallback_map using an exact-match comparison,
 * matching upstream systemd-localed semantics exactly.  The caller passes
 * the raw LANG value (e.g. "en_AU" or "en_AU.UTF-8").  Because the map
 * keys use bare locale codes without an encoding suffix, a value like
 * "en_AU.UTF-8" will NOT match "en_AU" — this is intentional and matches
 * upstream behaviour (the fallback table is only consulted when the locale
 * is specified without an explicit encoding).
 *
 * Returns the LANGUAGE fallback string (static, must not be freed), or NULL
 * if no match is found.
 */
static const gchar *
rcl_find_language_fallback( const gchar *lang )
{
  guint i;

  if( !lang || lang[0] == '\0' )
    return NULL;

  for( i = 0; language_fallback_map[i].lang != NULL; i++ )
    if( g_strcmp0( lang, language_fallback_map[i].lang ) == 0 )
      return language_fallback_map[i].language;

  return NULL;
}

static gboolean
rcl_keymap_to_x11( const gchar *keymap, gchar **out_layout, gchar **out_variant )
{
  guint i;
  for( i = 0; keymap_map[i].vconsole_keymap != NULL; i++ )
  {
    if( g_strcmp0( keymap, keymap_map[i].vconsole_keymap ) == 0 )
    {
      *out_layout  = g_strdup( keymap_map[i].x11_layout );
      *out_variant = g_strdup( keymap_map[i].x11_variant );
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
rcl_x11_to_keymap( const gchar *layout, gchar **out_keymap )
{
  guint i;
  /* Match on layout alone (first component if a comma-separated list was
     passed); this is a best-effort convenience, not an authoritative
     mapping. */
  gchar *first_layout = NULL;
  const gchar *comma;
  gboolean found = FALSE;

  if( !layout || layout[0] == '\0' )
    return FALSE;

  comma = strchr( layout, ',' );
  first_layout = comma ? g_strndup( layout, (gsize)(comma - layout) )
                       : g_strdup( layout );

  for( i = 0; keymap_map[i].vconsole_keymap != NULL; i++ )
  {
    if( g_strcmp0( first_layout, keymap_map[i].x11_layout ) == 0 )
    {
      *out_keymap = g_strdup( keymap_map[i].vconsole_keymap );
      found = TRUE;
      break;
    }
  }

  g_free( first_layout );
  return found;
}

/* --------------------------------------------------------------------------
   Small file-reading helpers (KEY=value / KEY="value" files)
   -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
   X11 keyboard config (xorg.conf.d "InputClass" snippet) reader/writer.

   We read/write a minimal, fixed-shape file of the form:

     Section "InputClass"
         Identifier "system-keyboard"
         MatchIsKeyboard "on"
         Option "XkbLayout" "us"
         Option "XkbModel" "pc105"
         Option "XkbVariant" ""
         Option "XkbOptions" ""
     EndSection

   matching what systemd-localed and most distro X11 configs use. Options
   with an empty value are still emitted (most tools treat a present-but-
   empty Option the same as an absent one).
   -------------------------------------------------------------------------- */
static gchar *
extract_xkb_option( const gchar *contents, const gchar *option_name )
{
  gchar *needle = g_strdup_printf( "\"%s\"", option_name );
  gchar *pos = strstr( contents, needle );
  gchar *result = NULL;

  g_free( needle );

  if( pos )
  {
    /* Skip past the option name's closing quote, then find the next
       quoted string, which holds the value. */
    gchar *value_start = strchr( pos + 1, '"' );
    if( value_start )
    {
      value_start = strchr( value_start + 1, '"' ); /* opening quote of value */
      if( value_start )
      {
        gchar *value_end = strchr( value_start + 1, '"' );
        if( value_end )
          result = g_strndup( value_start + 1, (gsize)(value_end - value_start - 1) );
      }
    }
  }

  return result ? result : g_strdup( "" );
}

static void
read_x11_keyboard_conf( const gchar *path,
                        gchar **out_layout, gchar **out_model,
                        gchar **out_variant, gchar **out_options )
{
  gchar *contents = NULL;

  *out_layout  = g_strdup( "" );
  *out_model   = g_strdup( "" );
  *out_variant = g_strdup( "" );
  *out_options = g_strdup( "" );

  if( !g_file_get_contents( path, &contents, NULL, NULL ) )
    return;

  g_free( *out_layout );  *out_layout  = extract_xkb_option( contents, "XkbLayout" );
  g_free( *out_model );   *out_model   = extract_xkb_option( contents, "XkbModel" );
  g_free( *out_variant ); *out_variant = extract_xkb_option( contents, "XkbVariant" );
  g_free( *out_options ); *out_options = extract_xkb_option( contents, "XkbOptions" );

  g_free( contents );
}

static gboolean
write_x11_keyboard_conf( const gchar *path,
                         const gchar *layout, const gchar *model,
                         const gchar *variant, const gchar *options,
                         GError **error )
{
  GString *out;
  gchar   *dir;
  gchar   *tmp_path;
  gboolean ok;

  dir = g_path_get_dirname( path );
  if( g_mkdir_with_parents( dir, 0755 ) != 0 && errno != EEXIST )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 "mkdir -p %s: %s", dir, g_strerror(errno) );
    g_free( dir );
    return FALSE;
  }
  g_free( dir );

  out = g_string_new( NULL );
  g_string_append( out, "# Written by localed - see localed(8)\n" );
  g_string_append( out, "Section \"InputClass\"\n" );
  g_string_append( out, "    Identifier \"system-keyboard\"\n" );
  g_string_append( out, "    MatchIsKeyboard \"on\"\n" );
  g_string_append_printf( out, "    Option \"XkbLayout\" \"%s\"\n",  layout  ? layout  : "" );
  g_string_append_printf( out, "    Option \"XkbModel\" \"%s\"\n",   model   ? model   : "" );
  g_string_append_printf( out, "    Option \"XkbVariant\" \"%s\"\n", variant ? variant : "" );
  g_string_append_printf( out, "    Option \"XkbOptions\" \"%s\"\n", options ? options : "" );
  g_string_append( out, "EndSection\n" );

  tmp_path = g_strdup_printf( "%s.tmp", path );
  ok = g_file_set_contents( tmp_path, out->str, (gssize)out->len, error );

  if( ok && g_rename( tmp_path, path ) != 0 )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 "rename(%s, %s): %s", tmp_path, path, g_strerror(errno) );
    ok = FALSE;
  }

  g_free( tmp_path );
  g_string_free( out, TRUE );
  return ok;
}

/* --------------------------------------------------------------------------
   Slackware rc.keymap reader/writer.

   Slackware's console keymap is configured via /etc/rc.d/rc.keymap, a small
   shell script of the form:

     #!/bin/bash
     # Load the keyboard map.  More maps are in /usr/share/kbd/keymaps.
     if [ -x /usr/bin/loadkeys ]; then
       /usr/bin/loadkeys us
     fi

   We parse the loadkeys argument on read and regenerate the whole script on
   write so the file is always in a clean, known format.

   VConsoleKeymapToggle has no Slackware equivalent so it is persisted in a
   small sidecar file (RCL_RC_KEYMAP_TOGGLE_FILE).  The sidecar contains
   only the bare keymap name on a single line; an absent or empty file means
   no toggle keymap is configured.
   -------------------------------------------------------------------------- */

/* Extract the keymap argument from a Slackware rc.keymap script.
 * Returns a newly-allocated string, or NULL/empty-string when not found. */
static gchar *
rcl_read_rc_keymap( const gchar *path )
{
  gchar  *contents = NULL;
  gchar **lines;
  gchar  *result   = NULL;
  gsize   i;

  if( !g_file_get_contents( path, &contents, NULL, NULL ) )
    return g_strdup( "" );

  lines = g_strsplit( contents, "\n", -1 );
  g_free( contents );

  for( i = 0; lines[i] != NULL && result == NULL; i++ )
  {
    gchar *line = g_strstrip( lines[i] );

    /* Match:  /usr/bin/loadkeys <keymap>
     *    or:  loadkeys <keymap>
     * Ignore comment lines and blank lines. */
    if( line[0] == '#' || line[0] == '\0' )
      continue;

    {
      const gchar *p = strstr( line, "loadkeys" );
      if( p )
      {
        p += strlen( "loadkeys" );
        /* skip whitespace */
        while( *p == ' ' || *p == '\t' )
          p++;
        if( *p && *p != '#' && *p != '\n' )
        {
          /* read until whitespace or end */
          const gchar *end = p;
          while( *end && *end != ' ' && *end != '\t' && *end != '\n' )
            end++;
          result = g_strndup( p, (gsize)(end - p) );
        }
      }
    }
  }

  g_strfreev( lines );
  return result ? result : g_strdup( "" );
}

/* Write a Slackware rc.keymap script for the given keymap.
 * An empty keymap writes a no-op script (preserves the file but calls
 * no loadkeys, consistent with "keymap cleared" semantics). */
static gboolean
rcl_write_rc_keymap( const gchar *path, const gchar *keymap, GError **error )
{
  GString *out;
  gchar   *tmp_path;
  gchar   *dir;
  gboolean ok;

  dir = g_path_get_dirname( path );
  if( g_mkdir_with_parents( dir, 0755 ) != 0 && errno != EEXIST )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 "mkdir -p %s: %s", dir, g_strerror(errno) );
    g_free( dir );
    return FALSE;
  }
  g_free( dir );

  out = g_string_new( NULL );
  g_string_append( out,
    "#!/bin/bash\n"
    "# Written by localed - see localed(8)\n"
    "# More keymaps are in /usr/share/kbd/keymaps\n"
    "if [ -x /usr/bin/loadkeys ]; then\n" );

  if( keymap && keymap[0] != '\0' )
    g_string_append_printf( out, "  /usr/bin/loadkeys %s\n", keymap );
  else
    g_string_append( out, "  : # no keymap configured\n" );

  g_string_append( out, "fi\n" );

  tmp_path = g_strdup_printf( "%s.tmp", path );
  ok = g_file_set_contents( tmp_path, out->str, (gssize)out->len, error );

  if( ok )
  {
    /* Make the script executable */
    g_chmod( tmp_path, 0755 );

    if( g_rename( tmp_path, path ) != 0 )
    {
      g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                   "rename(%s, %s): %s", tmp_path, path, g_strerror(errno) );
      ok = FALSE;
    }
  }

  g_free( tmp_path );
  g_string_free( out, TRUE );
  return ok;
}

/* Read the toggle keymap from the sidecar file.
 * Returns a newly-allocated string (empty if absent). */
static gchar *
rcl_read_rc_keymap_toggle( const gchar *path )
{
  gchar *contents = NULL;
  gchar *result;

  if( !g_file_get_contents( path, &contents, NULL, NULL ) )
    return g_strdup( "" );

  result = g_strstrip( contents );  /* in-place, returns same pointer */
  result = g_strdup( result );
  g_free( contents );
  return result;
}

/* Write (or remove) the toggle keymap sidecar file.
 * An empty keymap_toggle removes the file. */
static gboolean
rcl_write_rc_keymap_toggle( const gchar *path,
                             const gchar *keymap_toggle,
                             GError     **error )
{
  if( !keymap_toggle || keymap_toggle[0] == '\0' )
  {
    /* Remove the sidecar; ENOENT is fine */
    if( g_unlink( path ) != 0 && errno != ENOENT )
    {
      g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                   "unlink(%s): %s", path, g_strerror(errno) );
      return FALSE;
    }
    return TRUE;
  }

  {
    gchar    *line     = g_strdup_printf( "%s\n", keymap_toggle );
    gchar    *tmp_path = g_strdup_printf( "%s.tmp", path );
    gboolean  ok       = g_file_set_contents( tmp_path, line, -1, error );

    if( ok && g_rename( tmp_path, path ) != 0 )
    {
      g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                   "rename(%s, %s): %s", tmp_path, path, g_strerror(errno) );
      ok = FALSE;
    }

    g_free( tmp_path );
    g_free( line );
    return ok;
  }
}

/* --------------------------------------------------------------------------
   Input validation
   -------------------------------------------------------------------------- */

/* Locale values (e.g. "en_US.UTF-8") and the LANGUAGE priority list
   (e.g. "en_US:en_GB:en") share a conservative safe charset. */
static gboolean
rcl_locale_value_is_safe( const gchar *value )
{
  const gchar *p;

  if( !value )
    return FALSE;

  for( p = value; *p; p++ )
  {
    guchar c = (guchar) *p;
    if( g_ascii_isalnum(c) )
      continue;
    switch( c )
    {
      case '_': case '.': case '-': case '@': case ':': case ',':
        continue;
      default:
        return FALSE;
    }
  }
  return TRUE;
}

/* Console keymap names and X11 layout/model/variant/options strings forbid
   quotes, backslashes, and control characters. */
static gboolean
rcl_keyboard_value_is_safe( const gchar *value )
{
  const gchar *p;

  if( !value )
    return FALSE;

  for( p = value; *p; p++ )
  {
    guchar c = (guchar) *p;
    if( c < 0x20 || c == 0x7f )
      return FALSE;
    if( c == '"' || c == '\\' )
      return FALSE;
  }
  return TRUE;
}

/* --------------------------------------------------------------------------
   Property refresh
   -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
   Slackware lang.sh reader/writer.

   /etc/profile.d/lang.sh is sourced by /etc/profile at login.  It contains
   shell variable assignments of the form:

     export LANG="en_US.UTF-8"
     export LC_COLLATE="C"

   We parse only the lines that match "export KEY=..." for known locale
   variable names, and regenerate the file on write with the same format so
   it stays valid POSIX sh.

   On read:  returns a GHashTable (key->value) of locale variable assignments.
   On write: produces an atomic temp-file + rename to avoid partial writes.
   Existing lines for unknown variable names (comments, PATH exports, etc.)
   are preserved on write via a read-modify-write strategy.
   -------------------------------------------------------------------------- */

/*
 * rcl_read_lang_sh:
 * Parse /etc/profile.d/lang.sh and return a GHashTable (key->value) of
 * known locale variable assignments.  Lines of the form
 *   export KEY=value
 *   export KEY="value"
 * are recognised; the "export " prefix and optional quotes are stripped.
 * Returns an empty table (never NULL) when the file is missing.
 * The caller must g_hash_table_unref() the result.
 */
static GHashTable *
rcl_read_lang_sh( const gchar *path )
{
  GHashTable *table = g_hash_table_new_full( g_str_hash, g_str_equal,
                                              g_free, g_free );
  gchar  *contents = NULL;
  gchar **lines;
  gsize   i;

  if( !g_file_get_contents( path, &contents, NULL, NULL ) )
    return table;

  lines = g_strsplit( contents, "\n", -1 );
  g_free( contents );

  for( i = 0; lines[i] != NULL; i++ )
  {
    gchar *line = g_strstrip( lines[i] );
    gchar *rest, *eq, *key, *val;
    gsize  len;

    if( line[0] == '#' || line[0] == '\0' )
      continue;

    /* Must start with "export " */
    if( strncmp( line, "export ", 7 ) != 0 )
      continue;

    rest = line + 7;
    while( *rest == ' ' || *rest == '\t' )
      rest++;

    eq = strchr( rest, '=' );
    if( !eq )
      continue;

    key = g_strndup( rest, (gsize)(eq - rest) );
    g_strstrip( key );

    /* Only store known locale variables */
    if( !rcl_locale_variable_is_known( key ) )
    {
      g_free( key );
      continue;
    }

    val = g_strdup( eq + 1 );
    g_strstrip( val );

    /* Strip surrounding double quotes */
    len = strlen( val );
    if( len >= 2 && val[0] == '"' && val[len-1] == '"' )
    {
      gchar *unquoted = g_strndup( val + 1, len - 2 );
      g_free( val );
      val = unquoted;
    }

    g_hash_table_replace( table, key, val );
  }

  g_strfreev( lines );
  return table;
}

/*
 * rcl_write_lang_sh:
 * Write updated locale variable assignments back to /etc/profile.d/lang.sh,
 * preserving any lines not in known_keys (comments, unrecognised exports,
 * etc.) using a read-modify-write strategy.
 *
 * known_keys / new_values are parallel NULL-terminated arrays of const gchar *.
 * A NULL or empty new_values[i] removes that key from the file.
 * All written values are double-quoted and preceded by "export ".
 *
 * The write is atomic (temp file + rename).
 */
static gboolean
rcl_write_lang_sh( const gchar         *path,
                   const gchar *const  *known_keys,
                   const gchar *const  *new_values,
                   GError             **error )
{
  GHashTable    *new_vals;
  GString       *out;
  gchar         *contents = NULL;
  gchar        **lines;
  gsize          i;
  gchar         *tmp_path;
  gboolean       ok;
  gchar         *dir;

  dir = g_path_get_dirname( path );
  if( g_mkdir_with_parents( dir, 0755 ) != 0 && errno != EEXIST )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 "mkdir -p %s: %s", dir, g_strerror(errno) );
    g_free( dir );
    return FALSE;
  }
  g_free( dir );

  /* Build the updated value map from the incoming arrays */
  new_vals = g_hash_table_new( g_str_hash, g_str_equal );
  for( i = 0; known_keys[i] != NULL; i++ )
    g_hash_table_replace( new_vals,
                          (gpointer) known_keys[i],
                          (gpointer) (new_values[i] ? new_values[i] : "") );

  out = g_string_new( "# Written by localed - see localed(8)\n" );

  /* Re-read the file to preserve non-locale lines */
  if( g_file_get_contents( path, &contents, NULL, NULL ) )
  {
    lines = g_strsplit( contents, "\n", -1 );
    g_free( contents );

    for( i = 0; lines[i] != NULL; i++ )
    {
      gchar *line = g_strstrip( lines[i] );
      gchar *rest, *eq, *key;
      gboolean is_locale_export = FALSE;

      if( line[0] == '\0' )
        continue;

      /* Drop old "Written by localed" header; we wrote a fresh one above */
      if( strstr( line, "Written by localed" ) )
        continue;

      if( line[0] == '#' )
      {
        g_string_append_printf( out, "%s\n", line );
        continue;
      }

      if( strncmp( line, "export ", 7 ) == 0 )
      {
        rest = line + 7;
        while( *rest == ' ' || *rest == '\t' )
          rest++;
        eq = strchr( rest, '=' );
        if( eq )
        {
          key = g_strndup( rest, (gsize)(eq - rest) );
          g_strstrip( key );
          if( rcl_locale_variable_is_known( key ) )
            is_locale_export = TRUE;
          g_free( key );
        }
      }

      if( !is_locale_export )
        g_string_append_printf( out, "%s\n", line );
      /* Known locale exports are rewritten below in canonical order */
    }

    g_strfreev( lines );
  }

  /* Append locale variables in canonical order */
  {
    guint j;
    for( j = 0; locale_variable_names[j] != NULL; j++ )
    {
      const gchar *val = g_hash_table_lookup( new_vals,
                                              locale_variable_names[j] );
      if( val && val[0] != '\0' )
        g_string_append_printf( out, "export %s=\"%s\"\n",
                                locale_variable_names[j], val );
    }
  }

  g_hash_table_unref( new_vals );

  tmp_path = g_strdup_printf( "%s.tmp", path );
  ok = g_file_set_contents( tmp_path, out->str, (gssize)out->len, error );

  if( ok && g_rename( tmp_path, path ) != 0 )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 "rename(%s, %s): %s", tmp_path, path, g_strerror(errno) );
    ok = FALSE;
  }

  g_free( tmp_path );
  g_string_free( out, TRUE );
  return ok;
}

/* Build the canonical "KEY=value" GStrv for the Locale property from
   whatever known LANG/LANGUAGE/LC_xxx keys are present in RCL_LANG_SH_FILE. */
static gchar **
read_locale_strv( void )
{
  GHashTable *pairs = rcl_read_lang_sh( RCL_LANG_SH_FILE );
  GPtrArray  *arr   = g_ptr_array_new();
  guint       i;

  for( i = 0; locale_variable_names[i] != NULL; i++ )
  {
    const gchar *val = g_hash_table_lookup( pairs, locale_variable_names[i] );
    if( val && val[0] != '\0' )
      g_ptr_array_add( arr, g_strdup_printf( "%s=%s", locale_variable_names[i], val ) );
  }

  g_ptr_array_add( arr, NULL );
  g_hash_table_unref( pairs );

  return (gchar **) g_ptr_array_free( arr, FALSE );
}

static gboolean
strv_equal( gchar **a, gchar **b )
{
  guint i;

  if( !a || !b )
    return a == b;

  for( i = 0; a[i] != NULL && b[i] != NULL; i++ )
    if( g_strcmp0( a[i], b[i] ) != 0 )
      return FALSE;

  return a[i] == NULL && b[i] == NULL;
}

/*
 * Populate (or refresh) all cached properties from the underlying files,
 * then emit PropertiesChanged for anything that actually changed.
 */
void
rcl_daemon_sync_dbus_properties( RclLocaleDaemon *daemon )
{
  RclLocaleDaemonPrivate *priv = daemon->priv;
  GVariantBuilder changed_props;
  gboolean any_changed = FALSE;

  g_variant_builder_init( &changed_props, G_VARIANT_TYPE("a{sv}") );

#define UPDATE_STR_PROP( field, new_val, dbus_name )              \
  do {                                                            \
    gchar *_nv = (new_val);                                       \
    if( g_strcmp0( priv->field, _nv ) != 0 )                     \
    {                                                             \
      g_free( priv->field );                                      \
      priv->field = _nv ? g_strdup(_nv) : g_strdup("");          \
      g_variant_builder_add( &changed_props, "{sv}",             \
                             dbus_name,                           \
                             g_variant_new_string(priv->field) );\
      any_changed = TRUE;                                         \
    }                                                             \
    g_free( _nv );                                                \
  } while(0)

  /* ---- Locale (array of "KEY=value") ---- */
  {
    gchar **new_locale = read_locale_strv();
    if( !strv_equal( priv->locale, new_locale ) )
    {
      g_strfreev( priv->locale );
      priv->locale = new_locale;
      g_variant_builder_add( &changed_props, "{sv}", "Locale",
                             g_variant_new_strv( (const gchar * const *) priv->locale, -1 ) );
      any_changed = TRUE;
    }
    else
    {
      g_strfreev( new_locale );
    }
  }

  /* ---- rc.keymap (Slackware console keyboard) ---- */
  {
    UPDATE_STR_PROP( vconsole_keymap,
                     rcl_read_rc_keymap( RCL_RC_KEYMAP_FILE ),
                     "VConsoleKeymap" );
    /* Toggle is persisted in a small sidecar file */
    UPDATE_STR_PROP( vconsole_keymap_toggle,
                     rcl_read_rc_keymap_toggle( RCL_RC_KEYMAP_TOGGLE_FILE ),
                     "VConsoleKeymapToggle" );
  }

  /* ---- X11 keyboard config ---- */
  {
    gchar *layout, *model, *variant, *options;
    read_x11_keyboard_conf( RCL_X11_KEYBOARD_FILE, &layout, &model, &variant, &options );
    UPDATE_STR_PROP( x11_layout,  layout,  "X11Layout" );
    UPDATE_STR_PROP( x11_model,   model,   "X11Model" );
    UPDATE_STR_PROP( x11_variant, variant, "X11Variant" );
    UPDATE_STR_PROP( x11_options, options, "X11Options" );
  }

#undef UPDATE_STR_PROP

  /* Emit PropertiesChanged only if something actually changed */
  if( any_changed && priv->connection && priv->registration_id > 0 )
  {
    GError *err = NULL;
    g_dbus_connection_emit_signal(
      priv->connection,
      NULL,
      "/org/freedesktop/locale1",
      "org.freedesktop.DBus.Properties",
      "PropertiesChanged",
      g_variant_new( "(sa{sv}as)",
                     "org.freedesktop.locale1",
                     &changed_props,
                     NULL ),
      &err );

    if( err )
    {
      g_warning( "PropertiesChanged emission failed: %s", err->message );
      g_error_free( err );
    }
  }
  else
  {
    g_variant_builder_clear( &changed_props );
  }
}

/* --------------------------------------------------------------------------
   PolKit authorisation (asynchronous)

   The authorisation check is performed asynchronously so the agent password
   dialog never blocks the single-threaded GMainLoop.  Each in-flight call
   carries an AuthCtx describing the operation to run once (and only if) it
   is authorised.  The operation handlers below complete the invocation
   exactly once each.
   -------------------------------------------------------------------------- */

typedef enum
{
  OP_SET_LOCALE,
  OP_SET_VCONSOLE_KEYBOARD,
  OP_SET_X11_KEYBOARD,
} LocaleOp;

typedef struct
{
  RclLocaleDaemon        *daemon;       /* owned ref */
  GDBusMethodInvocation  *invocation;   /* completed exactly once */
  LocaleOp                op;

  gchar  **locale;        /* OP_SET_LOCALE: owned GStrv */

  gchar   *keymap;        /* OP_SET_VCONSOLE_KEYBOARD */
  gchar   *keymap_toggle;

  gchar   *x11_layout;    /* OP_SET_X11_KEYBOARD */
  gchar   *x11_model;
  gchar   *x11_variant;
  gchar   *x11_options;

  gboolean convert;       /* shared by the two keyboard ops */
} AuthCtx;

static void
auth_ctx_free( AuthCtx *ctx )
{
  if( !ctx )
    return;
  g_clear_object( &ctx->daemon );
  g_strfreev( ctx->locale );
  g_free( ctx->keymap );
  g_free( ctx->keymap_toggle );
  g_free( ctx->x11_layout );
  g_free( ctx->x11_model );
  g_free( ctx->x11_variant );
  g_free( ctx->x11_options );
  g_free( ctx );
}

/* ---- operation handlers: run only after a successful authorisation ---- */

static void
do_set_locale( RclLocaleDaemon *daemon, gchar **locale,
               GDBusMethodInvocation *invocation )
{
  const gchar *known_keys[ G_N_ELEMENTS(locale_variable_names) ];
  const gchar *new_values[ G_N_ELEMENTS(locale_variable_names) ];
  guint   i, n_known = 0;
  GError *error = NULL;

  /*
   * Bare-locale shorthand: if exactly one string with no '=' is provided,
   * treat it as LANG=<value>, matching upstream systemd-localed behaviour.
   * Example: SetLocale(["en_US.UTF-8"], true)  ->  LANG=en_US.UTF-8
   */
  if( locale[0] != NULL && locale[1] == NULL && strchr(locale[0], '=') == NULL )
  {
    gchar *bare = locale[0];
    if( !rcl_locale_value_is_safe( bare ) )
    {
      g_dbus_method_invocation_return_error( invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Invalid locale specification: '%s'", bare );
      return;
    }
    locale[0] = g_strdup_printf( "LANG=%s", bare );
    g_free( bare );
  }

  /* Validate every entry first; reject the whole call on the first bad one,
     same semantics as systemd-localed. */
  for( i = 0; locale[i] != NULL; i++ )
  {
    gchar *eq = strchr( locale[i], '=' );
    gchar *key, *val;

    if( !eq )
    {
      g_dbus_method_invocation_return_error( invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Locale assignment '%s' is not of the form KEY=value", locale[i] );
      return;
    }

    key = g_strndup( locale[i], (gsize)(eq - locale[i]) );
    val = eq + 1;

    if( !rcl_locale_variable_is_known( key ) )
    {
      g_dbus_method_invocation_return_error( invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Unknown locale variable '%s'", key );
      g_free( key );
      return;
    }

    if( val[0] != '\0' && !rcl_locale_value_is_safe( val ) )
    {
      g_dbus_method_invocation_return_error( invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Invalid value for %s", key );
      g_free( key );
      return;
    }

    g_free( key );
  }

  /* Build the known_keys/new_values pair arrays. An entry not present in
     the incoming array is left untouched in the file (SetLocale is allowed
     to update a subset, e.g. just LANG). */
  for( i = 0; locale_variable_names[i] != NULL; i++ )
  {
    guint j;
    const gchar *match = NULL;

    for( j = 0; locale[j] != NULL; j++ )
    {
      gchar *eq = strchr( locale[j], '=' );
      gsize  klen = eq ? (gsize)(eq - locale[j]) : 0;
      if( eq && strncmp( locale[j], locale_variable_names[i], klen ) == 0 &&
          locale_variable_names[i][klen] == '\0' )
      {
        match = eq + 1;
        break;
      }
    }

    if( match )
    {
      known_keys[n_known] = locale_variable_names[i];
      new_values[n_known] = match;
      n_known++;
    }
  }
  known_keys[n_known] = NULL;
  new_values[n_known] = NULL;

  /*
   * LANGUAGE auto-derivation (gap 1 vs systemd 257).
   *
   * If LANG was supplied in this call but LANGUAGE was not, check the
   * language-fallback table.  If a match is found, inject LANGUAGE into
   * the write arrays so it is persisted atomically in the same
   * write_key_value_file() call.
   *
   * Conditions for the injection:
   *   a) LANG= is being set by this call (match != NULL for "LANG")
   *   b) LANGUAGE= is NOT being set by this call
   *   c) rcl_find_language_fallback() returns a non-NULL chain
   *
   * The injected LANGUAGE value is appended to known_keys/new_values
   * (the arrays are sized G_N_ELEMENTS(locale_variable_names) so there is
   * always room for one more entry before the NULL sentinel).
   */
  {
    gboolean lang_set      = FALSE;
    gboolean language_set  = FALSE;
    guint    k;

    for( k = 0; k < n_known; k++ )
    {
      if( g_strcmp0( known_keys[k], "LANG"     ) == 0 ) lang_set     = TRUE;
      if( g_strcmp0( known_keys[k], "LANGUAGE" ) == 0 ) language_set = TRUE;
    }

    if( lang_set && !language_set )
    {
      /* Find the LANG value we are about to write */
      const gchar *lang_value = NULL;
      for( k = 0; k < n_known; k++ )
        if( g_strcmp0( known_keys[k], "LANG" ) == 0 )
        {
          lang_value = new_values[k];
          break;
        }

      if( lang_value && lang_value[0] != '\0' )
      {
        const gchar *fallback = rcl_find_language_fallback( lang_value );
        if( fallback )
        {
          g_debug( "LANG=%s -> auto-setting LANGUAGE=%s", lang_value, fallback );
          known_keys[n_known] = "LANGUAGE";
          new_values[n_known] = fallback;
          n_known++;
          known_keys[n_known] = NULL;
          new_values[n_known] = NULL;
        }
      }
    }
  }

  if( !rcl_write_lang_sh( RCL_LANG_SH_FILE, known_keys, new_values, &error ) )
  {
    g_dbus_method_invocation_return_gerror( invocation, error );
    g_error_free( error );
    return;
  }

  rcl_daemon_sync_dbus_properties( daemon );
  g_dbus_method_invocation_return_value( invocation, NULL );
}

static void
do_set_vconsole_keyboard( RclLocaleDaemon *daemon,
                          const gchar *keymap, const gchar *keymap_toggle,
                          gboolean convert,
                          GDBusMethodInvocation *invocation )
{
  GError *error = NULL;

  if( keymap[0] != '\0' && !rcl_keyboard_value_is_safe( keymap ) )
  {
    g_dbus_method_invocation_return_error( invocation,
      G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Invalid keymap '%s'", keymap );
    return;
  }
  if( keymap_toggle[0] != '\0' && !rcl_keyboard_value_is_safe( keymap_toggle ) )
  {
    g_dbus_method_invocation_return_error( invocation,
      G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
      "Invalid keymap toggle '%s'", keymap_toggle );
    return;
  }

  if( !rcl_write_rc_keymap( RCL_RC_KEYMAP_FILE, keymap, &error ) )
  {
    g_dbus_method_invocation_return_gerror( invocation, error );
    g_error_free( error );
    return;
  }

  /* Persist toggle in sidecar (empty value removes the file) */
  if( !rcl_write_rc_keymap_toggle( RCL_RC_KEYMAP_TOGGLE_FILE, keymap_toggle, &error ) )
  {
    g_warning( "could not write toggle sidecar: %s", error->message );
    g_clear_error( &error );
  }

  if( convert && keymap[0] != '\0' )
  {
    gchar *x11_layout = NULL, *x11_variant = NULL;
    if( rcl_keymap_to_x11( keymap, &x11_layout, &x11_variant ) )
    {
      GError *xerr = NULL;
      RclLocaleDaemonPrivate *priv = daemon->priv;
      if( !write_x11_keyboard_conf( RCL_X11_KEYBOARD_FILE,
                                    x11_layout, priv->x11_model,
                                    x11_variant, priv->x11_options, &xerr ) )
      {
        g_warning( "convert: could not update X11 keyboard config: %s", xerr->message );
        g_clear_error( &xerr );
      }
      g_free( x11_layout );
      g_free( x11_variant );
    }
    else
    {
      g_debug( "convert: no built-in X11 mapping for keymap '%s'", keymap );
    }
  }

  rcl_daemon_sync_dbus_properties( daemon );
  g_dbus_method_invocation_return_value( invocation, NULL );
}

static void
do_set_x11_keyboard( RclLocaleDaemon *daemon,
                     const gchar *layout, const gchar *model,
                     const gchar *variant, const gchar *options,
                     gboolean convert,
                     GDBusMethodInvocation *invocation )
{
  GError *error = NULL;

  if( ( layout[0]  && !rcl_keyboard_value_is_safe(layout) )  ||
      ( model[0]   && !rcl_keyboard_value_is_safe(model) )   ||
      ( variant[0] && !rcl_keyboard_value_is_safe(variant) ) ||
      ( options[0] && !rcl_keyboard_value_is_safe(options) ) )
  {
    g_dbus_method_invocation_return_error( invocation,
      G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
      "Invalid X11 keyboard configuration value" );
    return;
  }

  if( !write_x11_keyboard_conf( RCL_X11_KEYBOARD_FILE,
                                layout, model, variant, options, &error ) )
  {
    g_dbus_method_invocation_return_gerror( invocation, error );
    g_error_free( error );
    return;
  }

  if( convert && layout[0] != '\0' )
  {
    gchar *vc_keymap = NULL;
    if( rcl_x11_to_keymap( layout, &vc_keymap ) )
    {
      GError *kerr = NULL;
      if( !rcl_write_rc_keymap( RCL_RC_KEYMAP_FILE, vc_keymap, &kerr ) )
      {
        g_warning( "convert: could not update rc.keymap: %s", kerr->message );
        g_clear_error( &kerr );
      }
      g_free( vc_keymap );
    }
    else
    {
      g_debug( "convert: no built-in console mapping for X11 layout '%s'", layout );
    }
  }

  rcl_daemon_sync_dbus_properties( daemon );
  g_dbus_method_invocation_return_value( invocation, NULL );
}

/* ---- async authorisation callback ---- */

static void
on_check_done( GObject *source, GAsyncResult *res, gpointer user_data )
{
  AuthCtx                   *ctx    = user_data;
  PolkitAuthorizationResult *result;
  GError                    *error  = NULL;

  result = polkit_authority_check_authorization_finish(
             POLKIT_AUTHORITY(source), res, &error );

  if( !result )
  {
    g_dbus_method_invocation_return_gerror( ctx->invocation, error );
    g_error_free( error );
    auth_ctx_free( ctx );
    return;
  }

  if( !polkit_authorization_result_get_is_authorized( result ) )
  {
    g_dbus_method_invocation_return_error( ctx->invocation,
      G_DBUS_ERROR, G_DBUS_ERROR_AUTH_FAILED,
      "Not authorised to perform the requested action" );
    g_object_unref( result );
    auth_ctx_free( ctx );
    return;
  }
  g_object_unref( result );

  switch( ctx->op )
  {
    case OP_SET_LOCALE:
      do_set_locale( ctx->daemon, ctx->locale, ctx->invocation );
      break;
    case OP_SET_VCONSOLE_KEYBOARD:
      do_set_vconsole_keyboard( ctx->daemon, ctx->keymap, ctx->keymap_toggle,
                                ctx->convert, ctx->invocation );
      break;
    case OP_SET_X11_KEYBOARD:
      do_set_x11_keyboard( ctx->daemon, ctx->x11_layout, ctx->x11_model,
                           ctx->x11_variant, ctx->x11_options,
                           ctx->convert, ctx->invocation );
      break;
  }

  auth_ctx_free( ctx );
}

/* ---- async authorisation starter (takes ownership of ctx) ---- */

static void
check_polkit_async( AuthCtx *ctx, const gchar *action_id, gboolean interactive )
{
  RclLocaleDaemonPrivate        *priv = ctx->daemon->priv;
  PolkitSubject                 *subject;
  PolkitCheckAuthorizationFlags  flags;
  const gchar                   *sender;

  /* Acquire the authority handle lazily if the startup fetch failed.  This
     is a fast, non-interactive call (it only connects to polkitd). */
  if( priv->authority == NULL )
  {
    GError *err = NULL;
    priv->authority = polkit_authority_get_sync( NULL, &err );
    if( priv->authority == NULL )
    {
      g_dbus_method_invocation_return_gerror( ctx->invocation, err );
      g_error_free( err );
      auth_ctx_free( ctx );
      return;
    }
  }

  sender  = g_dbus_method_invocation_get_sender( ctx->invocation );
  subject = polkit_system_bus_name_new( sender );
  flags   = interactive
              ? POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION
              : POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE;

  polkit_authority_check_authorization(
    priv->authority, subject, action_id, NULL, flags,
    NULL /* cancellable */, on_check_done, ctx );

  g_object_unref( subject );
}

/* --------------------------------------------------------------------------
   D-Bus property getter
   -------------------------------------------------------------------------- */
static GVariant *
handle_get_property( GDBusConnection  *connection,
                     const gchar      *sender,
                     const gchar      *object_path,
                     const gchar      *interface_name,
                     const gchar      *property_name,
                     GError          **error,
                     gpointer          user_data )
{
  RclLocaleDaemon        *daemon = RCL_LOCALE_DAEMON( user_data );
  RclLocaleDaemonPrivate *priv   = daemon->priv;

#define RETURN_STR(field) \
  return g_variant_new_string( priv->field ? priv->field : "" )

  if( g_strcmp0(property_name, "Locale") == 0 )
    return g_variant_new_strv( (const gchar * const *) priv->locale, -1 );

  if( g_strcmp0(property_name, "VConsoleKeymap"      ) == 0 ) RETURN_STR(vconsole_keymap);
  if( g_strcmp0(property_name, "VConsoleKeymapToggle") == 0 ) RETURN_STR(vconsole_keymap_toggle);
  if( g_strcmp0(property_name, "X11Layout"           ) == 0 ) RETURN_STR(x11_layout);
  if( g_strcmp0(property_name, "X11Model"            ) == 0 ) RETURN_STR(x11_model);
  if( g_strcmp0(property_name, "X11Variant"          ) == 0 ) RETURN_STR(x11_variant);
  if( g_strcmp0(property_name, "X11Options"          ) == 0 ) RETURN_STR(x11_options);

#undef RETURN_STR

  g_set_error( error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
               "Unknown property '%s'", property_name );
  return NULL;
}

/* Properties are read-only via D-Bus; all changes go through Set* methods. */
static gboolean
handle_set_property( GDBusConnection  *connection,
                     const gchar      *sender,
                     const gchar      *object_path,
                     const gchar      *interface_name,
                     const gchar      *property_name,
                     GVariant         *value,
                     GError          **error,
                     gpointer          user_data )
{
  g_set_error( error, G_DBUS_ERROR, G_DBUS_ERROR_PROPERTY_READ_ONLY,
               "Property '%s' is read-only; use the Set* methods", property_name );
  return FALSE;
}

/* --------------------------------------------------------------------------
   D-Bus method handler
   -------------------------------------------------------------------------- */
static void
handle_method_call( GDBusConnection       *connection,
                    const gchar           *sender,
                    const gchar           *object_path,
                    const gchar           *interface_name,
                    const gchar           *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data )
{
  RclLocaleDaemon *daemon = RCL_LOCALE_DAEMON( user_data );
  AuthCtx         *ctx;

  if( g_strcmp0(method_name, "SetLocale") == 0 )
  {
    gchar    **locale;
    gboolean   interactive;
    GVariant  *locale_v;

    g_variant_get( parameters, "(@asb)", &locale_v, &interactive );
    locale = g_variant_dup_strv( locale_v, NULL );
    g_variant_unref( locale_v );

    ctx = g_new0( AuthCtx, 1 );
    ctx->daemon     = g_object_ref( daemon );
    ctx->invocation = invocation;
    ctx->op         = OP_SET_LOCALE;
    ctx->locale     = locale;

    check_polkit_async( ctx, RCL_LOCALE_POLKIT_SET_LOCALE, interactive );
    return;
  }

  if( g_strcmp0(method_name, "SetVConsoleKeyboard") == 0 )
  {
    const gchar *keymap, *keymap_toggle;
    gboolean     convert, interactive;

    g_variant_get( parameters, "(&s&sbb)", &keymap, &keymap_toggle,
                   &convert, &interactive );

    ctx = g_new0( AuthCtx, 1 );
    ctx->daemon         = g_object_ref( daemon );
    ctx->invocation     = invocation;
    ctx->op             = OP_SET_VCONSOLE_KEYBOARD;
    ctx->keymap         = g_strdup( keymap );
    ctx->keymap_toggle  = g_strdup( keymap_toggle );
    ctx->convert        = convert;

    check_polkit_async( ctx, RCL_LOCALE_POLKIT_SET_KEYBOARD, interactive );
    return;
  }

  if( g_strcmp0(method_name, "SetX11Keyboard") == 0 )
  {
    const gchar *layout, *model, *variant, *options;
    gboolean     convert, interactive;

    g_variant_get( parameters, "(&s&s&s&sbb)", &layout, &model, &variant,
                   &options, &convert, &interactive );

    ctx = g_new0( AuthCtx, 1 );
    ctx->daemon      = g_object_ref( daemon );
    ctx->invocation  = invocation;
    ctx->op          = OP_SET_X11_KEYBOARD;
    ctx->x11_layout  = g_strdup( layout );
    ctx->x11_model   = g_strdup( model );
    ctx->x11_variant = g_strdup( variant );
    ctx->x11_options = g_strdup( options );
    ctx->convert     = convert;

    check_polkit_async( ctx, RCL_LOCALE_POLKIT_SET_KEYBOARD, interactive );
    return;
  }

  g_dbus_method_invocation_return_error( invocation,
    G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
    "Unknown method '%s'", method_name );
}

/* --------------------------------------------------------------------------
   D-Bus vtable
   -------------------------------------------------------------------------- */
static const GDBusInterfaceVTable interface_vtable = {
  handle_method_call,
  handle_get_property,
  handle_set_property,
};

/* --------------------------------------------------------------------------
   RclDaemon virtual method implementations
   -------------------------------------------------------------------------- */

RclDaemon *
rcl_daemon_new( void )
{
  return RCL_DAEMON( g_object_new(RCL_TYPE_LOCALE_DAEMON, NULL) );
}

gboolean
rcl_daemon_startup( RclDaemon *daemon, GDBusConnection *connection )
{
  RclLocaleDaemon        *self = RCL_LOCALE_DAEMON( daemon );
  RclLocaleDaemonPrivate *priv = self->priv;
  GDBusInterfaceInfo     *iface_info;
  GError *error = NULL;

  priv->connection    = g_object_ref( connection );
  priv->introspection = g_dbus_node_info_new_for_xml(
                          locale1_introspection_xml, &error );
  if( !priv->introspection )
  {
    g_warning( "Failed to parse introspection XML: %s", error->message );
    g_error_free( error );
    return FALSE;
  }

  iface_info = g_dbus_node_info_lookup_interface(
                 priv->introspection, "org.freedesktop.locale1" );

  priv->registration_id = g_dbus_connection_register_object(
    connection,
    "/org/freedesktop/locale1",
    iface_info,
    &interface_vtable,
    self,
    NULL,
    &error );

  if( priv->registration_id == 0 )
  {
    g_warning( "Could not register D-Bus object: %s", error->message );
    g_error_free( error );
    return FALSE;
  }

  rcl_daemon_sync_dbus_properties( self );

  /* Acquire the PolKit authority once up front.  This is non-interactive and
     fast; if polkitd is not reachable yet we retry lazily on first use. */
  {
    GError *pk_error = NULL;
    priv->authority = polkit_authority_get_sync( NULL, &pk_error );
    if( priv->authority == NULL )
    {
      g_warning( "Could not acquire PolKit authority at startup: %s",
                 pk_error ? pk_error->message : "(unknown)" );
      g_clear_error( &pk_error );
    }
  }

  g_debug( "localed D-Bus object registered at /org/freedesktop/locale1" );
  return TRUE;
}

void
rcl_daemon_shutdown( RclDaemon *daemon )
{
  RclLocaleDaemon        *self = RCL_LOCALE_DAEMON( daemon );
  RclLocaleDaemonPrivate *priv = self->priv;

  if( priv->connection && priv->registration_id > 0 )
  {
    g_dbus_connection_unregister_object( priv->connection,
                                         priv->registration_id );
    priv->registration_id = 0;
  }

  g_clear_object( &priv->connection );

  if( priv->introspection )
  {
    g_dbus_node_info_unref( priv->introspection );
    priv->introspection = NULL;
  }
}

void
rcl_daemon_set_debug( RclDaemon *daemon, gboolean debug )
{
  RCL_LOCALE_DAEMON(daemon)->priv->debug = debug;
}

/* --------------------------------------------------------------------------
   GObject lifecycle
   -------------------------------------------------------------------------- */

static void
rcl_locale_daemon_finalize( GObject *object )
{
  RclLocaleDaemonPrivate *priv = RCL_LOCALE_DAEMON(object)->priv;

  g_clear_object( &priv->authority );

  g_strfreev( priv->locale );
  g_free( priv->vconsole_keymap );
  g_free( priv->vconsole_keymap_toggle );
  g_free( priv->x11_layout );
  g_free( priv->x11_model );
  g_free( priv->x11_variant );
  g_free( priv->x11_options );

  G_OBJECT_CLASS(rcl_locale_daemon_parent_class)->finalize(object);
}

static void
rcl_locale_daemon_class_init( RclLocaleDaemonClass *klass )
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = rcl_locale_daemon_finalize;
}

static void
rcl_locale_daemon_init( RclLocaleDaemon *self )
{
  self->priv = rcl_locale_daemon_get_instance_private( self );
  self->priv->locale = g_new0( gchar *, 1 ); /* empty GStrv until first sync */
}
