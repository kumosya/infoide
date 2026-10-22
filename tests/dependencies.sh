#!/bin/sh
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cli=$(realpath "${1:-build/infoide}")
sdk=$(realpath "${OSINFO_SDK:-resources/sdk}")
disk=$(realpath "${OSINFO_DISK:-resources/osinfo.img}")
iso=${OSINFO_ISO:-}
if [ -n "$iso" ]; then iso=$(realpath "$iso"); fi
inpkg=${INPKG:-inpkg}
stage=$(mktemp -d "${TMPDIR:-/tmp}/infoide-dependencies.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT HUP INT TERM
mkdir "$stage/projects with spaces"
project="$stage/projects with spaces/probe"
"$cli" create "$project"
"$cli" create "$stage/support-helper"
"$cli" pkg --project "$stage/support-helper" --sdk "$sdk"
mkdir -p "$project/deps" "$project/config" "$stage/payload"
pack_data() {
    printf '%s\n' "$1" > "$stage/payload/data"
    printf 'name=support-data\nversion=0.1.0\nfile=/usr/share/infoide-dependency/data\ninstall=data -> /usr/share/infoide-dependency/data\n' > "$stage/payload/package.conf"
    "$inpkg" pack "$stage/payload" --manifest "$stage/payload/package.conf" --output "$project/deps/support-data.inpk"
}
pack_data first
cat > "$project/qemu-check.sh" <<'SH'
#!/bin/sh
printf '%s\n' "$@" > qemu-invoked
SH
chmod +x "$project/qemu-check.sh"
cat > "$project/config/test-debug.json" <<JSON
{
  "dependencies": ["deps/support-data.inpk", "$stage/support-helper/build/packages/support-helper.inpk"],
  "qemu": "./qemu-check.sh",
  "display": "none"
}
JSON
config=config/test-debug.json
install_project() { "$cli" "$@" --project "$project" --sdk "$sdk" --debug-config "$config"; }
template_hash=$(sha256sum "$disk")
install_project install --disk "$disk"
# Read the actual partition offset and size, independent of the template geometry.
extract_disk() {
    offset=$(od -An -tu4 -j454 -N4 "$project/osinfo.img" | tr -d ' ')
    size=$(od -An -tu4 -j458 -N4 "$project/osinfo.img" | tr -d ' ')
    dd if="$project/osinfo.img" of="$stage/root.ext3" bs=1M iflag=skip_bytes,count_bytes \
        skip="$((offset * 512))" count="$((size * 512))" status=none
}
assert_files() {
    debugfs -R 'cat /usr/share/infoide-dependency/data' "$stage/root.ext3" 2>/dev/null | grep -qx "$1"
    for name in support-helper probe; do
        debugfs -R "stat /usr/bin/$name" "$stage/root.ext3" > "$stage/stat.log" 2>&1
        grep -q 'Type: regular' "$stage/stat.log"
    done
}
extract_disk
assert_files first
pack_data second
install_project run
test -f "$project/qemu-invoked"
extract_disk
assert_files second
rm "$project/qemu-invoked"
install_project debug
grep -qx -- -gdb "$project/qemu-invoked"
grep -qx -- -S "$project/qemu-invoked"
rm "$project/qemu-invoked"
old=$(sha256sum "$project/osinfo.img")
# A later invalid package must not publish changes from an earlier valid upgrade.
pack_data third
cp "$stage/support-helper/build/packages/support-helper.inpk" "$stage/helper-good.inpk"
printf 'corrupt\n' > "$stage/support-helper/build/packages/support-helper.inpk"
if install_project run > "$stage/failure.log" 2>&1; then echo 'accepted corrupted dependency' >&2; exit 1; fi
test "$old" = "$(sha256sum "$project/osinfo.img")"
test ! -e "$project/qemu-invoked"
cp "$stage/helper-good.inpk" "$stage/support-helper/build/packages/support-helper.inpk"
for value in '"bad"' '[17]' '["missing.inpk"]' '["deps/support-data.inpk", "deps/support-data.inpk"]'; do
    printf '{"dependencies": %s}\n' "$value" > "$project/config/invalid.json"
    if "$cli" install --project "$project" --sdk "$sdk" --debug-config config/invalid.json > "$stage/failure.log" 2>&1; then
        echo "accepted invalid dependencies: $value" >&2; exit 1
    fi
    test "$old" = "$(sha256sum "$project/osinfo.img")"
done
test "$template_hash" = "$(sha256sum "$disk")"
if [ -n "$iso" ]; then
    iso_hash=$(sha256sum "$iso")
    install_project install --boot cdrom --iso "$iso"
    mkdir "$stage/optical"
    xorriso -osirrox on -indev "$project/osinfo.iso" -extract /boot/rootfs.ext3 "$stage/optical/root.ext3" > "$stage/optical.log" 2>&1
    cp "$stage/optical/root.ext3" "$stage/root.ext3"
    assert_files third
    old_iso=$(sha256sum "$project/osinfo.iso")
    printf 'corrupt\n' > "$stage/support-helper/build/packages/support-helper.inpk"
    if install_project install --boot cdrom --iso "$iso" > "$stage/failure.log" 2>&1; then echo 'accepted corrupted ISO dependency' >&2; exit 1; fi
    test "$old_iso" = "$(sha256sum "$project/osinfo.iso")"
    test "$iso_hash" = "$(sha256sum "$iso")"
fi
echo 'Dependency package installation, upgrade, config validation, disk/ISO rollback and run/debug checks passed'
