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

## System locale updates

The bundled `localectl` uses standard `SetLocale` for ordinary locale
updates and merges unspecified variables from existing configuration:

```sh
localectl set-locale LANG=en_US.UTF-8
localectl set-locale LC_COLLATE=C
localectl set-locale LC_COLLATE=
localectl unset-locale LC_COLLATE
```

The final two commands are equivalent. Standard `SetLocale` follows the
systemd v262 source: merge existing values, derive LANGUAGE when applicable,
and remove redundant assignments equal to LANG. Empty values and duplicate
keys are rejected by that standard method. A single bare locale is supported.

The client routes requests containing empty assignments or explicit LANGUAGE
preferences through the explicitly nonstandard `UpdateLocale` extension, which
preserves unspecified assignments and can remove named keys from both
scripts. The entire request uses one method, not multiple calls.
Both methods read snapshots after Polkit authorization and update both
scripts transactionally without inventing C-shell-only LC_COLLATE defaults.
Upgrade the daemon and bundled client together. Requested locale names are
checked against installed glibc locales using `newlocale()` without changing
the daemon's process locale. C and POSIX are always accepted. The
`UpdateLocale` LANGUAGE extension validates colon-separated translation
preferences syntactically rather than requiring each language to be an
installed glibc locale. Explicit removals and existing unspecified settings
are preserved. Missing locales are rejected; localed does not generate
locales automatically.

## Keyboard validation and conversion

Nonempty console keymap and toggle names must identify installed readable
`.map` or `.map.gz` files under `/usr/share/keymaps`, `/usr/share/kbd/keymaps`
or `/usr/lib/kbd/keymaps`. The daemon searches subdirectories and supports
file aliases but does not follow symlink directories. Invalid requests fail
before Polkit and are checked again after authorization. Empty values retain
reset semantics. This verifies file availability, not map contents; no
keymap is loaded during validation.


`SetVConsoleKeyboard` and `SetX11Keyboard` both accept a `convert`
boolean. When `true`, a built-in mapping table translates the console
keymap to an X11 layout (or vice versa). The table covers common
single-layout keymaps (`us`, `de`, `fr`, `gb`, `es`, `ru`, `se`,
etc.). No conversion is attempted when no match is found or the derived
console keymap is unavailable, rather than persisting an unusable keymap.

## Adding a translation

1. Add the language code to `po/LINGUAS` (one code per line).
2. Generate an initial `.po` file:
   ```sh
   ninja -C build localed-pot
   msginit -l de -o po/de.po -i po/localed.pot
   ```
3. Translate the strings in `po/de.po`.
4. Rebuild; Meson compiles and installs `.mo` files automatically.
