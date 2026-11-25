// native-threads: every wait of libinstant's pthreads and semaphores (they sleep
// on futexes, as mlibc's do). Run by tools/run-thread-suite.sh; exits 0 only if
// everything passed.
#include <syscall.hpp>

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

namespace {
int g_total = 0, g_failed = 0;

void say(const char* s) {
    std::uint64_t n = 0;
    while (s[n]) n++;
    // Straight to the handle: libc's write(1) prefers the console service.
    _syscall_impl(static_cast<std::uint64_t>(std::Syscall::Write), 1, reinterpret_cast<std::uint64_t>(s), n);
}
void say(long long v) {
    char b[24];
    int i = 23;
    b[i] = 0;
    const bool negative = v < 0;
    unsigned long long u = negative ? 0ULL - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
    do {
        b[--i] = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u);
    if (negative) b[--i] = '-';
    say(b + i);
}
void check(bool ok, const char* what, long long got = 0) {
    g_total++;
    if (!ok) {
        g_failed++;
        say("  FAIL: ");
        say(what);
        say(" (got ");
        say(got);
        say(")\n");
    }
}
long long now_us() {
    struct timespec t {};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}
struct timespec in_ms(long long ms) {
    struct timespec t {};
    clock_gettime(CLOCK_REALTIME, &t);
    const long long ns = t.tv_sec * 1000000000LL + t.tv_nsec + ms * 1000000;
    return {static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
}
void sleep_ms(unsigned ms) {
    _syscall_impl(static_cast<std::uint64_t>(std::Syscall::Sleep), ms);
}
std::uint64_t tsc() {
    std::uint32_t a, d;
    asm volatile("rdtsc" : "=a"(a), "=d"(d));
    return (static_cast<std::uint64_t>(d) << 32) | a;
}
void* number(long v) { return reinterpret_cast<void*>(v); }

pthread_t g_threads[16];
void start(int n, void* (*fn)(void*)) {
    for (long i = 0; i < n; i++) check(pthread_create(&g_threads[i], nullptr, fn, number(i)) == 0, "pthread_create");
}
long join_all(int n) {
    long sum = 0;
    for (int i = 0; i < n; i++) {
        void* r = nullptr;
        check(pthread_join(g_threads[i], &r) == 0, "pthread_join");
        sum += reinterpret_cast<long>(r);
    }
    return sum;
}

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_a = PTHREAD_COND_INITIALIZER, g_b = PTHREAD_COND_INITIALIZER;
long g_count;
int g_turn, g_ready, g_waiting, g_woken, g_flag;

void* locker(void* arg) {
    for (int i = 0; i < 5000; i++) {
        pthread_mutex_lock(&g_mutex);
        const long v = g_count;
        if ((i & 127) == static_cast<int>(reinterpret_cast<long>(arg))) std::yield();
        g_count = v + 1;
        pthread_mutex_unlock(&g_mutex);
    }
    return number(1);
}
void* player(void* arg) {
    const int me = static_cast<int>(reinterpret_cast<long>(arg));
    long lost = 0;
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < 3000; i++) {
        while (g_turn != me) {
            struct timespec d = in_ms(3000);
            if (pthread_cond_timedwait(me ? &g_b : &g_a, &g_mutex, &d) == ETIMEDOUT && g_turn != me) {
                lost++;
                g_turn = me;
            }
        }
        g_turn = !me;
        pthread_cond_signal(me ? &g_a : &g_b);
    }
    pthread_mutex_unlock(&g_mutex);
    return number(lost);
}
void* waiter(void*) {
    pthread_mutex_lock(&g_mutex);
    g_waiting++;
    while (!g_ready) pthread_cond_wait(&g_a, &g_mutex);
    if (g_ready < 100) g_ready--;
    g_woken++;
    pthread_mutex_unlock(&g_mutex);
    return nullptr;
}
int locked_read(int* v) {
    pthread_mutex_lock(&g_mutex);
    const int r = *v;
    pthread_mutex_unlock(&g_mutex);
    return r;
}
void until(int* v, int want) {
    for (int i = 0; i < 3000 && locked_read(v) != want; i++) sleep_ms(1);
}

sem_t g_s1, g_s2;
void* sem_player(void* arg) {
    for (int i = 0; i < 3000; i++) {
        if (arg) {
            sem_wait(&g_s2);
            sem_post(&g_s1);
        } else {
            sem_post(&g_s2);
            sem_wait(&g_s1);
        }
    }
    return nullptr;
}

pthread_rwlock_t g_rw = PTHREAD_RWLOCK_INITIALIZER;
int g_readers, g_writers, g_bad;
void* rw_user(void* arg) {
    unsigned seed = static_cast<unsigned>(reinterpret_cast<long>(arg)) + 1;
    for (int i = 0; i < 2000; i++) {
        seed = seed * 1664525u + 1013904223u;
        if ((seed >> 16) % 6) {
            pthread_rwlock_rdlock(&g_rw);
            __atomic_add_fetch(&g_readers, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&g_writers, __ATOMIC_SEQ_CST)) g_bad = 1;
            if ((i & 31) == 0) std::yield();
            __atomic_sub_fetch(&g_readers, 1, __ATOMIC_SEQ_CST);
        } else {
            pthread_rwlock_wrlock(&g_rw);
            if (__atomic_add_fetch(&g_writers, 1, __ATOMIC_SEQ_CST) != 1 || __atomic_load_n(&g_readers, __ATOMIC_SEQ_CST)) g_bad = 1;
            __atomic_sub_fetch(&g_writers, 1, __ATOMIC_SEQ_CST);
        }
        pthread_rwlock_unlock(&g_rw);
    }
    return nullptr;
}

void* sleeper(void* arg) {
    sleep_ms(80);
    return number(reinterpret_cast<long>(arg) + 5);
}
void* cond_cancelled(void*) {
    pthread_mutex_lock(&g_mutex);
    __atomic_store_n(&g_flag, 1, __ATOMIC_SEQ_CST);
    for (;;) pthread_cond_wait(&g_b, &g_mutex);
}
void* sem_cancelled(void*) {
    __atomic_store_n(&g_flag, 1, __ATOMIC_SEQ_CST);
    for (;;) sem_wait(&g_s1);
}
void* id_of(void*) { return number(static_cast<long>(std::getpid() == static_cast<std::uint64_t>(0) ? -1 : std::getpid())); }
void* tid_of(void*) { return number(static_cast<long>(std::gettid())); }

std::uint64_t syscall_cost() {
    std::uint64_t best = ~0ULL;
    for (int k = 0; k < 5; k++) {
        const std::uint64_t t0 = tsc();
        for (int i = 0; i < 20000; i++) std::gettid();
        const std::uint64_t t = (tsc() - t0) / 20000;
        if (t < best) best = t;
    }
    return best;
}
}  // namespace

int main(int, char**) {
    pthread_t t {};
    void* r = nullptr;
    pthread_create(&t, nullptr, id_of, nullptr);
    pthread_join(t, &r);
    check(reinterpret_cast<long>(r) == static_cast<long>(std::getpid()), "getpid is the process's in a thread", reinterpret_cast<long>(r));
    pthread_create(&t, nullptr, tid_of, nullptr);
    pthread_join(t, &r);
    check(reinterpret_cast<long>(r) != static_cast<long>(std::getpid()), "gettid is not", reinterpret_cast<long>(r));

    start(6, locker);
    check(join_all(6) == 6 && g_count == 30000, "6 threads x 5000 under a mutex", g_count);
    start(2, player);
    const long lost = join_all(2);
    check(lost == 0, "6000 turns on condition variables, no signal lost", lost);
    start(5, waiter);
    until(&g_waiting, 5);
    sleep_ms(10);
    pthread_mutex_lock(&g_mutex);
    g_ready = 1;
    pthread_cond_signal(&g_a);
    pthread_mutex_unlock(&g_mutex);
    until(&g_woken, 1);
    sleep_ms(20);
    check(locked_read(&g_woken) == 1, "signal lets one of 5 go on", g_woken);
    pthread_mutex_lock(&g_mutex);
    g_ready = 1000;
    pthread_cond_broadcast(&g_a);
    pthread_mutex_unlock(&g_mutex);
    join_all(5);
    check(g_woken == 5, "broadcast the rest", g_woken);

    pthread_mutex_lock(&g_mutex);
    struct timespec d = in_ms(130);
    long long t0 = now_us();
    int e = pthread_cond_timedwait(&g_a, &g_mutex, &d);
    long long took = now_us() - t0;
    pthread_mutex_unlock(&g_mutex);
    check(e == ETIMEDOUT && took >= 128000 && took < 260000, "cond_timedwait of 130 ms (us)", took);

    sem_init(&g_s1, 0, 0);
    sem_init(&g_s2, 0, 0);
    start(2, sem_player);
    join_all(2);
    check(true, "6000 turns on semaphores");
    d = in_ms(40);
    t0 = now_us();
    e = sem_timedwait(&g_s1, &d);
    took = now_us() - t0;
    check(e == -1 && errno == ETIMEDOUT && took >= 38000 && took < 160000, "sem_timedwait of 40 ms (us)", took);

    start(6, rw_user);
    join_all(6);
    check(!g_bad, "rwlock: readers and writers keep apart");

    t0 = now_us();
    pthread_create(&t, nullptr, sleeper, number(7));
    pthread_join(t, &r);
    took = now_us() - t0;
    check(reinterpret_cast<long>(r) == 12 && took >= 78000, "join waits for the thread (us)", took);
    bool ok = true;
    for (long i = 0; i < 300; i++) {
        pthread_create(&t, nullptr, tid_of, nullptr);
        if (pthread_join(t, &r) != 0 || r == nullptr) ok = false;
    }
    check(ok, "300 threads one after the other");

    for (int k = 0; k < 4; k++) {
        __atomic_store_n(&g_flag, 0, __ATOMIC_SEQ_CST);
        pthread_create(&t, nullptr, k & 1 ? sem_cancelled : cond_cancelled, nullptr);
        while (!__atomic_load_n(&g_flag, __ATOMIC_SEQ_CST)) sleep_ms(1);
        sleep_ms(5);
        t0 = now_us();
        pthread_cancel(t);
        pthread_join(t, &r);
        took = now_us() - t0;
        check(r == PTHREAD_CANCELED && took < 200000, k & 1 ? "cancelled in sem_wait (us)" : "cancelled in cond_wait (us)", took);
        if (!(k & 1)) pthread_mutex_unlock(&g_mutex);   // left to the dead thread
        pthread_mutex_init(&g_mutex, nullptr);
    }

    const std::uint64_t alone = syscall_cost();
    g_ready = g_waiting = 0;
    start(8, waiter);
    until(&g_waiting, 8);
    sleep_ms(20);
    const std::uint64_t beside = syscall_cost();
    pthread_mutex_lock(&g_mutex);
    g_ready = 1000;
    pthread_cond_broadcast(&g_a);
    pthread_mutex_unlock(&g_mutex);
    join_all(8);
    check(beside < 2 * alone, "a syscall beside 8 waiting threads (TSC ticks)", static_cast<long long>(beside));
    say("  (a syscall: ");
    say(static_cast<long long>(alone));
    say(" TSC ticks alone, ");
    say(static_cast<long long>(beside));
    say(" beside 8 waiting threads)\nnative-threads: ");
    say(g_total - g_failed);
    say(" of ");
    say(g_total);
    say(" passed\n");
    return g_failed ? 1 : 0;
}
