#include "common.hpp"

#define STBI_ASSERT(x) ((void)0)
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

CursorImage::CursorImage()
    : pixels(nullptr),
      width(0),
      height(0),
      hotspotX(0),
      hotspotY(0),
      ready(false) {
}

CursorImage::~CursorImage() {
    reset();
}

bool CursorImage::loadFromFile(const char* path) {
    if (!path || path[0] == '\0') {
        return false;
    }

    reset();

    std::Stat stat = {};
    if (std::stat(path, &stat) == fail || stat.st_size == 0) {
        return false;
    }

    const std::Handle file = std::open(path, O_RDONLY);
    if (file == fail) {
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
        return false;
    }

    int targetWidth = imageWidth;
    int targetHeight = imageHeight;
    if (targetWidth > kMaxCursorSize || targetHeight > kMaxCursorSize) {
        if (targetWidth >= targetHeight) {
            targetHeight = (targetHeight * kMaxCursorSize) / targetWidth;
            targetWidth = kMaxCursorSize;
        } else {
            targetWidth = (targetWidth * kMaxCursorSize) / targetHeight;
            targetHeight = kMaxCursorSize;
        }
        if (targetWidth < 1) {
            targetWidth = 1;
        }
        if (targetHeight < 1) {
            targetHeight = 1;
        }
    }

    pixels = new (std::nothrow) RGBATexel[static_cast<std::size_t>(targetWidth) * static_cast<std::size_t>(targetHeight)];
    if (!pixels) {
        stbi_image_free(imagePixels);
        return false;
    }

    for (int y = 0; y < targetHeight; ++y) {
        const double srcTop = (static_cast<double>(y) * static_cast<double>(imageHeight)) / static_cast<double>(targetHeight);
        const double srcBottom = (static_cast<double>(y + 1) * static_cast<double>(imageHeight)) / static_cast<double>(targetHeight);
        int startY = static_cast<int>(std::floor(srcTop));
        int endY = static_cast<int>(std::ceil(srcBottom));
        if (startY < 0) {
            startY = 0;
        }
        if (endY > imageHeight) {
            endY = imageHeight;
        }

        for (int x = 0; x < targetWidth; ++x) {
            const double srcLeft = (static_cast<double>(x) * static_cast<double>(imageWidth)) / static_cast<double>(targetWidth);
            const double srcRight = (static_cast<double>(x + 1) * static_cast<double>(imageWidth)) / static_cast<double>(targetWidth);
            int startX = static_cast<int>(std::floor(srcLeft));
            int endX = static_cast<int>(std::ceil(srcRight));
            if (startX < 0) {
                startX = 0;
            }
            if (endX > imageWidth) {
                endX = imageWidth;
            }

            double totalWeight = 0.0;
            double alphaSum = 0.0;
            double redSum = 0.0;
            double greenSum = 0.0;
            double blueSum = 0.0;

            for (int sampleY = startY; sampleY < endY; ++sampleY) {
                const double overlapTop = srcTop > static_cast<double>(sampleY) ? srcTop : static_cast<double>(sampleY);
                const double overlapBottom = srcBottom < static_cast<double>(sampleY + 1) ? srcBottom : static_cast<double>(sampleY + 1);
                const double yWeight = overlapBottom - overlapTop;
                if (yWeight <= 0.0) {
                    continue;
                }

                for (int sampleX = startX; sampleX < endX; ++sampleX) {
                    const double overlapLeft = srcLeft > static_cast<double>(sampleX) ? srcLeft : static_cast<double>(sampleX);
                    const double overlapRight = srcRight < static_cast<double>(sampleX + 1) ? srcRight : static_cast<double>(sampleX + 1);
                    const double xWeight = overlapRight - overlapLeft;
                    if (xWeight <= 0.0) {
                        continue;
                    }

                    const double weight = xWeight * yWeight;
                    const unsigned char* src = imagePixels + ((sampleY * imageWidth + sampleX) * 4);
                    const double alpha = (static_cast<double>(src[3]) / 255.0) * weight;
                    totalWeight += weight;
                    alphaSum += alpha;
                    redSum += static_cast<double>(src[0]) * alpha;
                    greenSum += static_cast<double>(src[1]) * alpha;
                    blueSum += static_cast<double>(src[2]) * alpha;
                }
            }

            const double outAlpha = totalWeight > 0.0 ? (alphaSum / totalWeight) : 0.0;
            RGBATexel& dst = pixels[y * targetWidth + x];
            if (outAlpha <= 0.0) {
                dst = {};
                continue;
            }

            dst.r = static_cast<std::uint8_t>((redSum / alphaSum) + 0.5);
            dst.g = static_cast<std::uint8_t>((greenSum / alphaSum) + 0.5);
            dst.b = static_cast<std::uint8_t>((blueSum / alphaSum) + 0.5);
            dst.a = static_cast<std::uint8_t>((outAlpha * 255.0) + 0.5);
        }
    }

    stbi_image_free(imagePixels);
    width = targetWidth;
    height = targetHeight;
    hotspotX = 0;
    hotspotY = 0;
    ready = true;
    return true;
}

void CursorImage::reset() {
    delete[] pixels;
    pixels = nullptr;
    width = 0;
    height = 0;
    hotspotX = 0;
    hotspotY = 0;
    ready = false;
}

ImageAsset::ImageAsset()
    : pixels(nullptr),
      width(0),
      height(0),
      ready(false) {
}

ImageAsset::~ImageAsset() {
    reset();
}

bool ImageAsset::loadFromFile(const char* path) {
    if (!path || path[0] == '\0') {
        return false;
    }

    reset();

    std::Stat stat = {};
    if (std::stat(path, &stat) == fail || stat.st_size == 0) {
        return false;
    }

    const std::Handle file = std::open(path, O_RDONLY);
    if (file == fail) {
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
        return false;
    }

    pixels = new (std::nothrow) RGBATexel[static_cast<std::size_t>(imageWidth) * static_cast<std::size_t>(imageHeight)];
    if (!pixels) {
        stbi_image_free(imagePixels);
        return false;
    }

    for (int y = 0; y < imageHeight; ++y) {
        for (int x = 0; x < imageWidth; ++x) {
            const unsigned char* src = imagePixels + ((y * imageWidth + x) * 4);
            RGBATexel& dst = pixels[y * imageWidth + x];
            dst.r = src[0];
            dst.g = src[1];
            dst.b = src[2];
            dst.a = src[3];
        }
    }

    stbi_image_free(imagePixels);
    width = imageWidth;
    height = imageHeight;
    ready = true;
    return true;
}

bool ImageAsset::resizeTo(int targetWidth, int targetHeight) {
    if (!ready || !pixels || targetWidth <= 0 || targetHeight <= 0) {
        return false;
    }
    if (width == targetWidth && height == targetHeight) {
        return true;
    }

    RGBATexel* resized = new (std::nothrow) RGBATexel[static_cast<std::size_t>(targetWidth) * static_cast<std::size_t>(targetHeight)];
    if (!resized) {
        return false;
    }

    const bool ok = std::Resizer::resize(
        reinterpret_cast<const std::Resizer::RGBA*>(pixels),
        width,
        height,
        reinterpret_cast<std::Resizer::RGBA*>(resized),
        targetWidth,
        targetHeight,
        targetWidth
    );
    if (!ok) {
        delete[] resized;
        return false;
    }

    delete[] pixels;
    pixels = resized;
    width = targetWidth;
    height = targetHeight;
    return true;
}

void ImageAsset::reset() {
    delete[] pixels;
    pixels = nullptr;
    width = 0;
    height = 0;
    ready = false;
}

bool TaskbarStatusAssets::load() {
    bool ok = true;
    if (!noInternet.loadFromFile(kTaskbarNoInternetIconPath)) {
        write_str("[graphics.compositor] no internet icon load failed\n");
        ok = false;
    } else if (!noInternet.resizeTo(kTaskbarStatusIconSize, kTaskbarStatusIconSize)) {
        write_str("[graphics.compositor] no internet icon resize failed\n");
        ok = false;
    }

    if (!noSound.loadFromFile(kTaskbarNoSoundIconPath)) {
        write_str("[graphics.compositor] no sound icon load failed\n");
        ok = false;
    } else if (!noSound.resizeTo(kTaskbarStatusIconSize, kTaskbarStatusIconSize)) {
        write_str("[graphics.compositor] no sound icon resize failed\n");
        ok = false;
    }

    return ok;
}

bool WindowControlAssets::load() {
    bool ok = true;
    const int iconSize = kControlIconSize + 6;
    if (!close.loadFromFile(kWindowCloseIconPath)) {
        write_str("[graphics.compositor] close icon load failed\n");
        ok = false;
    } else if (!close.resizeTo(iconSize, iconSize)) {
        write_str("[graphics.compositor] close icon resize failed\n");
        ok = false;
    }

    if (!minimize.loadFromFile(kWindowMinimizeIconPath)) {
        write_str("[graphics.compositor] minimize icon load failed\n");
        ok = false;
    } else if (!minimize.resizeTo(iconSize, iconSize)) {
        write_str("[graphics.compositor] minimize icon resize failed\n");
        ok = false;
    }

    if (!resize.loadFromFile(kWindowResizeIconPath)) {
        write_str("[graphics.compositor] resize icon load failed\n");
        ok = false;
    } else if (!resize.resizeTo(iconSize, iconSize)) {
        write_str("[graphics.compositor] resize icon resize failed\n");
        ok = false;
    }

    return ok;
}
