class Distribution {
 public:
    static void create(const Options& options) {
        if (options.sdk.empty() || options.disk.empty() || options.output.empty()) throw std::runtime_error("distribution requires --sdk, --disk and --output; --iso is optional");
        const auto destination = fs::absolute(options.output), sdk = fs::canonical(options.sdk), disk = fs::canonical(options.disk);
        if (fs::exists(destination)) throw std::runtime_error("distribution output already exists");
        if (Files::inside(destination, sdk) || !fs::is_regular_file(sdk / "sdk.json")) throw std::runtime_error("invalid distribution SDK/output");
        DiskImage::rootPartition(disk);
        fs::path iso;
        if (!options.iso.empty()) { iso = fs::canonical(options.iso); if (!fs::is_regular_file(iso)) throw std::runtime_error("missing optional ISO"); }
        const auto resources = destination / "lib/osinfo";
        fs::create_directories(destination / "bin"); fs::create_directories(resources);
        fs::copy_file(Resources::executable(), destination / "bin/infoide");
        const auto packageTool = PackageTools::executable();
        if (!Process::available(packageTool)) throw std::runtime_error("distribution requires inpkg; set INPKG=/path/to/inpkg");
        fs::path packagePath = packageTool;
        if (!packagePath.has_parent_path()) {
            const char* search = std::getenv("PATH");
            std::stringstream paths(search ? search : "");
            std::string directory;
            while (std::getline(paths, directory, ':')) {
                const auto candidate = fs::path(directory.empty() ? "." : directory) / packageTool;
                if (access(candidate.c_str(), X_OK) == 0) { packagePath = candidate; break; }
            }
        }
        fs::copy_file(fs::canonical(packagePath), resources / "inpkg");
        fs::copy(sdk, resources / "sdk", fs::copy_options::recursive);
        fs::copy_file(disk, resources / "osinfo.img");
        if (!iso.empty()) fs::copy_file(iso, resources / "osinfo.iso");
        Files::write(resources / "run-qemu.sh", R"SH(#!/bin/sh
set -eu
# This convenience launcher uses an existing project image; InfoIDE builds and
# installs the project before invoking QEMU with its own debug configuration.
boot=disk
image=
display=gtk
debug=0
port=1234
while [ "$#" -gt 0 ]; do
    case "$1" in
        --image) image=${2:?missing project image}; shift 2;;
        --boot) boot=${2:?missing mode}; shift 2;;
        --headless) display=none; shift;;
        --gdb) debug=1; shift;;
        --gdb-port) port=${2:?missing port}; shift 2;;
        *) echo "unknown option: $1" >&2; exit 2;;
    esac
done
case "$boot" in disk|cdrom) ;; *) echo 'boot must be disk or cdrom' >&2; exit 2;; esac
if [ -z "$image" ]; then
    if [ "$boot" = disk ]; then image="$PWD/osinfo.img"; else image="$PWD/osinfo.iso"; fi
fi
case "$port" in ''|*[!0-9]*) echo 'invalid GDB port' >&2; exit 2;; esac
test "$port" -ge 1 && test "$port" -le 65535
test -f "$image" || { echo 'project image missing; run infoide run first' >&2; exit 1; }
home=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
image=$(realpath "$image")
case "$image" in "$home"/*) echo 'refusing to start an installed template directly' >&2; exit 2;; esac
exec 9>"$image.lock"
flock -n 9 || { echo 'project image is in use' >&2; exit 1; }
set -- "${QEMU:-qemu-system-x86_64}" -m 512 -smp 4 -display "$display" -serial stdio -no-reboot
if [ "$boot" = disk ]; then
    escaped=$(printf '%s' "$image" | sed 's/,/,,/g')
    set -- "$@" -boot order=c,strict=on -drive "file=$escaped,if=none,format=raw,id=osdisk" -device virtio-blk-pci,drive=osdisk,disable-legacy=on,bootindex=0
else set -- "$@" -boot order=d,strict=on -cdrom "$image"; fi
if [ "$debug" = 1 ]; then set -- "$@" -S -gdb "tcp:127.0.0.1:$port"; fi
exec "$@"
)SH");
        Files::write(destination / "uninstall.sh",
#include "uninstall-script.h"
        );
        Files::write(destination / "install.sh", R"SH(#!/bin/sh
set -eu
source=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
destdir=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --destdir) destdir=$(realpath -m "${2:?missing staging directory}"); shift 2;;
        --help) echo 'Usage: ./install.sh [--destdir DIR]; installs /usr/local/bin/infoide and /usr/local/lib/osinfo/'; exit 0;;
        *) echo "unknown option: $1" >&2; exit 2;;
    esac
done
for tool in g++ gcc debugfs flock qemu-system-x86_64; do
    command -v "$tool" >/dev/null 2>&1 || { echo "missing host dependency: $tool" >&2; exit 1; }
done
target="$destdir/usr/local"
resources="$target/lib/osinfo"
install -d "$target/bin" "$resources/sdk"
install -m755 "$source/bin/infoide" "$target/bin/infoide"
install -m755 "$source/lib/osinfo/inpkg" "$resources/inpkg"
cp -R "$source/lib/osinfo/sdk/." "$resources/sdk/"
install -m644 "$source/lib/osinfo/osinfo.img" "$resources/osinfo.img"
install -m755 "$source/lib/osinfo/run-qemu.sh" "$resources/run-qemu.sh"
install -m755 "$source/uninstall.sh" "$resources/uninstall.sh"
if [ -f "$source/lib/osinfo/osinfo.iso" ]; then install -m644 "$source/lib/osinfo/osinfo.iso" "$resources/osinfo.iso"; fi
printf 'Installed %s/bin/infoide and %s/lib/osinfo/\n' "$target" "$target"
)SH");
        for (const auto& path : {destination / "bin/infoide", destination / "install.sh", destination / "uninstall.sh", resources / "run-qemu.sh", resources / "inpkg"}) fs::permissions(path, static_cast<fs::perms>(0755));
        Process::run({"tar", "-czf", destination.string() + ".tar.gz", "-C", destination.parent_path().string(), destination.filename().string()}, fs::current_path());
        std::cout << "Linux installer: " << destination.string() + ".tar.gz" << '\n';
    }
};
