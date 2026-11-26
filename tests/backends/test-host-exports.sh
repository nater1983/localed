#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Check the actual ELF dynamic symbol tables, not just the source map.
set -eu

nm_tool=$1
daemon=$2
shift 2

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export LC_ALL=C

"$nm_tool" -D --defined-only --format=posix "$daemon" > "$tmp/daemon"
awk '$1 ~ /^rcl_/ {print $1}' "$tmp/daemon" > "$tmp/host.unsorted"
sort -u "$tmp/host.unsorted" > "$tmp/host"
: > "$tmp/imports.unsorted"
printf '%s\n' rcl_backend_module_query > "$tmp/entry"

for module do
    "$nm_tool" -D --defined-only --format=posix "$module" > "$tmp/defined"
    awk '$1 ~ /^rcl_/ {print $1}' "$tmp/defined" > "$tmp/module.unsorted"
    sort -u "$tmp/module.unsorted" > "$tmp/module"
    diff -u "$tmp/entry" "$tmp/module"
    "$nm_tool" -D --undefined-only --format=posix "$module" > "$tmp/undefined"
    awk '$1 ~ /^rcl_/ {print $1}' "$tmp/undefined" >> "$tmp/imports.unsorted"
done

sort -u "$tmp/imports.unsorted" > "$tmp/imports"

# Fail on either missing imports or unintended project exports.
diff -u "$tmp/imports" "$tmp/host"

# The entry point belongs to each module, never the host executable.
# main is not a module API either.
if awk '$1 == "main" || $1 == "rcl_backend_module_query" {found=1}
        END {exit !found}' "$tmp/daemon"; then
    echo "Unexpected daemon entry point in the dynamic symbol table" >&2
    exit 1
fi
