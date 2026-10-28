// dmesg: print the kernel log (the last 128 KiB of what it wrote to the serial
// port, kept whether or not the machine has one). On real hardware this is
// where the NIC drivers say what they found:  dmesg | grep net
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "instantos_net.h"

int main(void) {
    enum { kSize = 128 * 1024 };
    char* log = malloc(kSize);
    if (!log) {
        fprintf(stderr, "dmesg: out of memory\n");
        return 1;
    }
    long n = instantos_syscall(INSTANTOS_SYS_KERNEL_LOG, (long)log, kSize, 0);
    if (n < 0) {
        fprintf(stderr, "dmesg: KernelLog: %s\n", strerror((int)-n));
        return 1;
    }
    // The ring may begin mid-line; start at the first whole one.
    long start = 0;
    if (n == kSize) {
        while (start < n && log[start] != '\n') start++;
        start++;
    }
    for (long off = start; off < n;) {
        ssize_t w = write(1, log + off, (size_t)(n - off));
        if (w <= 0) break;
        off += w;
    }
    free(log);
    return 0;
}
