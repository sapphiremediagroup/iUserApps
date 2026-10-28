#include "common.hpp"

std::WindowInfo gWindowsScratch[kMaxWindows];
SurfaceCacheEntry gSurfaceCache[kMaxSurfaceCache];
ui::Font gClockTimeFont;
ui::Font gClockDateFont;
CompositorTimingCounters gTiming = {};
