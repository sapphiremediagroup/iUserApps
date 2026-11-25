// instant-install: install InstantOS onto a partition (docs: TODO.md
// "Installed system + Limine"). Run it from the live system (the Limine entry
// with root=none, or any boot that does not have the target as /):
//
//   instant-install                  the disks' partitions; InstantOS's marked
//   instant-install diskNpM          erase that partition and install onto it
//   instant-install --update diskNpM  put this image's system over an installed
//                                     one, keeping every other file
//   --yes                            do either without asking
//
// Only partitions the kernel counts as InstantOS's (the InstantOS GPT type, or
// Windows "basic data" holding FAT32 labelled INSTANTOS or ext4 labelled
// InstantOS) can be chosen. Installing gives the partition the InstantOS type, makes an ext4 file system on it
// (mke2fs, the features the kernel's driver is tested with), mounts it at
// /target, copies the system (/bin, /lib, /include, /netsurf), makes the
// usual directories and /test/logs, and writes /.installed, which tells the
// kernel at boot to use the partition's /bin and /lib.
//
// Updating formats nothing and leaves the GPT type alone. It writes the files
// of /bin, /lib, /include and /netsurf again, each to a temporary name that is
// then renamed over the old one, so one that is cut off leaves every file
// either old or new and can be run again. Files the image does not have stay.
// The toolchain is unpacked only when it is not the one that is there
// (/.toolchain). Files of those names changed by hand are replaced.
//
// Booting it: iboot needs root=<the partition's GUID> (it is printed at the
// end) in \EFI\InstantOS\iboot.cfg or the Limine entry's cmdline.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "instantos_net.h"

static const char* kTarget = "/target";

static void guid_text(const uint8_t g[16], char out[37]) {
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    char* p = out;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) *p++ = '-';
        p += sprintf(p, "%02x", g[order[i]]);
    }
    *p = 0;
}

static long list_partitions(struct partition_record* parts, long cap) {
    long n = instantos_syscall(INSTANTOS_SYS_PARTITION_LIST, (long)parts, cap, 0);
    if (n < 0) {
        fprintf(stderr, "instant-install: PartitionList: %s\n", strerror((int)-n));
        exit(1);
    }
    return n < cap ? n : cap;
}

static void print_partitions(const struct partition_record* parts, long n) {
    printf("%-10s %10s  %-36s  %s\n", "Device", "Size", "Partition GUID", "");
    for (long i = 0; i < n; i++) {
        const struct partition_record* p = &parts[i];
        char guid[37];
        guid_text(p->unique_guid, guid);
        char dev[16];
        snprintf(dev, sizeof(dev), "disk%up%u", p->disk, p->index);
        printf("%-10s %8llu M  %s  %s%s%s%s\n", dev, (unsigned long long)(p->length_byte >> 20), guid,
               (p->flags & PARTITION_INSTANTOS) ? "InstantOS's" : "another system's",
               (p->flags & PARTITION_ESP) ? ", EFI system partition" : "",
               (p->flags & PARTITION_MOUNTED) ? ", mounted as /" : "",
               p->name[0] ? "" : "");
    }
}

static int run(char* const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setenv("MKE2FS_CONFIG", "/lib/e2fsprogs/mke2fs.conf", 1);
        setenv("PATH", "/bin", 1);   // tar runs gzip
        execv(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static long gFiles, gBytes;
static int gUpdate;   // --update: files are replaced whole, by rename

// The PC's INITRD carries the C/C++ toolchain here (tools/make-pc-initrd.sh);
// it is unpacked onto the partition, not copied with /lib.
static const char* kPayloadDir = "/lib/install";
static const char* kToolchain = "/lib/install/instantos-dev.tar.gz";

// Copies a tree (files, directories, symlinks), modes kept.
static int copy_tree(const char* from, const char* to) {
    if (strcmp(from, kPayloadDir) == 0) return 0;
    struct stat st;
    if (lstat(from, &st) != 0) {
        fprintf(stderr, "instant-install: %s: %s\n", from, strerror(errno));
        return -1;
    }
    if (S_ISLNK(st.st_mode)) {
        char target[1024];
        ssize_t n = readlink(from, target, sizeof(target) - 1);
        if (n < 0) return -1;
        target[n] = 0;
        unlink(to);
        return symlink(target, to);
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(to, st.st_mode & 07777) != 0 && errno != EEXIST) {
            fprintf(stderr, "instant-install: mkdir %s: %s\n", to, strerror(errno));
            return -1;
        }
        DIR* dir = opendir(from);
        if (!dir) return -1;
        struct dirent* e;
        int result = 0;
        while ((e = readdir(dir)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char a[1024], b[1024];
            snprintf(a, sizeof(a), "%s/%s", from, e->d_name);
            snprintf(b, sizeof(b), "%s/%s", to, e->d_name);
            if (copy_tree(a, b) != 0) result = -1;
        }
        closedir(dir);
        return result;
    }
    int in = open(from, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "instant-install: %s: %s\n", from, strerror(errno));
        return -1;
    }
    char temporary[1100];
    snprintf(temporary, sizeof(temporary), "%s.instant-new", to);
    const char* path = gUpdate ? temporary : to;
    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
    if (out < 0) {
        fprintf(stderr, "instant-install: %s: %s\n", path, strerror(errno));
        close(in);
        return -1;
    }
    static char buffer[1 << 20];
    ssize_t n;
    int result = 0;
    while ((n = read(in, buffer, sizeof(buffer))) > 0) {
        if (write(out, buffer, (size_t)n) != n) {
            fprintf(stderr, "instant-install: writing %s: %s\n", path, strerror(errno));
            result = -1;
            break;
        }
        gBytes += n;
    }
    if (n < 0) {
        fprintf(stderr, "instant-install: reading %s: %s\n", from, strerror(errno));
        result = -1;
    }
    close(in);
    close(out);
    if (gUpdate) {
        if (result == 0 && rename(temporary, to) != 0) {
            fprintf(stderr, "instant-install: replacing %s: %s\n", to, strerror(errno));
            result = -1;
        }
        if (result != 0) unlink(temporary);
    }
    gFiles++;
    return result;
}

// The system's trees and the usual directories, onto the target.
static int copy_system(void) {
    static const char* trees[] = {"/bin", "/lib", "/include", "/netsurf"};
    int failed = 0;
    for (size_t i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
        struct stat st;
        if (stat(trees[i], &st) != 0) continue;
        char to[256];
        snprintf(to, sizeof(to), "%s%s", kTarget, trees[i]);
        printf("      %s\n", trees[i]);
        fflush(stdout);
        if (copy_tree(trees[i], to) != 0) failed = 1;
    }
    static const char* dirs[] = {"/tmp", "/etc", "/dev", "/sys", "/var", "/home", "/drivers", "/test", "/test/logs"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char d[256];
        snprintf(d, sizeof(d), "%s%s", kTarget, dirs[i]);
        mkdir(d, 0755);
    }
    printf("      %ld files, %ld MiB\n", gFiles, gBytes >> 20);
    return failed ? -1 : 0;
}

// What identifies a toolchain tarball: gzip's trailer, the CRC32 and length of
// what it holds. False when it cannot be read.
static int toolchain_stamp(char out[32]) {
    unsigned char t[8];
    int fd = open(kToolchain, O_RDONLY);
    if (fd < 0) return 0;
    int ok = lseek(fd, -8, SEEK_END) >= 0 && read(fd, t, 8) == 8;
    close(fd);
    if (!ok) return 0;
    snprintf(out, 32, "%02x%02x%02x%02x-%02x%02x%02x%02x", t[3], t[2], t[1], t[0], t[7], t[6], t[5], t[4]);
    return 1;
}

static void target_path(char* out, size_t size, const char* name) {
    snprintf(out, size, "%s%s", kTarget, name);
}

// Unpacks the image's toolchain onto the target, and notes which one it is.
// 0 done (or none in the image, or it is there already), -1 failed.
static int install_toolchain(const char* step) {
    struct stat payload;
    if (stat(kToolchain, &payload) != 0) {
        printf("%s no toolchain in this image, skipped\n", step);
        return 0;
    }
    char stamp[32] = "", path[256], have[32] = "";
    int stamped = toolchain_stamp(stamp);
    target_path(path, sizeof(path), "/.toolchain");
    FILE* f = fopen(path, "r");
    if (f) {
        if (!fgets(have, sizeof(have), f)) have[0] = 0;
        have[strcspn(have, "\n")] = 0;
        fclose(f);
    }
    char gcc[256];
    struct stat st;
    target_path(gcc, sizeof(gcc), "/usr/bin/gcc");
    if (gUpdate && stamped && strcmp(have, stamp) == 0 && stat(gcc, &st) == 0) {
        printf("%s the C/C++ toolchain is this one already\n", step);
        return 0;
    }
    printf("%s unpacking the C/C++ toolchain (%lld MiB, a minute or more on a hard disk)\n", step,
           (long long)payload.st_size >> 20);
    fflush(stdout);
    unlink(path);   // no stamp until it is whole
    char* tar[] = {"/bin/tar", "xzf", (char*)kToolchain, "-C", (char*)kTarget, NULL};
    if (run(tar) != 0) return -1;
    if (stamped && (f = fopen(path, "w")) != NULL) {
        fprintf(f, "%s\n", stamp);
        fclose(f);
    }
    return 0;
}

static int update(const char* name, unsigned disk, unsigned index) {
    gUpdate = 1;
    printf("[1/3] mounting it at %s\n", kTarget);
    mkdir(kTarget, 0755);
    long r = instantos_syscall(INSTANTOS_SYS_PARTITION_MOUNT, disk, index, (long)kTarget);
    if (r < 0) {
        fprintf(stderr, "instant-install: PartitionMount: %s%s\n", strerror((int)-r),
                r == -EINVAL ? " (not an installed system's ext4 partition?)" : "");
        return 1;
    }
    char marker[256];
    struct stat st;
    target_path(marker, sizeof(marker), "/.installed");
    if (stat(marker, &st) != 0) {
        fprintf(stderr, "instant-install: %s has no /.installed: nothing to update, nothing changed\n", name);
        return 1;
    }
    printf("[2/3] updating the system\n");
    if (copy_system() != 0) {
        sync();
        fprintf(stderr, "instant-install: some files were not updated (see above); run the update again\n");
        return 1;
    }
    const int toolchain = install_toolchain("[3/3]");
    FILE* f = fopen(marker, "a");
    if (f) {
        time_t now = time(NULL);
        fprintf(f, "updated %s", ctime(&now));
        fclose(f);
    }
    sync();
    if (toolchain != 0) {
        fprintf(stderr, "instant-install: unpacking the toolchain failed; run the update again\n");
        return 1;
    }
    printf("\nUpdated. Your files are as they were. Reboot and pick \"InstantOS\".\n");
    return 0;
}

int main(int argc, char** argv) {
    static struct partition_record parts[64];
    int yes = 0, updating = 0;
    const char* name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--yes") == 0) yes = 1;
        else if (strcmp(argv[i], "--update") == 0) updating = 1;
        else if (!name && argv[i][0] != '-') name = argv[i];
        else {
            fprintf(stderr, "usage: instant-install [--update] [--yes] [diskNpM]\n");
            return 2;
        }
    }
    long n = list_partitions(parts, 64);
    if (!name) {
        print_partitions(parts, n);
        printf("\nInstall with: instant-install diskNpM (an InstantOS partition, not mounted)\n"
               "Update an installed system, keeping your files: instant-install --update diskNpM\n");
        return 0;
    }

    unsigned disk = 0, index = 0;
    if (sscanf(name, "disk%up%u", &disk, &index) != 2) {
        fprintf(stderr, "instant-install: %s: expected a name like disk0p3 (run instant-install alone to list them)\n", name);
        return 2;
    }
    const struct partition_record* p = NULL;
    for (long i = 0; i < n; i++) {
        if (parts[i].disk == disk && parts[i].index == index) p = &parts[i];
    }
    if (!p) {
        fprintf(stderr, "instant-install: no partition %s\n", name);
        return 1;
    }
    if (!(p->flags & PARTITION_INSTANTOS) || (p->flags & PARTITION_ESP)) {
        fprintf(stderr, "instant-install: %s belongs to another system: refusing\n", name);
        return 1;
    }
    if (p->flags & PARTITION_MOUNTED) {
        fprintf(stderr, "instant-install: %s is mounted as /: boot the live system (root=none) to install onto it\n", name);
        return 1;
    }
    char guid[37];
    guid_text(p->unique_guid, guid);
    const unsigned long long blocks = p->length_byte / 4096;
    printf(updating ? "Updating InstantOS on %s (%llu MiB, partition %s).\n"
                    : "Installing InstantOS onto %s (%llu MiB, partition %s).\n", name,
           (unsigned long long)(p->length_byte >> 20), guid);
    if (!yes) {
        if (updating) printf("The system's own files on %s are replaced, yours are kept. Type YES to go on: ", name);
        else printf("Everything on %s will be erased. Type YES to go on: ", name);
        fflush(stdout);
        char answer[16] = {0};
        if (!fgets(answer, sizeof(answer), stdin) || strncmp(answer, "YES", 3) != 0) {
            printf("Nothing changed.\n");
            return 1;
        }
    }

    if (updating) return update(name, disk, index);

    printf("[1/6] marking it as an InstantOS partition\n");
    long r = instantos_syscall(INSTANTOS_SYS_PARTITION_SET_TYPE, disk, index, 0);
    if (r < 0) {
        fprintf(stderr, "instant-install: PartitionSetType: %s\n", strerror((int)-r));
        return 1;
    }

    printf("[2/6] making the ext4 file system\n");
    char dev[32], count[32];
    snprintf(dev, sizeof(dev), "/dev/%s", name);
    snprintf(count, sizeof(count), "%llu", blocks);
    char* mkfs[] = {"/bin/mke2fs", "-q", "-F", "-b", "4096", "-I", "256", "-O", "has_journal,extent",
                    "-E", "lazy_itable_init=0,lazy_journal_init=0", "-L", "InstantOS", dev, count, NULL};
    if (run(mkfs) != 0) {
        fprintf(stderr, "instant-install: mke2fs failed\n");
        return 1;
    }

    printf("[3/6] mounting it at %s\n", kTarget);
    mkdir(kTarget, 0755);
    r = instantos_syscall(INSTANTOS_SYS_PARTITION_MOUNT, disk, index, (long)kTarget);
    if (r < 0) {
        fprintf(stderr, "instant-install: PartitionMount: %s\n", strerror((int)-r));
        return 1;
    }

    printf("[4/6] copying the system\n");
    if (copy_system() != 0) {
        fprintf(stderr, "instant-install: some files were not copied (see above); the system is not marked installed\n");
        return 1;
    }

    // Without it the system is still complete, so a failure is only reported.
    const int toolchainFailed = install_toolchain("[5/6]") != 0;

    printf("[6/6] marking it installed\n");
    char marker[256];
    snprintf(marker, sizeof(marker), "%s/.installed", kTarget);
    FILE* f = fopen(marker, "w");
    if (!f) {
        fprintf(stderr, "instant-install: %s: %s\n", marker, strerror(errno));
        return 1;
    }
    time_t now = time(NULL);
    fprintf(f, "InstantOS installed %s", ctime(&now));
    fprintf(f, "partition %s\n", guid);
    fclose(f);
    sync();

    // Nothing has to be saved anywhere: the partition keeps its GUID, so an
    // iboot.cfg written for it before (install-limine-windows.ps1) still names
    // it, and without root= the kernel takes the disk's InstantOS partition.
    if (toolchainFailed) {
        fprintf(stderr, "instant-install: unpacking the toolchain failed: the system is installed without gcc\n");
    }
    printf("\nDone. Reboot and pick \"InstantOS\": it boots from this partition\n"
           "(GUID %s; nothing else needs setting).\n"
           "Once booted, `cat /.installed` or `dmesg | grep installed` confirms it.\n", guid);
    return 0;
}
