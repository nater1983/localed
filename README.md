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
- Integrates with AccountsService when it is available at runtime.
- Provides optional GNOME and Plasma 6 input-source integration through independently loadable GModule backends, with simultaneous module support.
- Does not require systemd; it uses D-Bus and a logind-compatible session
  manager for desktop session discovery.

GNOME and Plasma 6 use AccountsService for automatic primary locale
synchronization. Neither desktop backend monitors locale configuration files.

## Dependencies

| Library / service | pkg-config name | Minimum version / requirement |
|---|---|---|
| GLib / GIO / GModule | `glib-2.0`, `gio-2.0`, `gio-unix-2.0`, `gmodule-2.0` | 2.56 |
| Polkit | `polkit-gobject-1` | 0.113 |
| D-Bus daemon | — | Runtime requirement |
| logind-compatible service | — | Runtime requirement for desktop session discovery |

The daemon is activated by `dbus-daemon` through D-Bus system-service
activation (`[D-BUS Service]`). There is no systemd dependency.
AccountsService integration is optional at runtime: `localed` monitors its
D-Bus name and synchronizes account language changes when the service is
available. No AccountsService development library is required.

The GNOME reader uses GIO/GSettings; the Plasma reader uses GIO/GDBus.
Both are compiled into one user-side input-sources helper executable.
Neither requires desktop-specific development libraries.

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

The current 0.4.0 development configuration defaults to building both desktop
modules. Select the desired configuration explicitly for packaging.

```sh
meson setup build --prefix=/usr --libdir=lib64 --libexecdir=libexec \
    --sysconfdir=/etc -Ddesktop-backend=both -Dtests=false
meson compile -C build
```

The example uses Slackware x86_64 paths; select the appropriate libdir on
other architectures.

| Option | Description |
|---|---|
| `none` | Build without desktop keyboard integration. |
| `gnome` | Build the GNOME module and helper reader. |
| `plasma6` | Build the Plasma 6 module and helper reader. |
| `both` | Build both modules and readers (current default). |

### Automated tests

Run the entire backend build matrix with isolated configuration files:

```sh
sh tests/run-backend-matrix.sh
```

The runner creates an independent temporary build and configuration directory
for each backend selection, executes tests serially, and retains the logs.
For a single configuration, use an isolated test build:

```sh
test_config=$(mktemp -d)
meson setup build-tests -Ddesktop-backend=both -Dtests=true \
    -Dlang_sh="$test_config/lang.sh" \
    -Drc_keymap="$test_config/rc.keymap" \
    -Dx11_keyboard_conf="$test_config/00-keyboard.conf"
meson compile -C build-tests
meson test -C build-tests --num-processes 1 --print-errorlogs
```

Use a fresh test build directory. Do not install a test build or run the tests
with production configuration paths. The test binaries are not installed.

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

All backend configurations install the same daemon and helper executable
names under `libexecdir` (normally `/usr/libexec`):

```text
/usr/libexec/
├── localed
└── localed-input-sources-helper
```

Selected desktop modules are installed in `<libdir>/localed/backends`:
on Slackware x86_64, `/usr/lib64/localed/backends/gnome.so` and
`/usr/lib64/localed/backends/plasma.so`. The module loader uses the configured
installation directory; the daemon and modules share internal ABI version 6
and must be installed from matching builds.

There is one input-sources helper executable. Desktop-specific reader code
is compiled into it when selected, and the daemon starts it with `gnome` or
`plasma6`. Do not run the helper as root for session diagnostics.

For upgrades, confirm the Meson prefix before installation: a default
`/usr/local` installation does not replace binaries under `/usr`.
Restart the running daemon after installing matching modules and helper.

## Desktop input-source integration

Desktop integration is selected at build time with `-Ddesktop-backend`.
The backend manager loads the selected modules at runtime. Desktop-specific
code remains outside the core daemon.

Discovery instances report complete session snapshots. Operational instances
are bound to a specific local user's elogind session; inactive instances
are retained across virtual terminal switches. Only one eligible active
graphical session can own automatic keyboard writes. The manager revokes
ownership on invalidation and rechecks eligibility before granting writes;
the backend checks global session uniqueness again before applying a
snapshot. Ambiguous or failed discovery does not authorize a new writer.
The helper is stopped on ownership loss and restarted when ownership returns.

### Console-launched X11 desktops

Slackware users may launch a desktop with `startx` instead of a display
manager. Elogind can report the original `Type=tty` with an empty Desktop
property even while Xorg is running. localed resolves these sessions to X11
only after checking the Xorg executable (root-owned regular file, not
group/world writable), process UID and exact elogind session membership.
The backend independently verifies the desktop and user session bus before
starting the helper. Plain console logins are not graphical sessions.

Console sessions are periodically rechecked at 2.5-second intervals because
starting or exiting Xorg need not change elogind's session properties.
Unchanged discovery and nonmatching desktops are not logged repeatedly.
Normal session transitions and inspection errors remain visible in debug mode.
A daemon must be running for automatic synchronization to occur.

For diagnostics, run `/usr/libexec/localed --debug` as root with any existing
instance stopped (or use `--replace`), and run desktop session D-Bus commands
as the actual desktop user. `loginctl show-session` may still report
`Type=tty`; localed does not alter elogind's session properties.

### GNOME

Build with:

```sh
meson setup build-gnome -Ddesktop-backend=gnome
```

When enabled, the GNOME backend:

- Watches logind for GNOME session changes.
- Identifies the active local GNOME session and its user.
- Locates and connects to that user's GNOME session bus.
- Reads the GNOME input-source settings through GSettings.
- Monitors changes to the GNOME input-source list and active source.
- Synchronizes the complete GNOME XKB layout, variant, and options list to
  the configured Xorg keyboard file.
- Synchronizes the active GNOME source to Slackware's console keymap when a
  matching console keymap mapping is available.

The complete XKB layout list and the active console keymap are deliberately
handled separately. For example, if GNOME has `us` and `es` configured
but `us` is active, the Xorg configuration contains `us,es` while
`rc.keymap` uses `us`. Switching the active GNOME source to `es` changes
the console keymap to `es` without removing either XKB layout from the
Xorg configuration.

The GNOME backend is loaded from `gnome.so`; its session settings reader
is compiled into the shared `localed-input-sources-helper` executable.
The helper executable name and installation path are the same for every
backend configuration.


### KDE Plasma 6

Build with:

```sh
meson setup build-plasma6 -Ddesktop-backend=plasma6
```

The Plasma 6 backend communicates with KWin over the user's session bus:

- Service: `org.kde.keyboard`
- Object path: `/Layouts`
- Interface: `org.kde.KeyboardLayouts`

It calls `getLayoutsList()` to retrieve the ordered `a(sss)` layout list and
`getLayout()` to retrieve the active layout index. KWin's `LayoutNames`
tuple contains `shortName`, `displayName`, and `longName`; the second
field is **not** an XKB variant. The helper reads `VariantList` and
`Options` from the session user's `~/.config/kxkbrc` and aligns variants
with the ordered layout list.

The helper monitors KWin's `layoutChanged` and `layoutListChanged` signals.
Active-layout changes update the Slackware console keymap, while layout-list
changes refresh the complete XKB layout, variant, and options configuration
in the configured Xorg keyboard file. The existing XKB validation and
console-keymap mapping are reused.

The Plasma backend is loaded from `plasma.so`. As with GNOME, the Plasma-specific reader is compiled into the single
`localed-input-sources-helper` executable. No separate Plasma helper binary
or KDE development library is required; communication uses GIO/GDBus.

### Adding other desktop environments

The backend interface is intended to keep desktop-specific integration out
of the locale daemon's core. Future desktop integrations can add their
implementation behind the same versioned module interface and build-time selection. They
should reuse the single `localed-input-sources-helper` executable rather
than install a separate helper binary for each desktop.

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

## Keyboard conversion

`SetVConsoleKeyboard` and `SetX11Keyboard` both accept a `convert`
boolean. When `true`, a built-in mapping table translates the console
keymap to an X11 layout (or vice versa). The table covers common
single-layout keymaps (`us`, `de`, `fr`, `gb`, `es`, `ru`, `se`,
etc.). No conversion is attempted when no match is found, rather than
guessing.

## AccountsService integration

When `org.freedesktop.Accounts` is available on the system bus, `localed`
subscribes to account `Language` property changes and applies them for
accounts in the configured privileged group (default: `wheel`) with the
only active local user session (including TTY-based desktop sessions). The monitor reconnects when
AccountsService appears or restarts. Without AccountsService, automatic
desktop locale synchronization is unavailable; manual locale changes
through locale1 remain supported. Plasma 6.7.6 was runtime-tested with
SetLanguage; other Plasma versions have not yet been runtime-verified.
Because D-Bus property-change signals do not identify the original caller,
this integration cannot perform caller-based Polkit authorization. Other
users should request system-wide changes through the
`org.freedesktop.locale1` D-Bus methods, which are authorized by Polkit.

## Adding a translation

1. Add the language code to `po/LINGUAS` (one code per line).
2. Generate an initial `.po` file:
   ```sh
   ninja -C build localed-pot
   msginit -l de -o po/de.po -i po/localed.pot
   ```
3. Translate the strings in `po/de.po`.
4. Rebuild; Meson compiles and installs `.mo` files automatically.
