// InstantOS libc-torture: a dynamically-linked mlibc program that exercises the
// C library end-to-end (the real libc frontends + the instantos sysdeps), the
// Phase 2 exit gate. It validates this phase's work through the actual libc:
//   - file-backed mmap (VmMap -> MmapFile), msync           [P2.1a]
//   - openat/fstatat/unlinkat/renameat + fchdir with dirfd  [P2.1c]
//   - pread/pwrite, fsync, sigaltstack, getrusage           [P2.1d]
//   - math correctness (pow/exp/log/sqrt/trig + long double)[P2.2]
//   - time (CLOCK_MONOTONIC monotonic, mtime coherence)     [P2.4]
// plus baseline stdio/string/malloc/dirent/signal coverage.
//
// Results are reported over the serial port (syscall 88) so they land in the
// boot log regardless of stdout routing; the tested functions themselves are
// the real libc calls.

#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/resource.h>

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

// ---- serial reporting (syscall 88), independent of stdio ----
static void serial_write(const char* s) {
    unsigned long n = 0;
    while (s[n]) n++;
    register unsigned long rax asm("rax") = 88;
    register unsigned long rbx asm("rbx") = (unsigned long)s;
    register unsigned long r10 asm("r10") = n;
    asm volatile("syscall" : "+r"(rax) : "r"(rbx), "r"(r10) : "rcx", "r11", "memory");
}
static void sdec(long v) {
    char b[24]; int i = 23; b[23] = 0;
    unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
    if (v == 0) { serial_write("0"); return; }
    while (u) { b[--i] = (char)('0' + u % 10); u /= 10; }
    if (v < 0) b[--i] = '-';
    serial_write(&b[i]);
}

static int g_total = 0, g_pass = 0;
static void chk(int cond, const char* name) {
    g_total++;
    if (cond) { g_pass++; }
    else { serial_write("  FAIL: "); serial_write(name); serial_write("\n"); }
}

// double approx-equal
static int feq(double a, double b, double eps) { double d = a - b; if (d < 0) d = -d; return d <= eps; }

static volatile sig_atomic_t g_usr1 = 0;
static void on_usr1(int sig) { (void)sig; g_usr1 = 1; }

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    serial_write("[libctorture] BEGIN\n");

    // Diagnostic: confirm a raw syscall and a known-good libc call work before
    // the first sprintf (which is where this binary faults).
    serial_write("[ckpt] A: pre-getpid\n");
    { volatile long pid = (long)getpid(); (void)pid; }
    serial_write("[ckpt] B: post-getpid, pre-strlen\n");
    { volatile unsigned long L = strlen("hello"); (void)L; }
    serial_write("[ckpt] C: post-strlen, pre-sprintf-int\n");

    // ============ string / stdio / stdlib ============
    {
        char buf[64];
        int ni = sprintf(buf, "%d %s", 42, "hello");
        chk(ni == 8 && strcmp(buf, "42 hello") == 0, "sprintf int/str");
        serial_write("[ckpt] D: post-sprintf-int, pre-sprintf-float\n");
        int n = sprintf(buf, "%d %s %.2f", 42, "hello", 3.14159);
        serial_write("[ckpt] E: post-sprintf-float\n");
        chk(n == 13 && strcmp(buf, "42 hello 3.14") == 0, "sprintf int/str/float");
        serial_write("[ckpt] F: pre-snprintf\n");
        int m = snprintf(buf, 5, "%d", 123456);   // truncates to "1234"
        chk(m == 6 && strcmp(buf, "1234") == 0, "snprintf truncation + return");
        serial_write("[ckpt] G: pre-sscanf\n");
        int a = 0; char word[16] = {0}; double d = 0;
        int got = sscanf("7 world 2.5", "%d %15s %lf", &a, word, &d);
        chk(got == 3 && a == 7 && strcmp(word, "world") == 0 && feq(d, 2.5, 1e-9), "sscanf %d %s %lf");
        serial_write("[ckpt] H: pre-strtol\n");
        chk(strtol("  -2a", NULL, 10) == -2, "strtol");
        serial_write("[ckpt] I: pre-strtod\n");
        chk(feq(strtod("3.5x", NULL), 3.5, 1e-9), "strtod");
        serial_write("[ckpt] J: pre-str*\n");
        chk(strcmp("abc", "abc") == 0 && strcmp("abc", "abd") < 0, "strcmp");
        chk(strchr("abcd", 'c') != NULL && strstr("hello world", "wor") != NULL, "strchr/strstr");
        chk(memcmp("abc", "abd", 2) == 0 && memcmp("abc", "abd", 3) != 0, "memcmp");
        serial_write("[ckpt] K: post-string-block\n");
    }
    {
        serial_write("[ckpt] L: pre-malloc\n");
        char* p = (char*)malloc(100);
        chk(p != NULL, "malloc(100)");
        if (p) { memset(p, 0x5a, 100); chk(p[0] == 0x5a && p[99] == 0x5a, "malloc buffer writable"); }
        serial_write("[ckpt] M: pre-realloc\n");
        char* q = (char*)realloc(p, 4096);
        chk(q != NULL && q[0] == 0x5a && q[99] == 0x5a, "realloc grows + preserves");
        free(q);
        serial_write("[ckpt] N: pre-calloc\n");
        int* c = (int*)calloc(16, sizeof(int));
        chk(c != NULL && c[0] == 0 && c[15] == 0, "calloc zeroes");
        free(c);
        serial_write("[ckpt] O: post-malloc-block\n");
    }
    {
        serial_write("[ckpt] P: pre-setenv\n");
        setenv("LT_VAR", "xyz", 1);
        const char* e = getenv("LT_VAR");
        chk(e != NULL && strcmp(e, "xyz") == 0, "setenv/getenv roundtrip");
        serial_write("[ckpt] Q: post-setenv\n");
    }
    serial_write("[ckpt] R: pre-file-block\n");
    // Diagnostic: what is writable here? report open() results for /tmp and /.
    { int t = open("/tmp/lt_probe", O_WRONLY | O_CREAT | O_TRUNC, 0644);
      serial_write("[probe] open /tmp/lt_probe fd="); sdec(t); serial_write(" errno="); sdec(errno); serial_write("\n");
      if (t >= 0) { close(t); unlink("/tmp/lt_probe"); }
      int r = open("/lt_probe", O_WRONLY | O_CREAT | O_TRUNC, 0644);
      serial_write("[probe] open /lt_probe fd="); sdec(r); serial_write(" errno="); sdec(errno); serial_write("\n");
      if (r >= 0) { close(r); unlink("/lt_probe"); } }
    serial_write("[ckpt] R2: post-probe\n");

    // ============ file I/O (open/read/write/lseek/fstat, FILE*) ============
    const char* F = "/tmp/lt_file.bin";
    {
        serial_write("[ckpt] R3: pre-open-F\n");
        int fd = open(F, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        chk(fd >= 0, "open O_CREAT|O_WRONLY");
        serial_write("[ckpt] R4: post-open-F\n");
        if (fd >= 0) { chk(write(fd, "ABCDEFGH", 8) == 8, "write 8 bytes"); close(fd); }
        serial_write("[ckpt] R5: post-write/close\n");

        fd = open(F, O_RDONLY, 0);
        char b[16] = {0};
        chk(fd >= 0 && read(fd, b, 8) == 8 && memcmp(b, "ABCDEFGH", 8) == 0, "read back 8 bytes");
        chk(fd < 0 || lseek(fd, 4, SEEK_SET) == 4, "lseek SEEK_SET");
        char c2[8] = {0};
        chk(fd >= 0 && read(fd, c2, 4) == 4 && memcmp(c2, "EFGH", 4) == 0, "read at offset 4");
        chk(fd < 0 || lseek(fd, 0, SEEK_END) == 8, "lseek SEEK_END == size");
        serial_write("[ckpt] R6: pre-fstat\n");
        struct stat st;
        chk(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == 8 && S_ISREG(st.st_mode), "fstat size/mode");
        if (fd >= 0) close(fd);
        serial_write("[ckpt] R7: post-fstat/close\n");
    }
    {
        serial_write("[ckpt] R8: pre-fopen\n");
        FILE* f = fopen(F, "w");
        chk(f != NULL, "fopen w");
        serial_write("[ckpt] R9: post-fopen\n");
        if (f) {
            chk(fprintf(f, "line=%d\n", 5) == 7, "fprintf");
            serial_write("[ckpt] R10: post-fprintf\n");
            chk(fwrite("XY", 1, 2, f) == 2, "fwrite");
            fclose(f);
        }
        serial_write("[ckpt] R11: pre-fopen-r\n");
        f = fopen(F, "r");
        char line[32] = {0};
        chk(f != NULL && fgets(line, sizeof(line), f) != NULL && strcmp(line, "line=5\n") == 0, "fgets line");
        if (f) {
            chk(ftell(f) == 7, "ftell after fgets");
            char two[4] = {0};
            chk(fread(two, 1, 2, f) == 2 && memcmp(two, "XY", 2) == 0, "fread");
            fclose(f);
        }
        serial_write("[ckpt] R12: post-FILE-block\n");
    }

    // ============ pread / pwrite (P2.1d): positioned, fd offset untouched ====
    serial_write("[ckpt] S: pre-pread-block\n");
    {
        int fd = open(F, O_RDWR | O_CREAT | O_TRUNC, 0644);
        chk(write(fd, "0123456789", 10) == 10, "seed for pread/pwrite");
        chk(lseek(fd, 3, SEEK_SET) == 3, "seek to 3");
        char pb[4] = {0};
        serial_write("[ckpt] S2: pre-pread\n");
        chk(pread(fd, pb, 3, 6) == 3 && memcmp(pb, "678", 3) == 0, "pread at offset 6");
        serial_write("[ckpt] S3: post-pread\n");
        chk(lseek(fd, 0, SEEK_CUR) == 3, "pread left fd offset unchanged");
        chk(pwrite(fd, "ZZ", 2, 0) == 2, "pwrite at offset 0");
        chk(lseek(fd, 0, SEEK_CUR) == 3, "pwrite left fd offset unchanged");
        char vb[4] = {0};
        chk(pread(fd, vb, 2, 0) == 2 && memcmp(vb, "ZZ", 2) == 0, "pwrite landed at offset 0");
        close(fd);
    }

    // ============ fsync (P2.1d) ============
    serial_write("[ckpt] T: pre-fsync-block\n");
    {
        int fd = open(F, O_WRONLY | O_TRUNC, 0644);
        write(fd, "sync", 4);
        chk(fsync(fd) == 0, "fsync returns 0");
        close(fd);
    }

    // ============ mmap file-backed (P2.1a) ============
    serial_write("[ckpt] U: pre-mmap-block\n");
    {
        // seed a 5000-byte file (two pages + tail)
        int fd = open(F, O_RDWR | O_CREAT | O_TRUNC, 0644);
        char* seed = (char*)malloc(5000);
        for (int i = 0; i < 5000; i++) seed[i] = (char)((i * 7 + 3) & 0xff);
        write(fd, seed, 5000);
        fsync(fd);
        // MAP_PRIVATE read view
        serial_write("[ckpt] U2: pre-mmap-call\n");
        void* m = mmap(NULL, 8192, PROT_READ, MAP_PRIVATE, fd, 0);
        serial_write("[ckpt] U3: post-mmap-call m="); sdec((long)m); serial_write("\n");
        chk(m != MAP_FAILED, "mmap MAP_PRIVATE PROT_READ");
        if (m != MAP_FAILED) {
            unsigned char* mp = (unsigned char*)m;
            serial_write("[ckpt] U4: pre-first-mapping-read\n");
            volatile unsigned char first = mp[0];
            serial_write("[ckpt] U5: page0 val="); sdec((long)first); serial_write("\n");
            volatile unsigned char pg1 = mp[4096];
            serial_write("[ckpt] U5b: page1 val="); sdec((long)pg1); serial_write("\n");
            int okc = 1; for (int i = 0; i < 5000; i++) if (mp[i] != (unsigned char)seed[i]) { okc = 0; break; }
            serial_write("[ckpt] U6: post-content-loop\n");
            chk(okc, "mmap file contents match");
            int okz = 1; for (int i = 5000; i < 8192; i++) if (mp[i] != 0) { okz = 0; break; }
            serial_write("[ckpt] U7: post-eof-loop\n");
            chk(okz, "mmap EOF tail zero-filled");
            serial_write("[ckpt] U8: pre-munmap\n");
            chk(munmap(m, 8192) == 0, "munmap private");
            serial_write("[ckpt] U9: post-munmap\n");
        }
        // MAP_SHARED write + msync -> persists
        serial_write("[ckpt] V: pre-shared-mmap\n");
        void* ms = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        serial_write("[ckpt] V2: post-shared-mmap ms="); sdec((long)ms); serial_write("\n");
        chk(ms != MAP_FAILED, "mmap MAP_SHARED rw");
        if (ms != MAP_FAILED) {
            serial_write("[ckpt] V3: pre-shared-write\n");
            ((unsigned char*)ms)[0] = 0xEE; ((unsigned char*)ms)[1] = 0xDD;
            serial_write("[ckpt] V4: post-shared-write, pre-msync\n");
            chk(msync(ms, 4096, MS_SYNC) == 0, "msync");
            serial_write("[ckpt] V5: post-msync, pre-munmap\n");
            munmap(ms, 4096);
            serial_write("[ckpt] V6: post-shared-munmap\n");
        }
        char rb[2] = {0};
        chk(pread(fd, rb, 2, 0) == 2 && (unsigned char)rb[0] == 0xEE && (unsigned char)rb[1] == 0xDD, "MAP_SHARED write persisted");
        close(fd);
        free(seed);
    }
    serial_write("[ckpt] W: pre-anon-mmap-block\n");
    {
        // anonymous mmap still works
        void* a = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        chk(a != MAP_FAILED, "mmap anonymous");
        if (a != MAP_FAILED) { ((char*)a)[65535] = 9; chk(((char*)a)[65535] == 9, "anon mmap writable"); munmap(a, 65536); }
    }

    // ============ openat / fstatat / unlinkat / renameat + fchdir (P2.1c) ===
    {
        mkdir("/tmp/lt_d", 0755);
        // create /tmp/lt_d/a.txt via openat(dirfd, "a.txt")
        int dfd = open("/tmp/lt_d", O_RDONLY | O_DIRECTORY, 0);
        chk(dfd >= 0, "open dir O_DIRECTORY");
        int afd = openat(dfd, "a.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        chk(afd >= 0, "openat(dirfd, a.txt) create");
        if (afd >= 0) { write(afd, "AT!", 3); close(afd); }
        // fstatat resolves relative to dirfd
        struct stat st;
        chk(fstatat(dfd, "a.txt", &st, 0) == 0 && st.st_size == 3, "fstatat(dirfd, a.txt)");
        // read it back via openat
        afd = openat(dfd, "a.txt", O_RDONLY, 0);
        char b[8] = {0};
        chk(afd >= 0 && read(afd, b, 8) == 3 && memcmp(b, "AT!", 3) == 0, "openat read-back");
        if (afd >= 0) close(afd);
        // renameat within the dir
        chk(renameat(dfd, "a.txt", dfd, "b.txt") == 0, "renameat(dirfd, a->b)");
        chk(fstatat(dfd, "b.txt", &st, 0) == 0, "renameat target exists");
        chk(fstatat(dfd, "a.txt", &st, 0) != 0, "renameat source gone");
        // fchdir then relative open resolves in the dir
        char cwd0[256]; getcwd(cwd0, sizeof(cwd0));
        chk(fchdir(dfd) == 0, "fchdir(dirfd)");
        char cwd[256];
        chk(getcwd(cwd, sizeof(cwd)) != NULL && strcmp(cwd, "/tmp/lt_d") == 0, "getcwd after fchdir == /tmp/lt_d");
        int rfd = open("b.txt", O_RDONLY, 0);   // relative to new cwd
        chk(rfd >= 0, "relative open after fchdir");
        if (rfd >= 0) close(rfd);
        chdir(cwd0);   // restore
        // unlinkat
        chk(unlinkat(dfd, "b.txt", 0) == 0, "unlinkat(dirfd, b.txt)");
        chk(fstatat(dfd, "b.txt", &st, 0) != 0, "unlinkat removed file");
        close(dfd);
    }

    // ============ opendir / readdir ============
    {
        mkdir("/tmp/lt_rd", 0755);
        int f1 = open("/tmp/lt_rd/one", O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f1 >= 0) close(f1);
        int f2 = open("/tmp/lt_rd/two", O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f2 >= 0) close(f2);
        DIR* dir = opendir("/tmp/lt_rd");
        chk(dir != NULL, "opendir");
        int sawOne = 0, sawTwo = 0, count = 0;
        if (dir) {
            struct dirent* de;
            while ((de = readdir(dir)) != NULL) {
                if (strcmp(de->d_name, "one") == 0) sawOne = 1;
                if (strcmp(de->d_name, "two") == 0) sawTwo = 1;
                count++;
            }
            closedir(dir);
        }
        chk(sawOne && sawTwo, "readdir enumerates entries");
        chk(count >= 2, "readdir count >= 2");
        unlink("/tmp/lt_rd/one"); unlink("/tmp/lt_rd/two"); rmdir("/tmp/lt_rd");
    }

    // ============ sigaltstack (P2.1d) ============
    {
        static char altstk[16384];
        stack_t ss, oss;
        ss.ss_sp = altstk; ss.ss_size = sizeof(altstk); ss.ss_flags = 0;
        chk(sigaltstack(&ss, NULL) == 0, "sigaltstack set");
        memset(&oss, 0, sizeof(oss));
        chk(sigaltstack(NULL, &oss) == 0 && oss.ss_sp == altstk && (size_t)oss.ss_size == sizeof(altstk),
            "sigaltstack get returns what was set");
    }

    // ============ signals ============
    {
        g_usr1 = 0;
        signal(SIGUSR1, on_usr1);
        raise(SIGUSR1);
        chk(g_usr1 == 1, "raise(SIGUSR1) delivers to handler");
        signal(SIGUSR1, SIG_DFL);
    }

    // ============ getrusage / times (P2.1d) ============
    {
        struct rusage ru;
        chk(getrusage(RUSAGE_SELF, &ru) == 0, "getrusage RUSAGE_SELF");
    }

    // ============ math (P2.2) ============
    {
        chk(feq(sqrt(2.0), 1.4142135623730951, 1e-12), "sqrt(2)");
        chk(pow(2.0, 10.0) == 1024.0, "pow(2,10)==1024");
        chk(feq(exp(1.0), 2.718281828459045, 1e-12), "exp(1)");
        chk(feq(log(2.718281828459045), 1.0, 1e-12), "log(e)==1");
        chk(feq(log2(8.0), 3.0, 1e-12) && feq(log10(1000.0), 3.0, 1e-12), "log2/log10");
        chk(sin(0.0) == 0.0 && feq(cos(0.0), 1.0, 1e-15), "sin(0)/cos(0)");
        chk(feq(sin(3.14159265358979323846 / 6.0), 0.5, 1e-12), "sin(pi/6)==0.5");
        chk(feq(atan2(1.0, 1.0), 3.14159265358979323846 / 4.0, 1e-12), "atan2(1,1)==pi/4");
        chk(floor(3.7) == 3.0 && ceil(3.2) == 4.0 && round(2.5) == 3.0, "floor/ceil/round");
        chk(feq(fmod(10.0, 3.0), 1.0, 1e-12), "fmod(10,3)==1");
        chk(fabs(-5.0) == 5.0 && copysign(3.0, -1.0) == -3.0, "fabs/copysign");
        int ex = 0; double mant = frexp(1024.0, &ex);
        chk(feq(mant, 0.5, 1e-12) && ex == 11 && ldexp(0.5, 11) == 1024.0, "frexp/ldexp");
        chk(isnan(NAN) && isinf(INFINITY) && !isnan(1.0), "isnan/isinf");
        chk(powl(2.0L, 10.0L) == 1024.0L && feq((double)sqrtl(2.0L), 1.4142135623730951, 1e-12), "long double powl/sqrtl");
    }

    // ============ time (P2.4) ============
    {
        struct timespec t1, t2;
        chk(clock_gettime(CLOCK_MONOTONIC, &t1) == 0, "clock_gettime MONOTONIC");
        struct timespec req = {0, 60L * 1000L * 1000L};   // 60ms
        nanosleep(&req, NULL);
        clock_gettime(CLOCK_MONOTONIC, &t2);
        int nondec = (t2.tv_sec > t1.tv_sec) || (t2.tv_sec == t1.tv_sec && t2.tv_nsec >= t1.tv_nsec);
        chk(nondec, "CLOCK_MONOTONIC non-decreasing");
        double dt = (double)(t2.tv_sec - t1.tv_sec) + (double)(t2.tv_nsec - t1.tv_nsec) / 1e9;
        chk(dt >= 0.03, "monotonic advanced >= 30ms after 60ms sleep");

        struct timespec rt;
        chk(clock_gettime(CLOCK_REALTIME, &rt) == 0 && rt.tv_sec > 1600000000L, "CLOCK_REALTIME > 2020");

        time_t now = time(NULL);
        int fd = open("/tmp/lt_t.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { write(fd, "t", 1); close(fd); }
        struct stat st;
        long skew = 999;
        if (stat("/tmp/lt_t.txt", &st) == 0) { skew = (long)(st.st_mtime - now); if (skew < 0) skew = -skew; }
        chk(skew <= 5, "fresh file mtime coherent with time() (<=5s)");
        unlink("/tmp/lt_t.txt");

        struct tm* tmv = gmtime(&now);
        char tb[64] = {0};
        chk(tmv != NULL && strftime(tb, sizeof(tb), "%Y", tmv) == 4 && tb[0] == '2', "gmtime/strftime year 2xxx");
    }

    // ============ process / identity ============
    {
        chk(getpid() > 0, "getpid > 0");
        chk(getppid() >= 0, "getppid >= 0");
        struct utsname un;
        chk(uname(&un) == 0 && un.sysname[0] != 0, "uname");
    }

    unlink(F);
    mkdir("/tmp/lt_d", 0755); rmdir("/tmp/lt_d");   // best-effort cleanup

    // ============ SCORE ============
    serial_write("[libctorture] SCORE pass=");
    sdec(g_pass);
    serial_write(" total=");
    sdec(g_total);
    serial_write(" fail=");
    sdec(g_total - g_pass);
    serial_write("\n[libctorture] END\n");
    return (g_pass == g_total) ? 0 : 1;
}
