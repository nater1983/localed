# localed

A lightweight implementation of the `org.freedesktop.locale1` D-Bus service
(`localed`), following the same daemon pattern as `hostnamed`.

## Dependencies

| Library | pkg-config name | Minimum version |
|---|---|---|
| GLib / GIO | `glib-2.0`, `gio-2.0`, `gio-unix-2.0` | 2.56 |
| polkit | `polkit-gobject-1` | 0.113 |
| D-Bus | `dbus-1` | any recent |

The daemon is activated by dbus-daemon directly via D-Bus service activation
(`[D-BUS Service]`). There is no systemd dependency.

On Debian/Ubuntu:
```sh
apt install libglib2.0-dev libpolkit-gobject-1-dev libdbus-1-dev \
            meson ninja-build pkg-config
```

On Fedora/RHEL:
```sh
dnf install glib2-devel polkit-devel dbus-devel \
            meson ninja-build pkg-config
```

## Building

```sh
meson setup build
ninja -C build
```

Common configure options:

```sh
# Install under /usr instead of /usr/local
meson setup build --prefix=/usr

# Disable polkit (for testing only – not for production)
meson setup build -Dpolkit=disabled

# Override the D-Bus service directory explicitly
meson setup build -Ddbus_system_service_dir=/usr/share/dbus-1/system-services

# Override locale file paths (Slackware defaults shown)
meson setup build \
    -Drc_keymap=/etc/rc.d/rc.keymap \
    -Dlang_sh=/etc/profile.d/lang.sh \
    -Dprivileged-group=wheel \
    -Dx11_keyboard_conf=/etc/X11/xorg.conf.d/00-keyboard.conf
```

## Installing

```sh
ninja -C build install          # installs to prefix (default /usr/local)
sudo ninja -C build install     # for system-wide install
```

## Project layout

```
.
├── meson.build             # top-level build definition
├── meson_options.txt       # user-settable options (-D flags)
├── src/
│   ├── meson.build         # compiles the localed binary
│   ├── main.c              # GMainLoop, D-Bus name ownership, inotify watches
│   ├── rcl-locale.h        # public interface: types, macros, path constants
│   └── rcl-locale.c        # D-Bus skeleton, property sync, polkit, file I/O
├── data/
│   ├── meson.build                                  # installs D-Bus and polkit data files
│   ├── org.freedesktop.locale1.service.in           # D-Bus activation
│   ├── org.freedesktop.locale1.conf                 # D-Bus security policy
│   ├── org.freedesktop.locale1.policy               # polkit actions
│   ├── org.freedesktop.locale1.rules.in             # polkit rules template
│   └── org.freedesktop.locale1.xml                  # introspection XML reference
└── po/
    ├── meson.build         # gettext / i18n integration
    ├── POTFILES            # source files with translatable strings
    └── LINGUAS             # enabled translation languages
```

## D-Bus interface quick reference

```sh
# Read all properties
busctl get-property org.freedesktop.locale1 \
       /org/freedesktop/locale1 org.freedesktop.locale1 Locale

# Set the system locale (triggers polkit prompt)
busctl call org.freedesktop.locale1 \
       /org/freedesktop/locale1 org.freedesktop.locale1 \
       SetLocale asb 1 LANG=en_US.UTF-8 true

# Set the X11 keyboard layout
busctl call org.freedesktop.locale1 \
       /org/freedesktop/locale1 org.freedesktop.locale1 \
       SetX11Keyboard ssssbb us pc105 "" "" false true

# Set the console keymap and convert to X11
busctl call org.freedesktop.locale1 \
       /org/freedesktop/locale1 org.freedesktop.locale1 \
       SetVConsoleKeyboard ssbb us "" true true
```

## Files managed

| File | Property |
|---|---|
| `/etc/profile.d/lang.sh` | `Locale` (array of KEY=value) |
| `/etc/rc.d/rc.keymap` | `VConsoleKeymap`, `VConsoleKeymapToggle` |
| `/etc/X11/xorg.conf.d/00-keyboard.conf` | `X11Layout`, `X11Model`, `X11Variant`, `X11Options` |

All three files are watched with inotify; `localed` re-reads them and emits
`PropertiesChanged` automatically whenever they are modified by any tool.

## Keyboard conversion

`SetVConsoleKeyboard` and `SetX11Keyboard` both accept a `convert` boolean.
When `true`, a built-in mapping table translates the console keymap to an
X11 layout (or vice versa). The table covers common single-layout keymaps
(`us`, `de`, `fr`, `gb`, `es`, `ru`, `se`, etc.). No conversion is attempted
when no match is found, rather than guessing.

## Adding a translation

1. Add the language code to `po/LINGUAS` (one code per line).
2. Generate an initial `.po` file:
   ```sh
   ninja -C build localed-pot
   msginit -l de -o po/de.po -i po/localed.pot
   ```
3. Translate the strings in `po/de.po`.
4. Rebuild – Meson compiles and installs `.mo` files automatically.
