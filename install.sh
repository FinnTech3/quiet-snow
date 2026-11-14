#!/bin/sh
# Installs Quiet Snow into ~/Applications and starts it. No admin password, nothing else touched.
# Uninstall: pkill -x QuietSnow; rm -rf ~/Applications/QuietSnow.app
set -eu
[ "$(uname -s)" = Darwin ] || { echo "Quiet Snow is for macOS." >&2; exit 1; }
dest="$HOME/Applications"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$dest"
curl -fsSL "https://github.com/FinnTech3/quiet-snow/releases/latest/download/QuietSnow.zip" -o "$tmp/QuietSnow.zip"
pkill -x QuietSnow 2>/dev/null || true
rm -rf "$dest/QuietSnow.app"
ditto -x -k "$tmp/QuietSnow.zip" "$dest"
open "$dest/QuietSnow.app"
echo "Quiet Snow is running. Look for the snowflake in your menu bar."
