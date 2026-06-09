#include <abi-bits/errno.h>
#include <bits/ensure.h>
#include <bits/syscall.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/tcb.hpp>
#include <string.h>
#include <time.h>

extern "C" long __do_syscall0(long);
extern "C" long __do_syscall1(long, long);
extern "C" long __do_syscall2(long, long, long);
extern "C" long __do_syscall3(long, long, long, long);
extern "C" long __do_syscall4(long, long, long, long, long);
extern "C" long __do_syscall5(long, long, long, long, long, long);

namespace {
constexpr int kMaxFd = 256;
long fdTable[kMaxFd] = {0, 1, 2};

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
}

int Sysdeps<Isatty>::operator()(int) {
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
    long handle = handle_for_fd(fd);
    if (handle < 0 || fd < 3) return EBADF;
    long ret = __do_syscall1(INSTANTOS_SYS_CLOSE, handle);
    if (int e = sc_error(ret); e) return e;
    fdTable[fd] = 0;
    return sc_error(ret);
}

int Sysdeps<Seek>::operator()(int, off_t, int, off_t*) {
    return ESPIPE;
}

[[noreturn]] void Sysdeps<Exit>::operator()(int status) {
    __do_syscall1(INSTANTOS_SYS_EXIT, status);
    __builtin_unreachable();
}

pid_t Sysdeps<GetPid>::operator()() {
    return __do_syscall0(INSTANTOS_SYS_GETPID);
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

int Sysdeps<VmProtect>::operator()(void*, size_t, int) {
    return 0;
}

int Sysdeps<ClockGet>::operator()(int, time_t* secs, long* nanos) {
    long ms = __do_syscall0(INSTANTOS_SYS_GETTIME);
    if (int e = sc_error(ms); e) return e;
    *secs = ms / 1000;
    *nanos = (ms % 1000) * 1000000;
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

}
