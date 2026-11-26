/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Generic entry point for the single localed input-sources helper process.
 * Desktop-specific readers are linked into this executable at build time.
 */

#include "config.h"

#include <glib.h>
#include <locale.h>

#if HAVE_GNOME_BACKEND
#include "gnome/gnome-input-sources-helper.h"
#endif
#if HAVE_PLASMA6_BACKEND
#include "plasma6/plasma6-input-sources-helper.h"
#endif

int
main(int argc, char **argv)
{
  setlocale(LC_ALL, "");
  bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  /* Keep the legacy no-argument invocation for single-backend builds.
   * Explicit selection is required when both readers are compiled in. */
  const gchar *id = NULL;
  if (argc == 2)
    id = argv[1];
  else if (argc == 1)
  {
#if HAVE_GNOME_BACKEND && !HAVE_PLASMA6_BACKEND
    id = "gnome";
#elif HAVE_PLASMA6_BACKEND && !HAVE_GNOME_BACKEND
    id = "plasma6";
#endif
  }

#if HAVE_GNOME_BACKEND
  if (g_strcmp0(id, "gnome") == 0)
    return rcl_gnome_input_sources_helper_run();
#endif
#if HAVE_PLASMA6_BACKEND
  if (g_strcmp0(id, "plasma6") == 0)
    return rcl_plasma6_input_sources_helper_run();
#endif
  (void) id;
  g_printerr("%s\\n", _("localed-input-sources-helper: select a compiled desktop backend (gnome or plasma6)"));
  return 1;
}
