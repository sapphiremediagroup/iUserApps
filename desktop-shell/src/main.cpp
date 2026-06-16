#include <cstdio.hpp>
#include <cstring.hpp>
#include <fcntl.h>
#include <instant/window.hpp>
#include <math.h>
#include <new.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>

#ifdef NULL
#undef NULL
#endif
#define NULL 0

#define STBI_ASSERT(x) ((void)0)
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr std::uint32_t kSurfaceWidth = 900;
constexpr std::uint32_t kSurfaceHeight = 620;
constexpr std::uint32_t kAnimationTickMs = 120;
constexpr char kBackgroundDirectory[] = "/bin/backgrounds";
constexpr char kDefaultBackground[] = "/bin/backgrounds/instantos_2.png";

constexpr std::uint32_t kColorBackgroundTop = 0x00141b2dU;
constexpr std::uint32_t kColorBackgroundBottom = 0x001e3048U;
constexpr std::uint32_t kColorPanel = 0x0026394dU;
constexpr std::uint32_t kColorAccent = 0x00e39d3fU;
constexpr std::uint32_t kColorAccentSoft = 0x004d86b8U;
constexpr std::uint32_t kColorTileA = 0x00344f71U;
constexpr std::uint32_t kColorTileB = 0x00273c59U;
constexpr int kTaskbarY = static_cast<int>(kSurfaceHeight) - 58;
constexpr int kTaskbarButtonSize = 48;
constexpr int kTaskbarButtonGap = 72;
constexpr int kLauncherMaxEntries = 12;

struct LauncherEntry {
    char name[64];
    char path[96];
};

struct DesktopState {
    std::uint32_t* wallpaper;
    bool wallpaperReady;
    bool focused;
    bool launcherOpen;
    bool terminalStarted;
    std::uint32_t activitySeed;
    char currentBackground[192];
    LauncherEntry launcherEntries[kLauncherMaxEntries];
    std::uint32_t launcherEntryCount;
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

bool launch_file_browser() {
    return std::spawn("/bin/file-browser") != fail;
}

bool launch_background_switcher() {
    return std::spawn("/bin/background-switcher") != fail;
}

bool launch_terminal() {
    return std::spawn("/bin/terminal") != fail;
}

bool launch_cube() {
    return std::spawn("/bin/cube") != fail;
}

bool launch_path(const char* path) {
    return path && path[0] != '\0' && std::spawn(path) != fail;
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

std::uint32_t blend_rgb(std::uint32_t dst, std::uint32_t src, std::uint8_t alpha) {
    const std::uint32_t inv = 255U - alpha;
    const std::uint32_t r = (color_r(dst) * inv + color_r(src) * alpha) / 255U;
    const std::uint32_t g = (color_g(dst) * inv + color_g(src) * alpha) / 255U;
    const std::uint32_t b = (color_b(dst) * inv + color_b(src) * alpha) / 255U;
    return pack_rgb(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b));
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

    std::size_t length = std::strlen(buffer);
    std::size_t index = 0;
    while (text[index] != '\0' && length + 1 < capacity) {
        buffer[length++] = text[index++];
    }
    buffer[length] = '\0';
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
    if (endX > static_cast<int>(surfaceWidth)) endX = static_cast<int>(surfaceWidth);
    if (endY > static_cast<int>(surfaceHeight)) endY = static_cast<int>(surfaceHeight);

    for (int drawY = startY; drawY < endY; ++drawY) {
        for (int drawX = startX; drawX < endX; ++drawX) {
            pixels[drawY * surfaceWidth + drawX] = color;
        }
    }
}

std::uint8_t glyph_rows(char c, int row) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    static constexpr std::uint8_t digits[10][7] = {
        {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
        {14, 17, 1, 2, 4, 8, 31}, {30, 1, 1, 14, 1, 1, 30},
        {2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14},
        {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
        {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12},
    };
    static constexpr std::uint8_t letters[26][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14},
        {7,2,2,2,18,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4}, {17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
    };

    if (c >= '0' && c <= '9') return digits[c - '0'][row];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'][row];
    if (c == '-') return row == 3 ? 31 : 0;
    if (c == '.') return row == 6 ? 4 : 0;
    if (c == '/') return static_cast<std::uint8_t>(1U << (6 - row));
    return 0;
}

void draw_text(std::uint32_t* pixels, int x, int y, const char* text, std::uint32_t color) {
    if (!pixels || !text) return;
    int cursor = x;
    for (std::size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] == ' ') {
            cursor += 6;
            continue;
        }
        for (int row = 0; row < 7; ++row) {
            const std::uint8_t bits = glyph_rows(text[i], row);
            for (int col = 0; col < 5; ++col) {
                if ((bits & (1U << (4 - col))) != 0) {
                    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, cursor + col * 2, y + row * 2, 2, 2, color);
                }
            }
        }
        cursor += 12;
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

void scale_image_to_wallpaper(
    std::uint32_t* wallpaper,
    int imageWidth,
    int imageHeight,
    const unsigned char* imagePixels
) {
    if (!wallpaper || !imagePixels || imageWidth <= 0 || imageHeight <= 0) {
        return;
    }

    draw_gradient(wallpaper);

    std::uint32_t drawWidth = kSurfaceWidth;
    std::uint32_t drawHeight = static_cast<std::uint32_t>((static_cast<std::uint64_t>(imageHeight) * kSurfaceWidth) / imageWidth);
    if (drawHeight > kSurfaceHeight) {
        drawHeight = kSurfaceHeight;
        drawWidth = static_cast<std::uint32_t>((static_cast<std::uint64_t>(imageWidth) * kSurfaceHeight) / imageHeight);
    }

    if (drawWidth == 0 || drawHeight == 0) {
        return;
    }

    const int offsetX = static_cast<int>((kSurfaceWidth - drawWidth) / 2U);
    const int offsetY = static_cast<int>((kSurfaceHeight - drawHeight) / 2U);

    for (std::uint32_t y = 0; y < drawHeight; ++y) {
        const int srcY = static_cast<int>((static_cast<std::uint64_t>(y) * imageHeight) / drawHeight);
        for (std::uint32_t x = 0; x < drawWidth; ++x) {
            const int srcX = static_cast<int>((static_cast<std::uint64_t>(x) * imageWidth) / drawWidth);
            const unsigned char* src = imagePixels + ((srcY * imageWidth + srcX) * 4);
            const std::uint32_t srcColor = pack_rgb(src[0], src[1], src[2]);
            const std::uint8_t alpha = src[3];
            std::uint32_t& dst = wallpaper[(offsetY + static_cast<int>(y)) * kSurfaceWidth + offsetX + static_cast<int>(x)];
            dst = alpha == 255 ? srcColor : blend_rgb(dst, srcColor, alpha);
        }
    }
}

bool load_background(DesktopState* state, const char* path) {
    if (!state || !path || path[0] == '\0') {
        return false;
    }

    if (!state->wallpaper) {
        state->wallpaper = new (std::nothrow) std::uint32_t[kSurfaceWidth * kSurfaceHeight];
        if (!state->wallpaper) {
            return false;
        }
    }

    std::Stat stat = {};
    if (std::stat(path, &stat) == fail || stat.st_size == 0) {
        draw_gradient(state->wallpaper);
        state->wallpaperReady = false;
        return false;
    }

    const std::Handle file = std::open(path, O_RDONLY);
    if (file == fail) {
        draw_gradient(state->wallpaper);
        state->wallpaperReady = false;
        return false;
    }

    const std::size_t fileSize = static_cast<std::size_t>(stat.st_size);
    unsigned char* encoded = new (std::nothrow) unsigned char[fileSize];
    if (!encoded) {
        std::close(file);
        return false;
    }

    std::size_t totalRead = 0;
    while (totalRead < fileSize) {
        const std::uint64_t bytesRead = std::read(file, encoded + totalRead, fileSize - totalRead);
        if (bytesRead == fail || bytesRead == 0) {
            delete[] encoded;
            std::close(file);
            draw_gradient(state->wallpaper);
            state->wallpaperReady = false;
            return false;
        }
        totalRead += static_cast<std::size_t>(bytesRead);
    }
    std::close(file);

    int imageWidth = 0;
    int imageHeight = 0;
    int components = 0;
    unsigned char* imagePixels = stbi_load_from_memory(encoded, static_cast<int>(fileSize), &imageWidth, &imageHeight, &components, 4);
    delete[] encoded;
    if (!imagePixels || imageWidth <= 0 || imageHeight <= 0) {
        draw_gradient(state->wallpaper);
        state->wallpaperReady = false;
        return false;
    }

    scale_image_to_wallpaper(state->wallpaper, imageWidth, imageHeight, imagePixels);
    stbi_image_free(imagePixels);

    std::strncpy(state->currentBackground, path, sizeof(state->currentBackground) - 1);
    state->currentBackground[sizeof(state->currentBackground) - 1] = '\0';
    state->wallpaperReady = true;
    return true;
}

bool load_first_available_background(DesktopState* state) {
    if (load_background(state, kDefaultBackground)) {
        return true;
    }

    std::DirEntry entries[32] = {};
    const std::uint64_t found = std::readdir(kBackgroundDirectory, entries, 32);
    if (found == fail) {
        return false;
    }

    for (std::uint64_t index = 0; index < found; ++index) {
        if (entries[index].type != std::FileType::Regular || !has_png_suffix(entries[index].name)) {
            continue;
        }

        char path[192] = {};
        append_text(path, sizeof(path), kBackgroundDirectory);
        append_text(path, sizeof(path), "/");
        append_text(path, sizeof(path), entries[index].name);
        if (load_background(state, path)) {
            return true;
        }
    }

    return false;
}

bool is_launchable_bin_entry(const std::DirEntry& entry) {
    if (entry.type != std::FileType::Regular || entry.name[0] == '.' || entry.name[0] == '\0') {
        return false;
    }
    return !has_png_suffix(entry.name) && std::strcmp(entry.name, "ld-instantos.so") != 0;
}

void refresh_launcher_entries(DesktopState* state) {
    if (!state) return;
    state->launcherEntryCount = 0;

    std::DirEntry entries[64] = {};
    const std::uint64_t found = std::readdir("/bin", entries, 64);
    if (found == fail) {
        return;
    }

    for (std::uint64_t index = 0; index < found && state->launcherEntryCount < kLauncherMaxEntries; ++index) {
        if (!is_launchable_bin_entry(entries[index])) {
            continue;
        }

        LauncherEntry& launcher = state->launcherEntries[state->launcherEntryCount++];
        std::strncpy(launcher.name, entries[index].name, sizeof(launcher.name) - 1);
        launcher.name[sizeof(launcher.name) - 1] = '\0';
        std::strncpy(launcher.path, "/bin/", sizeof(launcher.path) - 1);
        launcher.path[sizeof(launcher.path) - 1] = '\0';
        append_text(launcher.path, sizeof(launcher.path), entries[index].name);
    }
}

void draw_launcher_panel(std::uint32_t* pixels, const DesktopState& state) {
    if (!state.launcherOpen) {
        return;
    }

    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 24, 268, 852, 238, 0x00152133U);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 24, 268, 852, 4, kColorAccent);
    draw_text(pixels, 46, 290, "APP LAUNCHER", 0x00f2f2f2U);

    for (std::uint32_t i = 0; i < state.launcherEntryCount; ++i) {
        const int col = static_cast<int>(i % 3U);
        const int row = static_cast<int>(i / 3U);
        const int x = 46 + col * 272;
        const int y = 324 + row * 42;
        fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, x, y, 246, 30, (i % 2U) == 0 ? kColorTileA : kColorTileB);
        draw_text(pixels, x + 12, y + 8, state.launcherEntries[i].name, 0x00ffffffU);
    }
}

bool handle_launcher_click(DesktopState* state, int x, int y) {
    if (!state || !state->launcherOpen) {
        return false;
    }

    for (std::uint32_t i = 0; i < state->launcherEntryCount; ++i) {
        const int col = static_cast<int>(i % 3U);
        const int row = static_cast<int>(i / 3U);
        const int entryX = 46 + col * 272;
        const int entryY = 324 + row * 42;
        if (x >= entryX && x < entryX + 246 && y >= entryY && y < entryY + 30) {
            if (!launch_path(state->launcherEntries[i].path)) {
                write_str("[desktop-shell] FAIL spawn launcher entry\n");
            }
            state->launcherOpen = false;
            state->activitySeed += 61U;
            return true;
        }
    }

    if (x >= 24 && x < 876 && y >= 268 && y < 506) {
        return true;
    }

    state->launcherOpen = false;
    return true;
}

bool handle_taskbar_click(DesktopState* state, int x, int y) {
    if (!state || y < kTaskbarY || y >= kTaskbarY + 34) {
        return false;
    }

    for (int i = 0; i < 5; ++i) {
        const int buttonX = 26 + (i * kTaskbarButtonGap);
        if (x < buttonX || x >= buttonX + kTaskbarButtonSize) {
            continue;
        }

        bool ok = true;
        if (i == 0) ok = launch_terminal();
        else if (i == 1) ok = launch_file_browser();
        else if (i == 2) ok = launch_background_switcher();
        else if (i == 3) ok = launch_cube();
        else if (i == 4) {
            state->launcherOpen = !state->launcherOpen;
            if (state->launcherOpen) {
                refresh_launcher_entries(state);
            }
        }

        if (!ok) {
            write_str("[desktop-shell] FAIL spawn taskbar app\n");
        }
        state->activitySeed += 47U;
        return true;
    }

    return false;
}

bool handle_pointer_press(DesktopState* state, const std::Event& event) {
    if (!state || event.pointer.action != std::PointerEventAction::Button || event.pointer.buttons == 0) {
        return false;
    }

    if (handle_launcher_click(state, event.pointer.x, event.pointer.y)) {
        return true;
    }
    return handle_taskbar_click(state, event.pointer.x, event.pointer.y);
}

void draw_desktop(std::uint32_t* pixels, const DesktopState& state) {
    if (state.wallpaper && state.wallpaperReady) {
        std::memcpy(pixels, state.wallpaper, sizeof(std::uint32_t) * kSurfaceWidth * kSurfaceHeight);
    } else {
        draw_gradient(pixels);
    }

    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 0, 0, kSurfaceWidth, 56, kColorPanel);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 0, static_cast<int>(kSurfaceHeight) - 82, kSurfaceWidth, 82, kColorPanel);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 28, 88, 364, 212, kColorTileA);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 420, 88, 452, 132, kColorTileB);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 420, 244, 214, 260, kColorTileA);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 658, 244, 214, 260, kColorTileB);

    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 28, 332, 364, 172, kColorAccentSoft);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 54, 358, 312, 18, kColorPanel);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 54, 392, 224, 18, kColorPanel);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 54, 426, 280, 18, kColorPanel);

    const int indicatorWidth = 92 + static_cast<int>(state.activitySeed % 220U);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 54, 460, indicatorWidth, 22, kColorAccent);

    const char* taskbarLabels[5] = {"TERM", "FILES", "BG", "CUBE", "APPS"};
    for (int i = 0; i < 5; ++i) {
        const int offset = 26 + (i * kTaskbarButtonGap);
        fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, offset, kTaskbarY, kTaskbarButtonSize, 34, i == 4 && state.launcherOpen ? kColorAccent : kColorTileA);
        draw_text(pixels, offset + 5, kTaskbarY + 11, taskbarLabels[i], 0x00ffffffU);
    }

    draw_launcher_panel(pixels, state);

    fill_rect(
        pixels,
        kSurfaceWidth,
        kSurfaceHeight,
        static_cast<int>(kSurfaceWidth) - 168,
        static_cast<int>(kSurfaceHeight) - 62,
        132,
        36,
        state.focused ? kColorAccent : kColorTileB
    );

    fill_rect(
        pixels,
        kSurfaceWidth,
        kSurfaceHeight,
        0,
        0,
        kSurfaceWidth,
        6,
        state.focused ? kColorAccent : kColorTileB
    );

    const int pulseX = 440 + static_cast<int>((state.activitySeed * 13U) % 168U);
    const int pulseY = 274 + static_cast<int>((state.activitySeed * 7U) % 180U);
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, pulseX, pulseY, 26, 26, kColorAccent);
}

bool handle_shell_request(std::Handle queue, const std::IPCMessage& message, DesktopState* state, bool* redraw) {
    if (!state || !redraw || (message.flags & std::IPC_MESSAGE_REQUEST) == 0) {
        return false;
    }

    std::services::MessageHeader header = {};
    if (!std::services::decode_message(message, &header)) {
        return false;
    }

    if (header.version != std::services::desktop_shell::VERSION) {
        std::services::desktop_shell::SetBackgroundReply reply = {};
        reply.header.version = std::services::desktop_shell::VERSION;
        reply.header.opcode = header.opcode;
        reply.status = std::services::STATUS_BAD_VERSION;
        return std::queue_reply(queue, message.id, &reply, sizeof(reply)) != fail;
    }

    if (header.opcode == static_cast<std::uint16_t>(std::services::desktop_shell::Opcode::Hello)) {
        std::services::desktop_shell::HelloReply reply = {};
        reply.header.version = std::services::desktop_shell::VERSION;
        reply.header.opcode = static_cast<std::uint16_t>(std::services::desktop_shell::Opcode::Hello);
        reply.status = std::services::STATUS_OK;
        std::strncpy(reply.service_name, std::services::desktop_shell::NAME, sizeof(reply.service_name) - 1);
        reply.service_name[sizeof(reply.service_name) - 1] = '\0';
        return std::queue_reply(queue, message.id, &reply, sizeof(reply)) != fail;
    }

    if (header.opcode == static_cast<std::uint16_t>(std::services::desktop_shell::Opcode::SetBackground)) {
        std::services::desktop_shell::SetBackgroundRequest request = {};
        std::services::desktop_shell::SetBackgroundReply reply = {};
        reply.header.version = std::services::desktop_shell::VERSION;
        reply.header.opcode = static_cast<std::uint16_t>(std::services::desktop_shell::Opcode::SetBackground);
        reply.status = std::services::STATUS_BAD_PAYLOAD;

        if (std::services::decode_message(message, &request) &&
            request.path[0] != '\0' &&
            load_background(state, request.path)) {
            reply.status = std::services::STATUS_OK;
            state->activitySeed += 43U;
            *redraw = true;
        }

        return std::queue_reply(queue, message.id, &reply, sizeof(reply)) != fail;
    }

    std::services::desktop_shell::SetBackgroundReply reply = {};
    reply.header.version = std::services::desktop_shell::VERSION;
    reply.header.opcode = header.opcode;
    reply.status = std::services::STATUS_BAD_OPCODE;
    return std::queue_reply(queue, message.id, &reply, sizeof(reply)) != fail;
}

class DesktopShellWindow : public instant::Window {
private:
    instant::WindowConfig configure() override {
        instant::WindowConfig config = {};
        config.width = static_cast<int>(kSurfaceWidth);
        config.height = static_cast<int>(kSurfaceHeight);
        config.title = "Desktop Shell";
        config.frameIntervalMs = kAnimationTickMs;
        return config;
    }

    Result<bool, std::string> init() override {
        serviceQueue_ = std::queue_create();
        if (serviceQueue_ == fail || std::service_register(std::services::desktop_shell::NAME, serviceQueue_) == fail) {
            if (serviceQueue_ != fail) {
                std::close(serviceQueue_);
                serviceQueue_ = fail;
            }
            return Result<bool, std::string>::error("desktop.shell service_register failed");
        }

        state_ = {};
        state_.activitySeed = 1;
        load_first_available_background(&state_);
        draw_desktop(pixels(), state_);

        write_str("[desktop-shell] ready\n");
        if (!state_.terminalStarted) {
            if (!launch_terminal()) {
                write_str("[desktop-shell] FAIL autostart terminal\n");
            }
            state_.terminalStarted = true;
        }
        return true;
    }

    Result<bool, std::string> event(const std::Event& event) override {
        if (event.type == std::EventType::Window) {
            if (event.window.action == std::WindowEventAction::FocusGained) {
                state_.focused = true;
                redraw_ = true;
            } else if (event.window.action == std::WindowEventAction::FocusLost) {
                state_.focused = false;
                redraw_ = true;
            } else if (event.window.action == std::WindowEventAction::CloseRequested) {
                return false;
            }
        } else if (event.type == std::EventType::Pointer) {
            if (handle_pointer_press(&state_, event)) {
                redraw_ = true;
            }
        } else if (event.type == std::EventType::Key && event.key.action == std::KeyEventAction::Press) {
            const bool superPressed = (event.key.modifiers & std::KeyModifierSuper) != 0;
            const char key = event.key.text[0] != '\0' ? event.key.text[0] : static_cast<char>(event.key.keycode);
            if (superPressed && (key == 't' || key == 'T')) {
                if (!launch_terminal()) {
                    write_str("[desktop-shell] FAIL spawn terminal\n");
                }
                state_.activitySeed += 29U;
                redraw_ = true;
            } else if (superPressed && (key == 'f' || key == 'F')) {
                if (!launch_file_browser()) {
                    write_str("[desktop-shell] FAIL spawn file-browser\n");
                }
                state_.activitySeed += 31U;
                redraw_ = true;
            } else if (superPressed && (key == 'b' || key == 'B')) {
                if (!launch_background_switcher()) {
                    write_str("[desktop-shell] FAIL spawn background-switcher\n");
                }
                state_.activitySeed += 37U;
                redraw_ = true;
            } else if (superPressed && (key == 'c' || key == 'C')) {
                if (!launch_cube()) {
                    write_str("[desktop-shell] FAIL spawn cube\n");
                }
                state_.activitySeed += 41U;
                redraw_ = true;
            } else if (superPressed && (key == 'a' || key == 'A')) {
                state_.launcherOpen = !state_.launcherOpen;
                if (state_.launcherOpen) {
                    refresh_launcher_entries(&state_);
                }
                state_.activitySeed += 53U;
                redraw_ = true;
            } else {
                state_.activitySeed += 9U;
                redraw_ = true;
            }
        }

        return true;
    }

    Result<bool, std::string> update() override {
        if (serviceQueue_ != fail) {
            for (;;) {
                std::IPCMessage message = {};
                if (std::queue_receive(serviceQueue_, &message, false) == fail) {
                    break;
                }

                if (!handle_shell_request(serviceQueue_, message, &state_, &redraw_)) {
                    write_str("[desktop-shell] FAIL service request handling\n");
                }
            }
        }

        state_.activitySeed += state_.focused ? 5U : 2U;
        redraw_ = true;
        if (redraw_) {
            draw_desktop(pixels(), state_);
            redraw_ = false;
        }
        return true;
    }

    Result<bool, std::string> event() override {
        return true;
    }

    void cleanup() override {
        delete[] state_.wallpaper;
        state_.wallpaper = nullptr;
        if (serviceQueue_ != fail) {
            std::close(serviceQueue_);
            serviceQueue_ = fail;
        }
    }

    DesktopState state_ = {};
    std::Handle serviceQueue_ = fail;
    bool redraw_ = false;
};
}

INSTANT_WINDOW_APP(DesktopShellWindow)
