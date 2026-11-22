// SPDX-License-Identifier: BSD-3-Clause

#include "AiCore.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace aiofx {
namespace {

constexpr int kComponents = 4; // RGBA float

inline const float* RowOf(const float* base, long long rowBytes, int y)
{
    // cppcheck-suppress invalidPointerCast
    return reinterpret_cast<const float*>(reinterpret_cast<const char*>(base) + rowBytes * y);
}

inline float* RowOf(float* base, long long rowBytes, int y)
{
    // cppcheck-suppress invalidPointerCast
    return reinterpret_cast<float*>(reinterpret_cast<char*>(base) + rowBytes * y);
}

} // namespace

void PackRGB(const float* rgba, long long rowBytes, int x0, int y0, int w, int h, float* planar)
{
    const size_t plane = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (int y = 0; y < h; ++y) {
        const float* row = RowOf(rgba, rowBytes, y0 + y) + static_cast<long long>(x0) * kComponents;
        for (int x = 0; x < w; ++x, row += kComponents) {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
            planar[i] = row[0];
            planar[plane + i] = row[1];
            planar[2 * plane + i] = row[2];
        }
    }
}

// cppcheck-suppress constParameterPointer
// (false positive: this function writes RGB through `rgba` via RowOf)
void UnpackRGB(const float* planar, int w, int h, float* rgba, long long rowBytes, int x0, int y0)
{
    const size_t plane = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (int y = 0; y < h; ++y) {
        float* row = RowOf(rgba, rowBytes, y0 + y) + static_cast<long long>(x0) * kComponents;
        for (int x = 0; x < w; ++x, row += kComponents) {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
            row[0] = planar[i];
            row[1] = planar[plane + i];
            row[2] = planar[2 * plane + i];
            // row[3] (alpha) is deliberately not touched.
        }
    }
}

std::vector<TileRect> PlanTiles(int width, int height, int tileSize)
{
    std::vector<TileRect> tiles;
    if (width <= 0 || height <= 0) {
        return tiles;
    }
    if (tileSize <= 0 || tileSize >= std::max(width, height)) {
        tiles.push_back(TileRect{ 0, 0, width, height });
        return tiles;
    }
    for (int y = 0; y < height; y += tileSize) {
        for (int x = 0; x < width; x += tileSize) {
            tiles.push_back(TileRect{ x, y, std::min(tileSize, width - x),
                                      std::min(tileSize, height - y) });
        }
    }
    return tiles;
}

TileRect Expand(const TileRect& r, int halo, int width, int height)
{
    const int x0 = std::max(0, r.x - halo);
    const int y0 = std::max(0, r.y - halo);
    const int x1 = std::min(width, r.x + r.w + halo);
    const int y1 = std::min(height, r.y + r.h + halo);
    return TileRect{ x0, y0, x1 - x0, y1 - y0 };
}

float EdgeWeight(int indexInData, int dataExtent, bool atImageStart, bool atImageEnd, int halo)
{
    if (halo <= 0) {
        return 1.0f;
    }
    const int fromStart = atImageStart ? halo : indexInData;
    const int fromEnd = atImageEnd ? halo : (dataExtent - 1 - indexInData);
    const int distance = std::min(fromStart, fromEnd);
    if (distance >= halo) {
        return 1.0f;
    }
    return static_cast<float>(distance + 1) / static_cast<float>(halo);
}

TileAccumulator::TileAccumulator(int width, int height, int channels)
    : _width(width > 0 ? width : 0)
    , _height(height > 0 ? height : 0)
    , _channels(channels > 0 ? channels : 0)
    , _acc(static_cast<size_t>(_width) * static_cast<size_t>(_height) *
               static_cast<size_t>(_channels), 0.0f)
    , _weight(static_cast<size_t>(_width) * static_cast<size_t>(_height) *
                  static_cast<size_t>(_channels), 0.0f)
{
}

void TileAccumulator::Add(const TileRect& data, const TileRect& core, const float* planar,
                          int channels, int halo)
{
    // The planes are sized by the constructor. Writing more channels than that
    // would index past them, which is how a four-channel model would corrupt
    // the heap instead of failing.
    if (channels != _channels) {
        throw std::invalid_argument(
            "TileAccumulator: Add() channel count differs from the constructor");
    }
    const size_t dataPlane = static_cast<size_t>(data.w) * static_cast<size_t>(data.h);
    const bool startX = data.x == 0;
    const bool endX = data.x + data.w == _width;
    const bool startY = data.y == 0;
    const bool endY = data.y + data.h == _height;

    for (int y = core.y; y < core.y + core.h; ++y) {
        const int iy = y - data.y;
        const float wy = EdgeWeight(iy, data.h, startY, endY, halo);
        for (int x = core.x; x < core.x + core.w; ++x) {
            const int ix = x - data.x;
            const float w = wy * EdgeWeight(ix, data.w, startX, endX, halo);
            const size_t src = static_cast<size_t>(iy) * static_cast<size_t>(data.w) +
                               static_cast<size_t>(ix);
            const size_t dst = static_cast<size_t>(y) * static_cast<size_t>(_width) +
                               static_cast<size_t>(x);
            for (int c = 0; c < channels; ++c) {
                _acc[static_cast<size_t>(c) * static_cast<size_t>(_width) * static_cast<size_t>(_height) + dst] +=
                    planar[static_cast<size_t>(c) * dataPlane + src] * w;
                _weight[static_cast<size_t>(c) * static_cast<size_t>(_width) * static_cast<size_t>(_height) + dst] += w;
            }
        }
    }
}

void TileAccumulator::Resolve(float* planarOut) const
{
    const size_t total = static_cast<size_t>(_width) * static_cast<size_t>(_height) *
                         static_cast<size_t>(_channels);
    for (size_t i = 0; i < total; ++i) {
        const float w = _weight[i];
        planarOut[i] = w > 0.0f ? _acc[i] / w : 0.0f;
    }
}

void RunWhole(ModelRunner& runner, const float* rgba, int channels, int width, int height,
              long long rowBytes, float* planarOut)
{
    const size_t elements = static_cast<size_t>(channels) * static_cast<size_t>(width) *
                            static_cast<size_t>(height);
    if (elements == 0) {
        return;
    }
    std::vector<float> input(elements);
    PackRGB(rgba, rowBytes, 0, 0, width, height, input.data());
    runner.Run(input.data(), channels, height, width, planarOut);
}

void RunTiled(ModelRunner& runner, const float* rgba, int channels, int width, int height,
              long long rowBytes, const TiledOptions& opts, float* planarOut)
{
    const auto tiles = PlanTiles(width, height, opts.tileSize);
    if (tiles.empty()) {
        return;
    }
    if (tiles.size() == 1 && opts.halo <= 0) {
        // Nothing to blend and nothing to add: skip the copy through the
        // accumulator.
        RunWhole(runner, rgba, channels, width, height, rowBytes, planarOut);
        return;
    }

    TileAccumulator acc(width, height, channels);
    std::vector<float> in;
    std::vector<float> out;
    for (const TileRect& core : tiles) {
        const TileRect data = Expand(core, opts.halo, width, height);
        const size_t elements = static_cast<size_t>(channels) * static_cast<size_t>(data.w) *
                                static_cast<size_t>(data.h);
        in.resize(elements);
        out.resize(elements);
        PackRGB(rgba, rowBytes, data.x, data.y, data.w, data.h, in.data());
        runner.Run(in.data(), channels, data.h, data.w, out.data());
        acc.Add(data, core, out.data(), channels, opts.halo);
    }
    acc.Resolve(planarOut);
}

} // namespace aiofx
