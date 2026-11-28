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
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glib-object.h>

#include "rcl-locale.h"
#include "rcl-file-utils.h"
#include "rcl-file-transaction.h"
#include "rcl-xkb-config.h"

#ifdef RCL_LOCALE_TESTING
/*
 * Fail one forward configuration rename in the D-Bus integration tests.
 * Rollback uses rcl-file-transaction.c directly and is deliberately not
 * intercepted, allowing the test to verify restoration after a partial
 * multi-file update.
 */
static gchar *test_fail_rename_destination;

void
rcl_locale_test_fail_rename_to ( const gchar *destination )
{
  g_free( test_fail_rename_destination );
  test_fail_rename_destination = g_strdup( destination );
}

static int
rcl_locale_rename ( const gchar *source, const gchar *destination )
{
  if( test_fail_rename_destination &&
      g_strcmp0( destination, test_fail_rename_destination ) == 0 )
  {
    errno = EIO;
    return -1;
  }

  return g_rename( source, destination );
}
#else
#define rcl_locale_rename g_rename
#endif

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
  /* Localed extension: modify only the explicitly named assignments. */
  "    <method name='UpdateLocale'>"
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

  /*
   * Authorization requests use the daemon's private system-bus connection.
   * This avoids PolkitAuthority creating a separate GDBus connection whose
   * default exit-on-close behavior can terminate the daemon during bus loss.
   */
  GCancellable     *auth_cancellable;

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

static gboolean
rcl_keyboard_value_is_safe(const gchar *value);

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
rcl_x11_to_keymap( const gchar *layout,
                   const gchar *variant,
                   gchar **out_keymap )
{
  guint i;
  /*
   * Match the first layout and its corresponding first variant.  XKB uses
   * positional correspondence between comma-separated layout and variant
   * lists; ignoring the variant can select a different console map (for
   * example, "ch" + "fr" must map to "ch-fr", not "ch").
   */
  gchar *first_layout = NULL;
  gchar *first_variant = NULL;
  const gchar *comma;
  gboolean found = FALSE;

  if( !layout || layout[0] == '\0' )
    return FALSE;

  comma = strchr( layout, ',' );
  first_layout = comma ? g_strndup( layout, (gsize)(comma - layout) )
                       : g_strdup( layout );

  if( !variant )
    variant = "";

  comma = strchr( variant, ',' );
  first_variant = comma ? g_strndup( variant, (gsize)(comma - variant) )
                        : g_strdup( variant );

  for( i = 0; keymap_map[i].vconsole_keymap != NULL; i++ )
  {
    if( g_strcmp0( first_layout, keymap_map[i].x11_layout ) == 0 &&
        g_strcmp0( first_variant, keymap_map[i].x11_variant ) == 0 )
    {
      *out_keymap = g_strdup( keymap_map[i].vconsole_keymap );
      found = TRUE;
      break;
    }
  }

  g_free( first_layout );
  g_free( first_variant );
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
static void
read_x11_keyboard_conf( const gchar *path,
                        gchar **out_layout, gchar **out_model,
                        gchar **out_variant, gchar **out_options )
{
  gchar *contents = NULL;
  gchar *layout = NULL, *model = NULL, *variant = NULL, *options = NULL;
  GError *error = NULL;

  *out_layout  = g_strdup( "" );
  *out_model   = g_strdup( "" );
  *out_variant = g_strdup( "" );
  *out_options = g_strdup( "" );

  if( !g_file_get_contents( path, &contents, NULL, &error ) )
  {
    /*
     * A missing Xorg snippet is a normal unconfigured state.  Other read
     * failures must be visible in the journal rather than silently looking
     * identical to a missing file.
     */
    if( !g_error_matches( error, G_FILE_ERROR, G_FILE_ERROR_NOENT ) )
      g_warning( "read_x11_keyboard_conf: cannot read %s: %s",
                 path, error ? error->message : "unknown read error" );
    g_clear_error( &error );
    return;
  }

  if( !rcl_xkb_config_parse( contents, &layout, &model, &variant,
                             &options, &error ) )
  {
    g_warning( "read_x11_keyboard_conf: cannot parse %s: %s",
               path, error ? error->message : "unknown parser error" );
    g_clear_error( &error );
    g_free( contents );
    return;
  }

  g_free( contents );

  g_free( *out_layout );  *out_layout = layout;
  g_free( *out_model );   *out_model = model;
  g_free( *out_variant ); *out_variant = variant;
  g_free( *out_options ); *out_options = options;

  /* Validate values read from disk against the same allowlist used by
   * SetX11Keyboard. Unsafe values are not exposed as D-Bus properties. */
#define SANITISE_XKB(field, name, kind)                                  \
  do {                                                                      \
    if( (*field)[0] != '\0' && !rcl_xkb_value_is_valid( kind, *field ) )   \
    {                                                                       \
      g_warning( "read_x11_keyboard_conf: invalid value for %s, ignoring", \
                 name );                                                    \
      g_clear_pointer( field, g_free );                                     \
      *field = g_strdup( "" );                                              \
    }                                                                       \
  } while(0)

  SANITISE_XKB( out_layout,  "XkbLayout",  RCL_XKB_VALUE_LAYOUT );
  SANITISE_XKB( out_model,   "XkbModel",   RCL_XKB_VALUE_MODEL );
  SANITISE_XKB( out_variant, "XkbVariant", RCL_XKB_VALUE_VARIANT );
  SANITISE_XKB( out_options, "XkbOptions", RCL_XKB_VALUE_OPTIONS );

#undef SANITISE_XKB
}

/* Construct canonical Xorg configuration without touching the filesystem. */
static GString *
rcl_render_x11_keyboard(const gchar *layout, const gchar *model,
                        const gchar *variant, const gchar *options)
{
  GString *out = g_string_new(NULL);
  g_string_append(out, "# Written by localed - see localed(8)\n");
  g_string_append(out, "Section \"InputClass\"\n");
  g_string_append(out, "    Identifier \"system-keyboard\"\n");
  g_string_append(out, "    MatchIsKeyboard \"on\"\n");
  g_string_append_printf(out, "    Option \"XkbLayout\" \"%s\"\n", layout ? layout : "");
  g_string_append_printf(out, "    Option \"XkbModel\" \"%s\"\n", model ? model : "");
  g_string_append_printf(out, "    Option \"XkbVariant\" \"%s\"\n", variant ? variant : "");
  g_string_append_printf(out, "    Option \"XkbOptions\" \"%s\"\n", options ? options : "");
  g_string_append(out, "EndSection\n");
  return out;
}

static gboolean write_x11_keyboard_conf(const gchar *path,
    const gchar *layout, const gchar *model, const gchar *variant,
    const gchar *options, GError **error);

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

    /* Skip the [ -x /usr/bin/loadkeys ] test line — it contains ']'.
     * On the actual invocation line, the keymap is the last whitespace-
     * delimited token after "loadkeys". */
    if( strstr( line, "loadkeys" ) && !strchr( line, ']' ) )
    {
      const gchar *p = strstr( line, "loadkeys" );
      p += strlen( "loadkeys" );
      while( *p == ' ' || *p == '\t' ) p++;
      if( *p && *p != '#' && *p != '\0' )
      {
        /* $NF: walk to the last token on the line */
        const gchar *tok = p;
        while( *p )
        {
          if( ( *p == ' ' || *p == '\t' ) && *(p+1) && *(p+1) != ' ' && *(p+1) != '\t' )
            tok = p + 1;
          p++;
        }
        /* trim trailing whitespace */
        const gchar *end = p;
        while( end > tok && ( *(end-1) == ' ' || *(end-1) == '\t' || *(end-1) == '\n' ) )
          end--;
        if( end > tok )
        {
          result = g_strndup( tok, (gsize)(end - tok) );

          /* The writer quotes the keymap: /usr/bin/loadkeys "us"
           * Strip surrounding double-quotes so the stored value is bare. */
          if( result[0] == '"' )
          {
            gsize rlen = strlen( result );
            if( rlen >= 2 && result[rlen-1] == '"' )
            {
              gchar *unquoted = g_strndup( result + 1, rlen - 2 );
              g_free( result );
              result = unquoted;
            }
          }
        }
      }
    }
  }

  g_strfreev( lines );

  /* Validate the parsed keymap against the same allowlist used by
   * SetVConsoleKeyboard and rcl_read_rc_keymap_toggle().  A manually-edited
   * rc.keymap with a loadkeys argument containing characters outside
   * [A-Za-z0-9\-_.,+:] (path traversal, shell metacharacters, spaces) is
   * reset to empty rather than served as the VConsoleKeymap D-Bus property. */
  if( result && result[0] != '\0' && !rcl_keyboard_value_is_safe( result ) )
  {
    g_warning( "rcl_read_rc_keymap: unsafe keymap value in %s, ignoring", path );
    g_free( result );
    result = NULL;
  }

  return result ? result : g_strdup( "" );
}

/* An empty keymap renders a no-op script, not a loadkeys invocation. */
static GString *
rcl_render_rc_keymap(const gchar *keymap)
{
  GString *out = g_string_new(
      "#!/bin/bash\n"
      "# Written by localed - see localed(8)\n"
      "# More keymaps are in /usr/share/kbd/keymaps\n"
      "if [ -x /usr/bin/loadkeys ]; then\n");
  if (keymap && *keymap)
    g_string_append_printf(out, "  /usr/bin/loadkeys \"%s\"\n", keymap);
  else
    g_string_append(out, "  : # no keymap configured\n");
  g_string_append(out, "fi\n");
  return out;
}

/* Read the toggle keymap from the sidecar file.
 * Returns a newly-allocated string (empty if absent or invalid). */
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

  /* Validate against the same allowlist used by SetVConsoleKeyboard and all
   * other keyboard string fields.  A manually-edited sidecar with characters
   * outside [A-Za-z0-9\-_.,+:] is reset to empty rather than served as a
   * D-Bus property, consistent with the SANITISE_XKB pattern used for X11
   * fields and the safe-charset check in rcl_read_rc_keymap(). */
  if( result[0] != '\0' && !rcl_keyboard_value_is_safe( result ) )
  {
    g_warning( "rcl_read_rc_keymap_toggle: unsafe value in %s, ignoring", path );
    g_free( result );
    result = g_strdup( "" );
  }

  return result;
}

/* Maximum accepted length for locale and keyboard configuration values. */
#define MAX_XKB_VALUE_LEN  256

/* --------------------------------------------------------------------------
   Input validation
   -------------------------------------------------------------------------- */

/* Locale values (e.g. "en_US.UTF-8") and the LANGUAGE priority list
   (e.g. "en_US:en_GB:en") share a conservative safe charset. */
static gboolean
rcl_locale_value_is_safe( const gchar *value )
{
  const gchar *p;

  if( !value || strlen( value ) > MAX_XKB_VALUE_LEN )
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

/* Console keymap names and X11 layout/model/variant/options strings are
   restricted to a strict allowlist: alphanumeric characters plus the handful
   of punctuation that appears in real keymap and XKB identifiers.
   Shell metacharacters ($, `, !, (, ), ;, |, &, >, <, *, ?, ~, {, }, space,
   etc.) are implicitly denied — they never appear in legitimate values and
   would be dangerous if written into rc.keymap or xorg.conf.d (both are
   executed/sourced as shell or parsed by Xorg). */
static gboolean
rcl_keyboard_value_is_safe( const gchar *value )
{
  const gchar *p;

  /* Match the reader's cap so accepted values read back unchanged. */
  if( !value || strlen( value ) > MAX_XKB_VALUE_LEN )
    return FALSE;

  for( p = value; *p; p++ )
  {
    guchar c = (guchar) *p;
    if( g_ascii_isalnum(c) )
      continue;
    switch( c )
    {
      /* Separators used in XKB option strings ("grp:win_space_toggle"),
         layout lists ("us,de"), and variant names ("nodeadkeys"). */
      case '-': case '_': case '.': case ',': case '+': case ':':
        continue;
      default:
        return FALSE;
    }
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

static gchar *rcl_locale_assignment_key(const gchar *line, gboolean csh);
static gchar *rcl_locale_assignment_value(const gchar *line, const gchar *key,
                                         gboolean csh, const gchar **suffix);

/* Read the same simple literal exports accepted by the locale writer,
 * including indentation, quoting and trailing comments. Never source them. */
static GHashTable *
rcl_read_lang_sh(const gchar *path)
{
  GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);
  g_autofree gchar *contents = NULL;
  g_auto(GStrv) lines = NULL;
  guint i;

  if (!g_file_get_contents(path, &contents, NULL, NULL))
    return table;

  lines = g_strsplit(contents, "\n", -1);
  for (i = 0; lines[i]; i++)
  {
    g_autofree gchar *key = rcl_locale_assignment_key(lines[i], FALSE);
    gchar *value;

    if (!key || !rcl_locale_variable_is_known(key))
      continue;

    value = rcl_locale_assignment_value(lines[i], key, FALSE, NULL);
    if (!value)
    {
      g_warning("rcl_read_lang_sh: skipping unsafe value for %s", key);
      continue;
    }

    g_hash_table_replace(table, g_steal_pointer(&key), value);
  }
  return table;
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
  OP_UPDATE_LOCALE,
  OP_SET_VCONSOLE_KEYBOARD,
  OP_SET_X11_KEYBOARD,
} LocaleOp;

typedef struct
{
  RclLocaleDaemon        *daemon;       /* owned ref */
  GDBusMethodInvocation  *invocation;   /* owned ref; completed exactly once */
  GCancellable           *cancellable;  /* owned ref to this daemon session */
  LocaleOp                op;

  gchar  **locale;        /* OP_SET_LOCALE / OP_UPDATE_LOCALE: owned GStrv */

  gchar   *keymap;        /* OP_SET_VCONSOLE_KEYBOARD */
  gchar   *keymap_toggle;

  gchar   *x11_layout;    /* OP_SET_X11_KEYBOARD */
  gchar   *x11_model;
  gchar   *x11_variant;
  gchar   *x11_options;

  gboolean convert;       /* shared by the two keyboard ops */

  GDBusConnection *auth_connection;
  gchar *cancellation_id;
  gulong cancel_handler_id;
} AuthCtx;

typedef struct
{
  GDBusConnection *connection;
  gchar *cancellation_id;
} AuthCancelData;

static void
auth_cancel_data_free( gpointer user_data )
{
  AuthCancelData *data = user_data;

  g_clear_object( &data->connection );
  g_free( data->cancellation_id );
  g_free( data );
}

static void
on_cancel_authorization_done( GObject *source,
                              GAsyncResult *result,
                              gpointer user_data )
{
  GError *error = NULL;
  GVariant *reply;

  reply = g_dbus_connection_call_finish( G_DBUS_CONNECTION(source),
                                         result, &error );
  if( reply )
    g_variant_unref( reply );
  else if( error )
  {
    g_debug( "PolicyKit cancellation request could not be delivered: %s",
             error->message );
    g_clear_error( &error );
  }
}

static void
on_auth_cancelled( GCancellable *cancellable, gpointer user_data )
{
  AuthCancelData *data = user_data;

  /*
   * CancelCheckAuthorization is best-effort. The local GDBus call is also
   * cancelled, and its completion path independently prevents a cancelled
   * request from applying any privileged change.
   */
  g_dbus_connection_call(
    data->connection,
    "org.freedesktop.PolicyKit1",
    "/org/freedesktop/PolicyKit1/Authority",
    "org.freedesktop.PolicyKit1.Authority",
    "CancelCheckAuthorization",
    g_variant_new( "(s)", data->cancellation_id ),
    NULL, /* CancelCheckAuthorization has no output arguments. */
    G_DBUS_CALL_FLAGS_NONE,
    -1,
    NULL,
    on_cancel_authorization_done,
    NULL );
}

static void
auth_ctx_free( AuthCtx *ctx )
{
  if( !ctx )
    return;

  if( ctx->cancellable && ctx->cancel_handler_id )
  {
    g_cancellable_disconnect( ctx->cancellable, ctx->cancel_handler_id );
    ctx->cancel_handler_id = 0;
  }

  g_clear_object( &ctx->auth_connection );
  g_free( ctx->cancellation_id );
  g_clear_object( &ctx->daemon );
  g_clear_object( &ctx->invocation );
  g_clear_object( &ctx->cancellable );
  g_strfreev( ctx->locale );
  g_free( ctx->keymap );
  g_free( ctx->keymap_toggle );
  g_free( ctx->x11_layout );
  g_free( ctx->x11_model );
  g_free( ctx->x11_variant );
  g_free( ctx->x11_options );
  g_free( ctx );
}

static gboolean rcl_write_locale_files( const gchar *const *known_keys,
                                         const gchar *const *new_values,
                                         GError **error );
static gboolean rcl_patch_locale_files( const gchar *const *known_keys,
                                         const gchar *const *new_values,
                                         GError **error );
static gboolean rcl_write_vconsole_files( const gchar *keymap,
                                          const gchar *keymap_toggle,
                                          GError **error );
static gboolean rcl_write_x11_vconsole_files( const gchar *layout,
                                              const gchar *model,
                                              const gchar *variant,
                                              const gchar *options,
                                              const gchar *keymap,
                                              GError **error );
static gboolean rcl_write_vconsole_x11_files( const gchar *keymap,
                                              const gchar *keymap_toggle,
                                              const gchar *layout,
                                              const gchar *model,
                                              const gchar *variant,
                                              const gchar *options,
                                              GError **error );

/* ---- operation handlers: run only after a successful authorisation ---- */

static void
do_set_locale( RclLocaleDaemon *daemon, gchar **locale,
               gboolean partial, GDBusMethodInvocation *invocation )
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
    if( !rcl_locale_value_is_safe( locale[0] ) )
    {
      g_dbus_method_invocation_return_error( invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Invalid locale specification: '%s'", locale[0] );
      return;
    }
    gchar *expanded = g_strdup_printf( "LANG=%s", locale[0] );
    g_free( locale[0] );
    locale[0] = expanded;
  }

  /* Reject oversized arrays before doing any per-entry work.
   * The maximum useful number of entries equals the number of known locale
   * variables; anything larger is either a client bug or a DoS attempt.
   * G_N_ELEMENTS includes the NULL sentinel, so subtract 1 for the count. */
  {
    guint n_entries = 0;
    while( locale[n_entries] != NULL )
      n_entries++;
    if( n_entries > G_N_ELEMENTS(locale_variable_names) - 1 )
    {
      g_dbus_method_invocation_return_error( invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Too many locale entries (max %u)",
        (guint)(G_N_ELEMENTS(locale_variable_names) - 1) );
      return;
    }
  }

  if( partial && !locale[0] )
  {
    g_dbus_method_invocation_return_error( invocation,
      G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
      "UpdateLocale requires at least one assignment" );
    return;
  }

  /* Validate every entry first; reject the whole call on the first bad one,
     same semantics as systemd-localed. */
  for( i = 0; locale[i] != NULL; i++ )
  {
    gchar *eq = strchr( locale[i], '=' );
    g_autofree gchar *key = NULL;
    const gchar *val;

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
      return;
    }

    if( partial )
    {
      guint j;
      for( j = 0; j < i; j++ )
      {
        const gchar *previous = strchr(locale[j], '=');
        if( previous && (gsize)(previous - locale[j]) == strlen(key) &&
            strncmp(locale[j], key, strlen(key)) == 0 )
        {
          g_dbus_method_invocation_return_error(invocation,
            G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
            "Duplicate locale variable '%s'", key);
          return;
        }
      }
    }

    if( val[0] != '\0' && !rcl_locale_value_is_safe( val ) )
    {
      g_dbus_method_invocation_return_error( invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Invalid value for %s", key );
      return;
    }
  }

  /* Build the known_keys/new_values pair arrays. SetLocale replaces the
     whole locale set (systemd semantics): every known variable not
     supplied in the call is removed from both scripts. */
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

  if( partial )
  {
    if( !rcl_patch_locale_files(known_keys, new_values, &error) )
    {
      rcl_daemon_sync_dbus_properties(daemon);
      g_dbus_method_invocation_return_gerror(invocation, error);
      g_error_free(error);
      return;
    }
    rcl_daemon_sync_dbus_properties(daemon);
    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }

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

  if( !rcl_write_locale_files( known_keys, new_values, &error ) )
  {
    rcl_daemon_sync_dbus_properties( daemon );
    g_dbus_method_invocation_return_gerror( invocation, error );
    g_error_free( error );
    return;
  }

  rcl_daemon_sync_dbus_properties( daemon );
  g_dbus_method_invocation_return_value( invocation, NULL );
}

/*
 * Apply a GNOME AccountsService language update without making a recursive
 * D-Bus call to our own locale1 service. The bridge in main.c has already
 * checked the affected user's membership in PRIVILEGED_GROUP.
 *
 * Match SetLocale semantics: replace the system locale assignments with LANG,
 * and apply the same LANGUAGE fallback when the selected value has a mapping.
 */
gboolean
rcl_locale_daemon_set_language_from_accounts( RclLocaleDaemon *daemon,
                                              const gchar *language,
                                              GError **error )
{
  const gchar *known_keys[3] = { "LANG", NULL, NULL };
  const gchar *new_values[3] = { language, NULL, NULL };
  const gchar *fallback;

  if (!language || !rcl_locale_value_is_safe(language))
  {
    g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                        _("Invalid locale value from AccountsService"));
    return FALSE;
  }

  fallback = rcl_find_language_fallback(language);
  if (fallback)
  {
    known_keys[1] = "LANGUAGE";
    new_values[1] = fallback;
  }

  if (!rcl_write_locale_files(known_keys, new_values, error))
  {
    rcl_daemon_sync_dbus_properties(daemon);
    return FALSE;
  }

  rcl_daemon_sync_dbus_properties(daemon);
  return TRUE;
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

  if( convert && keymap[0] != '\0' )
  {
    gchar *x11_layout = NULL, *x11_variant = NULL;
    if( rcl_keymap_to_x11( keymap, &x11_layout, &x11_variant ) )
    {
      RclLocaleDaemonPrivate *priv = daemon->priv;
      if( !rcl_write_vconsole_x11_files( keymap, keymap_toggle,
                                         x11_layout, priv->x11_model,
                                         x11_variant, priv->x11_options,
                                         &error ) )
      {
        rcl_daemon_sync_dbus_properties( daemon );
        g_dbus_method_invocation_return_gerror( invocation, error );
        g_clear_error( &error );
        g_free( x11_layout );
        g_free( x11_variant );
        return;
      }
      g_free( x11_layout );
      g_free( x11_variant );
    }
    else
    {
      g_debug( "convert: no built-in X11 mapping for keymap '%s'", keymap );
      if( !rcl_write_vconsole_files( keymap, keymap_toggle, &error ) )
      {
        rcl_daemon_sync_dbus_properties( daemon );
        g_dbus_method_invocation_return_gerror( invocation, error );
        g_clear_error( &error );
        return;
      }
    }
  }
  else if( !rcl_write_vconsole_files( keymap, keymap_toggle, &error ) )
  {
    rcl_daemon_sync_dbus_properties( daemon );
    g_dbus_method_invocation_return_gerror( invocation, error );
    g_clear_error( &error );
    return;
  }

  rcl_daemon_sync_dbus_properties( daemon );
  g_dbus_method_invocation_return_value( invocation, NULL );
}

static gboolean
rcl_x11_keyboard_values_are_valid( const gchar *layout,
                                   const gchar *model,
                                   const gchar *variant,
                                   const gchar *options )
{
  return rcl_xkb_value_is_valid( RCL_XKB_VALUE_LAYOUT, layout ) &&
         rcl_xkb_value_is_valid( RCL_XKB_VALUE_MODEL, model ) &&
         rcl_xkb_value_is_valid( RCL_XKB_VALUE_VARIANT, variant ) &&
         rcl_xkb_value_is_valid( RCL_XKB_VALUE_OPTIONS, options );
}

static void
do_set_x11_keyboard( RclLocaleDaemon *daemon,
                     const gchar *layout, const gchar *model,
                     const gchar *variant, const gchar *options,
                     gboolean convert,
                     GDBusMethodInvocation *invocation )
{
  GError *error = NULL;

  if( !rcl_x11_keyboard_values_are_valid( layout, model, variant, options ) )
  {
    g_dbus_method_invocation_return_error( invocation,
      G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
      "Invalid X11 keyboard configuration value" );
    return;
  }

  if( convert && layout[0] != '\0' )
  {
    gchar *vc_keymap = NULL;
    if( rcl_x11_to_keymap( layout, variant, &vc_keymap ) )
    {
      if( !rcl_write_x11_vconsole_files( layout, model, variant, options,
                                         vc_keymap, &error ) )
      {
        rcl_daemon_sync_dbus_properties( daemon );
        g_dbus_method_invocation_return_gerror( invocation, error );
        g_clear_error( &error );
        g_free( vc_keymap );
        return;
      }
      g_free( vc_keymap );
    }
    else
    {
      g_debug( "convert: no built-in console mapping for X11 layout '%s'", layout );
      if( !write_x11_keyboard_conf( RCL_X11_KEYBOARD_FILE,
                                    layout, model, variant, options, &error ) )
      {
        rcl_daemon_sync_dbus_properties( daemon );
        g_dbus_method_invocation_return_gerror( invocation, error );
        g_clear_error( &error );
        return;
      }
    }
  }
  else if( !write_x11_keyboard_conf( RCL_X11_KEYBOARD_FILE,
                                     layout, model, variant, options, &error ) )
  {
    rcl_daemon_sync_dbus_properties( daemon );
    g_dbus_method_invocation_return_gerror( invocation, error );
    g_clear_error( &error );
    return;
  }

  rcl_daemon_sync_dbus_properties( daemon );
  g_dbus_method_invocation_return_value( invocation, NULL );
}

/* ---- asynchronous PolicyKit D-Bus authorization ---- */

static void
on_check_done( GObject *source, GAsyncResult *res, gpointer user_data )
{
  AuthCtx  *ctx = user_data;
  GVariant *reply;
  GVariant *details = NULL;
  GError   *error = NULL;
  gboolean  authorized = FALSE;
  gboolean  challenge = FALSE;

  reply = g_dbus_connection_call_finish( G_DBUS_CONNECTION(source),
                                         res, &error );

  if( !reply )
  {
    if( g_cancellable_is_cancelled( ctx->cancellable ) )
      g_dbus_method_invocation_return_dbus_error(
        ctx->invocation,
        "org.freedesktop.DBus.Error.Disconnected",
        "Daemon connection was closed before authorization completed" );
    else
      g_dbus_method_invocation_return_gerror( ctx->invocation, error );

    g_clear_error( &error );
    auth_ctx_free( ctx );
    return;
  }

  g_variant_get( reply, "((bb@a{ss}))", &authorized, &challenge, &details );
  g_variant_unref( details );
  g_variant_unref( reply );

  /*
   * Cancellation can race with a successful PolicyKit reply. Check the
   * daemon-session cancellable before performing any privileged file update.
   */
  if( g_cancellable_is_cancelled( ctx->cancellable ) )
  {
    g_dbus_method_invocation_return_dbus_error(
      ctx->invocation,
      "org.freedesktop.DBus.Error.Disconnected",
      "Daemon connection was closed before authorization completed" );
    auth_ctx_free( ctx );
    return;
  }

  if( !authorized )
  {
    if( challenge )
      g_dbus_method_invocation_return_dbus_error(
        ctx->invocation,
        "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired",
        "Authentication is required to perform the requested action" );
    else
      g_dbus_method_invocation_return_error(
        ctx->invocation, G_DBUS_ERROR, G_DBUS_ERROR_AUTH_FAILED,
        "Not authorised to perform the requested action" );

    auth_ctx_free( ctx );
    return;
  }

  switch( ctx->op )
  {
    case OP_SET_LOCALE:
      do_set_locale( ctx->daemon, ctx->locale, FALSE, ctx->invocation );
      break;
    case OP_UPDATE_LOCALE:
      do_set_locale( ctx->daemon, ctx->locale, TRUE, ctx->invocation );
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

/* ---- asynchronous PolicyKit authorization starter ---- */

static void
check_polkit_async( AuthCtx *ctx, const gchar *action_id, gboolean interactive )
{
  RclLocaleDaemonPrivate *priv = ctx->daemon->priv;
  GVariantBuilder subject_details;
  GVariantBuilder details;
  GVariant *parameters;
  AuthCancelData *cancel_data;
  const gchar *sender;
  guint flags;

  if( !priv->connection || g_dbus_connection_is_closed(priv->connection) )
  {
    g_dbus_method_invocation_return_dbus_error(
      ctx->invocation,
      "org.freedesktop.DBus.Error.Disconnected",
      "System bus connection is unavailable" );
    auth_ctx_free( ctx );
    return;
  }

  ctx->cancellable = g_object_ref( priv->auth_cancellable );
  if( g_cancellable_is_cancelled( ctx->cancellable ) )
  {
    g_dbus_method_invocation_return_dbus_error(
      ctx->invocation,
      "org.freedesktop.DBus.Error.Disconnected",
      "Authorization was cancelled before it could be started" );
    auth_ctx_free( ctx );
    return;
  }

  sender = g_dbus_method_invocation_get_sender( ctx->invocation );

  g_variant_builder_init( &subject_details, G_VARIANT_TYPE("a{sv}") );
  g_variant_builder_add( &subject_details, "{sv}", "name",
                         g_variant_new_string(sender) );

  g_variant_builder_init( &details, G_VARIANT_TYPE("a{ss}") );

  flags = interactive ? 1u : 0u; /* AllowUserInteraction */

  ctx->auth_connection = g_object_ref( priv->connection );
  ctx->cancellation_id = g_uuid_string_random();

  parameters = g_variant_new(
    "((s@a{sv})s@a{ss}us)",
    "system-bus-name",
    g_variant_builder_end( &subject_details ),
    action_id,
    g_variant_builder_end( &details ),
    flags,
    ctx->cancellation_id );

  /*
   * Queue CheckAuthorization before connecting the cancellation handler.
   * If the cancellable is already cancelled, g_cancellable_connect() invokes
   * the handler immediately; queuing the check first preserves D-Bus message
   * order so CancelCheckAuthorization cannot overtake its own request.
   */
  g_dbus_connection_call(
    ctx->auth_connection,
    "org.freedesktop.PolicyKit1",
    "/org/freedesktop/PolicyKit1/Authority",
    "org.freedesktop.PolicyKit1.Authority",
    "CheckAuthorization",
    parameters,
    G_VARIANT_TYPE( "((bba{ss}))" ),
    G_DBUS_CALL_FLAGS_NONE,
    -1,
    ctx->cancellable,
    on_check_done,
    ctx );

  cancel_data = g_new0( AuthCancelData, 1 );
  cancel_data->connection = g_object_ref( ctx->auth_connection );
  cancel_data->cancellation_id = g_strdup( ctx->cancellation_id );

  ctx->cancel_handler_id =
    g_cancellable_connect( ctx->cancellable,
                           G_CALLBACK(on_auth_cancelled),
                           cancel_data,
                           auth_cancel_data_free );
}

/* Identify a simple Slackware locale assignment without interpreting or
 * executing shell code. Keep every unrelated line byte-for-byte. */
static gchar *
rcl_locale_assignment_key(const gchar *line, gboolean csh)
{
  const gchar *p = line, *start;
  const gchar *prefix = csh ? "setenv" : "export";
  gsize length = strlen(prefix);

  while (*p == ' ' || *p == '\t')
    p++;
  if (!g_str_has_prefix(p, prefix) || !g_ascii_isspace(p[length]))
    return NULL;
  p += length;
  while (*p == ' ' || *p == '\t')
    p++;
  start = p;
  while (g_ascii_isalnum(*p) || *p == '_')
    p++;
  if (p == start)
    return NULL;
  length = (gsize)(p - start);
  if (csh)
  {
    if (*p && !g_ascii_isspace(*p))
      return NULL;
  }
  else
  {
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p != '=')
      return NULL;
  }
  return g_strndup(start, length);
}

/* A targeted line must contain just one simple assignment. Reject compound
 * shell statements rather than accidentally removing a second variable. */
static gchar *
rcl_locale_assignment_value(const gchar *line, const gchar *key,
                             gboolean csh, const gchar **suffix)
{
  const gchar *p = line, *start;
  gchar quote = '\0';
  g_autofree gchar *value = NULL;

  while (*p == ' ' || *p == '\t')
    p++;
  p += csh ? strlen("setenv") : strlen("export");
  while (*p == ' ' || *p == '\t')
    p++;
  p += strlen(key);
  while (*p == ' ' || *p == '\t')
    p++;
  if (!csh)
    p++; /* The caller already verified '='. */
  if (*p == '\'' || *p == '"')
    quote = *p++;
  start = p;
  if (quote)
  {
    while (*p && *p != quote)
      p++;
    if (*p != quote)
      return NULL;
  }
  else
  {
    while (*p && !g_ascii_isspace(*p))
      p++;
  }
  value = g_strndup(start, p - start);
  if (*value && !rcl_locale_value_is_safe(value))
    return NULL;
  if (quote)
    p++;
  if (*p && !g_ascii_isspace(*p))
    return NULL;
  if (suffix)
    *suffix = p;
  while (g_ascii_isspace(*p))
    p++;
  return (!*p || *p == '#') ? g_steal_pointer(&value) : NULL;
}

static void
rcl_locale_append_assignment(GString *out, const gchar *key,
                             const gchar *value, gboolean csh)
{
  if (!value || !*value)
    return;
  if (out->len && out->str[out->len - 1] != '\n')
    g_string_append_c(out, '\n');
  if (csh)
    g_string_append_printf(out, "setenv %s %s\n", key, value);
  else
    g_string_append_printf(out, "export %s=\"%s\"\n", key, value);
}

/* Recognize the Slackware footer, including when a test uses a relocated
 * configuration path. Do not mistake unrelated end comments for it. */
static gboolean
rcl_locale_is_footer(const gchar *line, gboolean csh)
{
  g_autofree gchar *copy = g_strdup(line);
  const gchar *marker = g_strstrip(copy);
  const gchar *stock = csh ? "/etc/profile.d/lang.csh" :
                             "/etc/profile.d/lang.sh";
  const gchar *configured = csh ? RCL_LANG_CSH_FILE : RCL_LANG_SH_FILE;

  if (!g_str_has_prefix(marker, "# End of "))
    return FALSE;

  marker += strlen("# End of ");
  return g_str_equal(marker, stock) || g_str_equal(marker, configured);
}

/* Preserve leading indentation, trailing whitespace/comments and the
 * original line termination when changing an existing assignment. */
static void
rcl_locale_replace_assignment(GString *out, const gchar *line,
                              const gchar *key, const gchar *value,
                              const gchar *suffix, gboolean csh,
                              gboolean newline)
{
  const gchar *start = line;

  if (!value || !*value)
    return;
  while (*start == ' ' || *start == '\t')
    start++;
  g_string_append_len(out, line, start - line);
  if (csh)
    g_string_append_printf(out, "setenv %s %s", key, value);
  else
    g_string_append_printf(out, "export %s=\"%s\"", key, value);
  g_string_append(out, suffix);
  if (newline)
    g_string_append_c(out, '\n');
}

/* Build an update from the transaction snapshot, not from a cached D-Bus
 * property. Absent/empty values remove only their named assignments. */
static GString *
rcl_locale_patch_contents(const gchar *contents, const gchar *const *keys,
                          const gchar *const *values, gboolean csh,
                          GError **error)
{
  GString *out = g_string_new(NULL);
  gboolean seen[G_N_ELEMENTS(locale_variable_names)] = { FALSE };
  gboolean have_footer = FALSE;
  gsize footer_offset = 0;
  guint i;
  const gchar *line = contents;

  if (line)
  {
    while (*line)
    {
      const gchar *end = strchr(line, '\n');
      gsize length = end ? (gsize)(end - line) : strlen(line);
      g_autofree gchar *copy = g_strndup(line, length);
      g_autofree gchar *key = rcl_locale_assignment_key(copy, csh);
      gboolean replace = FALSE;

      if (!have_footer && rcl_locale_is_footer(copy, csh))
      {
        have_footer = TRUE;
        footer_offset = out->len;
      }

      for (i = 0; key && keys[i]; i++)
        if (g_str_equal(key, keys[i]))
        {
          const gchar *suffix = NULL;
          g_autofree gchar *previous =
              rcl_locale_assignment_value(copy, key, csh, &suffix);
          if (!previous)
          {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Cannot safely update complex shell assignment for %s", key);
            g_string_free(out, TRUE);
            return NULL;
          }
          /* Keep the first assignment's position and remove duplicates. */
          if (!seen[i])
          {
            if (*previous && g_strcmp0(previous, values[i]) == 0)
              g_string_append_len(out, line, length + (end != NULL));
            else
              rcl_locale_replace_assignment(out, copy, keys[i], values[i],
                                            suffix, csh, end != NULL);
          }
          seen[i] = TRUE;
          replace = TRUE;
          break;
        }
      if (!replace)
        g_string_append_len(out, line, length + (end != NULL));
      line += length + (end != NULL);
    }
  }
  else
  {
    if (csh)
      g_string_append(out, "#!/bin/csh\n");
    g_string_append(out, "# Written by localed - see localed(8)\n");
  }

  if (have_footer)
  {
    g_autoptr(GString) additions = g_string_new(NULL);
    for (i = 0; keys[i]; i++)
      if (!seen[i])
        rcl_locale_append_assignment(additions, keys[i], values[i], csh);
    if (additions->len)
      g_string_insert_len(out, footer_offset, additions->str, additions->len);
  }
  else
  {
    for (i = 0; keys[i]; i++)
      if (!seen[i])
        rcl_locale_append_assignment(out, keys[i], values[i], csh);
  }
  return out;
}

static gboolean
rcl_write_snapshot_contents(const RclFileSnapshot *snapshot, GString *contents,
                            mode_t default_mode, gboolean *changed,
                            GError **error)
{
  g_autofree gchar *dir = g_path_get_dirname(snapshot->path);
  g_autofree gchar *tmp = g_strdup_printf("%s.XXXXXX", snapshot->path);
  int fd;
  gboolean ok;

  if (changed)
    *changed = FALSE;
  if (snapshot->existed && contents->len == snapshot->length &&
      memcmp(contents->str, snapshot->contents, contents->len) == 0)
    return TRUE;
  if (g_mkdir_with_parents(dir, 0755) != 0)
  {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                _("mkdir -p %s: %s"), dir, g_strerror(errno));
    return FALSE;
  }
  fd = g_mkstemp(tmp);
  if (fd < 0)
  {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                _("create temporary file for %s: %s"), snapshot->path,
                g_strerror(errno));
    return FALSE;
  }
  if (!rcl_file_close_temporary(fd, tmp, snapshot->path, error))
    return FALSE;
  ok = g_file_set_contents(tmp, contents->str, contents->len, error);
  if (ok)
    ok = rcl_file_apply_mode_for_write(tmp, snapshot->path, default_mode, error);
  if (ok && rcl_locale_rename(tmp, snapshot->path) != 0)
  {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                _("rename(%s, %s): %s"), tmp, snapshot->path, g_strerror(errno));
    ok = FALSE;
  }
  if (!ok)
    rcl_file_cleanup_temporary(tmp, error);
  else if (changed)
    *changed = TRUE;
  return ok;
}

/* Parse a transaction snapshot without executing either shell script.
 * Keep explicit empty assignments distinct from absent keys. Last literal
 * assignment wins, matching the locale property reader. */
static GHashTable *
rcl_locale_snapshot_values(const gchar *contents, gboolean csh, GError **error)
{
  g_autoptr(GHashTable) entries = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                        g_free, g_free);
  g_auto(GStrv) lines = g_strsplit(contents ? contents : "", "\n", -1);
  guint i;

  for (i = 0; lines[i]; i++)
  {
    g_autofree gchar *key = rcl_locale_assignment_key(lines[i], csh);
    gchar *value;
    if (!key || !rcl_locale_variable_is_known(key))
      continue;
    value = rcl_locale_assignment_value(lines[i], key, csh, NULL);
    if (!value)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Cannot safely read complex locale assignment for %s", key);
      return NULL;
    }
    g_hash_table_replace(entries, g_steal_pointer(&key), value);
  }

  return g_steal_pointer(&entries);
}

/* Update both scripts in the main-loop authorization callback. Other daemon
 * requests cannot interleave between the snapshot and the final rename. */
static gboolean
rcl_commit_locale_files(const gchar *const *keys,
                        const gchar *const *values,
                        gboolean partial, GError **error)
{
  RclFileSnapshot sh = {0}, csh = {0};
  g_autoptr(GString) sh_new = NULL;
  g_autoptr(GString) csh_new = NULL;
  g_autoptr(GHashTable) sh_values = NULL;
  g_autoptr(GHashTable) csh_values = NULL;
  const gchar *merged[G_N_ELEMENTS(locale_variable_names)] = { NULL };
  guint i, j;
  GError *write_error = NULL, *rollback_error = NULL;
  gboolean ok = FALSE, sh_changed = FALSE;

  if (!rcl_file_snapshot_take(&sh, RCL_LANG_SH_FILE, &write_error) ||
      !rcl_file_snapshot_take(&csh, RCL_LANG_CSH_FILE, &write_error))
    goto fail;
  if ((sh.existed && memchr(sh.contents, '\0', sh.length)) ||
      (csh.existed && memchr(csh.contents, '\0', csh.length)))
  {
    g_set_error_literal(&write_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Locale configuration contains embedded NUL bytes");
    goto fail;
  }

  /* Reconcile only during an authorized update, never at startup or on a
   * file-monitor event. For partial updates preserve every existing known
   * value, preferring lang.sh on conflicts because it backs Locale.
   * Explicitly requested values (including removals) override both files. */
  if (partial)
  {
    sh_values = rcl_locale_snapshot_values(sh.contents, FALSE, &write_error);
    if (!sh_values)
      goto fail;
    csh_values = rcl_locale_snapshot_values(csh.contents, TRUE, &write_error);
    if (!csh_values)
      goto fail;
  }

  for (i = 0; locale_variable_names[i]; i++)
  {
    const gchar *key = locale_variable_names[i];
    const gchar *value = NULL;

    for (j = 0; keys[j]; j++)
      if (g_str_equal(key, keys[j]))
      {
        value = values[j] ? values[j] : "";
        break;
      }

    if (!value && partial)
    {
      const gchar *sh_value = g_hash_table_lookup(sh_values, key);
      const gchar *csh_value = g_hash_table_lookup(csh_values, key);

      if (sh_value && csh_value && g_strcmp0(sh_value, csh_value) != 0)
        g_debug("reconciling locale variable %s using lang.sh", key);

      value = sh_value ? sh_value : csh_value;
    }

    merged[i] = value ? value : "";
  }

  sh_new = rcl_locale_patch_contents(sh.contents, locale_variable_names,
                                    merged, FALSE, &write_error);
  if (!sh_new)
    goto fail;
  csh_new = rcl_locale_patch_contents(csh.contents, locale_variable_names,
                                     merged, TRUE, &write_error);
  if (!csh_new)
    goto fail;

  if (!rcl_write_snapshot_contents(&sh, sh_new, 0755, &sh_changed, &write_error))
    goto fail;
  if (!rcl_write_snapshot_contents(&csh, csh_new, 0755, NULL, &write_error))
  {
    if (sh_changed && !rcl_file_snapshot_restore(&sh, &rollback_error))
      rcl_report_rollback_failure(&write_error, rollback_error);
    goto fail;
  }
  ok = TRUE;
  goto out;
fail:
  rcl_report_write_failure(error, write_error);
out:
  rcl_file_snapshot_clear(&sh);
  rcl_file_snapshot_clear(&csh);
  return ok;
}

/* Partial updates reconcile existing values across both shell scripts. */
static gboolean
rcl_patch_locale_files(const gchar *const *keys, const gchar *const *values,
                       GError **error)
{
  return rcl_commit_locale_files(keys, values, TRUE, error);
}

/* SetLocale replaces the complete known locale set in both scripts.
 * Do not inject a shell-specific LC_COLLATE default. */
static gboolean
rcl_write_locale_files(const gchar *const *known_keys,
                       const gchar *const *new_values, GError **error)
{
  return rcl_commit_locale_files(known_keys, new_values, FALSE, error);
}

/* Keyboard files are changed only by authorized public setter handlers.
 * Prepare all requested snapshots before writing any file, then restore
 * only files actually changed if a later operation fails. */
typedef struct
{
  const gchar *path;
  GString *contents; /* NULL means remove the optional toggle sidecar. */
  mode_t mode;
  RclFileSnapshot snapshot;
  gboolean changed;
} RclKeyboardWrite;

static gboolean
rcl_commit_keyboard_files(RclKeyboardWrite *files, guint count, GError **error)
{
  GError *write_error = NULL;
  guint i;
  gboolean ok = FALSE;

  for (i = 0; i < count; i++)
    if (!rcl_file_snapshot_take(&files[i].snapshot, files[i].path, &write_error))
      goto out;

  for (i = 0; i < count; i++)
  {
    RclKeyboardWrite *file = &files[i];

    if (file->contents)
    {
      if (!rcl_write_snapshot_contents(&file->snapshot, file->contents,
                                       file->mode, &file->changed, &write_error))
        goto rollback;
    }
    else if (file->snapshot.existed)
    {
      if (g_unlink(file->path) != 0)
      {
        int saved_errno = errno;
        g_set_error(&write_error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                    _("unlink(%s): %s"), file->path, g_strerror(saved_errno));
        goto rollback;
      }
      file->changed = TRUE;
    }
  }
  ok = TRUE;
  goto out;

rollback:
  while (i > 0)
  {
    GError *rollback_error = NULL;
    RclKeyboardWrite *file = &files[--i];
    if (file->changed &&
        !rcl_file_snapshot_restore(&file->snapshot, &rollback_error))
      rcl_report_rollback_failure(&write_error, rollback_error);
  }
out:
  if (!ok)
    rcl_report_write_failure(error, write_error);
  for (i = 0; i < count; i++)
    rcl_file_snapshot_clear(&files[i].snapshot);
  return ok;
}

static gboolean
write_x11_keyboard_conf(const gchar *path,
    const gchar *layout, const gchar *model, const gchar *variant,
    const gchar *options, GError **error)
{
  g_autoptr(GString) x11 = rcl_render_x11_keyboard(layout, model, variant, options);
  RclKeyboardWrite files[] = {
    { .path = path, .contents = x11, .mode = 0644 }
  };
  return rcl_commit_keyboard_files(files, G_N_ELEMENTS(files), error);
}

static GString *
rcl_render_toggle(const gchar *toggle)
{
  if (!toggle || !*toggle)
    return NULL;
  GString *out = g_string_new(toggle);
  g_string_append_c(out, '\n');
  return out;
}

static gboolean
rcl_write_vconsole_files(const gchar *keymap, const gchar *keymap_toggle,
                         GError **error)
{
  g_autoptr(GString) vc = rcl_render_rc_keymap(keymap);
  g_autoptr(GString) toggle = rcl_render_toggle(keymap_toggle);
  RclKeyboardWrite files[] = {
    { .path = RCL_RC_KEYMAP_FILE, .contents = vc, .mode = 0755 },
    { .path = RCL_RC_KEYMAP_TOGGLE_FILE, .contents = toggle, .mode = 0644 }
  };
  return rcl_commit_keyboard_files(files, G_N_ELEMENTS(files), error);
}

static gboolean
rcl_write_x11_vconsole_files(const gchar *layout, const gchar *model,
    const gchar *variant, const gchar *options, const gchar *keymap,
    GError **error)
{
  g_autoptr(GString) x11 = rcl_render_x11_keyboard(layout, model, variant, options);
  g_autoptr(GString) vc = rcl_render_rc_keymap(keymap);
  RclKeyboardWrite files[] = {
    { .path = RCL_X11_KEYBOARD_FILE, .contents = x11, .mode = 0644 },
    { .path = RCL_RC_KEYMAP_FILE, .contents = vc, .mode = 0755 }
  };
  return rcl_commit_keyboard_files(files, G_N_ELEMENTS(files), error);
}

static gboolean
rcl_write_vconsole_x11_files(const gchar *keymap, const gchar *keymap_toggle,
    const gchar *layout, const gchar *model, const gchar *variant,
    const gchar *options, GError **error)
{
  g_autoptr(GString) vc = rcl_render_rc_keymap(keymap);
  g_autoptr(GString) toggle = rcl_render_toggle(keymap_toggle);
  g_autoptr(GString) x11 = rcl_render_x11_keyboard(layout, model, variant, options);
  RclKeyboardWrite files[] = {
    { .path = RCL_RC_KEYMAP_FILE, .contents = vc, .mode = 0755 },
    { .path = RCL_RC_KEYMAP_TOGGLE_FILE, .contents = toggle, .mode = 0644 },
    { .path = RCL_X11_KEYBOARD_FILE, .contents = x11, .mode = 0644 }
  };
  return rcl_commit_keyboard_files(files, G_N_ELEMENTS(files), error);
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
               _("Unknown property '%s'"), property_name );
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
               _("Property '%s' is read-only; use the Set* methods"), property_name );
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

  if( g_strcmp0(method_name, "SetLocale") == 0 ||
      g_strcmp0(method_name, "UpdateLocale") == 0 )
  {
    gchar    **locale;
    gboolean   interactive;
    GVariant  *locale_v;

    g_variant_get( parameters, "(@asb)", &locale_v, &interactive );
    locale = g_variant_dup_strv( locale_v, NULL );
    g_variant_unref( locale_v );

    ctx = g_new0( AuthCtx, 1 );
    ctx->daemon     = g_object_ref( daemon );
    ctx->invocation = g_object_ref( invocation );
    ctx->op         = g_strcmp0(method_name, "UpdateLocale") == 0
                      ? OP_UPDATE_LOCALE : OP_SET_LOCALE;
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
    ctx->invocation = g_object_ref( invocation );
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

    /*
     * Reject malformed values before contacting PolicyKit.  The operation
     * handler repeats this validation after authorization as defense in depth.
     */
    if( !rcl_x11_keyboard_values_are_valid( layout, model, variant, options ) )
    {
      g_dbus_method_invocation_return_error( invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Invalid X11 keyboard configuration value" );
      return;
    }

    ctx = g_new0( AuthCtx, 1 );
    ctx->daemon      = g_object_ref( daemon );
    ctx->invocation = g_object_ref( invocation );
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

  /*
   * Startup may be called again after a bus/name loss. Always release any
   * previous registration, introspection data, and authorization session
   * before attaching the new connection.
   */
  rcl_daemon_shutdown( daemon );

  priv->connection       = g_object_ref( connection );
  priv->auth_cancellable = g_cancellable_new();
  priv->introspection = g_dbus_node_info_new_for_xml(
                          locale1_introspection_xml, &error );
  if( !priv->introspection )
  {
    g_warning( "Failed to parse introspection XML: %s", error->message );
    g_error_free( error );
    rcl_daemon_shutdown( daemon );
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
    rcl_daemon_shutdown( daemon );
    return FALSE;
  }

  rcl_daemon_sync_dbus_properties( self );

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

  /*
   * Cancel outstanding authorization checks before releasing the session.
   * Their callbacks may still run, but cancellation prevents them from
   * executing the protected Set* operation after this daemon session ends.
   */
  if( priv->auth_cancellable )
    g_cancellable_cancel( priv->auth_cancellable );
  g_clear_object( &priv->auth_cancellable );

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

  /* Guard against finalization without an explicit rcl_daemon_shutdown() call
   * (e.g. on a startup error path).  Unregistering an already-unregistered
   * object is a no-op, and g_clear_object on a NULL pointer is safe. */
  if( priv->connection && priv->registration_id > 0 )
  {
    g_dbus_connection_unregister_object( priv->connection,
                                         priv->registration_id );
    priv->registration_id = 0;
  }
  g_clear_object( &priv->connection );

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
