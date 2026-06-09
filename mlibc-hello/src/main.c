#include <stdio.h>
#include <unistd.h>

static unsigned long instant_syscall2(unsigned long number, unsigned long arg1, unsigned long arg2) {
    unsigned long result;
    register unsigned long rax asm("rax") = number;
    register unsigned long rbx asm("rbx") = arg1;
    register unsigned long r10 asm("r10") = arg2;
    asm volatile("syscall" : "=a"(result) : "a"(rax), "r"(rbx), "r"(r10) : "rcx", "r11", "memory");
    return result;
}

static void serial_write(const char* text) {
    unsigned long length = 0;
    while (text[length]) {
        length++;
    }
    instant_syscall2(88, (unsigned long)text, length);
}

int main(int argc, char** argv) {
    serial_write("mlibc-hello: serial-start\n");
    printf("mlibc-hello: argc=%d argv0=%s pid=%d\n", argc, argv && argv[0] ? argv[0] : "<none>", getpid());
    serial_write("mlibc-hello: serial-done\n");
    return 0;
}
