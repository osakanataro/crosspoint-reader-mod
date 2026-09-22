#pragma once

#include <cstdint>

// Verification build only (-DOST_SCALE_TEST=1): offers a virtual "18 pt" reader size that
// loads the family's 16 pt .cpfont and scales every glyph by 9/8 as it enters the resident
// caches, so the page is laid out and drawn with 18 pt metrics from 16 pt bitmaps. The
// point is to judge, on the panel, whether a scaled 16 pt is an acceptable stand-in for a
// real 18 pt — the size a flash-resident font cache could hold when the 18 pt file cannot.
//
// The size is stored in SETTINGS.fontPointSize as this sentinel so section caches, font ids
// and the size picker all treat it as a size of its own, distinct from both 16 and 18.
#ifndef OST_SCALE_TEST
#define OST_SCALE_TEST 0
#endif

#if OST_SCALE_TEST
inline constexpr uint8_t OST_SCALED_18_FROM_16_PT = 118;
inline constexpr uint8_t OST_SCALE_BASE_PT = 16;
inline constexpr uint8_t OST_SCALE_NUM = 9;
inline constexpr uint8_t OST_SCALE_DEN = 8;
#endif
