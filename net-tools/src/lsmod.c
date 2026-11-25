// lsmod: the loaded driver modules (.idrv, docs/idrv.md).
//   lsmod        one line each: name, version, devices, size, where from
//   lsmod -v     also the description, load address and relocations
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "instantos_net.h"

int main(int argc, char** argv) {
    int verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    if (argc > 1 && !verbose) {
        fprintf(stderr, "usage: lsmod [-v]\n");
        return 2;
    }
    static struct module_info modules[64];
    long n = instantos_syscall(INSTANTOS_SYS_MODULE_LIST, (long)modules, 64, 0);
    if (n < 0) {
        fprintf(stderr, "lsmod: ModuleList: %s\n", strerror((int)-n));
        return 1;
    }
    if (n == 0) {
        printf("no driver modules loaded\n");
        return 0;
    }
    printf("%-16s %-8s %7s %7s  %s\n", "Module", "Version", "Devices", "Size", "From");
    for (long i = 0; i < n && i < 64; i++) {
        const struct module_info* m = &modules[i];
        printf("%-16s %-8s %7u %6uK  %s%s\n", m->name, m->version, m->devices, m->image_size / 1024, m->source,
               (m->flags & MODULE_INIT_FAILED) ? "  (init failed)" : "");
        if (verbose) {
            printf("    %s\n    at 0x%llx, %u relocations, %s\n", m->description, (unsigned long long)m->base,
                   m->relocations, (m->flags & MODULE_AT_BOOT) ? "loaded at boot" : "loaded by modload");
        }
    }
    if (n > 64) {
        printf("(%ld more)\n", n - 64);
    }
    return 0;
}
