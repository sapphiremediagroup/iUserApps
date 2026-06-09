#include <cstdint.hpp>
#include <cstring.hpp>
#include <fcntl.h>
#include <cmath.hpp>
#include <math.h>
#include <new.hpp>
#include <service_protocol.hpp>
#ifdef NULL
#undef NULL
#endif
#define NULL 0

static double stbtt_local_fmod(double value, double modulus) {
    return std::fmod(value, modulus);
}

static double stbtt_local_cuberoot(double value) {
    if (value == 0.0) {
        return 0.0;
    }

    const double sign = value < 0.0 ? -1.0 : 1.0;
    double guess = std::fabs(value);
    if (guess < 1.0) {
        guess = 1.0;
    }

    for (int iteration = 0; iteration < 24; ++iteration) {
        guess = (2.0 * guess + std::fabs(value) / (guess * guess)) / 3.0;
    }
    return sign * guess;
}

static double stbtt_local_pow(double value, double exponent) {
    const double oneThird = 1.0 / 3.0;
    const double diff = exponent - oneThird;
    if (diff > -0.000001 && diff < 0.000001) {
        return stbtt_local_cuberoot(value);
    }

    if (exponent == 0.0) {
        return 1.0;
    }
    if (exponent == 1.0) {
        return value;
    }

    const double integer = std::floor(exponent);
    if (exponent == integer) {
        const bool negative = integer < 0.0;
        std::uint64_t count = static_cast<std::uint64_t>(negative ? -integer : integer);
        double result = 1.0;
        for (std::uint64_t index = 0; index < count; ++index) {
            result *= value;
        }
        return negative ? (result == 0.0 ? 0.0 : 1.0 / result) : result;
    }

    return 1.0;
}

static double stbtt_local_acos(double value) {
    if (value <= -1.0) {
        return 3.14159265358979323846;
    }
    if (value >= 1.0) {
        return 0.0;
    }
    return static_cast<double>(acosf(static_cast<float>(value)));
}

#define STBTT_ifloor(x) static_cast<int>(std::floor(x))
#define STBTT_iceil(x) static_cast<int>(std::ceil(x))
#define STBTT_sqrt(x) std::sqrt(x)
#define STBTT_fmod(x, y) stbtt_local_fmod((x), (y))
#define STBTT_pow(x, y) stbtt_local_pow((x), (y))
#define STBTT_cos(x) std::cos(x)
#define STBTT_acos(x) stbtt_local_acos((x))
#define STBTT_fabs(x) std::fabs(x)
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
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
constexpr char kFontPath[] = "/bin/JetBrainsMono-Regular.ttf";
constexpr unsigned char kFirstCachedGlyph = 32;
constexpr unsigned char kLastCachedGlyph = 126;
constexpr std::size_t kCachedGlyphCount = static_cast<std::size_t>(kLastCachedGlyph - kFirstCachedGlyph + 1);

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

struct GlyphBitmap {
    unsigned char* pixels;
    int width;
    int height;
    int xOffset;
    int yOffset;
    int advance;
};

struct UIFont {
    bool valid;
    unsigned char* data;
    stbtt_fontinfo info;
    float scale;
    int ascent;
    int descent;
    int lineGap;
    int cellWidth;
    int lineHeight;
    int baseline;
    GlyphBitmap glyphs[kCachedGlyphCount];
};

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

static BrowserState gBrowserState = {};
static std::DirEntry gDirScratch[kMaxEntries];

void write_str(const char* s) {
    std::write(std::STDOUT_HANDLE, s, std::strlen(s));
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

std::uint32_t blend_rgb(std::uint32_t dst, std::uint32_t src, std::uint8_t alpha) {
    const std::uint32_t inv = 255U - alpha;
    const std::uint32_t r = (color_r(dst) * inv + color_r(src) * alpha) / 255U;
    const std::uint32_t g = (color_g(dst) * inv + color_g(src) * alpha) / 255U;
    const std::uint32_t b = (color_b(dst) * inv + color_b(src) * alpha) / 255U;
    return pack_rgb(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b));
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

bool initialize_font(UIFont* font) {
    if (!font) {
        return false;
    }

    std::memset(font, 0, sizeof(*font));
    std::Stat stat = {};
    if (std::stat(kFontPath, &stat) == fail || stat.st_size == 0) {
        return false;
    }

    const std::Handle file = std::open(kFontPath, O_RDONLY);
    if (file == fail) {
        return false;
    }

    const std::size_t fontSize = static_cast<std::size_t>(stat.st_size);
    font->data = new (std::nothrow) unsigned char[fontSize];
    if (!font->data) {
        std::close(file);
        return false;
    }

    std::size_t totalRead = 0;
    while (totalRead < fontSize) {
        const std::uint64_t bytesRead = std::read(file, font->data + totalRead, fontSize - totalRead);
        if (bytesRead == fail || bytesRead == 0) {
            delete[] font->data;
            font->data = nullptr;
            std::close(file);
            std::memset(font, 0, sizeof(*font));
            return false;
        }
        totalRead += static_cast<std::size_t>(bytesRead);
    }
    std::close(file);

    const int fontOffset = stbtt_GetFontOffsetForIndex(font->data, 0);
    if (fontOffset < 0 || stbtt_InitFont(&font->info, font->data, fontOffset) == 0) {
        delete[] font->data;
        font->data = nullptr;
        std::memset(font, 0, sizeof(*font));
        return false;
    }

    font->scale = stbtt_ScaleForPixelHeight(&font->info, static_cast<float>(kFontPixelHeight));
    if (font->scale <= 0.0f) {
        delete[] font->data;
        font->data = nullptr;
        std::memset(font, 0, sizeof(*font));
        return false;
    }

    stbtt_GetFontVMetrics(&font->info, &font->ascent, &font->descent, &font->lineGap);
    font->baseline = static_cast<int>(font->ascent * font->scale + 0.999f);
    font->lineHeight = static_cast<int>(((font->ascent - font->descent + font->lineGap) * font->scale) + 0.999f);
    if (font->lineHeight <= 0) {
        font->lineHeight = static_cast<int>(kFontPixelHeight) + 4;
    }

    int advance = 0;
    int leftSideBearing = 0;
    stbtt_GetCodepointHMetrics(&font->info, 'M', &advance, &leftSideBearing);
    (void) leftSideBearing;
    font->cellWidth = static_cast<int>(advance * font->scale + 0.999f);
    if (font->cellWidth <= 0) {
        font->cellWidth = 8;
    }

    for (std::size_t index = 0; index < kCachedGlyphCount; ++index) {
        const int codepoint = static_cast<int>(kFirstCachedGlyph + index);
        GlyphBitmap& glyph = font->glyphs[index];

        int advanceWidth = 0;
        int glyphLeftSideBearing = 0;
        stbtt_GetCodepointHMetrics(&font->info, codepoint, &advanceWidth, &glyphLeftSideBearing);
        (void) glyphLeftSideBearing;

        glyph.advance = static_cast<int>(advanceWidth * font->scale + 0.999f);
        if (glyph.advance <= 0) {
            glyph.advance = font->cellWidth;
        }

        glyph.pixels = stbtt_GetCodepointBitmap(
            &font->info,
            font->scale,
            font->scale,
            codepoint,
            &glyph.width,
            &glyph.height,
            &glyph.xOffset,
            &glyph.yOffset
        );
        if (!glyph.pixels) {
            glyph.width = 0;
            glyph.height = 0;
            glyph.xOffset = 0;
            glyph.yOffset = 0;
        }
    }

    font->valid = true;
    return true;
}

void destroy_font(UIFont* font) {
    if (!font || !font->valid) {
        return;
    }

    for (std::size_t index = 0; index < kCachedGlyphCount; ++index) {
        if (font->glyphs[index].pixels) {
            stbtt_FreeBitmap(font->glyphs[index].pixels, nullptr);
            font->glyphs[index].pixels = nullptr;
        }
    }
    delete[] font->data;
    std::memset(font, 0, sizeof(*font));
}

void draw_text(
    std::uint32_t* pixels,
    std::uint32_t surfaceWidth,
    std::uint32_t surfaceHeight,
    UIFont& font,
    int x,
    int baselineY,
    const char* text,
    std::uint32_t color
) {
    if (!pixels || !font.valid || !text) {
        return;
    }

    int penX = x;
    for (std::size_t index = 0; text[index] != '\0'; ++index) {
        const unsigned char ch = static_cast<unsigned char>(text[index]);
        if (ch < kFirstCachedGlyph || ch > kLastCachedGlyph) {
            penX += font.cellWidth;
            continue;
        }

        const GlyphBitmap& glyph = font.glyphs[ch - kFirstCachedGlyph];
        if (!glyph.pixels || glyph.width <= 0 || glyph.height <= 0) {
            penX += glyph.advance > 0 ? glyph.advance : font.cellWidth;
            continue;
        }

        const int startX = penX + glyph.xOffset;
        const int startY = baselineY + glyph.yOffset;

        for (int drawY = 0; drawY < glyph.height; ++drawY) {
            const int dstY = startY + drawY;
            if (dstY < 0 || dstY >= static_cast<int>(surfaceHeight)) {
                continue;
            }

            for (int drawX = 0; drawX < glyph.width; ++drawX) {
                const int dstX = startX + drawX;
                if (dstX < 0 || dstX >= static_cast<int>(surfaceWidth)) {
                    continue;
                }

                const std::uint8_t alpha = glyph.pixels[drawY * glyph.width + drawX];
                if (alpha == 0) {
                    continue;
                }

                std::uint32_t& dst = pixels[dstY * surfaceWidth + dstX];
                dst = blend_rgb(dst, color, alpha);
            }
        }

        penX += glyph.advance > 0 ? glyph.advance : font.cellWidth;
    }
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

void draw_browser(std::uint32_t* pixels, UIFont& font, const BrowserState& state) {
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
        draw_text(pixels, kSurfaceWidth, kSurfaceHeight, font, kPaddingX + 14, listTop + font.baseline, "(no entries)", kColorDim);
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
            if (entry.dir.type == std::FileType::Directory) {
                append_text(line, sizeof(line), "[dir] ");
            } else if (entry.executable) {
                append_text(line, sizeof(line), "[exe] ");
            } else {
                append_text(line, sizeof(line), "[file] ");
            }
            append_text(line, sizeof(line), entry.dir.name);

            draw_text(
                pixels,
                kSurfaceWidth,
                kSurfaceHeight,
                font,
                kPaddingX + 18,
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
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    const std::Handle compositor = connect_service(std::services::graphics_compositor::NAME);
    if (compositor == fail) {
        write_str("[file-browser] FAIL service_connect graphics.compositor\n");
        return 1;
    }

    const std::Handle surface = std::surface_create(
        kSurfaceWidth,
        kSurfaceHeight,
        std::services::surfaces::FORMAT_BGRA8
    );
    if (surface == fail) {
        write_str("[file-browser] FAIL surface_create\n");
        std::close(compositor);
        return 1;
    }

    auto* pixels = static_cast<std::uint32_t*>(std::shared_map(surface));
    if (pixels == reinterpret_cast<std::uint32_t*>(fail) || pixels == nullptr) {
        write_str("[file-browser] FAIL shared_map(surface)\n");
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    const std::Handle window = std::compositor_create_window(compositor, kSurfaceWidth, kSurfaceHeight, 0);
    if (window == fail) {
        write_str("[file-browser] FAIL compositor_create_window\n");
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    if (std::window_set_title(window, "File Browser") == fail ||
        std::window_attach_surface(window, surface) == fail) {
        write_str("[file-browser] FAIL window setup\n");
        std::close(window);
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    const std::Handle events = std::window_event_queue(window);
    if (events == fail) {
        write_str("[file-browser] FAIL window_event_queue\n");
        std::close(window);
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    UIFont font = {};
    if (!initialize_font(&font)) {
        write_str("[file-browser] FAIL font init\n");
        std::close(events);
        std::close(window);
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    BrowserState& state = gBrowserState;
    initialize_state(&state);
    load_directory(&state);
    draw_browser(pixels, font, state);
    if (std::surface_commit(surface, 0, 0, kSurfaceWidth, kSurfaceHeight) == fail) {
        write_str("[file-browser] FAIL initial surface_commit\n");
        destroy_font(&font);
        std::close(events);
        std::close(window);
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    const int rowHeight = font.lineHeight + kRowGap;
    const int listTop = kHeaderHeight + 24;
    const std::uint32_t visibleRows =
        static_cast<std::uint32_t>((static_cast<int>(kSurfaceHeight) - listTop - kFooterHeight - 24) / rowHeight);

    while (state.running) {
        std::Event event = {};
        if (std::event_wait(events, &event) == fail) {
            std::yield();
            continue;
        }

        bool redraw = false;
        if (event.type == std::EventType::Window) {
            if (event.window.action == std::WindowEventAction::FocusGained) {
                state.focused = true;
                redraw = true;
            } else if (event.window.action == std::WindowEventAction::FocusLost) {
                state.focused = false;
                redraw = true;
            } else if (event.window.action == std::WindowEventAction::CloseRequested) {
                state.running = false;
                break;
            }
        } else {
            const char key = translate_key(event);
            if (key == 0) {
                continue;
            }

            const char lowered = ascii_lower(key);
            if (key == '\b') {
                open_parent_directory(&state);
                redraw = true;
            } else if (key == '\n') {
                open_selected_entry(&state);
                redraw = true;
            } else if (lowered == 'j' || lowered == 's') {
                move_selection(&state, 1, visibleRows);
                redraw = true;
            } else if (lowered == 'k' || lowered == 'w') {
                move_selection(&state, -1, visibleRows);
                redraw = true;
            } else if (lowered == 'r') {
                load_directory(&state);
                redraw = true;
            } else if (lowered == 'q') {
                state.running = false;
                break;
            }
        }

        if (redraw) {
            draw_browser(pixels, font, state);
            if (std::surface_commit(surface, 0, 0, kSurfaceWidth, kSurfaceHeight) == fail) {
                write_str("[file-browser] FAIL surface_commit update\n");
                break;
            }
        }
    }

    destroy_font(&font);
    std::close(events);
    std::close(window);
    std::close(surface);
    std::close(compositor);
    return 0;
}
