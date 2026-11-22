// SPDX-License-Identifier: BSD-3-Clause
//
// The parts of an inference plug-in that are not the model: turning an OFX
// image into a tensor, cutting it into tiles, and putting it back together.
//
// Everything here is free of ONNX Runtime types, so it is unit-testable without
// a model, a GPU or a host (see test/ai_pipeline_test.cpp).
//
// Terminology, because the two rects are what tiling bugs are made of:
//
//   data rect  the pixels actually handed to the model, = core + halo, clamped
//              to the image. The model's padding happens at the edge of THIS
//              rect, so its border pixels are the least trustworthy.
//   core rect  the pixels this tile is responsible for writing. It is data
//              inset by the halo, so every core pixel saw a full neighbourhood.

#ifndef AIOFX_AI_CORE_H
#define AIOFX_AI_CORE_H

#include <vector>

#include "ModelRunner.h"

namespace aiofx {

struct TileRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// ---------------------------------------------------------------------- packing
//
// OFX gives an interleaved RGBA float image with a signed row stride
// (kOfxImagePropRowBytes) and a render window that need not start at (0,0).
// Models want planar NCHW. These two functions are that translation, and they
// are where "it works in Resolve, shears in Nuke" bugs live.

/// rgba[x0..x0+w, y0..y0+h] -> planar [3][h][w], tightly packed.
/// `rowBytes` is bytes per row of `rgba` and may be negative.
void PackRGB(const float* rgba, long long rowBytes, int x0, int y0, int w, int h, float* planar);

/// planar [3][w*h] -> rgba[x0..x0+w, y0..y0+h]; alpha is left untouched.
void UnpackRGB(const float* planar, int w, int h, float* rgba, long long rowBytes, int x0, int y0);

// ----------------------------------------------------------------------- tiling

/// Disjoint tiles covering the image. tileSize <= 0 means "one tile".
std::vector<TileRect> PlanTiles(int width, int height, int tileSize);

/// `r` grown by `halo` on every side, clamped to [0,width) x [0,height).
TileRect Expand(const TileRect& r, int halo, int width, int height);

/// Weight for one pixel, 1.0 well inside the data rect and ramping to the edge
/// over `halo` pixels. The ramp is skipped on a side that coincides with the
/// image border, where there is no neighbouring tile to blend with.
float EdgeWeight(int indexInData, int dataExtent, bool atImageStart, bool atImageEnd, int halo);

/// Accumulates overlapping model outputs and normalises by the total weight.
/// `channels` sizes both full-frame planes; Add() must be given the same count.
class TileAccumulator
{
public:
    TileAccumulator(int width, int height, int channels);

    /// `planar` holds channels*data.w*data.h values for `data`. Throws
    /// std::invalid_argument when `core` does not lie inside `data` and the
    /// image, which is the case that would otherwise read or write out of
    /// bounds.
    void Add(const TileRect& data, const TileRect& core, const float* planar, int channels, int halo);

    /// Writes channels*width*height values; pixels with no contribution are 0.
    void Resolve(float* planarOut) const;

private:
    int _width;
    int _height;
    int _channels;
    std::vector<float> _acc;
    std::vector<float> _weight;
};

struct TiledOptions {
    /// Core tile size in pixels. <= 0 means "do not tile".
    int tileSize = 256;
    /// Pixels of context added on each side. Must be at least the model's
    /// receptive field radius: 1 for a single 3x3 convolution, more for a
    /// stack of them. Too small is a correctness bug, not a quality trade.
    /// Negative values are clamped to 0 by RunTiled().
    int halo = 8;
};

// -------------------------------------------------------------------- inference

/// One model evaluation over the whole frame. Fastest for small images and for
/// models whose memory use is bounded; the caller must know the image fits.
void RunWhole(ModelRunner& runner, const float* rgba, int channels, int width, int height,
              long long rowBytes, float* planarOut);

/// Model evaluation over tiles of `opts.tileSize` with `opts.halo` context,
/// blended back together. It bounds the model's working set (one tile plus its
/// halo); it does not shrink the plug-in's own buffers. The accumulator holds
/// two full-frame planes per channel - 24 bytes/pixel at three channels, about
/// 800 MB at 8K - on top of the caller's output.
void RunTiled(ModelRunner& runner, const float* rgba, int channels, int width, int height,
              long long rowBytes, const TiledOptions& opts, float* planarOut);

} // namespace aiofx

#endif // AIOFX_AI_CORE_H
