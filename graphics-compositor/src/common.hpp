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
#include <ui/graphics.hpp>
#include <ui/theme.hpp>

#ifdef NULL
#undef NULL
#endif
#define NULL 0

constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr std::uint32_t kFrameIntervalMs = 16;
constexpr std::uint32_t kCompositorBufferCount = 3;
constexpr std::uint32_t kBackground = 0x00101820;
constexpr std::uint32_t kBackgroundBottom = 0x001d2b3a;
constexpr std::uint32_t kTitleText = 0x00f2f5f7;
// Window frames (CLAUDE.md "Base program style"): one rounded surface with a
// hairline border, a 40 px title bar holding the app's icon and name, the
// controls on the right, and a soft shadow reaching kShadowMargin past it.
constexpr int kBorder = 1;
constexpr int kTitleBarHeight = 40;
constexpr int kWindowCornerRadius = 12;
constexpr int kControlWidth = 46;
constexpr int kControlGlyphSize = 12;
constexpr int kTitleIconSize = 16;
constexpr int kTitleIconX = 14;
constexpr int kTitleTextX = 38;
constexpr int kShadowBlur = 24;
constexpr int kShadowOffsetY = 10;
constexpr int kShadowMargin = kShadowBlur + kShadowOffsetY;
constexpr int kTitleIconCount = 6;   // terminal, folder, image, cube, globe, app

// The taskbar: one floating bar kTaskbarMargin from the screen's sides and
// bottom. Start button on the left, app tiles centred (the pinned apps, then
// a tile per window of any other app), the tray and clock on the right.
// Windows stay kWorkAreaGap above it.
constexpr int kTaskbarHeight = 52;
constexpr int kTaskbarMargin = 12;
constexpr int kTaskbarRadius = 14;
constexpr int kWorkAreaGap = 12;
constexpr int kStartSize = 36;
constexpr int kStartRadius = 10;
constexpr int kTileSize = 44;
constexpr int kTileGap = 6;
constexpr int kTileRadius = 10;
constexpr int kTileIconSize = 22;
constexpr int kTrayIconSize = 18;
constexpr int kTooltipHeight = 26;
constexpr int kPinnedCount = 5;
constexpr int kMaxTiles = 16;
constexpr int kBarBlurRadius = 10;
constexpr char kNotchBoldFontPath[] = "/bin/StackSansNotch-Bold.ttf";
constexpr int kResizeGripSize = 14;
constexpr std::uint64_t kMaxWindows = 64;
constexpr std::uint64_t kMaxSurfaceCache = 64;
constexpr char kBackgroundDirectory[] = "/bin/backgrounds";
constexpr char kDefaultBackground[] = "/bin/backgrounds/instantos_2.png";
constexpr char kDefaultCursorPath[] = "/bin/cursors/default.png";
constexpr char kIconDirectory[] = "/bin/icons/";
constexpr int kMaxCursorSize = 24;

struct CursorState {
    int x;
    int y;
};

using ui::Rect;

// Where the taskbar's parts are. Tiles 0..kPinnedCount-1 are the pinned
// apps; the rest stand for one window each, oldest first.
struct TaskbarTile {
    Rect rect;
    int slot;                  // the pinned app, or -1
    std::uint64_t windowId;    // the window an unpinned tile stands for
};

struct TaskbarLayout {
    Rect bar;
    Rect start;
    int tileCount;
    TaskbarTile tiles[kMaxTiles];
};

enum class TaskbarHitKind { None, Bar, Start, Tile };

struct TaskbarHit {
    TaskbarHitKind kind;
    int tile;
};

// Where pointer input goes: a press inside a window's content holds the
// pointer for that window until every button is up.
struct PointerRoute {
    std::uint64_t grabWindowId;   // 0: none
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

// The pointer image, scaled down to kMaxCursorSize by area averaging.
struct CursorImage {
    ui::Image image;
    int hotspotX = 0;
    int hotspotY = 0;

    bool loadFromFile(const char* path);
    bool ready() const { return image.valid(); }
};

// Window controls: 0 close, 1 maximize, 2 minimize (and 3, the restore
// glyph a maximized window shows instead of maximize).
enum WindowControl { kControlClose = 0, kControlMaximize = 1, kControlMinimize = 2, kControlRestore = 3 };

struct WindowControlAssets {
    // glyphs[control][0 focused, 1 unfocused, 2 white on the red close].
    ui::Image glyphs[4][3];
    // The app icon a title bar shows, picked by title: [icon][0 focused, 1 unfocused].
    ui::Image apps[kTitleIconCount][2];

    bool load();
};

struct TaskbarAssets {
    // The app icons at tile size: [icon][0 open, 1 not running].
    ui::Image apps[kTitleIconCount][2];
    ui::Image wifi;
    ui::Image wifiOff;
    ui::Image volumeOff;
    ui::Font startFont;   // the start button's "IO"

    bool load();
};

// The taskbar tile under the pointer (-1: none); it shows a tooltip.
extern int gTaskbarHover;

// The window control under the pointer (drawn highlighted).
struct WindowHover {
    std::uint64_t windowId;   // 0: none
    int control;
};
extern WindowHover gWindowHover;


// The scene is drawn into one of these in turn, then presented.
struct TripleBufferState {
    std::uint32_t* pixels[kCompositorBufferCount];
    ui::Canvas buffers[kCompositorBufferCount];
    std::uint32_t nextIndex;

    TripleBufferState();
    TripleBufferState(const TripleBufferState&) = delete;
    TripleBufferState& operator=(const TripleBufferState&) = delete;
    ~TripleBufferState();

    bool initialize(std::uint32_t width, std::uint32_t height);
    ui::Canvas& acquire();
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
    std::uint64_t generation;   // commits seen since the surface appeared
    std::uint32_t unshownLists; // window lists in a row that did not show it
};

// Window lists a surface may go unshown (and uncommitted) before its entry
// is dropped (forget_unshown_surfaces).
constexpr std::uint32_t kSurfaceForgetLists = 8;

struct DesktopBackground {
    std::uint32_t* pixels;
    std::uint32_t width;
    std::uint32_t height;
    bool ready;
    char currentPath[192];
    // The wallpaper under the taskbar, blurred (taskbar_bar_rect's size).
    std::uint32_t* barBlur;
    Rect barBlurRect;
};

struct CompositorTimingCounters {
    std::uint64_t backgroundMs;
    std::uint64_t windowFrameMs;
    std::uint64_t surfaceBlitMs;
    std::uint64_t sceneRedrawMs;
    std::uint64_t presentMs;
    std::uint64_t frames;
    std::uint64_t flipWaitMs;   // flip backend: waiting for the back buffer to leave the screen
    std::uint64_t flips;
};

extern std::WindowInfo gWindowsScratch[kMaxWindows];
extern SurfaceCacheEntry gSurfaceCache[kMaxSurfaceCache];
extern CompositorTimingCounters gTiming;

void write_str(const char* s);
void write_u64(std::uint64_t value);
void add_elapsed(std::uint64_t startMs, std::uint64_t* counter);
void report_timing_if_needed();
bool launch_file_browser();
bool launch_cube();
bool launch_background_switcher();
bool launch_terminal();
bool launch_browser();
// The taskbar's centre slots, left to right, their labels, and the title of
// the window each one's app opens (to mark the ones running).
const char* taskbar_slot_label(int slot);
const char* taskbar_slot_window_title(int slot);
// Whether the interface has an address (kept by a thread following
// NetGetConfig); the taskbar shows the "no internet" icon only when not.
extern volatile bool gNetworkOnline;
void start_network_watch(std::Handle wakeQueue);
bool decode_event_message(const std::IPCMessage& message, std::Event* event);
bool has_png_suffix(const char* name);
void append_text(char* buffer, std::size_t capacity, const char* text);

void present_texel_row_to_framebuffer(const std::uint32_t* srcWords, std::uint32_t* dst, std::uint32_t count);

bool load_background(DesktopBackground* background, std::uint32_t width, std::uint32_t height, const char* path);
bool load_first_available_background(DesktopBackground* background, std::uint32_t width, std::uint32_t height);
void draw_background(ui::Canvas& canvas, const DesktopBackground* background);

Rect cursor_rect(const CursorState& cursor, const CursorImage& cursorImage);
Rect clamp_rect(const Rect& rect, std::uint32_t width, std::uint32_t height);
void present_rect_to_framebuffer(const ui::Canvas& source, const FramebufferView& target, const Rect& rect);
void present_cursor_move(const ui::Canvas& scene, const FramebufferView& fb, const CursorState& oldCursor, const CursorState& newCursor, const CursorImage& cursorImage);
void move_cursor(const FramebufferView& fb, CursorState* cursor, int x, int y);
void present_to_framebuffer(const ui::Canvas& source, const FramebufferView& target);

bool build_hello_reply(const std::IPCMessage& message, std::services::graphics_compositor::HelloReply* reply);
bool build_background_reply(const std::IPCMessage& message, std::services::graphics_compositor::SetBackgroundReply* reply, DesktopBackground* background, std::uint32_t width, std::uint32_t height, bool* sceneDirty);

int frame_width(const std::WindowInfo& window);
int frame_height(const std::WindowInfo& window);
bool is_window_visible(const std::WindowInfo& window);
SurfaceCacheEntry* find_surface_cache(SurfaceCacheEntry* cache, std::uint64_t surfaceId);
void remember_surface(SurfaceCacheEntry* cache, const std::SurfaceInfo& info);
void forget_unshown_surfaces(SurfaceCacheEntry* cache, const std::WindowInfo* windows, std::uint64_t windowCount);
bool pump_surface_updates(SurfaceCacheEntry* cache);
std::uint64_t fetch_windows(std::WindowInfo* windows, std::uint64_t capacity);
Rect consume_surface_dirty_rect(const std::WindowInfo* windows, std::uint64_t windowCount, SurfaceCacheEntry* cache, std::uint32_t width, std::uint32_t height);
void clear_surface_dirty_flags(SurfaceCacheEntry* cache);

int window_control_slot_from_left(int control);
Rect window_control_rect(const std::WindowInfo& window, int control);
int title_icon_index(const char* title);
void draw_window_frame(ui::Canvas& canvas, const std::WindowInfo& window, const WindowControlAssets* controls);
void draw_window_title(ui::Canvas& canvas, const std::WindowInfo& window, const WindowControlAssets* controls);
void blit_window_surface(ui::Canvas& canvas, const std::WindowInfo& window, const SurfaceCacheEntry* surfaceEntry);

Rect taskbar_bar_rect(std::uint32_t screenWidth, std::uint32_t screenHeight);
// The lowest a window's frame may reach.
int work_area_bottom(std::uint32_t screenHeight);
TaskbarLayout taskbar_layout(std::uint32_t screenWidth, std::uint32_t screenHeight, const std::WindowInfo* windows, std::uint64_t windowCount);
TaskbarHit taskbar_hit(const TaskbarLayout& layout, int x, int y);
// A tile's tooltip, above it (empty for -1).
Rect taskbar_tooltip_rect(const TaskbarLayout& layout, int tile, const std::WindowInfo* windows, std::uint64_t windowCount);
// A click on a tile: focus or minimize its app's window, or start the app.
// True when windows changed.
bool taskbar_click(const TaskbarLayout& layout, int tile, const std::WindowInfo* windows, std::uint64_t windowCount);
bool launch_taskbar_slot(int slot);
void draw_taskbar(ui::Canvas& canvas, const std::WindowInfo* windows, std::uint64_t windowCount, const TaskbarAssets* assets, const DesktopBackground* background);
void redraw_scene(ui::Canvas& canvas, const std::WindowInfo* windows, std::uint64_t windowCount, SurfaceCacheEntry* cache, const DesktopBackground* background, const TaskbarAssets* taskbar, const WindowControlAssets* controls);
void redraw_scene_rect(ui::Canvas& canvas, const std::WindowInfo* windows, std::uint64_t windowCount, SurfaceCacheEntry* cache, const DesktopBackground* background, const Rect& dirty, const TaskbarAssets* taskbar, const WindowControlAssets* controls);

const std::WindowInfo* top_window_at(const std::WindowInfo* windows, std::uint64_t count, int x, int y);
void route_pointer_to_window(const std::Event& event, const std::WindowInfo* windows, std::uint64_t count,
                             std::uint32_t screenWidth, std::uint32_t screenHeight,
                             const DragState& drag, PointerRoute* route);
const std::WindowInfo* window_by_id(const std::WindowInfo* windows, std::uint64_t count, std::uint64_t id);
Rect window_frame_rect(const std::WindowInfo& window);
// The frame and its shadow: what must be repainted when the window moves.
Rect window_damage_rect(const std::WindowInfo& window);
// The control (WindowControl) at a point of the window, or -1.
int window_control_at(const std::WindowInfo& window, int x, int y);
bool pointer_on_close(const std::WindowInfo& window, int x, int y);
bool pointer_on_maximize(const std::WindowInfo& window, int x, int y);
bool pointer_on_minimize(const std::WindowInfo& window, int x, int y);
bool pointer_on_resize_grip(const std::WindowInfo& window, int x, int y);
bool pointer_on_titlebar(const std::WindowInfo& window, int x, int y);
void begin_move(DragState* drag, const std::WindowInfo& window, int pointerX, int pointerY);
void begin_resize(DragState* drag, const std::WindowInfo& window, int pointerX, int pointerY);
void clear_drag(DragState* drag);
bool handle_pointer_event(const std::Event& event, const std::WindowInfo* windows, std::uint64_t windowCount, std::uint32_t screenWidth, std::uint32_t screenHeight, DragState* drag, std::uint16_t* buttons, Rect* windowDirty);
