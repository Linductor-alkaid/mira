#include "memory_capacity_vfs.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace mira::storage {
Error memory_capacity_error() {
    Error e;
    e.code = ErrorCode::ResourceExhausted;
    e.domain = "mira.state_store.capacity";
    e.safe_message = "memory file capacity exhausted";
    return e;
}
class MemoryCapacityVfs::Impl final {
  public:
    struct File {
        sqlite3_file file{};
        sqlite3_file *real = nullptr;
        Impl *owner = nullptr;
        std::size_t index = 0;
    };
    sqlite3_vfs vfs{};
    sqlite3_vfs *base = nullptr;
    std::string vfs_name;
    std::array<std::filesystem::path, 4> paths;
    std::array<std::string, 4> names;
    std::uint64_t limit = 0, operation_limit = 0, reserve = 0, peak = 0;
    bool rejection = false, registered = false;

    ~Impl() {
        if (registered)
            sqlite3_vfs_unregister(&vfs);
    }
    static Impl &owner(sqlite3_vfs *v) { return *static_cast<Impl *>(v->pAppData); }
    static File &file(sqlite3_file *f) { return *reinterpret_cast<File *>(f); }
    bool known(const char *p) const noexcept {
        if (!p)
            return false;
        for (const auto &n : names)
            if (std::strcmp(p, n.c_str()) == 0)
                return true;
        return false;
    }
    static const sqlite3_io_methods *methods(sqlite3_file *f) { return file(f).real->pMethods; }

    int scan(SqliteMemoryCapacitySnapshot &out) const noexcept {
        try {
            std::array<std::uint64_t, 4> sizes{};
            for (std::size_t n = 0; n < paths.size(); ++n) {
                std::error_code ec;
                const auto status = std::filesystem::symlink_status(paths[n], ec);
                if (ec == std::errc::no_such_file_or_directory ||
                    status.type() == std::filesystem::file_type::not_found)
                    continue;
                if (ec || !std::filesystem::is_regular_file(status))
                    return SQLITE_IOERR;
                const auto bytes = std::filesystem::file_size(paths[n], ec);
                if (ec || bytes > std::numeric_limits<std::uint64_t>::max() - out.file_bytes)
                    return SQLITE_IOERR;
                sizes[n] = bytes;
                out.file_bytes += bytes;
            }
            out.limit_bytes = limit;
            out.reserve_bytes = reserve;
            out.database_bytes = sizes[0];
            out.wal_bytes = sizes[1];
            out.shm_bytes = sizes[2];
            out.journal_bytes = sizes[3];
            out.peak_admitted_file_bytes = peak;
            return SQLITE_OK;
        } catch (...) {
            return SQLITE_IOERR;
        }
    }
    int admit(std::size_t index, sqlite3_int64 length) noexcept {
        if (length < 0 || index >= paths.size())
            return SQLITE_IOERR;
        SqliteMemoryCapacitySnapshot current;
        const int scanned = scan(current);
        if (scanned != SQLITE_OK)
            return scanned;
        const std::array<std::uint64_t, 4> sizes{current.database_bytes, current.wal_bytes,
                                                 current.shm_bytes, current.journal_bytes};
        const auto wanted = static_cast<std::uint64_t>(length);
        const auto growth = wanted > sizes[index] ? wanted - sizes[index] : 0;
        if (current.file_bytes > operation_limit || growth > operation_limit - current.file_bytes) {
            rejection = true;
            return SQLITE_FULL;
        }
        peak = std::max(peak, current.file_bytes + growth);
        return SQLITE_OK;
    }
    static const sqlite3_io_methods &io() {
        static const sqlite3_io_methods value{
            3,
            [](sqlite3_file *f) { return methods(f)->xClose(file(f).real); },
            [](sqlite3_file *f, void *p, int n, sqlite3_int64 at) {
                return methods(f)->xRead(file(f).real, p, n, at);
            },
            [](sqlite3_file *f, const void *p, int n, sqlite3_int64 at) {
                auto &wrapped = file(f);
                if (n < 0 || at < 0 || at > std::numeric_limits<sqlite3_int64>::max() - n)
                    return SQLITE_IOERR_WRITE;
                const int admitted = wrapped.owner->admit(wrapped.index, at + n);
                return admitted == SQLITE_OK ? methods(f)->xWrite(wrapped.real, p, n, at)
                                             : admitted;
            },
            [](sqlite3_file *f, sqlite3_int64 n) {
                auto &wrapped = file(f);
                const int admitted = wrapped.owner->admit(wrapped.index, n);
                return admitted == SQLITE_OK ? methods(f)->xTruncate(wrapped.real, n) : admitted;
            },
            [](sqlite3_file *f, int flags) { return methods(f)->xSync(file(f).real, flags); },
            [](sqlite3_file *f, sqlite3_int64 *n) {
                return methods(f)->xFileSize(file(f).real, n);
            },
            [](sqlite3_file *f, int n) { return methods(f)->xLock(file(f).real, n); },
            [](sqlite3_file *f, int n) { return methods(f)->xUnlock(file(f).real, n); },
            [](sqlite3_file *f, int *n) { return methods(f)->xCheckReservedLock(file(f).real, n); },
            [](sqlite3_file *f, int op, void *p) {
                // Hints must not preallocate beyond xWrite/xTruncate admission.
                if (op == SQLITE_FCNTL_SIZE_HINT || op == SQLITE_FCNTL_CHUNK_SIZE)
                    return SQLITE_OK;
                return methods(f)->xFileControl(file(f).real, op, p);
            },
            [](sqlite3_file *f) { return methods(f)->xSectorSize(file(f).real); },
            [](sqlite3_file *f) { return methods(f)->xDeviceCharacteristics(file(f).real); },
            [](sqlite3_file *f, int page, int size, int extend, void volatile **out) {
                if (methods(f)->iVersion < 2 || !methods(f)->xShmMap)
                    return SQLITE_IOERR_SHMMAP;
                if (extend) {
                    if (page < 0 || size <= 0)
                        return SQLITE_IOERR_SHMMAP;
                    const auto length = (static_cast<sqlite3_int64>(page) + 1) * size;
                    const int admitted = file(f).owner->admit(2, length);
                    if (admitted != SQLITE_OK)
                        return admitted;
                }
                return methods(f)->xShmMap(file(f).real, page, size, extend, out);
            },
            [](sqlite3_file *f, int at, int n, int flags) {
                return methods(f)->xShmLock(file(f).real, at, n, flags);
            },
            [](sqlite3_file *f) { methods(f)->xShmBarrier(file(f).real); },
            [](sqlite3_file *f, int remove) { return methods(f)->xShmUnmap(file(f).real, remove); },
            [](sqlite3_file *, sqlite3_int64, int, void **out) {
                *out = nullptr; // Keep mapping behind normal bounded file operations.
                return SQLITE_OK;
            },
            [](sqlite3_file *, sqlite3_int64, void *) { return SQLITE_OK; }};
        return value;
    }
    void configure() {
        if (!base)
            return;
        vfs = *base;
        vfs.iVersion = std::min(base->iVersion, 2);
        vfs.szOsFile = static_cast<int>(sizeof(File)) + base->szOsFile;
        vfs.zName = vfs_name.c_str();
        vfs.pAppData = this;
        vfs.pNext = nullptr;
        vfs.xOpen = [](sqlite3_vfs *v, sqlite3_filename name, sqlite3_file *out, int flags,
                       int *out_flags) {
            auto &s = owner(v);
            auto *wrapped = new (out) File{};
            if (!name)
                return SQLITE_CANTOPEN;
            std::size_t index = 0;
            while (index < s.names.size() && std::strcmp(name, s.names[index].c_str()) != 0)
                ++index;
            if (index == s.names.size())
                return SQLITE_CANTOPEN;
            wrapped->real =
                reinterpret_cast<sqlite3_file *>(reinterpret_cast<std::byte *>(out) + sizeof(File));
            std::memset(wrapped->real, 0, static_cast<std::size_t>(s.base->szOsFile));
            wrapped->owner = &s;
            wrapped->index = index;
            const int rc = s.base->xOpen(s.base, name, wrapped->real, flags, out_flags);
            if (rc != SQLITE_OK) {
                if (wrapped->real->pMethods)
                    wrapped->real->pMethods->xClose(wrapped->real);
                return rc;
            }
            wrapped->file.pMethods = &io();
            return SQLITE_OK;
        };
        vfs.xDelete = [](sqlite3_vfs *v, const char *p, int sync) {
            auto &s = owner(v);
            if (!s.known(p))
                return SQLITE_IOERR_DELETE;
            return s.base->xDelete(s.base, p, sync);
        };
        vfs.xAccess = [](sqlite3_vfs *v, const char *p, int flags, int *out) {
            auto &s = owner(v);
            return s.base->xAccess(s.base, p, flags, out);
        };
        vfs.xFullPathname = [](sqlite3_vfs *v, const char *p, int n, char *out) {
            auto &s = owner(v);
            return s.base->xFullPathname(s.base, p, n, out);
        };
        vfs.xDlOpen = [](sqlite3_vfs *v, const char *p) {
            auto &s = owner(v);
            return s.base->xDlOpen(s.base, p);
        };
        vfs.xDlError = [](sqlite3_vfs *v, int n, char *out) {
            auto &s = owner(v);
            s.base->xDlError(s.base, n, out);
        };
        vfs.xDlSym = [](sqlite3_vfs *v, void *p, const char *n) {
            auto &s = owner(v);
            return s.base->xDlSym(s.base, p, n);
        };
        vfs.xDlClose = [](sqlite3_vfs *v, void *p) {
            auto &s = owner(v);
            s.base->xDlClose(s.base, p);
        };
        vfs.xRandomness = [](sqlite3_vfs *v, int n, char *out) {
            auto &s = owner(v);
            return s.base->xRandomness(s.base, n, out);
        };
        vfs.xSleep = [](sqlite3_vfs *v, int n) {
            auto &s = owner(v);
            return s.base->xSleep(s.base, n);
        };
        vfs.xCurrentTime = [](sqlite3_vfs *v, double *out) {
            auto &s = owner(v);
            return s.base->xCurrentTime(s.base, out);
        };
        vfs.xGetLastError = [](sqlite3_vfs *v, int n, char *out) {
            auto &s = owner(v);
            return s.base->xGetLastError ? s.base->xGetLastError(s.base, n, out) : 0;
        };
        vfs.xCurrentTimeInt64 = [](sqlite3_vfs *v, sqlite3_int64 *out) {
            auto &s = owner(v);
            if (s.base->iVersion >= 2 && s.base->xCurrentTimeInt64)
                return s.base->xCurrentTimeInt64(s.base, out);
            double value = 0;
            const int rc = s.base->xCurrentTime(s.base, &value);
            *out = static_cast<sqlite3_int64>(value * 86400000.0);
            return rc;
        };
    }
};
MemoryCapacityVfs::MemoryCapacityVfs(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MemoryCapacityVfs::~MemoryCapacityVfs() = default;
Result<std::unique_ptr<MemoryCapacityVfs>>
MemoryCapacityVfs::create(const SqliteMemoryStoreOptions &options) {
    auto impl = std::make_unique<Impl>();
    impl->limit = options.max_storage_bytes;
    impl->operation_limit = impl->limit;
    impl->reserve = options.capacity_reserve_bytes;
    impl->base = sqlite3_vfs_find(nullptr);
    if (!impl->base || impl->base->mxPathname <= 0 || impl->base->mxPathname > 1024 * 1024)
        return memory_capacity_error();
    // SQLite xOpen receives the native VFS spelling, which can differ from
    // filesystem::weakly_canonical (notably drive/path casing on Windows).
    std::vector<char> full(static_cast<std::size_t>(impl->base->mxPathname) + 1, '\0');
    if (impl->base->xFullPathname(impl->base, options.path.string().c_str(),
                                  static_cast<int>(full.size()), full.data()) != SQLITE_OK)
        return memory_capacity_error();
    const std::string name(full.data());
    impl->names = {name, name + "-wal", name + "-shm", name + "-journal"};
    std::error_code ec;
    const auto path = std::filesystem::absolute(options.path, ec).lexically_normal();
    if (ec)
        return memory_capacity_error();
    impl->paths = {path, std::filesystem::path(path.string() + "-wal"),
                   std::filesystem::path(path.string() + "-shm"),
                   std::filesystem::path(path.string() + "-journal")};
    SqliteMemoryCapacitySnapshot current;
    if (impl->scan(current) != SQLITE_OK || current.file_bytes > impl->limit)
        return memory_capacity_error();
    impl->peak = current.file_bytes;
    impl->vfs_name = "mira-memory-capacity-" + Id128::generate().to_string();
    impl->configure();
    if (!impl->base || sqlite3_vfs_register(&impl->vfs, 0) != SQLITE_OK)
        return memory_capacity_error();
    impl->registered = true;
    return std::unique_ptr<MemoryCapacityVfs>(new MemoryCapacityVfs(std::move(impl)));
}
const char *MemoryCapacityVfs::name() const { return impl_->vfs_name.c_str(); }
Result<SqliteMemoryCapacitySnapshot> MemoryCapacityVfs::snapshot() const {
    SqliteMemoryCapacitySnapshot result;
    if (impl_->scan(result) != SQLITE_OK)
        return memory_capacity_error();
    return result;
}
void MemoryCapacityVfs::reset_rejection() {
    impl_->rejection = false;
    impl_->operation_limit = impl_->limit;
}
void MemoryCapacityVfs::set_operation_limit(std::uint64_t bytes) {
    impl_->operation_limit = std::min(bytes, impl_->limit);
}
bool MemoryCapacityVfs::rejected() const { return impl_->rejection; }
} // namespace mira::storage
