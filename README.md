# localed

A lightweight implementation of the `org.freedesktop.locale1` D-Bus service
(`localed`) for Slackware and other non-systemd Linux distributions. It
provides system locale and keyboard configuration through the standard
locale1 D-Bus interface, without depending on systemd.

The daemon follows the same general service pattern as
[nater1983/hostnamed](https://github.com/nater1983/hostnamed).

## Features

- Implements the `org.freedesktop.locale1` D-Bus interface.
- Reads and writes Slackware's locale and keyboard configuration files.
- Uses atomic file updates and transactional rollback for multi-file changes.
- Watches configuration files with inotify and emits D-Bus
  `PropertiesChanged` notifications when values change.
- Validates Xorg XKB configuration before accepting or reporting values.
- Uses Polkit for authorization, with a configurable privileged group
  (default: `wheel`).
- Does not require systemd, elogind, AccountsService or any desktop environment.
- System configuration changes require authorized locale1 requests; personal
  desktop settings remain owned by the desktop.

Desktop integration is maintained separately in the [localed-desktop source tree](https://github.com/nater1983/localed-desktop).
It is not part of the core daemon.

### Optional desktop companion

After acquiring the locale1 bus name, `localed` checks its configured
libexec directory for `localed-input-sources-helper` (normally
`/usr/libexec/localed-input-sources-helper`), provided by the separate
localed-desktop package. If installed as a trusted root-owned
executable in a root-owned, non-writable-by-other-users directory tree,
localed starts it in observation-only mode. It does not search PATH or
follow symbolic links in that path. If the executable is absent, unsafe,
or fails to launch, the core locale1 service continues normally. No
restart timer is scheduled if the companion exits; detection is repeated
on the next locale1 ownership acquisition or daemon restart.

The core stops its owned companion on shutdown or loss of locale1 ownership.
The installed helper is a single executable: the core starts it with
`--managed-companion`, and it invokes itself with `--session-worker`
after dropping to the verified desktop session user. Desktop selection
and worker lifecycle remain in the companion package. No desktop build or
runtime dependencies are added to localed.

To disable automatic companion startup, launch localed with
`--no-desktop-companion`. Do not run an independently managed companion at
the same time as the automatically started one. Automatic discovery does
not enable synchronization of personal desktop settings to system defaults.
Upgrade or remove legacy integrated input-sources helpers before using
this feature; older binaries with the same name are not compatible.

## Dependencies

| Library / service | pkg-config name | Minimum version / requirement |
|---|---|---|
| GLib / GIO | `glib-2.0`, `gio-2.0`, `gio-unix-2.0` | 2.56 |
| Polkit | `polkit-gobject-1` | 0.113 |
| D-Bus daemon | — | Runtime requirement |

The daemon is activated by `dbus-daemon` through D-Bus system-service
activation (`[D-BUS Service]`). There is no systemd dependency.
On Debian/Ubuntu:

```sh
apt install libglib2.0-dev libpolkit-gobject-1-dev \
            meson ninja-build pkg-config
```

On Fedora/RHEL:

```sh
dnf install glib2-devel polkit-devel \
            meson ninja-build pkg-config
```

## Building

Build the standalone system configuration daemon:

```sh
meson setup build
meson compile -C build
```

Automated tests are disabled by default and can be enabled for development
or package validation:

```sh
meson setup build-tests -Dtests=true
meson compile -C build-tests
meson test -C build-tests --print-errorlogs
```

The D-Bus test executable automatically creates a private configuration
directory for each fixture under the build's tests directory, independently
of the daemon's configured paths. No manual path overrides are necessary.
The production daemon retains its normal configuration paths even with
`-Dtests=true`. The external private-bus reconnect shell harness starts a
real daemon; use a separately configured daemon with isolated paths for
that harness.

The SlackBuild enables the test targets and runs the suite before installing
the package. Test executables are not installed into the resulting package.

### Common Meson options

Install under `/usr` instead of `/usr/local`:

```sh
meson setup build --prefix=/usr
```

Override the D-Bus service directory explicitly:

```sh
meson setup build -Ddbus_system_service_dir=/usr/share/dbus-1/system-services
```

Override locale and keyboard file paths (Slackware defaults shown):

```sh
meson setup build \
    -Drc_keymap=/etc/rc.d/rc.keymap \
    -Dlang_sh=/etc/profile.d/lang.sh \
    -Dprivileged-group=wheel \
    -Dx11_keyboard_conf=/etc/X11/xorg.conf.d/00-keyboard.conf
```

## Installing

```sh
meson install -C build
```

For a system-wide installation, configure with `--prefix=/usr` and install
with the required privileges.

The daemon is installed under `libexecdir` (normally `/usr/libexec/localed`).
The `localectl` client is installed under `bindir`. No desktop helper is installed.

## D-Bus interface quick reference

Read the `Locale` property:

```sh
busctl get-property org.freedesktop.locale1 \
       /org/freedesktop/locale1 org.freedesktop.locale1 Locale
```

Set the system locale (may trigger a Polkit prompt):

```sh
busctl call org.freedesktop.locale1 \
       /org/freedesktop/locale1 org.freedesktop.locale1 \
       SetLocale asb 1 LANG=en_US.UTF-8 true
```

Set the X11 keyboard layout:

```sh
busctl call org.freedesktop.locale1 \
       /org/freedesktop/locale1 org.freedesktop.locale1 \
       SetX11Keyboard ssssbb us pc105 "" "" false true
```

Set the console keymap and convert to X11:

```sh
busctl call org.freedesktop.locale1 \
       /org/freedesktop/locale1 org.freedesktop.locale1 \
       SetVConsoleKeyboard ssbb us "" true true
```

## Files managed

| File | Property / purpose |
|---|---|
| `/etc/profile.d/lang.sh` | `Locale` (array of `KEY=value`) |
| `/etc/profile.d/lang.csh` | C-shell companion locale settings (written alongside `lang.sh`) |
| `/etc/rc.d/rc.keymap` | `VConsoleKeymap` |
| `/etc/rc.d/rc.keymap.toggle` | `VConsoleKeymapToggle` |
| `/etc/X11/xorg.conf.d/00-keyboard.conf` | `X11Layout`, `X11Model`, `X11Variant`, `X11Options` |

The configured files are watched with inotify; `localed` re-reads them and
emits `PropertiesChanged` when their values change. The locale property is
read from Slackware's `lang.sh` script, not `/etc/locale.conf`. The console
keymap is managed through Slackware's `rc.keymap` script, not
`/etc/vconsole.conf`.

## Partial system locale updates

The bundled `localectl` preserves existing locale settings that were not
named in the command:

```sh
localectl set-locale LANG=en_US.UTF-8
localectl set-locale LC_COLLATE=C
localectl set-locale LC_COLLATE=
localectl unset-locale LC_COLLATE
```

The final two commands are equivalent. An empty value removes the named
assignment from both `lang.sh` and `lang.csh`. Other existing assignments,
including `LANGUAGE`, are preserved. The daemon reads the files after
Polkit authorization and updates them transactionally. This supports
simple literal locale assignments, not arbitrary shell programs.
The standard `SetLocale` D-Bus method retains its replacement semantics;
the bundled client uses the new `UpdateLocale` extension instead. Upgrade
the daemon and client together; older daemons do not expose `UpdateLocale`.

## Keyboard conversion

`SetVConsoleKeyboard` and `SetX11Keyboard` both accept a `convert`
boolean. When `true`, a built-in mapping table translates the console
keymap to an X11 layout (or vice versa). The table covers common
single-layout keymaps (`us`, `de`, `fr`, `gb`, `es`, `ru`, `se`,
etc.). No conversion is attempted when no match is found, rather than
guessing.

## Adding a translation

1. Add the language code to `po/LINGUAS` (one code per line).
2. Generate an initial `.po` file:
   ```sh
   ninja -C build localed-pot
   msginit -l de -o po/de.po -i po/localed.pot
   ```
3. Translate the strings in `po/de.po`.
4. Rebuild; Meson compiles and installs `.mo` files automatically.
