_dbus_srv_opt = get_option('dbus_system_service_dir')
if _dbus_srv_opt != ''
  dbus_service_dir = _dbus_srv_opt
else
  dbus_dep = dependency('dbus-1', required : false)
  if dbus_dep.found()
    dbus_service_dir = dbus_dep.get_variable(
      pkgconfig        : 'system_bus_services_dir',
      pkgconfig_define : ['datadir', join_paths(prefix, datadir)],
    )
  else
    dbus_service_dir = join_paths(prefix, datadir, 'dbus-1', 'system-services')
  endif
endif

dbus_conf_dir_opt = get_option('dbus_system_conf_dir')

if dbus_conf_dir_opt != ''
  dbus_conf_dir = dbus_conf_dir_opt
else
  dbus_conf_dir = join_paths(prefix, datadir, 'dbus-1', 'system.d')
endif

_polkit_opt = get_option('polkit_policy_dir')
if _polkit_opt != ''
  polkit_policy_dir = _polkit_opt
else
  if polkit_found
    polkit_policy_dir = polkit.get_variable(
      pkgconfig        : 'actiondir',
      pkgconfig_define : ['datadir', join_paths(prefix, datadir)],
    )
  else
    polkit_policy_dir = join_paths(prefix, datadir, 'polkit-1', 'actions')
  endif
endif

dbus_service_conf = configuration_data()
dbus_service_conf.set('SBINDIR', join_paths(prefix, get_option('libexecdir')))

configure_file(
  input         : 'org.freedesktop.locale1.service.in',
  output        : 'org.freedesktop.locale1.service',
  configuration : dbus_service_conf,
  install       : true,
  install_dir   : dbus_service_dir,
)

install_data(
  'org.freedesktop.locale1.conf',
  install_dir : dbus_conf_dir,
)

if polkit_found
  install_data(
    'org.freedesktop.locale1.policy',
    install_dir : polkit_policy_dir,
  )
endif

rules_conf = configuration_data()
rules_conf.set('PRIVILEGED_GROUP', get_option('privileged-group'))

rules_file = configure_file(
  input         : 'org.freedesktop.locale1.rules.in',
  output        : 'org.freedesktop.locale1.rules',
  configuration : rules_conf,
)

polkit_rules_dir = join_paths(prefix, datadir, 'polkit-1', 'rules.d')

install_data(
  rules_file,
  install_dir : polkit_rules_dir,
)

summary({
  'D-Bus service dir' : dbus_service_dir,
  'D-Bus conf dir'    : dbus_conf_dir,
  'polkit policy dir' : polkit_policy_dir,
}, section : 'Data files')
