#pragma once

#ifdef __aarch64__
#include <arm_neon.h>

constexpr bool x_targetX64 = false;
constexpr std::string x_asmCommentStart = "//";

static inline double fast_sqrt(double v)
{
    return vget_lane_f64(vsqrt_f64(vdup_n_f64(v)), 0);
}

static inline void clear_cache(char* from, char* to)
{
    __builtin___clear_cache(from, to);
}

#else
#include <emmintrin.h>

constexpr bool x_targetX64 = true;
constexpr std::string x_asmCommentStart = "#";

static inline double fast_sqrt(double v)
{
    __m128d x; x[0] = v;
    x = _mm_sqrt_sd(x, x);
    return x[0];
}

static inline void clear_cache(char*, char*) { }
#endif

constexpr size_t x_targetPageSize = x_targetX64 ? 4096 : 16384;
