#include <cmath.hpp>
#include <cstdint.hpp>
#include <cstring.hpp>
#include <fcntl.h>
#include <math.h>
#include <new.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>

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

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr char kFontPath[] = "/bin/JetBrainsMono-Regular.ttf";
constexpr char kFontFamily[] = "JetBrains Mono";

struct LoadedFont {
    bool valid;
    unsigned char* data;
    std::size_t size;
    stbtt_fontinfo info;
    int ascent;
    int descent;
    int lineGap;
};

void write_str(const char* s) {
    std::serial_write(s, std::strlen(s));
}

void write_service_line(const char* suffix) {
    write_str("[font.manager] ");
    write_str(suffix);
}

std::uint32_t normalize_pixel_height(std::uint32_t pixelHeight) {
    if (pixelHeight == 0) {
        return std::services::font_manager::DEFAULT_PIXEL_HEIGHT;
    }
    if (pixelHeight < 6) {
        return 6;
    }
    if (pixelHeight > 72) {
        return 72;
    }
    return pixelHeight;
}

bool load_font(LoadedFont* font) {
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
            std::memset(font, 0, sizeof(*font));
            std::close(file);
            return false;
        }
        totalRead += static_cast<std::size_t>(bytesRead);
    }
    std::close(file);

    const int offset = stbtt_GetFontOffsetForIndex(font->data, 0);
    if (offset < 0 || stbtt_InitFont(&font->info, font->data, offset) == 0) {
        delete[] font->data;
        std::memset(font, 0, sizeof(*font));
        return false;
    }

    stbtt_GetFontVMetrics(&font->info, &font->ascent, &font->descent, &font->lineGap);
    font->size = fontSize;
    font->valid = true;
    return true;
}

bool validate_header(const std::services::MessageHeader& header, std::services::font_manager::Opcode opcode, std::uint16_t* status) {
    if (!status) {
        return false;
    }

    *status = std::services::STATUS_OK;
    if (header.version != std::services::font_manager::VERSION) {
        *status = std::services::STATUS_BAD_VERSION;
        return false;
    }
    if (header.opcode != static_cast<std::uint16_t>(opcode)) {
        *status = std::services::STATUS_BAD_OPCODE;
        return false;
    }
    return true;
}

float scale_for_height(const LoadedFont& font, std::uint32_t pixelHeight) {
    return stbtt_ScaleForPixelHeight(&font.info, static_cast<float>(normalize_pixel_height(pixelHeight)));
}

int scaled_ceil(int value, float scale) {
    return static_cast<int>(value * scale + 0.999f);
}

void fill_common_header(std::services::MessageHeader* header, std::services::font_manager::Opcode opcode) {
    header->version = std::services::font_manager::VERSION;
    header->opcode = static_cast<std::uint16_t>(opcode);
    header->reserved = 0;
}

bool build_hello_reply(const std::IPCMessage& message, std::services::font_manager::HelloReply* reply) {
    if (!reply) {
        return false;
    }

    std::services::font_manager::HelloRequest request = {};
    if (!std::services::decode_message(message, &request)) {
        return false;
    }

    std::memset(reply, 0, sizeof(*reply));
    fill_common_header(&reply->header, std::services::font_manager::Opcode::Hello);
    validate_header(request.header, std::services::font_manager::Opcode::Hello, &reply->status);
    std::strncpy(reply->service_name, std::services::font_manager::NAME, sizeof(reply->service_name) - 1);
    return true;
}

bool build_font_info_reply(const LoadedFont& font, const std::IPCMessage& message, std::services::font_manager::FontInfoReply* reply) {
    if (!reply) {
        return false;
    }

    std::services::font_manager::FontInfoRequest request = {};
    if (!std::services::decode_message(message, &request)) {
        return false;
    }

    std::memset(reply, 0, sizeof(*reply));
    fill_common_header(&reply->header, std::services::font_manager::Opcode::GetFontInfo);
    if (!validate_header(request.header, std::services::font_manager::Opcode::GetFontInfo, &reply->status)) {
        return true;
    }
    if (!font.valid) {
        reply->status = std::services::STATUS_BAD_PAYLOAD;
        return true;
    }

    const std::uint32_t pixelHeight = normalize_pixel_height(request.pixelHeight);
    const float scale = scale_for_height(font, pixelHeight);
    int advance = 0;
    int leftSideBearing = 0;
    stbtt_GetCodepointHMetrics(&font.info, 'M', &advance, &leftSideBearing);
    (void) leftSideBearing;

    reply->pixelHeight = pixelHeight;
    reply->ascent = font.ascent;
    reply->descent = font.descent;
    reply->lineGap = font.lineGap;
    reply->baseline = scaled_ceil(font.ascent, scale);
    reply->lineHeight = scaled_ceil(font.ascent - font.descent + font.lineGap, scale);
    if (reply->lineHeight <= 0) {
        reply->lineHeight = static_cast<int>(pixelHeight) + 4;
    }
    reply->cellWidth = scaled_ceil(advance, scale);
    if (reply->cellWidth <= 0) {
        reply->cellWidth = 8;
    }
    std::strncpy(reply->family, kFontFamily, sizeof(reply->family) - 1);
    return true;
}

bool build_glyph_metrics_reply(const LoadedFont& font, const std::IPCMessage& message, std::services::font_manager::GlyphMetricsReply* reply) {
    if (!reply) {
        return false;
    }

    std::services::font_manager::GlyphMetricsRequest request = {};
    if (!std::services::decode_message(message, &request)) {
        return false;
    }

    std::memset(reply, 0, sizeof(*reply));
    fill_common_header(&reply->header, std::services::font_manager::Opcode::GetGlyphMetrics);
    reply->codepoint = request.codepoint;
    if (!validate_header(request.header, std::services::font_manager::Opcode::GetGlyphMetrics, &reply->status)) {
        return true;
    }
    if (!font.valid) {
        reply->status = std::services::STATUS_BAD_PAYLOAD;
        return true;
    }

    const std::uint32_t pixelHeight = normalize_pixel_height(request.pixelHeight);
    const float scale = scale_for_height(font, pixelHeight);
    int advance = 0;
    int leftSideBearing = 0;
    stbtt_GetCodepointHMetrics(&font.info, static_cast<int>(request.codepoint), &advance, &leftSideBearing);
    (void) leftSideBearing;
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetCodepointBitmapBox(&font.info, static_cast<int>(request.codepoint), scale, scale, &x0, &y0, &x1, &y1);
    reply->pixelHeight = pixelHeight;
    reply->width = x1 - x0;
    reply->height = y1 - y0;
    reply->xOffset = x0;
    reply->yOffset = y0;
    reply->advance = scaled_ceil(advance, scale);
    return true;
}

bool build_glyph_row_reply(const LoadedFont& font, const std::IPCMessage& message, std::services::font_manager::GlyphRowReply* reply) {
    if (!reply) {
        return false;
    }

    std::services::font_manager::GlyphRowRequest request = {};
    if (!std::services::decode_message(message, &request)) {
        return false;
    }

    std::memset(reply, 0, sizeof(*reply));
    fill_common_header(&reply->header, std::services::font_manager::Opcode::GetGlyphRow);
    reply->codepoint = request.codepoint;
    reply->row = request.row;
    if (!validate_header(request.header, std::services::font_manager::Opcode::GetGlyphRow, &reply->status)) {
        return true;
    }
    if (!font.valid) {
        reply->status = std::services::STATUS_BAD_PAYLOAD;
        return true;
    }

    const std::uint32_t pixelHeight = normalize_pixel_height(request.pixelHeight);
    const float scale = scale_for_height(font, pixelHeight);
    int width = 0;
    int height = 0;
    int xOffset = 0;
    int yOffset = 0;
    unsigned char* bitmap = stbtt_GetCodepointBitmap(&font.info, scale, scale, static_cast<int>(request.codepoint),
                                                     &width, &height, &xOffset, &yOffset);
    (void) xOffset;
    (void) yOffset;

    reply->pixelHeight = pixelHeight;
    if (!bitmap || width < 0 || height < 0 ||
        width > static_cast<int>(std::services::font_manager::MAX_GLYPH_ROW_PIXELS) ||
        request.row >= static_cast<std::uint32_t>(height)) {
        reply->status = std::services::STATUS_BAD_PAYLOAD;
        if (bitmap) {
            stbtt_FreeBitmap(bitmap, nullptr);
        }
        return true;
    }

    reply->width = static_cast<std::uint32_t>(width);
    std::memcpy(reply->pixels, bitmap + (request.row * static_cast<std::uint32_t>(width)), static_cast<std::size_t>(width));
    stbtt_FreeBitmap(bitmap, nullptr);
    return true;
}
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    LoadedFont font = {};
    if (!load_font(&font)) {
        write_service_line("font load failed\n");
    }

    const std::Handle queue = std::queue_create();
    if (queue == fail) {
        write_service_line("queue_create failed\n");
        return 1;
    }

    if (std::service_register(std::services::font_manager::NAME, queue) == fail) {
        write_service_line("service_register failed\n");
        std::close(queue);
        return 1;
    }

    write_service_line(font.valid ? "ready\n" : "ready without font\n");

    for (;;) {
        std::IPCMessage message = {};
        if (std::queue_receive(queue, &message, true) == fail) {
            write_service_line("queue_receive failed\n");
            break;
        }

        if ((message.flags & std::IPC_MESSAGE_REQUEST) == 0 || message.size < sizeof(std::services::MessageHeader)) {
            continue;
        }

        std::services::MessageHeader header = {};
        std::memcpy(&header, message.data, sizeof(header));

        char reply[sizeof(std::services::font_manager::GlyphRowReply)] = {};
        std::uint64_t replySize = 0;
        bool built = false;
        const auto opcode = static_cast<std::services::font_manager::Opcode>(header.opcode);
        if (opcode == std::services::font_manager::Opcode::Hello) {
            built = build_hello_reply(message, reinterpret_cast<std::services::font_manager::HelloReply*>(reply));
            replySize = sizeof(std::services::font_manager::HelloReply);
        } else if (opcode == std::services::font_manager::Opcode::GetFontInfo) {
            built = build_font_info_reply(font, message, reinterpret_cast<std::services::font_manager::FontInfoReply*>(reply));
            replySize = sizeof(std::services::font_manager::FontInfoReply);
        } else if (opcode == std::services::font_manager::Opcode::GetGlyphMetrics) {
            built = build_glyph_metrics_reply(font, message, reinterpret_cast<std::services::font_manager::GlyphMetricsReply*>(reply));
            replySize = sizeof(std::services::font_manager::GlyphMetricsReply);
        } else if (opcode == std::services::font_manager::Opcode::GetGlyphRow) {
            built = build_glyph_row_reply(font, message, reinterpret_cast<std::services::font_manager::GlyphRowReply*>(reply));
            replySize = sizeof(std::services::font_manager::GlyphRowReply);
        } else {
            std::services::font_manager::HelloReply bad = {};
            fill_common_header(&bad.header, std::services::font_manager::Opcode::Hello);
            bad.status = std::services::STATUS_BAD_OPCODE;
            std::memcpy(reply, &bad, sizeof(bad));
            replySize = sizeof(bad);
            built = true;
        }

        if (!built || std::queue_reply(queue, message.id, reply, replySize) == fail) {
            write_service_line("queue_reply failed\n");
            break;
        }
    }

    delete[] font.data;
    std::close(queue);
    return 1;
}
