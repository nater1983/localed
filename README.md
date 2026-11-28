# localed-desktop

Optional desktop locale and keyboard integration extracted from localed 0.4.0.
This is a separate project, not a replacement for systemd-localed or localed.
It never owns org.freedesktop.locale1, installs no D-Bus activation or
Polkit policy, and does not write system configuration files itself.

## Policy and scope

The default is observation only. An administrator may explicitly launch it
with `--sync-system-defaults` to opt into the historical automatic
desktop-to-system synchronization policy. This is not standard locale1
behavior. It can affect other users and consoles; do not enable it unless
that is the desired system policy. There is no automatic service startup.

Keyboard synchronization uses authorized locale1 SetX11Keyboard and
SetVConsoleKeyboard calls with conversion disabled. Language synchronization
uses localed's nonstandard UpdateLocale extension to preserve other locale
assignments; a service without this extension will reject that update.
There is intentionally no fallback to SetLocale replacement behavior.
AccountsService monitoring retains the privileged-group and unique active
local account checks. Keyboard monitoring retains the original graphical
session selection and helper privilege drop. No changes are requested from
locale1 in observation mode.

## Build

Requires GLib/GIO >= 2.56, Meson, Ninja, and a C compiler. At runtime,
session monitoring requires elogind/logind, the selected desktop and
setpriv (util-linux) for running the helper as the verified session user.

    meson setup build -Ddesktop-backend=gnome -Dtests=true
    meson compile -C build
    meson test -C build --print-errorlogs
    meson install -C build

Use plasma6 instead of gnome for Plasma 6, or none for AccountsService only.
The selected backend is compiled in; simultaneous desktop backends and
GModule loading are not part of this initial extraction.

After installation, run the installed localed-desktop binary as root for
observation (the helper is launched with the selected session user's UID).
For explicit opt-in synchronization, pass --sync-system-defaults.
Do not run the legacy integrated daemon and the companion with automatic
synchronization enabled simultaneously.

## Provenance

Extracted from nater1983/localed at 6c941f6560f9224f14c00663919dd7bd2cf6a691.
Original authorship and GPL licensing are retained. The standalone adapter,
build and tests require independent validation before release.
