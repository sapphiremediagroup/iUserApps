#pragma once

#include <cstdint.hpp>
#include <cstring.hpp>
#include <cmath.hpp>
#include <anti_aliasing.hpp>
#include <resizer.hpp>
#include <emmintrin.h>
#include <fcntl.h>
#include <math.h>
#include <new.hpp>
#include <service_protocol.hpp>
#include <syscall.hpp>
#include <time.hpp>

#ifdef NULL
#undef NULL
#endif
#define NULL 0

#include "font_types.hpp"

constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr std::uint32_t kFrameIntervalMs = 16;
constexpr std::uint32_t kCompositorBufferCount = 3;
constexpr std::uint32_t kBackground = 0x00101820;
constexpr std::uint32_t kBackgroundBottom = 0x001d2b3a;
constexpr std::uint32_t kWindowBackground = 0x00f3f5f7;
constexpr std::uint32_t kWindowBorder = 0x00191c20;
constexpr std::uint32_t kWindowBorderFocused = 0x002a3038;
constexpr std::uint32_t kTitleBar = 0x00101010;
constexpr std::uint32_t kTitleBarFocused = 0x00101010;
constexpr std::uint32_t kTitleBarHighlight = 0x00222428;
constexpr std::uint32_t kButtonClose = 0x00ff5f57;
constexpr std::uint32_t kButtonMin = 0x00ffbd2e;
constexpr std::uint32_t kButtonMax = 0x0028c840;
constexpr std::uint32_t kResizeGrip = 0x00556777;
constexpr std::uint32_t kTitleText = 0x00f2f5f7;
constexpr std::uint32_t kTaskbarColor = 0x00101010;
constexpr int kBorder = 1;
constexpr int kTitleBarHeight = 34;
constexpr int kWindowCornerRadius = 18;
constexpr int kButtonSize = 28;
constexpr int kButtonGap = 4;
constexpr int kButtonMarginLeft = 14;
constexpr int kControlIconSize = 10;
constexpr int kResizeGripSize = 14;
constexpr int kTaskbarWidth = 230;
constexpr int kTaskbarCenterWidth = 378;
constexpr int kTaskbarCenterSlotCount = 4;
constexpr int kTaskbarHeight = 66;
constexpr int kTaskbarMarginX = 30;
constexpr int kTaskbarBottomMargin = 25;
constexpr int kTaskbarCornerRadius = 25;
constexpr int kTaskbarBrandOffsetX = 32;
constexpr int kTaskbarBrandWidth = 123;
constexpr int kTaskbarBrandHeight = 34;
constexpr int kTaskbarStatusIconSize = 32;
constexpr int kTaskbarStatusNoInternetOffsetX = 18;
constexpr int kTaskbarStatusNoSoundOffsetX = 62;
constexpr int kTaskbarClockRightPadding = 24;
constexpr int kTaskbarClockTimeBaselineOffsetY = 5 + 25;
constexpr int kTaskbarClockDateBaselineOffsetY = 36 + 15;
constexpr std::uint64_t kMaxWindows = 64;
constexpr std::uint64_t kMaxSurfaceCache = 64;
constexpr char kUIFontPath[] = "/bin/JetBrainsMono-Regular.ttf";
constexpr char kClockTimeFontPath[] = "/bin/StackSansNotch-Regular.ttf";
constexpr char kClockDateFontPath[] = "/bin/StackSansNotch-ExtraLight.ttf";
constexpr char kBackgroundDirectory[] = "/bin/backgrounds";
constexpr char kDefaultBackground[] = "/bin/backgrounds/instantos_2.png";
constexpr char kDefaultCursorPath[] = "/bin/cursors/default.png";
constexpr char kTaskbarBrandPath[] = "/bin/images/InstantOS_sapph.png";
constexpr char kTaskbarNoInternetIconPath[] = "/bin/images/no_itnernet.png";
constexpr char kTaskbarNoSoundIconPath[] = "/bin/images/no_sound.png";
constexpr char kWindowCloseIconPath[] = "/bin/images/close.png";
constexpr char kWindowMinimizeIconPath[] = "/bin/images/minimize.png";
constexpr char kWindowResizeIconPath[] = "/bin/images/resize.png";
constexpr int kMaxCursorSize = 24;
constexpr std::uint32_t kUIFontPixelHeight = 13;
constexpr std::uint32_t kClockTimeFontPixelHeight = 26;
constexpr std::uint32_t kClockDateFontPixelHeight = 18;
constexpr int kFontSupersampleScale = 2;

struct CachedGlyph {
    std::uint8_t* alpha;
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
    int lineHeight;
    int baseline;
    CachedGlyph glyphs[95];
};

struct CursorState {
    int x;
    int y;
};

struct Rect {
    int x;
    int y;
    int width;
    int height;
};

struct DragState {
    std::uint64_t windowId;
    bool moving;
    bool resizing;
    int grabOffsetX;
    int grabOffsetY;
    int anchorX;
    int anchorY;
    int startWidth;
    int startHeight;
};

struct FramebufferView {
    std::uint32_t* pixels;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pitch;
};

struct alignas(std::uint32_t) RGBATexel {
    std::uint8_t b;
    std::uint8_t g;
    std::uint8_t r;
    std::uint8_t a;
};

struct CursorImage {
    RGBATexel* pixels;
    int width;
    int height;
    int hotspotX;
    int hotspotY;
    bool ready;

    CursorImage();
    CursorImage(const CursorImage&) = delete;
    CursorImage& operator=(const CursorImage&) = delete;
    ~CursorImage();

    bool loadFromFile(const char* path);
    void reset();
};

struct ImageAsset {
    RGBATexel* pixels;
    int width;
    int height;
    bool ready;

    ImageAsset();
    ImageAsset(const ImageAsset&) = delete;
    ImageAsset& operator=(const ImageAsset&) = delete;
    ~ImageAsset();

    bool loadFromFile(const char* path);
    bool resizeTo(int width, int height);
    void reset();
};

struct WindowControlAssets {
    ImageAsset close;
    ImageAsset minimize;
    ImageAsset resize;

    bool load();
};

struct TaskbarStatusAssets {
    ImageAsset noInternet;
    ImageAsset noSound;

    bool load();
};

struct RenderBuffer {
    std::uint32_t* pixels;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pitch;
};

void draw_image_scaled(const RenderBuffer& buffer, const ImageAsset& image, int x, int y, int width, int height);

struct TripleBufferState {
    RenderBuffer buffers[kCompositorBufferCount];
    std::uint32_t nextIndex;

    TripleBufferState();
    TripleBufferState(const TripleBufferState&) = delete;
    TripleBufferState& operator=(const TripleBufferState&) = delete;
    ~TripleBufferState();

    bool initialize(std::uint32_t width, std::uint32_t height);
    RenderBuffer& acquire();
    void reset();
};

struct SurfaceCacheEntry {
    bool valid;
    std::uint64_t id;
    std::SurfaceInfo info;
    bool dirty;
    std::uint32_t dirtyX;
    std::uint32_t dirtyY;
    std::uint32_t dirtyWidth;
    std::uint32_t dirtyHeight;
};

struct DesktopBackground {
    std::uint32_t* pixels;
    std::uint32_t width;
    std::uint32_t height;
    bool ready;
    char currentPath[192];
};

struct CompositorTimingCounters {
    std::uint64_t backgroundMs;
    std::uint64_t windowFrameMs;
    std::uint64_t surfaceBlitMs;
    std::uint64_t sceneRedrawMs;
    std::uint64_t presentMs;
    std::uint64_t frames;
};

extern std::WindowInfo gWindowsScratch[kMaxWindows];
extern SurfaceCacheEntry gSurfaceCache[kMaxSurfaceCache];
extern UIFont gUIFont;
extern UIFont gClockTimeFont;
extern UIFont gClockDateFont;
extern Rect gDrawClip;
extern bool gDrawClipEnabled;
extern CompositorTimingCounters gTiming;

using TexelWord = std::uint32_t __attribute__((may_alias));

void write_str(const char* s);
void write_u64(std::uint64_t value);
void add_elapsed(std::uint64_t startMs, std::uint64_t* counter);
void report_timing_if_needed();
bool launch_file_browser();
bool launch_cube();
bool launch_background_switcher();
bool launch_terminal();
bool decode_event_message(const std::IPCMessage& message, std::Event* event);
bool has_png_suffix(const char* name);
void append_text(char* buffer, std::size_t capacity, const char* text);

RGBATexel rgba_from_rgb(std::uint32_t color);
std::uint32_t rgb_to_screen(const RGBATexel& pixel);
std::uint32_t rgb_to_texel_word(std::uint32_t color);
std::uint32_t texel_to_word(const RGBATexel& pixel);
RGBATexel texel_word_to_rgba(std::uint32_t word);
void fill_texel_row(std::uint32_t* words, std::uint32_t count, const RGBATexel& color);
void copy_rgb_row_to_texels(std::uint32_t* dstWords, const std::uint32_t* src, std::uint32_t count);
void present_texel_row_to_framebuffer(const std::uint32_t* srcWords, std::uint32_t* dst, std::uint32_t count);
RGBATexel blend(const RGBATexel& dst, const RGBATexel& src, std::uint8_t alpha);
void clear_buffer(const RenderBuffer& buffer, const RGBATexel& color);
void draw_gradient(const RenderBuffer& buffer);

void fill_rect(const RenderBuffer& buffer, int x, int y, int width, int height, const RGBATexel& color);
void blend_pixel(const RenderBuffer& buffer, int x, int y, const RGBATexel& color, std::uint8_t alpha);
int clamp_radius(int width, int height, int radius);
bool point_in_rounded_rect(int x, int y, int rectX, int rectY, int width, int height, int radius);
bool point_in_top_rounded_rect(int x, int y, int rectX, int rectY, int width, int height, int radius);
void fill_rounded_rect(const RenderBuffer& buffer, int x, int y, int width, int height, int radius, const RGBATexel& color);
void fill_top_rounded_rect(const RenderBuffer& buffer, int x, int y, int width, int height, int radius, const RGBATexel& color);
void fill_circle(const RenderBuffer& buffer, int centerX, int centerY, int radius, const RGBATexel& color);
void draw_line_rect(const RenderBuffer& buffer, int x0, int y0, int x1, int y1, int thickness, const RGBATexel& color);

void draw_gradient_pixels(std::uint32_t* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t pitch);
bool ensure_background_storage(DesktopBackground* background, std::uint32_t width, std::uint32_t height);
void scale_background_image(DesktopBackground* background, int imageWidth, int imageHeight, const unsigned char* imagePixels);
bool load_background(DesktopBackground* background, std::uint32_t width, std::uint32_t height, const char* path);
bool load_first_available_background(DesktopBackground* background, std::uint32_t width, std::uint32_t height);
void draw_background(const RenderBuffer& buffer, const DesktopBackground* background);
void draw_background_rect(const RenderBuffer& buffer, const DesktopBackground* background, const Rect& rect);

bool initialize_font(UIFont* font, const char* path, std::uint32_t pixelHeight);
bool initialize_ui_font(UIFont* font);
void destroy_ui_font(UIFont* font);
int text_width(UIFont& font, const char* text);
int floor_div_int(int value, int divisor);
std::uint8_t downsample_glyph_alpha(const unsigned char* bitmap, int width, int height, int x, int y);
void draw_text(const RenderBuffer& buffer, UIFont& font, int x, int y, const char* text, const RGBATexel& color);

void draw_cursor(const RenderBuffer& buffer, const CursorState& cursor, const CursorImage& cursorImage);
Rect cursor_rect(const CursorState& cursor, const CursorImage& cursorImage);
bool rect_is_empty(const Rect& rect);
Rect union_rect(const Rect& a, const Rect& b);
Rect intersect_rect(const Rect& a, const Rect& b);
Rect clamp_rect(const Rect& rect, std::uint32_t width, std::uint32_t height);
void present_rect_to_framebuffer(const RenderBuffer& source, const FramebufferView& target, const Rect& rect);
void draw_cursor_to_framebuffer(const FramebufferView& fb, const CursorState& cursor, const CursorImage& cursorImage);
void present_cursor_move(const RenderBuffer& scene, const FramebufferView& fb, const CursorState& oldCursor, const CursorState& newCursor, const CursorImage& cursorImage);
void move_cursor(const FramebufferView& fb, CursorState* cursor, int x, int y);
void present_to_framebuffer(const RenderBuffer& source, const FramebufferView& target);

bool build_hello_reply(const std::IPCMessage& message, std::services::graphics_compositor::HelloReply* reply);
bool build_background_reply(const std::IPCMessage& message, std::services::graphics_compositor::SetBackgroundReply* reply, DesktopBackground* background, std::uint32_t width, std::uint32_t height, bool* sceneDirty);

bool point_in_rect(int px, int py, int x, int y, int width, int height);
int frame_width(const std::WindowInfo& window);
int frame_height(const std::WindowInfo& window);
bool is_window_visible(const std::WindowInfo& window);
SurfaceCacheEntry* find_surface_cache(SurfaceCacheEntry* cache, std::uint64_t surfaceId);
void remember_surface(SurfaceCacheEntry* cache, const std::SurfaceInfo& info);
bool pump_surface_updates(SurfaceCacheEntry* cache);
std::uint64_t fetch_windows(std::WindowInfo* windows, std::uint64_t capacity);
Rect consume_surface_dirty_rect(const std::WindowInfo* windows, std::uint64_t windowCount, SurfaceCacheEntry* cache, std::uint32_t width, std::uint32_t height);
void clear_surface_dirty_flags(SurfaceCacheEntry* cache);

int window_control_slot_from_left(int control);
Rect window_control_rect(const std::WindowInfo& window, int control);
void draw_window_control_icon_fallback(const RenderBuffer& buffer, const Rect& rect, int control, bool maximized);
void draw_window_control_icon(const RenderBuffer& buffer, const Rect& rect, int control, bool maximized, const WindowControlAssets* controls);
void draw_window_frame(const RenderBuffer& buffer, const std::WindowInfo& window, const WindowControlAssets* controls);
void blit_window_surface(const RenderBuffer& buffer, const std::WindowInfo& window, const SurfaceCacheEntry* surfaceEntry);

Rect taskbar_rect_at(const RenderBuffer& buffer, int index);
int taskbar_center_slot_at(std::uint32_t screenWidth, std::uint32_t screenHeight, int x, int y);
bool launch_taskbar_slot(int slot);
void draw_image_scaled(const RenderBuffer& buffer, const ImageAsset& image, int x, int y, int width, int height);
void draw_taskbar(const RenderBuffer& buffer, const ImageAsset* brandImage, const TaskbarStatusAssets* statusIcons);
void redraw_scene(const RenderBuffer& buffer, const std::WindowInfo* windows, std::uint64_t windowCount, SurfaceCacheEntry* cache, const DesktopBackground* background, const ImageAsset* brandImage, const TaskbarStatusAssets* statusIcons, const WindowControlAssets* controls);
void redraw_scene_rect(const RenderBuffer& buffer, const std::WindowInfo* windows, std::uint64_t windowCount, SurfaceCacheEntry* cache, const DesktopBackground* background, const Rect& dirty, const ImageAsset* brandImage, const TaskbarStatusAssets* statusIcons, const WindowControlAssets* controls);

const std::WindowInfo* top_window_at(const std::WindowInfo* windows, std::uint64_t count, int x, int y);
const std::WindowInfo* window_by_id(const std::WindowInfo* windows, std::uint64_t count, std::uint64_t id);
Rect window_frame_rect(const std::WindowInfo& window);
bool pointer_on_close(const std::WindowInfo& window, int x, int y);
bool pointer_on_maximize(const std::WindowInfo& window, int x, int y);
bool pointer_on_minimize(const std::WindowInfo& window, int x, int y);
bool pointer_on_resize_grip(const std::WindowInfo& window, int x, int y);
bool pointer_on_titlebar(const std::WindowInfo& window, int x, int y);
void begin_move(DragState* drag, const std::WindowInfo& window, int pointerX, int pointerY);
void begin_resize(DragState* drag, const std::WindowInfo& window, int pointerX, int pointerY);
void clear_drag(DragState* drag);
bool handle_pointer_event(const std::Event& event, const std::WindowInfo* windows, std::uint64_t windowCount, std::uint32_t screenWidth, std::uint32_t screenHeight, DragState* drag, std::uint16_t* buttons, Rect* windowDirty);
