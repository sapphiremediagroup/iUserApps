#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

namespace {

volatile sig_atomic_t g_signal_count = 0;
volatile sig_atomic_t g_altstack_seen = 0;
volatile sig_atomic_t g_thread_signal_seen = 0;
char* g_altstack_low = nullptr;
char* g_altstack_high = nullptr;
pthread_key_t g_tls_key = 0;
volatile int g_tls_destructor_count = 0;

struct CondContext {
    pthread_mutex_t* mutex;
    pthread_cond_t* cond;
    int* ready;
};

struct SemContext {
    sem_t* sem;
};

void count_signal_handler(int)
{
    g_signal_count = g_signal_count + 1;
}

void altstack_signal_handler(int)
{
    char marker = 0;
    char* address = &marker;
    if (address >= g_altstack_low && address < g_altstack_high) {
        g_altstack_seen = 1;
    }
}

void thread_signal_handler(int)
{
    g_thread_signal_seen = 1;
}

void* signal_wait_thread(void*)
{
    for (int i = 0; i < 100 && !g_thread_signal_seen; ++i) {
        sleep(1);
    }
    return reinterpret_cast<void*>(static_cast<unsigned long>(g_thread_signal_seen));
}

void tls_destructor(void* value)
{
    if (value != nullptr) {
        g_tls_destructor_count = g_tls_destructor_count + 1;
    }
}

void* tls_errno_thread(void*)
{
    static int value = 42;
    errno = EACCES;
    if (pthread_setspecific(g_tls_key, &value) != 0 || pthread_getspecific(g_tls_key) != &value) {
        return nullptr;
    }
    return reinterpret_cast<void*>(static_cast<unsigned long>(errno));
}

void* cond_signal_thread(void* raw)
{
    auto* context = static_cast<CondContext*>(raw);
    if (pthread_mutex_lock(context->mutex) != 0) {
        return nullptr;
    }
    *context->ready = 1;
    pthread_cond_signal(context->cond);
    pthread_mutex_unlock(context->mutex);
    return reinterpret_cast<void*>(1);
}

void* sem_post_thread(void* raw)
{
    auto* context = static_cast<SemContext*>(raw);
    for (int i = 0; i < 5; ++i) {
        sleep(0);
    }
    return sem_post(context->sem) == 0 ? reinterpret_cast<void*>(1) : nullptr;
}

void* cancel_wait_thread(void*)
{
    for (;;) {
        pthread_testcancel();
        sleep(0);
    }
}

int fail(const char* step)
{
    printf("posix-smoke: FAIL %s errno=%d\n", step, errno);
    return 1;
}

int require(bool condition, const char* step)
{
    return condition ? 0 : fail(step);
}

int ensure_scratch_dir()
{
    if (mkdir("/tmp", 0777) == 0 || errno == EEXIST) {
        return 0;
    }
    return fail("mkdir /tmp");
}

int test_filesystem()
{
    if (ensure_scratch_dir() != 0) {
        return 1;
    }
    errno = 0;
    if (mkdir("/tmp", 0777) == 0 || errno != EEXIST) {
        return fail("mkdir existing");
    }
    errno = 0;
    if (mkdir("/tmp/.", 0777) == 0 || errno != EEXIST) {
        return fail("mkdir dot");
    }
    errno = 0;
    int dir_fd = open("/tmp", O_RDONLY);
    if (dir_fd < 0) {
        return fail("open directory read");
    }
    char dir_byte = 0;
    errno = 0;
    if (read(dir_fd, &dir_byte, 1) >= 0 || errno != EISDIR) {
        close(dir_fd);
        return fail("read directory");
    }
    errno = 0;
    if (ftruncate(dir_fd, 0) == 0 || errno != EISDIR) {
        close(dir_fd);
        return fail("ftruncate directory");
    }
    close(dir_fd);
    errno = 0;
    dir_fd = open("/tmp", O_WRONLY);
    if (dir_fd >= 0 || errno != EISDIR) {
        if (dir_fd >= 0) {
            close(dir_fd);
        }
        return fail("open directory write");
    }
    errno = 0;
    dir_fd = open("/tmp", O_RDONLY | O_TRUNC);
    if (dir_fd >= 0 || errno != EISDIR) {
        if (dir_fd >= 0) {
            close(dir_fd);
        }
        return fail("open directory truncate");
    }
    dir_fd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) {
        return fail("open directory flag");
    }
    close(dir_fd);

    char long_path[300];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[0] = '/';
    long_path[sizeof(long_path) - 1] = '\0';
    errno = 0;
    struct stat ignored {};
    if (stat(long_path, &ignored) == 0 || errno != ENAMETOOLONG) {
        return fail("path too long");
    }

    const char* path = "/tmp/posix-smoke-file";
    const char* hardlink = "/tmp/posix-smoke-link";
    const char* symlink_path = "/tmp/posix-smoke-symlink";
    const char* symlink_relative = "/tmp/posix-smoke-relative-symlink";
    const char* symlink_dangling = "/tmp/posix-smoke-dangling";
    const char* symlink_loop = "/tmp/posix-smoke-loop";
    const char* symlink_dir = "/tmp/posix-smoke-dir-symlink";
    const char* symlink_real_dir = "/tmp/posix-smoke-real-dir";
    const char* symlink_real_dir_file = "/tmp/posix-smoke-real-dir/file";
    const char* renamed = "/tmp/posix-smoke-renamed";
    unlink(path);
    unlink(hardlink);
    unlink(symlink_path);
    unlink(symlink_relative);
    unlink(symlink_dangling);
    unlink(symlink_loop);
    unlink(symlink_dir);
    unlink(symlink_real_dir_file);
    rmdir(symlink_real_dir);
    unlink(renamed);

    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        return fail("open");
    }

    const char payload[] = "posix-smoke";
    if (write(fd, payload, sizeof(payload)) != (ssize_t)sizeof(payload)) {
        close(fd);
        return fail("write");
    }
    if (fsync(fd) != 0 || fdatasync(fd) != 0) {
        close(fd);
        return fail("sync file");
    }
    if (lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return fail("lseek");
    }

    char buffer[sizeof(payload)] = {};
    if (read(fd, buffer, sizeof(buffer)) != (ssize_t)sizeof(buffer)) {
        close(fd);
        return fail("read");
    }
    if (memcmp(buffer, payload, sizeof(payload)) != 0) {
        close(fd);
        return fail("read payload");
    }

    struct stat st {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != sizeof(payload)) {
        close(fd);
        return fail("fstat");
    }
    close(fd);
    errno = 0;
    if (fsync(fd) == 0 || errno != EBADF) {
        return fail("fsync closed fd");
    }
    errno = 0;
    fd = open(path, O_ACCMODE);
    if (fd >= 0 || errno != EINVAL) {
        if (fd >= 0) {
            close(fd);
        }
        return fail("open invalid access mode");
    }
    errno = 0;
    fd = open(path, O_RDONLY | 0100000000);
    if (fd >= 0 || errno != EINVAL) {
        if (fd >= 0) {
            close(fd);
        }
        return fail("open invalid flag");
    }

    mode_t old_umask = umask(0027);
    const char* umask_file = "/tmp/posix-smoke-umask-file";
    const char* umask_dir = "/tmp/posix-smoke-umask-dir";
    unlink(umask_file);
    rmdir(umask_dir);
    fd = open(umask_file, O_CREAT | O_EXCL | O_RDWR, 0666);
    umask(old_umask);
    if (fd < 0) {
        return fail("open umask file");
    }
    close(fd);
    if (stat(umask_file, &st) != 0 || (st.st_mode & 0777) != 0640) {
        return fail("stat umask file mode");
    }
    old_umask = umask(0027);
    if (mkdir(umask_dir, 0777) != 0) {
        umask(old_umask);
        return fail("mkdir umask dir");
    }
    umask(old_umask);
    if (stat(umask_dir, &st) != 0 || (st.st_mode & 0777) != 0750) {
        return fail("stat umask dir mode");
    }

    errno = 0;
    if (stat("/tmp/posix-smoke-file/", &st) == 0 || errno != ENOTDIR) {
        return fail("stat trailing slash file");
    }
    errno = 0;
    fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd >= 0 || errno != ENOTDIR) {
        if (fd >= 0) {
            close(fd);
        }
        return fail("open file directory flag");
    }
    errno = 0;
    fd = open("/tmp/posix-smoke-missing", O_RDONLY | O_DIRECTORY);
    if (fd >= 0 || errno != ENOENT) {
        if (fd >= 0) {
            close(fd);
        }
        return fail("open missing directory flag");
    }
    errno = 0;
    fd = open("/tmp/posix-smoke-file/", O_RDONLY);
    if (fd >= 0 || errno != ENOTDIR) {
        if (fd >= 0) {
            close(fd);
        }
        return fail("open trailing slash file");
    }
    errno = 0;
    if (unlink("/tmp/posix-smoke-file/") == 0 || errno != ENOTDIR) {
        return fail("unlink trailing slash file");
    }
    errno = 0;
    if (truncate("/tmp/posix-smoke-file/", 0) == 0 || errno != ENOTDIR) {
        return fail("truncate trailing slash file");
    }
    errno = 0;
    if (chmod("/tmp/posix-smoke-file/", 0600) == 0 || errno != ENOTDIR) {
        return fail("chmod trailing slash file");
    }
    errno = 0;
    if (utime("/tmp/posix-smoke-file/", nullptr) == 0 || errno != ENOTDIR) {
        return fail("utime trailing slash file");
    }

    errno = 0;
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd >= 0 || errno != EEXIST) {
        if (fd >= 0) {
            close(fd);
        }
        return fail("open exclusive existing");
    }

    if (link(path, hardlink) != 0) {
        return fail("link");
    }
    errno = 0;
    if (link(path, hardlink) == 0 || errno != EEXIST) {
        return fail("link existing");
    }
    struct stat original {};
    struct stat linked {};
    if (stat(path, &original) != 0 || stat(hardlink, &linked) != 0 ||
        original.st_ino != linked.st_ino || linked.st_nlink < 2) {
        return fail("stat hardlink");
    }
    if (symlink(path, symlink_path) != 0) {
        return fail("symlink");
    }
    char link_target[128] {};
    ssize_t link_len = readlink(symlink_path, link_target, sizeof(link_target));
    if (link_len != (ssize_t)strlen(path) || memcmp(link_target, path, strlen(path)) != 0) {
        return fail("readlink");
    }
    char tiny_link_target[4] {};
    link_len = readlink(symlink_path, tiny_link_target, sizeof(tiny_link_target));
    if (link_len != (ssize_t)sizeof(tiny_link_target) || memcmp(tiny_link_target, path, sizeof(tiny_link_target)) != 0) {
        return fail("readlink truncate");
    }
    errno = 0;
    if (readlink(symlink_path, link_target, 0) >= 0 || errno != EINVAL) {
        return fail("readlink zero size");
    }
    errno = 0;
    if (readlink(path, link_target, sizeof(link_target)) >= 0 || errno != EINVAL) {
        return fail("readlink regular file");
    }
    errno = 0;
    if (readlink("/tmp/posix-smoke-missing-link", link_target, sizeof(link_target)) >= 0 || errno != ENOENT) {
        return fail("readlink missing");
    }
    struct stat symlink_stat {};
    if (lstat(symlink_path, &symlink_stat) != 0 || !S_ISLNK(symlink_stat.st_mode) ||
        symlink_stat.st_size != (off_t)strlen(path)) {
        return fail("lstat symlink");
    }
    if (stat(symlink_path, &symlink_stat) != 0 || !S_ISREG(symlink_stat.st_mode) ||
        symlink_stat.st_ino != original.st_ino) {
        return fail("stat symlink target");
    }
    if (symlink("posix-smoke-file", symlink_relative) != 0) {
        return fail("symlink relative");
    }
    if (stat(symlink_relative, &symlink_stat) != 0 || !S_ISREG(symlink_stat.st_mode) ||
        symlink_stat.st_ino != original.st_ino) {
        return fail("stat relative symlink");
    }
    if (symlink("posix-smoke-missing-target", symlink_dangling) != 0) {
        return fail("symlink dangling");
    }
    if (lstat(symlink_dangling, &symlink_stat) != 0 || !S_ISLNK(symlink_stat.st_mode)) {
        return fail("lstat dangling symlink");
    }
    errno = 0;
    if (stat(symlink_dangling, &symlink_stat) == 0 || errno != ENOENT) {
        return fail("stat dangling symlink");
    }
    errno = 0;
    int dangling_fd = open(symlink_dangling, O_RDONLY);
    if (dangling_fd >= 0 || errno != ENOENT) {
        if (dangling_fd >= 0) {
            close(dangling_fd);
        }
        return fail("open dangling symlink");
    }
    errno = 0;
    dangling_fd = open(symlink_dangling, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (dangling_fd >= 0 || errno != EEXIST) {
        if (dangling_fd >= 0) {
            close(dangling_fd);
        }
        return fail("open exclusive dangling symlink");
    }
    if (mkdir(symlink_real_dir, 0700) != 0) {
        return fail("mkdir symlink real dir");
    }
    int real_dir_fd = open(symlink_real_dir_file, O_CREAT | O_RDWR, 0600);
    if (real_dir_fd < 0) {
        return fail("create symlink real dir file");
    }
    close(real_dir_fd);
    if (symlink("posix-smoke-real-dir", symlink_dir) != 0) {
        return fail("symlink dir");
    }
    if (stat("/tmp/posix-smoke-dir-symlink/file", &symlink_stat) != 0 || !S_ISREG(symlink_stat.st_mode)) {
        return fail("stat intermediate dir symlink");
    }
    int symlink_dir_fd = open("/tmp/posix-smoke-dir-symlink/", O_RDONLY | O_DIRECTORY);
    if (symlink_dir_fd < 0) {
        return fail("open symlink directory slash");
    }
    close(symlink_dir_fd);
    int symlink_fd = open(symlink_path, O_RDONLY);
    if (symlink_fd < 0) {
        return fail("open symlink");
    }
    close(symlink_fd);
    errno = 0;
    symlink_fd = open(symlink_path, O_RDONLY | O_NOFOLLOW);
    if (symlink_fd >= 0 || errno != ELOOP) {
        if (symlink_fd >= 0) {
            close(symlink_fd);
        }
        return fail("open symlink nofollow");
    }
    errno = 0;
    symlink_fd = open(symlink_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (symlink_fd >= 0 || errno != EEXIST) {
        if (symlink_fd >= 0) {
            close(symlink_fd);
        }
        return fail("open exclusive symlink");
    }
    if (unlink(symlink_relative) != 0 || stat(path, &symlink_stat) != 0 || !S_ISREG(symlink_stat.st_mode)) {
        return fail("unlink symlink keeps target");
    }
    if (symlink("posix-smoke-loop", symlink_loop) != 0) {
        return fail("symlink loop");
    }
    if (lstat(symlink_loop, &symlink_stat) != 0 || !S_ISLNK(symlink_stat.st_mode)) {
        return fail("lstat symlink loop");
    }
    errno = 0;
    if (stat(symlink_loop, &symlink_stat) == 0 || errno != ELOOP) {
        return fail("stat symlink loop");
    }
    errno = 0;
    symlink_fd = open(symlink_loop, O_RDONLY);
    if (symlink_fd >= 0 || errno != ELOOP) {
        if (symlink_fd >= 0) {
            close(symlink_fd);
        }
        return fail("open symlink loop");
    }
    errno = 0;
    if (mkdir("/tmp/posix-smoke-file/child", 0700) == 0 || errno != ENOTDIR) {
        return fail("mkdir file parent");
    }
    errno = 0;
    if (link(path, "/tmp/posix-smoke-file/child") == 0 || errno != ENOTDIR) {
        return fail("link file parent");
    }
    errno = 0;
    int child_fd = open("/tmp/posix-smoke-file/child", O_CREAT | O_RDWR, 0600);
    if (child_fd >= 0 || errno != ENOTDIR) {
        if (child_fd >= 0) {
            close(child_fd);
        }
        return fail("open file parent");
    }
    errno = 0;
    if (unlink("/tmp/posix-smoke-file/child") == 0 || errno != ENOTDIR) {
        return fail("unlink file parent");
    }
    errno = 0;
    if (rmdir("/tmp/posix-smoke-file/child") == 0 || errno != ENOTDIR) {
        return fail("rmdir file parent");
    }
    errno = 0;
    if (stat("/tmp/posix-smoke-file/child", &st) == 0 || errno != ENOTDIR) {
        return fail("stat file parent");
    }
    errno = 0;
    if (rename("/tmp/posix-smoke-file/child", "/tmp/posix-smoke-missing") == 0 || errno != ENOTDIR) {
        return fail("rename old file parent");
    }
    errno = 0;
    if (rename(path, "/tmp/posix-smoke-file/child") == 0 || errno != ENOTDIR) {
        return fail("rename new file parent");
    }
    errno = 0;
    if (chmod("/tmp/posix-smoke-file/child", 0600) == 0 || errno != ENOTDIR) {
        return fail("chmod file parent");
    }
    errno = 0;
    if (utime("/tmp/posix-smoke-file/child", nullptr) == 0 || errno != ENOTDIR) {
        return fail("utime file parent");
    }
    errno = 0;
    if (truncate("/tmp/posix-smoke-file/child", 0) == 0 || errno != ENOTDIR) {
        return fail("truncate file parent");
    }
    if (rename(path, hardlink) != 0 || stat(path, &original) != 0 ||
        stat(hardlink, &linked) != 0 || original.st_ino != linked.st_ino || linked.st_nlink < 2) {
        return fail("rename same hardlink");
    }

    errno = 0;
    if (unlink("/tmp") == 0 || errno != EISDIR) {
        return fail("unlink directory");
    }
    errno = 0;
    if (rmdir(path) == 0 || errno != ENOTDIR) {
        return fail("rmdir file");
    }
    errno = 0;
    if (truncate("/tmp", 0) == 0 || errno != EISDIR) {
        return fail("truncate directory");
    }
    errno = 0;
    if (rmdir("/tmp") == 0 || errno != ENOTEMPTY) {
        return fail("rmdir non-empty");
    }

    if (truncate(hardlink, 5) != 0 || stat(path, &st) != 0 || st.st_size != 5) {
        return fail("truncate hardlink");
    }

    fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) {
        return fail("open append");
    }
    if (lseek(fd, 0, SEEK_SET) != 0 || write(fd, "!", 1) != 1) {
        close(fd);
        return fail("write append");
    }
    close(fd);
    if (stat(path, &st) != 0 || st.st_size != 6) {
        return fail("stat append");
    }

    if (chmod(path, 0400) != 0) {
        return fail("chmod read only");
    }
    errno = 0;
    fd = open(path, O_RDONLY | O_TRUNC);
    if (fd >= 0 || errno != EINVAL) {
        if (fd >= 0) {
            close(fd);
        }
        return fail("open truncate denied");
    }
    if (stat(path, &st) != 0 || st.st_size != 6) {
        return fail("stat truncate denied");
    }

    errno = 0;
    fd = open(path, O_WRONLY);
    if (fd >= 0 || errno != EACCES) {
        if (fd >= 0) {
            close(fd);
        }
        return fail("open write denied");
    }

    if (chmod(path, 0200) != 0) {
        return fail("chmod write only");
    }
    errno = 0;
    fd = open(path, O_RDONLY);
    if (fd >= 0 || errno != EACCES) {
        if (fd >= 0) {
            close(fd);
        }
        return fail("open read denied");
    }

    if (chmod(path, 0600) != 0) {
        return fail("chmod restore");
    }
    errno = 0;
    fd = open(path, O_RDONLY | O_TRUNC);
    if (fd >= 0 || errno != EINVAL) {
        if (fd >= 0) {
            close(fd);
        }
        return fail("open readonly truncate invalid");
    }
    if (stat(path, &st) != 0 || st.st_size != 6) {
        return fail("stat readonly truncate invalid");
    }

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        return fail("open write only access");
    }
    errno = 0;
    if (read(fd, buffer, 1) >= 0 || errno != EBADF) {
        close(fd);
        return fail("read write only fd");
    }
    close(fd);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return fail("open read only access");
    }
    errno = 0;
    if (write(fd, "x", 1) >= 0 || errno != EBADF) {
        close(fd);
        return fail("write read only fd");
    }
    close(fd);

    const char* no_search_dir = "/tmp/posix-smoke-no-search";
    const char* no_search_file = "/tmp/posix-smoke-no-search/file";
    unlink(no_search_file);
    rmdir(no_search_dir);
    if (mkdir(no_search_dir, 0700) != 0) {
        return fail("mkdir no search dir");
    }
    fd = open(no_search_file, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        rmdir(no_search_dir);
        return fail("open no search file");
    }
    close(fd);
    if (chmod(no_search_dir, 0000) != 0) {
        return fail("chmod no search dir");
    }
    errno = 0;
    if (stat(no_search_file, &st) == 0 || errno != EACCES) {
        chmod(no_search_dir, 0700);
        unlink(no_search_file);
        rmdir(no_search_dir);
        return fail("stat no search file");
    }
    errno = 0;
    fd = open(no_search_file, O_RDONLY);
    if (fd >= 0 || errno != EACCES) {
        if (fd >= 0) {
            close(fd);
        }
        chmod(no_search_dir, 0700);
        unlink(no_search_file);
        rmdir(no_search_dir);
        return fail("open no search file");
    }
    errno = 0;
    if (unlink(no_search_file) == 0 || errno != EACCES) {
        chmod(no_search_dir, 0700);
        unlink(no_search_file);
        rmdir(no_search_dir);
        return fail("unlink no search file");
    }
    errno = 0;
    if (mkdir("/tmp/posix-smoke-no-search/child", 0700) == 0 || errno != EACCES) {
        chmod(no_search_dir, 0700);
        unlink(no_search_file);
        rmdir(no_search_dir);
        return fail("mkdir no search child");
    }
    errno = 0;
    if (link(path, "/tmp/posix-smoke-no-search/link") == 0 || errno != EACCES) {
        chmod(no_search_dir, 0700);
        unlink(no_search_file);
        rmdir(no_search_dir);
        return fail("link no search child");
    }
    errno = 0;
    if (rename(no_search_file, "/tmp/posix-smoke-no-search/renamed") == 0 || errno != EACCES) {
        chmod(no_search_dir, 0700);
        unlink(no_search_file);
        rmdir(no_search_dir);
        return fail("rename no search child");
    }
    errno = 0;
    if (chmod(no_search_file, 0600) == 0 || errno != EACCES) {
        chmod(no_search_dir, 0700);
        unlink(no_search_file);
        rmdir(no_search_dir);
        return fail("chmod no search file");
    }
    errno = 0;
    if (utime(no_search_file, nullptr) == 0 || errno != EACCES) {
        chmod(no_search_dir, 0700);
        unlink(no_search_file);
        rmdir(no_search_dir);
        return fail("utime no search file");
    }
    errno = 0;
    if (truncate(no_search_file, 0) == 0 || errno != EACCES) {
        chmod(no_search_dir, 0700);
        unlink(no_search_file);
        rmdir(no_search_dir);
        return fail("truncate no search file");
    }
    if (chmod(no_search_dir, 0700) != 0 || unlink(no_search_file) != 0 || rmdir(no_search_dir) != 0) {
        return fail("cleanup no search dir");
    }

    const char* no_write_dir = "/tmp/posix-smoke-no-write";
    const char* no_write_file = "/tmp/posix-smoke-no-write/file";
    const char* no_write_child = "/tmp/posix-smoke-no-write/child";
    unlink(no_write_file);
    rmdir(no_write_child);
    rmdir(no_write_dir);
    if (mkdir(no_write_dir, 0700) != 0) {
        return fail("mkdir no write dir");
    }
    fd = open(no_write_file, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        rmdir(no_write_dir);
        return fail("open no write file");
    }
    close(fd);
    if (mkdir(no_write_child, 0700) != 0 || chmod(no_write_dir, 0500) != 0) {
        chmod(no_write_dir, 0700);
        unlink(no_write_file);
        rmdir(no_write_child);
        rmdir(no_write_dir);
        return fail("setup no write dir");
    }
    errno = 0;
    fd = open("/tmp/posix-smoke-no-write/new", O_CREAT | O_RDWR, 0600);
    if (fd >= 0 || errno != EACCES) {
        if (fd >= 0) {
            close(fd);
            unlink("/tmp/posix-smoke-no-write/new");
        }
        chmod(no_write_dir, 0700);
        unlink(no_write_file);
        rmdir(no_write_child);
        rmdir(no_write_dir);
        return fail("open no write child");
    }
    errno = 0;
    if (mkdir("/tmp/posix-smoke-no-write/newdir", 0700) == 0 || errno != EACCES) {
        chmod(no_write_dir, 0700);
        unlink(no_write_file);
        rmdir(no_write_child);
        rmdir(no_write_dir);
        return fail("mkdir no write child");
    }
    errno = 0;
    if (unlink(no_write_file) == 0 || errno != EACCES) {
        chmod(no_write_dir, 0700);
        unlink(no_write_file);
        rmdir(no_write_child);
        rmdir(no_write_dir);
        return fail("unlink no write file");
    }
    errno = 0;
    if (rmdir(no_write_child) == 0 || errno != EACCES) {
        chmod(no_write_dir, 0700);
        unlink(no_write_file);
        rmdir(no_write_child);
        rmdir(no_write_dir);
        return fail("rmdir no write child");
    }
    errno = 0;
    if (link(path, "/tmp/posix-smoke-no-write/link") == 0 || errno != EACCES) {
        chmod(no_write_dir, 0700);
        unlink("/tmp/posix-smoke-no-write/link");
        unlink(no_write_file);
        rmdir(no_write_child);
        rmdir(no_write_dir);
        return fail("link no write child");
    }
    errno = 0;
    if (rename(no_write_file, "/tmp/posix-smoke-no-write/renamed") == 0 || errno != EACCES) {
        chmod(no_write_dir, 0700);
        unlink(no_write_file);
        unlink("/tmp/posix-smoke-no-write/renamed");
        rmdir(no_write_child);
        rmdir(no_write_dir);
        return fail("rename old no write child");
    }
    errno = 0;
    if (rename(path, "/tmp/posix-smoke-no-write/renamed") == 0 || errno != EACCES) {
        chmod(no_write_dir, 0700);
        unlink(no_write_file);
        unlink("/tmp/posix-smoke-no-write/renamed");
        rmdir(no_write_child);
        rmdir(no_write_dir);
        return fail("rename new no write child");
    }
    if (chmod(no_write_dir, 0700) != 0 || unlink(no_write_file) != 0 ||
        rmdir(no_write_child) != 0 || rmdir(no_write_dir) != 0) {
        return fail("cleanup no write dir");
    }

    const char* no_read_dir = "/tmp/posix-smoke-no-read";
    rmdir(no_read_dir);
    if (mkdir(no_read_dir, 0700) != 0 || chmod(no_read_dir, 0100) != 0) {
        chmod(no_read_dir, 0700);
        rmdir(no_read_dir);
        return fail("setup no read dir");
    }
    errno = 0;
    DIR* no_read_handle = opendir(no_read_dir);
    if (no_read_handle != nullptr || errno != EACCES) {
        if (no_read_handle != nullptr) {
            closedir(no_read_handle);
        }
        chmod(no_read_dir, 0700);
        rmdir(no_read_dir);
        return fail("opendir no read dir");
    }
    errno = 0;
    fd = open(no_read_dir, O_RDONLY);
    if (fd >= 0 || errno != EACCES) {
        if (fd >= 0) {
            close(fd);
        }
        chmod(no_read_dir, 0700);
        rmdir(no_read_dir);
        return fail("open no read dir");
    }
    if (chmod(no_read_dir, 0700) != 0 || rmdir(no_read_dir) != 0) {
        return fail("cleanup no read dir");
    }

    const char* no_exec_dir = "/tmp/posix-smoke-no-exec";
    rmdir(no_exec_dir);
    if (mkdir(no_exec_dir, 0700) != 0 || chmod(no_exec_dir, 0600) != 0) {
        chmod(no_exec_dir, 0700);
        rmdir(no_exec_dir);
        return fail("setup no exec dir");
    }
    errno = 0;
    if (chdir(no_exec_dir) == 0 || errno != EACCES) {
        chmod(no_exec_dir, 0700);
        rmdir(no_exec_dir);
        return fail("chdir no exec dir");
    }
    if (chmod(no_exec_dir, 0700) != 0 || rmdir(no_exec_dir) != 0) {
        return fail("cleanup no exec dir");
    }

    struct utimbuf times {};
    times.actime = 10;
    times.modtime = 20;
    if (utime(path, &times) != 0) {
        return fail("utime");
    }
    if (stat(path, &st) != 0 || st.st_atime != 10 || st.st_mtime != 20) {
        return fail("stat utime timestamps");
    }
    struct timespec ts[2] {};
    ts[0].tv_sec = 30;
    ts[0].tv_nsec = 0;
    ts[1].tv_sec = 40;
    ts[1].tv_nsec = 0;
    if (utimensat(AT_FDCWD, path, ts, 0) != 0 || stat(path, &st) != 0 ||
        st.st_atime != 30 || st.st_mtime != 40) {
        return fail("utimensat");
    }
    fd = open(path, O_RDWR);
    if (fd < 0) {
        return fail("open futimens file");
    }
    ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = 50;
    ts[1].tv_nsec = 0;
    if (futimens(fd, ts) != 0) {
        close(fd);
        return fail("futimens");
    }
    close(fd);
    if (stat(path, &st) != 0 || st.st_atime != 30 || st.st_mtime != 50) {
        return fail("stat futimens timestamps");
    }
    ts[0].tv_sec = 0;
    ts[0].tv_nsec = 1000000000L;
    ts[1].tv_sec = 0;
    ts[1].tv_nsec = 0;
    errno = 0;
    if (utimensat(AT_FDCWD, path, ts, 0) == 0 || errno != EINVAL) {
        return fail("utimensat invalid nsec");
    }

    if (rename(path, renamed) != 0 || access(renamed, F_OK) != 0) {
        return fail("rename");
    }

    DIR* dir = opendir("/tmp");
    if (!dir) {
        return fail("opendir");
    }
    bool saw_dot = false;
    bool saw_dotdot = false;
    bool saw_entry = false;
    while (dirent* entry = readdir(dir)) {
        if (strcmp(entry->d_name, ".") == 0) {
            saw_dot = true;
        }
        if (strcmp(entry->d_name, "..") == 0) {
            saw_dotdot = true;
        }
        if (strcmp(entry->d_name, "posix-smoke-renamed") == 0) {
            saw_entry = true;
        }
    }
    closedir(dir);
    if (!saw_dot || !saw_dotdot || !saw_entry) {
        return fail("readdir");
    }

    const char* nlink_dir = "/tmp/posix-smoke-nlink-dir";
    rmdir(nlink_dir);
    struct stat tmp_before {};
    struct stat tmp_after_mkdir {};
    struct stat tmp_after_rmdir {};
    if (stat("/tmp", &tmp_before) != 0 || mkdir(nlink_dir, 0700) != 0 ||
        stat("/tmp", &tmp_after_mkdir) != 0 || tmp_after_mkdir.st_nlink != tmp_before.st_nlink + 1 ||
        rmdir(nlink_dir) != 0 || stat("/tmp", &tmp_after_rmdir) != 0 ||
        tmp_after_rmdir.st_nlink != tmp_before.st_nlink) {
        return fail("directory nlink");
    }

    errno = 0;
    dir = opendir(renamed);
    if (dir != nullptr || errno != ENOTDIR) {
        if (dir != nullptr) {
            closedir(dir);
        }
        return fail("opendir file");
    }

    if (unlink(hardlink) != 0 || unlink(renamed) != 0) {
        return fail("unlink");
    }
    const char* remove_dir = "/tmp/posix-smoke-remove-dir";
    rmdir(remove_dir);
    if (mkdir(remove_dir, 0700) != 0 || remove(remove_dir) != 0 ||
        access(remove_dir, F_OK) == 0 || errno != ENOENT) {
        return fail("remove directory");
    }
    return 0;
}

int test_mmap()
{
    errno = 0;
    if (mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE | 0x40000000, -1, 0) != MAP_FAILED ||
        errno != EINVAL) {
        return fail("mmap invalid flags");
    }
    errno = 0;
    if (mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED ||
        errno != EINVAL) {
        return fail("mmap fixed null");
    }

    char* anonymous = (char*)mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (anonymous == MAP_FAILED) {
        return fail("mmap anonymous");
    }
    errno = 0;
    if (mprotect(anonymous, 4096, PROT_READ | 0x80) == 0 || errno != EINVAL) {
        munmap(anonymous, 4096);
        return fail("mprotect invalid prot");
    }
    errno = 0;
    if (munmap(anonymous + 1, 4096) == 0 || errno != EINVAL) {
        munmap(anonymous, 4096);
        return fail("munmap unaligned");
    }
    strcpy(anonymous, "mapped");
    if (strcmp(anonymous, "mapped") != 0) {
        munmap(anonymous, 4096);
        return fail("mmap anonymous contents");
    }

    char* fixed = (char*)mmap(anonymous, 4096, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (fixed != anonymous || fixed[0] != '\0') {
        munmap(anonymous, 4096);
        return fail("mmap fixed replace");
    }

    if (munmap(anonymous, 4096) != 0) {
        return fail("munmap anonymous");
    }

    char* none = (char*)mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (none == MAP_FAILED) {
        return fail("mmap prot none");
    }
    if (mprotect(none, 4096, PROT_READ | PROT_WRITE) != 0) {
        munmap(none, 4096);
        return fail("mprotect restore");
    }
    strcpy(none, "restored");
    if (strcmp(none, "restored") != 0 || munmap(none, 4096) != 0) {
        return fail("mmap prot none restore");
    }

    const char* path = "/tmp/posix-smoke-map";
    unlink(path);
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        return fail("open mmap file");
    }
    const char payload[] = "file-backed-map";
    if (write(fd, payload, sizeof(payload)) != (ssize_t)sizeof(payload)) {
        close(fd);
        return fail("write mmap file");
    }

    char* mapped = (char*)mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd);
        return fail("mmap file");
    }
    int result = require(memcmp(mapped, payload, sizeof(payload)) == 0, "mmap file payload");
    if (mprotect(mapped, 4096, PROT_READ | PROT_WRITE) != 0 && result == 0) {
        result = fail("mprotect private file writable");
    }
    mapped[0] = 'X';
    if (munmap(mapped, 4096) != 0 && result == 0) {
        result = fail("munmap file");
    }
    if (lseek(fd, 0, SEEK_SET) != 0 && result == 0) {
        result = fail("seek private mmap file");
    }
    char verify[sizeof(payload)] {};
    if ((result == 0 && read(fd, verify, sizeof(verify)) != (ssize_t)sizeof(verify)) ||
        (result == 0 && memcmp(verify, payload, sizeof(payload)) != 0)) {
        result = fail("mmap private no writeback");
    }

    char* shared = (char*)mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED && result == 0) {
        result = fail("mmap shared writable");
    }
    if (shared != MAP_FAILED) {
        shared[0] = 'S';
        if (msync(shared, 4096, MS_SYNC) != 0 && result == 0) {
            result = fail("msync shared writable");
        }
        if (lseek(fd, 0, SEEK_SET) != 0 && result == 0) {
            result = fail("seek shared mmap file");
        }
        if ((result == 0 && read(fd, verify, sizeof(verify)) != (ssize_t)sizeof(verify)) ||
            (result == 0 && verify[0] != 'S')) {
            result = fail("mmap shared writeback");
        }
        shared[1] = 'H';
        if (munmap(shared, 4096) != 0 && result == 0) {
            result = fail("munmap shared writable");
        }
        if (lseek(fd, 0, SEEK_SET) != 0 && result == 0) {
            result = fail("seek munmap shared file");
        }
        if ((result == 0 && read(fd, verify, sizeof(verify)) != (ssize_t)sizeof(verify)) ||
            (result == 0 && verify[1] != 'H')) {
            result = fail("munmap shared writeback");
        }
    }

    errno = 0;
    if (msync(nullptr, 4096, MS_SYNC) == 0 || errno != EINVAL) {
        result = fail("msync invalid");
    }
    close(fd);

    int ro_fd = open(path, O_RDONLY);
    if (ro_fd < 0 && result == 0) {
        result = fail("open mmap read only fd");
    }
    if (ro_fd >= 0) {
        char* private_write = (char*)mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, ro_fd, 0);
        if (private_write == MAP_FAILED && result == 0) {
            result = fail("mmap private writable read only fd");
        }
        if (private_write != MAP_FAILED) {
            private_write[0] = 'P';
            if (munmap(private_write, 4096) != 0 && result == 0) {
                result = fail("munmap private writable read only fd");
            }
        }
        errno = 0;
        char* denied = (char*)mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ro_fd, 0);
        if (denied != MAP_FAILED || errno != EBADF) {
            if (denied != MAP_FAILED) {
                munmap(denied, 4096);
            }
            result = fail("mmap shared writable read only fd");
        }
        close(ro_fd);
    }
    unlink(path);
    return result;
}

int test_ipc()
{
    int fds[2] {};
    if (pipe(fds) != 0) {
        return fail("pipe");
    }

    pollfd pfd {};
    pfd.fd = fds[1];
    pfd.events = POLLOUT;
    if (poll(&pfd, 1, 0) < 0 || (pfd.revents & POLLOUT) == 0) {
        close(fds[0]);
        close(fds[1]);
        return fail("poll write");
    }

    const char byte = 'x';
    if (write(fds[1], &byte, 1) != 1) {
        close(fds[0]);
        close(fds[1]);
        return fail("pipe write");
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fds[0], &readfds);
    timeval timeout {};
    if (select(fds[0] + 1, &readfds, nullptr, nullptr, &timeout) < 0 ||
        !FD_ISSET(fds[0], &readfds)) {
        close(fds[0]);
        close(fds[1]);
        return fail("select read");
    }

    char out = 0;
    if (read(fds[0], &out, 1) != 1 || out != byte) {
        close(fds[0]);
        close(fds[1]);
        return fail("pipe read");
    }
    close(fds[0]);
    close(fds[1]);
    return 0;
}

int test_signals()
{
    sigset_t set {};
    sigset_t old {};
    if (sigemptyset(&set) != 0 || sigaddset(&set, SIGTERM) != 0 ||
        sigismember(&set, SIGTERM) != 1) {
        return fail("sigset");
    }
    if (sigprocmask(SIG_BLOCK, &set, &old) != 0 ||
        sigprocmask(SIG_SETMASK, &old, nullptr) != 0) {
        return fail("sigprocmask");
    }

    struct sigaction current {};
    if (sigaction(SIGTERM, nullptr, &current) != 0) {
        return fail("sigaction query");
    }

    struct sigaction action {};
    action.sa_handler = SIG_IGN;
    action.sa_mask = set;
    action.sa_flags = 0x1234;
    struct sigaction previous {};
    if (sigaction(SIGTERM, &action, &previous) != 0 ||
        previous.sa_handler != current.sa_handler ||
        sigaction(SIGTERM, nullptr, &current) != 0 ||
        current.sa_handler != SIG_IGN ||
        current.sa_mask != set ||
        current.sa_flags != 0x1234) {
        return fail("sigaction state");
    }

    sigset_t filled {};
    if (sigfillset(&filled) != 0 || sigismember(&filled, SIGKILL) != 0) {
        return fail("sigfillset unblockable");
    }

    if (sigprocmask(SIG_BLOCK, &set, &old) != 0 ||
        raise(SIGTERM) != 0 ||
        sigprocmask(SIG_SETMASK, &old, nullptr) != 0) {
        return fail("blocked ignored signal");
    }

    if (pthread_sigmask(SIG_BLOCK, &set, &old) != 0 ||
        pthread_sigmask(SIG_SETMASK, &old, nullptr) != 0) {
        return fail("pthread_sigmask");
    }

    struct sigaction count_action {};
    count_action.sa_handler = count_signal_handler;
    if (sigemptyset(&count_action.sa_mask) != 0 || sigaddset(&count_action.sa_mask, SIGUSR2) != 0) {
        return fail("handler mask setup");
    }
    if (sigaction(SIGUSR1, &count_action, nullptr) != 0 || raise(SIGUSR1) != 0 || g_signal_count != 1) {
        return fail("signal handler return");
    }

    static char alt_stack[SIGSTKSZ];
    stack_t stack {};
    stack.ss_sp = alt_stack;
    stack.ss_size = sizeof(alt_stack);
    stack.ss_flags = 0;
    g_altstack_low = alt_stack;
    g_altstack_high = alt_stack + sizeof(alt_stack);
    if (sigaltstack(&stack, nullptr) != 0) {
        return fail("sigaltstack install");
    }
    struct sigaction alt_action {};
    alt_action.sa_handler = altstack_signal_handler;
    alt_action.sa_flags = SA_ONSTACK;
    if (sigemptyset(&alt_action.sa_mask) != 0 ||
        sigaction(SIGUSR2, &alt_action, nullptr) != 0 ||
        raise(SIGUSR2) != 0 ||
        g_altstack_seen != 1) {
        return fail("sigaltstack delivery");
    }
    stack.ss_sp = nullptr;
    stack.ss_size = 0;
    stack.ss_flags = SS_DISABLE;
    if (sigaltstack(&stack, nullptr) != 0) {
        return fail("sigaltstack disable");
    }

    struct sigaction thread_action {};
    thread_action.sa_handler = thread_signal_handler;
    if (sigemptyset(&thread_action.sa_mask) != 0 || sigaction(SIGUSR2, &thread_action, nullptr) != 0) {
        return fail("thread signal action");
    }
    pthread_t thread {};
    if (pthread_create(&thread, nullptr, signal_wait_thread, nullptr) != 0) {
        return fail("pthread signal create");
    }
    if (pthread_kill(thread, SIGUSR2) != 0) {
        return fail("pthread_kill");
    }
    void* thread_result = nullptr;
    if (pthread_join(thread, &thread_result) != 0 || thread_result == nullptr) {
        return fail("pthread signal join");
    }

    action.sa_handler = SIG_DFL;
    action.sa_mask = 0;
    action.sa_flags = 0;
    if (sigaction(SIGTERM, &action, nullptr) != 0) {
        return fail("sigaction restore");
    }

    errno = 0;
    if (signal(SIGKILL, SIG_IGN) != SIG_ERR || errno != EINVAL) {
        return fail("signal sigkill");
    }
    return 0;
}

int test_threads_and_semaphores()
{
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    if (pthread_mutex_lock(&mutex) != 0 || pthread_mutex_unlock(&mutex) != 0) {
        return fail("pthread mutex");
    }

    pthread_mutexattr_t mutex_attr {};
    if (pthread_mutexattr_init(&mutex_attr) != 0 ||
        pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE) != 0) {
        return fail("pthread mutexattr recursive");
    }
    pthread_mutex_t recursive_mutex {};
    if (pthread_mutex_init(&recursive_mutex, &mutex_attr) != 0 ||
        pthread_mutex_lock(&recursive_mutex) != 0 ||
        pthread_mutex_lock(&recursive_mutex) != 0 ||
        pthread_mutex_unlock(&recursive_mutex) != 0 ||
        pthread_mutex_unlock(&recursive_mutex) != 0 ||
        pthread_mutex_destroy(&recursive_mutex) != 0) {
        return fail("pthread recursive mutex");
    }

    if (pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_ERRORCHECK) != 0) {
        return fail("pthread mutexattr errorcheck");
    }
    pthread_mutex_t errorcheck_mutex {};
    if (pthread_mutex_init(&errorcheck_mutex, &mutex_attr) != 0 ||
        pthread_mutex_lock(&errorcheck_mutex) != 0 ||
        pthread_mutex_lock(&errorcheck_mutex) != EDEADLK ||
        pthread_mutex_unlock(&errorcheck_mutex) != 0 ||
        pthread_mutex_destroy(&errorcheck_mutex) != 0 ||
        pthread_mutexattr_destroy(&mutex_attr) != 0) {
        return fail("pthread errorcheck mutex");
    }

    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    if (pthread_cond_signal(&cond) != 0 || pthread_cond_broadcast(&cond) != 0) {
        return fail("pthread cond");
    }
    int cond_ready = 0;
    CondContext cond_context { &mutex, &cond, &cond_ready };
    pthread_t cond_thread {};
    if (pthread_mutex_lock(&mutex) != 0 ||
        pthread_create(&cond_thread, nullptr, cond_signal_thread, &cond_context) != 0) {
        return fail("pthread cond create");
    }
    while (!cond_ready) {
        if (pthread_cond_wait(&cond, &mutex) != 0) {
            return fail("pthread cond wait");
        }
    }
    if (pthread_mutex_unlock(&mutex) != 0) {
        return fail("pthread cond unlock");
    }
    void* cond_result = nullptr;
    if (pthread_join(cond_thread, &cond_result) != 0 || cond_result == nullptr) {
        return fail("pthread cond join");
    }

    pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
    if (pthread_rwlock_rdlock(&rwlock) != 0 ||
        pthread_rwlock_trywrlock(&rwlock) != EBUSY ||
        pthread_rwlock_unlock(&rwlock) != 0 ||
        pthread_rwlock_wrlock(&rwlock) != 0 ||
        pthread_rwlock_tryrdlock(&rwlock) != EBUSY ||
        pthread_rwlock_unlock(&rwlock) != 0 ||
        pthread_rwlock_destroy(&rwlock) != 0) {
        return fail("pthread rwlock");
    }

    sem_t sem {};
    int value = 0;
    if (sem_init(&sem, 0, 1) != 0 || sem_wait(&sem) != 0 ||
        sem_trywait(&sem) != -1 || errno != EAGAIN ||
        sem_post(&sem) != 0 || sem_getvalue(&sem, &value) != 0 || value != 1 ||
        sem_destroy(&sem) != 0) {
        return fail("semaphore");
    }
    if (sem_init(&sem, 0, 0) != 0) {
        return fail("semaphore init wait");
    }
    SemContext sem_context { &sem };
    pthread_t sem_thread {};
    if (pthread_create(&sem_thread, nullptr, sem_post_thread, &sem_context) != 0 ||
        sem_wait(&sem) != 0) {
        return fail("semaphore wait/post");
    }
    void* sem_result = nullptr;
    if (pthread_join(sem_thread, &sem_result) != 0 || sem_result == nullptr || sem_destroy(&sem) != 0) {
        return fail("semaphore wait join");
    }

    errno = 0;
    g_tls_destructor_count = 0;
    if (pthread_key_create(&g_tls_key, tls_destructor) != 0) {
        return fail("pthread key create");
    }
    pthread_t tls_thread {};
    if (pthread_create(&tls_thread, nullptr, tls_errno_thread, nullptr) != 0) {
        return fail("pthread tls create");
    }
    void* tls_result = nullptr;
    if (pthread_join(tls_thread, &tls_result) != 0 ||
        tls_result != reinterpret_cast<void*>(static_cast<unsigned long>(EACCES)) ||
        errno != 0 ||
        g_tls_destructor_count != 1 ||
        pthread_key_delete(g_tls_key) != 0) {
        return fail("pthread tls errno");
    }

    pthread_t cancel_thread {};
    if (pthread_create(&cancel_thread, nullptr, cancel_wait_thread, nullptr) != 0 ||
        pthread_cancel(cancel_thread) != 0) {
        return fail("pthread cancel create");
    }
    void* cancel_result = nullptr;
    if (pthread_join(cancel_thread, &cancel_result) != 0 || cancel_result != PTHREAD_CANCELED) {
        return fail("pthread cancel join");
    }
    return 0;
}

int test_network_headers()
{
    in_addr addr {};
    if (inet_pton(AF_INET, "127.0.0.1", &addr) != 1) {
        return fail("inet_pton");
    }

    char text[INET_ADDRSTRLEN] {};
    if (!inet_ntop(AF_INET, &addr, text, sizeof(text)) || strcmp(text, "127.0.0.1") != 0) {
        return fail("inet_ntop");
    }

    if (ntohl(htonl(0x12345678U)) != 0x12345678U || ntohs(htons(0x1234U)) != 0x1234U) {
        return fail("byte order");
    }

    errno = 0;
    if (socket(AF_UNIX, SOCK_STREAM, 0) != -1 || errno != EAFNOSUPPORT) {
        return fail("socket af unsupported");
    }

    int stream_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (stream_fd < 0) {
        return fail("socket stream");
    }
    sockaddr_in stream_local {};
    stream_local.sin_family = AF_INET;
    stream_local.sin_port = htons(43211);
    stream_local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(stream_fd, reinterpret_cast<sockaddr*>(&stream_local), sizeof(stream_local)) != 0) {
        close(stream_fd);
        return fail("bind stream");
    }
    if (listen(stream_fd, 2) != 0) {
        close(stream_fd);
        return fail("listen stream");
    }
    errno = 0;
    if (accept(stream_fd, nullptr, nullptr) != -1 || errno != EAGAIN) {
        close(stream_fd);
        return fail("accept empty stream");
    }
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) {
        close(stream_fd);
        return fail("socket stream client");
    }
    if (connect(client_fd, reinterpret_cast<sockaddr*>(&stream_local), sizeof(stream_local)) != 0) {
        close(client_fd);
        close(stream_fd);
        return fail("connect stream");
    }
    pollfd stream_poll {};
    stream_poll.fd = stream_fd;
    stream_poll.events = POLLIN;
    if (poll(&stream_poll, 1, 0) != 1 || (stream_poll.revents & POLLIN) == 0) {
        close(client_fd);
        close(stream_fd);
        return fail("poll stream accept");
    }
    int accepted_fd = accept(stream_fd, nullptr, nullptr);
    if (accepted_fd < 0) {
        close(client_fd);
        close(stream_fd);
        return fail("accept stream");
    }
    const char stream_message[] = "stream-local";
    if (send(client_fd, stream_message, sizeof(stream_message), 0) != (ssize_t)sizeof(stream_message)) {
        close(accepted_fd);
        close(client_fd);
        close(stream_fd);
        return fail("send stream client");
    }
    stream_poll.fd = accepted_fd;
    stream_poll.events = POLLIN;
    stream_poll.revents = 0;
    if (poll(&stream_poll, 1, 0) != 1 || (stream_poll.revents & POLLIN) == 0) {
        close(accepted_fd);
        close(client_fd);
        close(stream_fd);
        return fail("poll stream read");
    }
    char stream_buffer[sizeof(stream_message)] {};
    if (recv(accepted_fd, stream_buffer, 6, 0) != 6 || memcmp(stream_buffer, "stream", 6) != 0) {
        close(accepted_fd);
        close(client_fd);
        close(stream_fd);
        return fail("recv stream partial");
    }
    if (recv(accepted_fd, stream_buffer, sizeof(stream_buffer), 0) != (ssize_t)(sizeof(stream_message) - 6) ||
        memcmp(stream_buffer, "-local", sizeof(stream_message) - 6) != 0) {
        close(accepted_fd);
        close(client_fd);
        close(stream_fd);
        return fail("recv stream remainder");
    }
    if (send(accepted_fd, stream_message, 4, 0) != 4) {
        close(accepted_fd);
        close(client_fd);
        close(stream_fd);
        return fail("send stream accepted");
    }
    memset(stream_buffer, 0, sizeof(stream_buffer));
    if (recv(client_fd, stream_buffer, sizeof(stream_buffer), 0) != 4 || memcmp(stream_buffer, "stre", 4) != 0) {
        close(accepted_fd);
        close(client_fd);
        close(stream_fd);
        return fail("recv stream client");
    }
    if (shutdown(client_fd, SHUT_WR) != 0 || recv(accepted_fd, stream_buffer, sizeof(stream_buffer), 0) != 0) {
        close(accepted_fd);
        close(client_fd);
        close(stream_fd);
        return fail("stream eof");
    }
    close(accepted_fd);
    close(client_fd);
    close(stream_fd);

    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        return fail("socket datagram");
    }

    int option = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) != 0) {
        close(socket_fd);
        return fail("setsockopt reuseaddr");
    }
    option = 0;
    socklen_t option_len = sizeof(option);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &option, &option_len) != 0 ||
        option != 1 || option_len != sizeof(option)) {
        close(socket_fd);
        return fail("getsockopt reuseaddr");
    }
    option = 0;
    option_len = sizeof(option);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_TYPE, &option, &option_len) != 0 ||
        option != SOCK_DGRAM) {
        close(socket_fd);
        return fail("getsockopt type");
    }

    sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_port = htons(43210);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        close(socket_fd);
        return fail("bind datagram");
    }
    int conflict_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (conflict_fd < 0) {
        close(socket_fd);
        return fail("socket bind conflict");
    }
    errno = 0;
    if (bind(conflict_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0 || errno != EADDRINUSE) {
        close(conflict_fd);
        close(socket_fd);
        return fail("bind datagram conflict");
    }
    close(conflict_fd);

    int ephemeral_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ephemeral_fd < 0) {
        close(socket_fd);
        return fail("socket ephemeral bind");
    }
    sockaddr_in ephemeral {};
    ephemeral.sin_family = AF_INET;
    ephemeral.sin_port = 0;
    ephemeral.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ephemeral_fd, reinterpret_cast<sockaddr*>(&ephemeral), sizeof(ephemeral)) != 0) {
        close(ephemeral_fd);
        close(socket_fd);
        return fail("bind ephemeral datagram");
    }
    close(ephemeral_fd);
    if (connect(socket_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        close(socket_fd);
        return fail("connect datagram");
    }

    pollfd socket_poll {};
    socket_poll.fd = socket_fd;
    socket_poll.events = POLLOUT;
    if (poll(&socket_poll, 1, 0) != 1 || (socket_poll.revents & POLLOUT) == 0) {
        close(socket_fd);
        return fail("poll socket");
    }

    const char datagram[] = "udp-local";
    if (send(socket_fd, datagram, sizeof(datagram), 0) != (ssize_t)sizeof(datagram)) {
        close(socket_fd);
        return fail("send datagram");
    }
    socket_poll.events = POLLIN;
    socket_poll.revents = 0;
    if (poll(&socket_poll, 1, 0) != 1 || (socket_poll.revents & POLLIN) == 0) {
        close(socket_fd);
        return fail("poll socket datagram read");
    }
    char datagram_buffer[sizeof(datagram)] {};
    if (recv(socket_fd, datagram_buffer, sizeof(datagram_buffer), 0) != (ssize_t)sizeof(datagram) ||
        memcmp(datagram_buffer, datagram, sizeof(datagram)) != 0) {
        close(socket_fd);
        return fail("recv datagram");
    }
    errno = 0;
    if (recv(socket_fd, datagram_buffer, sizeof(datagram_buffer), MSG_DONTWAIT) != -1 || errno != EAGAIN) {
        close(socket_fd);
        return fail("recv datagram empty");
    }
    if (sendto(socket_fd, datagram, 4, 0, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 4) {
        close(socket_fd);
        return fail("sendto datagram");
    }
    socklen_t source_len = sizeof(local);
    if (recvfrom(socket_fd, datagram_buffer, sizeof(datagram_buffer), 0,
                 reinterpret_cast<sockaddr*>(&local), &source_len) != 4) {
        close(socket_fd);
        return fail("recvfrom datagram");
    }
    socket_poll.events = POLLOUT;
    socket_poll.revents = 0;
    if (shutdown(socket_fd, SHUT_WR) != 0) {
        close(socket_fd);
        return fail("shutdown socket");
    }
    socket_poll.revents = 0;
    if (poll(&socket_poll, 1, 0) != 0 || socket_poll.revents != 0) {
        close(socket_fd);
        return fail("poll socket write shutdown");
    }
    errno = 0;
    if (send(socket_fd, "", 0, 0) != -1 || errno != EPIPE) {
        close(socket_fd);
        return fail("send shutdown");
    }
    if (shutdown(socket_fd, SHUT_RD) != 0) {
        close(socket_fd);
        return fail("shutdown socket read");
    }
    socket_poll.events = POLLIN | POLLHUP;
    socket_poll.revents = 0;
    if (poll(&socket_poll, 1, 0) != 1 || (socket_poll.revents & POLLHUP) == 0) {
        close(socket_fd);
        return fail("poll socket hup");
    }
    close(socket_fd);

    errno = 0;
    int regular_fd = open("/tmp/posix-smoke-socket-check", O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (regular_fd < 0) {
        return fail("socket regular open");
    }
    if (getsockopt(regular_fd, SOL_SOCKET, SO_TYPE, &option, &option_len) != -1 ||
        errno != ENOTSOCK) {
        close(regular_fd);
        unlink("/tmp/posix-smoke-socket-check");
        return fail("getsockopt nonsocket");
    }
    close(regular_fd);
    unlink("/tmp/posix-smoke-socket-check");

    addrinfo hints {};
    addrinfo* result = nullptr;
    int gai_result = getaddrinfo("localhost", "80", &hints, &result);
    if (gai_result != EAI_NONAME || result != nullptr) {
        return fail("getaddrinfo stub");
    }
    freeaddrinfo(result);
    return 0;
}

} // namespace

int main(int, char**)
{
    if (test_filesystem() != 0 ||
        test_mmap() != 0 ||
        test_ipc() != 0 ||
        test_signals() != 0 ||
        test_threads_and_semaphores() != 0 ||
        test_network_headers() != 0) {
        return 1;
    }

    printf("posix-smoke: ok\n");
    return 0;
}
