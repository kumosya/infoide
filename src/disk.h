// Regular-file image updates; no loop devices, mounts, or elevated privileges.
class FileLock {
    int descriptor_ = -1;
 public:
    explicit FileLock(const fs::path& path, bool write) {
        descriptor_ = open(path.c_str(), (write ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor_ < 0) throw std::runtime_error("cannot open image: " + path.string());
        struct flock lock{};
        lock.l_type = write ? F_WRLCK : F_RDLCK; lock.l_whence = SEEK_SET;
        if (fcntl(descriptor_, F_OFD_SETLK, &lock) < 0) {
            close(descriptor_); descriptor_ = -1;
            throw std::runtime_error("image is in use; stop QEMU before installing: " + path.string());
        }
    }
    ~FileLock() { if (descriptor_ >= 0) close(descriptor_); }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
};

class ImageLock {
    int descriptor_ = -1;
 public:
    explicit ImageLock(const fs::path& image) {
        if (fs::is_symlink(image) || (fs::exists(image) && !fs::is_regular_file(image))) throw std::runtime_error("image must be a regular file");
        descriptor_ = open((image.string() + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (descriptor_ < 0) throw std::runtime_error("cannot open project image lock");
        if (flock(descriptor_, LOCK_EX | LOCK_NB) < 0) {
            close(descriptor_); descriptor_ = -1;
            throw std::runtime_error("project image is in use");
        }
        try { if (fs::exists(image)) { FileLock available(image, true); } }
        catch (...) { close(descriptor_); descriptor_ = -1; throw; }
    }
    ~ImageLock() { if (descriptor_ >= 0) close(descriptor_); }
    ImageLock(const ImageLock&) = delete;
    ImageLock& operator=(const ImageLock&) = delete;
};

class DiskImage {
 public:
    struct Partition { uint64_t offset, size; };
    static Partition rootPartition(const fs::path& image) {
        if (!fs::is_regular_file(image) || fs::is_symlink(image)) throw std::runtime_error("disk image must be a regular file");
        std::ifstream input(image, std::ios::binary);
        unsigned char mbr[512]{}; input.read(reinterpret_cast<char*>(mbr), sizeof(mbr));
        auto le32 = [&](unsigned position) { uint64_t value = 0; for (unsigned i = 0; i < 4; ++i) value |= uint64_t(mbr[position + i]) << (8 * i); return value; };
        const Partition root{le32(454) * 512, le32(458) * 512};
        if (!input || mbr[510] != 0x55 || mbr[511] != 0xaa || mbr[450] != 0x83 ||
            root.offset < 512 || root.size < 1024 * 1024 || root.offset > fs::file_size(image) || root.size > fs::file_size(image) - root.offset)
            throw std::runtime_error("expected an MBR image with a Linux root in partition 1");
        return root;
    }
    static void copyRange(const fs::path& source, uint64_t offset, uint64_t size, const fs::path& destination, uint64_t targetOffset) {
        std::ifstream input(source, std::ios::binary);
        std::fstream output(destination, std::ios::binary | std::ios::in | std::ios::out);
        input.seekg(offset); output.seekp(targetOffset);
        std::vector<char> buffer(1024 * 1024);
        while (size) {
            const size_t count = std::min<uint64_t>(size, buffer.size());
            input.read(buffer.data(), count);
            if (input.gcount() != static_cast<std::streamsize>(count)) throw std::runtime_error("short disk image read");
            output.write(buffer.data(), count); size -= count;
        }
        output.flush();
        if (!output) throw std::runtime_error("disk image write failed");
    }
    static void install(const fs::path& base, const fs::path& output, const Arguments& archives) {
        if (!base.empty() && fs::weakly_canonical(base) == fs::weakly_canonical(output)) throw std::runtime_error("project disk must differ from template");
        const auto source = fs::exists(output) ? fs::canonical(output) : fs::canonical(base);
        FileLock sourceLock(source, fs::exists(output));
        const auto partition = rootPartition(source);
        TemporaryDirectory temporary;
        const auto stage = temporary.path(), filesystem = stage / "root.ext3";
        Files::write(filesystem, ""); fs::resize_file(filesystem, partition.size);
        copyRange(source, partition.offset, partition.size, filesystem, 0);
        FilesystemImage::install(filesystem, archives, stage);
        TemporaryFile result(output);
        fs::copy_file(source, result.path(), fs::copy_options::overwrite_existing);
        copyRange(filesystem, 0, partition.size, result.path(), partition.offset);
        fs::permissions(result.path(), fs::exists(output) ? fs::status(output).permissions() : static_cast<fs::perms>(0600));
        result.publish(output);
        std::cout << "Installed application into project disk: " << output << '\n';
    }
};
