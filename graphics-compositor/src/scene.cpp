#include "common.hpp"

Rect taskbar_rect_at(const RenderBuffer& buffer, int index) {
    const int screenWidth = static_cast<int>(buffer.width);
    const int screenHeight = static_cast<int>(buffer.height);
    const int y = screenHeight - kTaskbarBottomMargin - kTaskbarHeight;

    if (index == 0) {
        return { kTaskbarMarginX, y, kTaskbarWidth, kTaskbarHeight };
    }
    if (index == 1) {
        return { (screenWidth - kTaskbarCenterWidth) / 2, y, kTaskbarCenterWidth, kTaskbarHeight };
    }
    return { screenWidth - kTaskbarMarginX - kTaskbarWidth, y, kTaskbarWidth, kTaskbarHeight };
}

void draw_image_scaled(const RenderBuffer& buffer, const ImageAsset& image, int x, int y, int width, int height) {
    if (!image.ready || !image.pixels || width <= 0 || height <= 0) {
        return;
    }

    const auto* pixels = reinterpret_cast<const std::Resizer::RGBA*>(image.pixels);
    for (int drawY = 0; drawY < height; ++drawY) {
        for (int drawX = 0; drawX < width; ++drawX) {
            const std::Resizer::RGBA resized = std::Resizer::sample(
                pixels,
                image.width,
                image.height,
                width,
                height,
                drawX,
                drawY
            );
            const RGBATexel src = {
                resized.b,
                resized.g,
                resized.r,
                resized.a
            };
            blend_pixel(buffer, x + drawX, y + drawY, src, src.a);
        }
    }
}

void draw_taskbar(const RenderBuffer& buffer, const ImageAsset* brandImage, const TaskbarStatusAssets* statusIcons) {
    const RGBATexel color = rgba_from_rgb(kTaskbarColor);
    for (int index = 0; index < 3; ++index) {
        const Rect rect = taskbar_rect_at(buffer, index);
        fill_rounded_rect(buffer, rect.x, rect.y, rect.width, rect.height, kTaskbarCornerRadius, color);
    }

    if (brandImage && brandImage->ready) {
        const Rect leftRect = taskbar_rect_at(buffer, 0);
        draw_image_scaled(buffer,
                          *brandImage,
                          leftRect.x + kTaskbarBrandOffsetX,
                          leftRect.y + ((leftRect.height - kTaskbarBrandHeight) / 2),
                          kTaskbarBrandWidth,
                          kTaskbarBrandHeight);
    }

    const Rect rightRect = taskbar_rect_at(buffer, 2);
    if (statusIcons) {
        const int iconY = rightRect.y + ((rightRect.height - kTaskbarStatusIconSize) / 2);
        if (statusIcons->noInternet.ready) {
            draw_image_scaled(buffer,
                              statusIcons->noInternet,
                              rightRect.x + kTaskbarStatusNoInternetOffsetX,
                              iconY,
                              kTaskbarStatusIconSize,
                              kTaskbarStatusIconSize);
        }
        if (statusIcons->noSound.ready) {
            draw_image_scaled(buffer,
                              statusIcons->noSound,
                              rightRect.x + kTaskbarStatusNoSoundOffsetX,
                              iconY,
                              kTaskbarStatusIconSize,
                              kTaskbarStatusIconSize);
        }
    }

    const std::Time::DateTime now = std::Time::now();
    const std::Time::TimeString timeText = std::Time::time_string(now);
    const std::Time::DateString dateText = std::Time::date_string(now);
    const RGBATexel textColor = rgba_from_rgb(kTitleText);
    UIFont& timeFont = gClockTimeFont.valid ? gClockTimeFont : gUIFont;
    UIFont& dateFont = gClockDateFont.valid ? gClockDateFont : gUIFont;
    const int timeWidth = text_width(timeFont, timeText.value);
    const int dateWidth = text_width(dateFont, dateText.value);
    draw_text(buffer,
              timeFont,
              rightRect.x + rightRect.width - kTaskbarClockRightPadding - timeWidth,
              rightRect.y + kTaskbarClockTimeBaselineOffsetY,
              timeText.value,
              textColor);
    draw_text(buffer,
              dateFont,
              rightRect.x + rightRect.width - kTaskbarClockRightPadding - dateWidth,
              rightRect.y + kTaskbarClockDateBaselineOffsetY,
              dateText.value,
              textColor);
}

void redraw_scene(const RenderBuffer& buffer, SurfaceCacheEntry* cache, const DesktopBackground* background,
                  const ImageAsset* brandImage, const TaskbarStatusAssets* statusIcons,
                  const WindowControlAssets* controls) {
    const std::uint64_t redrawStart = std::gettime();
    std::uint64_t phaseStart = std::gettime();
    draw_background(buffer, background);
    add_elapsed(phaseStart, &gTiming.backgroundMs);

    std::memset(gWindowsScratch, 0, sizeof(gWindowsScratch));
    const std::uint64_t count = fetch_windows(gWindowsScratch, kMaxWindows);
    for (std::uint64_t i = 0; i < count; ++i) {
        if (!is_window_visible(gWindowsScratch[i])) {
            continue;
        }

        phaseStart = std::gettime();
        draw_window_frame(buffer, gWindowsScratch[i], controls);
        add_elapsed(phaseStart, &gTiming.windowFrameMs);
        phaseStart = std::gettime();
        blit_window_surface(buffer, gWindowsScratch[i], find_surface_cache(cache, gWindowsScratch[i].surfaceID));
        add_elapsed(phaseStart, &gTiming.surfaceBlitMs);
    }

    draw_taskbar(buffer, brandImage, statusIcons);
    add_elapsed(redrawStart, &gTiming.sceneRedrawMs);
}

void redraw_scene_rect(const RenderBuffer& buffer, SurfaceCacheEntry* cache, const DesktopBackground* background,
                       const Rect& dirty, const ImageAsset* brandImage, const TaskbarStatusAssets* statusIcons,
                       const WindowControlAssets* controls) {
    const Rect clipped = clamp_rect(dirty, buffer.width, buffer.height);
    if (rect_is_empty(clipped)) {
        return;
    }

    const std::uint64_t redrawStart = std::gettime();
    std::uint64_t phaseStart = std::gettime();
    draw_background_rect(buffer, background, clipped);
    add_elapsed(phaseStart, &gTiming.backgroundMs);
    gDrawClip = clipped;
    gDrawClipEnabled = true;

    std::memset(gWindowsScratch, 0, sizeof(gWindowsScratch));
    const std::uint64_t count = fetch_windows(gWindowsScratch, kMaxWindows);
    for (std::uint64_t i = 0; i < count; ++i) {
        if (!is_window_visible(gWindowsScratch[i])) {
            continue;
        }

        const Rect frame = { gWindowsScratch[i].x, gWindowsScratch[i].y,
            frame_width(gWindowsScratch[i]), frame_height(gWindowsScratch[i]) };
        if (rect_is_empty(intersect_rect(frame, clipped))) {
            continue;
        }
        phaseStart = std::gettime();
        draw_window_frame(buffer, gWindowsScratch[i], controls);
        add_elapsed(phaseStart, &gTiming.windowFrameMs);
        phaseStart = std::gettime();
        blit_window_surface(buffer, gWindowsScratch[i], find_surface_cache(cache, gWindowsScratch[i].surfaceID));
        add_elapsed(phaseStart, &gTiming.surfaceBlitMs);
    }

    draw_taskbar(buffer, brandImage, statusIcons);

    gDrawClipEnabled = false;
    add_elapsed(redrawStart, &gTiming.sceneRedrawMs);
}
