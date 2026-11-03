// Keeps a copy of the kernel log on the disk InstantOS runs from, for machines
// without a serial port: /kernel.log, which the new lines are appended to four
// times a second (the file stays open; rewriting it whole each time was slow
// enough on FAT32 to fall far behind), so after a hang it lacks at most the
// last quarter second. The previous boot's copy is kept as /previous.log. The
// kernel starts it only when it mounted an InstantOS data partition.

#include <cstdint.hpp>
#include <cstring.hpp>
#include <fcntl.h>
#include <syscall.hpp>

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr std::uint64_t kLogSize = 128 * 1024;
constexpr std::uint64_t kIntervalMs = 250;
constexpr std::uint64_t kHeartbeatRounds = 4;   // one a second
constexpr char kLogPath[] = "/kernel.log";
constexpr char kPreviousPath[] = "/previous.log";   // 8.3: the FAT32 driver writes short names

char gLog[kLogSize];
char gCopy[kLogSize];

void write_str(const char* s) {
    std::serial_write(s, std::strlen(s));
}

bool failed(std::uint64_t result) {
    return static_cast<std::int64_t>(result) < 0;
}

bool write_file(const char* path, const char* data, std::uint64_t size) {
    const std::Handle file = std::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file == fail) {
        return false;
    }
    std::uint64_t done = 0;
    while (done < size) {
        const std::uint64_t written = std::write(file, data + done, size - done);
        if (failed(written) || written == 0) {
            break;
        }
        done += written;
    }
    std::close(file);
    return done == size;
}

// The last boot's /kernel.log becomes /previous.log.
void keep_previous() {
    const std::Handle file = std::open(kLogPath, O_RDONLY);
    if (file == fail) {
        return;
    }
    std::uint64_t size = 0;
    while (size < kLogSize) {
        const std::uint64_t got = std::read(file, gCopy + size, kLogSize - size);
        if (failed(got) || got == 0) {
            break;
        }
        size += got;
    }
    std::close(file);
    if (size > 0) {
        write_file(kPreviousPath, gCopy, size);
    }
}
bool write_all(std::Handle file, const char* data, std::uint64_t size) {
    std::uint64_t done = 0;
    while (done < size) {
        const std::uint64_t written = std::write(file, data + done, size - done);
        if (failed(written) || written == 0) {
            return false;
        }
        done += written;
    }
    return true;
}

// Where the lines new since `previous` (what the ring held last time) start
// in `current`: after the last place previous's final bytes appear. Once the
// ring is full its start moves, so offsets alone do not say. -1 when they are
// gone (more than the whole ring came in between).
std::int64_t new_lines_start(const char* current, std::uint64_t size, const char* previous, std::uint64_t previousSize) {
    if (previousSize == 0) {
        return 0;
    }
    const std::uint64_t anchor = previousSize < 64 ? previousSize : 64;
    const char* tail = previous + previousSize - anchor;
    for (std::uint64_t at = size >= anchor ? size - anchor + 1 : 0; at-- > 0;) {
        if (std::memcmp(current + at, tail, anchor) == 0) {
            return static_cast<std::int64_t>(at + anchor);
        }
    }
    return -1;
}
}  // namespace

int main(int, char**) {
    keep_previous();
    const std::Handle file = std::open(kLogPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file == fail) {
        write_str("[log-saver] cannot open /kernel.log\n");
        return 1;
    }
    std::uint64_t saved = 0;     // bytes of gCopy (the ring as last written)
    std::uint64_t failures = 0;
    for (std::uint64_t round = 0;; ++round) {
        // A heartbeat in the log: after a hang its last one says when the
        // system (or at least this process) stopped running.
        if (round % kHeartbeatRounds == 0) {
            char line[64] = "[log-saver] alive uptime_ms=";
            char digits[24];
            std::uint64_t value = std::gettime();
            int n = 0;
            do {
                digits[n++] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value > 0);
            std::size_t at = std::strlen(line);
            while (n > 0) {
                line[at++] = digits[--n];
            }
            line[at++] = '\n';
            line[at] = '\0';
            write_str(line);
        }
        const std::uint64_t size = std::kernel_log(gLog, kLogSize);
        if (!failed(size) && size > 0) {
            std::int64_t start = new_lines_start(gLog, size, gCopy, saved);
            bool ok = true;
            if (start < 0) {
                static const char kGap[] = "\n[log-saver] ... lines lost here ...\n";
                ok = write_all(file, kGap, sizeof(kGap) - 1);
                start = 0;
            }
            if (ok && static_cast<std::uint64_t>(start) < size) {
                ok = write_all(file, gLog + start, size - static_cast<std::uint64_t>(start));
            }
            if (ok) {
                std::memcpy(gCopy, gLog, size);
                saved = size;
            } else if (++failures <= 3) {
                write_str("[log-saver] cannot write /kernel.log, will retry\n");
            }
        }
        std::sleep(kIntervalMs);
    }
}
