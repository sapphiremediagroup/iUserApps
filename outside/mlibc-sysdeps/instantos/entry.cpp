#include <stdint.h>
#include <stdlib.h>
#include <mlibc/elf/startup.h>

extern "C" void __dlapi_enter(uintptr_t*);
extern "C" void __instantos_restore_fdtable(void);
extern char** environ;

extern "C" void __mlibc_entry(uintptr_t* entry_stack, int (*main_fn)(int, char**, char**)) {
    __dlapi_enter(entry_stack);
    // Rebuild the userspace fd table from the snapshot the previous image stashed
    // before exec (P3.2 fd-inheritance); a no-op for the initial process.
    __instantos_restore_fdtable();
    int result = main_fn(mlibc::entry_stack.argc, mlibc::entry_stack.argv, environ);
    exit(result);
}
