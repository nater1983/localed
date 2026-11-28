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
- Uses Polkit policy for public locale and keyboard configuration requests,
  without granting unconditional authorization to members of `wheel`.
- Observes AccountsService per-user language changes without changing system defaults.
- Provides optional desktop input-source integration through a build-time
  backend selection.
- Does not require systemd; it uses D-Bus and a logind-compatible session
  manager for desktop session discovery.

The Plasma 6 input-source backend observes keyboard settings only. It does
not monitor `~/.config/plasma-localerc` or copy a user's locale preferences
to the system-wide locale scripts. Plasma continues to manage its own
per-user locale preferences.

## Dependencies

| Library / service | pkg-config name | Minimum version / requirement |
|---|---|---|
| GLib / GIO | `glib-2.0`, `gio-2.0`, `gio-unix-2.0` | 2.56 |
| Polkit | `polkit-gobject-1` | 0.113 |
| D-Bus daemon | — | Runtime requirement |
| logind-compatible service | — | Runtime requirement for desktop session discovery |

The daemon is activated by `dbus-daemon` through D-Bus system-service
activation (`[D-BUS Service]`). There is no systemd dependency.
AccountsService observation is optional at runtime: `localed` monitors its
D-Bus name and logs per-user language changes when the service is available.
No AccountsService development library is required.

The GNOME input-source backend uses GIO/GSettings and the GNOME session's
D-Bus connection. It does not add a separate helper executable or a separate
build-time GNOME library dependency.

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

The default build does not enable a desktop integration backend:

```sh
meson setup build
meson compile -C build
```

To build the GNOME input-source backend:

```sh
meson setup build-gnome -Ddesktop-backend=gnome
meson compile -C build-gnome
```

To build the KDE Plasma 6 input-source backend:

```sh
meson setup build-plasma6 -Ddesktop-backend=plasma6
meson compile -C build-plasma6
```

The backend is selected with the `desktop-backend` Meson option:

| Option | Description |
|---|---|
| `none` | Default. Build localed without a desktop-specific input-source backend. |
| `gnome` | Build the GNOME session and input-source integration. |
| `plasma6` | Build the KDE Plasma 6 session and input-source integration. |

For example, to explicitly select the default configuration:

```sh
meson setup build -Ddesktop-backend=none
```

Automated tests are disabled by default and can be enabled for development
or package validation:

```sh
meson setup build-tests -Dtests=true
meson compile -C build-tests
meson test -C build-tests --print-errorlogs
```

To include the GNOME backend tests:

```sh
meson setup build-gnome-tests -Ddesktop-backend=gnome -Dtests=true
meson compile -C build-gnome-tests
meson test -C build-gnome-tests --print-errorlogs
```

To include the Plasma 6 backend tests:

```sh
meson setup build-plasma6-tests -Ddesktop-backend=plasma6 -Dtests=true
meson compile -C build-plasma6-tests
meson test -C build-plasma6-tests --print-errorlogs
```

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

All backend configurations install the same daemon and helper executable
names under `libexecdir` (normally `/usr/libexec`):

```text
/usr/libexec/
├── localed
└── localed-input-sources-helper
```

There is one input-sources helper executable. Desktop-specific helper code
is compiled into that executable when its backend is selected; GNOME does not
install a separately named GNOME helper.

### Authorization and upgrading

The shipped Polkit policy uses `auth_admin` for inactive and remote
callers and `auth_admin_keep` for active local callers. The vendor rule
at `/usr/share/polkit-1/rules.d/org.freedesktop.locale1.rules` explicitly
applies the same requirements to members of the configured privileged
group (default: `wheel`). Other callers fall through to Polkit policy.
Group membership alone does not authorize a request; Polkit may
temporarily retain administrator authentication for active local sessions.

Package upgrades and manual installs using the same prefix replace the
previous passwordless vendor rule at its existing path. If an older
installation used a different prefix, inspect any remaining duplicate
vendor rules. Do not remove unrelated administrator-managed rules under
`/etc/polkit-1`.

The `privileged-group` build option configures the vendor authentication
rule. Desktop keyboard and AccountsService language observation do not
bypass Polkit or write system defaults.

## Desktop input-source integration

Desktop integration is optional and selected at build time with
`-Ddesktop-backend`. The default is `none`, keeping the daemon independent
of desktop-specific APIs.

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
- Validates and logs per-user XKB layout, variant and options snapshots.

Switching the active GNOME layout or changing its input-source list does
not change system keyboard defaults. GNOME manages its own session layout.

The GNOME backend is compiled into `localed`; its session settings reader
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
These signals refresh the observed per-user snapshot only. Plasma manages
the session layout itself; neither the console keymap nor Xorg system
defaults are changed by backend observation.

System defaults are written only through authorized `SetVConsoleKeyboard`
or `SetX11Keyboard` calls, including their optional conversion. Repeated
byte-identical configuration requests skip writes.

As with GNOME, the Plasma-specific reader is compiled into the single
`localed-input-sources-helper` executable. No separate Plasma helper binary
or KDE development library is required; communication uses GIO/GDBus.

### Adding other desktop environments

The backend interface is intended to keep desktop-specific integration out
of the locale daemon's core. Future desktop integrations can add their
implementation behind the same interface and build-time selection. They
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
including `LANGUAGE`, are preserved. If the scripts already differ,
explicit requests take precedence, `lang.sh` wins conflicting values,
and assignments present only in `lang.csh` are preserved in both files.
The daemon reconciles them only on an authorized update, not on startup
or file-monitor events. It reads both snapshots and updates them
transactionally. This supports
simple literal locale assignments, not arbitrary shell programs.
The standard `SetLocale` D-Bus method retains its replacement semantics
for both scripts without injecting a C-shell-only `LC_COLLATE` default;
the bundled client uses the new `UpdateLocale` extension instead. Upgrade
the daemon and client together; older daemons do not expose `UpdateLocale`.
The Plasma locale-file monitor is no longer used. AccountsService per-user
language notifications also leave system locale settings unchanged.

## Keyboard conversion

`SetVConsoleKeyboard` and `SetX11Keyboard` both accept a `convert`
boolean. When `true`, a built-in mapping table translates the console
keymap to an X11 layout (or vice versa). The table covers common
single-layout keymaps (`us`, `de`, `fr`, `gb`, `es`, `ru`, `se`,
etc.). No conversion is attempted when no match is found, rather than
guessing.

## AccountsService observation

When `org.freedesktop.Accounts` is available on the system bus, localed
observes account `Language` property changes for diagnostic logging only.
The monitor follows service appearance, disappearance and bus reconnection.
It does not change system locale files or the locale1 system properties,
regardless of account UID or group membership. AccountsService and desktop
clients remain responsible for per-user language preferences.

System locale changes require explicit, Polkit-authorized
`SetLocale` or `UpdateLocale` calls. Property-change signals do not identify
the original caller and cannot authorize a system update.

## Adding a translation

1. Add the language code to `po/LINGUAS` (one code per line).
2. Generate an initial `.po` file:
   ```sh
   ninja -C build localed-pot
   msginit -l de -o po/de.po -i po/localed.pot
   ```
3. Translate the strings in `po/de.po`.
4. Rebuild; Meson compiles and installs `.mo` files automatically.
