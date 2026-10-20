// Compile-time geometry of the volumetric display.
//
// The display is a stack of GM_LAYERS transparent monochrome OLED panels,
// each GM_PANEL_W x GM_PANEL_H pixels, viewed from the front (the driver's
// seat). In the firmware the panel size comes from menuconfig (Gmeter >
// Panels); host builds pass -D flags.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
#define GM_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define GM_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#ifndef GM_PANEL_W
#define GM_PANEL_W CONFIG_GMETER_PANEL_W
#endif
#ifndef GM_PANEL_H
#define GM_PANEL_H CONFIG_GMETER_PANEL_H
#endif
#endif

#ifndef GM_PANEL_W
#define GM_PANEL_W 128
#endif
#ifndef GM_PANEL_H
#define GM_PANEL_H 64
#endif
#ifndef GM_LAYERS
#define GM_LAYERS 8
#endif
// Physical spacing between panels expressed in pixel pitches, so that the
// simulation can use one unit (pixels) for all three axes. A 1.5" 128x64
// panel has a ~0.27 mm pitch; panels spaced ~4-5 mm apart => ~16 px/layer.
#ifndef GM_LAYER_PITCH_PX
#define GM_LAYER_PITCH_PX 16.0f
#endif

GM_STATIC_ASSERT(GM_PANEL_H % 8 == 0, "panel height must be a multiple of 8 (SSD130x page format)");
GM_STATIC_ASSERT(GM_LAYERS >= 1 && GM_LAYERS <= 32, "unsupported layer count");

#define GM_W GM_PANEL_W  // columns per panel (car left -> right)
#define GM_H GM_PANEL_H  // rows per panel    (car up -> down, screen convention)
#define GM_D GM_LAYERS   // layers            (viewer -> windshield)
#define GM_PAGES (GM_H / 8)  // SSD130x pages per panel
GM_STATIC_ASSERT(GM_PAGES <= 8, "at most 64 rows per panel (gm_frame_shift packs a column into 64 bits)");
#define GM_BYTES_PER_LAYER (GM_W * GM_PAGES)
#define GM_BYTES_PER_FRAME (GM_BYTES_PER_LAYER * GM_D)

// Depth of the volume in pixel units (layer 0 and layer GM_D-1 sit half a
// pitch inside the box so every layer owns an equal slab).
#define GM_DEPTH_PX (GM_D * GM_LAYER_PITCH_PX)
