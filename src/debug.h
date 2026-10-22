class DebugConfiguration {
    fs::path root_;
    std::string template_;
    static uint32_t number(const Json& config, const std::string& key, uint32_t fallback, uint32_t maximum) {
        const auto* field = config.find(key);
        if (!field) return fallback;
        const auto* value = std::get_if<double>(&field->value);
        if (!value || !std::isfinite(*value) || *value < 1 || *value > maximum || std::floor(*value) != *value) throw std::runtime_error("invalid debug setting: " + key);
        return static_cast<uint32_t>(*value);
    }
    static bool boolean(const Json& config, const std::string& key, bool fallback) {
        const auto* field = config.find(key);
        if (!field) return fallback;
        if (const auto* value = std::get_if<bool>(&field->value)) return *value;
        throw std::runtime_error("invalid boolean debug setting: " + key);
    }
 public:
    std::string boot, qemu, display, serial;
    Arguments dependencies;
    fs::path image;
    uint32_t memory, cpus, gdbPort;
    bool waitForGdb;
    explicit DebugConfiguration(const Options& options, const fs::path& root) : root_(root) {
        auto path = root / "infoide-debug.json";
        if (!options.debugConfig.empty()) path = fs::path(options.debugConfig).is_absolute() ? fs::path(options.debugConfig) : root / options.debugConfig;
        if (!fs::exists(path) && options.debugConfig.empty()) create(root);
        const auto config = Json::parse(Files::read(path));
        std::set<fs::path> seen;
        for (const auto& dependency : config.strings("dependencies")) {
            const auto package = fs::path(dependency).is_absolute() ? fs::path(dependency) : root / dependency;
            if (dependency.empty() || dependency.find('\0') != std::string::npos ||
                package.extension() != ".inpk" || !fs::is_regular_file(package))
                throw std::runtime_error("missing or invalid dependency package: " + dependency);
            const auto canonical = fs::canonical(package);
            if (!seen.insert(canonical).second) throw std::runtime_error("duplicate dependency package: " + dependency);
            dependencies.push_back(canonical.string());
        }
        boot = options.boot.empty() ? config.text("boot", "disk") : options.boot;
        if (boot != "disk" && boot != "cdrom") throw std::runtime_error("boot must be disk or cdrom");
        const bool disk = boot == "disk";
        template_ = disk ? options.disk : options.iso;
        if (template_.empty()) template_ = config.text(disk ? "disk_template" : "iso_template");
        const auto configured = options.output.empty() ? config.text(disk ? "disk_image" : "iso_image", disk ? "osinfo.img" : "osinfo.iso") : options.output;
        image = fs::weakly_canonical(fs::path(configured).is_absolute() ? fs::path(configured) : root / configured);
        if (!Files::inside(image, root) || image.parent_path() != root || image.extension() != (disk ? ".img" : ".iso")) throw std::runtime_error("debug image must be a project-root .img or .iso file");
        qemu = options.qemu.empty() ? config.text("qemu", "qemu-system-x86_64") : options.qemu;
        if (qemu.find('/') != std::string::npos) qemu = fs::weakly_canonical(fs::path(qemu).is_absolute() ? fs::path(qemu) : root / qemu).string();
        memory = number(config, "memory_mb", 512, 1048576); cpus = number(config, "cpus", 4, 256); gdbPort = number(config, "gdb_port", 1234, 65535);
        display = options.headless ? "none" : config.text("display", "gtk"); serial = config.text("serial", "stdio");
        waitForGdb = boolean(config, "wait_for_gdb", true);
        if (display != "none" && display != "gtk" && display != "sdl") throw std::runtime_error("unsupported debug display");
        if (serial != "stdio" && serial != "none") throw std::runtime_error("debug serial must be stdio or none");
    }
    fs::path base() const {
        if (!template_.empty()) return fs::canonical(fs::path(template_).is_absolute() ? fs::path(template_) : root_ / template_);
        if (boot == "disk" && fs::exists(image)) return {};
        return Resources::find(boot == "disk" ? "osinfo.img" : "osinfo.iso");
    }
    void run(bool debug) const {
        Arguments arguments{qemu, "-m", std::to_string(memory), "-smp", std::to_string(cpus), "-boot", boot == "disk" ? "order=c,strict=on" : "order=d,strict=on"};
        if (boot == "disk") {
            std::string filename;
            for (char character : image.string()) { filename += character; if (character == ',') filename += ','; }
            arguments.insert(arguments.end(), {"-drive", "file=" + filename + ",if=none,format=raw,id=osdisk", "-device", "virtio-blk-pci,drive=osdisk,disable-legacy=on,bootindex=0"});
        }
        else arguments.insert(arguments.end(), {"-cdrom", image.string()});
        arguments.insert(arguments.end(), {"-display", display, "-serial", serial, "-no-reboot"});
        if (debug) {
            arguments.insert(arguments.end(), {"-gdb", "tcp:127.0.0.1:" + std::to_string(gdbPort)});
            if (waitForGdb) arguments.push_back("-S");
        }
        Process::run(arguments, root_);
    }
    static void create(const fs::path& root) {
        const auto path = root / "infoide-debug.json";
        if (fs::exists(path)) return;
        Files::write(path, R"JSON({
  "boot": "disk",
  "disk_template": "",
  "iso_template": "",
  "disk_image": "osinfo.img",
  "iso_image": "osinfo.iso",
  "dependencies": [],
  "qemu": "qemu-system-x86_64",
  "memory_mb": 512,
  "cpus": 4,
  "display": "gtk",
  "serial": "stdio",
  "gdb_port": 1234,
  "wait_for_gdb": true
}
)JSON");
    }
};
