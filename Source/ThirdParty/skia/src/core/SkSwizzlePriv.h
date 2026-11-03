/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkSwizzlePriv_DEFINED
#define SkSwizzlePriv_DEFINED

#include "src/core/SkColorData.h"
#include "src/core/SkVx.h"

#include <cstdint>

namespace SkOpts {
    // Swizzle input into some sort of 8888 pixel, {premul,unpremul} x {rgba,bgra}.
    using Swizzle_8888_u32 = void (*)(uint32_t*, const uint32_t*, int);
    extern Swizzle_8888_u32 RGBA_to_BGRA,          // i.e. just swap RB
                            RGBA_to_rgbA,          // i.e. just premultiply
                            RGBA_to_bgrA,          // i.e. swap RB and premultiply
                            rgbA_to_RGBA,          // i.e. just unpremultiply
                            rgbA_to_BGRA,          // i.e. swap RB and unpremultiply
                            inverted_CMYK_to_RGB1, // i.e. convert color space
                            inverted_CMYK_to_BGR1; // i.e. convert color space

    using Swizzle_8888_u8 = void (*)(uint32_t*, const uint8_t*, int);
    extern Swizzle_8888_u8 RGB_to_RGB1,     // i.e. insert an opaque alpha
                           RGB_to_BGR1,     // i.e. swap RB and insert an opaque alpha
                           gray_to_RGB1,    // i.e. expand to color channels + an opaque alpha
                           grayA_to_RGBA,   // i.e. expand to color channels
                           grayA_to_rgbA;   // i.e. expand to color channels and premultiply

    void Init_Swizzler();
}  // namespace SkOpts

static inline skvx::float4 swizzle_rb(const skvx::float4& x) {
    return skvx::shuffle<2, 1, 0, 3>(x);
}

static inline skvx::float4 swizzle_rb_if_bgra(const skvx::float4& x) {
#if defined(SK_PMCOLOR_IS_BGRA)
    return swizzle_rb(x);
#else
    return x;
#endif
}

static inline skvx::float4 Sk4f_fromL32(uint32_t px) {
    return skvx::cast<float>(skvx::byte4::Load(&px)) * (1 / 255.0f);
}

static inline uint32_t Sk4f_toL32(const skvx::float4& px) {
    uint32_t l32;
    // For the expected positive color values, the +0.5 before the pin and cast effectively rounds
    // to the nearest int without having to call round() or lrint().
    skvx::cast<uint8_t>(skvx::pin(px * 255.f + 0.5f, skvx::float4(0.f), skvx::float4(255.f)))
                       .store(&l32);
    return l32;
}

// Sk4f_fromL32 and Sk4f_toL32 work on a pixel's bytes in memory order. A packed color value
// (SkColor, SkPMColor) keeps channel 0 in its low-order byte, which is the last byte in memory on
// big-endian, so these variants convert by value.
static inline skvx::float4 Sk4f_fromColorValue(uint32_t value) {
#ifdef SK_CPU_BENDIAN
    return skvx::shuffle<3, 2, 1, 0>(Sk4f_fromL32(value));
#else
    return Sk4f_fromL32(value);
#endif
}

static inline uint32_t Sk4f_toColorValue(const skvx::float4& px) {
#ifdef SK_CPU_BENDIAN
    return Sk4f_toL32(skvx::shuffle<3, 2, 1, 0>(px));
#else
    return Sk4f_toL32(px);
#endif
}
#endif  // SkSwizzlePriv_DEFINED
