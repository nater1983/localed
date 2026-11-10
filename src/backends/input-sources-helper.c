/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Generic entry point for the single localed input-sources helper process.
 * Desktop-specific readers are linked into this executable at build time.
 */

#include "config.h"

#include <glib.h>

#if HAVE_GNOME_BACKEND
#include "gnome/gnome-input-sources-helper.h"
#elif HAVE_PLASMA6_BACKEND
#include "plasma6/plasma6-input-sources-helper.h"
#endif

int
main(int argc, char **argv)
{
  (void) argc;
  (void) argv;

#if HAVE_GNOME_BACKEND
  return rcl_gnome_input_sources_helper_run();
#elif HAVE_PLASMA6_BACKEND
  return rcl_plasma6_input_sources_helper_run();
#else
  g_printerr("localed-input-sources-helper: no desktop input-source backend was selected\n");
  return 1;
#endif
}
