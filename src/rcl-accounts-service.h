#pragma once
#include <gio/gio.h>
#include "rcl-locale-client.h"
G_BEGIN_DECLS
#define RCL_TYPE_ACCOUNTS_SERVICE (rcl_accounts_service_get_type())
G_DECLARE_FINAL_TYPE(RclAccountsService, rcl_accounts_service, RCL, ACCOUNTS_SERVICE, GObject)
RclAccountsService *rcl_accounts_service_new(void);
gboolean rcl_accounts_service_start(RclAccountsService *self, GError **error);
void rcl_accounts_service_stop(RclAccountsService *self);
G_END_DECLS
