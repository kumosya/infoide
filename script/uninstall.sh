#!/bin/sh
set -eu
destdir=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --destdir) destdir=$(realpath -m -- "${2:?missing staging directory}"); shift 2;;
        --help) echo 'Usage: ./uninstall.sh [--destdir DIR]; removes /usr/local/bin/infoide and /usr/local/lib/osinfo/'; exit 0;;
        *) echo "unknown option: $1" >&2; exit 2;;
    esac
done
target="$destdir/usr/local"
# Do not traverse a staging prefix redirected outside the selected root.
for directory in "$destdir/usr" "$target" "$target/bin" "$target/lib"; do
    if [ -L "$directory" ]; then
        echo "refusing symlink installation prefix: $directory" >&2
        exit 2
    fi
done
rm -f -- "$target/bin/infoide"
rm -rf -- "$target/lib/osinfo"
printf 'Uninstalled %s/bin/infoide and %s/lib/osinfo/\n' "$target" "$target"
