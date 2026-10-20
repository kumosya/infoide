// Linux host integration. Included inside namespace infoide.
class Resources {
 public:
    static fs::path executable() { return fs::canonical("/proc/self/exe"); }
    static fs::path find(const std::string& name) {
        std::vector<fs::path> directories;
        if (const char* home = std::getenv("INFOIDE_HOME")) directories.emplace_back(home);
        directories.push_back(executable().parent_path().parent_path() / "lib/osinfo");
        directories.emplace_back("/usr/local/lib/osinfo");
        for (const auto& directory : directories) if (fs::exists(directory / name)) return fs::canonical(directory / name);
        throw std::runtime_error("InfoIDE resource missing: " + name + "; install the Linux distribution or set INFOIDE_HOME");
    }
};

class TemporaryDirectory {
    fs::path path_;
 public:
    TemporaryDirectory() {
        std::string pattern = (fs::temp_directory_path() / "infoide-XXXXXX").string();
        std::vector<char> bytes(pattern.begin(), pattern.end()); bytes.push_back(0);
        if (!mkdtemp(bytes.data())) throw std::runtime_error("cannot create workspace");
        path_ = bytes.data();
    }
    ~TemporaryDirectory() {
        // ISO extraction restores read-only directories; make our workspace
        // traversable and removable even after an interrupted host operation.
        std::error_code error;
        fs::recursive_directory_iterator item(path_, fs::directory_options::skip_permission_denied, error), end;
        while (!error && item != end) {
            if (!item->is_symlink(error) && item->is_directory(error)) fs::permissions(item->path(), fs::perms::owner_all, fs::perm_options::add, error);
            item.increment(error);
        }
        fs::remove_all(path_, error);
    }
    const fs::path& path() const { return path_; }
};

class Bundle {
 public:
    static void write(const fs::path& destination, const Arguments& inputs) {
        if (inputs.empty() || inputs.size() > 65535) throw std::runtime_error("invalid bundle member count");
        std::vector<unsigned char> output;
        auto integer = [&](uint32_t value, unsigned size) { for (unsigned i = 0; i < size; ++i) output.push_back(value >> (8 * i)); };
        struct Member { std::string name; uint32_t crc, size, offset; };
        std::vector<Member> members;
        std::set<std::string> names;
        for (const auto& input : inputs) {
            const auto name = fs::path(input).filename().string(); Files::relative(name);
            if (fs::path(name).extension() != ".inpk" || !names.insert(name).second) throw std::runtime_error("duplicate or invalid bundle member");
            PackageTools::run({"inpkg", "check", fs::absolute(input).string()});
            inpkg::Bytes data;
            if (!inpkg::hostRead(input.c_str(), data) || data.size() > 512 * 1024 * 1024 || output.size() + data.size() > 0xffff0000ULL) throw std::runtime_error("bundle too large");
            uint32_t crc = UINT32_MAX;
            for (auto byte : data) { crc ^= byte; for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1))); }
            Member member{name, crc ^ UINT32_MAX, static_cast<uint32_t>(data.size()), static_cast<uint32_t>(output.size())};
            integer(0x04034b50, 4); integer(20, 2); integer(0, 2); integer(0, 2); integer(0, 4);
            integer(member.crc, 4); integer(member.size, 4); integer(member.size, 4); integer(name.size(), 2); integer(0, 2);
            output.insert(output.end(), name.begin(), name.end()); output.insert(output.end(), data.begin(), data.end()); members.push_back(member);
        }
        const uint32_t directory = output.size();
        for (const auto& member : members) {
            integer(0x02014b50, 4); integer(0x0314, 2); integer(20, 2); integer(0, 2); integer(0, 2); integer(0, 4);
            integer(member.crc, 4); integer(member.size, 4); integer(member.size, 4); integer(member.name.size(), 2);
            integer(0, 2); integer(0, 2); integer(0, 2); integer(0, 2); integer(0100644U << 16, 4); integer(member.offset, 4);
            output.insert(output.end(), member.name.begin(), member.name.end());
        }
        const uint32_t size = output.size() - directory;
        integer(0x06054b50, 4); integer(0, 2); integer(0, 2); integer(members.size(), 2); integer(members.size(), 2); integer(size, 4); integer(directory, 4); integer(0, 2);
        const auto target = fs::absolute(destination); TemporaryFile temporary(target);
        std::ofstream stream(temporary.path(), std::ios::binary); stream.write(reinterpret_cast<const char*>(output.data()), output.size()); stream.close();
        if (!stream) throw std::runtime_error("cannot write bundle");
        temporary.publish(target);
    }
};

class FilesystemImage {
    using Inventory = std::map<std::string, std::string>;
    static Inventory inventory(const fs::path& root) {
        Inventory result;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_symlink()) throw std::runtime_error("linked image payload");
            if (!entry.is_regular_file()) continue;
            inpkg::Bytes bytes;
            if (!inpkg::hostRead(entry.path().c_str(), bytes)) throw std::runtime_error("cannot read image payload");
            result[entry.path().lexically_relative(root).generic_string()] = inpkg::digest(bytes) + ":" + std::to_string(static_cast<unsigned>(entry.status().permissions()) & 0777);
        }
        return result;
    }
    static std::string quote(const fs::path& path) {
        std::string output = "\"";
        for (unsigned char c : path.string()) {
            if (c < 32 || c == 127) throw std::runtime_error("control byte in image path");
            if (c == '\\' || c == '"') output += '\\';
            output += c;
        }
        return output + '"';
    }
 public:
    static void install(const fs::path& image, const fs::path& archive, const fs::path& stage) {
        if (!Process::available("debugfs")) throw std::runtime_error("missing host tool: debugfs");
        const auto root = stage / "root";
        fs::create_directories(root);
        fs::permissions(image, fs::perms::owner_write, fs::perm_options::add);
        Process::run({"debugfs", "-R", "rdump / " + quote(root), image.string()}, stage, stage / "extract.log");
        if (!fs::is_regular_file(root / "etc/fstab") || !fs::is_regular_file(root / "lib/version.1")) throw std::runtime_error("invalid OSInfo root filesystem");
        const auto before = inventory(root);
        std::set<std::string> directories;
        for (const auto& entry : fs::recursive_directory_iterator(root)) if (entry.is_directory()) directories.insert(entry.path().lexically_relative(root).generic_string());
        PackageTools::run({"inpkg", "upgrade", fs::canonical(archive).string(), "--install-missing", "--sysroot", root.string(), "-y"});
        const auto after = inventory(root);
        std::ostringstream batch;
        for (const auto& entry : fs::recursive_directory_iterator(root)) if (entry.is_directory()) {
            const auto name = entry.path().lexically_relative(root).generic_string();
            if (!directories.count(name)) batch << "mkdir " << quote("/" + name) << '\n';
        }
        for (const auto& [name, hash] : before) if (!after.count(name) || after.at(name) != hash) batch << "rm " << quote("/" + name) << '\n';
        for (const auto& [name, hash] : after) if (!before.count(name) || before.at(name) != hash) {
            const auto target = quote("/" + name);
            batch << "write " << quote(root / name) << ' ' << target << '\n';
            batch << "set_inode_field " << target << " mode 0" << std::oct << (0100000 | (static_cast<unsigned>(fs::status(root / name).permissions()) & 0777)) << std::dec << '\n';
            batch << "set_inode_field " << target << " uid 0\nset_inode_field " << target << " gid 0\n";
        }
        Files::write(stage / "install.debugfs", batch.str());
        Process::run({"debugfs", "-w", "-f", (stage / "install.debugfs").string(), image.string()}, stage, stage / "install.log");
        const auto verify = stage / "verify"; fs::create_directory(verify);
        Process::run({"debugfs", "-R", "rdump / " + quote(verify), image.string()}, stage, stage / "verify.log");
        const auto actual = inventory(verify);
        if (actual != after) {
            std::cerr << Files::read(stage / "install.log");
            for (const auto& [name, fingerprint] : after) if (!actual.count(name) || actual.at(name) != fingerprint)
                std::cerr << "image mismatch: " << name << " expected=" << fingerprint << " actual=" << (actual.count(name) ? actual.at(name) : "missing") << '\n';
            for (const auto& [name, fingerprint] : actual) if (!after.count(name)) std::cerr << "image unexpected file: " << name << '\n';
            throw std::runtime_error("image package verification failed; output was not published");
        }
    }
};

class OpticalImage {
 public:
    static void install(const fs::path& source, const fs::path& output, const fs::path& archive) {
        for (const auto* tool : {"xorriso", "grub-mkrescue"}) if (!Process::available(tool)) throw std::runtime_error(std::string("missing host tool: ") + tool);
        if (fs::weakly_canonical(source) == fs::weakly_canonical(output)) throw std::runtime_error("output ISO must differ from base ISO");
        TemporaryDirectory temporary;
        const auto stage = temporary.path(), cd = stage / "cd", image = cd / "boot/rootfs.ext3";
        Process::run({"xorriso", "-osirrox", "on", "-indev", fs::canonical(source).string(), "-extract", "/", cd.string()}, stage);
        if (!fs::is_regular_file(image)) throw std::runtime_error("base ISO has no /boot/rootfs.ext3");
        FilesystemImage::install(image, archive, stage);
        TemporaryFile result(fs::absolute(output));
        Process::run({"grub-mkrescue", "-o", result.path().string(), cd.string()}, stage);
        result.publish(fs::absolute(output));
        std::cout << "Installed application into CD: " << output << '\n';
    }
};

#include "disk.h"
#include "debug.h"

#include "distribution.h"
