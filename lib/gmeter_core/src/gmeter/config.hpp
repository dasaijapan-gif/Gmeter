// Compile-time geometry of the volumetric display.
//
// The display is a stack of GM_LAYERS transparent monochrome OLED panels,
// each GM_PANEL_W x GM_PANEL_H pixels, viewed from the front (the driver's
// seat). Override any of these with -D flags (the ESP-IDF firmware's C core has its own copy, gm_config.h).
#pragma once

#include <cstdint>

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

static_assert(GM_PANEL_H % 8 == 0, "panel height must be a multiple of 8 (SSD130x page format)");
static_assert(GM_LAYERS >= 1 && GM_LAYERS <= 32, "unsupported layer count");

namespace gm {

constexpr int kW = GM_PANEL_W;          // columns per panel (car left -> right)
constexpr int kH = GM_PANEL_H;          // rows per panel    (car up -> down, screen convention)
constexpr int kD = GM_LAYERS;           // layers            (viewer -> windshield)
constexpr int kPages = kH / 8;          // SSD130x pages per panel
static_assert(kPages <= 8, "at most 64 rows per panel (VoxelFrame::shift packs a column into 64 bits)");
constexpr int kBytesPerLayer = kW * kPages;
constexpr int kBytesPerFrame = kBytesPerLayer * kD;
constexpr float kLayerPitchPx = GM_LAYER_PITCH_PX;

// Depth of the volume in pixel units (layer 0 and layer kD-1 sit half a pitch
// inside the box so every layer owns an equal slab).
constexpr float kDepthPx = kD * kLayerPitchPx;

}  // namespace gm
