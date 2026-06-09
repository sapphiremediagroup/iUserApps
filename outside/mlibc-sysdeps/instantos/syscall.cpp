#include <bits/syscall.h>
#include <errno.h>

extern "C" long __do_syscall_ret(unsigned long ret) {
#ifdef MLIBC_BUILDING_RTLD
    return static_cast<long>(ret);
#else
    if (ret > static_cast<unsigned long>(-4096)) {
        errno = -static_cast<long>(ret);
        return -1;
    }
    return static_cast<long>(ret);
#endif
}

using sc_word_t = long;

extern "C" sc_word_t __do_syscall0(long sc) {
    register sc_word_t ret asm("rax");
    register sc_word_t num asm("rax") = sc;
    asm volatile("syscall" : "=a"(ret) : "a"(num) : "rcx", "r11", "memory");
    return ret;
}

extern "C" sc_word_t __do_syscall1(long sc, sc_word_t arg1) {
    register sc_word_t ret asm("rax");
    register sc_word_t num asm("rax") = sc;
    register sc_word_t a1 asm("rbx") = arg1;
    asm volatile("syscall" : "=a"(ret) : "a"(num), "r"(a1) : "rcx", "r11", "memory");
    return ret;
}

extern "C" sc_word_t __do_syscall2(long sc, sc_word_t arg1, sc_word_t arg2) {
    register sc_word_t ret asm("rax");
    register sc_word_t num asm("rax") = sc;
    register sc_word_t a1 asm("rbx") = arg1;
    register sc_word_t a2 asm("r10") = arg2;
    asm volatile("syscall" : "=a"(ret) : "a"(num), "r"(a1), "r"(a2) : "rcx", "r11", "memory");
    return ret;
}

extern "C" sc_word_t __do_syscall3(long sc, sc_word_t arg1, sc_word_t arg2, sc_word_t arg3) {
    register sc_word_t ret asm("rax");
    register sc_word_t num asm("rax") = sc;
    register sc_word_t a1 asm("rbx") = arg1;
    register sc_word_t a2 asm("r10") = arg2;
    register sc_word_t a3 asm("rdx") = arg3;
    asm volatile("syscall" : "=a"(ret) : "a"(num), "r"(a1), "r"(a2), "r"(a3) : "rcx", "r11", "memory");
    return ret;
}

extern "C" sc_word_t __do_syscall4(long sc, sc_word_t arg1, sc_word_t arg2, sc_word_t arg3, sc_word_t arg4) {
    register sc_word_t ret asm("rax");
    register sc_word_t num asm("rax") = sc;
    register sc_word_t a1 asm("rbx") = arg1;
    register sc_word_t a2 asm("r10") = arg2;
    register sc_word_t a3 asm("rdx") = arg3;
    register sc_word_t a4 asm("r8") = arg4;
    asm volatile("syscall" : "=a"(ret) : "a"(num), "r"(a1), "r"(a2), "r"(a3), "r"(a4) : "rcx", "r11", "memory");
    return ret;
}

extern "C" sc_word_t __do_syscall5(long sc, sc_word_t arg1, sc_word_t arg2, sc_word_t arg3, sc_word_t arg4, sc_word_t arg5) {
    register sc_word_t ret asm("rax");
    register sc_word_t num asm("rax") = sc;
    register sc_word_t a1 asm("rbx") = arg1;
    register sc_word_t a2 asm("r10") = arg2;
    register sc_word_t a3 asm("rdx") = arg3;
    register sc_word_t a4 asm("r8") = arg4;
    register sc_word_t a5 asm("r9") = arg5;
    asm volatile("syscall" : "=a"(ret) : "a"(num), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "rcx", "r11", "memory");
    return ret;
}

extern "C" sc_word_t __do_syscall6(long sc, sc_word_t arg1, sc_word_t arg2, sc_word_t arg3, sc_word_t arg4, sc_word_t arg5, sc_word_t) {
    return __do_syscall5(sc, arg1, arg2, arg3, arg4, arg5);
}
