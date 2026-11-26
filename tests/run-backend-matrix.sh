#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build and test all supported desktop configurations independently.
# Invoke with: sh tests/run-backend-matrix.sh
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
matrix_dir=$(mktemp -d "${TMPDIR:-/tmp}/localed-matrix.XXXXXX")
matrix_dir=$(CDPATH= cd -- "$matrix_dir" && pwd -P)
status=0

printf 'Matrix build and test logs: %s\n' "$matrix_dir"
printf 'Source directory: %s\n' "$source_dir"

for backend in none gnome plasma6 both; do
    build="$matrix_dir/$backend/build"
    config="$matrix_dir/$backend/config"
    mkdir -p "$config"
    printf '\n=== desktop-backend=%s ===\n' "$backend"

    # Never run tests against the installed Slackware configuration files.
    # Each variant gets an independent build and configuration directory.
    if meson setup "$build" "$source_dir" \
        -Dtests=true \
        "-Ddesktop-backend=$backend" \
        "-Dlang_sh=$config/lang.sh" \
        "-Drc_keymap=$config/rc.keymap" \
        "-Dx11_keyboard_conf=$config/00-keyboard.conf" &&
       meson compile -C "$build" &&
       meson test -C "$build" --num-processes 1 --print-errorlogs; then
        printf '%s: PASS\n' "$backend" >> "$matrix_dir/results.txt"
    else
        printf '%s: FAIL\n' "$backend" >> "$matrix_dir/results.txt"
        status=1
    fi
done

printf '\n=== Matrix results ===\n'
cat "$matrix_dir/results.txt"
printf '\nBuild directories and logs retained at %s\n' "$matrix_dir"
exit "$status"
