// reboot / poweroff: restarts or powers off the machine (root only). One
// binary, installed under both names; `reboot -p` also powers off. It waits a
// moment first so log-saver can put the last kernel log lines on the disk,
// then the kernel flushes the drives and resets.

#include <cstdint.hpp>
#include <cstring.hpp>
#include <syscall.hpp>

namespace {

void out(const char* text) {
    std::write(1, text, std::strlen(text));
}

const char* base_name(const char* path) {
    const char* name = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    return name;
}

} // namespace

int main(int argc, char** argv) {
    bool powerOff = argc > 0 && std::strcmp(base_name(argv[0]), "poweroff") == 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-p") == 0) {
            powerOff = true;
        } else {
            out("usage: reboot [-p]   (or poweroff)\n");
            return 2;
        }
    }

    out(powerOff ? "Powering off...\n" : "Restarting...\n");
    std::sleep(600);   // log-saver appends every 250 ms
    const std::uint64_t result = std::reboot(powerOff ? std::RebootMode::PowerOff : std::RebootMode::Restart);
    if (static_cast<std::int64_t>(result) == -1) {
        out("reboot: permission denied (run it as root)\n");
    } else {
        out("reboot: the machine did not restart\n");
    }
    return 1;
}
