#include <stdint.h>
#include <stdlib.h>
#include <mlibc/elf/startup.h>

#include <bits/syscall.h>

extern "C" void __dlapi_enter(uintptr_t*);
extern "C" long __do_syscall2(long, long, long);
extern char** environ;

namespace {

size_t cstr_len(const char* text) {
    size_t length = 0;
    while (text[length]) {
        length++;
    }
    return length;
}

void serial_write(const char* text) {
    __do_syscall2(88, reinterpret_cast<long>(text), static_cast<long>(cstr_len(text)));
}

}

extern "C" void __mlibc_entry(uintptr_t* entry_stack, int (*main_fn)(int, char**, char**)) {
    __dlapi_enter(entry_stack);
    serial_write("mlibc-entry: before-main\n");
    int result = main_fn(mlibc::entry_stack.argc, mlibc::entry_stack.argv, environ);
    serial_write("mlibc-entry: after-main\n");
    exit(result);
}
