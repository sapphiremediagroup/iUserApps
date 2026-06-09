#include <cstdint.hpp>
#include <cstring.hpp>
#include <fcntl.h>
#include <new.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>
#include <termios.h>

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr std::uint32_t kSurfaceWidth = 920;
constexpr std::uint32_t kSurfaceHeight = 560;
constexpr std::uint32_t kFrameIntervalMs = 33;
constexpr std::uint32_t kCursorBlinkMs = 500;
constexpr int kPaddingX = 14;
constexpr int kPaddingY = 14;
constexpr int kPromptGap = 8;
constexpr std::uint32_t kFontPixelHeight = 15;
constexpr std::uint32_t kMaxHistoryLines = 256;
constexpr std::uint32_t kMaxLineChars = 240;
constexpr std::uint32_t kMaxInputChars = 220;
constexpr std::uint32_t kDirBatchEntries = 64;
constexpr unsigned char kFirstCachedGlyph = 32;
constexpr unsigned char kLastCachedGlyph = 126;
constexpr std::size_t kCachedGlyphCount = static_cast<std::size_t>(kLastCachedGlyph - kFirstCachedGlyph + 1);

constexpr std::uint32_t kColorBackground = 0x00091218U;
constexpr std::uint32_t kColorHeader = 0x00111e2fU;
constexpr std::uint32_t kColorHeaderAccent = 0x003ba4d8U;
constexpr std::uint32_t kColorText = 0x00d7f2e6U;
constexpr std::uint32_t kColorPrompt = 0x00f2c14eU;
constexpr std::uint32_t kColorDim = 0x0073a49fU;
constexpr std::uint32_t kColorCursor = 0x00f2c14eU;
constexpr std::uint32_t kColorSuccess = 0x0085d39aU;
constexpr std::uint32_t kColorError = 0x00ef798aU;

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
    std::Handle service;
    int cellWidth;
    int lineHeight;
    int baseline;
    GlyphBitmap glyphs[kCachedGlyphCount];
};

struct TerminalState {
    char history[kMaxHistoryLines][kMaxLineChars];
    std::uint32_t historyCount;
    char input[kMaxInputChars];
    std::uint32_t inputLength;
    char cwd[256];
    bool focused;
    bool running;
    bool cursorVisible;
    std::uint64_t lastBlinkMs;
    std::Handle consoleQueue;
    char stdinBuf[1024];
    std::uint32_t stdinHead;
    std::uint32_t stdinTail;
    termios tty;
    pid_t foregroundPgid;
    struct PendingRead {
        std::uint64_t requestId;
        std::uint64_t count;
    } pendingReads[16];
    std::uint32_t pendingReadCount;
};

static TerminalState gTerminalState = {};

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

void destroy_font(UIFont* font);

bool initialize_font(UIFont* font) {
    if (!font) {
        return false;
    }

    std::memset(font, 0, sizeof(*font));
    font->service = connect_service(std::services::font_manager::NAME);
    if (font->service == fail) {
        return false;
    }

    std::services::font_manager::FontInfoRequest infoRequest = {};
    infoRequest.header.version = std::services::font_manager::VERSION;
    infoRequest.header.opcode = static_cast<std::uint16_t>(std::services::font_manager::Opcode::GetFontInfo);
    infoRequest.pixelHeight = kFontPixelHeight;

    std::IPCMessage message = {};
    if (!std::services::encode_request(&message, infoRequest)) {
        std::close(font->service);
        std::memset(font, 0, sizeof(*font));
        return false;
    }

    std::services::font_manager::FontInfoReply infoReply = {};
    std::uint64_t replySize = 0;
    if (std::queue_request(font->service, &message, &infoReply, sizeof(infoReply), &replySize) == fail ||
        replySize < sizeof(infoReply) ||
        infoReply.status != std::services::STATUS_OK) {
        std::close(font->service);
        std::memset(font, 0, sizeof(*font));
        return false;
    }

    font->baseline = infoReply.baseline;
    font->lineHeight = infoReply.lineHeight;
    font->cellWidth = infoReply.cellWidth;

    for (std::size_t index = 0; index < kCachedGlyphCount; ++index) {
        const int codepoint = static_cast<int>(kFirstCachedGlyph + index);
        GlyphBitmap& glyph = font->glyphs[index];

        std::services::font_manager::GlyphMetricsRequest metricsRequest = {};
        metricsRequest.header.version = std::services::font_manager::VERSION;
        metricsRequest.header.opcode = static_cast<std::uint16_t>(std::services::font_manager::Opcode::GetGlyphMetrics);
        metricsRequest.pixelHeight = kFontPixelHeight;
        metricsRequest.codepoint = static_cast<std::uint32_t>(codepoint);
        if (!std::services::encode_request(&message, metricsRequest)) {
            destroy_font(font);
            return false;
        }

        std::services::font_manager::GlyphMetricsReply metricsReply = {};
        replySize = 0;
        if (std::queue_request(font->service, &message, &metricsReply, sizeof(metricsReply), &replySize) == fail ||
            replySize < sizeof(metricsReply) ||
            metricsReply.status != std::services::STATUS_OK) {
            destroy_font(font);
            return false;
        }

        glyph.width = metricsReply.width;
        glyph.height = metricsReply.height;
        glyph.xOffset = metricsReply.xOffset;
        glyph.yOffset = metricsReply.yOffset;
        glyph.advance = metricsReply.advance;
        if (glyph.advance <= 0) {
            glyph.advance = font->cellWidth;
        }

        const std::uint64_t pixelCount = static_cast<std::uint64_t>(glyph.width) * static_cast<std::uint64_t>(glyph.height);
        if (glyph.width <= 0 || glyph.height <= 0 || glyph.width > static_cast<int>(std::services::font_manager::MAX_GLYPH_ROW_PIXELS) || pixelCount == 0) {
            glyph.width = 0;
            glyph.height = 0;
            glyph.xOffset = 0;
            glyph.yOffset = 0;
            continue;
        }

        glyph.pixels = new (std::nothrow) unsigned char[static_cast<std::size_t>(pixelCount)];
        if (!glyph.pixels) {
            destroy_font(font);
            return false;
        }

        for (int row = 0; row < glyph.height; ++row) {
            std::services::font_manager::GlyphRowRequest rowRequest = {};
            rowRequest.header.version = std::services::font_manager::VERSION;
            rowRequest.header.opcode = static_cast<std::uint16_t>(std::services::font_manager::Opcode::GetGlyphRow);
            rowRequest.pixelHeight = kFontPixelHeight;
            rowRequest.codepoint = static_cast<std::uint32_t>(codepoint);
            rowRequest.row = static_cast<std::uint32_t>(row);
            if (!std::services::encode_request(&message, rowRequest)) {
                destroy_font(font);
                return false;
            }

            std::services::font_manager::GlyphRowReply rowReply = {};
            replySize = 0;
            if (std::queue_request(font->service, &message, &rowReply, sizeof(rowReply), &replySize) == fail ||
                replySize < sizeof(rowReply) ||
                rowReply.status != std::services::STATUS_OK ||
                rowReply.width != static_cast<std::uint32_t>(glyph.width)) {
                destroy_font(font);
                return false;
            }

            std::memcpy(
                glyph.pixels + (static_cast<std::size_t>(row) * static_cast<std::size_t>(glyph.width)),
                rowReply.pixels,
                static_cast<std::size_t>(glyph.width)
            );
        }
    }

    font->valid = true;
    return true;
}

void destroy_font(UIFont* font) {
    if (!font) {
        return;
    }

    for (std::size_t index = 0; index < kCachedGlyphCount; ++index) {
        if (font->glyphs[index].pixels) {
            delete[] font->glyphs[index].pixels;
            font->glyphs[index].pixels = nullptr;
        }
    }
    if (font->service != fail && font->service != 0) {
        std::close(font->service);
    }
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
    buffer[len] = c;
    buffer[len + 1] = '\0';
}

void capture_cwd(TerminalState* state) {
    if (!state) {
        return;
    }

    if (std::getcwd(state->cwd, sizeof(state->cwd)) == nullptr) {
        state->cwd[0] = '/';
        state->cwd[1] = '\0';
    }
}

void push_history_line(TerminalState* state, const char* text) {
    if (!state || !text) {
        return;
    }

    if (state->historyCount >= kMaxHistoryLines) {
        std::memmove(
            state->history,
            state->history + 1,
            sizeof(state->history[0]) * (kMaxHistoryLines - 1)
        );
        state->historyCount = kMaxHistoryLines - 1;
    }

    std::strncpy(state->history[state->historyCount], text, kMaxLineChars - 1);
    state->history[state->historyCount][kMaxLineChars - 1] = '\0';
    state->historyCount++;
}

void append_wrapped_text(TerminalState* state, const char* text, std::uint32_t columns) {
    if (!state || !text) {
        return;
    }

    if (columns == 0) {
        columns = 1;
    }
    if (columns >= kMaxLineChars) {
        columns = kMaxLineChars - 1;
    }

    char line[kMaxLineChars];
    std::size_t length = 0;
    line[0] = '\0';

    for (std::size_t index = 0;; ++index) {
        const char c = text[index];
        if (c == '\0' || c == '\n') {
            line[length] = '\0';
            push_history_line(state, line);
            length = 0;
            line[0] = '\0';
            if (c == '\0') {
                break;
            }
            continue;
        }

        if (c == '\r') {
            continue;
        }

        if (c == '\t') {
            for (int tab = 0; tab < 4; ++tab) {
                if (length >= columns) {
                    line[length] = '\0';
                    push_history_line(state, line);
                    length = 0;
                }
                line[length++] = ' ';
                line[length] = '\0';
            }
            continue;
        }

        char printable = c;
        if (static_cast<unsigned char>(printable) < 32U || static_cast<unsigned char>(printable) > 126U) {
            printable = '.';
        }

        if (length >= columns) {
            line[length] = '\0';
            push_history_line(state, line);
            length = 0;
        }

        line[length++] = printable;
        line[length] = '\0';
    }
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

char* trim_in_place(char* text) {
    if (!text) {
        return text;
    }

    while (*text && is_space(*text)) {
        ++text;
    }

    std::size_t len = std::strlen(text);
    while (len > 0 && is_space(text[len - 1])) {
        text[--len] = '\0';
    }

    return text;
}

char* next_argument(char* text) {
    if (!text) {
        return nullptr;
    }

    while (*text && !is_space(*text)) {
        ++text;
    }
    while (*text && is_space(*text)) {
        *text = '\0';
        ++text;
    }
    return text;
}

bool normalize_absolute_path(const char* input, char* output, std::size_t outputSize) {
    if (!input || !output || outputSize < 2 || input[0] != '/') {
        return false;
    }

    char temp[256];
    std::strncpy(temp, input, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    char* components[64];
    int depth = 0;

    char* cursor = temp;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        char* token = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        if (*cursor == '/') {
            *cursor++ = '\0';
        }

        if (std::strcmp(token, ".") == 0) {
            continue;
        }
        if (std::strcmp(token, "..") == 0) {
            if (depth > 0) {
                --depth;
            }
            continue;
        }

        if (depth >= 64) {
            return false;
        }
        components[depth++] = token;
    }

    std::size_t length = 0;
    output[length++] = '/';

    for (int index = 0; index < depth; ++index) {
        const char* token = components[index];
        if (index > 0) {
            if (length + 1 >= outputSize) {
                return false;
            }
            output[length++] = '/';
        }

        for (std::size_t charIndex = 0; token[charIndex] != '\0'; ++charIndex) {
            if (length + 1 >= outputSize) {
                return false;
            }
            output[length++] = token[charIndex];
        }
    }

    output[length] = '\0';
    return true;
}

bool resolve_path(const TerminalState& state, const char* path, char* output, std::size_t outputSize) {
    if (!path || !output || outputSize == 0) {
        return false;
    }

    char combined[256];
    if (path[0] == '/') {
        std::strncpy(combined, path, sizeof(combined) - 1);
        combined[sizeof(combined) - 1] = '\0';
    } else {
        const char* base = state.cwd[0] != '\0' ? state.cwd : "/";
        std::strncpy(combined, base, sizeof(combined) - 1);
        combined[sizeof(combined) - 1] = '\0';

        if (std::strcmp(combined, "/") != 0) {
            append_char(combined, sizeof(combined), '/');
        }
        append_text(combined, sizeof(combined), path);
    }

    return normalize_absolute_path(combined, output, outputSize);
}

bool build_bin_target(const char* name, char* output, std::size_t outputSize) {
    if (!name || !output || outputSize == 0) {
        return false;
    }

    output[0] = '\0';
    append_text(output, outputSize, "/bin/");
    append_text(output, outputSize, name);
    return true;
}

void build_prompt(const TerminalState& state, char* out, std::size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }

    const char* cwd = state.cwd[0] != '\0' ? state.cwd : "/";
    const std::size_t cwdLength = std::strlen(cwd);

    out[0] = '\0';
    if (cwdLength > 22) {
        append_text(out, outSize, "...");
        append_text(out, outSize, cwd + (cwdLength - 19));
    } else {
        append_text(out, outSize, cwd);
    }
    append_text(out, outSize, " $ ");
}

void print_number_line(TerminalState* state, const char* label, std::uint64_t value, std::uint32_t columns) {
    char line[kMaxLineChars];
    char number[32];
    line[0] = '\0';
    format_uint(value, number, sizeof(number));
    append_text(line, sizeof(line), label);
    append_text(line, sizeof(line), number);
    append_wrapped_text(state, line, columns);
}

void print_result_line(TerminalState* state, const char* label, bool ok, std::uint32_t columns) {
    char line[kMaxLineChars];
    line[0] = '\0';
    append_text(line, sizeof(line), label);
    append_text(line, sizeof(line), ok ? "ok" : "fail");
    append_wrapped_text(state, line, columns);
}

void shell_help(TerminalState* state, std::uint32_t columns) {
    append_wrapped_text(state, "Built-in commands:", columns);
    append_wrapped_text(state, "  help          show this help", columns);
    append_wrapped_text(state, "  clear         clear scrollback", columns);
    append_wrapped_text(state, "  echo TEXT     print text", columns);
    append_wrapped_text(state, "  time          show uptime in milliseconds", columns);
    append_wrapped_text(state, "  about         show system information", columns);
    append_wrapped_text(state, "  pid           show current process id", columns);
    append_wrapped_text(state, "  whoami        show current user and ids", columns);
    append_wrapped_text(state, "  pwd           print current directory", columns);
    append_wrapped_text(state, "  cd PATH       change directory", columns);
    append_wrapped_text(state, "  ls [PATH]     list a directory", columns);
    append_wrapped_text(state, "  cat PATH      print a text file", columns);
    append_wrapped_text(state, "  spawn PATH    start an executable by path", columns);
    append_wrapped_text(state, "  launch NAME   start /bin/NAME", columns);
    append_wrapped_text(state, "  exit          close the terminal window", columns);
}

void shell_about(TerminalState* state, std::uint32_t columns) {
    std::OSInfo info = {};
    if (std::osinfo(&info) == fail) {
        append_wrapped_text(state, "osinfo syscall failed", columns);
        return;
    }

    char line[kMaxLineChars];
    line[0] = '\0';
    append_text(line, sizeof(line), info.osname);
    append_text(line, sizeof(line), " ");
    char version[64];
    version[0] = '\0';
    char major[16];
    char minor[16];
    char patch[16];
    format_uint(info.major, major, sizeof(major));
    format_uint(info.minor, minor, sizeof(minor));
    format_uint(info.patch, patch, sizeof(patch));
    append_text(version, sizeof(version), major);
    append_char(version, sizeof(version), '.');
    append_text(version, sizeof(version), minor);
    append_char(version, sizeof(version), '.');
    append_text(version, sizeof(version), patch);
    append_text(line, sizeof(line), version);
    append_wrapped_text(state, line, columns);

    line[0] = '\0';
    append_text(line, sizeof(line), "user: ");
    append_text(line, sizeof(line), info.loggedOnUser);
    append_wrapped_text(state, line, columns);

    line[0] = '\0';
    append_text(line, sizeof(line), "cpu: ");
    append_text(line, sizeof(line), info.cpuname);
    append_wrapped_text(state, line, columns);

    line[0] = '\0';
    append_text(line, sizeof(line), "memory used/max (MiB): ");
    append_text(line, sizeof(line), info.usedRamGB);
    append_text(line, sizeof(line), "/");
    append_text(line, sizeof(line), info.maxRamGB);
    append_wrapped_text(state, line, columns);
}

void shell_whoami(TerminalState* state, std::uint32_t columns) {
    std::OSInfo info = {};
    if (std::osinfo(&info) == fail) {
        append_wrapped_text(state, "osinfo syscall failed", columns);
        return;
    }

    char line[kMaxLineChars];
    line[0] = '\0';
    append_text(line, sizeof(line), info.loggedOnUser);
    append_text(line, sizeof(line), " uid=");
    char value[32];
    format_uint(std::getuid(), value, sizeof(value));
    append_text(line, sizeof(line), value);
    append_text(line, sizeof(line), " gid=");
    format_uint(std::getgid(), value, sizeof(value));
    append_text(line, sizeof(line), value);
    append_text(line, sizeof(line), " sid=");
    format_uint(std::getsessionid(), value, sizeof(value));
    append_text(line, sizeof(line), value);
    append_wrapped_text(state, line, columns);
}

void shell_pwd(TerminalState* state, std::uint32_t columns) {
    capture_cwd(state);
    append_wrapped_text(state, state->cwd, columns);
}

void shell_cd(TerminalState* state, const char* arg, std::uint32_t columns) {
    if (!arg || arg[0] == '\0') {
        append_wrapped_text(state, "cd: missing path", columns);
        return;
    }

    char resolved[256];
    if (!resolve_path(*state, arg, resolved, sizeof(resolved))) {
        append_wrapped_text(state, "cd: invalid path", columns);
        return;
    }

    if (std::chdir(resolved) == fail) {
        append_wrapped_text(state, "cd: failed", columns);
        return;
    }

    capture_cwd(state);
    append_wrapped_text(state, state->cwd, columns);
}

void shell_ls(TerminalState* state, const char* arg, std::uint32_t columns) {
    char resolved[256];
    const char* target = arg && arg[0] != '\0' ? arg : state->cwd;
    if (!resolve_path(*state, target, resolved, sizeof(resolved))) {
        append_wrapped_text(state, "ls: invalid path", columns);
        return;
    }

    std::DirEntry entries[kDirBatchEntries];
    const std::uint64_t count = std::readdir(resolved, entries, kDirBatchEntries);
    if (count == fail) {
        append_wrapped_text(state, "ls: failed", columns);
        return;
    }
    if (count == 0) {
        append_wrapped_text(state, "(empty)", columns);
        return;
    }

    for (std::uint64_t index = 0; index < count; ++index) {
        char line[kMaxLineChars];
        line[0] = '\0';
        if (entries[index].type == std::FileType::Directory) {
            append_text(line, sizeof(line), "[dir] ");
        } else {
            append_text(line, sizeof(line), "[file] ");
        }
        append_text(line, sizeof(line), entries[index].name);
        append_wrapped_text(state, line, columns);
    }
}

void shell_cat(TerminalState* state, const char* arg, std::uint32_t columns) {
    if (!arg || arg[0] == '\0') {
        append_wrapped_text(state, "cat: missing path", columns);
        return;
    }

    char resolved[256];
    if (!resolve_path(*state, arg, resolved, sizeof(resolved))) {
        append_wrapped_text(state, "cat: invalid path", columns);
        return;
    }

    const std::Handle file = std::open(resolved, O_RDONLY);
    if (file == fail) {
        append_wrapped_text(state, "cat: open failed", columns);
        return;
    }

    char buffer[257];
    bool anyData = false;
    for (;;) {
        const std::uint64_t bytes = std::read(file, buffer, sizeof(buffer) - 1);
        if (bytes == fail) {
            append_wrapped_text(state, "cat: read failed", columns);
            break;
        }
        if (bytes == 0) {
            break;
        }

        anyData = true;
        buffer[bytes] = '\0';
        append_wrapped_text(state, buffer, columns);
    }

    if (!anyData) {
        append_wrapped_text(state, "(empty file)", columns);
    }

    std::close(file);
}

void shell_spawn(TerminalState* state, const char* arg, std::uint32_t columns) {
    if (!arg || arg[0] == '\0') {
        append_wrapped_text(state, "spawn: missing executable path", columns);
        return;
    }

    char resolved[256];
    if (!resolve_path(*state, arg, resolved, sizeof(resolved))) {
        append_wrapped_text(state, "spawn: invalid path", columns);
        return;
    }

    const std::uint64_t pid = std::spawn(resolved);
    if (pid == fail) {
        append_wrapped_text(state, "spawn: failed", columns);
        return;
    }

    char line[kMaxLineChars];
    char value[32];
    line[0] = '\0';
    append_text(line, sizeof(line), "spawned pid=");
    format_uint(pid, value, sizeof(value));
    append_text(line, sizeof(line), value);
    append_wrapped_text(state, line, columns);
}

void shell_launch(TerminalState* state, const char* arg, std::uint32_t columns) {
    if (!arg || arg[0] == '\0') {
        append_wrapped_text(state, "launch: missing executable name", columns);
        return;
    }

    char target[256];
    if (!build_bin_target(arg, target, sizeof(target))) {
        append_wrapped_text(state, "launch: invalid executable name", columns);
        return;
    }

    const std::uint64_t pid = std::spawn(target);
    if (pid == fail) {
        append_wrapped_text(state, "launch: failed", columns);
        return;
    }

    char line[kMaxLineChars];
    char value[32];
    line[0] = '\0';
    append_text(line, sizeof(line), "launched ");
    append_text(line, sizeof(line), target);
    append_text(line, sizeof(line), " pid=");
    format_uint(pid, value, sizeof(value));
    append_text(line, sizeof(line), value);
    append_wrapped_text(state, line, columns);
}

void execute_command(TerminalState* state, std::uint32_t columns) {
    if (!state) {
        return;
    }

    capture_cwd(state);

    char prompt[64];
    build_prompt(*state, prompt, sizeof(prompt));

    char commandLine[kMaxLineChars];
    commandLine[0] = '\0';
    append_text(commandLine, sizeof(commandLine), prompt);
    append_text(commandLine, sizeof(commandLine), state->input);
    append_wrapped_text(state, commandLine, columns);

    char commandBuffer[kMaxInputChars];
    std::strncpy(commandBuffer, state->input, sizeof(commandBuffer) - 1);
    commandBuffer[sizeof(commandBuffer) - 1] = '\0';
    state->input[0] = '\0';
    state->inputLength = 0;

    char* command = trim_in_place(commandBuffer);
    if (command[0] == '\0') {
        return;
    }

    char* args = next_argument(command);
    args = trim_in_place(args);

    if (std::strcmp(command, "help") == 0) {
        shell_help(state, columns);
    } else if (std::strcmp(command, "clear") == 0) {
        state->historyCount = 0;
    } else if (std::strcmp(command, "echo") == 0) {
        append_wrapped_text(state, args, columns);
    } else if (std::strcmp(command, "time") == 0) {
        print_number_line(state, "uptime_ms=", std::gettime(), columns);
    } else if (std::strcmp(command, "about") == 0) {
        shell_about(state, columns);
    } else if (std::strcmp(command, "pid") == 0) {
        print_number_line(state, "pid=", std::getpid(), columns);
    } else if (std::strcmp(command, "whoami") == 0) {
        shell_whoami(state, columns);
    } else if (std::strcmp(command, "pwd") == 0) {
        shell_pwd(state, columns);
    } else if (std::strcmp(command, "cd") == 0) {
        shell_cd(state, args, columns);
    } else if (std::strcmp(command, "ls") == 0) {
        shell_ls(state, args, columns);
    } else if (std::strcmp(command, "cat") == 0) {
        shell_cat(state, args, columns);
    } else if (std::strcmp(command, "spawn") == 0) {
        shell_spawn(state, args, columns);
    } else if (std::strcmp(command, "launch") == 0) {
        shell_launch(state, args, columns);
    } else if (std::strcmp(command, "exit") == 0) {
        state->running = false;
    } else {
        char line[kMaxLineChars];
        line[0] = '\0';
        append_text(line, sizeof(line), "unknown command: ");
        append_text(line, sizeof(line), command);
        append_wrapped_text(state, line, columns);
    }
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
    if (event.key.keycode == '\n' || event.key.keycode == '\b' || event.key.keycode == '\t') {
        return static_cast<char>(event.key.keycode);
    }

    return 0;
}

static bool tty_canonical(const TerminalState& state) {
    return (state.tty.c_lflag & ICANON) != 0;
}

static bool tty_echo(const TerminalState& state) {
    return (state.tty.c_lflag & ECHO) != 0;
}

static void stdin_push(TerminalState* state, char c) {
    if (!state) {
        return;
    }
    const std::uint32_t nextTail = (state->stdinTail + 1U) % static_cast<std::uint32_t>(sizeof(state->stdinBuf));
    if (nextTail != state->stdinHead) {
        state->stdinBuf[state->stdinTail] = c;
        state->stdinTail = nextTail;
    }
}

static void commit_input_to_stdin(TerminalState* state, bool includeNewline) {
    if (!state) {
        return;
    }
    for (std::uint32_t i = 0; i < state->inputLength; ++i) {
        stdin_push(state, state->input[i]);
    }
    if (includeNewline) {
        stdin_push(state, '\n');
    }
}

static void clear_input_line(TerminalState* state) {
    if (!state) {
        return;
    }
    state->inputLength = 0;
    state->input[0] = '\0';
}

static void complete_pending_read(TerminalState* state, const void* data, std::uint64_t size) {
    if (!state || state->consoleQueue == fail || state->pendingReadCount == 0) {
        return;
    }

    const std::uint64_t requestId = state->pendingReads[0].requestId;
    for (std::uint32_t i = 1; i < state->pendingReadCount; ++i) {
        state->pendingReads[i - 1] = state->pendingReads[i];
    }
    state->pendingReadCount--;
    std::queue_reply(state->consoleQueue, requestId, data, size);
}

void handle_input(TerminalState* state, char c, std::uint32_t columns) {
    if (!state || c == 0) {
        return;
    }

    if (state->pendingReadCount != 0) {
        if (tty_canonical(*state)) {
            const char erase = state->tty.c_cc[VERASE] ? static_cast<char>(state->tty.c_cc[VERASE]) : '\b';
            const char eof = state->tty.c_cc[VEOF] ? static_cast<char>(state->tty.c_cc[VEOF]) : 4;
            if (c == erase || c == '\b') {
                if (state->inputLength > 0) {
                    state->input[--state->inputLength] = '\0';
                }
                return;
            }
            if (c == eof) {
                if (state->inputLength == 0) {
                    complete_pending_read(state, nullptr, 0);
                    return;
                }
                commit_input_to_stdin(state, false);
                clear_input_line(state);
                return;
            }
            if (c == '\n') {
                if (tty_echo(*state) && state->inputLength != 0) {
                    append_wrapped_text(state, state->input, columns);
                }
                commit_input_to_stdin(state, true);
                clear_input_line(state);
                return;
            }
            if (c == '\t') {
                c = ' ';
            }
            if (static_cast<unsigned char>(c) < 32U || static_cast<unsigned char>(c) > 126U) {
                return;
            }
            if (state->inputLength + 1 < kMaxInputChars) {
                state->input[state->inputLength++] = c;
                state->input[state->inputLength] = '\0';
            }
        } else {
            if (state->tty.c_iflag & ICRNL) {
                if (c == '\r') {
                    c = '\n';
                }
            }
            stdin_push(state, c);
            if (tty_echo(*state)) {
                if (c == '\n') {
                    if (state->inputLength != 0) {
                        append_wrapped_text(state, state->input, columns);
                    }
                    clear_input_line(state);
                } else if (c == '\b') {
                    if (state->inputLength > 0) {
                        state->input[--state->inputLength] = '\0';
                    }
                } else if (static_cast<unsigned char>(c) >= 32U && static_cast<unsigned char>(c) <= 126U &&
                           state->inputLength + 1 < kMaxInputChars) {
                    state->input[state->inputLength++] = c;
                    state->input[state->inputLength] = '\0';
                }
            }
        }
        return;
    }

    if (c == '\b') {
        if (state->inputLength > 0) {
            state->input[--state->inputLength] = '\0';
        }
        return;
    }

    if (c == '\n') {
        execute_command(state, columns);
        return;
    }

    if (c == '\t') {
        c = ' ';
    }

    if (static_cast<unsigned char>(c) < 32U || static_cast<unsigned char>(c) > 126U) {
        return;
    }

    if (state->inputLength + 1 >= kMaxInputChars) {
        return;
    }

    state->input[state->inputLength++] = c;
    state->input[state->inputLength] = '\0';
}

void draw_terminal(std::uint32_t* pixels, UIFont& font, const TerminalState& state) {
    fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, 0, 0, kSurfaceWidth, kSurfaceHeight, kColorBackground);

    const std::uint32_t columns = static_cast<std::uint32_t>((kSurfaceWidth - (kPaddingX * 2)) / font.cellWidth);
    std::uint32_t historyRows = 0;
    if (font.lineHeight > 0) {
        const int usableHeight = static_cast<int>(kSurfaceHeight) - (kPaddingY * 2) - font.lineHeight - kPromptGap;
        if (usableHeight > 0) {
            historyRows = static_cast<std::uint32_t>(usableHeight / font.lineHeight);
        }
    }

    const std::uint32_t startIndex = state.historyCount > historyRows ? state.historyCount - historyRows : 0;
    int baselineY = kPaddingY + font.baseline;
    for (std::uint32_t index = startIndex; index < state.historyCount; ++index) {
        draw_text(pixels, kSurfaceWidth, kSurfaceHeight, font, kPaddingX, baselineY, state.history[index], kColorText);
        baselineY += font.lineHeight;
    }

    const int promptBaselineY = static_cast<int>(kSurfaceHeight) - kPaddingY;
    char prompt[64];
    build_prompt(state, prompt, sizeof(prompt));
    draw_text(pixels, kSurfaceWidth, kSurfaceHeight, font, kPaddingX, promptBaselineY, prompt, kColorPrompt);

    const std::uint32_t promptColumns = static_cast<std::uint32_t>(std::strlen(prompt));
    std::uint32_t visibleInputOffset = 0;
    const bool suppressEcho = state.pendingReadCount != 0 && !tty_echo(state);
    const std::uint32_t displayInputLength = suppressEcho ? 0 : state.inputLength;
    const char* displayInput = suppressEcho ? "" : state.input;
    if (columns > promptColumns + 1 && displayInputLength > (columns - promptColumns - 1)) {
        visibleInputOffset = displayInputLength - (columns - promptColumns - 1);
    }
    draw_text(
        pixels,
        kSurfaceWidth,
        kSurfaceHeight,
        font,
        kPaddingX + static_cast<int>(promptColumns * font.cellWidth),
        promptBaselineY,
        displayInput + visibleInputOffset,
        kColorText
    );

    if (state.focused && state.cursorVisible) {
        const std::uint32_t visibleInputLength = displayInputLength - visibleInputOffset;
        const int cursorX = kPaddingX + static_cast<int>((promptColumns + visibleInputLength) * font.cellWidth);
        const int cursorY = promptBaselineY - font.baseline;
        fill_rect(pixels, kSurfaceWidth, kSurfaceHeight, cursorX, cursorY, 2, font.lineHeight, kColorCursor);
    }
}

void initialize_terminal_state(TerminalState* state, std::uint32_t columns) {
    if (!state) {
        return;
    }

    std::memset(state, 0, sizeof(*state));
    state->running = true;
    state->cursorVisible = true;
    state->lastBlinkMs = std::gettime();
    capture_cwd(state);
    state->tty.c_iflag = ICRNL | IXON;
    state->tty.c_oflag = OPOST | ONLCR;
    state->tty.c_cflag = CS8 | CREAD;
    state->tty.c_lflag = ISIG | ICANON | ECHO | ECHOE;
    state->tty.c_cc[VINTR] = 3;
    state->tty.c_cc[VEOF] = 4;
    state->tty.c_cc[VERASE] = '\b';
    state->tty.c_cc[VKILL] = 21;
    state->tty.c_cc[VMIN] = 1;
    state->tty.c_cc[VTIME] = 0;
    state->tty.c_cc[VEOL] = '\n';
    state->tty.c_ispeed = 38400;
    state->tty.c_ospeed = 38400;
    state->foregroundPgid = 0;

    append_wrapped_text(state, "InstantOS Terminal", columns);
    append_wrapped_text(state, "Type 'help' to see available commands.", columns);
}
}

namespace {
inline constexpr std::uint8_t kConsoleOpWrite = 1;
inline constexpr std::uint8_t kConsoleOpRead = 2;
inline constexpr std::uint8_t kConsoleOpGetAttr = 3;
inline constexpr std::uint8_t kConsoleOpSetAttr = 4;
inline constexpr std::uint8_t kConsoleOpIsATTY = 5;
inline constexpr std::uint8_t kConsoleOpGetPgrp = 6;
inline constexpr std::uint8_t kConsoleOpSetPgrp = 7;
}

static void service_console_output(TerminalState* state, const std::uint8_t* bytes, std::uint64_t n, std::uint32_t columns) {
    if (!state || !bytes || n == 0) {
        return;
    }

    char line[kMaxLineChars];
    std::uint32_t pos = 0;

    for (std::uint64_t i = 0; i < n; ++i) {
        char c = static_cast<char>(bytes[i]);
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[pos] = '\0';
            append_wrapped_text(state, line, columns);
            pos = 0;
            continue;
        }

        if (static_cast<unsigned char>(c) < 32U || static_cast<unsigned char>(c) > 126U) {
            c = '?';
        }

        if (pos + 1 < sizeof(line)) {
            line[pos++] = c;
        }
    }

    if (pos != 0) {
        line[pos] = '\0';
        append_wrapped_text(state, line, columns);
    }
}

static std::uint32_t stdin_available(const TerminalState& state) {
    if (state.stdinHead == state.stdinTail) {
        return 0;
    }
    if (state.stdinTail > state.stdinHead) {
        return state.stdinTail - state.stdinHead;
    }
    return static_cast<std::uint32_t>(sizeof(state.stdinBuf)) - state.stdinHead + state.stdinTail;
}

static std::uint32_t stdin_pop(TerminalState& state, std::uint8_t* out, std::uint32_t maxBytes) {
    std::uint32_t copied = 0;
    while (copied < maxBytes && state.stdinHead != state.stdinTail) {
        out[copied++] = static_cast<std::uint8_t>(state.stdinBuf[state.stdinHead]);
        state.stdinHead = (state.stdinHead + 1U) % static_cast<std::uint32_t>(sizeof(state.stdinBuf));
    }
    return copied;
}

static void try_satisfy_pending_reads(TerminalState* state, std::uint32_t columns) {
    (void)columns;
    if (!state || state->consoleQueue == fail) {
        return;
    }

    while (state->pendingReadCount != 0 && stdin_available(*state) != 0) {
        TerminalState::PendingRead pr = state->pendingReads[0];
        for (std::uint32_t i = 1; i < state->pendingReadCount; ++i) {
            state->pendingReads[i - 1] = state->pendingReads[i];
        }
        state->pendingReadCount--;

        std::uint8_t reply[256];
        const std::uint32_t want = pr.count > sizeof(reply) ? static_cast<std::uint32_t>(sizeof(reply)) : static_cast<std::uint32_t>(pr.count);
        const std::uint32_t n = stdin_pop(*state, reply, want);
        std::queue_reply(state->consoleQueue, pr.requestId, reply, n);
    }
}

static void pump_console_queue(TerminalState* state, std::uint32_t columns, bool* redraw) {
    if (!state || state->consoleQueue == fail) {
        return;
    }

    for (;;) {
        std::IPCMessage msg = {};
        if (std::queue_receive(state->consoleQueue, &msg, false) == fail) {
            break;
        }

        if ((msg.flags & std::IPC_MESSAGE_REQUEST) != 0) {
            if (msg.size < 1) {
                continue;
            }
            const std::uint8_t op = msg.data[0];

            if (op == kConsoleOpGetAttr) {
                std::queue_reply(state->consoleQueue, msg.id, &state->tty, sizeof(state->tty));
                continue;
            }

            if (op == kConsoleOpSetAttr) {
                if (msg.size >= 1 + sizeof(state->tty)) {
                    std::memcpy(&state->tty, msg.data + 1, sizeof(state->tty));
                    if (state->tty.c_cc[VMIN] == 0) {
                        state->tty.c_cc[VMIN] = 1;
                    }
                    std::queue_reply(state->consoleQueue, msg.id, nullptr, 0);
                }
                continue;
            }

            if (op == kConsoleOpIsATTY) {
                const std::uint8_t ok = 1;
                std::queue_reply(state->consoleQueue, msg.id, &ok, sizeof(ok));
                continue;
            }

            if (op == kConsoleOpGetPgrp) {
                std::queue_reply(state->consoleQueue, msg.id, &state->foregroundPgid, sizeof(state->foregroundPgid));
                continue;
            }

            if (op == kConsoleOpSetPgrp) {
                if (msg.size >= 1 + sizeof(state->foregroundPgid)) {
                    std::memcpy(&state->foregroundPgid, msg.data + 1, sizeof(state->foregroundPgid));
                    std::queue_reply(state->consoleQueue, msg.id, nullptr, 0);
                }
                continue;
            }

            if (op == kConsoleOpRead) {
                std::uint64_t want = 0;
                if (msg.size >= 1 + sizeof(want)) {
                    std::memcpy(&want, msg.data + 1, sizeof(want));
                }

                if (want == 0) {
                    std::queue_reply(state->consoleQueue, msg.id, nullptr, 0);
                    continue;
                }

                if (state->pendingReadCount < (sizeof(state->pendingReads) / sizeof(state->pendingReads[0]))) {
                    state->pendingReads[state->pendingReadCount++] = { msg.id, want };
                }
                try_satisfy_pending_reads(state, columns);
            }
            continue;
        }

        if (msg.size < 2) {
            continue;
        }
        if (msg.data[0] != kConsoleOpWrite) {
            continue;
        }

        service_console_output(state, msg.data + 2, msg.size - 2, columns);
        if (redraw) {
            *redraw = true;
        }
    }
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    const std::Handle compositor = connect_service(std::services::graphics_compositor::NAME);
    if (compositor == fail) {
        write_str("[terminal] FAIL service_connect graphics.compositor\n");
        return 1;
    }

    const std::Handle surface = std::surface_create(
        kSurfaceWidth,
        kSurfaceHeight,
        std::services::surfaces::FORMAT_BGRA8
    );
    if (surface == fail) {
        write_str("[terminal] FAIL surface_create\n");
        std::close(compositor);
        return 1;
    }

    auto* pixels = static_cast<std::uint32_t*>(std::shared_map(surface));
    if (pixels == reinterpret_cast<std::uint32_t*>(fail) || pixels == nullptr) {
        write_str("[terminal] FAIL shared_map(surface)\n");
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    const std::Handle window = std::compositor_create_window(compositor, kSurfaceWidth, kSurfaceHeight, 0);
    if (window == fail) {
        write_str("[terminal] FAIL compositor_create_window\n");
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    if (std::window_set_title(window, "Terminal") == fail ||
        std::window_attach_surface(window, surface) == fail) {
        write_str("[terminal] FAIL window setup\n");
        std::close(window);
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    const std::Handle events = std::window_event_queue(window);
    if (events == fail) {
        write_str("[terminal] FAIL window_event_queue\n");
        std::close(window);
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    UIFont font = {};
    if (!initialize_font(&font)) {
        write_str("[terminal] FAIL font init\n");
        std::close(events);
        std::close(window);
        std::close(surface);
        std::close(compositor);
        return 1;
    }

    const std::uint32_t columns = static_cast<std::uint32_t>((kSurfaceWidth - (kPaddingX * 2)) / font.cellWidth);
    TerminalState& terminal = gTerminalState;
    initialize_terminal_state(&terminal, columns);

    terminal.consoleQueue = std::queue_create();
    if (terminal.consoleQueue != fail) {
        if (std::service_register("terminal.console", terminal.consoleQueue) == fail) {
            std::close(terminal.consoleQueue);
            terminal.consoleQueue = fail;
        }
    }

    bool redraw = true;
    while (terminal.running) {
        pump_console_queue(&terminal, columns, &redraw);

        for (;;) {
            std::Event event = {};
            if (std::event_poll(events, &event) == fail) {
                break;
            }

            if (event.type == std::EventType::Window) {
                if (event.window.action == std::WindowEventAction::FocusGained) {
                    terminal.focused = true;
                    redraw = true;
                } else if (event.window.action == std::WindowEventAction::FocusLost) {
                    terminal.focused = false;
                    redraw = true;
                } else if (event.window.action == std::WindowEventAction::CloseRequested) {
                    terminal.running = false;
                    break;
                }
            } else {
                const char c = translate_key(event);
                if (c != 0) {
                    handle_input(&terminal, c, columns);
                    try_satisfy_pending_reads(&terminal, columns);
                    capture_cwd(&terminal);
                    terminal.cursorVisible = true;
                    terminal.lastBlinkMs = std::gettime();
                    redraw = true;
                }
            }
        }

        const std::uint64_t now = std::gettime();
        if (now - terminal.lastBlinkMs >= kCursorBlinkMs) {
            terminal.cursorVisible = !terminal.cursorVisible;
            terminal.lastBlinkMs = now;
            redraw = true;
        }

        if (redraw) {
            draw_terminal(pixels, font, terminal);
            if (std::surface_commit(surface, 0, 0, kSurfaceWidth, kSurfaceHeight) == fail) {
                write_str("[terminal] FAIL surface_commit\n");
                break;
            }
            redraw = false;
        }

        std::sleep(kFrameIntervalMs);
    }

    destroy_font(&font);
    if (terminal.consoleQueue != fail) {
        std::close(terminal.consoleQueue);
    }
    std::close(events);
    std::close(window);
    std::close(surface);
    std::close(compositor);
    return 0;
}
