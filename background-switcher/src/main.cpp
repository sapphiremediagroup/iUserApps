#include <cstdint.hpp>
#include <cstring.hpp>
#include <fcntl.h>
#include <instant/font.hpp>
#include <instant/window.hpp>
#include <new.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>
#ifdef NULL
#undef NULL
#endif
#define NULL 0

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr std::uint32_t kSurfaceWidth = 720;
constexpr std::uint32_t kSurfaceHeight = 460;
constexpr std::uint32_t kFontPixelHeight = 15;
constexpr std::uint32_t kFrameIntervalMs = 33;
constexpr std::uint64_t kApplyCooldownMs = 250;
constexpr std::uint32_t kMaxEntries = 32;
constexpr int kPaddingX = 0;
constexpr int kPaddingY = 16;
constexpr int kHeaderHeight = 0;
constexpr int kFooterHeight = 0;
constexpr int kRowHeight = 34;
constexpr char kBackgroundDirectory[] = "/bin/backgrounds";

constexpr std::uint32_t kColorBackgroundTop = 0x00131a25U;
constexpr std::uint32_t kColorBackgroundBottom = 0x001d2837U;
constexpr std::uint32_t kColorHeader = 0x00233446U;
constexpr std::uint32_t kColorPanel = 0x00141d2dU;
constexpr std::uint32_t kColorPanelSoft = 0x001b2739U;
constexpr std::uint32_t kColorSelected = 0x00398dd0U;
constexpr std::uint32_t kColorText = 0x00dff1eeU;
constexpr std::uint32_t kColorDim = 0x007f9fabU;
constexpr std::uint32_t kColorSuccess = 0x0087d89fU;
constexpr std::uint32_t kColorError = 0x00ec8d95U;

struct BackgroundEntry {
    char name[128];
    char path[192];
};

struct AppState {
    BackgroundEntry entries[kMaxEntries];
    std::uint32_t entryCount;
    std::uint32_t selected;
    std::uint32_t scroll;
    bool focused;
    bool running;
    std::uint64_t lastApplyMs;
    char status[160];
    std::uint32_t statusColor;
};

void write_str(const char* s) {
    std::serial_write(s, std::strlen(s));
}

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

std::uint8_t color_r(std::uint32_t color) {
    return static_cast<std::uint8_t>((color >> 16) & 0xFFU);
}

std::uint8_t color_g(std::uint32_t color) {
    return static_cast<std::uint8_t>((color >> 8) & 0xFFU);
}

std::uint8_t color_b(std::uint32_t color) {
    return static_cast<std::uint8_t>(color & 0xFFU);
}

std::uint32_t pack_rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(g) << 8) |
           static_cast<std::uint32_t>(b);
}

void fill_rect(
    std::uint32_t* pixels,
    std::uint32_t surfaceWidth,
    std::uint32_t surfaceHeight,
    int x,
    int y,
    int width,
    int height,
    std::uint32_t color
) {
    if (!pixels || width <= 0 || height <= 0) {
        return;
    }

    int startX = x < 0 ? 0 : x;
    int startY = y < 0 ? 0 : y;
    int endX = x + width;
    int endY = y + height;
    if (endX > static_cast<int>(surfaceWidth)) {
        endX = static_cast<int>(surfaceWidth);
    }
    if (endY > static_cast<int>(surfaceHeight)) {
        endY = static_cast<int>(surfaceHeight);
    }

    for (int drawY = startY; drawY < endY; ++drawY) {
        for (int drawX = startX; drawX < endX; ++drawX) {
            pixels[drawY * surfaceWidth + drawX] = color;
        }
    }
}

void draw_gradient(std::uint32_t* pixels) {
    for (std::uint32_t y = 0; y < kSurfaceHeight; ++y) {
        const std::uint32_t mix = (y * 255U) / (kSurfaceHeight - 1U);
        const std::uint32_t red = ((color_r(kColorBackgroundTop) * (255U - mix)) +
                                   (color_r(kColorBackgroundBottom) * mix)) / 255U;
        const std::uint32_t green = ((color_g(kColorBackgroundTop) * (255U - mix)) +
                                     (color_g(kColorBackgroundBottom) * mix)) / 255U;
        const std::uint32_t blue = ((color_b(kColorBackgroundTop) * (255U - mix)) +
                                    (color_b(kColorBackgroundBottom) * mix)) / 255U;
        const std::uint32_t rowColor = pack_rgb(
            static_cast<std::uint8_t>(red),
            static_cast<std::uint8_t>(green),
            static_cast<std::uint8_t>(blue)
        );
        for (std::uint32_t x = 0; x < kSurfaceWidth; ++x) {
            pixels[y * kSurfaceWidth + x] = rowColor;
        }
    }
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
    if (!buffer || capacity == 0 || !text) {
        return;
    }

    std::size_t len = std::strlen(buffer);
    std::size_t index = 0;
    while (text[index] != '\0' && len + 1 < capacity) {
        buffer[len++] = text[index++];
    }
    buffer[len] = '\0';
}

void set_status(AppState* state, const char* text, std::uint32_t color) {
    if (!state) {
        return;
    }

    std::strncpy(state->status, text ? text : "", sizeof(state->status) - 1);
    state->status[sizeof(state->status) - 1] = '\0';
    state->statusColor = color;
}

void sort_entries(AppState* state) {
    if (!state) {
        return;
    }

    for (std::uint32_t i = 0; i < state->entryCount; ++i) {
        for (std::uint32_t j = i + 1; j < state->entryCount; ++j) {
            if (std::strcmp(state->entries[j].name, state->entries[i].name) < 0) {
                BackgroundEntry temp = state->entries[i];
                state->entries[i] = state->entries[j];
                state->entries[j] = temp;
            }
        }
    }
}

bool load_backgrounds(AppState* state) {
    if (!state) {
        return false;
    }

    state->entryCount = 0;
    std::DirEntry entries[kMaxEntries] = {};
    const std::uint64_t found = std::readdir(kBackgroundDirectory, entries, kMaxEntries);
    if (found == fail) {
        set_status(state, "Failed to read /bin/backgrounds", kColorError);
        return false;
    }

    for (std::uint64_t index = 0; index < found && state->entryCount < kMaxEntries; ++index) {
        if (entries[index].type != std::FileType::Regular || !has_png_suffix(entries[index].name)) {
            continue;
        }

        BackgroundEntry& entry = state->entries[state->entryCount++];
        std::strncpy(entry.name, entries[index].name, sizeof(entry.name) - 1);
        entry.name[sizeof(entry.name) - 1] = '\0';
        entry.path[0] = '\0';
        append_text(entry.path, sizeof(entry.path), kBackgroundDirectory);
        append_text(entry.path, sizeof(entry.path), "/");
        append_text(entry.path, sizeof(entry.path), entries[index].name);
    }

    sort_entries(state);
    if (state->entryCount == 0) {
        set_status(state, "No PNG files found in /bin/backgrounds", kColorError);
        return false;
    }

    state->selected = 0;
    state->scroll = 0;
    set_status(state, "Enter applies the selected background", kColorDim);
    return true;
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

void ensure_visible(AppState* state, std::uint32_t visibleRows) {
    if (!state || visibleRows == 0) {
        return;
    }

    if (state->selected < state->scroll) {
        state->scroll = state->selected;
    } else if (state->selected >= state->scroll + visibleRows) {
        state->scroll = state->selected - visibleRows + 1;
    }
}

void draw_ui(std::uint32_t* pixels, instant::UIFont& font, const AppState& state) {
    draw_gradient(pixels);

    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 0, kHeaderHeight, kSurfaceWidth, kSurfaceHeight - kHeaderHeight - kFooterHeight, kColorPanel);
    
    const int listTop = kHeaderHeight + 12;
    const std::uint32_t visibleRows = static_cast<std::uint32_t>((kSurfaceHeight - kHeaderHeight - kFooterHeight - 24) / kRowHeight);

    if (state.entryCount == 0) {
        instant::draw_text(pixels, kSurfaceWidth, kSurfaceHeight, font, kPaddingX, listTop + font.baseline, "(no backgrounds)", kColorDim);
    }

    for (std::uint32_t row = 0; row < visibleRows; ++row) {
        const std::uint32_t index = state.scroll + row;
        if (index >= state.entryCount) {
            break;
        }

        const int rowY = listTop + static_cast<int>(row) * kRowHeight;
        const bool selected = index == state.selected;
        fill_rect(
            pixels,
            kSurfaceWidth,
            kSurfaceHeight,
            kPaddingX - 6,
            rowY,
            static_cast<int>(kSurfaceWidth) - (kPaddingX * 2) + 12,
            kRowHeight - 4,
            selected ? kColorSelected : kColorPanelSoft
        );
        instant::draw_text(
            pixels,
            kSurfaceWidth,
            kSurfaceHeight,
            font,
            kPaddingX + 8,
            rowY + font.baseline + 6,
            state.entries[index].name,
            kColorText
        );
    }
}

class BackgroundSwitcherWindow : public instant::Window {
private:
    instant::WindowConfig configure() override {
        instant::WindowConfig config = {};
        config.width = static_cast<int>(kSurfaceWidth);
        config.height = static_cast<int>(kSurfaceHeight);
        config.title = "Background Switcher";
        config.frameIntervalMs = kFrameIntervalMs;
        return config;
    }

    Result<bool, std::string> init() override {
        if (!instant::initialize_ui_font(kFontPixelHeight)) {
            return Result<bool, std::string>::error("font init failed");
        }

        state_ = {};
        state_.running = true;
        state_.statusColor = kColorDim;
        load_backgrounds(&state_);
        return true;
    }

    Result<bool, std::string> update() override {
        const std::uint32_t visibleRows = static_cast<std::uint32_t>((kSurfaceHeight - kHeaderHeight - kFooterHeight - 24) / kRowHeight);
        ensure_visible(&state_, visibleRows == 0 ? 1 : visibleRows);
        draw_ui(pixels(), instant::gUIFont, state_);
        return true;
    }

    Result<bool, std::string> event(const std::Event& event) override {
        if (event.type == std::EventType::Window) {
            if (event.window.action == std::WindowEventAction::FocusGained) {
                state_.focused = true;
            } else if (event.window.action == std::WindowEventAction::FocusLost) {
                state_.focused = false;
            } else if (event.window.action == std::WindowEventAction::CloseRequested) {
                state_.running = false;
                return false;
            }
        } else if (event.type == std::EventType::Key && event.key.action == std::KeyEventAction::Press) {
            const char key = event.key.text[0] != '\0' ? event.key.text[0] : static_cast<char>(event.key.keycode);
            if (event.key.keycode == 27) {
                state_.running = false;
                close();
                return false;
            } else if (event.key.keycode == '\n' || event.key.keycode == '\r' || key == ' ') {
                const std::uint64_t now = std::gettime();
                if (now - state_.lastApplyMs >= kApplyCooldownMs) {
                    state_.lastApplyMs = now;
                    if (state_.entryCount != 0 && apply_background(state_.entries[state_.selected].path)) {
                        set_status(&state_, "Background updated", kColorSuccess);
                    } else {
                        set_status(&state_, "Failed to update background", kColorError);
                    }
                }
            } else if (event.key.keycode == 'k' || event.key.keycode == 'K' || event.key.keycode == 'w' || event.key.keycode == 'W') {
                if (state_.selected > 0) {
                    state_.selected--;
                }
            } else if (event.key.keycode == 'j' || event.key.keycode == 'J' || event.key.keycode == 's' || event.key.keycode == 'S') {
                if (state_.selected + 1 < state_.entryCount) {
                    state_.selected++;
                }
            }
        }

        return state_.running;
    }

    Result<bool, std::string> event() override {
        return true;
    }

    void cleanup() override {
        instant::destroy_ui_font();
    }

    AppState state_ = {};
};
}

INSTANT_WINDOW_APP(BackgroundSwitcherWindow)
