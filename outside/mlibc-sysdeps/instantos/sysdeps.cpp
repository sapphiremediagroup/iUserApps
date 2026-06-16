#include <abi-bits/errno.h>
#include <bits/ensure.h>
#include <bits/syscall.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/debug.hpp>
#include <mlibc/tcb.hpp>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <abi-bits/ioctls.h>
#include <abi-bits/signal.h>
#include <abi-bits/stat.h>
#include <abi-bits/fcntl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/socket.h>
#include <poll.h>
#include <sys/utsname.h>

// Kernel KernelStatfs layout (struct KernelStatfs in the kernel).
struct instantos_statfs {
    unsigned long blockSize;
    unsigned long totalBlocks;
    unsigned long freeBlocks;
    unsigned long totalInodes;
    unsigned long freeInodes;
    unsigned long nameMax;
    unsigned int fsType;
    unsigned int reserved;
};

// Kernel-side stat layout (struct Stat in the kernel). Flat 64-bit timestamps,
// field order differs from mlibc's struct stat. Field names avoid the POSIX
// st_atime/st_mtime/st_ctime macros.
struct instantos_stat {
    unsigned long k_dev;
    unsigned long k_ino;
    unsigned int k_mode;
    unsigned int k_nlink;
    unsigned int k_uid;
    unsigned int k_gid;
    unsigned long k_rdev;
    unsigned long k_size;
    unsigned long k_blksize;
    unsigned long k_blocks;
    unsigned long k_atime;
    unsigned long k_mtime;
    unsigned long k_ctime;
};

// Kernel-side signal-action layout (struct SigActionInfo in the kernel). All
// fields are 64-bit; note the field order differs from mlibc's struct sigaction.
struct instantos_sigaction {
    unsigned long handler;
    unsigned long mask;
    unsigned long flags;
    unsigned long restorer;
};

// Minimal winsize matching the kernel KernelWinsize / Linux struct winsize.
struct instantos_winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

extern "C" long __do_syscall0(long);
extern "C" long __do_syscall1(long, long);
extern "C" long __do_syscall2(long, long, long);
extern "C" long __do_syscall3(long, long, long, long);
extern "C" long __do_syscall4(long, long, long, long, long);
extern "C" long __do_syscall5(long, long, long, long, long, long);

namespace {
constexpr int kMaxFd = 256;
// constinit forces compile-time initialization so no static constructor is
// emitted. The rtld links this translation unit and panics if it finds any
// own global constructor (init_array entry), so every file-scope object here
// must be constant-initialized.
constinit long fdTable[kMaxFd] = {0, 1, 2};

int sc_error(long ret) {
    return static_cast<unsigned long>(ret) > static_cast<unsigned long>(-4096) ? -ret : 0;
}

long handle_for_fd(int fd) {
    if (fd < 0 || fd >= kMaxFd) {
        return -1;
    }
    return fdTable[fd];
}

int register_fd(long handle) {
    for (int fd = 3; fd < kMaxFd; ++fd) {
        if (fdTable[fd] == 0) {
            fdTable[fd] = handle;
            return fd;
        }
    }
    return -1;
}

int register_fd_at(int fd, long handle) {
    if (fd < 0 || fd >= kMaxFd) {
        return -1;
    }
    fdTable[fd] = handle;
    return fd;
}

// --- Directory streams ---------------------------------------------------
// The kernel's readdir syscall is path-based and returns the whole directory
// at once. mlibc's opendir/readdir model wants a handle plus incremental
// ReadEntries calls. We bridge the two with a small table of open directory
// streams: OpenDir records the path, and the first ReadEntries snapshots the
// whole directory into the slot, after which a cursor walks the snapshot.

// Kernel-side directory entry (matches struct DirEntry in the kernel: a
// fixed-size name, a 64-bit inode, and a 32-bit FileType enum).
struct instantos_dirent {
    char name[256];
    unsigned long inode;
    unsigned int type;
};

// FileType enum values from the kernel (include/fs/vfs/vfs.hpp).
enum {
    kFtRegular = 0,
    kFtDirectory = 1,
    kFtCharDevice = 2,
    kFtBlockDevice = 3,
    kFtSymlink = 4,
    kFtPipe = 5,
    kFtSocket = 6,
};

constexpr int kMaxDirs = 32;
constexpr int kMaxDirEntries = 256;
// Directory handles are returned to mlibc as fds (opendir stores them in
// DIR::__handle, closedir calls close() on them). Offset them well above the
// real fd range so Close() can recognise and free them without touching the
// fd table.
constexpr int kDirHandleBase = 0x40000000;

struct DirStream {
    bool inUse = false;
    bool loaded = false;
    int cursor = 0;
    int count = 0;
    char path[256];
    instantos_dirent entries[kMaxDirEntries];
};

// constinit + value-initialization keeps this in .bss with no static
// constructor; see the note on fdTable above.
constinit DirStream dirStreams[kMaxDirs] = {};

int alloc_dir_stream(const char* path) {
    for (int i = 0; i < kMaxDirs; ++i) {
        if (!dirStreams[i].inUse) {
            dirStreams[i].inUse = true;
            dirStreams[i].loaded = false;
            dirStreams[i].cursor = 0;
            dirStreams[i].count = 0;
            size_t j = 0;
            for (; path[j] != '\0' && j + 1 < sizeof(dirStreams[i].path); ++j) {
                dirStreams[i].path[j] = path[j];
            }
            dirStreams[i].path[j] = '\0';
            return i;
        }
    }
    return -1;
}

unsigned char dt_from_filetype(unsigned int type) {
    switch (type) {
        case kFtDirectory: return 4;   // DT_DIR
        case kFtRegular: return 8;     // DT_REG
        case kFtCharDevice: return 2;  // DT_CHR
        case kFtBlockDevice: return 6; // DT_BLK
        case kFtSymlink: return 10;    // DT_LNK
        case kFtPipe: return 1;        // DT_FIFO
        case kFtSocket: return 12;     // DT_SOCK
        default: return 0;             // DT_UNKNOWN
    }
}

}

namespace mlibc {

void Sysdeps<LibcPanic>::operator()() {
    __builtin_trap();
}

void Sysdeps<LibcLog>::operator()(const char* message) {
    size_t length = strlen(message);
    __do_syscall3(INSTANTOS_SYS_WRITE, 2, reinterpret_cast<long>(message), static_cast<long>(length));
    char newline = '\n';
    __do_syscall3(INSTANTOS_SYS_WRITE, 2, reinterpret_cast<long>(&newline), 1);
    // Also mirror to the serial port (SYS_SERIALWRITE=88) so panics/logs from
    // console-less processes (e.g. the rtld loading an init binary) are visible.
    __do_syscall2(INSTANTOS_SYS_SERIALWRITE, reinterpret_cast<long>(message), static_cast<long>(length));
    __do_syscall2(INSTANTOS_SYS_SERIALWRITE, reinterpret_cast<long>(&newline), 1);
}

int Sysdeps<Isatty>::operator()(int fd) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    struct instantos_winsize ws;
    long ret = __do_syscall3(INSTANTOS_SYS_IOCTL, handle, TIOCGWINSZ, reinterpret_cast<long>(&ws));
    if (sc_error(ret)) {
        return ENOTTY;
    }
    return 0;
}

int Sysdeps<Write>::operator()(int fd, const void* buffer, size_t size, ssize_t* written) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    long ret = __do_syscall3(INSTANTOS_SYS_WRITE, handle, reinterpret_cast<long>(buffer), static_cast<long>(size));
    if (int e = sc_error(ret); e) return e;
    *written = ret;
    return 0;
}

int Sysdeps<Read>::operator()(int fd, void* buffer, unsigned long size, long* read) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    long ret = __do_syscall3(INSTANTOS_SYS_READ, handle, reinterpret_cast<long>(buffer), static_cast<long>(size));
    if (int e = sc_error(ret); e) return e;
    *read = ret;
    return 0;
}

int Sysdeps<Open>::operator()(const char* path, int flags, unsigned int mode, int* fd) {
    long ret = __do_syscall3(INSTANTOS_SYS_OPEN, reinterpret_cast<long>(path), flags, mode);
    if (int e = sc_error(ret); e) return e;
    int localFd = register_fd(ret);
    if (localFd < 0) {
        __do_syscall1(INSTANTOS_SYS_CLOSE, ret);
        return EMFILE;
    }
    *fd = localFd;
    return 0;
}

int Sysdeps<Close>::operator()(int fd) {
    // Directory handles (from OpenDir) live in a high range and only need the
    // in-memory stream freed; they have no kernel fd backing them.
    if (fd >= kDirHandleBase) {
        int slot = fd - kDirHandleBase;
        if (slot < 0 || slot >= kMaxDirs || !dirStreams[slot].inUse) return EBADF;
        dirStreams[slot].inUse = false;
        dirStreams[slot].loaded = false;
        return 0;
    }
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    // The standard streams (fd 0/1/2) are backed by the console or an inherited
    // tty; the kernel refuses to close them. Report success rather than EBADF so
    // that libc stdio teardown (e.g. coreutils' close_stdout / fclose(stdout))
    // does not report a spurious "error closing file".
    if (fd < 3) return 0;
    long ret = __do_syscall1(INSTANTOS_SYS_CLOSE, handle);
    if (int e = sc_error(ret); e) return e;
    fdTable[fd] = 0;
    return sc_error(ret);
}

int Sysdeps<Seek>::operator()(int fd, off_t offset, int whence, off_t* new_offset) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    long ret = __do_syscall3(INSTANTOS_SYS_SEEK, handle, static_cast<long>(offset),
                             static_cast<long>(whence));
    if (int e = sc_error(ret); e) return e;
    if (new_offset) *new_offset = static_cast<off_t>(ret);
    return 0;
}

int Sysdeps<GetCwd>::operator()(char* buffer, size_t size) {
    long ret = __do_syscall2(INSTANTOS_SYS_GETCWD, reinterpret_cast<long>(buffer),
                             static_cast<long>(size));
    return sc_error(ret);
}

int Sysdeps<Chdir>::operator()(const char* path) {
    long ret = __do_syscall1(INSTANTOS_SYS_CHDIR, reinterpret_cast<long>(path));
    return sc_error(ret);
}

int Sysdeps<OpenDir>::operator()(const char* path, int* handle) {
    if (!path || !handle) return EINVAL;
    int slot = alloc_dir_stream(path);
    if (slot < 0) return EMFILE;
    *handle = slot + kDirHandleBase;
    return 0;
}

int Sysdeps<ReadEntries>::operator()(int handle, void* buffer, size_t max_size, size_t* bytes_read) {
    int slot = handle - kDirHandleBase;
    if (slot < 0 || slot >= kMaxDirs || !dirStreams[slot].inUse) return EBADF;
    if (!buffer || !bytes_read) return EINVAL;

    DirStream& dir = dirStreams[slot];

    // Snapshot the whole directory on first read.
    if (!dir.loaded) {
        long ret = __do_syscall3(INSTANTOS_SYS_READDIR,
                                 reinterpret_cast<long>(dir.path),
                                 reinterpret_cast<long>(dir.entries),
                                 kMaxDirEntries);
        if (int e = sc_error(ret); e) return e;
        dir.count = static_cast<int>(ret);
        dir.cursor = 0;
        dir.loaded = true;
    }

    // mlibc's struct dirent layout (linux ABI):
    //   ino_t d_ino (8) | off_t d_off (8) | reclen_t d_reclen (2) |
    //   unsigned char d_type (1) | char d_name[256]
    // The fields up to d_name occupy a fixed prefix; d_reclen must include the
    // bytes consumed by this record so mlibc can advance __ent_next.
    const size_t namePrefix = 8 + 8 + 2 + 1;  // offsetof(struct dirent, d_name)
    size_t written = 0;
    uint8_t* out = static_cast<uint8_t*>(buffer);

    while (dir.cursor < dir.count) {
        const instantos_dirent& e = dir.entries[dir.cursor];
        size_t nameLen = strlen(e.name);
        // Record size: prefix + name + NUL, rounded up to 8 for alignment.
        size_t reclen = namePrefix + nameLen + 1;
        reclen = (reclen + 7) & ~static_cast<size_t>(7);

        if (written + reclen > max_size) {
            break;  // No room; return what we have, resume next call.
        }

        uint8_t* rec = out + written;
        unsigned long d_ino = e.inode;
        long long d_off = static_cast<long long>(dir.cursor + 1);
        unsigned short d_reclen = static_cast<unsigned short>(reclen);
        unsigned char d_type = dt_from_filetype(e.type);

        memcpy(rec + 0, &d_ino, 8);
        memcpy(rec + 8, &d_off, 8);
        memcpy(rec + 16, &d_reclen, 2);
        rec[18] = d_type;
        memcpy(rec + namePrefix, e.name, nameLen);
        rec[namePrefix + nameLen] = '\0';
        // Zero any alignment padding tail.
        for (size_t p = namePrefix + nameLen + 1; p < reclen; ++p) {
            rec[p] = 0;
        }

        written += reclen;
        dir.cursor++;
    }

    *bytes_read = written;
    return 0;
}

int Sysdeps<Mkdir>::operator()(const char* path, mode_t mode) {
    long ret = __do_syscall2(INSTANTOS_SYS_MKDIR, reinterpret_cast<long>(path),
                             static_cast<long>(mode));
    return sc_error(ret);
}

int Sysdeps<Rmdir>::operator()(const char* path) {
    long ret = __do_syscall1(INSTANTOS_SYS_RMDIR, reinterpret_cast<long>(path));
    return sc_error(ret);
}

int Sysdeps<Unlinkat>::operator()(int /*dirfd*/, const char* path, int flags) {
    // AT_REMOVEDIR (0x200) selects rmdir semantics; otherwise unlink a file.
    if (flags & 0x200) {
        long ret = __do_syscall1(INSTANTOS_SYS_RMDIR, reinterpret_cast<long>(path));
        return sc_error(ret);
    }
    long ret = __do_syscall1(INSTANTOS_SYS_UNLINK, reinterpret_cast<long>(path));
    return sc_error(ret);
}

int Sysdeps<GetHostname>::operator()(char* buffer, size_t bufsize) {
    // The kernel has no hostname concept; report a fixed name.
    static const char kHostname[] = "instantos";
    if (!buffer || bufsize == 0) return EINVAL;
    size_t i = 0;
    for (; kHostname[i] != '\0' && i + 1 < bufsize; ++i) {
        buffer[i] = kHostname[i];
    }
    buffer[i] = '\0';
    return 0;
}

int Sysdeps<GetPgid>::operator()(int pid, int* pgid) {
    // No process groups in the kernel: a process is its own group leader.
    if (!pgid) return EINVAL;
    if (pid == 0) {
        pid = static_cast<int>(__do_syscall0(INSTANTOS_SYS_GETPID));
    }
    *pgid = pid;
    return 0;
}

int Sysdeps<SetPgid>::operator()(int, int) {
    // The kernel has no process groups; accept and ignore.
    return 0;
}

// Kernel poll descriptor (matches struct PollFD in the kernel).
struct instantos_pollfd {
    long long fd;
    short events;
    short revents;
};

int Sysdeps<Pselect>::operator()(int num_fds, fd_set* read_set, fd_set* write_set,
                                 fd_set* except_set, const struct timespec* timeout,
                                 const sigset_t* /*sigmask*/, int* num_events) {
    constexpr short kPollIn = 0x0001;
    constexpr short kPollOut = 0x0004;

    // Operate on fd_set's raw bit array (fds_bits[128], one bit per fd) to avoid
    // the FD_* macros, which pull in helper functions unavailable in the rtld.
    auto fdIsSet = [](const fd_set* s, int fd) -> bool {
        return s && (s->fds_bits[fd >> 3] & (1u << (fd & 7))) != 0;
    };
    auto fdClearAll = [](fd_set* s) {
        if (!s) return;
        for (unsigned i = 0; i < sizeof(s->fds_bits); ++i) s->fds_bits[i] = 0;
    };
    auto fdSet = [](fd_set* s, int fd) {
        if (s) s->fds_bits[fd >> 3] |= (1u << (fd & 7));
    };

    instantos_pollfd pfds[64];
    int mapFd[64];
    int n = 0;

    for (int fd = 0; fd < num_fds && n < 64; ++fd) {
        short ev = 0;
        if (fdIsSet(read_set, fd)) ev |= kPollIn;
        if (fdIsSet(write_set, fd)) ev |= kPollOut;
        if (!ev) continue;
        long handle = handle_for_fd(fd);
        if (handle < 0) return EBADF;
        pfds[n].fd = handle;
        pfds[n].events = ev;
        pfds[n].revents = 0;
        mapFd[n] = fd;
        ++n;
    }

    bool infinite = (timeout == nullptr);
    long timeoutMs = infinite
        ? -1  // kPollWaitForever in the kernel
        : (timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000);

    long ready = 0;
    if (n > 0) {
        // The kernel poll blocks when timeoutMs != 0 (and -1 blocks forever).
        ready = __do_syscall3(INSTANTOS_SYS_POLL, reinterpret_cast<long>(pfds),
                              static_cast<long>(n), timeoutMs);
        if (int e = sc_error(ready); e) return e;
    }

    int total = 0;
    fdClearAll(read_set);
    fdClearAll(write_set);
    fdClearAll(except_set);
    for (int i = 0; i < n; ++i) {
        bool hit = false;
        if (read_set && (pfds[i].revents & kPollIn)) { fdSet(read_set, mapFd[i]); hit = true; }
        if (write_set && (pfds[i].revents & kPollOut)) { fdSet(write_set, mapFd[i]); hit = true; }
        if (hit) ++total;
    }
    if (num_events) *num_events = total;
    return 0;
}

int Sysdeps<Poll>::operator()(struct pollfd* fds, nfds_t count, int timeout, int* num_events) {
    // Translate mlibc's struct pollfd (int fd) into the kernel's pollfd layout
    // (instantos_pollfd: 64-bit handle + events/revents).  The POLL* event bit
    // values are identical between the Linux ABI mlibc uses and the kernel
    // (POLLIN=0x0001, POLLOUT=0x0004), so events/revents pass through directly.
    if (count == 0) {
        // Nothing to wait on: honour the timeout via the kernel poll with an
        // empty set is not possible, so just report no events.
        if (num_events) *num_events = 0;
        return 0;
    }
    if (count > 64) {
        return EINVAL;
    }

    instantos_pollfd kfds[64];
    for (nfds_t i = 0; i < count; ++i) {
        long handle = handle_for_fd(fds[i].fd);
        // Negative fds are skipped per POSIX; pass an invalid handle so the
        // kernel ignores them rather than failing the whole call.
        kfds[i].fd = (fds[i].fd < 0 || handle < 0) ? -1 : handle;
        kfds[i].events = fds[i].events;
        kfds[i].revents = 0;
    }

    long ready = __do_syscall3(INSTANTOS_SYS_POLL, reinterpret_cast<long>(kfds),
                               static_cast<long>(count), static_cast<long>(timeout));
    if (int e = sc_error(ready); e) return e;

    for (nfds_t i = 0; i < count; ++i) {
        fds[i].revents = kfds[i].revents;
    }
    if (num_events) *num_events = static_cast<int>(ready);
    return 0;
}

namespace {
void translate_stat(const instantos_stat& in, struct stat* out) {
    memset(out, 0, sizeof(*out));
    out->st_dev = in.k_dev;
    out->st_ino = in.k_ino;
    out->st_nlink = in.k_nlink;
    out->st_mode = in.k_mode;
    out->st_uid = in.k_uid;
    out->st_gid = in.k_gid;
    out->st_rdev = in.k_rdev;
    out->st_size = static_cast<off_t>(in.k_size);
    out->st_blksize = static_cast<blksize_t>(in.k_blksize);
    out->st_blocks = static_cast<blkcnt_t>(in.k_blocks);
    out->st_atim.tv_sec = static_cast<time_t>(in.k_atime);
    out->st_mtim.tv_sec = static_cast<time_t>(in.k_mtime);
    out->st_ctim.tv_sec = static_cast<time_t>(in.k_ctime);
}
}

int Sysdeps<Stat>::operator()(fsfd_target fsfdt, int fd, const char* path, int flags,
                              struct stat* statbuf) {
    instantos_stat ks;
    long ret;

    switch (fsfdt) {
        case fsfd_target::path:
            ret = __do_syscall2((flags & AT_SYMLINK_NOFOLLOW) ? INSTANTOS_SYS_LSTAT
                                                              : INSTANTOS_SYS_STAT,
                                reinterpret_cast<long>(path), reinterpret_cast<long>(&ks));
            break;
        case fsfd_target::fd: {
            long handle = handle_for_fd(fd);
            if (handle < 0) return EBADF;
            ret = __do_syscall2(INSTANTOS_SYS_FSTAT, handle, reinterpret_cast<long>(&ks));
            break;
        }
        case fsfd_target::fd_path:
            // Only AT_FDCWD-relative paths are supported; ignore the (cwd) fd.
            ret = __do_syscall2((flags & AT_SYMLINK_NOFOLLOW) ? INSTANTOS_SYS_LSTAT
                                                              : INSTANTOS_SYS_STAT,
                                reinterpret_cast<long>(path), reinterpret_cast<long>(&ks));
            break;
        default:
            return EINVAL;
    }

    if (int e = sc_error(ret); e) return e;
    translate_stat(ks, statbuf);
    return 0;
}

int Sysdeps<Fcntl>::operator()(int fd, int request, va_list args, int* result) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    long arg = va_arg(args, long);
    long ret = __do_syscall3(INSTANTOS_SYS_FCNTL, handle, request, arg);
    if (int e = sc_error(ret); e) return e;

    // F_DUPFD / F_DUPFD_CLOEXEC return a kernel handle that must be mapped to a
    // new userspace fd; other commands return a plain value.
    if (request == F_DUPFD || request == F_DUPFD_CLOEXEC) {
        int newfd = register_fd(ret);
        if (newfd < 0) {
            __do_syscall1(INSTANTOS_SYS_CLOSE, ret);
            return EMFILE;
        }
        if (result) *result = newfd;
        return 0;
    }

    if (result) *result = static_cast<int>(ret);
    return 0;
}

[[noreturn]] void Sysdeps<Exit>::operator()(int status) {
    __do_syscall1(INSTANTOS_SYS_EXIT, status);
    __builtin_unreachable();
}

pid_t Sysdeps<GetPid>::operator()() {
    return __do_syscall0(INSTANTOS_SYS_GETPID);
}

pid_t Sysdeps<GetPpid>::operator()() {
    return __do_syscall0(INSTANTOS_SYS_GETPPID);
}

uid_t Sysdeps<GetUid>::operator()() {
    return static_cast<uid_t>(__do_syscall0(INSTANTOS_SYS_GETUID));
}

uid_t Sysdeps<GetEuid>::operator()() {
    // The kernel does not track a separate effective UID.
    return static_cast<uid_t>(__do_syscall0(INSTANTOS_SYS_GETUID));
}

gid_t Sysdeps<GetGid>::operator()() {
    return static_cast<gid_t>(__do_syscall0(INSTANTOS_SYS_GETGID));
}

gid_t Sysdeps<GetEgid>::operator()() {
    // The kernel does not track a separate effective GID.
    return static_cast<gid_t>(__do_syscall0(INSTANTOS_SYS_GETGID));
}

int Sysdeps<TcbSet>::operator()(void* pointer) {
    long ret = __do_syscall1(INSTANTOS_SYS_SET_THREAD_POINTER, reinterpret_cast<long>(pointer));
    return sc_error(ret);
}

int Sysdeps<AnonAllocate>::operator()(size_t size, void** pointer) {
    return sysdep<VmMap>(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, pointer);
}

int Sysdeps<AnonFree>::operator()(void* pointer, size_t size) {
    return sysdep<VmUnmap>(pointer, size);
}

int Sysdeps<VmMap>::operator()(void* hint, size_t size, int prot, int, int, off_t, void** window) {
    long ret = __do_syscall3(INSTANTOS_SYS_MMAP, reinterpret_cast<long>(hint), static_cast<long>(size), prot);
    if (int e = sc_error(ret); e) return e;
    *window = reinterpret_cast<void*>(ret);
    return 0;
}

int Sysdeps<VmUnmap>::operator()(void* pointer, size_t size) {
    long ret = __do_syscall2(INSTANTOS_SYS_MUNMAP, reinterpret_cast<long>(pointer), static_cast<long>(size));
    return sc_error(ret);
}

int Sysdeps<VmProtect>::operator()(void* pointer, size_t size, int prot) {
    long ret = __do_syscall3(INSTANTOS_SYS_MPROTECT, reinterpret_cast<long>(pointer),
                             static_cast<long>(size), prot);
    return sc_error(ret);
}

int Sysdeps<ClockGet>::operator()(int clock, time_t* secs, long* nanos) {
    // CLOCK_REALTIME (0) and CLOCK_TAI must be wall-clock time (seconds since
    // the Unix epoch) so time()/clock_gettime() and tools like uptime/date work.
    // CLOCK_MONOTONIC (1) / _BOOTTIME use the millisecond uptime counter.
    constexpr int kClockMonotonic = 1;
    constexpr int kClockMonotonicRaw = 4;
    constexpr int kClockBoottime = 7;
    if (clock == kClockMonotonic || clock == kClockMonotonicRaw || clock == kClockBoottime) {
        long ms = __do_syscall0(INSTANTOS_SYS_GETTIME);
        if (int e = sc_error(ms); e) return e;
        *secs = ms / 1000;
        *nanos = (ms % 1000) * 1000000;
        return 0;
    }
    // Provide sub-second (millisecond) precision for the wall clock. The kernel
    // derives unix time as boot_unix_time + uptime_ms/1000, so the fractional
    // part of "now" within the current second is exactly uptime_ms % 1000.
    // Without this, gettimeofday() has 1-second granularity, which breaks any
    // code that schedules sub-second timers (e.g. NetSurf's fetch scheduler,
    // which polls every 10ms and otherwise stalls a full second per step).
    //
    // Read the millisecond uptime first, then the whole-second unix time, so if
    // a second boundary is crossed between the two reads the seconds value is
    // the newer (>=) one; this keeps the clock from ever jumping backwards.
    long ms = __do_syscall0(INSTANTOS_SYS_GETTIME);
    long unixt = __do_syscall0(INSTANTOS_SYS_GETUNIXTIME);
    if (int e = sc_error(unixt); e) return e;
    *secs = unixt;
    *nanos = (sc_error(ms) == 0) ? (ms % 1000) * 1000000 : 0;
    return 0;
}

void Sysdeps<Yield>::operator()() {
    __do_syscall0(INSTANTOS_SYS_YIELD);
}

int Sysdeps<Sleep>::operator()(time_t* secs, long* nanos) {
    long ms = (*secs * 1000) + (*nanos / 1000000);
    long ret = __do_syscall1(INSTANTOS_SYS_SLEEP, ms);
    if (int e = sc_error(ret); e) return e;
    *secs = 0;
    *nanos = 0;
    return 0;
}

int Sysdeps<FutexWait>::operator()(int*, int, const timespec*) {
    sysdep<Yield>();
    return 0;
}

int Sysdeps<FutexWake>::operator()(int*, bool) {
    return 0;
}

int Sysdeps<Ioctl>::operator()(int fd, unsigned long request, void* arg, int* result) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    long ret = __do_syscall3(INSTANTOS_SYS_IOCTL, handle, static_cast<long>(request),
                             reinterpret_cast<long>(arg));
    if (int e = sc_error(ret); e) return e;
    if (result) *result = static_cast<int>(ret);
    return 0;
}

int Sysdeps<Tcgetattr>::operator()(int fd, struct termios* attr) {
    int result = 0;
    return sysdep<Ioctl>(fd, TCGETS, attr, &result);
}

int Sysdeps<Tcsetattr>::operator()(int fd, int optional_action, const struct termios* attr) {
    unsigned long request = TCSETS;
    if (optional_action == TCSADRAIN) request = TCSETSW;
    else if (optional_action == TCSAFLUSH) request = TCSETSF;
    int result = 0;
    return sysdep<Ioctl>(fd, request, const_cast<struct termios*>(attr), &result);
}

int Sysdeps<Sigprocmask>::operator()(int how, const sigset_t* set, sigset_t* retrieve) {
    // The kernel models the blocked set as a single 64-bit mask; mlibc's
    // sigset_t is an array of unsigned long. Only the first word carries the
    // signals the kernel supports.
    unsigned long kset = set ? set->__sig[0] : 0;
    unsigned long kold = 0;
    long ret = __do_syscall3(INSTANTOS_SYS_SIGPROCMASK, how,
                             set ? reinterpret_cast<long>(&kset) : 0,
                             retrieve ? reinterpret_cast<long>(&kold) : 0);
    if (int e = sc_error(ret); e) return e;
    if (retrieve) {
        for (auto& word : retrieve->__sig) word = 0;
        retrieve->__sig[0] = kold;
    }
    return 0;
}

int Sysdeps<Sigaction>::operator()(int signum, const struct sigaction* __restrict act,
                                   struct sigaction* __restrict oldact) {
    instantos_sigaction kact;
    instantos_sigaction kold;

    if (act) {
        kact.handler = reinterpret_cast<unsigned long>(act->sa_handler);
        kact.mask = act->sa_mask.__sig[0];
        kact.flags = act->sa_flags;
        kact.restorer = reinterpret_cast<unsigned long>(act->sa_restorer);
    }

    long ret = __do_syscall3(INSTANTOS_SYS_SIGACTION, signum,
                             act ? reinterpret_cast<long>(&kact) : 0,
                             oldact ? reinterpret_cast<long>(&kold) : 0);
    if (int e = sc_error(ret); e) return e;

    if (oldact) {
        for (auto& word : oldact->sa_mask.__sig) word = 0;
        oldact->sa_handler = reinterpret_cast<void (*)(int)>(kold.handler);
        oldact->sa_mask.__sig[0] = kold.mask;
        oldact->sa_flags = kold.flags;
        oldact->sa_restorer = reinterpret_cast<void (*)(void)>(kold.restorer);
    }
    return 0;
}

int Sysdeps<Dup>::operator()(int fd, int, int* newfd) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    long ret = __do_syscall1(INSTANTOS_SYS_DUP, handle);
    if (int e = sc_error(ret); e) return e;
    int local = register_fd(ret);
    if (local < 0) {
        __do_syscall1(INSTANTOS_SYS_CLOSE, ret);
        return EMFILE;
    }
    *newfd = local;
    return 0;
}

int Sysdeps<Dup2>::operator()(int fd, int, int newfd) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    if (newfd < 0 || newfd >= kMaxFd) return EBADF;

    // Determine the kernel target handle for newfd. For stdio slots the kernel
    // accepts the encoded File slot; otherwise allocate against the existing.
    long targetHandle = fdTable[newfd];
    long ret;
    if (targetHandle != 0 || newfd < 3) {
        ret = __do_syscall2(INSTANTOS_SYS_DUP2, handle, targetHandle ? targetHandle : newfd);
    } else {
        ret = __do_syscall1(INSTANTOS_SYS_DUP, handle);
    }
    if (int e = sc_error(ret); e) return e;
    register_fd_at(newfd, ret);
    return 0;
}

int Sysdeps<Pipe>::operator()(int* fds, int) {
    long handles[2];
    long ret = __do_syscall1(INSTANTOS_SYS_PIPE, reinterpret_cast<long>(handles));
    if (int e = sc_error(ret); e) return e;
    int r = register_fd(handles[0]);
    int w = register_fd(handles[1]);
    if (r < 0 || w < 0) {
        if (r >= 0) { __do_syscall1(INSTANTOS_SYS_CLOSE, handles[0]); fdTable[r] = 0; }
        if (w >= 0) { __do_syscall1(INSTANTOS_SYS_CLOSE, handles[1]); fdTable[w] = 0; }
        return EMFILE;
    }
    fds[0] = r;
    fds[1] = w;
    return 0;
}

int Sysdeps<Openat>::operator()(int, const char* path, int flags, mode_t mode, int* fd) {
    // No *at semantics yet; ignore dirfd and treat path as absolute/cwd-relative.
    long ret = __do_syscall3(INSTANTOS_SYS_OPEN, reinterpret_cast<long>(path), flags, mode);
    if (int e = sc_error(ret); e) return e;
    int localFd = register_fd(ret);
    if (localFd < 0) {
        __do_syscall1(INSTANTOS_SYS_CLOSE, ret);
        return EMFILE;
    }
    *fd = localFd;
    return 0;
}

int Sysdeps<Fork>::operator()(pid_t* child) {
    long ret = __do_syscall0(INSTANTOS_SYS_FORK);
    if (int e = sc_error(ret); e) return e;
    // Parent gets the child pid; child gets 0. The fd table lives in copied
    // memory so the child inherits it automatically.
    *child = static_cast<pid_t>(ret);
    return 0;
}

int Sysdeps<Waitpid>::operator()(pid_t pid, int* status, int flags, struct rusage*, pid_t* ret_pid) {
    long ret = __do_syscall3(INSTANTOS_SYS_WAIT, pid, reinterpret_cast<long>(status), flags);
    if (int e = sc_error(ret); e) return e;
    *ret_pid = static_cast<pid_t>(ret);
    return 0;
}

int Sysdeps<Execve>::operator()(const char* path, char* const argv[], char* const envp[]) {
    long ret = __do_syscall3(INSTANTOS_SYS_EXEC, reinterpret_cast<long>(path),
                             reinterpret_cast<long>(argv), reinterpret_cast<long>(envp));
    // Only returns on failure.
    return sc_error(ret);
}

// --- coreutils Tier 1: thin wrappers over existing kernel syscalls ---------

int Sysdeps<Kill>::operator()(pid_t pid, int sig) {
    long ret = __do_syscall2(INSTANTOS_SYS_KILL, pid, sig);
    return sc_error(ret);
}

int Sysdeps<Chmod>::operator()(const char* pathname, mode_t mode) {
    // sys_chmod(target, mode, byHandle=0 -> path)
    long ret = __do_syscall3(INSTANTOS_SYS_CHMOD, reinterpret_cast<long>(pathname),
                             static_cast<long>(mode), 0);
    return sc_error(ret);
}

int Sysdeps<Fchmod>::operator()(int fd, mode_t mode) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    // sys_chmod(target=handle, mode, byHandle=1)
    long ret = __do_syscall3(INSTANTOS_SYS_CHMOD, handle, static_cast<long>(mode), 1);
    return sc_error(ret);
}

int Sysdeps<Truncate>::operator()(const char* path, off_t length) {
    // sys_truncate(target, size, byHandle=0 -> path)
    long ret = __do_syscall3(INSTANTOS_SYS_TRUNCATE, reinterpret_cast<long>(path),
                             static_cast<long>(length), 0);
    return sc_error(ret);
}

int Sysdeps<Ftruncate>::operator()(int fd, size_t size) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    // sys_truncate(target=handle, size, byHandle=1)
    long ret = __do_syscall3(INSTANTOS_SYS_TRUNCATE, handle, static_cast<long>(size), 1);
    return sc_error(ret);
}

int Sysdeps<Rename>::operator()(const char* path, const char* new_path) {
    long ret = __do_syscall2(INSTANTOS_SYS_RENAME, reinterpret_cast<long>(path),
                             reinterpret_cast<long>(new_path));
    return sc_error(ret);
}

int Sysdeps<Renameat>::operator()(int /*olddirfd*/, const char* old_path,
                                  int /*newdirfd*/, const char* new_path) {
    // Only AT_FDCWD-relative paths are supported; ignore dirfds. mlibc's
    // rename() routes through Renameat, so this is the path coreutils' mv uses.
    long ret = __do_syscall2(INSTANTOS_SYS_RENAME, reinterpret_cast<long>(old_path),
                             reinterpret_cast<long>(new_path));
    return sc_error(ret);
}

// --- *at variants: coreutils invokes these instead of the base calls -------
// Each ignores dirfd (AT_FDCWD-relative only) and delegates to the existing
// kernel syscall.

int Sysdeps<Fchmodat>::operator()(int /*dirfd*/, const char* pathname, mode_t mode, int /*flags*/) {
    long ret = __do_syscall3(INSTANTOS_SYS_CHMOD, reinterpret_cast<long>(pathname),
                             static_cast<long>(mode), 0);
    return sc_error(ret);
}

int Sysdeps<Linkat>::operator()(int /*olddirfd*/, const char* old_path,
                                int /*newdirfd*/, const char* new_path, int /*flags*/) {
    long ret = __do_syscall2(INSTANTOS_SYS_LINK, reinterpret_cast<long>(old_path),
                             reinterpret_cast<long>(new_path));
    return sc_error(ret);
}

int Sysdeps<Symlinkat>::operator()(const char* target_path, int /*dirfd*/, const char* link_path) {
    long ret = __do_syscall2(INSTANTOS_SYS_SYMLINK, reinterpret_cast<long>(target_path),
                             reinterpret_cast<long>(link_path));
    return sc_error(ret);
}

int Sysdeps<Readlinkat>::operator()(int /*dirfd*/, const char* path, void* buffer,
                                    size_t max_size, ssize_t* length) {
    long ret = __do_syscall3(INSTANTOS_SYS_READLINK, reinterpret_cast<long>(path),
                             reinterpret_cast<long>(buffer), static_cast<long>(max_size));
    if (int e = sc_error(ret); e) return e;
    *length = ret;
    return 0;
}

int Sysdeps<Umask>::operator()(mode_t mode, mode_t* old) {
    // The kernel has no umask concept; track it per-process in libc state. This
    // is the correct home for umask (a process-local attribute) and lets tools
    // like install/mkdir -m query and set it.
    static mode_t current = 022;
    if (old) *old = current;
    current = mode & 0777;
    return 0;
}

namespace {
void fill_statvfs(const instantos_statfs& ks, struct statvfs* out) {
    memset(out, 0, sizeof(*out));
    out->f_bsize = ks.blockSize;
    out->f_frsize = ks.blockSize;
    out->f_blocks = ks.totalBlocks;
    out->f_bfree = ks.freeBlocks;
    out->f_bavail = ks.freeBlocks;
    out->f_files = ks.totalInodes;
    out->f_ffree = ks.freeInodes;
    out->f_favail = ks.freeInodes;
    out->f_fsid = 0;
    out->f_flag = 0;
    out->f_namemax = ks.nameMax;
    out->f_type = ks.fsType;
}
}  // namespace

int Sysdeps<Statvfs>::operator()(const char* path, struct statvfs* out) {
    instantos_statfs ks;
    // sys_statfs(target=path, byHandle=0, statbuf)
    long ret = __do_syscall3(INSTANTOS_SYS_STATFS, reinterpret_cast<long>(path), 0,
                             reinterpret_cast<long>(&ks));
    if (int e = sc_error(ret); e) return e;
    fill_statvfs(ks, out);
    return 0;
}

int Sysdeps<Fstatvfs>::operator()(int fd, struct statvfs* out) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    instantos_statfs ks;
    // sys_statfs(target=handle, byHandle=1, statbuf)
    long ret = __do_syscall3(INSTANTOS_SYS_STATFS, handle, 1, reinterpret_cast<long>(&ks));
    if (int e = sc_error(ret); e) return e;
    fill_statvfs(ks, out);
    return 0;
}

// chown/lchown/fchown/fchownat all route through Fchownat in mlibc:
//   chown(path)     -> Fchownat(AT_FDCWD, path, ..., 0)
//   lchown(path)    -> Fchownat(AT_FDCWD, path, ..., AT_SYMLINK_NOFOLLOW)
//   fchown(fd)      -> Fchownat(fd, "", ..., AT_EMPTY_PATH)
//   fchownat(...)   -> Fchownat(dirfd, path, ..., flags)
int Sysdeps<Fchownat>::operator()(int dirfd, const char* pathname, uid_t owner, gid_t group, int flags) {
    constexpr int kAtEmptyPath = 0x1000;
    if (flags & kAtEmptyPath) {
        // Operate on the open fd itself (fchown).
        long handle = handle_for_fd(dirfd);
        if (handle < 0) return EBADF;
        // sys_chown(target=handle, byHandle=1, uid, gid, flags)
        long ret = __do_syscall5(INSTANTOS_SYS_CHOWN, handle, 1,
                                 static_cast<long>(owner), static_cast<long>(group),
                                 static_cast<long>(flags));
        return sc_error(ret);
    }
    // Path-based (AT_FDCWD only). flags may carry AT_SYMLINK_NOFOLLOW for lchown.
    long ret = __do_syscall5(INSTANTOS_SYS_CHOWN, reinterpret_cast<long>(pathname), 0,
                             static_cast<long>(owner), static_cast<long>(group),
                             static_cast<long>(flags));
    return sc_error(ret);
}

int Sysdeps<Mknodat>::operator()(int /*dirfd*/, const char* path, int mode, int dev) {
    // Only AT_FDCWD-relative paths are supported; ignore dirfd. The kernel
    // sys_mknod selects the node type from the S_IFMT bits in `mode`.
    long ret = __do_syscall3(INSTANTOS_SYS_MKNOD, reinterpret_cast<long>(path),
                             static_cast<long>(static_cast<unsigned>(mode)),
                             static_cast<long>(static_cast<unsigned>(dev)));
    return sc_error(ret);
}

int Sysdeps<Mkfifoat>::operator()(int /*dirfd*/, const char* path, mode_t mode) {
    // mkfifo always creates a FIFO: force the S_IFIFO type bit.
    constexpr unsigned kSIFIFO = 0x1000;
    long ret = __do_syscall3(INSTANTOS_SYS_MKNOD, reinterpret_cast<long>(path),
                             static_cast<long>((static_cast<unsigned>(mode) & 07777) | kSIFIFO),
                             0);
    return sc_error(ret);
}

int Sysdeps<GetEntropy>::operator()(void* buffer, size_t length) {
    // getentropy()/getrandom() route here. The kernel fills the buffer with
    // up to 256 bytes per call; loop for longer requests.
    size_t done = 0;
    while (done < length) {
        size_t chunk = (length - done) < 256 ? (length - done) : 256;
        long ret = __do_syscall2(INSTANTOS_SYS_GETENTROPY,
                                 reinterpret_cast<long>(static_cast<char*>(buffer) + done),
                                 static_cast<long>(chunk));
        if (int e = sc_error(ret); e) return e;
        done += chunk;
    }
    return 0;
}

// --- Process limits / credentials / timing (cheap, mostly fixed values) ----

int Sysdeps<GetRlimit>::operator()(int resource, struct rlimit* limit) {
    if (!limit) return EINVAL;
    // The kernel enforces no per-process resource limits; report generous,
    // effectively-unlimited values so tools (e.g. sort sizing its buffers) get
    // sensible answers. RLIMIT_NOFILE matches the userspace fd table cap.
    limit->rlim_cur = RLIM_INFINITY;
    limit->rlim_max = RLIM_INFINITY;
    if (resource == RLIMIT_NOFILE) {
        limit->rlim_cur = 256;
        limit->rlim_max = 256;
    }
    return 0;
}

int Sysdeps<SetRlimit>::operator()(int /*resource*/, const struct rlimit* /*limit*/) {
    // No enforced limits to change; accept silently.
    return 0;
}

int Sysdeps<Times>::operator()(struct tms* tms, clock_t* out) {
    // No per-process CPU accounting yet. Report zero CPU time and the
    // monotonic millisecond clock as the "elapsed" tick return (times() returns
    // a clock_t of elapsed real time).
    if (tms) {
        tms->tms_utime = 0;
        tms->tms_stime = 0;
        tms->tms_cutime = 0;
        tms->tms_cstime = 0;
    }
    long ms = __do_syscall0(INSTANTOS_SYS_GETTIME);
    if (out) *out = static_cast<clock_t>(ms);
    return 0;
}

int Sysdeps<ClockSet>::operator()(int /*clock*/, time_t /*secs*/, long /*nanos*/) {
    // The RTC is not settable from here; accept as a no-op so `date -s` does
    // not hard-fail. (The wall clock is unchanged.)
    return 0;
}

int Sysdeps<ClockGetres>::operator()(int /*clock*/, time_t* secs, long* nanos) {
    // The kernel clock advances in milliseconds.
    if (secs) *secs = 0;
    if (nanos) *nanos = 1000000;  // 1 ms
    return 0;
}

int Sysdeps<GetSid>::operator()(pid_t /*pid*/, pid_t* sid) {
    if (!sid) return EINVAL;
    *sid = static_cast<pid_t>(__do_syscall0(INSTANTOS_SYS_GETSESSIONID));
    return 0;
}

int Sysdeps<GetGroups>::operator()(size_t size, gid_t* list, int* ret) {
    // No supplementary groups; the process belongs to its primary gid only.
    gid_t gid = static_cast<gid_t>(__do_syscall0(INSTANTOS_SYS_GETGID));
    if (size == 0) {
        if (ret) *ret = 1;  // number of groups available
        return 0;
    }
    if (!list || size < 1) return EINVAL;
    list[0] = gid;
    if (ret) *ret = 1;
    return 0;
}

int Sysdeps<GetResuid>::operator()(uid_t* ruid, uid_t* euid, uid_t* suid) {
    uid_t uid = static_cast<uid_t>(__do_syscall0(INSTANTOS_SYS_GETUID));
    if (ruid) *ruid = uid;
    if (euid) *euid = uid;  // kernel tracks no separate effective/saved uid
    if (suid) *suid = uid;
    return 0;
}

int Sysdeps<GetResgid>::operator()(gid_t* rgid, gid_t* egid, gid_t* sgid) {
    gid_t gid = static_cast<gid_t>(__do_syscall0(INSTANTOS_SYS_GETGID));
    if (rgid) *rgid = gid;
    if (egid) *egid = gid;
    if (sgid) *sgid = gid;
    return 0;
}

int Sysdeps<GetLoadavg>::operator()(double* samples) {
    // No load-average accounting in the kernel; report 0.00 across the board.
    // (getloadavg() callers like `uptime` just print these values.)
    if (samples) {
        samples[0] = 0.0;
        samples[1] = 0.0;
        samples[2] = 0.0;
    }
    return 0;
}

int Sysdeps<Fadvise>::operator()(int /*fd*/, off_t /*offset*/, off_t /*length*/, int /*advice*/) {
    // posix_fadvise() is purely advisory; accepting it as a no-op is correct.
    return 0;
}

int Sysdeps<Link>::operator()(const char* old_path, const char* new_path) {
    long ret = __do_syscall2(INSTANTOS_SYS_LINK, reinterpret_cast<long>(old_path),
                             reinterpret_cast<long>(new_path));
    return sc_error(ret);
}

int Sysdeps<Symlink>::operator()(const char* target_path, const char* link_path) {
    long ret = __do_syscall2(INSTANTOS_SYS_SYMLINK, reinterpret_cast<long>(target_path),
                             reinterpret_cast<long>(link_path));
    return sc_error(ret);
}

int Sysdeps<Readlink>::operator()(const char* path, void* buffer, size_t max_size, ssize_t* length) {
    long ret = __do_syscall3(INSTANTOS_SYS_READLINK, reinterpret_cast<long>(path),
                             reinterpret_cast<long>(buffer), static_cast<long>(max_size));
    if (int e = sc_error(ret); e) return e;
    *length = ret;  // kernel returns bytes copied (no NUL terminator)
    return 0;
}

int Sysdeps<Fchdir>::operator()(int /*fd*/) {
    // The kernel has no fd->path lookup, and chdir is path-based. mlibc only
    // needs this for fchdir(); report ENOSYS so callers fall back to a
    // path-based chdir where possible.
    return ENOSYS;
}

int Sysdeps<Utimensat>::operator()(int dirfd, const char* pathname,
                                   const struct timespec times[2], int flags) {
    // The kernel Utime takes second-resolution atime/mtime and has no OMIT
    // concept, so resolve UTIME_NOW/UTIME_OMIT (and a null times) here.
    // Only AT_FDCWD-relative paths are supported.
    (void)dirfd;
    (void)flags;

    long now = __do_syscall0(INSTANTOS_SYS_GETUNIXTIME);
    long atime = now;
    long mtime = now;

    bool needExisting = false;
    if (times) {
        // Decide each field; UTIME_OMIT requires the current on-disk value.
        if (times[0].tv_nsec == UTIME_OMIT || times[1].tv_nsec == UTIME_OMIT) {
            needExisting = true;
        }
    }

    long existAtime = now;
    long existMtime = now;
    if (needExisting && pathname) {
        instantos_stat ks;
        long sret = __do_syscall2((flags & AT_SYMLINK_NOFOLLOW) ? INSTANTOS_SYS_LSTAT
                                                                : INSTANTOS_SYS_STAT,
                                  reinterpret_cast<long>(pathname), reinterpret_cast<long>(&ks));
        if (!sc_error(sret)) {
            existAtime = static_cast<long>(ks.k_atime);
            existMtime = static_cast<long>(ks.k_mtime);
        }
    }

    if (times) {
        if (times[0].tv_nsec == UTIME_OMIT) {
            atime = existAtime;
        } else if (times[0].tv_nsec != UTIME_NOW) {
            atime = static_cast<long>(times[0].tv_sec);
        }
        if (times[1].tv_nsec == UTIME_OMIT) {
            mtime = existMtime;
        } else if (times[1].tv_nsec != UTIME_NOW) {
            mtime = static_cast<long>(times[1].tv_sec);
        }
    }

    // sys_utime(target=path, atime, mtime, byHandle=0)
    long ret = __do_syscall4(INSTANTOS_SYS_UTIME, reinterpret_cast<long>(pathname),
                             atime, mtime, 0);
    return sc_error(ret);
}

int Sysdeps<Access>::operator()(const char* path, int mode) {
    // sys_access(path, mode) — mode bits F_OK=0, X_OK=1, W_OK=2, R_OK=4.
    long ret = __do_syscall2(INSTANTOS_SYS_ACCESS, reinterpret_cast<long>(path),
                             static_cast<long>(mode));
    return sc_error(ret);
}

int Sysdeps<Faccessat>::operator()(int dirfd, const char* pathname, int mode, int flags) {
    // Only AT_FDCWD-relative paths are supported; ignore dirfd and AT_* flags
    // (AT_EACCESS is moot under the single-credential model).
    (void)dirfd;
    (void)flags;
    long ret = __do_syscall2(INSTANTOS_SYS_ACCESS, reinterpret_cast<long>(pathname),
                             static_cast<long>(mode));
    return sc_error(ret);
}

// --- Sockets -------------------------------------------------------------
// The kernel exposes a BSD-style socket API (see src/cpu/syscall/socket.cpp).
// Socket handles are ordinary kernel object handles, so they flow through the
// same fdTable used for files: a successful socket()/accept() returns a kernel
// handle which we register as a small integer fd.  The kernel address layout
// is a verbatim Linux `struct sockaddr_in` (AF_INET == 2, port and address in
// network byte order), so addresses pass through untouched.
//
// The kernel send/recv only operate on a single flat buffer; we implement the
// scatter/gather MsgSend/MsgRecv on top of per-iovec send/recv calls.

int Sysdeps<Socket>::operator()(int family, int type, int protocol, int* fd) {
    long ret = __do_syscall3(INSTANTOS_SYS_SOCKET,
                             static_cast<long>(family),
                             static_cast<long>(type),
                             static_cast<long>(protocol));
    if (int e = sc_error(ret); e) return e;
    int localFd = register_fd(ret);
    if (localFd < 0) {
        __do_syscall1(INSTANTOS_SYS_CLOSE, ret);
        return EMFILE;
    }
    *fd = localFd;
    return 0;
}

int Sysdeps<Bind>::operator()(int fd, const struct sockaddr* addr_ptr, socklen_t addr_length) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    long ret = __do_syscall3(INSTANTOS_SYS_BIND, handle,
                             reinterpret_cast<long>(addr_ptr),
                             static_cast<long>(addr_length));
    return sc_error(ret);
}

int Sysdeps<Connect>::operator()(int fd, const struct sockaddr* addr_ptr, socklen_t addr_length) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    long ret = __do_syscall3(INSTANTOS_SYS_CONNECT, handle,
                             reinterpret_cast<long>(addr_ptr),
                             static_cast<long>(addr_length));
    int err = sc_error(ret);
    if (err != EAGAIN) {
        // Either established (0) or a hard failure; report it faithfully.
        return err;
    }

    // The kernel sent the SYN and reports EAGAIN while the TCP handshake is in
    // progress (it is driven forward by the userspace packet pump). POSIX
    // distinguishes by the socket's blocking mode:
    //   - non-blocking sockets must get EINPROGRESS and let the caller poll
    //     for POLLOUT (this is exactly what libcurl's multi interface does);
    //   - blocking sockets must wait here until the handshake completes.
    long flags = __do_syscall3(INSTANTOS_SYS_FCNTL, handle, F_GETFL, 0);
    bool nonblock = (sc_error(flags) == 0) && (flags & O_NONBLOCK);
    if (nonblock) {
        return EINPROGRESS;
    }

    // Blocking connect: wait for the socket to become writable (handshake
    // established) or error out.
    for (;;) {
        instantos_pollfd pfd;
        pfd.fd = handle;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        long ready = __do_syscall3(INSTANTOS_SYS_POLL,
                                   reinterpret_cast<long>(&pfd), 1, -1);
        if (int e = sc_error(ready); e) {
            if (e == EINTR) continue;
            return e;
        }
        if (pfd.revents & (POLLERR | POLLHUP)) {
            // Surface the queued socket error (SO_ERROR) if any.
            int soerr = 0;
            socklen_t len = sizeof(soerr);
            long g = __do_syscall5(INSTANTOS_SYS_GETSOCKOPT, handle,
                                   SOL_SOCKET, SO_ERROR,
                                   reinterpret_cast<long>(&soerr),
                                   reinterpret_cast<long>(&len));
            if (sc_error(g) == 0 && soerr != 0) return soerr;
            return ECONNREFUSED;
        }
        if (pfd.revents & POLLOUT) {
            return 0;
        }
    }
}

int Sysdeps<Listen>::operator()(int fd, int backlog) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    long ret = __do_syscall2(INSTANTOS_SYS_LISTEN, handle, static_cast<long>(backlog));
    return sc_error(ret);
}

int Sysdeps<Accept>::operator()(int fd, int* newfd, struct sockaddr* addr_ptr,
                                socklen_t* addr_length, int flags) {
    (void)flags;
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;

    // The kernel reads a 32-bit capacity from *addr_length on input and writes
    // back the actual length there.  mlibc passes socklen_t (also 32-bit), so
    // the pointer can be handed through directly.
    long ret = __do_syscall3(INSTANTOS_SYS_ACCEPT, handle,
                             reinterpret_cast<long>(addr_ptr),
                             reinterpret_cast<long>(addr_length));
    if (int e = sc_error(ret); e) return e;

    int localFd = register_fd(ret);
    if (localFd < 0) {
        __do_syscall1(INSTANTOS_SYS_CLOSE, ret);
        return EMFILE;
    }
    *newfd = localFd;
    return 0;
}

int Sysdeps<Sendto>::operator()(int fd, const void* buffer, size_t size, int flags,
                                const struct sockaddr* sock_addr, socklen_t addr_length,
                                ssize_t* length) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;

    // For a connectionless datagram socket the destination is supplied per
    // call; the kernel has no separate sendto, so connect() the peer first
    // (matches the native ilibcxx sendto behaviour).
    if (sock_addr != nullptr) {
        long c = __do_syscall3(INSTANTOS_SYS_CONNECT, handle,
                               reinterpret_cast<long>(sock_addr),
                               static_cast<long>(addr_length));
        if (int e = sc_error(c); e && e != EAGAIN) return e;
    }

    long ret = __do_syscall4(INSTANTOS_SYS_SEND, handle,
                             reinterpret_cast<long>(buffer),
                             static_cast<long>(size),
                             static_cast<long>(flags));
    if (int e = sc_error(ret); e) return e;
    if (length) *length = ret;
    return 0;
}

int Sysdeps<Recvfrom>::operator()(int fd, void* buffer, size_t size, int flags,
                                  struct sockaddr* sock_addr, socklen_t* addr_length,
                                  ssize_t* length) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;

    long ret = __do_syscall4(INSTANTOS_SYS_RECV, handle,
                             reinterpret_cast<long>(buffer),
                             static_cast<long>(size),
                             static_cast<long>(flags));
    if (int e = sc_error(ret); e) return e;
    if (length) *length = ret;
    // The kernel recv does not report the source address; signal "none".
    (void)sock_addr;
    if (addr_length) *addr_length = 0;
    return 0;
}

int Sysdeps<MsgSend>::operator()(int fd, const struct msghdr* hdr, int flags, ssize_t* length) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;

    // Optional per-message destination address (datagram sockets).
    if (hdr->msg_name != nullptr && hdr->msg_namelen != 0) {
        long c = __do_syscall3(INSTANTOS_SYS_CONNECT, handle,
                               reinterpret_cast<long>(hdr->msg_name),
                               static_cast<long>(hdr->msg_namelen));
        if (int e = sc_error(c); e && e != EAGAIN) return e;
    }

    ssize_t total = 0;
    for (int i = 0; i < hdr->msg_iovlen; ++i) {
        const struct iovec* iov = &hdr->msg_iov[i];
        if (iov->iov_len == 0) continue;
        long ret = __do_syscall4(INSTANTOS_SYS_SEND, handle,
                                 reinterpret_cast<long>(iov->iov_base),
                                 static_cast<long>(iov->iov_len),
                                 static_cast<long>(flags));
        if (int e = sc_error(ret); e) {
            if (total > 0) break;  // partial progress already made
            return e;
        }
        total += ret;
        if (static_cast<size_t>(ret) < iov->iov_len) break;  // short write
    }
    if (length) *length = total;
    return 0;
}

int Sysdeps<MsgRecv>::operator()(int fd, struct msghdr* hdr, int flags, ssize_t* length) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;

    ssize_t total = 0;
    for (int i = 0; i < hdr->msg_iovlen; ++i) {
        const struct iovec* iov = &hdr->msg_iov[i];
        if (iov->iov_len == 0) continue;
        long ret = __do_syscall4(INSTANTOS_SYS_RECV, handle,
                                 reinterpret_cast<long>(iov->iov_base),
                                 static_cast<long>(iov->iov_len),
                                 static_cast<long>(flags));
        if (int e = sc_error(ret); e) {
            if (total > 0) break;
            return e;
        }
        total += ret;
        if (static_cast<size_t>(ret) < iov->iov_len) break;  // no more data ready
    }
    hdr->msg_namelen = 0;
    hdr->msg_controllen = 0;
    hdr->msg_flags = 0;
    if (length) *length = total;
    return 0;
}

int Sysdeps<Shutdown>::operator()(int sockfd, int how) {
    long handle = handle_for_fd(sockfd);
    if (handle < 0) return EBADF;
    long ret = __do_syscall2(INSTANTOS_SYS_SHUTDOWN, handle, static_cast<long>(how));
    return sc_error(ret);
}

int Sysdeps<GetSockopt>::operator()(int fd, int layer, int number,
                                    void* __restrict buffer, socklen_t* __restrict size) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    long ret = __do_syscall5(INSTANTOS_SYS_GETSOCKOPT, handle,
                             static_cast<long>(layer),
                             static_cast<long>(number),
                             reinterpret_cast<long>(buffer),
                             reinterpret_cast<long>(size));
    return sc_error(ret);
}

int Sysdeps<SetSockopt>::operator()(int fd, int layer, int number,
                                    const void* buffer, socklen_t size) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    long ret = __do_syscall5(INSTANTOS_SYS_SETSOCKOPT, handle,
                             static_cast<long>(layer),
                             static_cast<long>(number),
                             reinterpret_cast<long>(buffer),
                             static_cast<long>(size));
    return sc_error(ret);
}

int Sysdeps<Sockname>::operator()(int fd, struct sockaddr* addr_ptr,
                                  socklen_t max_addr_length, socklen_t* actual_length) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    // The kernel reads an input capacity from *actual_length and writes back the
    // real length there, so seed it with max_addr_length first.
    socklen_t cap = max_addr_length;
    long ret = __do_syscall3(INSTANTOS_SYS_GETSOCKNAME, handle,
                             reinterpret_cast<long>(addr_ptr),
                             reinterpret_cast<long>(&cap));
    if (int e = sc_error(ret); e) return e;
    if (actual_length) *actual_length = cap;
    return 0;
}

int Sysdeps<Peername>::operator()(int fd, struct sockaddr* addr_ptr,
                                  socklen_t max_addr_length, socklen_t* actual_length) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    socklen_t cap = max_addr_length;
    long ret = __do_syscall3(INSTANTOS_SYS_GETPEERNAME, handle,
                             reinterpret_cast<long>(addr_ptr),
                             reinterpret_cast<long>(&cap));
    if (int e = sc_error(ret); e) return e;
    if (actual_length) *actual_length = cap;
    return 0;
}

int Sysdeps<Uname>::operator()(struct utsname* buf) {
    // InstantOS has no uname syscall; report static identity. utsname has five
    // fixed 65-byte char fields (sysname/nodename/release/version/machine).
    if (!buf) return EFAULT;
    auto setf = [](char* dst, const char* src) {
        size_t i = 0;
        for (; src[i] && i < 64; ++i) dst[i] = src[i];
        dst[i] = '\0';
    };
    setf(buf->sysname, "InstantOS");
    setf(buf->nodename, "instantos");
    setf(buf->release, "1.0");
    setf(buf->version, "InstantOS 1.0");
    setf(buf->machine, "x86_64");
    return 0;
}

}
