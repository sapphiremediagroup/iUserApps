#include <cstdint.hpp>
#include <cstring.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>
#include <ui/ui.hpp>

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr std::uint64_t kApplyCooldownMs = 250;
constexpr std::uint32_t kMaxEntries = 32;
constexpr char kBackgroundDirectory[] = "/bin/backgrounds";


struct BackgroundEntry {
    char name[128];
    char path[192];
};

std::Handle connect_service(const char* name) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        const std::Handle handle = std::service_connect(name);
        if (handle != fail) {
            return handle;
        }
        std::yield();
    }
    return fail;
}

bool has_png_suffix(const char* name) {
    const std::size_t length = std::strlen(name);
    if (length < 4) {
        return false;
    }
    const char* suffix = name + length - 4;
    return (suffix[0] == '.' &&
            (suffix[1] == 'p' || suffix[1] == 'P') &&
            (suffix[2] == 'n' || suffix[2] == 'N') &&
            (suffix[3] == 'g' || suffix[3] == 'G'));
}

void append_text(char* buffer, std::size_t capacity, const char* text) {
    std::size_t len = std::strlen(buffer);
    std::size_t index = 0;
    while (text[index] != '\0' && len + 1 < capacity) {
        buffer[len++] = text[index++];
    }
    buffer[len] = '\0';
}

bool apply_background(const char* path) {
    if (!path || path[0] == '\0') {
        return false;
    }

    const std::Handle compositor = connect_service(std::services::graphics_compositor::NAME);
    if (compositor == fail) {
        return false;
    }

    std::services::graphics_compositor::SetBackgroundRequest request = {};
    request.header.version = std::services::graphics_compositor::VERSION;
    request.header.opcode = static_cast<std::uint16_t>(std::services::graphics_compositor::Opcode::SetBackground);
    std::strncpy(request.path, path, sizeof(request.path) - 1);
    request.path[sizeof(request.path) - 1] = '\0';

    std::IPCMessage message = {};
    if (!std::services::encode_request(&message, request)) {
        std::close(compositor);
        return false;
    }

    std::services::graphics_compositor::SetBackgroundReply reply = {};
    std::uint64_t responseSize = 0;
    const std::uint64_t result = std::queue_request(compositor, &message, &reply, sizeof(reply), &responseSize);
    std::close(compositor);
    if (result == fail || responseSize < sizeof(reply)) {
        return false;
    }
    return reply.status == std::services::STATUS_OK;
}

class BackgroundSwitcher : public ui::Window {
    ui::Panel statusBar_{this};
    ui::Label status_{&statusBar_};
    ui::ListBox list_{this};

    BackgroundEntry entries_[kMaxEntries] = {};
    std::uint32_t entryCount_ = 0;
    std::uint64_t lastApplyMs_ = 0;

    void set_status(const char* text, ui::Color color) {
        status_.set_text(text).set_color(color);
    }

    void load_backgrounds() {
        const ui::Theme& t = ui::theme();
        entryCount_ = 0;
        std::DirEntry found[kMaxEntries] = {};
        const std::uint64_t count = std::readdir(kBackgroundDirectory, found, kMaxEntries);
        if (count == fail) {
            set_status("Failed to read /bin/backgrounds", t.danger);
            return;
        }
        for (std::uint64_t index = 0; index < count && entryCount_ < kMaxEntries; ++index) {
            if (found[index].type != std::FileType::Regular || !has_png_suffix(found[index].name)) {
                continue;
            }
            BackgroundEntry& entry = entries_[entryCount_++];
            std::strncpy(entry.name, found[index].name, sizeof(entry.name) - 1);
            entry.path[0] = '\0';
            append_text(entry.path, sizeof(entry.path), kBackgroundDirectory);
            append_text(entry.path, sizeof(entry.path), "/");
            append_text(entry.path, sizeof(entry.path), found[index].name);
        }
        for (std::uint32_t i = 0; i < entryCount_; ++i) {
            for (std::uint32_t j = i + 1; j < entryCount_; ++j) {
                if (std::strcmp(entries_[j].name, entries_[i].name) < 0) {
                    const BackgroundEntry temp = entries_[i];
                    entries_[i] = entries_[j];
                    entries_[j] = temp;
                }
            }
        }
        list_.set_count(static_cast<int>(entryCount_));
        list_.set_selected(entryCount_ > 0 ? 0 : -1);
        if (entryCount_ == 0) {
            set_status("No PNG files found in /bin/backgrounds", t.danger);
            return;
        }
        set_status("Enter or a double click applies the selected background", t.textDim);
    }

    void apply() {
        const ui::Theme& t = ui::theme();
        const int selected = list_.selected();
        const std::uint64_t now = std::gettime();
        if (selected < 0 || now - lastApplyMs_ < kApplyCooldownMs) {
            return;
        }
        lastApplyMs_ = now;
        if (apply_background(entries_[selected].path)) {
            set_status("Background updated", t.success);
        } else {
            set_status("Failed to update background", t.danger);
        }
    }

    void on_create() override {
        const ui::Theme& t = ui::theme();
        const ui::Size size = client_size();
        statusBar_.set_bounds({ 0, 0, size.width, t.statusHeight }).set_dock(ui::Dock::Bottom);
        status_.set_bounds({ 2 * t.padding, 0, size.width - 4 * t.padding, t.statusHeight })
            .set_anchor(ui::AnchorLeft | ui::AnchorTop | ui::AnchorRight);
        list_.set_dock(ui::Dock::Fill);
        list_.row_text = [this](int row) -> const char* { return entries_[row].name; };
        list_.on_activate = [this](int) { apply(); };
        load_backgrounds();
        list_.focus();
    }

    void on_paint(ui::Canvas& canvas, const ui::Rect& dirty) override {
        const ui::Theme& t = ui::theme();
        canvas.fill(dirty, t.window);
        const ui::Size size = client_size();
        canvas.fill({ 0, size.height - t.statusHeight, size.width, 1 }, t.divider);
    }

    bool on_key_down(const ui::KeyEvent& key) override {
        if (key.ctrl() || key.alt()) {
            return false;
        }
        if (key.key == std::KeyEscape) {
            close();
            return true;
        }
        if (key.key == std::KeySpace) {
            apply();
            return true;
        }
        return false;
    }
};

}  // namespace

int main(int, char**) {
    return ui::run<BackgroundSwitcher>({ .width = 720, .height = 460, .title = "Background Switcher" });
}
