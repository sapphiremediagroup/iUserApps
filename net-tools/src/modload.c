// modload: load a driver module (.idrv, docs/idrv.md) for the devices it
// matches. Root only. The kernel log (dmesg | grep idrv) says what it did.
//   modload /drivers/rtl8139.idrv
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "instantos_net.h"

int main(int argc, char** argv) {
    if (argc != 2 || argv[1][0] == '-') {
        fprintf(stderr, "usage: modload FILE.idrv\n");
        return 2;
    }
    // The kernel takes an absolute path.
    char path[PATH_MAX];
    if (argv[1][0] == '/') {
        snprintf(path, sizeof(path), "%s", argv[1]);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) {
            perror("modload: getcwd");
            return 1;
        }
        snprintf(path, sizeof(path), "%s/%s", strcmp(cwd, "/") == 0 ? "" : cwd, argv[1]);
    }
    long r = instantos_syscall(INSTANTOS_SYS_MODULE_LOAD, (long)path, 0, 0);
    if (r == 0) {
        printf("modload: %s loaded\n", path);
        return 0;
    }
    const char* why = strerror((int)-r);
    switch ((int)-r) {
    case ENOENT: why = "no such file"; break;
    case EEXIST: why = "a module of that name is already loaded (lsmod)"; break;
    case ENODEV: why = "no device here that it drives (or another module has it)"; break;
    case EINVAL: why = "not a driver module, or built for another kernel (dmesg | grep idrv)"; break;
    case EPERM: why = "only root can load drivers"; break;
    default: break;
    }
    fprintf(stderr, "modload: %s: %s\n", path, why);
    return 1;
}
