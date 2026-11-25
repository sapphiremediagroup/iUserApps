#include <abi-bits/errno.h>
#include <bits/ensure.h>
#include <bits/syscall.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/debug.hpp>
#include <mlibc/tcb.hpp>
#include <mlibc/allocator.hpp>
#include <string.h>
#include <time.h>
#include <sched.h>
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
#include <sys/time.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/utsname.h>
#include <unistd.h>

// Terminal ioctl request numbers (match the kernel PTY_TC* constants in
// include/cpu/tty/pty.hpp, i.e. the Linux values). The linux abi-bits ioctls.h
// used by this sysdeps build supplies TIOCGWINSZ but not the TCGETS family;
// recovered here from the transcript (lost sysdeps.cpp region).
#ifndef TCGETS
#define TCGETS  0x5401
#endif
#ifndef TCSETS
#define TCSETS  0x5402
#endif
#ifndef TCSETSW
#define TCSETSW 0x5403
#endif
#ifndef TCSETSF
#define TCSETSF 0x5404
#endif

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

// Argument block for the file-backed mmap syscall (SyscallNumber::MmapFile).
// Passed by pointer because six 64-bit words exceed the 5-argument syscall
// convention. Field order must match struct MmapFileArgs in the kernel.
struct instantos_mmap_file_args {
    unsigned long addr;
    unsigned long length;
    unsigned long prot;
    unsigned long flags;
    unsigned long fd;
    unsigned long offset;
};

// Kernel OSInfo layout used to answer the sysconf memory queries without the
// noisy mlibc fallback. Despite the historical field names, the RAM strings are
// decimal MiB values (see Syscall::sys_osinfo()).
struct instantos_os_info {
    char osname[10];
    char logged_on_user[32];
    char cpu_name[64];
    char total_ram_mb[8];
    char used_ram_mb[8];
    unsigned char major;
    unsigned char minor;
    unsigned char patch;
    unsigned long build_number;
};

extern "C" long __do_syscall0(long);
extern "C" long __do_syscall1(long, long);
extern "C" long __do_syscall2(long, long, long);
extern "C" long __do_syscall3(long, long, long, long);
extern "C" long __do_syscall4(long, long, long, long, long);
extern "C" long __do_syscall5(long, long, long, long, long, long);

// Signal-return trampoline. mlibc does not supply an sa_restorer, so the kernel
// would otherwise have a signal handler "return" to a garbage/uninitialised
// address. This stub is installed as the restorer for every sigaction: when a
// handler returns, control lands here and issues SYS_SigReturn (21), which
// restores the pre-signal register context and unblocks the signal. Defined in
// assembly so it has no prologue/return of its own.
asm(
    ".pushsection .text\n"
    ".globl __instantos_sigrestorer\n"
    ".hidden __instantos_sigrestorer\n"
    ".type __instantos_sigrestorer, @function\n"
    ".align 16\n"
    "__instantos_sigrestorer:\n"
    "    movl $21, %eax\n"   // SyscallNumber::SigReturn
    "    syscall\n"
    ".popsection\n"
);
extern "C" void __instantos_sigrestorer();

namespace {
// The fd numbers of this process -> kernel handles. Chunks of one page, made
// as they are needed (the first is static) and never moved, so a lookup takes
// no lock; up to RLIMIT_NOFILE's ceiling (HandleTable::MaxNofile). 0 is a free
// slot, except fds 0-2, whose handles are 0, 1 and 2 (the stdio convention).
//
// constinit forces compile-time initialization so no static constructor is
// emitted. The rtld links this translation unit and panics if it finds any
// own global constructor (init_array entry), so every file-scope object here
// must be constant-initialized.
constexpr int kFdChunk = 512;
constexpr int kFdCeiling = 65536;
constexpr int kFdChunks = kFdCeiling / kFdChunk;
constinit long fdChunk0[kFdChunk] = {0, 1, 2};
constinit long* fdChunks[kFdChunks] = {fdChunk0};
// No free fd from 3 up lies below it (a hint: threads race on it, the slots
// themselves are taken by compare-and-swap).
constinit int fdLowestFree = 3;
// RLIMIT_NOFILE's soft limit, asked of the kernel once (0: not yet).
constinit long fdSoftLimit = 0;

int sc_error(long ret) {
    return static_cast<unsigned long>(ret) > static_cast<unsigned long>(-4096) ? -ret : 0;
}

long* fd_slot(int fd, bool make) {
    if (fd < 0 || fd >= kFdCeiling) {
        return nullptr;
    }
    long*& link = fdChunks[fd / kFdChunk];
    long* chunk = __atomic_load_n(&link, __ATOMIC_ACQUIRE);
    if (!chunk) {
        if (!make) return nullptr;
        // A private anonymous page (zeroed), straight from the kernel.
        long page = __do_syscall3(INSTANTOS_SYS_MMAP, 0, kFdChunk * sizeof(long), 3 /*read, write*/);
        if (sc_error(page)) return nullptr;
        long* expected = nullptr;
        if (__atomic_compare_exchange_n(&link, &expected, reinterpret_cast<long*>(page), false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            chunk = reinterpret_cast<long*>(page);
        } else {
            __do_syscall2(INSTANTOS_SYS_MUNMAP, page, kFdChunk * sizeof(long));   // another thread made it
            chunk = expected;
        }
    }
    return &chunk[fd % kFdChunk];
}

long fd_limit() {
    long limit = __atomic_load_n(&fdSoftLimit, __ATOMIC_RELAXED);
    if (limit > 0) return limit;
    struct { unsigned long cur, max; } r = {0, 0};
    limit = __do_syscall2(INSTANTOS_SYS_GETRLIMIT, 7 /*RLIMIT_NOFILE*/, reinterpret_cast<long>(&r)) == 0
        ? static_cast<long>(r.cur > static_cast<unsigned long>(kFdCeiling) ? kFdCeiling : r.cur)
        : 1024;
    if (limit < 3) limit = 3;
    __atomic_store_n(&fdSoftLimit, limit, __ATOMIC_RELAXED);
    return limit;
}

// One past the highest fd in use (at least 3).
int fd_top() {
    for (int c = kFdChunks - 1; c >= 0; --c) {
        long* chunk = __atomic_load_n(&fdChunks[c], __ATOMIC_ACQUIRE);
        if (!chunk) continue;
        for (int i = kFdChunk - 1; i >= 0; --i) {
            if (__atomic_load_n(&chunk[i], __ATOMIC_RELAXED) != 0) {
                const int top = c * kFdChunk + i + 1;
                return top < 3 ? 3 : top;
            }
        }
    }
    return 3;
}

long handle_for_fd(int fd) {
    long* slot = fd_slot(fd, false);
    if (!slot) {
        return -1;
    }
    const long handle = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    return handle == 0 && fd > 2 ? -1 : handle;   // 0: a free slot
}

// Gives fd back (its slot is already 0, or is made 0 here).
void free_fd(int fd) {
    if (fd < 3) return;
    if (long* slot = fd_slot(fd, false)) {
        __atomic_store_n(slot, 0L, __ATOMIC_RELEASE);
    }
    int low = __atomic_load_n(&fdLowestFree, __ATOMIC_RELAXED);
    while (fd < low && !__atomic_compare_exchange_n(&fdLowestFree, &low, fd, true,
                                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

unsigned long parse_decimal(const char* value) {
    unsigned long result = 0;
    for (size_t i = 0; value[i] >= '0' && value[i] <= '9'; ++i) {
        result = result * 10 + static_cast<unsigned long>(value[i] - '0');
    }
    return result;
}

// <instantos/window.h>: the kernel handle behind an fd, for the InstantOS
// calls that take handles (a dma-buf for SurfaceFromBuffer).
extern "C" [[gnu::visibility("default")]] long instantos_fd_handle(int fd) {
    return handle_for_fd(fd);
}

// <instantos/futex.h>
extern "C" [[gnu::visibility("default")]] int instantos_futex_wait(const volatile uint32_t* address, uint32_t expected,
                                                                  const struct timespec* timeout) {
    return mlibc::sysdep<FutexWait>(const_cast<int*>(reinterpret_cast<const volatile int*>(address)),
                                    static_cast<int>(expected), timeout);
}
extern "C" [[gnu::visibility("default")]] int instantos_futex_wake(const volatile uint32_t* address, int count) {
    const long n = __do_syscall2(INSTANTOS_SYS_FUTEX_WAKE,
                                 reinterpret_cast<long>(const_cast<const uint32_t*>(address)),
                                 count < 0 ? 0L : static_cast<long>(count));
    return n < 0 ? 0 : static_cast<int>(n);
}

// The lowest free fd from `min` below RLIMIT_NOFILE, or -1 (EMFILE).
int register_fd_min(long handle, int min) {
    if (min < 0) min = 0;
    const int hint = __atomic_load_n(&fdLowestFree, __ATOMIC_RELAXED);
    const bool fromHint = min <= hint;   // fds 0-2 are never free slots
    const int limit = static_cast<int>(fd_limit());
    for (int fd = fromHint ? hint : min; fd < limit; ++fd) {
        long* slot = fd_slot(fd, true);
        if (!slot) return -1;
        // Slots are taken atomically: threads open files at the same time.
        long unused = 0;
        if (__atomic_compare_exchange_n(slot, &unused, handle, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            if (fromHint) {
                int seen = hint;
                __atomic_compare_exchange_n(&fdLowestFree, &seen, fd + 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
            }
            return fd;
        }
    }
    return -1;
}

int register_fd(long handle) {
    return register_fd_min(handle, 3);
}

// <instantos/window.h>: a kernel handle an InstantOS call gave (a scanout
// buffer's dma-buf) as an fd of this process; closing the fd closes it.
extern "C" [[gnu::visibility("default")]] int instantos_handle_fd(long handle) {
    if (handle < 0) return -1;
    return register_fd(handle);
}

int register_fd_at(int fd, long handle) {
    long* slot = fd_slot(fd, true);
    if (!slot) {
        return -1;
    }
    __atomic_store_n(slot, handle, __ATOMIC_RELEASE);
    return fd;
}

// AT_FDCWD: "resolve relative to the current working directory" (Linux ABI).
constexpr int kAtFdCwd = -100;

// Resolve the path a *at call should operate on. When `path` is absolute or the
// directory fd is AT_FDCWD, the original path is used unchanged. Otherwise the
// directory fd's absolute path (from the kernel fd->path syscall) is prefixed,
// composing "<dir>/<path>" into `out`. Returns the string to use (either `path`
// itself or `out`), or nullptr on failure with *err set to an errno value.
const char* at_resolve(int dirfd, const char* path, char* out, size_t outsz, int* err) {
    *err = 0;
    if (!path) { *err = EINVAL; return nullptr; }
    if (path[0] == '/' || dirfd == kAtFdCwd) {
        return path;   // absolute, or already cwd-relative
    }
    long handle = handle_for_fd(dirfd);
    if (handle < 0) { *err = EBADF; return nullptr; }
    char dir[256];
    long n = __do_syscall3(INSTANTOS_SYS_FD_PATH, handle,
                           reinterpret_cast<long>(dir), static_cast<long>(sizeof(dir)));
    if (int e = sc_error(n); e) { *err = e; return nullptr; }
    size_t di = 0;
    for (long i = 0; i < n && di + 1 < outsz; ++i) out[di++] = dir[i];
    if (di == 0 || out[di - 1] != '/') {
        if (di + 1 < outsz) out[di++] = '/';
    }
    for (size_t pi = 0; path[pi] != '\0' && di + 1 < outsz; ++pi) out[di++] = path[pi];
    out[di] = '\0';
    return out;
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
// The kernel's readdir has no offset, so a snapshot must hold the whole
// directory. It starts at 256 entries and grows until the kernel returns fewer
// than asked for: a fixed 256 cut binutils' bfd/ (~450 entries) short, and
// make's VPATH cache then could not see elfxx-target.h (P7.1).
constexpr int kFirstDirEntries = 256;
constexpr int kMaxDirEntries = 65536;
// Directory handles are returned to mlibc as fds (opendir stores them in
// DIR::__handle, closedir calls close() on them). Offset them well above the
// real fd range so Close() can recognise and free them without touching the
// fd table.
constexpr int kDirHandleBase = 0x40000000;

struct DirStream {
    bool inUse = false;
    bool loaded = false;
    bool fromFd = false;   // opened via fdopendir(): keyed by a real fd, not a high handle
    int ownerFd = 0;       // the owning real fd when fromFd (freed by close(ownerFd))
    int cursor = 0;
    int count = 0;
    char path[256];
    instantos_dirent* entries = nullptr;   // getAllocator(), freed by free_dir_entries
};

void free_dir_entries(DirStream& dir) {
    if (dir.entries) {
        getAllocator().free(dir.entries);
        dir.entries = nullptr;
    }
    dir.count = 0;
}

// constinit + value-initialization keeps this in .bss with no static
// constructor; see the note on fdTable above.
constinit DirStream dirStreams[kMaxDirs] = {};

int alloc_dir_stream(const char* path) {
    for (int i = 0; i < kMaxDirs; ++i) {
        if (!__atomic_exchange_n(&dirStreams[i].inUse, true, __ATOMIC_ACQ_REL)) {
            dirStreams[i].loaded = false;
            dirStreams[i].fromFd = false;
            dirStreams[i].ownerFd = 0;
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

// Find the directory snapshot owned by a real fd (fdopendir), or -1.
int find_dir_stream_by_fd(int fd) {
    for (int i = 0; i < kMaxDirs; ++i) {
        if (dirStreams[i].inUse && dirStreams[i].fromFd && dirStreams[i].ownerFd == fd) {
            return i;
        }
    }
    return -1;
}

// Resolve an open fd to its absolute path via the kernel fd->path syscall.
const char* fd_to_path(int fd, char* out, size_t outsz, int* err) {
    *err = 0;
    long h = handle_for_fd(fd);
    if (h < 0) { *err = EBADF; return nullptr; }
    long n = __do_syscall3(INSTANTOS_SYS_FD_PATH, h,
                           reinterpret_cast<long>(out), static_cast<long>(outsz));
    if (int e = sc_error(n); e) { *err = e; return nullptr; }
    if (n <= 0 || static_cast<size_t>(n) >= outsz) { *err = EINVAL; return nullptr; }
    out[n] = '\0';
    return out;
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

// P3.2: restore the libc fd-number -> handle table that Execve stashed before an
// exec, so inherited fds (>2) survive the image replacement. Called once from
// __mlibc_entry at process start; the kernel returns nothing for a process that
// never stashed (initial process / fork child that didn't exec), leaving the
// default {0,1,2} table intact.
extern "C" void __instantos_restore_fdtable(void) {
    const long count = __do_syscall2(INSTANTOS_SYS_FDTABLE_FETCH, 0, 0);   // how many
    if (count <= 0) return;
    if (count <= kFdChunk) {
        __do_syscall2(INSTANTOS_SYS_FDTABLE_FETCH, reinterpret_cast<long>(fdChunk0), count);
        return;
    }
    const long bytes = count * static_cast<long>(sizeof(long));
    long buffer = __do_syscall3(INSTANTOS_SYS_MMAP, 0, bytes, 3);
    if (sc_error(buffer)) return;
    const long got = __do_syscall2(INSTANTOS_SYS_FDTABLE_FETCH, buffer, count);
    const long* table = reinterpret_cast<const long*>(buffer);
    for (long fd = 0; fd < got && fd < kFdCeiling; ++fd) {
        if (table[fd] != 0 || fd < 3) register_fd_at(static_cast<int>(fd), table[fd]);
    }
    __do_syscall2(INSTANTOS_SYS_MUNMAP, buffer, bytes);
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
        free_dir_entries(dirStreams[slot]);
        dirStreams[slot].loaded = false;
        __atomic_store_n(&dirStreams[slot].inUse, false, __ATOMIC_RELEASE);
        return 0;
    }
    // A real fd may also own an fdopendir() directory snapshot (see ReadEntries);
    // release it here so closedir(fdopendir(fd)) does not leak a DirStream slot.
    int dslot = find_dir_stream_by_fd(fd);
    if (dslot >= 0) {
        free_dir_entries(dirStreams[dslot]);
        dirStreams[dslot].loaded = false;
        dirStreams[dslot].fromFd = false;
        __atomic_store_n(&dirStreams[dslot].inUse, false, __ATOMIC_RELEASE);
    }
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    // The standard streams (fd 0/1/2) are backed by the console or an inherited
    // tty; the kernel refuses to close them. Report success rather than EBADF so
    // that libc stdio teardown (e.g. coreutils' close_stdout / fclose(stdout))
    // does not report a spurious "error closing file".
    if (fd < 3) return 0;
    // Only one of two threads closing it gets the handle.
    long* slot = fd_slot(fd, false);
    handle = slot ? __atomic_exchange_n(slot, 0L, __ATOMIC_ACQ_REL) : 0;
    if (handle == 0) return EBADF;
    free_fd(fd);
    return sc_error(__do_syscall1(INSTANTOS_SYS_CLOSE, handle));
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
    // POSIX: ENOENT for a missing path, ENOTDIR for anything but a directory.
    // The stream reads its entries lazily, so without this opendir() on a file
    // succeeded; e2fsprogs' profile code (mke2fs reading its mke2fs.conf) asks
    // opendir whether a config path is a directory and wants ENOTDIR.
    instantos_stat ks;
    long st = __do_syscall2(INSTANTOS_SYS_STAT, reinterpret_cast<long>(path), reinterpret_cast<long>(&ks));
    if (int e = sc_error(st); e) return e;
    if ((ks.k_mode & 0170000) != 0040000) return ENOTDIR;
    int slot = alloc_dir_stream(path);
    if (slot < 0) return EMFILE;
    *handle = slot + kDirHandleBase;
    return 0;
}

int Sysdeps<ReadEntries>::operator()(int handle, void* buffer, size_t max_size, size_t* bytes_read) {
    if (!buffer || !bytes_read) return EINVAL;

    int slot;
    if (handle >= kDirHandleBase) {
        // opendir(path): high handle indexes the DirStream table directly.
        slot = handle - kDirHandleBase;
        if (slot < 0 || slot >= kMaxDirs || !dirStreams[slot].inUse) return EBADF;
    } else {
        // fdopendir(fd): mlibc stored a real fd in DIR::__handle. Directory
        // iteration is path-based here, so recover the fd's path (fd->path
        // syscall) once and drive the same snapshot machinery, keyed to the fd
        // so the snapshot persists across readdir() calls and is released by
        // close(fd). The fd itself stays open, so dirfd()+openat() keep working.
        slot = find_dir_stream_by_fd(handle);
        if (slot < 0) {
            char pbuf[256]; int e;
            const char* p = fd_to_path(handle, pbuf, sizeof(pbuf), &e);
            if (!p) return e;
            slot = alloc_dir_stream(p);
            if (slot < 0) return EMFILE;
            dirStreams[slot].fromFd = true;
            dirStreams[slot].ownerFd = handle;
        }
    }

    DirStream& dir = dirStreams[slot];

    // Snapshot the whole directory on first read.
    if (!dir.loaded) {
        free_dir_entries(dir);
        long ret = 0;
        for (int cap = kFirstDirEntries;; cap *= 4) {
            dir.entries = static_cast<instantos_dirent*>(
                getAllocator().allocate(sizeof(instantos_dirent) * cap));
            if (!dir.entries) return ENOMEM;
            ret = __do_syscall3(INSTANTOS_SYS_READDIR,
                                reinterpret_cast<long>(dir.path),
                                reinterpret_cast<long>(dir.entries),
                                cap);
            if (int e = sc_error(ret); e) { free_dir_entries(dir); return e; }
            if (ret < cap || cap >= kMaxDirEntries) break;
            free_dir_entries(dir);   // full: the directory may hold more
        }
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

int Sysdeps<Mkdirat>::operator()(int dirfd, const char* path, mode_t mode) {
    // Resolve `path` against the directory fd, then create via the path-based
    // mkdir syscall. GNU tar/coreutils create extracted directories with
    // mkdirat(chdir_fd, name, mode).
    char buf[512]; int e;
    const char* p = at_resolve(dirfd, path, buf, sizeof(buf), &e);
    if (!p) return e;
    long ret = __do_syscall2(INSTANTOS_SYS_MKDIR, reinterpret_cast<long>(p),
                             static_cast<long>(mode));
    return sc_error(ret);
}

int Sysdeps<Rmdir>::operator()(const char* path) {
    long ret = __do_syscall1(INSTANTOS_SYS_RMDIR, reinterpret_cast<long>(path));
    return sc_error(ret);
}

int Sysdeps<Unlinkat>::operator()(int dirfd, const char* path, int flags) {
    char buf[512]; int e;
    const char* p = at_resolve(dirfd, path, buf, sizeof(buf), &e);
    if (!p) return e;
    // AT_REMOVEDIR (0x200) selects rmdir semantics; otherwise unlink a file.
    if (flags & 0x200) {
        long ret = __do_syscall1(INSTANTOS_SYS_RMDIR, reinterpret_cast<long>(p));
        return sc_error(ret);
    }
    long ret = __do_syscall1(INSTANTOS_SYS_UNLINK, reinterpret_cast<long>(p));
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

namespace {
// Memory for one call: `small` on the stack, or a mapping when more is needed.
struct Scratch {
    void* data = nullptr;
    long mapped = 0;
    Scratch(void* small, long smallBytes, long need) {
        if (need <= smallBytes) {
            data = small;
            return;
        }
        const long r = __do_syscall3(INSTANTOS_SYS_MMAP, 0, need, 3);
        if (!sc_error(r)) {
            data = reinterpret_cast<void*>(r);
            mapped = need;
        }
    }
    ~Scratch() {
        if (mapped) __do_syscall2(INSTANTOS_SYS_MUNMAP, reinterpret_cast<long>(data), mapped);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};
}

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

    // Every fd an fd_set can hold (FD_SETSIZE).
    if (num_fds < 0) return EINVAL;
    if (num_fds > FD_SETSIZE) num_fds = FD_SETSIZE;
    // The kernel's pollfds one after the other, then which fd each one is.
    alignas(8) char small[64 * (sizeof(instantos_pollfd) + sizeof(int))];
    Scratch scratch(small, sizeof(small),
                    static_cast<long>(num_fds) * static_cast<long>(sizeof(instantos_pollfd) + sizeof(int)));
    if (!scratch.data) return ENOMEM;
    instantos_pollfd* pfds = static_cast<instantos_pollfd*>(scratch.data);
    int* mapFd = reinterpret_cast<int*>(pfds + num_fds);
    int n = 0;

    for (int fd = 0; fd < num_fds; ++fd) {
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
    } else if (timeoutMs > 0) {
        // select(0, ..., &tv) is a way to sleep.
        if (int e = sc_error(__do_syscall1(INSTANTOS_SYS_SLEEP, timeoutMs)); e) return e;
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
    if (count > static_cast<nfds_t>(kFdCeiling)) {
        return EINVAL;
    }

    instantos_pollfd small[64];
    Scratch scratch(small, sizeof(small), static_cast<long>(count) * static_cast<long>(sizeof(instantos_pollfd)));
    if (!scratch.data) return ENOMEM;
    instantos_pollfd* kfds = static_cast<instantos_pollfd*>(scratch.data);
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
        case fsfd_target::fd_path: {
            // fstatat(dirfd, path): resolve path against the directory fd.
            char buf[512]; int e;
            const char* p = at_resolve(fd, path, buf, sizeof(buf), &e);
            if (!p) return e;
            ret = __do_syscall2((flags & AT_SYMLINK_NOFOLLOW) ? INSTANTOS_SYS_LSTAT
                                                              : INSTANTOS_SYS_STAT,
                                reinterpret_cast<long>(p), reinterpret_cast<long>(&ks));
            break;
        }
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
    // new userspace fd. POSIX requires the LOWEST available fd >= arg (the third
    // fcntl argument): bash relies on this to move saved fds into a high range
    // (SHELL_FD_BASE=10) during redirection save/restore. Honoring the minimum
    // (instead of always starting at fd 3) keeps bash's fd bookkeeping correct.
    if (request == F_DUPFD || request == F_DUPFD_CLOEXEC) {
        if (arg < 0 || arg >= fd_limit()) {
            __do_syscall1(INSTANTOS_SYS_CLOSE, ret);
            return EINVAL;
        }
        int newfd = register_fd_min(ret, static_cast<int>(arg));
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

// The process's id, also in its threads (GetPID is the thread's).
pid_t Sysdeps<GetPid>::operator()() {
    return __do_syscall0(INSTANTOS_SYS_GETTGID);
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

namespace {
// To the kernel protection 0 is read and write (native programs' default), and
// PROT_NONE is this.
long kernel_prot(int prot) {
    return prot == PROT_NONE ? 0x100 : prot;
}
}

int Sysdeps<VmMap>::operator()(void* hint, size_t size, int prot, int flags, int fd, off_t offset, void** window) {
    // The kernel takes an address as fixed and replaces what is there: without
    // MAP_FIXED a hint must not.
    if (!(flags & MAP_FIXED)) {
        hint = nullptr;
    }
    // Private anonymous mappings (including AnonAllocate) use the plain
    // demand-paged mmap that ignores fd/offset; shared ones are an object the
    // kernel makes.
    const bool anonymous = (flags & MAP_ANONYMOUS) || fd < 0;
    if (anonymous && !(flags & MAP_SHARED)) {
        long ret = __do_syscall3(INSTANTOS_SYS_MMAP, reinterpret_cast<long>(hint),
                                 static_cast<long>(size), kernel_prot(prot));
        if (int e = sc_error(ret); e) return e;
        *window = reinterpret_cast<void*>(ret);
        return 0;
    }

    // File-backed mapping: translate the userspace fd to its kernel handle and
    // hand the kernel the full argument set via a struct (six values exceed the
    // 5-argument syscall convention).
    long handle = anonymous ? -1 : handle_for_fd(fd);
    if (handle < 0 && !anonymous) return EBADF;
    struct instantos_mmap_file_args args;
    args.addr = reinterpret_cast<unsigned long>(hint);
    args.length = static_cast<unsigned long>(size);
    args.prot = static_cast<unsigned long>(kernel_prot(prot));
    args.flags = static_cast<unsigned long>(anonymous ? flags | MAP_ANONYMOUS : flags);
    args.fd = static_cast<unsigned long>(handle);
    args.offset = static_cast<unsigned long>(offset);
    long ret = __do_syscall1(INSTANTOS_SYS_MMAP_FILE, reinterpret_cast<long>(&args));
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
                             static_cast<long>(size), kernel_prot(prot));
    return sc_error(ret);
}

int Sysdeps<MemfdCreate>::operator()(const char* name, int flags, int* fd) {
    long ret = __do_syscall2(INSTANTOS_SYS_MEMFD_CREATE, reinterpret_cast<long>(name), flags);
    if (int e = sc_error(ret); e) return e;
    const int made = register_fd(ret);
    if (made < 0) {
        __do_syscall1(INSTANTOS_SYS_CLOSE, ret);
        return EMFILE;
    }
    *fd = made;
    return 0;
}

int Sysdeps<Msync>::operator()(void* pointer, size_t size, int flags) {
    // Flush a MAP_SHARED file mapping's dirty pages to its backing file. The
    // kernel always syncs synchronously; MS_SYNC/MS_ASYNC/MS_INVALIDATE are
    // accepted and ignored beyond that.
    long ret = __do_syscall3(INSTANTOS_SYS_MSYNC, reinterpret_cast<long>(pointer),
                             static_cast<long>(size), static_cast<long>(flags));
    return sc_error(ret);
}

int Sysdeps<Sysconf>::operator()(int number, long* result) {
    if (!result) return EINVAL;

    switch (number) {
        case _SC_OPEN_MAX:
            *result = fd_limit();
            return 0;
        case _SC_CHILD_MAX:
            *result = 1024;
            return 0;
        case _SC_NPROCESSORS_CONF:
        case _SC_NPROCESSORS_ONLN: {
            // Every CPU the kernel brought up is online.
            long ret = __do_syscall2(INSTANTOS_SYS_CPU_INFO, 0, 0);
            *result = ret > 0 ? ret : 1;
            return 0;
        }
        case _SC_PHYS_PAGES:
        case _SC_AVPHYS_PAGES: {
            instantos_os_info info {};
            long ret = __do_syscall1(INSTANTOS_SYS_OSINFO, reinterpret_cast<long>(&info));
            if (int e = sc_error(ret); e) return e;
            unsigned long total_mb = parse_decimal(info.total_ram_mb);
            unsigned long used_mb = parse_decimal(info.used_ram_mb);
            unsigned long available_mb = used_mb < total_mb ? total_mb - used_mb : 0;
            *result = static_cast<long>((number == _SC_PHYS_PAGES ? total_mb : available_mb) * 256);
            return 0;
        }
        default:
            return EINVAL;
    }
}

int Sysdeps<Brk>::operator()(void**) {
    // InstantOS allocations are mmap-backed. mlibc only calls this for sbrk(0),
    // so report the unsupported brk arena without emitting a missing-sysdep log.
    return ENOMEM;
}

int Sysdeps<ClockGet>::operator()(int clock, time_t* secs, long* nanos) {
    // CLOCK_REALTIME (0) and CLOCK_TAI must be wall-clock time (seconds since
    // the Unix epoch) so time()/clock_gettime() and tools like uptime/date work.
    // CLOCK_MONOTONIC (1) / _BOOTTIME use the millisecond uptime counter.
    constexpr int kClockMonotonic = 1;
    constexpr int kClockMonotonicRaw = 4;
    constexpr int kClockBoottime = 7;
    if (clock == kClockMonotonic || clock == kClockMonotonicRaw || clock == kClockBoottime) {
        // Nanoseconds from the TSC (GetTimeNs); ms from the tick counter on
        // a kernel without it.
        long ns = __do_syscall0(INSTANTOS_SYS_GETTIME_NS);
        if (sc_error(ns) == 0) {
            *secs = ns / 1000000000L;
            *nanos = ns % 1000000000L;
            return 0;
        }
        long ms = __do_syscall0(INSTANTOS_SYS_GETTIME);
        if (int e = sc_error(ms); e) return e;
        *secs = ms / 1000;
        *nanos = (ms % 1000) * 1000000;
        return 0;
    }
    // The wall clock in nanoseconds; kernels without GetUnixTimeNs fall back
    // to the derivation below.
    long unixns = __do_syscall0(INSTANTOS_SYS_GETUNIXTIME_NS);
    if (sc_error(unixns) == 0) {
        *secs = unixns / 1000000000L;
        *nanos = unixns % 1000000000L;
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

int Sysdeps<SetItimer>::operator()(int which, const struct itimerval* new_value,
                                   struct itimerval* old_value) {
    long ret = __do_syscall3(INSTANTOS_SYS_SETITIMER, which, reinterpret_cast<long>(new_value),
                             reinterpret_cast<long>(old_value));
    return sc_error(ret);
}

int Sysdeps<GetItimer>::operator()(int which, struct itimerval* value) {
    long ret = __do_syscall2(INSTANTOS_SYS_GETITIMER, which, reinterpret_cast<long>(value));
    return sc_error(ret);
}

void Sysdeps<Yield>::operator()() {
    __do_syscall0(INSTANTOS_SYS_YIELD);
}

int Sysdeps<Sleep>::operator()(time_t* secs, long* nanos) {
    // At least as long as asked: the kernel sleeps whole milliseconds.
    long ms = (*secs * 1000) + (*nanos + 999999) / 1000000;
    long ret = __do_syscall1(INSTANTOS_SYS_SLEEP, ms);
    if (int e = sc_error(ret); e) return e;
    *secs = 0;
    *nanos = 0;
    return 0;
}

int Sysdeps<FutexWait>::operator()(int* pointer, int expected, const timespec* time) {
    // The kernel takes nanoseconds, ~0 for no timeout.
    unsigned long timeout = ~0UL;
    if (time) {
        if (time->tv_sec < 0 || time->tv_nsec < 0 || time->tv_nsec >= 1000000000) return EINVAL;
        const unsigned long secs = static_cast<unsigned long>(time->tv_sec);
        if (secs < 9000000000UL) {   // 285 years
            timeout = secs * 1000000000UL + static_cast<unsigned long>(time->tv_nsec);
        }
    }
    return sc_error(__do_syscall3(INSTANTOS_SYS_FUTEX_WAIT, reinterpret_cast<long>(pointer),
                                  static_cast<long>(static_cast<unsigned int>(expected)),
                                  static_cast<long>(timeout)));
}

int Sysdeps<FutexWake>::operator()(int* pointer, bool all) {
    return sc_error(__do_syscall2(INSTANTOS_SYS_FUTEX_WAKE, reinterpret_cast<long>(pointer), all ? -1L : 1L));
}

// The calling thread's id (the kernel's GetPID is per thread). The main
// thread's is the process's.
pid_t Sysdeps<FutexTid>::operator()() {
    return static_cast<pid_t>(__do_syscall0(INSTANTOS_SYS_GETPID));
}

// gettid(): the same number (what Tgkill takes).
pid_t Sysdeps<GetTid>::operator()() {
    return static_cast<pid_t>(__do_syscall0(INSTANTOS_SYS_GETPID));
}

// pthread_setname_np / pthread_getname_np: the kernel keeps no thread names,
// so a name is accepted and forgotten (Mesa names its worker threads; without
// these mlibc's __ensure fired on every one).
int Sysdeps<ThreadSetname>::operator()(void*, const char*) {
    return 0;
}

int Sysdeps<ThreadGetname>::operator()(void*, char* name, size_t size) {
    if (size == 0) {
        return ERANGE;
    }
    name[0] = '\0';
    return 0;
}

// pthread_setschedparam / pthread_getschedparam: the scheduler has no
// policies or priorities, so every thread is SCHED_OTHER at priority 0 and a
// request for another is accepted and ignored (Mesa lowers the priority of its
// worker threads; without these mlibc's __ensure fired).
int Sysdeps<GetSchedparam>::operator()(void*, int* policy, struct sched_param* param) {
    *policy = SCHED_OTHER;
    param->sched_priority = 0;
    return 0;
}

int Sysdeps<SetSchedparam>::operator()(void*, int, const struct sched_param*) {
    return 0;
}

// --- Threads -------------------------------------------------------------
//
// mlibc's generic pthread layer drives thread creation through three sysdeps:
// PrepareStack lays out a fresh stack, Clone spawns the kernel thread on it,
// and ThreadExit terminates the calling thread. Joining is done in userspace by
// mlibc (a futex wait for the TCB's tid, which the kernel clears when the
// thread is gone), so the kernel side is handle-free. See sys_thread_clone()
// and sys_thread_exit_notify() in the kernel.
//
// This block is excluded from the runtime linker (ld-instantos.so), which is
// single-threaded and does not link the pthread machinery (mlibc::thread_exit
// lives in libc.so only).
#ifndef MLIBC_BUILDING_RTLD

// Assembly entry stub for a newly spawned thread (defined below, outside the
// namespace so it keeps C linkage / a plain global symbol).
extern "C" void __mlibc_thread_entry();

// Matches mlibc's default pthread stack (see thread_attr_init()).
static constexpr size_t kDefaultThreadStack = 0x200000;

int Sysdeps<PrepareStack>::operator()(void** stack, void* entry, void* user_arg, void* tcb,
                                      size_t* stack_size, size_t* guard_size, void** stack_base) {
    if (!*stack_size)
        *stack_size = kDefaultThreadStack;

    if (*stack) {
        // Caller-provided stack (pthread_attr_setstack, or a kept one): as it is.
        *guard_size = 0;
        *stack_base = *stack;
    } else {
        // The guard, which no access survives, lies below the stack.
        *stack_size = (*stack_size + 4095) & ~size_t{4095};
        *guard_size = (*guard_size + 4095) & ~size_t{4095};
        void* base = nullptr;
        if (int e = sysdep<AnonAllocate>(*stack_size + *guard_size, &base); e)
            return e;
        if (*guard_size) {
            if (int e = sysdep<VmProtect>(base, *guard_size, PROT_NONE); e) {
                sysdep<VmUnmap>(base, *stack_size + *guard_size);
                return e;
            }
        }
        *stack_base = reinterpret_cast<char*>(base) + *guard_size;
    }

    // Lay the mlibc thread-entry arguments at the top of the stack. The stub
    // __mlibc_thread_entry pops them in this order: entry, tcb, user_arg, and a
    // final nul filler that leaves rsp 16-byte aligned before it calls the C
    // trampoline. The base is page-aligned and the size is a multiple of 16, so
    // the initial sp (top - 32) is 16-aligned as the SysV ABI requires.
    auto* sp = reinterpret_cast<void**>(reinterpret_cast<char*>(*stack_base) + *stack_size);
    *--sp = nullptr;     // alignment filler
    *--sp = user_arg;
    *--sp = tcb;
    *--sp = entry;
    *stack = reinterpret_cast<void*>(sp);
    return 0;
}

int Sysdeps<Clone>::operator()(void* tcb, pid_t* pid_out, void* stack) {
    // The new thread installs its own TLS (fs base = tcb) from the trampoline
    // via TcbSet, so the kernel does not need the tcb here.
    (void)tcb;
    long ret = __do_syscall2(INSTANTOS_SYS_THREAD_CLONE,
                             reinterpret_cast<long>(&__mlibc_thread_entry),
                             reinterpret_cast<long>(stack));
    if (int e = sc_error(ret); e)
        return e;
    *pid_out = static_cast<pid_t>(ret);
    return 0;
}

void Sysdeps<ThreadExit>::operator()() {
    __do_syscall1(INSTANTOS_SYS_THREAD_EXIT_NOTIFY, reinterpret_cast<long>(&get_current_tcb()->tid));
    __builtin_unreachable();
}

#endif  // !MLIBC_BUILDING_RTLD

namespace {

// The ioctls whose structs carry fds (the GPU's: DRM 'd', sync_file '>',
// dma-buf 'b'). The kernel's DRM code reads and writes an fd there as the
// slot of a File handle (handle = 1 << 48 | slot: 32 bits cannot hold a
// handle), while programs use this table's small fds: in-fds are turned
// into slots for the call and put back, out-fds become new table entries.
struct FdField {
    unsigned type;      // the ioctl type byte
    unsigned nr;        // its number
    unsigned offset;    // of the s32 fd in the struct
    bool out;           // written by the kernel (else read)
};

constexpr FdField kFdFields[] = {
    {'d', 0x2d, 8, true},    // DRM_IOCTL_PRIME_HANDLE_TO_FD: drm_prime_handle.fd
    {'d', 0x2e, 8, false},   // DRM_IOCTL_PRIME_FD_TO_HANDLE
    {'d', 0xC1, 8, true},    // DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD: drm_syncobj_handle.fd
    {'d', 0xC2, 8, false},   // DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE
    {'>', 3, 32, false},     // SYNC_IOC_MERGE: sync_merge_data.fd2 ...
    {'>', 3, 36, true},      // ... and .fence
    {'b', 2, 4, true},       // DMA_BUF_IOCTL_EXPORT_SYNC_FILE: .fd
    {'b', 3, 4, false},      // DMA_BUF_IOCTL_IMPORT_SYNC_FILE: .fd
};

constexpr long kFileHandleType = 1L << 48;
constexpr unsigned kExecbuffer2Nr = 0x40 + 0x29;   // DRM_IOCTL_I915_GEM_EXECBUFFER2(_WR)
constexpr unsigned long kExecFenceIn = 1UL << 16;
constexpr unsigned long kExecFenceOut = 1UL << 17;

// A table fd as the kernel's slot (-1 when it is not a file).
int32_t slot_for_fd(int32_t fd) {
    const long handle = handle_for_fd(fd);
    if (handle <= 0 || (handle >> 48) != 1) {
        return -1;
    }
    return static_cast<int32_t>(handle & 0xFFFFFFFFL);
}

// A slot the kernel handed out, as a new table fd (closed if the table is full).
int32_t fd_for_slot(int32_t slot) {
    if (slot < 0) {
        return -1;
    }
    const long handle = kFileHandleType | slot;
    const int fd = register_fd(handle);
    if (fd < 0) {
        __do_syscall1(INSTANTOS_SYS_CLOSE, handle);
    }
    return fd;
}

} // namespace

int Sysdeps<Ioctl>::operator()(int fd, unsigned long request, void* arg, int* result) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    const unsigned type = (request >> 8) & 0xFF;
    const unsigned nr = request & 0xFF;
    auto* bytes = static_cast<unsigned char*>(arg);

    // In-fds become slots for the call.
    int32_t saved[2] = {0, 0};
    int savedCount = 0;
    const FdField* fields[2] = {nullptr, nullptr};
    if (bytes) {
        for (const FdField& f : kFdFields) {
            if (f.type != type || f.nr != nr || savedCount == 2) continue;
            fields[savedCount] = &f;
            int32_t value;
            memcpy(&value, bytes + f.offset, sizeof(value));
            saved[savedCount++] = value;
            if (!f.out) {
                const int32_t slot = slot_for_fd(value);
                memcpy(bytes + f.offset, &slot, sizeof(slot));
            }
        }
    }
    // Execbuffer: FENCE_IN in rsvd2's low half, FENCE_OUT back in its high half.
    const bool execbuffer = bytes && type == 'd' && nr == kExecbuffer2Nr;
    unsigned long execFlags = 0;
    uint64_t rsvd2 = 0;
    if (execbuffer) {
        memcpy(&execFlags, bytes + 40, sizeof(execFlags));
        memcpy(&rsvd2, bytes + 56, sizeof(rsvd2));
        if (execFlags & kExecFenceIn) {
            const int32_t slot = slot_for_fd(static_cast<int32_t>(rsvd2 & 0xFFFFFFFFu));
            const uint64_t translated = (rsvd2 & ~0xFFFFFFFFull) | static_cast<uint32_t>(slot);
            memcpy(bytes + 56, &translated, sizeof(translated));
        }
    }

    long ret = __do_syscall3(INSTANTOS_SYS_IOCTL, handle, static_cast<long>(request),
                             reinterpret_cast<long>(arg));
    const int error = sc_error(ret);

    for (int i = 0; i < savedCount; ++i) {
        if (!fields[i]->out) {
            memcpy(bytes + fields[i]->offset, &saved[i], sizeof(saved[i]));   // the caller's fd, as it was
        } else if (!error) {
            int32_t slot;
            memcpy(&slot, bytes + fields[i]->offset, sizeof(slot));
            const int32_t local = fd_for_slot(slot);
            memcpy(bytes + fields[i]->offset, &local, sizeof(local));
            if (local < 0) return EMFILE;
        }
    }
    if (execbuffer) {
        uint64_t after;
        memcpy(&after, bytes + 56, sizeof(after));
        after = (after & ~0xFFFFFFFFull) | (rsvd2 & 0xFFFFFFFFull);   // FENCE_IN's fd as it was
        if (!error && (execFlags & kExecFenceOut)) {
            const int32_t local = fd_for_slot(static_cast<int32_t>(after >> 32));
            after = (after & 0xFFFFFFFFull) | (static_cast<uint64_t>(static_cast<uint32_t>(local)) << 32);
            memcpy(bytes + 56, &after, sizeof(after));
            if (local < 0) return EMFILE;
        } else {
            memcpy(bytes + 56, &after, sizeof(after));
        }
    }
    if (error) return error;
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

// The kernel's signal masks are one 64-bit word with signal N at bit N (1..63);
// mlibc's sigset_t (Linux's) has signal N at bit N-1 of its first word.
static unsigned long kernel_sigmask(const sigset_t* set) {
    return set ? set->__sig[0] << 1 : 0;
}

static void mlibc_sigmask(sigset_t* set, unsigned long mask) {
    for (auto& word : set->__sig) word = 0;
    set->__sig[0] = mask >> 1;
}

int Sysdeps<Sigprocmask>::operator()(int how, const sigset_t* set, sigset_t* retrieve) {
    unsigned long kset = kernel_sigmask(set);
    unsigned long kold = 0;
    long ret = __do_syscall3(INSTANTOS_SYS_SIGPROCMASK, how,
                             set ? reinterpret_cast<long>(&kset) : 0,
                             retrieve ? reinterpret_cast<long>(&kold) : 0);
    if (int e = sc_error(ret); e) return e;
    if (retrieve) mlibc_sigmask(retrieve, kold);
    return 0;
}

int Sysdeps<Sigsuspend>::operator()(const sigset_t* set) {
    unsigned long mask = kernel_sigmask(set);
    return sc_error(__do_syscall1(INSTANTOS_SYS_SIGSUSPEND, reinterpret_cast<long>(&mask)));
}

int Sysdeps<Sigpending>::operator()(sigset_t* set) {
    unsigned long mask = 0;
    if (int e = sc_error(__do_syscall1(INSTANTOS_SYS_SIGPENDING, reinterpret_cast<long>(&mask))); e) return e;
    mlibc_sigmask(set, mask);
    return 0;
}

// The kernel writes Linux's siginfo_t itself.
int Sysdeps<Sigtimedwait>::operator()(const sigset_t* __restrict set, siginfo_t* __restrict info,
                                      const struct timespec* __restrict timeout, int* out_signal) {
    unsigned long mask = kernel_sigmask(set);
    long ns = -1;   // no timeout
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000) return EINVAL;
        ns = timeout->tv_sec > 9000000000L ? 9000000000L * 1000000000L
                                           : timeout->tv_sec * 1000000000L + timeout->tv_nsec;
    }
    long ret = __do_syscall3(INSTANTOS_SYS_SIGTIMEDWAIT, reinterpret_cast<long>(&mask),
                             reinterpret_cast<long>(info), ns);
    if (int e = sc_error(ret); e) return e;
    *out_signal = static_cast<int>(ret);
    return 0;
}

int Sysdeps<Sigaction>::operator()(int signum, const struct sigaction* __restrict act,
                                   struct sigaction* __restrict oldact) {
    instantos_sigaction kact;
    instantos_sigaction kold;

    if (act) {
        kact.handler = reinterpret_cast<unsigned long>(act->sa_handler);
        kact.mask = kernel_sigmask(&act->sa_mask);
        kact.flags = act->sa_flags;
        // mlibc leaves sa_restorer unset; always install our sigreturn trampoline
        // so a returning handler restores context instead of jumping to garbage.
        kact.restorer = reinterpret_cast<unsigned long>(&__instantos_sigrestorer);
    }

    long ret = __do_syscall3(INSTANTOS_SYS_SIGACTION, signum,
                             act ? reinterpret_cast<long>(&kact) : 0,
                             oldact ? reinterpret_cast<long>(&kold) : 0);
    if (int e = sc_error(ret); e) return e;

    if (oldact) {
        oldact->sa_handler = reinterpret_cast<void (*)(int)>(kold.handler);
        mlibc_sigmask(&oldact->sa_mask, kold.mask);
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
    if (newfd < 0 || newfd >= fd_limit()) return EBADF;

    // Determine the kernel target handle for newfd. For stdio slots the kernel
    // accepts the encoded File slot; otherwise allocate against the existing.
    long* targetSlot = fd_slot(newfd, true);
    if (!targetSlot) return ENOMEM;
    long targetHandle = __atomic_load_n(targetSlot, __ATOMIC_ACQUIRE);
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
        if (r >= 0) free_fd(r);
        if (w >= 0) free_fd(w);
        __do_syscall1(INSTANTOS_SYS_CLOSE, handles[0]);
        __do_syscall1(INSTANTOS_SYS_CLOSE, handles[1]);
        return EMFILE;
    }
    fds[0] = r;
    fds[1] = w;
    return 0;
}

int Sysdeps<Openat>::operator()(int dirfd, const char* path, int flags, mode_t mode, int* fd) {
    // Resolve `path` against the directory fd (openat with a real dirfd is used
    // heavily by coreutils/find fts).
    char buf[512]; int e;
    const char* p = at_resolve(dirfd, path, buf, sizeof(buf), &e);
    if (!p) return e;
    long ret = __do_syscall3(INSTANTOS_SYS_OPEN, reinterpret_cast<long>(p), flags, mode);
    if (int e2 = sc_error(ret); e2) return e2;
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
    // Preserve the fd-number -> kernel-handle map across the image replacement so
    // inherited fds beyond stdio (e.g. a `make -j` jobserver pipe on fd 3/4)
    // survive exec. The kernel keeps the handle table (dropping CLOEXEC); this
    // snapshot lets the new image's libc rebuild its userspace fd map at entry
    // (see __instantos_restore_fdtable / entry.cpp).
    const int top = fd_top();
    long buffer = 0;
    long bytes = 0;
    if (top <= kFdChunk) {
        __do_syscall2(INSTANTOS_SYS_FDTABLE_STASH, reinterpret_cast<long>(fdChunk0), top);
    } else {
        // The chunks one after the other, in a buffer exec does away with.
        bytes = static_cast<long>(top) * static_cast<long>(sizeof(long));
        buffer = __do_syscall3(INSTANTOS_SYS_MMAP, 0, bytes, 3);
        if (sc_error(buffer)) return ENOMEM;
        long* table = reinterpret_cast<long*>(buffer);
        for (int fd = 0; fd < top; ++fd) {
            long* slot = fd_slot(fd, false);
            table[fd] = slot ? __atomic_load_n(slot, __ATOMIC_RELAXED) : 0;
        }
        __do_syscall2(INSTANTOS_SYS_FDTABLE_STASH, buffer, top);
    }
    long ret = __do_syscall3(INSTANTOS_SYS_EXEC, reinterpret_cast<long>(path),
                             reinterpret_cast<long>(argv), reinterpret_cast<long>(envp));
    // Only returns on failure.
    if (buffer) __do_syscall2(INSTANTOS_SYS_MUNMAP, buffer, bytes);
    return sc_error(ret);
}

// --- coreutils Tier 1: thin wrappers over existing kernel syscalls ---------

int Sysdeps<Kill>::operator()(pid_t pid, int sig) {
    long ret = __do_syscall2(INSTANTOS_SYS_KILL, pid, sig);
    return sc_error(ret);
}

// One thread, with si_code SI_TKILL (what pthread_cancel's handler checks).
int Sysdeps<Tgkill>::operator()(int tgid, int tid, int sig) {
    if (tid <= 0) return ESRCH;   // it has ended
    return sc_error(__do_syscall3(INSTANTOS_SYS_TGKILL, tgid, tid, sig));
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

int Sysdeps<Renameat>::operator()(int olddirfd, const char* old_path,
                                  int newdirfd, const char* new_path) {
    // mlibc's rename() also routes through Renameat (with AT_FDCWD). Resolve
    // each path against its directory fd so renameat(dirfd, ...) works too.
    char ob[512], nb[512]; int e1, e2;
    const char* op = at_resolve(olddirfd, old_path, ob, sizeof(ob), &e1);
    if (!op) return e1;
    const char* np = at_resolve(newdirfd, new_path, nb, sizeof(nb), &e2);
    if (!np) return e2;
    long ret = __do_syscall2(INSTANTOS_SYS_RENAME, reinterpret_cast<long>(op),
                             reinterpret_cast<long>(np));
    return sc_error(ret);
}

// --- *at variants: coreutils invokes these instead of the base calls -------
// Each resolves the path against its directory fd (at_resolve) and delegates to
// the existing kernel syscall.

int Sysdeps<Fchmodat>::operator()(int dirfd, const char* pathname, mode_t mode, int flags) {
    char buf[512]; int e;
    const char* p = at_resolve(dirfd, pathname, buf, sizeof(buf), &e);
    if (!p) return e;
    // As Linux: AT_SYMLINK_NOFOLLOW on a symlink is EOPNOTSUPP (links have no
    // mode of their own), anything else is changed as usual. GNU tar, as root,
    // sets every extracted symlink's mode this way and ignores only that error;
    // following the link instead failed on links whose target was not
    // extracted yet (mlibc's abi-bits headers), and tar exited 2 (P7.3).
    if (flags & AT_SYMLINK_NOFOLLOW) {
        struct stat st;
        if (int se = sysdep<Stat>(fsfd_target::path, AT_FDCWD, p, AT_SYMLINK_NOFOLLOW, &st); se) return se;
        if (S_ISLNK(st.st_mode)) return EOPNOTSUPP;
    }
    long ret = __do_syscall3(INSTANTOS_SYS_CHMOD, reinterpret_cast<long>(p),
                             static_cast<long>(mode), 0);
    return sc_error(ret);
}

int Sysdeps<Linkat>::operator()(int olddirfd, const char* old_path,
                                int newdirfd, const char* new_path, int /*flags*/) {
    char ob[512], nb[512]; int e1, e2;
    const char* op = at_resolve(olddirfd, old_path, ob, sizeof(ob), &e1);
    if (!op) return e1;
    const char* np = at_resolve(newdirfd, new_path, nb, sizeof(nb), &e2);
    if (!np) return e2;
    long ret = __do_syscall2(INSTANTOS_SYS_LINK, reinterpret_cast<long>(op),
                             reinterpret_cast<long>(np));
    return sc_error(ret);
}

int Sysdeps<Symlinkat>::operator()(const char* target_path, int dirfd, const char* link_path) {
    // target_path is the symlink's contents (not resolved); only link_path is
    // resolved against the directory fd.
    char buf[512]; int e;
    const char* lp = at_resolve(dirfd, link_path, buf, sizeof(buf), &e);
    if (!lp) return e;
    long ret = __do_syscall2(INSTANTOS_SYS_SYMLINK, reinterpret_cast<long>(target_path),
                             reinterpret_cast<long>(lp));
    return sc_error(ret);
}

int Sysdeps<Readlinkat>::operator()(int dirfd, const char* path, void* buffer,
                                    size_t max_size, ssize_t* length) {
    char buf[512]; int e;
    const char* p = at_resolve(dirfd, path, buf, sizeof(buf), &e);
    if (!p) return e;
    long ret = __do_syscall3(INSTANTOS_SYS_READLINK, reinterpret_cast<long>(p),
                             reinterpret_cast<long>(buffer), static_cast<long>(max_size));
    if (int e2 = sc_error(ret); e2) return e2;
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
    out->f_fsid = ks.fsType;
    out->f_flag = 0;
    out->f_namemax = ks.nameMax;
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
    // Path-based: resolve against the directory fd. flags may carry
    // AT_SYMLINK_NOFOLLOW for lchown.
    char buf[512]; int e;
    const char* p = at_resolve(dirfd, pathname, buf, sizeof(buf), &e);
    if (!p) return e;
    long ret = __do_syscall5(INSTANTOS_SYS_CHOWN, reinterpret_cast<long>(p), 0,
                             static_cast<long>(owner), static_cast<long>(group),
                             static_cast<long>(flags));
    return sc_error(ret);
}

int Sysdeps<Mknodat>::operator()(int dirfd, const char* path, int mode, int dev) {
    // The kernel sys_mknod selects the node type from the S_IFMT bits in `mode`.
    char buf[512]; int e;
    const char* p = at_resolve(dirfd, path, buf, sizeof(buf), &e);
    if (!p) return e;
    long ret = __do_syscall3(INSTANTOS_SYS_MKNOD, reinterpret_cast<long>(p),
                             static_cast<long>(static_cast<unsigned>(mode)),
                             static_cast<long>(static_cast<unsigned>(dev)));
    return sc_error(ret);
}

int Sysdeps<Mkfifoat>::operator()(int dirfd, const char* path, mode_t mode) {
    // mkfifo always creates a FIFO: force the S_IFIFO type bit.
    char buf[512]; int e;
    const char* p = at_resolve(dirfd, path, buf, sizeof(buf), &e);
    if (!p) return e;
    constexpr unsigned kSIFIFO = 0x1000;
    long ret = __do_syscall3(INSTANTOS_SYS_MKNOD, reinterpret_cast<long>(p),
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

// The kernel keeps RLIMIT_NOFILE (inherited, kept across exec; this table
// stays under its soft limit); the other resources read as unlimited.
int Sysdeps<GetRlimit>::operator()(int resource, struct rlimit* limit) {
    if (!limit) return EINVAL;
    struct { unsigned long cur, max; } r = {0, 0};
    if (int e = sc_error(__do_syscall2(INSTANTOS_SYS_GETRLIMIT, resource, reinterpret_cast<long>(&r))); e) return e;
    limit->rlim_cur = r.cur == ~0UL ? RLIM_INFINITY : r.cur;
    limit->rlim_max = r.max == ~0UL ? RLIM_INFINITY : r.max;
    return 0;
}

int Sysdeps<SetRlimit>::operator()(int resource, const struct rlimit* limit) {
    if (!limit) return EINVAL;
    struct { unsigned long cur, max; } r = {
        limit->rlim_cur == RLIM_INFINITY ? ~0UL : static_cast<unsigned long>(limit->rlim_cur),
        limit->rlim_max == RLIM_INFINITY ? ~0UL : static_cast<unsigned long>(limit->rlim_max)};
    if (int e = sc_error(__do_syscall2(INSTANTOS_SYS_SETRLIMIT, resource, reinterpret_cast<long>(&r))); e) return e;
    if (resource == RLIMIT_NOFILE) {
        __atomic_store_n(&fdSoftLimit, 0L, __ATOMIC_RELAXED);   // ask again
    }
    return 0;
}

int Sysdeps<Times>::operator()(struct tms* tms, clock_t* out) {
    // No per-process CPU accounting yet. Report zero CPU time and the
    // monotonic clock as the "elapsed" tick return (times() returns a clock_t
    // of elapsed real time), in mlibc's sysconf(_SC_CLK_TCK) ticks: 1000000.
    if (tms) {
        tms->tms_utime = 0;
        tms->tms_stime = 0;
        tms->tms_cutime = 0;
        tms->tms_cstime = 0;
    }
    long ns = __do_syscall0(INSTANTOS_SYS_GETTIME_NS);
    if (out) *out = static_cast<clock_t>(ns / 1000);
    return 0;
}

int Sysdeps<ClockSet>::operator()(int clock, time_t secs, long nanos) {
    // Only CLOCK_REALTIME can be set (root only); the RTC itself is left alone.
    if (clock != 0 || secs < 0 || nanos < 0 || nanos >= 1000000000L) return EINVAL;
    long ret = __do_syscall1(INSTANTOS_SYS_SETUNIXTIME, secs * 1000000000L + nanos);
    return sc_error(ret);
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

int Sysdeps<Fchdir>::operator()(int fd) {
    // chdir is path-based; resolve the fd to its directory path and chdir there.
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    char dir[256];
    long n = __do_syscall3(INSTANTOS_SYS_FD_PATH, handle,
                           reinterpret_cast<long>(dir), static_cast<long>(sizeof(dir)));
    if (int e = sc_error(n); e) return e;
    return sc_error(__do_syscall1(INSTANTOS_SYS_CHDIR, reinterpret_cast<long>(dir)));
}

int Sysdeps<Utimensat>::operator()(int dirfd, const char* pathname,
                                   const struct timespec times[2], int flags) {
    // The kernel Utime takes second-resolution atime/mtime and has no OMIT
    // concept, so resolve UTIME_NOW/UTIME_OMIT (and a null times) here.
    //
    // Two modes:
    //   pathname != null -> utimensat(dirfd, path, ...): resolve to an absolute
    //     path and use the path-based Utime.
    //   pathname == null -> futimens(fd, ...) is routed here (mlibc calls this
    //     sysdep with dirfd == fd, pathname == null). Operate on the open file
    //     handle via the by-handle Utime; the *at path resolver rejects a null
    //     path with EINVAL, so it must not be used for this case.
    const bool byHandle = (pathname == nullptr);
    char buf[512];
    long target;                 // absolute-path pointer, or a kernel fd handle
    if (byHandle) {
        target = handle_for_fd(dirfd);
        if (target < 0) return EBADF;
    } else {
        int rerr;
        const char* p = at_resolve(dirfd, pathname, buf, sizeof(buf), &rerr);
        if (!p) return rerr;
        target = reinterpret_cast<long>(p);
        // The kernel's Utime follows links and a link has no times of its own
        // here: AT_SYMLINK_NOFOLLOW on a symlink is EOPNOTSUPP, which GNU tar
        // ignores for links. Following it instead set the target's times, or
        // failed (ENOENT) on a link extracted before its target, and tar
        // exited 2 unpacking the toolchain tarball.
        if (flags & AT_SYMLINK_NOFOLLOW) {
            instantos_stat ls;
            long lret = __do_syscall2(INSTANTOS_SYS_LSTAT, target, reinterpret_cast<long>(&ls));
            if (int e = sc_error(lret); e) return e;
            if ((ls.k_mode & 0170000) == 0120000) return EOPNOTSUPP;
        }
    }

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
    if (needExisting) {
        instantos_stat ks;
        long sret = byHandle
            ? __do_syscall2(INSTANTOS_SYS_FSTAT, target, reinterpret_cast<long>(&ks))
            : __do_syscall2((flags & AT_SYMLINK_NOFOLLOW) ? INSTANTOS_SYS_LSTAT
                                                          : INSTANTOS_SYS_STAT,
                            target, reinterpret_cast<long>(&ks));
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

    // sys_utime(target = path pointer or fd handle, atime, mtime, byHandle)
    long ret = __do_syscall4(INSTANTOS_SYS_UTIME, target, atime, mtime, byHandle ? 1 : 0);
    return sc_error(ret);
}

int Sysdeps<Access>::operator()(const char* path, int mode) {
    // sys_access(path, mode) — mode bits F_OK=0, X_OK=1, W_OK=2, R_OK=4.
    long ret = __do_syscall2(INSTANTOS_SYS_ACCESS, reinterpret_cast<long>(path),
                             static_cast<long>(mode));
    return sc_error(ret);
}

int Sysdeps<Faccessat>::operator()(int dirfd, const char* pathname, int mode, int flags) {
    // Resolve against the directory fd; AT_* flags (AT_EACCESS) are moot under
    // the single-credential model.
    (void)flags;
    char buf[512]; int e;
    const char* p = at_resolve(dirfd, pathname, buf, sizeof(buf), &e);
    if (!p) return e;
    long ret = __do_syscall2(INSTANTOS_SYS_ACCESS, reinterpret_cast<long>(p),
                             static_cast<long>(mode));
    return sc_error(ret);
}

// --- P2.1d: durability, positioned I/O, alt signal stack, rusage, ttyname ---

int Sysdeps<Fsync>::operator()(int fd) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    return sc_error(__do_syscall1(INSTANTOS_SYS_FSYNC, handle));
}

// sync(2): commit every ext4 volume's running transaction and flush it
// (StorageSync, mode 0 = All). Without it mlibc's sync() did nothing.
void Sysdeps<Sync>::operator()() {
    __do_syscall1(INSTANTOS_SYS_STORAGE_SYNC, 0);
}

int Sysdeps<Fdatasync>::operator()(int fd) {
    // The kernel fsync flushes data + metadata; fdatasync maps to the same call.
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    return sc_error(__do_syscall1(INSTANTOS_SYS_FSYNC, handle));
}

int Sysdeps<Pread>::operator()(int fd, void* buf, size_t n, off_t off, ssize_t* bytes_read) {
    // No positioned-read syscall: save the current offset, seek+read, restore.
    // Single-threaded-safe (an fd is not shared concurrently within a process).
    constexpr long kSeekSet = 0, kSeekCur = 1;
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    long saved = __do_syscall3(INSTANTOS_SYS_SEEK, handle, 0, kSeekCur);
    if (int e = sc_error(saved); e) return e;
    if (int e = sc_error(__do_syscall3(INSTANTOS_SYS_SEEK, handle, static_cast<long>(off), kSeekSet)); e)
        return e;
    long r = __do_syscall3(INSTANTOS_SYS_READ, handle, reinterpret_cast<long>(buf), static_cast<long>(n));
    int rerr = sc_error(r);
    __do_syscall3(INSTANTOS_SYS_SEEK, handle, saved, kSeekSet);   // restore offset
    if (rerr) return rerr;
    *bytes_read = r;
    return 0;
}

int Sysdeps<Pwrite>::operator()(int fd, const void* buf, size_t n, off_t off, ssize_t* bytes_written) {
    constexpr long kSeekSet = 0, kSeekCur = 1;
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    long saved = __do_syscall3(INSTANTOS_SYS_SEEK, handle, 0, kSeekCur);
    if (int e = sc_error(saved); e) return e;
    if (int e = sc_error(__do_syscall3(INSTANTOS_SYS_SEEK, handle, static_cast<long>(off), kSeekSet)); e)
        return e;
    long w = __do_syscall3(INSTANTOS_SYS_WRITE, handle, reinterpret_cast<long>(buf), static_cast<long>(n));
    int werr = sc_error(w);
    __do_syscall3(INSTANTOS_SYS_SEEK, handle, saved, kSeekSet);
    if (werr) return werr;
    *bytes_written = w;
    return 0;
}

int Sysdeps<Sigaltstack>::operator()(const stack_t* ss, stack_t* oss) {
    // Kernel SignalStackInfo layout: {u64 sp; u64 size; u32 flags; u32 reserved}.
    // mlibc's stack_t orders the fields differently, so translate both ways.
    struct instantos_sigaltstack {
        unsigned long sp;
        unsigned long size;
        unsigned int flags;
        unsigned int reserved;
    };
    instantos_sigaltstack in{};
    instantos_sigaltstack out{};
    if (ss) {
        in.sp = reinterpret_cast<unsigned long>(ss->ss_sp);
        in.size = static_cast<unsigned long>(ss->ss_size);
        in.flags = static_cast<unsigned int>(ss->ss_flags);
    }
    long ret = __do_syscall2(INSTANTOS_SYS_SIGALTSTACK,
                             ss ? reinterpret_cast<long>(&in) : 0,
                             oss ? reinterpret_cast<long>(&out) : 0);
    if (int e = sc_error(ret); e) return e;
    if (oss) {
        oss->ss_sp = reinterpret_cast<void*>(out.sp);
        oss->ss_size = static_cast<size_t>(out.size);
        oss->ss_flags = static_cast<int>(out.flags);
    }
    return 0;
}

int Sysdeps<GetRusage>::operator()(int, struct rusage* usage) {
    // No per-process resource accounting yet; report an all-zero rusage. Callers
    // (the `time` builtin, make's timing) tolerate zeros.
    if (usage) {
        char* p = reinterpret_cast<char*>(usage);
        for (size_t i = 0; i < sizeof(*usage); ++i) p[i] = 0;
    }
    return 0;
}

int Sysdeps<Ttyname>::operator()(int fd, char* buf, size_t size) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    // Must refer to a terminal.
    struct instantos_winsize ws;
    if (sc_error(__do_syscall3(INSTANTOS_SYS_IOCTL, handle, TIOCGWINSZ, reinterpret_cast<long>(&ws)))) {
        return ENOTTY;
    }
    // Report the path the terminal was opened with (e.g. /dev/pts/N).
    long n = __do_syscall3(INSTANTOS_SYS_FD_PATH, handle, reinterpret_cast<long>(buf),
                           static_cast<long>(size));
    if (sc_error(n)) return ENOTTY;
    return 0;
}

// --- Sockets -------------------------------------------------------------
// The kernel exposes a BSD-style socket API (see src/cpu/syscall/socket.cpp).
// Socket handles are ordinary kernel object handles, so they flow through the
// same fdTable used for files: a successful socket()/accept() returns a kernel
// handle which we register as a small integer fd.  The kernel address layout
// is a verbatim Linux `struct sockaddr_in` (AF_INET == 2, port and address in
// network byte order), so addresses pass through untouched.
//
// The kernel send/recv/sendto/recvfrom operate on a single flat buffer. For
// streams MsgSend/MsgRecv loop over the iovecs; a datagram must stay one
// datagram, so it is gathered into (or scattered from) a bounce buffer.

namespace {

bool socket_is_datagram(long handle) {
    int type = 0;
    socklen_t len = sizeof(type);
    long g = __do_syscall5(INSTANTOS_SYS_GETSOCKOPT, handle, SOL_SOCKET, SO_TYPE,
                           reinterpret_cast<long>(&type), reinterpret_cast<long>(&len));
    return sc_error(g) == 0 && type == SOCK_DGRAM;
}

// One send of `size` bytes, to `dest` if given (sendto) or the connected peer.
long socket_send_one(long handle, const void* buffer, size_t size, int flags,
                     const struct sockaddr* dest, socklen_t dest_length) {
    if (dest != nullptr && dest_length != 0) {
        return __do_syscall5(INSTANTOS_SYS_SENDTO, handle, reinterpret_cast<long>(buffer),
                             static_cast<long>(size), static_cast<long>(flags),
                             reinterpret_cast<long>(dest));
    }
    return __do_syscall4(INSTANTOS_SYS_SEND, handle, reinterpret_cast<long>(buffer),
                         static_cast<long>(size), static_cast<long>(flags));
}

// One recv; the source address (a sockaddr_in) is stored to `from` when
// asked for, with its length capped by *from_length.
long socket_recv_one(long handle, void* buffer, size_t size, int flags,
                     struct sockaddr* from, socklen_t* from_length) {
    if (from == nullptr || from_length == nullptr) {
        return __do_syscall4(INSTANTOS_SYS_RECV, handle, reinterpret_cast<long>(buffer),
                             static_cast<long>(size), static_cast<long>(flags));
    }
    struct sockaddr_in source;
    memset(&source, 0, sizeof(source));
    long ret = __do_syscall5(INSTANTOS_SYS_RECVFROM, handle, reinterpret_cast<long>(buffer),
                             static_cast<long>(size), static_cast<long>(flags),
                             reinterpret_cast<long>(&source));
    if (sc_error(ret) == 0) {
        socklen_t n = *from_length < sizeof(source) ? *from_length : sizeof(source);
        memcpy(from, &source, n);
        *from_length = sizeof(source);
    }
    return ret;
}


// AF_UNIX: SocketSendMsg/SocketRecvMsg take one flat buffer plus kernel
// handles (SCM_RIGHTS). Other sockets answer EOPNOTSUPP, and the callers go
// on with Send/SendTo/Recv as before.
struct instantos_socket_message {
    unsigned long buffer;
    unsigned long length;
    unsigned long handles;
    unsigned int handleCount;
    unsigned int flags;
};

constexpr int kMaxPassedFds = 253;   // SCM_MAX_FD

size_t iov_total(const struct msghdr* hdr) {
    size_t size = 0;
    for (int i = 0; i < hdr->msg_iovlen; ++i) size += hdr->msg_iov[i].iov_len;
    return size;
}

// -1: not an AF_UNIX socket (use the old path); else 0 or an errno.
int unix_msg_send(long handle, const struct msghdr* hdr, int flags, ssize_t* length) {
    long handles[kMaxPassedFds];
    unsigned int count = 0;
    if (hdr->msg_control && hdr->msg_controllen >= sizeof(struct cmsghdr)) {
        auto* h = const_cast<struct msghdr*>(hdr);
        for (struct cmsghdr* c = CMSG_FIRSTHDR(h); c; c = CMSG_NXTHDR(h, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            const size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            const int* fds = reinterpret_cast<const int*>(CMSG_DATA(c));
            for (size_t i = 0; i < n; ++i) {
                if (count == kMaxPassedFds) return EINVAL;
                const long handle = handle_for_fd(fds[i]);
                if (handle < 0) return EBADF;
                handles[count++] = handle;
            }
        }
    }
    const size_t size = iov_total(hdr);
    const void* data = hdr->msg_iovlen == 1 ? hdr->msg_iov[0].iov_base : nullptr;
    char* bounce = nullptr;
    if (!data && size != 0) {
        bounce = static_cast<char*>(getAllocator().allocate(size));
        if (!bounce) return ENOMEM;
        size_t off = 0;
        for (int i = 0; i < hdr->msg_iovlen; ++i) {
            memcpy(bounce + off, hdr->msg_iov[i].iov_base, hdr->msg_iov[i].iov_len);
            off += hdr->msg_iov[i].iov_len;
        }
        data = bounce;
    }
    instantos_socket_message m;
    m.buffer = reinterpret_cast<unsigned long>(data);
    m.length = size;
    m.handles = reinterpret_cast<unsigned long>(handles);
    m.handleCount = count;
    m.flags = 0;
    long ret = __do_syscall3(INSTANTOS_SYS_SOCKET_SENDMSG, handle, reinterpret_cast<long>(&m), flags);
    if (bounce) getAllocator().free(bounce);
    int e = sc_error(ret);
    if (e == EOPNOTSUPP) return count ? EINVAL : -1;   // SCM_RIGHTS on an IPv4 socket
    if (e) return e;
    if (length) *length = ret;
    return 0;
}

int unix_msg_recv(long handle, struct msghdr* hdr, int flags, ssize_t* length) {
    const size_t size = iov_total(hdr);
    void* target = hdr->msg_iovlen == 1 ? hdr->msg_iov[0].iov_base : nullptr;
    char* bounce = nullptr;
    if (!target && size != 0) {
        bounce = static_cast<char*>(getAllocator().allocate(size));
        if (!bounce) return ENOMEM;
        target = bounce;
    }
    unsigned int capacity = 0;
    if (hdr->msg_control && hdr->msg_controllen >= CMSG_LEN(sizeof(int))) {
        capacity = (hdr->msg_controllen - CMSG_LEN(0)) / sizeof(int);
        if (capacity > kMaxPassedFds) capacity = kMaxPassedFds;
    }
    long handles[kMaxPassedFds];
    instantos_socket_message m;
    m.buffer = reinterpret_cast<unsigned long>(target);
    m.length = size;
    m.handles = reinterpret_cast<unsigned long>(handles);
    m.handleCount = capacity;
    m.flags = 0;
    long ret = __do_syscall3(INSTANTOS_SYS_SOCKET_RECVMSG, handle, reinterpret_cast<long>(&m), flags);
    int e = sc_error(ret);
    if (e) {
        if (bounce) getAllocator().free(bounce);
        return e == EOPNOTSUPP ? -1 : e;
    }
    if (bounce) {
        size_t off = 0;
        for (int i = 0; i < hdr->msg_iovlen && off < static_cast<size_t>(ret); ++i) {
            size_t n = hdr->msg_iov[i].iov_len;
            if (n > static_cast<size_t>(ret) - off) n = static_cast<size_t>(ret) - off;
            memcpy(hdr->msg_iov[i].iov_base, bounce + off, n);
            off += n;
        }
        getAllocator().free(bounce);
    }
    int msgFlags = static_cast<int>(m.flags);
    size_t controlUsed = 0;
    if (m.handleCount != 0) {
        struct cmsghdr* c = CMSG_FIRSTHDR(hdr);
        int* fds = reinterpret_cast<int*>(CMSG_DATA(c));
        unsigned int registered = 0;
        for (unsigned int i = 0; i < m.handleCount; ++i) {
            int fd = register_fd(handles[i]);
            if (fd < 0) {
                __do_syscall1(INSTANTOS_SYS_CLOSE, handles[i]);
                msgFlags |= MSG_CTRUNC;
                continue;
            }
            fds[registered++] = fd;
        }
        if (registered != 0) {
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(registered * sizeof(int));
            controlUsed = CMSG_SPACE(registered * sizeof(int));
            if (controlUsed > hdr->msg_controllen) controlUsed = hdr->msg_controllen;
        }
    }
    hdr->msg_controllen = controlUsed;
    hdr->msg_namelen = 0;
    hdr->msg_flags = msgFlags;
    if (length) *length = ret;
    return 0;
}
}  // namespace

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
    long ret = socket_send_one(handle, buffer, size, flags, sock_addr, addr_length);
    if (int e = sc_error(ret); e) return e;
    if (length) *length = ret;
    return 0;
}

int Sysdeps<Recvfrom>::operator()(int fd, void* buffer, size_t size, int flags,
                                  struct sockaddr* sock_addr, socklen_t* addr_length,
                                  ssize_t* length) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    long ret = socket_recv_one(handle, buffer, size, flags, sock_addr, addr_length);
    if (int e = sc_error(ret); e) return e;
    if (length) *length = ret;
    return 0;
}

int Sysdeps<MsgSend>::operator()(int fd, const struct msghdr* hdr, int flags, ssize_t* length) {
    long handle = handle_for_fd(fd);
    if (handle < 0) return EBADF;
    if (int u = unix_msg_send(handle, hdr, flags, length); u >= 0) return u;
    const auto* dest = static_cast<const struct sockaddr*>(hdr->msg_name);

    if (socket_is_datagram(handle)) {
        size_t size = 0;
        for (int i = 0; i < hdr->msg_iovlen; ++i) size += hdr->msg_iov[i].iov_len;
        const void* data = hdr->msg_iovlen == 1 ? hdr->msg_iov[0].iov_base : nullptr;
        char* bounce = nullptr;
        if (!data && size != 0) {
            bounce = static_cast<char*>(getAllocator().allocate(size));
            if (!bounce) return ENOMEM;
            size_t off = 0;
            for (int i = 0; i < hdr->msg_iovlen; ++i) {
                memcpy(bounce + off, hdr->msg_iov[i].iov_base, hdr->msg_iov[i].iov_len);
                off += hdr->msg_iov[i].iov_len;
            }
            data = bounce;
        }
        long ret = socket_send_one(handle, data, size, flags, dest, hdr->msg_namelen);
        if (bounce) getAllocator().free(bounce);
        if (int e = sc_error(ret); e) return e;
        if (length) *length = ret;
        return 0;
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
    if (int u = unix_msg_recv(handle, hdr, flags, length); u >= 0) return u;

    if (socket_is_datagram(handle)) {
        size_t size = 0;
        for (int i = 0; i < hdr->msg_iovlen; ++i) size += hdr->msg_iov[i].iov_len;
        char* bounce = nullptr;
        void* target = hdr->msg_iovlen == 1 ? hdr->msg_iov[0].iov_base : nullptr;
        if (!target) {
            bounce = static_cast<char*>(getAllocator().allocate(size ? size : 1));
            if (!bounce) return ENOMEM;
            target = bounce;
        }
        socklen_t namelen = hdr->msg_namelen;
        long ret = socket_recv_one(handle, target, size, flags,
                                   static_cast<struct sockaddr*>(hdr->msg_name),
                                   hdr->msg_name ? &namelen : nullptr);
        if (int e = sc_error(ret); e) {
            if (bounce) getAllocator().free(bounce);
            return e;
        }
        if (bounce) {
            size_t off = 0;
            for (int i = 0; i < hdr->msg_iovlen && off < static_cast<size_t>(ret); ++i) {
                size_t n = hdr->msg_iov[i].iov_len;
                if (n > static_cast<size_t>(ret) - off) n = static_cast<size_t>(ret) - off;
                memcpy(hdr->msg_iov[i].iov_base, bounce + off, n);
                off += n;
            }
            getAllocator().free(bounce);
        }
        hdr->msg_namelen = hdr->msg_name ? namelen : 0;
        hdr->msg_controllen = 0;
        hdr->msg_flags = 0;
        if (length) *length = ret;
        return 0;
    }

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

int Sysdeps<Socketpair>::operator()(int domain, int type_and_flags, int proto, int* fds) {
    long handles[2] = {0, 0};
    long ret = __do_syscall4(INSTANTOS_SYS_SOCKETPAIR, domain, type_and_flags, proto,
                             reinterpret_cast<long>(handles));
    if (int e = sc_error(ret); e) return e;
    int a = register_fd(handles[0]);
    int b = a >= 0 ? register_fd(handles[1]) : -1;
    if (a < 0 || b < 0) {
        if (a >= 0) free_fd(a);
        __do_syscall1(INSTANTOS_SYS_CLOSE, handles[0]);
        __do_syscall1(INSTANTOS_SYS_CLOSE, handles[1]);
        return EMFILE;
    }
    fds[0] = a;
    fds[1] = b;
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

}  // namespace mlibc

// --- New-thread entry path (global, C-linkage) ---------------------------
//
// Excluded from the runtime linker for the same reason as the sysdeps above.
#ifndef MLIBC_BUILDING_RTLD
//
// sys_thread_clone() starts a freshly spawned thread here with rsp pointing at
// the stack laid out by Sysdeps<PrepareStack>: [entry][tcb][arg][nul]. Pop those
// into the SysV argument registers and tail into the C trampoline. No TLS (fs)
// access may occur here — the trampoline installs it via TcbSet first.
asm(
    ".pushsection .text\n"
    ".globl __mlibc_thread_entry\n"
    ".type __mlibc_thread_entry, @function\n"
    "__mlibc_thread_entry:\n"
    "    popq %rdi\n"   // entry (start routine)
    "    popq %rsi\n"   // tcb
    "    popq %rdx\n"   // user_arg
    "    popq %rcx\n"   // nul filler
    "    call __mlibc_thread_trampoline\n"
    ".size __mlibc_thread_entry, . - __mlibc_thread_entry\n"
    ".popsection\n"
);

// Runs on the new thread's own stack. Marked no_stack_protector because it must
// not touch TLS (where the stack canary lives) before TcbSet has installed it.
extern "C" __attribute__((no_stack_protector)) void
__mlibc_thread_trampoline(void* (*fn)(void*), Tcb* tcb, void* arg) {
    if (mlibc::sysdep<TcbSet>(tcb))
        __ensure(!"instantos: failed to set TCB for new thread");

    // Wait until the parent publishes our TID.
    while (__atomic_load_n(&tcb->tid, __ATOMIC_RELAXED) == 0)
        mlibc::sysdep<FutexWait>(&tcb->tid, 0, nullptr);

    __atomic_fetch_or(&tcb->cancelBits, tcbCancelEnableBit, __ATOMIC_RELAXED);

    tcb->invokeThreadFunc(reinterpret_cast<void*>(fn), arg);

    mlibc::thread_exit(tcb->returnValue);
}

#endif  // !MLIBC_BUILDING_RTLD

// --- What Rust's std (x86_64-unknown-linux-musl) calls beyond POSIX --------
//
// Rust programs (tools/rust-port) are built for musl and linked against mlibc,
// whose headers follow the same Linux ABI. These are the musl/glibc functions
// std uses that mlibc lacks or keeps behind its Linux option (off here, so that
// configure scripts do not find epoll and the like).
#ifndef MLIBC_BUILDING_RTLD
#include <errno.h>
#include <pthread.h>
#include <spawn.h>
#include <stdlib.h>

namespace {
// musl's posix_spawn file action (mlibc's spawn.cpp has the same; it runs
// FDOP_CHDIR but has no function that adds one).
struct instantos_fdop {
    instantos_fdop *next, *prev;
    int cmd, fd, srcfd, oflag;
    mode_t mode;
    char path[];
};
constexpr int kFdopChdir = 4;
constexpr int kFdopFchdir = 5;

int add_fdop(posix_spawn_file_actions_t* fa, instantos_fdop* op) {
    op->prev = nullptr;
    if ((op->next = static_cast<instantos_fdop*>(fa->__actions))) op->next->prev = op;
    fa->__actions = op;
    return 0;
}

long futex_result(int e) {
    if (!e) return 0;
    errno = e;
    return -1;
}

// Linux's futex(2) as std uses it: WAIT (relative timeout) and WAIT_BITSET
// (absolute), WAKE, with any bitset taken as all and PRIVATE ignored.
long linux_futex(long uaddr, long op, long val, long timeoutPtr, long val3) {
    constexpr long kWait = 0, kWake = 1, kWaitBitset = 9, kWakeBitset = 10;
    constexpr long kClockRealtime = 256;
    auto* word = reinterpret_cast<int*>(uaddr);
    const long cmd = op & 127;
    if (cmd == kWake || cmd == kWakeBitset) {
        const long n = __do_syscall2(INSTANTOS_SYS_FUTEX_WAKE, uaddr, val < 0 ? 0x7fffffffL : val);
        if (int e = sc_error(n); e) return futex_result(e);
        return n;
    }
    if (cmd != kWait && cmd != kWaitBitset) return futex_result(ENOSYS);
    (void)val3;
    const auto* ts = reinterpret_cast<const struct timespec*>(timeoutPtr);
    if (!ts) return futex_result(mlibc::sysdep<FutexWait>(word, static_cast<int>(val), nullptr));
    struct timespec rel = *ts;
    if (cmd == kWaitBitset) {   // absolute: what is left of it
        struct timespec now;
        clock_gettime((op & kClockRealtime) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
        long long ns = (ts->tv_sec - now.tv_sec) * 1000000000LL + (ts->tv_nsec - now.tv_nsec);
        if (ns <= 0) return futex_result(ETIMEDOUT);
        rel.tv_sec = ns / 1000000000LL;
        rel.tv_nsec = ns % 1000000000LL;
    }
    return futex_result(mlibc::sysdep<FutexWait>(word, static_cast<int>(val), &rel));
}
}

// syscall(2) with Linux's numbers: the few std makes directly. The rest are
// ENOSYS, from which std falls back (statx, clone3, pidfd_*, copy_file_range).
extern "C" [[gnu::visibility("default")]] long syscall(long number, ...) {
    va_list ap;
    va_start(ap, number);
    long a[6];
    for (long& v : a) v = va_arg(ap, long);
    va_end(ap);
    switch (number) {
    case 202:   // futex
        return linux_futex(a[0], a[1], a[2], a[3], a[5]);
    case 318: {   // getrandom(buf, len, flags)
        auto* p = reinterpret_cast<unsigned char*>(a[0]);
        const size_t len = static_cast<size_t>(a[1]);
        for (size_t done = 0; done < len;) {
            const size_t n = len - done > 256 ? 256 : len - done;
            if (getentropy(p + done, n) != 0) return -1;
            done += n;
        }
        return static_cast<long>(len);
    }
    case 186:   // gettid
        return __do_syscall0(INSTANTOS_SYS_GETPID);
    case 39:    // getpid
        return getpid();
    default:
        errno = ENOSYS;
        return -1;
    }
}

extern "C" [[gnu::visibility("default")]] int posix_spawn_file_actions_addchdir_np(
        posix_spawn_file_actions_t* __restrict fa, const char* __restrict path) {
    auto* op = static_cast<instantos_fdop*>(malloc(sizeof(instantos_fdop) + strlen(path) + 1));
    if (!op) return ENOMEM;
    op->cmd = kFdopChdir;
    op->fd = -1;
    strcpy(op->path, path);
    return add_fdop(fa, op);
}

extern "C" [[gnu::visibility("default")]] int posix_spawn_file_actions_addfchdir_np(
        posix_spawn_file_actions_t* fa, int fd) {
    auto* op = static_cast<instantos_fdop*>(malloc(sizeof(instantos_fdop)));
    if (!op) return ENOMEM;
    op->cmd = kFdopFchdir;
    op->fd = fd;
    return add_fdop(fa, op);
}

// glibc's name for the XSI strerror_r, which musl's libc crate binding uses.
extern "C" [[gnu::visibility("default")]] int __xpg_strerror_r(int e, char* buffer, size_t size) {
    const char* text = strerror(e);
    const size_t n = strlen(text);
    if (!buffer || size == 0) return ERANGE;
    const size_t copy = n < size ? n : size - 1;
    memcpy(buffer, text, copy);
    buffer[copy] = 0;
    return copy < n ? ERANGE : 0;
}

// The CPUs this process may run on (SchedGetAffinity: one 64-bit word).
extern "C" [[gnu::visibility("default")]] int sched_getaffinity(pid_t pid, size_t size, cpu_set_t* set) {
    if (!set || size < 8) {
        errno = EINVAL;
        return -1;
    }
    unsigned long mask = 0;
    if (int e = sc_error(__do_syscall3(INSTANTOS_SYS_SCHED_GETAFFINITY, pid, 8, reinterpret_cast<long>(&mask))); e) {
        errno = e;
        return -1;
    }
    memset(set, 0, size);
    memcpy(set, &mask, sizeof(mask));
    return 0;
}

// A thread's stack: mlibc's own threads know theirs; the main thread's is the
// kernel's (32 pages under 0x7FFFFFFFE000 and an 8 MiB demand-zero reserve).
extern "C" [[gnu::visibility("default")]] int pthread_getattr_np(pthread_t thread, pthread_attr_t* attr) {
    auto* tcb = reinterpret_cast<Tcb*>(thread);
    *attr = pthread_attr_t{};
    if (tcb->stackAddr && tcb->stackSize) {
        attr->__mlibc_stackaddr = tcb->stackAddr;
        attr->__mlibc_stacksize = tcb->stackSize;
    } else {
        constexpr unsigned long kTop = 0x00007FFFFFFFE000UL;
        constexpr unsigned long kSize = 32 * 4096UL + 8 * 1024 * 1024UL;
        attr->__mlibc_stackaddr = reinterpret_cast<void*>(kTop - kSize);
        attr->__mlibc_stacksize = kSize;
    }
    attr->__mlibc_guardsize = tcb->guardSize;
    attr->__mlibc_detachstate = tcb->isJoinable ? PTHREAD_CREATE_JOINABLE : PTHREAD_CREATE_DETACHED;
    return 0;
}
#endif  // !MLIBC_BUILDING_RTLD
