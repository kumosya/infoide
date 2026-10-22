#include "json.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glob.h>
#include <fcntl.h>
#include <iostream>
#include <set>
#include <sstream>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
namespace infoide {
using Arguments = std::vector<std::string>;

class Files {
 public:
    static std::string read(const fs::path& path) {
        if (!fs::is_regular_file(path) || fs::file_size(path) > 4 * 1024 * 1024)
            throw std::runtime_error("missing or oversized configuration: " + path.string());
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot read " + path.string());
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    static std::vector<unsigned char> readBytes(const fs::path& path) {
        FILE* input = std::fopen(path.c_str(), "rb");
        if (!input) throw std::runtime_error("cannot read " + path.string());
        std::vector<unsigned char> output;
        unsigned char buffer[8192];
        size_t size;
        while ((size = std::fread(buffer, 1, sizeof(buffer), input))) {
            if (output.size() + size > 128 * 1024 * 1024) {
                std::fclose(input);
                throw std::runtime_error("file exceeds limit: " + path.string());
            }
            output.insert(output.end(), buffer, buffer + size);
        }
        const bool ok = !std::ferror(input);
        std::fclose(input);
        if (!ok) throw std::runtime_error("cannot read " + path.string());
        return output;
    }
    static std::string digest(const std::vector<unsigned char>& input) {
        static const uint32_t constants[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
            0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
            0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
            0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };
        uint32_t hash[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        const auto rotate = [](uint32_t value, unsigned bits) {
            return (value >> bits) | (value << (32 - bits));
        };
        const size_t blocks = (input.size() + 9 + 63) / 64;
        for (size_t block = 0; block < blocks; ++block) {
            uint32_t words[64]{};
            for (unsigned i = 0; i < 64; ++i) {
                const size_t offset = block * 64 + i;
                unsigned char value = 0;
                if (offset < input.size()) value = input[offset];
                else if (offset == input.size()) value = 128;
                if (offset >= blocks * 64 - 8)
                    value = static_cast<unsigned char>(
                        (static_cast<uint64_t>(input.size()) * 8) >>
                        ((blocks * 64 - 1 - offset) * 8));
                words[i / 4] |= static_cast<uint32_t>(value) << ((3 - i % 4) * 8);
            }
            for (unsigned i = 16; i < 64; ++i)
                words[i] = words[i - 16] +
                    (rotate(words[i - 15], 7) ^ rotate(words[i - 15], 18) ^ (words[i - 15] >> 3)) +
                    words[i - 7] +
                    (rotate(words[i - 2], 17) ^ rotate(words[i - 2], 19) ^ (words[i - 2] >> 10));
            uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3];
            uint32_t e = hash[4], f = hash[5], g = hash[6], h = hash[7];
            for (unsigned i = 0; i < 64; ++i) {
                const uint32_t first = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) +
                    ((e & f) ^ (~e & g)) + constants[i] + words[i];
                const uint32_t second = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) +
                    ((a & b) ^ (a & c) ^ (b & c));
                h = g; g = f; f = e; e = d + first;
                d = c; c = b; b = a; a = first + second;
            }
            hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d;
            hash[4] += e; hash[5] += f; hash[6] += g; hash[7] += h;
        }
        char result[65];
        for (unsigned i = 0; i < 8; ++i) std::snprintf(result + i * 8, 9, "%08x", hash[i]);
        return result;
    }
    static void write(const fs::path& path, const std::string& text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
        output.close();
        if (!output) throw std::runtime_error("cannot write " + path.string());
    }
    static bool inside(const fs::path& path, const fs::path& root) {
        auto relative = fs::weakly_canonical(path).lexically_relative(root);
        return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
    }
    static void relative(const std::string& text) {
        fs::path path(text);
        if (text.empty() || path.is_absolute() || text.find('\\') != std::string::npos)
            throw std::runtime_error("expected a relative path: " + text);
        for (const auto& part : path) if (part == ".." || part == "." || part.empty()) throw std::runtime_error("invalid path: " + text);
        for (unsigned char c : text) if (c < 32 || c == 127) throw std::runtime_error("control byte in path");
    }
    static std::string identifier(const std::string& text) {
        if (text.empty() || text.size() > 63 || text == "." || text == ".." ||
            !std::all_of(text.begin(), text.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_' || c == '+' || c == '.' || c == '-';
            })) throw std::runtime_error("invalid identifier: " + text);
        return text;
    }
};

class TemporaryFile {
    fs::path path_;
 public:
    explicit TemporaryFile(const fs::path& destination) {
        fs::create_directories(destination.parent_path());
        std::string pattern = destination.string() + ".tmp.XXXXXX";
        std::vector<char> bytes(pattern.begin(), pattern.end()); bytes.push_back(0);
        int descriptor = mkstemp(bytes.data());
        if (descriptor < 0) throw std::runtime_error("cannot create temporary file: " + std::string(std::strerror(errno)));
        close(descriptor); path_ = bytes.data();
    }
    ~TemporaryFile() { std::error_code ignored; fs::remove(path_, ignored); }
    const fs::path& path() const { return path_; }
    void publish(const fs::path& destination) { fs::rename(path_, destination); }
};

class Process {
 public:
    static bool available(const std::string& command) {
        if (command.find('/') != std::string::npos) return access(command.c_str(), X_OK) == 0;
        const char* environment = std::getenv("PATH");
        std::istringstream paths(environment ? environment : ""); std::string path;
        while (std::getline(paths, path, ':')) if (access((fs::path(path.empty() ? "." : path) / command).c_str(), X_OK) == 0) return true;
        return false;
    }
    static void run(const Arguments& arguments, const fs::path& directory, const fs::path& diagnostics = {}) {
        std::cout << "[RUN]";
        for (const auto& argument : arguments) {
            if (argument.find('\0') != std::string::npos) throw std::runtime_error("NUL byte in process argument");
            std::cout << " [" << argument << ']';
        }
        std::cout << std::endl;
        std::vector<char*> argv;
        for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        pid_t child = fork();
        if (child < 0) throw std::runtime_error("fork failed");
        if (!child) {
            if (chdir(directory.c_str()) != 0) { perror("chdir"); _exit(126); }
            if (!diagnostics.empty()) {
                const int output = open(diagnostics.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (output < 0 || dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0) _exit(126);
                close(output);
            }
            unsetenv("TAR_OPTIONS");
            execvp(argv[0], argv.data()); perror(argv[0]); _exit(127);
        }
        int status = 0;
        while (waitpid(child, &status, 0) < 0) if (errno != EINTR) throw std::runtime_error("waitpid failed");
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            if (!diagnostics.empty() && fs::is_regular_file(diagnostics)) std::cerr << Files::read(diagnostics);
            throw std::runtime_error(arguments[0] + " failed (" + (WIFEXITED(status) ? "exit " + std::to_string(WEXITSTATUS(status)) : "signal " + std::to_string(WTERMSIG(status))) + ")");
        }
    }
};

class PackageTools {
 public:
    static void run(const Arguments& arguments) {
        const char* configured = std::getenv("INPKG");
        const std::string executable = configured && *configured ? configured : "inpkg";
        if (!Process::available(executable))
            throw std::runtime_error("missing host dependency: inpkg; install it or set INPKG=/path/to/inpkg");
        Arguments command = arguments;
        command.at(0) = executable;
        Process::run(command, fs::current_path());
    }
};

struct Options {
    fs::path project = fs::current_path();
    std::string sdk, cxx, cc;
    std::string iso, disk, boot, debugConfig, qemu, output;
    bool headless = false;
};

#include "linux.h"

class Project {
    fs::path root_, sdk_, output_, payload_;
    Json config_;
    std::string name_, cxx_, cc_;
    std::map<std::string, std::string> kinds_;
    Arguments targets_;

    static void append(Arguments& to, const Arguments& from) { to.insert(to.end(), from.begin(), from.end()); }
    fs::path relative(const std::string& text) const {
        Files::relative(text);
        fs::path result = root_ / text;
        if (!Files::inside(result, root_)) throw std::runtime_error("path escapes project: " + text);
        return result;
    }
    fs::path outputPath(const fs::path& relativePath) const {
        fs::path result = output_ / relativePath;
        if (!Files::inside(result, root_)) throw std::runtime_error("build path escapes project: " + result.string());
        return result;
    }
    fs::path configuredPath(const std::string& text) const {
        if (text.empty() || text.find('\0') != std::string::npos) throw std::runtime_error("empty or invalid configured path");
        return fs::weakly_canonical(fs::path(text).is_absolute() ? fs::path(text) : root_ / text);
    }
    std::string compiler(const std::string& requested, const std::string& key, const std::string& fallback) const {
        std::string result = requested.empty() ? config_.text(key, fallback) : requested;
        if (result.find('/') != std::string::npos) result = configuredPath(result).string();
        if (!Process::available(result)) throw std::runtime_error(key + ": compiler not executable: " + result);
        return result;
    }
    Arguments paths(const Json& target, const std::string& field, const std::string& flag) const {
        Arguments result;
        for (const auto* object : {&config_, &target}) for (const auto& value : object->strings(field)) {
            result.push_back(flag); result.push_back(configuredPath(value).string());
        }
        return result;
    }
    Arguments sources(const Json& target) const {
        std::set<std::string> matches;
        for (const auto& pattern : target.strings("sources")) {
            fs::path full = relative(pattern);
            glob_t found{};
            int status = glob(full.c_str(), GLOB_NOSORT, nullptr, &found);
            if (status != 0 && status != GLOB_NOMATCH) { globfree(&found); throw std::runtime_error("cannot expand source: " + pattern); }
            for (size_t i = 0; i < found.gl_pathc; ++i) matches.insert(found.gl_pathv[i]);
            globfree(&found);
        }
        if (matches.empty()) throw std::runtime_error(target.text("name") + ": sources matched no files");
        for (const auto& value : matches) {
            const fs::path path(value);
            const auto extension = path.extension();
            if (!Files::inside(path, root_) || !fs::is_regular_file(path) ||
                (extension != ".cc" && extension != ".cpp" && extension != ".c" && extension != ".S"))
                throw std::runtime_error("unsupported source: " + value);
        }
        return {matches.begin(), matches.end()};
    }
    void buildTarget(const Json& target) {
        const std::string name = target.text("name"), kind = kinds_.at(name);
        Arguments objects;
        for (const auto& source : sources(target)) {
            const auto extension = fs::path(source).extension();
            const bool cpp = extension == ".cc" || extension == ".cpp";
            Arguments command{cpp ? cxx_ : cc_};
            append(command, config_.strings("cppflags")); append(command, target.strings("cppflags"));
            for (const auto& directory : {"", "stdc", "stdc++", "posix", "os-c++", "osgui-c++"}) {
                command.push_back("-I"); command.push_back((sdk_ / "include" / directory).string());
            }
            append(command, {"-I", root_.string()}); append(command, paths(target, "include_dirs", "-I"));
            const std::string flagField = cpp ? "cxxflags" : extension == ".S" ? "asflags" : "cflags";
            append(command, config_.strings(flagField)); append(command, target.strings(flagField));
            const auto object = outputPath(fs::path("obj") / name / fs::path(source).lexically_relative(root_).concat(".o"));
            TemporaryFile temporary(object);
            append(command, {"-c", source, "-o", temporary.path().string()});
            Process::run(command, root_); temporary.publish(object); objects.push_back(object.string());
        }
        Arguments command{cxx_};
        if (kind == "shared") append(command, {"-shared", "-Wl,-soname," + name});
        else {
            const bool service = kind == "boot-service";
            append(command, {"-no-pie", "-Wl,-E", "-Xlinker", "-T", "-Xlinker", (sdk_ / "lib" / (service ? "boot-service.lds" : "user.lds")).string(),
                             service ? "-Wl,--no-dynamic-linker" : "-Wl,--dynamic-linker=/lib/ld-osinfo-x86-64.so"});
            objects.push_back((sdk_ / "lib" / (service ? "service_start.o" : "start.o")).string());
        }
        append(command, paths(target, "library_dirs", "-L"));
        append(command, config_.strings("ldflags")); append(command, target.strings("ldflags"));
        if (auto runpath = target.text("runpath"); !runpath.empty()) append(command, {"-Xlinker", "-rpath", "-Xlinker", runpath});
        const auto destination = outputPath(fs::path("packages") / name_ / name);
        TemporaryFile temporary(destination);
        append(command, {"-o", temporary.path().string()}); append(command, objects);
        const Arguments libraries = target.find("libraries") ? target.strings("libraries") : config_.find("libraries") ? config_.strings("libraries") : Arguments{"c", "os-c++"};
        for (const auto& library : libraries) {
            const std::string filename = "lib" + Files::identifier(library) + ".so";
            fs::path selected;
            if (filename != name && kinds_.count(filename) && kinds_.at(filename) == "shared") {
                selected = payload_ / filename;
                if (std::find(targets_.begin(), targets_.end(), filename) == targets_.end())
                    throw std::runtime_error(name + ": shared-library target must be built first: " + filename);
            } else {
                Arguments directories = config_.strings("library_dirs"); append(directories, target.strings("library_dirs"));
                for (const auto& directory : directories) {
                    auto candidate = configuredPath(directory) / filename;
                    if (fs::is_regular_file(candidate)) { selected = candidate; break; }
                }
                if (selected.empty()) selected = sdk_ / "lib" / filename;
            }
            if (!fs::is_regular_file(selected)) throw std::runtime_error(name + ": missing library: " + library);
            command.push_back(selected.string());
        }
        for (const auto* object : {static_cast<const Json*>(&config_), &target})
            for (const auto& library : object->strings("ldlibs")) command.push_back("-l" + Files::identifier(library));
        Process::run(command, root_); temporary.publish(destination); targets_.push_back(name);
    }

 public:
    explicit Project(const Options& options) : root_(fs::canonical(options.project)),
        config_(Json::parse(Files::read(root_ / "infoide-project.json"))) {
        name_ = Files::identifier(config_.at("name").string());
        Files::identifier(config_.text("version", "1"));
        std::string sdk = options.sdk.empty() ? config_.text("sdk") : options.sdk;
        if (sdk.empty()) if (const char* environment = std::getenv("OSINFO_SDK")) sdk = environment;
        if (sdk.empty()) sdk = Resources::find("sdk").string();
        sdk_ = configuredPath(sdk);
        auto metadata = Json::parse(Files::read(sdk_ / "sdk.json"));
        if (metadata.text("target") != "osinfo-x86-64") throw std::runtime_error("unsupported SDK target");
        cxx_ = compiler(options.cxx, "cxx", Process::available("x86_64-elf-g++") ? "x86_64-elf-g++" : "g++");
        cc_ = compiler(options.cc, "cc", Process::available("x86_64-elf-gcc") ? "x86_64-elf-gcc" : "gcc");
        output_ = relative("build"); payload_ = outputPath(fs::path("packages") / name_);
        for (const auto& target : config_.at("targets").array()) {
            const auto name = Files::identifier(target.at("name").string());
            const auto kind = target.text("kind", "application");
            if (name == "package.conf" || (kind != "application" && kind != "shared" && kind != "boot-service") || !kinds_.emplace(name, kind).second)
                throw std::runtime_error("duplicate, reserved or unsupported target: " + name);
        }
        if (kinds_.empty()) throw std::runtime_error("targets must not be empty");
    }
    void build() {
        // Invalidate the previous package before starting a possibly failing build.
        fs::remove(outputPath(fs::path("packages") / (name_ + ".inpk")));
        for (const auto& target : config_.at("targets").array()) {
            try { buildTarget(target); }
            catch (const std::exception& error) { throw std::runtime_error("target " + target.text("name") + ": " + error.what()); }
        }
        std::cout << "Built " << name_ << '\n';
    }
    void package() {
        build();
        const auto& install = config_.at("install").object();
        if (install.empty()) throw std::runtime_error("install mappings must not be empty");
        std::string manifest = "name=" + name_ + "\nversion=" + config_.text("version", "1") + "\n";
        Arguments members{"package.conf"};
        std::set<std::string> destinations;
        for (const auto& [source, value] : install) {
            Files::relative(source);
            const auto target = value.string();
            if (source == "package.conf" || target.size() < 2 || target[0] != '/') throw std::runtime_error("invalid install mapping: " + source);
            Files::relative(target.substr(1));
            if (target == "/lib/ld-osinfo-x86-64.so" || target == "/lib/version.1") throw std::runtime_error("system-managed destination: " + target);
            if (!destinations.insert(target).second) throw std::runtime_error("duplicate install destination: " + target);
            fs::path staged = outputPath(fs::path("packages") / name_ / source);
            const bool binary = kinds_.count(source) && kinds_.at(source) != "shared";
            if (!kinds_.count(source)) {
                auto input = relative(source);
                if (!fs::is_regular_file(input)) throw std::runtime_error("missing package file: " + source);
                TemporaryFile temporary(staged); fs::copy_file(input, temporary.path(), fs::copy_options::overwrite_existing); temporary.publish(staged);
            }
            if (!fs::is_regular_file(staged) || fs::is_symlink(staged)) throw std::runtime_error("invalid package payload: " + source);
            fs::permissions(staged, static_cast<fs::perms>(binary ? 0755 : 0644));
            manifest += std::string(binary ? "bin=" : "file=") + target + "\ninstall=" + source + " -> " + target + "\n";
            members.push_back(source);
        }
        TemporaryFile config(payload_ / "package.conf"); Files::write(config.path(), manifest); config.publish(payload_ / "package.conf");
        fs::permissions(payload_ / "package.conf", static_cast<fs::perms>(0644));
        const auto archive = outputPath(fs::path("packages") / (name_ + ".inpk"));
        TemporaryFile temporary(archive);
        PackageTools::run({"inpkg", "pack", payload_.string(), "--manifest", (payload_ / "package.conf").string(), "--output", temporary.path().string()});
        temporary.publish(archive);
        std::cout << "Package: " << archive << '\n';
    }
    fs::path archive() const { return output_ / "packages" / (name_ + ".inpk"); }
    const fs::path& root() const { return root_; }
    static void create(const fs::path& directory) {
        auto destination = fs::absolute(directory).lexically_normal();
        const auto name = Files::identifier(destination.filename().string());
        if (fs::exists(destination)) throw std::runtime_error("project path already exists: " + destination.string());
        if (!fs::create_directory(destination)) throw std::runtime_error("cannot create project directory");
        fs::create_directory(destination / "src");
        Files::write(destination / "src/main.cc", "#include <cstdio>\nint main() { std::puts(\"Hello from InfoIDE\"); return 0; }\n");
        Files::write(destination / "infoide-project.json", "{\n  \"name\": \"" + name + "\",\n" + std::string(R"JSON(  "version": "0.1.1",
  "cppflags": ["-nostdinc", "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fPIC", "-m64", "-mcmodel=small", "-mno-red-zone"],
  "cflags": ["-std=c17", "-g", "-Os", "-Wall", "-Wextra"],
  "cxxflags": ["-std=c++20", "-g", "-fno-exceptions", "-fno-rtti", "-fno-use-cxa-atexit", "-fno-threadsafe-statics", "-nostdinc++", "-Os", "-Wall", "-Wextra"],
  "asflags": [],
  "ldflags": ["-nostdlib", "-nostartfiles", "-nodefaultlibs", "-Wl,--build-id=none,--hash-style=sysv", "-Wl,--no-as-needed"],
  "ldlibs": ["gcc"],
)JSON") + "  \"targets\": [{\"name\": \"" + name + "\", \"sources\": [\"src/*.cc\"], \"libraries\": [\"c\", \"os-c++\"]}],\n  \"install\": {\"" + name + "\": \"/usr/bin/" + name + "\"}\n}\n");
        DebugConfiguration::create(destination);
        Files::write(destination / ".gitignore", "build/\n/infoide\n/*.img\n/*.iso\n/*.img.lock\n/*.iso.lock\n");
        std::cout << "Created " << destination << '\n';
    }
};

class Cli {
 public:
    static int run(int argc, char** argv) {
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "help") {
            std::cout << "Usage: infoide create NAME\n       infoide build|pkg|install|run|debug [--project DIR] [--sdk DIR] [--cxx PATH] [--cc PATH]\n       install/run/debug: [--debug-config FILE] [--boot disk|cdrom] [--disk BASE.img] [--iso BASE.iso] [--output FILE] [--headless] [--qemu PATH]\n       infoide bundle --output FILE.zip PACKAGE.inpk...\n       infoide distribution --sdk DIR --disk FILE [--iso FILE] --output DIR\n";
            return argc < 2 ? 2 : 0;
        }
        const std::string command = argv[1];
        if (command == "--version") { std::cout << "infoide 0.1.1\n"; return 0; }
        if (command == "create") { if (argc != 3) throw std::runtime_error("usage: infoide create NAME"); Project::create(argv[2]); return 0; }
        if (command == "bundle") {
            if (argc < 5 || std::string(argv[2]) != "--output") throw std::runtime_error("usage: infoide bundle --output FILE.zip PACKAGE.inpk...");
            Arguments inputs; for (int i = 4; i < argc; ++i) inputs.push_back(argv[i]);
            Bundle::write(argv[3], inputs); return 0;
        }
        if (command != "build" && command != "pkg" && command != "install" && command != "run" && command != "debug" && command != "distribution") throw std::runtime_error("unknown command: " + command);
        Options options;
        for (int i = 2; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--headless") { options.headless = true; continue; }
            if (i + 1 == argc) throw std::runtime_error("missing value for " + key);
            const std::string value = argv[++i];
            if (key == "--project") options.project = value;
            else if (key == "--sdk") options.sdk = value;
            else if (key == "--cxx") options.cxx = value;
            else if (key == "--cc") options.cc = value;
            else if (key == "--iso") options.iso = value;
            else if (key == "--disk") options.disk = value;
            else if (key == "--boot") options.boot = value;
            else if (key == "--debug-config") options.debugConfig = value;
            else if (key == "--output") options.output = value;
            else if (key == "--qemu") options.qemu = value;
            else throw std::runtime_error("unknown option: " + key);
        }
        if (command == "distribution") { Distribution::create(options); return 0; }
        Project project(options);
        if (command == "install" || command == "run" || command == "debug") {
            DebugConfiguration settings(options, project.root());
            ImageLock lock(settings.image);
            project.package();
            auto archives = settings.dependencies;
            archives.push_back(project.archive().string());
            if (settings.boot == "disk") DiskImage::install(settings.base(), settings.image, archives);
            else OpticalImage::install(settings.base(), settings.image, archives);
            if (command != "install") settings.run(command == "debug");
        } else if (command == "build") project.build(); else project.package();
        return 0;
    }
};
}  // namespace infoide

int main(int argc, char** argv) {
    try { return infoide::Cli::run(argc, argv); }
    catch (const std::exception& error) { std::cerr << "infoide: " << error.what() << '\n'; return 1; }
}
