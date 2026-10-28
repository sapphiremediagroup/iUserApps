#include <cstdint.hpp>
#include <cstring.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>
#include <ui/window.hpp>

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr std::uint64_t kApplyCooldownMs = 250;
constexpr std::uint32_t kMaxEntries = 32;
constexpr int kListTop = 12;
constexpr int kRowHeight = 34;
constexpr int kStatusHeight = 28;
constexpr char kBackgroundDirectory[] = "/bin/backgrounds";

constexpr ui::Color kColorPanel = ui::rgb(0x141d2d);
constexpr ui::Color kColorPanelSoft = ui::rgb(0x1b2739);
constexpr ui::Color kColorSelected = ui::rgb(0x398dd0);
constexpr ui::Color kColorText = ui::rgb(0xdff1ee);
constexpr ui::Color kColorDim = ui::rgb(0x7f9fab);
constexpr ui::Color kColorSuccess = ui::rgb(0x87d89f);
constexpr ui::Color kColorError = ui::rgb(0xec8d95);

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
    BackgroundEntry entries_[kMaxEntries] = {};
    std::uint32_t entryCount_ = 0;
    std::uint32_t selected_ = 0;
    std::uint32_t scroll_ = 0;
    std::uint64_t lastApplyMs_ = 0;
    char status_[160] = {};
    ui::Color statusColor_ = kColorDim;

    void set_status(const char* text, ui::Color color) {
        std::strncpy(status_, text, sizeof(status_) - 1);
        status_[sizeof(status_) - 1] = '\0';
        statusColor_ = color;
        invalidate();
    }

    std::uint32_t visible_rows() const {
        const int rows = (client_size().height - kListTop - kStatusHeight) / kRowHeight;
        return rows > 0 ? static_cast<std::uint32_t>(rows) : 1;
    }

    void load_backgrounds() {
        entryCount_ = 0;
        std::DirEntry found[kMaxEntries] = {};
        const std::uint64_t count = std::readdir(kBackgroundDirectory, found, kMaxEntries);
        if (count == fail) {
            set_status("Failed to read /bin/backgrounds", kColorError);
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
        if (entryCount_ == 0) {
            set_status("No PNG files found in /bin/backgrounds", kColorError);
            return;
        }
        set_status("Enter or a double click applies the selected background", kColorDim);
    }

    void select(std::uint32_t index) {
        if (entryCount_ == 0) {
            return;
        }
        selected_ = index < entryCount_ ? index : entryCount_ - 1;
        if (selected_ < scroll_) {
            scroll_ = selected_;
        } else if (selected_ >= scroll_ + visible_rows()) {
            scroll_ = selected_ - visible_rows() + 1;
        }
        invalidate();
    }

    void apply() {
        const std::uint64_t now = std::gettime();
        if (entryCount_ == 0 || now - lastApplyMs_ < kApplyCooldownMs) {
            return;
        }
        lastApplyMs_ = now;
        if (apply_background(entries_[selected_].path)) {
            set_status("Background updated", kColorSuccess);
        } else {
            set_status("Failed to update background", kColorError);
        }
    }

    // The entry under a point in the content, or -1.
    int row_at(int y) const {
        if (y < kListTop) {
            return -1;
        }
        const std::uint32_t row = static_cast<std::uint32_t>((y - kListTop) / kRowHeight);
        const std::uint32_t index = scroll_ + row;
        return row < visible_rows() && index < entryCount_ ? static_cast<int>(index) : -1;
    }

    void on_create() override {
        load_backgrounds();
    }

    void on_size(ui::Size) override {
        select(selected_);   // keep it in view
    }

    void on_paint(ui::Canvas& canvas, const ui::Rect&) override {
        const ui::Size size = client_size();
        const ui::Font& font = ui::Font::ui();
        canvas.fill({ 0, 0, size.width, size.height }, kColorPanel);
        if (entryCount_ == 0) {
            canvas.text({ 8, kListTop, size.width - 16, kRowHeight }, "(no backgrounds)", font, kColorDim);
        }
        for (std::uint32_t row = 0; row < visible_rows(); ++row) {
            const std::uint32_t index = scroll_ + row;
            if (index >= entryCount_) {
                break;
            }
            const ui::Rect box = { 0, kListTop + static_cast<int>(row) * kRowHeight, size.width, kRowHeight - 4 };
            canvas.fill(box, index == selected_ ? kColorSelected : kColorPanelSoft);
            canvas.text({ box.x + 8, box.y, box.width - 16, box.height }, entries_[index].name, font, kColorText);
        }
        canvas.text({ 8, size.height - kStatusHeight, size.width - 16, kStatusHeight }, status_, font, statusColor_);
    }

    bool on_key_down(const ui::KeyEvent& key) override {
        if (key.ctrl() || key.alt()) {
            return false;
        }
        switch (key.key) {
        case std::KeyEscape:
            close();
            return true;
        case std::KeyEnter:
        case std::KeypadEnter:
        case std::KeySpace:
            apply();
            return true;
        case std::KeyUp:
        case std::KeyK:
        case std::KeyW:
            if (selected_ > 0) {
                select(selected_ - 1);
            }
            return true;
        case std::KeyDown:
        case std::KeyJ:
        case std::KeyS:
            select(selected_ + 1);
            return true;
        case std::KeyHome:
            select(0);
            return true;
        case std::KeyEnd:
            select(entryCount_ > 0 ? entryCount_ - 1 : 0);
            return true;
        default:
            return false;
        }
    }

    void on_mouse_down(const ui::MouseEvent& mouse) override {
        const int index = row_at(mouse.y);
        if (mouse.button != ui::MouseLeft || index < 0) {
            return;
        }
        select(static_cast<std::uint32_t>(index));
        if (mouse.clicks == 2) {
            apply();
        }
    }

    void on_wheel(const ui::MouseEvent&, int delta) override {
        const std::uint32_t rows = visible_rows();
        const std::uint32_t maxScroll = entryCount_ > rows ? entryCount_ - rows : 0;
        if (delta > 0 && scroll_ > 0) {
            --scroll_;
        } else if (delta < 0 && scroll_ < maxScroll) {
            ++scroll_;
        }
        invalidate();
    }
};

}  // namespace

int main(int, char**) {
    return ui::run<BackgroundSwitcher>({ .width = 720, .height = 460, .title = "Background Switcher" });
}
