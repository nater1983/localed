/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <gio/gio.h>

G_BEGIN_DECLS
typedef struct _RclSessionMonitor RclSessionMonitor;
typedef void (*RclSessionMonitorCallback)(gpointer user_data);

/* Main-context-owned monitor. Callbacks and user_data are borrowed; stop
 * before destroying the owner. The connection accessor is borrowed. */
RclSessionMonitor *rcl_session_monitor_new(RclSessionMonitorCallback refresh,
                                           RclSessionMonitorCallback lost,
                                           gpointer user_data);
gboolean rcl_session_monitor_start(RclSessionMonitor *self);
void rcl_session_monitor_stop(RclSessionMonitor *self);
void rcl_session_monitor_free(RclSessionMonitor *self);
GDBusConnection *rcl_session_monitor_get_connection(RclSessionMonitor *self);
gboolean rcl_session_monitor_is_available(RclSessionMonitor *self);
/* Retry an incomplete session scan after a bounded delay. */
void rcl_session_monitor_retry_refresh(RclSessionMonitor *self);
/* Notify the owner before debounced discovery, and allow a synchronous scan
 * on the owning main context. */
void rcl_session_monitor_set_invalidated(RclSessionMonitor *self,
                                         RclSessionMonitorCallback callback);
gboolean rcl_session_monitor_refresh_now(RclSessionMonitor *self);
G_END_DECLS
