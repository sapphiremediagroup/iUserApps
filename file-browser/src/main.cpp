#include <cstdint.hpp>
#include <cstring.hpp>
#include <fcntl.h>
#include <instant/font.hpp>
#include <instant/window.hpp>
#include <service_protocol.hpp>
#include <new.hpp>
#include <svg_renderer.hpp>
#ifdef NULL
#undef NULL
#endif
#define NULL 0
#include <syscall.hpp>

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr std::uint32_t kSurfaceWidth = 780;
constexpr std::uint32_t kSurfaceHeight = 560;
constexpr std::uint32_t kFontPixelHeight = 15;
constexpr std::uint32_t kMaxEntries = 128;
constexpr int kPaddingX = 0;
constexpr int kPaddingY = 16;
constexpr int kHeaderHeight = 0;
constexpr int kFooterHeight = 0;
constexpr int kRowGap = 6;
constexpr int kEntryIconSize = 18;
constexpr char kFolderIconPath[] = "/bin/images/folder.svg";
constexpr char kFileIconPath[] = "/bin/images/file.svg";
constexpr char kFolderIconFill[] = "#94d2ff";
constexpr char kFileIconFill[] = "#d5d5d5";

constexpr std::uint32_t kColorBackgroundTop = 0x00111924U;
constexpr std::uint32_t kColorBackgroundBottom = 0x00182634U;
constexpr std::uint32_t kColorHeader = 0x00223448U;
constexpr std::uint32_t kColorPanel = 0x00142030U;
constexpr std::uint32_t kColorPanelSoft = 0x001d2c40U;
constexpr std::uint32_t kColorSelected = 0x003484c6U;
constexpr std::uint32_t kColorSelectedMuted = 0x00284f72U;
constexpr std::uint32_t kColorText = 0x00d8eee8U;
constexpr std::uint32_t kColorDim = 0x0078a2a6U;
constexpr std::uint32_t kColorDirectory = 0x0094d2ffU;
constexpr std::uint32_t kColorExecutable = 0x0091e6a7U;
constexpr std::uint32_t kColorFile = 0x00d5d5d5U;
constexpr std::uint32_t kColorError = 0x00ef8b8bU;

struct BrowserEntry {
    std::DirEntry dir;
    bool executable;
};

struct BrowserState {
    char currentPath[256];
    char status[160];
    BrowserEntry entries[kMaxEntries];
    std::uint32_t entryCount;
    std::uint32_t selected;
    std::uint32_t scroll;
    bool focused;
    bool running;
};

struct SvgAsset {
    char* data;
    std::size_t size;
    bool ready;
};

static BrowserState gBrowserState = {};
static std::DirEntry gDirScratch[kMaxEntries];

void write_str(const char* s) {
    std::serial_write(s, std::strlen(s));
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
        const std::uint32_t rowColor = (red << 16) | (green << 8) | blue;
        for (std::uint32_t x = 0; x < kSurfaceWidth; ++x) {
            pixels[y * kSurfaceWidth + x] = rowColor;
        }
    }
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

void append_char(char* buffer, std::size_t capacity, char c) {
    if (!buffer || capacity == 0) {
        return;
    }

    std::size_t len = std::strlen(buffer);
    if (len + 1 >= capacity) {
        return;
    }

    buffer[len++] = c;
    buffer[len] = '\0';
}

void copy_string(char* buffer, std::size_t capacity, const char* text) {
    if (!buffer || capacity == 0) {
        return;
    }

    if (!text) {
        buffer[0] = '\0';
        return;
    }

    std::strncpy(buffer, text, capacity - 1);
    buffer[capacity - 1] = '\0';
}

void format_uint(std::uint64_t value, char* out, std::size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }

    if (value == 0) {
        if (outSize > 1) {
            out[0] = '0';
            out[1] = '\0';
        } else {
            out[0] = '\0';
        }
        return;
    }

    char temp[32];
    std::size_t pos = 0;
    while (value > 0 && pos < sizeof(temp)) {
        temp[pos++] = static_cast<char>('0' + (value % 10ULL));
        value /= 10ULL;
    }

    std::size_t writePos = 0;
    while (pos > 0 && writePos + 1 < outSize) {
        out[writePos++] = temp[--pos];
    }
    out[writePos] = '\0';
}

void destroy_svg_asset(SvgAsset* asset) {
    if (!asset) {
        return;
    }

    delete[] asset->data;
    asset->data = nullptr;
    asset->size = 0;
    asset->ready = false;
}

bool is_svg_root_at(const char* data, std::size_t size, std::size_t index) {
    if (index + 4 > size) {
        return false;
    }
    if (data[index] != '<' || data[index + 1] != 's' || data[index + 2] != 'v' || data[index + 3] != 'g') {
        return false;
    }
    return index + 4 == size || data[index + 4] == '>' || data[index + 4] == ' ' || data[index + 4] == '\t' || data[index + 4] == '\n' || data[index + 4] == '\r';
}

bool load_svg_asset(SvgAsset* asset, const char* path, const char* fill) {
    if (!asset || !path || !fill) {
        return false;
    }

    destroy_svg_asset(asset);

    std::Stat stat = {};
    if (std::stat(path, &stat) == fail || stat.st_size == 0) {
        return false;
    }

    const std::Handle file = std::open(path, O_RDONLY);
    if (file == fail) {
        return false;
    }

    const std::size_t fileSize = static_cast<std::size_t>(stat.st_size);
    char* raw = new (std::nothrow) char[fileSize + 1];
    if (!raw) {
        std::close(file);
        return false;
    }

    std::size_t totalRead = 0;
    while (totalRead < fileSize) {
        const std::uint64_t bytesRead = std::read(file, raw + totalRead, fileSize - totalRead);
        if (bytesRead == fail || bytesRead == 0) {
            delete[] raw;
            std::close(file);
            return false;
        }
        totalRead += static_cast<std::size_t>(bytesRead);
    }
    std::close(file);
    raw[fileSize] = '\0';

    std::size_t insertAt = 0;
    bool foundRoot = false;
    for (std::size_t index = 0; index < fileSize; ++index) {
        if (is_svg_root_at(raw, fileSize, index)) {
            insertAt = index + 4;
            foundRoot = true;
            break;
        }
    }

    static constexpr char fillPrefix[] = " fill=\"";
    static constexpr char fillSuffix[] = "\"";
    const std::size_t fillLength = std::strlen(fill);
    const std::size_t extra = foundRoot ? (sizeof(fillPrefix) - 1 + fillLength + sizeof(fillSuffix) - 1) : 0;
    char* data = new (std::nothrow) char[fileSize + extra + 1];
    if (!data) {
        delete[] raw;
        return false;
    }

    if (foundRoot) {
        std::memcpy(data, raw, insertAt);
        std::size_t cursor = insertAt;
        std::memcpy(data + cursor, fillPrefix, sizeof(fillPrefix) - 1);
        cursor += sizeof(fillPrefix) - 1;
        std::memcpy(data + cursor, fill, fillLength);
        cursor += fillLength;
        std::memcpy(data + cursor, fillSuffix, sizeof(fillSuffix) - 1);
        cursor += sizeof(fillSuffix) - 1;
        std::memcpy(data + cursor, raw + insertAt, fileSize - insertAt);
    } else {
        std::memcpy(data, raw, fileSize);
    }

    delete[] raw;
    asset->data = data;
    asset->size = fileSize + extra;
    asset->data[asset->size] = '\0';
    asset->ready = true;
    return true;
}

void draw_entry_icon(std::uint32_t* pixels, const SvgAsset& icon, int x, int y) {
    if (!pixels || !icon.ready || !icon.data) {
        return;
    }

    std::SvgRenderer::PixelBuffer target = {
        pixels,
        static_cast<int>(kSurfaceWidth),
        static_cast<int>(kSurfaceHeight),
        static_cast<int>(kSurfaceWidth)
    };
    std::SvgRenderer::RenderOptions options = std::SvgRenderer::default_options();
    options.x = x;
    options.y = y;
    options.width = kEntryIconSize;
    options.height = kEntryIconSize;
    std::SvgRenderer::render(icon.data, icon.size, target, options);
}

char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

bool is_launchable_name(const char* name) {
    if (!name) {
        return false;
    }

    const std::size_t length = std::strlen(name);
    if (length == 0) {
        return false;
    }

    for (std::size_t i = 0; i < length; ++i) {
        if (name[i] == '.') {
            return false;
        }
    }
    return true;
}

bool path_is_root(const char* path) {
    return path && path[0] == '/' && path[1] == '\0';
}

bool build_child_path(const char* base, const char* name, char* output, std::size_t outputSize) {
    if (!base || !name || !output || outputSize == 0) {
        return false;
    }

    output[0] = '\0';
    append_text(output, outputSize, base);
    if (!path_is_root(base)) {
        append_char(output, outputSize, '/');
    }
    append_text(output, outputSize, name);
    return output[0] != '\0';
}

bool build_parent_path(const char* current, char* output, std::size_t outputSize) {
    if (!current || !output || outputSize < 2) {
        return false;
    }

    if (path_is_root(current)) {
        copy_string(output, outputSize, "/");
        return true;
    }

    copy_string(output, outputSize, current);
    std::size_t length = std::strlen(output);
    while (length > 1 && output[length - 1] == '/') {
        output[--length] = '\0';
    }

    while (length > 1 && output[length - 1] != '/') {
        output[--length] = '\0';
    }

    if (length > 1 && output[length - 1] == '/') {
        output[length - 1] = '\0';
    }

    if (output[0] == '\0') {
        copy_string(output, outputSize, "/");
    }
    return true;
}

int compare_names(const char* lhs, const char* rhs) {
    if (!lhs && !rhs) {
        return 0;
    }
    if (!lhs) {
        return -1;
    }
    if (!rhs) {
        return 1;
    }

    for (std::size_t index = 0;; ++index) {
        const char a = ascii_lower(lhs[index]);
        const char b = ascii_lower(rhs[index]);
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        if (a == '\0') {
            return 0;
        }
    }
}

bool entry_before(const BrowserEntry& lhs, const BrowserEntry& rhs) {
    const bool lhsDir = lhs.dir.type == std::FileType::Directory;
    const bool rhsDir = rhs.dir.type == std::FileType::Directory;
    if (lhsDir != rhsDir) {
        return lhsDir;
    }
    return compare_names(lhs.dir.name, rhs.dir.name) < 0;
}

void sort_entries(BrowserState* state) {
    if (!state || state->entryCount < 2) {
        return;
    }

    for (std::uint32_t i = 0; i + 1 < state->entryCount; ++i) {
        for (std::uint32_t j = i + 1; j < state->entryCount; ++j) {
            if (entry_before(state->entries[j], state->entries[i])) {
                const BrowserEntry temp = state->entries[i];
                state->entries[i] = state->entries[j];
                state->entries[j] = temp;
            }
        }
    }
}

void set_status(BrowserState* state, const char* text) {
    if (!state) {
        return;
    }
    copy_string(state->status, sizeof(state->status), text);
}

void set_directory_status(BrowserState* state) {
    if (!state) {
        return;
    }

    char line[160];
    char number[32];
    line[0] = '\0';
    append_text(line, sizeof(line), state->currentPath);
    append_text(line, sizeof(line), "  entries=");
    format_uint(state->entryCount, number, sizeof(number));
    append_text(line, sizeof(line), number);
    set_status(state, line);
}

void ensure_selection_visible(BrowserState* state, std::uint32_t visibleRows) {
    if (!state || visibleRows == 0) {
        return;
    }

    if (state->selected < state->scroll) {
        state->scroll = state->selected;
    } else if (state->selected >= state->scroll + visibleRows) {
        state->scroll = state->selected - visibleRows + 1;
    }
}

bool load_directory(BrowserState* state) {
    if (!state) {
        return false;
    }

    const std::uint64_t found = std::readdir(state->currentPath, gDirScratch, kMaxEntries);
    if (found == fail) {
        set_status(state, "readdir failed");
        state->entryCount = 0;
        state->selected = 0;
        state->scroll = 0;
        return false;
    }

    state->entryCount = 0;
    for (std::uint64_t index = 0; index < found && state->entryCount < kMaxEntries; ++index) {
        if (std::strcmp(gDirScratch[index].name, ".") == 0 ||
            std::strcmp(gDirScratch[index].name, "..") == 0) {
            continue;
        }

        BrowserEntry& entry = state->entries[state->entryCount++];
        entry.dir = gDirScratch[index];
        entry.executable = entry.dir.type == std::FileType::Regular && is_launchable_name(entry.dir.name);
    }

    sort_entries(state);

    if (state->entryCount == 0) {
        state->selected = 0;
        state->scroll = 0;
        set_status(state, "(empty directory)");
        return true;
    }

    if (state->selected >= state->entryCount) {
        state->selected = state->entryCount - 1;
    }
    if (state->scroll >= state->entryCount) {
        state->scroll = 0;
    }

    set_directory_status(state);
    return true;
}

void move_selection(BrowserState* state, int delta, std::uint32_t visibleRows) {
    if (!state || state->entryCount == 0 || delta == 0) {
        return;
    }

    int next = static_cast<int>(state->selected) + delta;
    if (next < 0) {
        next = 0;
    }
    if (next >= static_cast<int>(state->entryCount)) {
        next = static_cast<int>(state->entryCount) - 1;
    }

    state->selected = static_cast<std::uint32_t>(next);
    ensure_selection_visible(state, visibleRows);
}

bool open_parent_directory(BrowserState* state) {
    if (!state) {
        return false;
    }

    if (path_is_root(state->currentPath)) {
        set_status(state, "already at root");
        return false;
    }

    char parent[256];
    if (!build_parent_path(state->currentPath, parent, sizeof(parent))) {
        set_status(state, "failed to resolve parent");
        return false;
    }

    copy_string(state->currentPath, sizeof(state->currentPath), parent);
    state->selected = 0;
    state->scroll = 0;
    return load_directory(state);
}

bool open_selected_entry(BrowserState* state) {
    if (!state || state->entryCount == 0 || state->selected >= state->entryCount) {
        return false;
    }

    const BrowserEntry& entry = state->entries[state->selected];
    char target[256];
    if (!build_child_path(state->currentPath, entry.dir.name, target, sizeof(target))) {
        set_status(state, "path too long");
        return false;
    }

    if (entry.dir.type == std::FileType::Directory) {
        copy_string(state->currentPath, sizeof(state->currentPath), target);
        state->selected = 0;
        state->scroll = 0;
        return load_directory(state);
    }

    if (!entry.executable) {
        set_status(state, "selected file is not launchable");
        return false;
    }

    const std::uint64_t pid = std::spawn(target);
    if (pid == fail) {
        set_status(state, "spawn failed");
        return false;
    }

    char line[160];
    char number[32];
    line[0] = '\0';
    append_text(line, sizeof(line), "launched ");
    append_text(line, sizeof(line), entry.dir.name);
    append_text(line, sizeof(line), " pid=");
    format_uint(pid, number, sizeof(number));
    append_text(line, sizeof(line), number);
    set_status(state, line);
    return true;
}

char translate_key(const std::Event& event) {
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

std::uint32_t entry_color(const BrowserEntry& entry) {
    if (entry.dir.type == std::FileType::Directory) {
        return kColorDirectory;
    }
    if (entry.executable) {
        return kColorExecutable;
    }
    return kColorFile;
}

void draw_browser(std::uint32_t* pixels, instant::UIFont& font, const BrowserState& state, const SvgAsset& folderIcon, const SvgAsset& fileIcon) {
    draw_gradient(pixels);
    fill_rect(
        pixels,
        kSurfaceWidth,
        kSurfaceHeight,
        kPaddingX,
        0,
        static_cast<int>(kSurfaceWidth) - (kPaddingX * 2),
        static_cast<int>(kSurfaceHeight),
        kColorPanel
    );

    const int listTop = kHeaderHeight + 24;
    const int rowHeight = font.lineHeight + kRowGap;
    const std::uint32_t visibleRows =
        static_cast<std::uint32_t>((static_cast<int>(kSurfaceHeight) - listTop - kFooterHeight - 24) / rowHeight);

    if (state.entryCount == 0) {
        instant::draw_text(pixels, kSurfaceWidth, kSurfaceHeight, font, kPaddingX + 14, listTop + font.baseline, "(no entries)", kColorDim);
    } else {
        for (std::uint32_t row = 0; row < visibleRows; ++row) {
            const std::uint32_t index = state.scroll + row;
            if (index >= state.entryCount) {
                break;
            }

            const BrowserEntry& entry = state.entries[index];
            const int rowY = listTop + static_cast<int>(row * rowHeight);
            const bool selected = index == state.selected;
            if (selected) {
                fill_rect(
                    pixels,
                    kSurfaceWidth,
                    kSurfaceHeight,
                    kPaddingX + 10,
                    rowY - font.baseline + 10,
                    static_cast<int>(kSurfaceWidth) - (kPaddingX * 2) - 20,
                    rowHeight,
                    state.focused ? kColorSelected : kColorSelectedMuted
                );
            }

            char line[320];
            line[0] = '\0';
            const bool directory = entry.dir.type == std::FileType::Directory;
            const SvgAsset& icon = directory ? folderIcon : fileIcon;
            if (entry.dir.type == std::FileType::Directory) {
                if (!folderIcon.ready) {
                    append_text(line, sizeof(line), "[dir] ");
                }
            } else if (entry.executable) {
                if (!fileIcon.ready) {
                    append_text(line, sizeof(line), "[exe] ");
                }
            } else {
                if (!fileIcon.ready) {
                    append_text(line, sizeof(line), "[file] ");
                }
            }
            append_text(line, sizeof(line), entry.dir.name);

            if (icon.ready) {
                draw_entry_icon(pixels, icon, kPaddingX + 18, rowY + 1);
            }

            instant::draw_text(
                pixels,
                kSurfaceWidth,
                kSurfaceHeight,
                font,
                icon.ready ? (kPaddingX + 18 + kEntryIconSize + 8) : (kPaddingX + 18),
                rowY + font.baseline,
                line,
                selected ? kColorText : entry_color(entry)
            );
        }
    }
}

void initialize_state(BrowserState* state) {
    if (!state) {
        return;
    }

    std::memset(state, 0, sizeof(*state));
    copy_string(state->currentPath, sizeof(state->currentPath), "/bin");
    copy_string(state->status, sizeof(state->status), "loading /bin");
    state->running = true;
}
class FileBrowserWindow : public instant::Window {
private:
    instant::WindowConfig configure() override {
        instant::WindowConfig config = {};
        config.width = static_cast<int>(kSurfaceWidth);
        config.height = static_cast<int>(kSurfaceHeight);
        config.title = "File Browser";
        config.frameIntervalMs = 16;
        config.autoCommit = false;
        return config;
    }

    Result<bool, std::string> init() override {
        if (!instant::initialize_ui_font(kFontPixelHeight)) {
            return Result<bool, std::string>::error("font init failed");
        }

        state_ = &gBrowserState;
        load_svg_asset(&folderIcon_, kFolderIconPath, kFolderIconFill);
        load_svg_asset(&fileIcon_, kFileIconPath, kFileIconFill);
        initialize_state(state_);
        load_directory(state_);

        const int rowHeight = instant::gUIFont.lineHeight + kRowGap;
        const int listTop = kHeaderHeight + 24;
        visibleRows_ = static_cast<std::uint32_t>((static_cast<int>(kSurfaceHeight) - listTop - kFooterHeight - 24) / rowHeight);
        if (visibleRows_ == 0) {
            visibleRows_ = 1;
        }

        draw_browser(pixels(), instant::gUIFont, *state_, folderIcon_, fileIcon_);
        if (!commit()) {
            return Result<bool, std::string>::error("initial surface_commit failed");
        }
        return true;
    }

    Result<bool, std::string> update() override {
        if (!dirty_) {
            return true;
        }

        draw_browser(pixels(), instant::gUIFont, *state_, folderIcon_, fileIcon_);
        if (!commit()) {
            return Result<bool, std::string>::error("surface_commit update failed");
        }
        dirty_ = false;
        return true;
    }

    Result<bool, std::string> event(const std::Event& event) override {
        if (!state_) {
            return true;
        }

        if (event.type == std::EventType::Window) {
            if (event.window.action == std::WindowEventAction::FocusGained) {
                state_->focused = true;
                dirty_ = true;
            } else if (event.window.action == std::WindowEventAction::FocusLost) {
                state_->focused = false;
                dirty_ = true;
            } else if (event.window.action == std::WindowEventAction::CloseRequested) {
                state_->running = false;
                return false;
            }
            return state_->running;
        }

        const char key = translate_key(event);
        if (key == 0) {
            return true;
        }

        const char lowered = ascii_lower(key);
        if (key == '\b') {
            open_parent_directory(state_);
            dirty_ = true;
        } else if (key == '\n') {
            open_selected_entry(state_);
            dirty_ = true;
        } else if (lowered == 'j' || lowered == 's') {
            move_selection(state_, 1, visibleRows_);
            dirty_ = true;
        } else if (lowered == 'k' || lowered == 'w') {
            move_selection(state_, -1, visibleRows_);
            dirty_ = true;
        } else if (lowered == 'r') {
            load_directory(state_);
            dirty_ = true;
        } else if (lowered == 'q') {
            state_->running = false;
            close();
            return false;
        }

        return state_->running;
    }

    Result<bool, std::string> event() override {
        return true;
    }

    void cleanup() override {
        instant::destroy_ui_font();
        destroy_svg_asset(&folderIcon_);
        destroy_svg_asset(&fileIcon_);
    }

    BrowserState* state_ = nullptr;
    SvgAsset folderIcon_ = {};
    SvgAsset fileIcon_ = {};
    std::uint32_t visibleRows_ = 1;
    bool dirty_ = false;
};
}

INSTANT_WINDOW_APP(FileBrowserWindow)
