#include <cstdlib.hpp>
#include <cstring.hpp>
#include <new.hpp>
#include <service_protocol.hpp>

static void writeStr(const char* s) {
    std::write(1, s, std::strlen(s));
}

static bool connectInputManager(std::Handle* outHandle) {
    if (outHandle == nullptr) {
        return false;
    }

    for (int attempt = 0; attempt < 50; ++attempt) {
        const std::Handle handle = std::service_connect(std::services::input_manager::NAME);
        if (handle != static_cast<std::Handle>(-1)) {
            *outHandle = handle;
            return true;
        }
        std::sleep(10);
    }

    return false;
}

static bool waitForService(const char* name, int attempts, int delayMs) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const std::Handle handle = std::service_connect(name);
        if (handle != static_cast<std::Handle>(-1)) {
            std::close(handle);
            return true;
        }

        if (delayMs > 0) {
            std::sleep(static_cast<std::uint64_t>(delayMs));
        }
    }

    return false;
}

static bool ensureGraphicsCompositor() {
    if (waitForService(std::services::graphics_compositor::NAME, 1, 0)) {
        return true;
    }

    if (std::spawn("/bin/graphics-compositor.exe") == static_cast<std::uint64_t>(-1)) {
        writeStr("Failed to start graphics-compositor.exe.\n");
        return false;
    }

    if (!waitForService(std::services::graphics_compositor::NAME, 100, 10)) {
        writeStr("Timed out waiting for graphics.compositor.\n");
        return false;
    }

    return true;
}

static char eventChar(const std::Event& event) {
    if (event.type != std::EventType::Key || event.key.action != std::KeyEventAction::Press) {
        return 0;
    }

    if (event.key.keycode == '\r') {
        return '\n';
    }

    if (event.key.text[0] != '\0') {
        return event.key.text[0];
    }

    if (event.key.keycode == '\n' || event.key.keycode == '\b') {
        return static_cast<char>(event.key.keycode);
    }

    return 0;
}

static int readLine(std::Handle inputQueue, char* buf, int maxLen, bool maskInput) {
    int n = 0;
    while (n < maxLen - 1) {
        std::Event event = {};
        if (std::event_wait(inputQueue, &event) != 0) {
            break;
        }

        const char c = eventChar(event);
        if (c == 0) {
            continue;
        }

        if (c == '\n' || c == '\r') break;
        if (c == '\b') {
            if (n > 0) {
                --n;
                writeStr("\b \b");
            }
            continue;
        }

        buf[n++] = c;
        if (maskInput) {
            writeStr("*");
        } else {
            char out[2] = { c, '\0' };
            writeStr(out);
        }
    }
    buf[n] = '\0';
    writeStr("\n");
    return n;
}

int main() {
    std::Handle inputQueue = static_cast<std::Handle>(-1);
    if (!connectInputManager(&inputQueue)) {
        writeStr("Failed to connect to input.manager.\n");
        return 1;
    }

    while (true) {
        char username[32];
        char password[64];
        std::memset(username, 0, sizeof(username));
        std::memset(password, 0, sizeof(password));

        writeStr("login: ");
        readLine(inputQueue, username, sizeof(username), false);

        writeStr("password: ");
        readLine(inputQueue, password, sizeof(password), true);

        std::LoginInfo info;
        std::memset(&info, 0, sizeof(info));
        std::memcpy(info.username, username, sizeof(info.username));
        std::memcpy(info.password, password, sizeof(info.password));

        std::uint64_t result = std::login(info);
        if (result != static_cast<std::uint64_t>(-1)) {
            writeStr("Login successful.\n");
            if (ensureGraphicsCompositor()) {
                if (std::spawn("/bin/desktop-shell.exe") == static_cast<std::uint64_t>(-1)) {
                    writeStr("Failed to start desktop-shell.exe.\n");
                }
            }
            std::yield();
            std::close(inputQueue);
            std::exit(0);
        } else {
            writeStr("Login failed.\n");
        }
    }
}
