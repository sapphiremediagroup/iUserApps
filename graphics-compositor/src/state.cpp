#include "common.hpp"

std::WindowInfo gWindowsScratch[kMaxWindows];
SurfaceCacheEntry gSurfaceCache[kMaxSurfaceCache];
UIFont gUIFont = {};
UIFont gClockTimeFont = {};
UIFont gClockDateFont = {};
Rect gDrawClip = { 0, 0, 0, 0 };
bool gDrawClipEnabled = false;
CompositorTimingCounters gTiming = {};

static_assert(sizeof(RGBATexel) == sizeof(std::uint32_t));
static_assert(sizeof(RGBATexel) == sizeof(std::Resizer::RGBA));
