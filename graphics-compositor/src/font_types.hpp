#pragma once

#include <cmath.hpp>
#include <math.h>

namespace graphics_compositor_stb {
double fmod(double value, double modulus);
double pow(double value, double exponent);
double acos(double value);
}

#define STBTT_ifloor(x) static_cast<int>(std::floor(x))
#define STBTT_iceil(x) static_cast<int>(std::ceil(x))
#define STBTT_sqrt(x) std::sqrt(x)
#define STBTT_fmod(x, y) graphics_compositor_stb::fmod((x), (y))
#define STBTT_pow(x, y) graphics_compositor_stb::pow((x), (y))
#define STBTT_cos(x) std::cos(x)
#define STBTT_acos(x) graphics_compositor_stb::acos((x))
#define STBTT_fabs(x) std::fabs(x)
#define STBTT_STATIC
#include <stb_truetype.h>
