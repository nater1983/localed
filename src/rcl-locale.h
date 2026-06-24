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

#pragma once

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS

/*---------------------------------------------------------------------------
  RclDaemon – abstract base type shared with main.c
  (Mirrors the pattern used in rcl-hostname.h / rcl-timedate.h so main.c
  stays consistent across all rcl-* daemons.)
 ---------------------------------------------------------------------------*/

#define RCL_TYPE_DAEMON       (rcl_daemon_get_type())
#define RCL_DAEMON(o)         (G_TYPE_CHECK_INSTANCE_CAST ((o), RCL_TYPE_DAEMON, RclDaemon))
#define RCL_IS_DAEMON(o)      (G_TYPE_CHECK_INSTANCE_TYPE ((o), RCL_TYPE_DAEMON))

typedef struct _RclDaemon      RclDaemon;
typedef struct _RclDaemonClass RclDaemonClass;

struct _RclDaemon
{
  GObject parent_instance;
};

struct _RclDaemonClass
{
  GObjectClass parent_class;
};

GType      rcl_daemon_get_type   ( void ) G_GNUC_CONST;
RclDaemon *rcl_daemon_new        ( void );
gboolean   rcl_daemon_startup    ( RclDaemon *daemon, GDBusConnection *connection );
void       rcl_daemon_shutdown   ( RclDaemon *daemon );
void       rcl_daemon_set_debug  ( RclDaemon *daemon, gboolean debug );

/*---------------------------------------------------------------------------
  RclLocaleDaemon – the concrete GObject that owns the D-Bus skeleton for
  org.freedesktop.locale1.

  Properties exposed on the bus (all read-only for clients; written via the
  Set* methods with polkit authorisation):

    Locale                – array of "KEY=value" strings from RCL_LANG_SH_FILE
                             (/etc/profile.d/lang.sh; "export KEY=value" format)
                             (LANG, LANGUAGE, LC_CTYPE, LC_NUMERIC, LC_TIME,
                             LC_COLLATE, LC_MONETARY, LC_MESSAGES, LC_PAPER,
                             LC_NAME, LC_ADDRESS, LC_TELEPHONE,
                             LC_MEASUREMENT, LC_IDENTIFICATION)
    VConsoleKeymap        – keymap name parsed from RCL_RC_KEYMAP_FILE
    VConsoleKeymapToggle  – toggle keymap name from RCL_RC_KEYMAP_TOGGLE_FILE
    X11Layout             – XkbLayout, from RCL_X11_KEYBOARD_FILE
    X11Model              – XkbModel
    X11Variant            – XkbVariant
    X11Options            – XkbOptions

  Methods:
    SetLocale         (as locale, b interactive)
      – validates and writes RCL_LANG_SH_FILE
    SetVConsoleKeyboard(s keymap, s keymap_toggle, b convert, b interactive)
      – writes RCL_RC_KEYMAP_FILE; if convert is TRUE also derives and writes
        a matching X11 layout via the built-in keymap → X11 layout table
    SetX11Keyboard     (s layout, s model, s variant, s options,
                         b convert, b interactive)
      – writes RCL_X11_KEYBOARD_FILE; if convert is TRUE also derives and
        writes a matching console keymap via the same table

  The 'interactive' boolean follows the systemd convention: when TRUE the
  caller is willing to wait for a polkit authentication dialog; when FALSE
  the call should fail immediately if the caller is not already authorised.
 ---------------------------------------------------------------------------*/

#define RCL_TYPE_LOCALE_DAEMON        (rcl_locale_daemon_get_type())
#define RCL_LOCALE_DAEMON(o)          (G_TYPE_CHECK_INSTANCE_CAST  ((o), RCL_TYPE_LOCALE_DAEMON, RclLocaleDaemon))
#define RCL_LOCALE_DAEMON_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST     ((k), RCL_TYPE_LOCALE_DAEMON, RclLocaleDaemonClass))
#define RCL_IS_LOCALE_DAEMON(o)       (G_TYPE_CHECK_INSTANCE_TYPE  ((o), RCL_TYPE_LOCALE_DAEMON))
#define RCL_IS_LOCALE_DAEMON_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE     ((k), RCL_TYPE_LOCALE_DAEMON))
#define RCL_LOCALE_DAEMON_GET_CLASS(o)(G_TYPE_INSTANCE_GET_CLASS   ((o), RCL_TYPE_LOCALE_DAEMON, RclLocaleDaemonClass))

typedef struct _RclLocaleDaemon        RclLocaleDaemon;
typedef struct _RclLocaleDaemonClass   RclLocaleDaemonClass;
typedef struct _RclLocaleDaemonPrivate RclLocaleDaemonPrivate;

struct _RclLocaleDaemon
{
  RclDaemon                 parent_instance;
  RclLocaleDaemonPrivate   *priv;
};

struct _RclLocaleDaemonClass
{
  RclDaemonClass parent_class;
};

GType rcl_locale_daemon_get_type ( void ) G_GNUC_CONST;

/*
 * rcl_daemon_sync_dbus_properties:
 *
 * Re-reads RCL_LANG_SH_FILE, RCL_RC_KEYMAP_FILE, and RCL_X11_KEYBOARD_FILE,
 * then emits PropertiesChanged on the D-Bus object for every value that
 * has changed since the last call.
 *
 * Called from main.c's inotify callback whenever a watched file changes.
 * Also called once during startup (from rcl_daemon_startup) to populate
 * the initial property values.
 */
void rcl_daemon_sync_dbus_properties ( RclLocaleDaemon *daemon );

/*---------------------------------------------------------------------------
  Polkit action IDs used when authorising Set* method calls.
  Defined here so both the daemon implementation and any test harness can
  reference the same strings.
 ---------------------------------------------------------------------------*/

#define RCL_LOCALE_POLKIT_SET_LOCALE   "org.freedesktop.locale1.set-locale"
#define RCL_LOCALE_POLKIT_SET_KEYBOARD "org.freedesktop.locale1.set-keyboard"

/*---------------------------------------------------------------------------
  File paths – gathered in one place so they're easy to override for tests
  or alternative distro layouts.  Defaults come from meson_options.txt via
  config.h.
 ---------------------------------------------------------------------------*/

#ifndef RCL_LANG_SH_FILE
#define RCL_LANG_SH_FILE       "/etc/profile.d/lang.sh"
#endif

#ifndef RCL_RC_KEYMAP_FILE
#define RCL_RC_KEYMAP_FILE         "/etc/rc.d/rc.keymap"
#endif

/* Sidecar file that persists VConsoleKeymapToggle across reboots.
 * Slackware has no native equivalent so we manage it ourselves. */
#ifndef RCL_RC_KEYMAP_TOGGLE_FILE
#define RCL_RC_KEYMAP_TOGGLE_FILE  "/etc/rc.d/rc.keymap.toggle"
#endif

#ifndef RCL_X11_KEYBOARD_FILE
#define RCL_X11_KEYBOARD_FILE  "/etc/X11/xorg.conf.d/00-keyboard.conf"
#endif

G_END_DECLS
