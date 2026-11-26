/* SPDX-License-Identifier: GPL-2.0-or-later */
/* White-box manager tests: use fake modules, not the host system bus.
 * Include the implementation to inspect private claims without exporting
 * test-only interfaces from the daemon. */
#include "../../src/backends/backend-manager.c"

#define SESSION_ONE "/org/freedesktop/login1/session/_31"
#define SESSION_TWO "/org/freedesktop/login1/session/_32"

typedef struct {
  GObject parent;
  GVariant *snapshot;
  gchar *id, *path;
  guint uid;
  gboolean started, enabled;
} FakeBackend;
typedef struct { GObjectClass parent; } FakeBackendClass;

static gboolean candidate_ready;
static guint enabled_count;
GType fake_backend_get_type(void);
static void fake_iface_init(RclBackendInterface *iface);
G_DEFINE_TYPE_WITH_CODE(FakeBackend, fake_backend, G_TYPE_OBJECT,
                       G_IMPLEMENT_INTERFACE(RCL_TYPE_BACKEND, fake_iface_init))

static void
fake_finalize(GObject *object)
{
  FakeBackend *self = (FakeBackend *) object;
  g_assert_false(self->enabled);
  g_clear_pointer(&self->snapshot, g_variant_unref);
  g_free(self->id);
  g_free(self->path);
  G_OBJECT_CLASS(fake_backend_parent_class)->finalize(object);
}

static void
fake_backend_class_init(FakeBackendClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = fake_finalize;
  g_signal_new("sessions-changed", G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_VARIANT);
  g_signal_new("sessions-invalidated", G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  g_signal_new("active-session-changed", G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 7,
      G_TYPE_BOOLEAN, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING,
      G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
}

static void fake_backend_init(FakeBackend *self) { (void) self; }
static gboolean fake_start(RclBackend *backend, GError **error)
{
  (void) error;
  ((FakeBackend *) backend)->started = TRUE;
  return TRUE;
}
static void fake_enable(RclBackend *backend, gboolean enabled)
{
  FakeBackend *self = (FakeBackend *) backend;
  if (self->enabled == enabled)
    return;
  self->enabled = enabled;
  if (enabled)
  {
    g_assert_nonnull(self->path);
    g_assert_true(self->started);
    enabled_count++;
    g_assert_cmpuint(enabled_count, ==, 1);
  }
  else
  {
    g_assert_cmpuint(enabled_count, ==, 1);
    enabled_count--;
  }
}
static void fake_stop(RclBackend *backend)
{
  fake_enable(backend, FALSE);
  ((FakeBackend *) backend)->started = FALSE;
}
static gboolean
fake_bind(RclBackend *backend, guint uid, const gchar *id,
          const gchar *path, GError **error)
{
  FakeBackend *self = (FakeBackend *) backend;
  if (self->started || self->path)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "already bound");
    return FALSE;
  }
  self->uid = uid;
  self->id = g_strdup(id);
  self->path = g_strdup(path);
  return TRUE;
}
static void fake_refresh(RclBackend *backend)
{
  FakeBackend *self = (FakeBackend *) backend;
  if (self->path)
    g_signal_emit_by_name(self, "active-session-changed",
        candidate_ready, self->uid, self->id, self->path,
        "seat0", "wayland", "test");
  else
    g_signal_emit_by_name(self, "sessions-changed", self->snapshot);
}
static void fake_iface_init(RclBackendInterface *iface)
{
  iface->start = fake_start;
  iface->stop = fake_stop;
  iface->set_write_enabled = fake_enable;
  iface->bind_session = fake_bind;
  iface->refresh = fake_refresh;
}
static RclBackend *fake_create(RclLocaleDaemon *daemon)
{
  (void) daemon;
  return g_object_new(fake_backend_get_type(), NULL);
}

static const RclBackendModuleDescriptor gnome_descriptor = {
  RCL_BACKEND_MODULE_ABI_VERSION, sizeof(RclBackendModuleDescriptor),
  "gnome", fake_create
};
static const RclBackendModuleDescriptor plasma_descriptor = {
  RCL_BACKEND_MODULE_ABI_VERSION, sizeof(RclBackendModuleDescriptor),
  "plasma6", fake_create
};

static FakeBackend *
add_monitor(RclBackendManager *manager, const RclBackendModuleDescriptor *desc)
{
  RclBackend *backend = fake_create(manager->daemon);
  g_signal_connect(backend, "sessions-changed", G_CALLBACK(sessions_changed), manager);
  g_signal_connect(backend, "sessions-invalidated", G_CALLBACK(sessions_invalidated), manager);
  g_hash_table_insert(manager->modules, g_strdup(desc->id), (gpointer) desc);
  g_hash_table_insert(manager->backends, g_strdup(desc->id), backend);
  return (FakeBackend *) backend;
}

static void
set_snapshot(FakeBackend *monitor, const gchar *value)
{
  g_clear_pointer(&monitor->snapshot, g_variant_unref);
  if (value)
  {
    monitor->snapshot = g_variant_parse(G_VARIANT_TYPE("a(usob)"),
                                       value, NULL, NULL, NULL);
    g_assert_nonnull(monitor->snapshot);
  }
  g_signal_emit_by_name(monitor, "sessions-invalidated");
}

static void
run_round(RclBackendManager *manager)
{
  if (manager->reconcile_id)
  {
    g_source_remove(manager->reconcile_id);
    manager->reconcile_id = 0;
  }
  reconcile_cb(manager);
}

static void
test_descriptor(void)
{
  RclBackendModuleDescriptor desc = gnome_descriptor;
  GError *error = NULL;
  g_assert_true(rcl_backend_module_validate(&desc, "gnome", &error));
  g_assert_no_error(error);
  desc.abi_version--;
  g_assert_false(rcl_backend_module_validate(&desc, "gnome", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error(&error);
  desc = gnome_descriptor;
  desc.struct_size = 0;
  g_assert_false(rcl_backend_module_validate(&desc, "gnome", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error(&error);
  desc = gnome_descriptor;
  desc.create = NULL;
  g_assert_false(rcl_backend_module_validate(&desc, "gnome", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);
  g_assert_false(rcl_backend_module_validate(&gnome_descriptor, "plasma6", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);
  g_assert_null(rcl_backend_module_open("relative.so", "gnome", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
}

static void
test_arbitration(void)
{
  RclLocaleDaemon *daemon = g_object_new(RCL_TYPE_LOCALE_DAEMON, NULL);
  RclBackendManager *manager = rcl_backend_manager_new(daemon);
  FakeBackend *primary = NULL, *secondary = NULL;
  RclBackend *first;
  GWeakRef first_ref;
  GObject *retained;
  guint i;

  if (HAVE_GNOME_BACKEND)
    primary = add_monitor(manager, &gnome_descriptor);
  if (HAVE_PLASMA6_BACKEND)
  {
    if (primary)
      secondary = add_monitor(manager, &plasma_descriptor);
    else
      primary = add_monitor(manager, &plasma_descriptor);
  }
  g_assert_nonnull(primary);
  g_assert_true(rcl_backend_manager_start(manager, NULL));
  candidate_ready = TRUE;
  set_snapshot(primary, "[(1000, '1', '" SESSION_ONE "', true)]");
  run_round(manager);
  if (secondary)
  {
    g_assert_null(manager->owner);
    set_snapshot(secondary, "[]");
    run_round(manager);
  }
  g_assert_nonnull(manager->owner);
  first = manager->owner;
  g_weak_ref_init(&first_ref, first);
  g_assert_cmpuint(enabled_count, ==, 1);

  /* A periodic unchanged snapshot must not churn a healthy writer.
   * But loss of global eligibility (e.g. an unclaimed Xorg session)
   * must revoke it even without a change in desktop discovery claims. */
  fake_refresh(RCL_BACKEND(primary));
  g_assert_true(manager->owner == first);
  g_assert_cmpuint(manager->reconcile_id, ==, 0);
  candidate_ready = FALSE;
  fake_refresh(RCL_BACKEND(primary));
  g_assert_null(manager->owner);
  g_assert_cmpuint(enabled_count, ==, 0);
  candidate_ready = TRUE;
  run_round(manager);
  g_assert_true(manager->owner == first);

  set_snapshot(primary, "[(1000, '1', '" SESSION_ONE "', false)]");
  g_assert_null(manager->owner);
  run_round(manager);
  g_assert_null(manager->owner);
  g_assert_cmpuint(g_hash_table_size(manager->sessions), ==, 1);
  retained = g_weak_ref_get(&first_ref);
  g_assert_true((gpointer) retained == (gpointer) first);
  g_object_unref(retained);

  set_snapshot(primary, "[(1000, '1', '" SESSION_ONE "', true)]");
  run_round(manager);
  g_assert_true(manager->owner == first);

  if (secondary)
    set_snapshot(secondary, "[(1001, '2', '" SESSION_TWO "', true)]");
  else
    set_snapshot(primary, "[(1000, '1', '" SESSION_ONE "', true),"
                         "(1001, '2', '" SESSION_TWO "', true)]");
  run_round(manager);
  g_assert_null(manager->owner);
  g_assert_cmpuint(enabled_count, ==, 0);
  g_assert_cmpuint(g_hash_table_size(manager->sessions), ==, 2);

  if (secondary)
    set_snapshot(secondary, "[]");
  else
    set_snapshot(primary, "[(1000, '1', '" SESSION_ONE "', true)]");
  candidate_ready = FALSE;
  run_round(manager);
  g_assert_null(manager->owner);
  candidate_ready = TRUE;
  run_round(manager);
  g_assert_true(manager->owner == first);

  if (secondary)
  {
    set_snapshot(secondary, NULL);
    run_round(manager);
    g_assert_null(manager->owner);
    set_snapshot(secondary, "[]");
    run_round(manager);
    g_assert_true(manager->owner == first);
  }

  set_snapshot(primary, "[(1000, '1', '" SESSION_ONE "', true),"
                       "(1001, '2', '" SESSION_ONE "', false)]");
  run_round(manager);
  g_assert_null(manager->owner);
  g_assert_null(g_weak_ref_get(&first_ref));
  g_weak_ref_clear(&first_ref);
  set_snapshot(primary, "[(1000, '1', '" SESSION_ONE "', true)]");
  run_round(manager);
  g_assert_nonnull(manager->owner);
  set_snapshot(primary, "[]");
  run_round(manager);
  g_assert_null(manager->owner);
  g_assert_cmpuint(g_hash_table_size(manager->sessions), ==, 0);

  for (i = 0; i < 3; i++)
  {
    rcl_backend_manager_stop(manager);
    g_assert_cmpuint(manager->reconcile_id, ==, 0);
    g_assert_cmpuint(g_hash_table_size(manager->reported), ==, 0);
    g_assert_true(rcl_backend_manager_start(manager, NULL));
    set_snapshot(primary, "[(1000, '1', '" SESSION_ONE "', true)]");
    run_round(manager);
    g_assert_nonnull(manager->owner);
  }
  set_snapshot(primary, "[]"); /* leave a scheduled reconciliation */
  rcl_backend_manager_free(manager);
  g_assert_cmpuint(enabled_count, ==, 0);
  g_object_unref(daemon);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/backend-module/descriptor", test_descriptor);
  g_test_add_func("/backend-manager/arbitration-lifecycle", test_arbitration);
  return g_test_run();
}
