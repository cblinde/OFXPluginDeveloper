// SPDX-License-Identifier: BSD-3-Clause
//
// Host test for the inference pipeline. No GPU, no OFX host, no Python:
//
//   1. PackRGB / UnpackRGB round-trip on tight rows, on padded rows with a
//      non-zero window origin, and on a negative row stride - the three OFX
//      cases that shear a naive packer.
//   2. One whole-frame model evaluation against models/test_golden.bin, which
//      tools/make-test-model.py produced with the Python ONNX Runtime.
//   3. Tiled evaluation against whole-frame evaluation over several tile sizes.
//      With a correct halo the two agree to within float rounding.
//   4. Negative controls: the same comparison with halo = 0 MUST disagree (so
//      test 3 is not passing for the wrong reason), a negative halo MUST come
//      out bit-identical to halo = 0 instead of shrinking the data rect below
//      the core rect, and Add() MUST refuse a core rect outside its data rect
//      rather than read or write past the buffers.
//
// Exit status 0 only if every case behaves as expected.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "AiCore.h"
#include "ModelRunner.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what, double detail = -1.0)
{
    std::printf("  %-56s %s", what, ok ? "ok" : "FAIL");
    if (detail >= 0.0) {
        std::printf("  (%.6g)", detail);
    }
    std::printf("\n");
    if (!ok) {
        ++g_failures;
    }
}

bool ReadFile(const std::string& path, void* dst, size_t bytes)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(bytes));
    return static_cast<size_t>(in.gcount()) == bytes;
}

/// One row of an interleaved image, reached through a signed byte stride.
const float* RowPtr(const float* base, long long rowBytes, int y)
{
    // cppcheck-suppress invalidPointerCast
    return reinterpret_cast<const float*>(reinterpret_cast<const char*>(base) + rowBytes * y);
}

double MaxAbsDiff(const float* a, const float* b, size_t n)
{
    double worst = 0.0;
    for (size_t i = 0; i < n; ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    }
    return worst;
}

/// An interleaved RGBA image laid out the way an OFX host may hand one over:
/// a base pointer, a signed byte stride, and an origin that need not be (0,0).
struct TestImage {
    std::vector<float> storage;
    const float* base = nullptr; // pixel (0,0) of the image
    long long rowBytes = 0;
    int width = 0;
    int height = 0;
};

TestImage MakeImage(const std::vector<float>& planar, int channels, int width, int height,
                    int paddedPixels, bool negativePitch)
{
    TestImage img;
    img.width = width;
    img.height = height;

    const int stridePixels = (width + paddedPixels) * 4;
    const int rows = height + 2;
    img.storage.assign(static_cast<size_t>(stridePixels) * static_cast<size_t>(rows) + 8, -7.0f);

    float* origin = img.storage.data();
    img.rowBytes = static_cast<long long>(stridePixels) * static_cast<long long>(sizeof(float));
    if (negativePitch) {
        // Bottom-up: the first image row lives at the highest address and the
        // stride is negative, which is what a top-down host produces.
        origin = img.storage.data() + static_cast<long long>(stridePixels) * (rows - 2);
        img.rowBytes = -img.rowBytes;
    }

    for (int y = 0; y < height; ++y) {
        // cppcheck-suppress invalidPointerCast
        float* row = reinterpret_cast<float*>(reinterpret_cast<char*>(origin) + img.rowBytes * y);
        for (int x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(width) +
                             static_cast<size_t>(x);
            float* p = row + static_cast<long long>(x) * 4;
            for (int c = 0; c < 3 && c < channels; ++c) {
                p[c] = planar[static_cast<size_t>(c) * static_cast<size_t>(width) *
                                  static_cast<size_t>(height) + i];
            }
            p[3] = 0.25f + 0.5f * static_cast<float>((x + y) % 3);
        }
    }
    img.base = origin;
    return img;
}

void TestPacking(const std::vector<float>& planar, int channels, int width, int height)
{
    std::printf("[1/4] PackRGB / UnpackRGB round-trip\n");

    struct Case {
        const char* label;
        int paddedPixels;
        int ox;
        int oy;
        bool negative;
    };
    const Case cases[] = {
        { "tight rows, origin (0,0)", 0, 0, 0, false },
        { "padded rows (+3 px), origin (2,1)", 3, 2, 1, false },
        { "negative row stride", 0, 0, 0, true },
    };

    for (const Case& c : cases) {
        TestImage img = MakeImage(planar, channels, width, height, c.paddedPixels, c.negative);

        // A few sub-windows, including 1x1, so the origin arithmetic is covered.
        const int sizes[][2] = { { width, height }, { 1, 1 }, { width / 2, height / 3 },
                                 { 3, 5 } };
        for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); ++si) {
            const int w = std::max(1, sizes[si][0]);
            const int h = std::max(1, sizes[si][1]);
            if (c.ox + w > width || c.oy + h > height) {
                continue;
            }
            std::vector<float> packed(static_cast<size_t>(channels) * static_cast<size_t>(w) *
                                      static_cast<size_t>(h));
            aiofx::PackRGB(img.base, img.rowBytes, c.ox, c.oy, w, h, packed.data());

            double worst = 0.0;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = static_cast<size_t>(c.oy + y) * static_cast<size_t>(width) +
                                     static_cast<size_t>(c.ox + x);
                    for (int ch = 0; ch < channels; ++ch) {
                        const double want = planar[static_cast<size_t>(ch) *
                                                   static_cast<size_t>(width) *
                                                   static_cast<size_t>(height) + i];
                        worst = std::max(worst, std::fabs(want - packed[
                            static_cast<size_t>(ch) * static_cast<size_t>(w) * static_cast<size_t>(h) +
                            static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)]));
                    }
                }
            }
            Check(worst == 0.0, (std::string(c.label) + " : pack " + std::to_string(w) + "x" +
                                 std::to_string(h)).c_str(), worst);
        }

        // Unpack back over the same window and confirm RGB and alpha.
        std::vector<float> packed(static_cast<size_t>(channels) * static_cast<size_t>(width) *
                                  static_cast<size_t>(height));
        std::vector<float> flipped(packed.size());
        aiofx::PackRGB(img.base, img.rowBytes, 0, 0, width, height, packed.data());
        std::fill(flipped.begin(), flipped.end(), 1.0f); // stand-in model output
        aiofx::UnpackRGB(flipped.data(), width, height, const_cast<float*>(img.base), img.rowBytes,
                         0, 0);

        bool written = true;
        bool alphaKept = true;
        for (int y = 0; y < height && written && alphaKept; ++y) {
            const float* row = RowPtr(img.base, img.rowBytes, y);
            for (int x = 0; x < width; ++x) {
                const float* p = row + static_cast<long long>(x) * 4;
                if (p[0] != 1.0f || p[1] != 1.0f || p[2] != 1.0f) {
                    written = false;
                    break;
                }
                if (p[3] != 0.25f + 0.5f * static_cast<float>((x + y) % 3)) {
                    alphaKept = false;
                    break;
                }
            }
        }
        Check(written, (std::string(c.label) + " : RGB written").c_str());
        Check(alphaKept, (std::string(c.label) + " : alpha untouched").c_str());
    }
}

void TestWhole(aiofx::ModelRunner& runner, const std::vector<float>& planar,
               const std::vector<float>& golden, int channels, int width, int height)
{
    std::printf("[2/4] whole-frame inference vs Python golden\n");
    TestImage img = MakeImage(planar, channels, width, height, 2, false);
    std::vector<float> out(golden.size(), 0.0f);
    aiofx::RunWhole(runner, img.base, channels, width, height, img.rowBytes, out.data());
    const double worst = MaxAbsDiff(out.data(), golden.data(), out.size());
    Check(worst < 1e-5, "max |c++ - python| over the frame", worst);
}

void TestTiling(aiofx::ModelRunner& runner, const std::vector<float>& planar,
                const std::vector<float>& whole, int channels, int width, int height)
{
    std::printf("[3/4] tiled inference vs whole-frame inference\n");
    TestImage img = MakeImage(planar, channels, width, height, 0, false);
    std::vector<float> out(whole.size(), 0.0f);

    const int tileSizes[] = { 5, 13, 17, 256 };
    for (int tile : tileSizes) {
        aiofx::TiledOptions opts;
        opts.tileSize = tile;
        opts.halo = 1; // one 3x3 convolution: radius 1 is exact
        aiofx::RunTiled(runner, img.base, channels, width, height, img.rowBytes, opts, out.data());
        const double worst = MaxAbsDiff(out.data(), whole.data(), out.size());
        Check(worst < 1e-5, ("tile=" + std::to_string(tile) + " halo=1 matches whole").c_str(),
              worst);
    }

    // Odd sizes and 1-pixel-tall images: the tile planner's clamping paths.
    const int odd[][2] = { { 1, 1 }, { 7, 5 }, { 64, 3 }, { 3, 48 } };
    for (size_t oi = 0; oi < sizeof(odd) / sizeof(odd[0]); ++oi) {
        const int w = odd[oi][0];
        const int h = odd[oi][1];
        if (w > width || h > height) {
            continue;
        }
        std::vector<float> sub(static_cast<size_t>(channels) * static_cast<size_t>(w) *
                               static_cast<size_t>(h));
        std::vector<float> subWhole(sub.size());
        std::vector<float> subTiled(sub.size());
        // Crop the fixture's top-left corner into a standalone image.
        for (int c = 0; c < channels; ++c) {
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t src = static_cast<size_t>(c) * static_cast<size_t>(width) *
                                           static_cast<size_t>(height) +
                                       static_cast<size_t>(y) * static_cast<size_t>(width) +
                                       static_cast<size_t>(x);
                    sub[static_cast<size_t>(c) * static_cast<size_t>(w) * static_cast<size_t>(h) +
                        static_cast<size_t>(y) * static_cast<size_t>(w) +
                        static_cast<size_t>(x)] = planar[src];
                }
            }
        }
        TestImage crop = MakeImage(sub, channels, w, h, 1, false);
        aiofx::RunWhole(runner, crop.base, channels, w, h, crop.rowBytes, subWhole.data());
        aiofx::TiledOptions opts;
        opts.tileSize = 4;
        opts.halo = 1;
        aiofx::RunTiled(runner, crop.base, channels, w, h, crop.rowBytes, opts, subTiled.data());
        const double worst = MaxAbsDiff(subWhole.data(), subTiled.data(), subTiled.size());
        Check(worst < 1e-5, ("odd " + std::to_string(w) + "x" + std::to_string(h) +
                             " tiled matches whole").c_str(), worst);
    }

    // A negative halo is a caller error, not "no context": with -1 the data
    // rect ends up inside the core rect and Add() indexes past the tile buffer
    // (ASan: SEGV in TileAccumulator::Add). RunTiled() clamps, so the result
    // must equal halo = 0 exactly - same arithmetic on the same rects.
    std::vector<float> zeroHalo(out.size(), 0.0f);
    aiofx::TiledOptions clampOpts;
    clampOpts.tileSize = 13;
    clampOpts.halo = 0;
    aiofx::RunTiled(runner, img.base, channels, width, height, img.rowBytes, clampOpts,
                    zeroHalo.data());
    // -1 shrinks the data rect and reads past a tile row; -7 inverts it (negative
    // width/height). Both must take the halo = 0 path.
    const int badHalos[] = { -1, -7 };
    for (int badHalo : badHalos) {
        clampOpts.halo = badHalo;
        aiofx::RunTiled(runner, img.base, channels, width, height, img.rowBytes, clampOpts,
                        out.data());
        const double clampDiff = MaxAbsDiff(out.data(), zeroHalo.data(), out.size());
        Check(clampDiff == 0.0,
              ("tile=13 halo=" + std::to_string(badHalo) + " is clamped to halo=0").c_str(),
              clampDiff);
    }

    std::printf("[4/4] negative controls: wrong blending, negative halo, bad rect\n");
    aiofx::TiledOptions opts;
    opts.tileSize = 13;
    opts.halo = 0;
    aiofx::RunTiled(runner, img.base, channels, width, height, img.rowBytes, opts, out.data());
    const double worst = MaxAbsDiff(out.data(), whole.data(), out.size());
    Check(worst > 1e-3, "tile=13 halo=0 disagrees with whole (expected)", worst);

    // The accumulator is public API: a core rect that does not lie inside its
    // data rect must be refused, not written through.
    aiofx::TileAccumulator acc(8, 8, 3);
    std::vector<float> tile(static_cast<size_t>(3) * 4 * 4, 0.0f);
    bool refused = false;
    try {
        acc.Add(aiofx::TileRect{ 0, 0, 4, 4 }, aiofx::TileRect{ 0, 0, 6, 6 }, tile.data(), 3, 1);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    Check(refused, "Add rejects a core rect outside its data rect");
}

} // namespace

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : "models";

    int channels = 0;
    int height = 0;
    int width = 0;
    {
        std::ifstream shape(dir + "/test_shape.txt");
        if (!(shape >> channels >> height >> width) || channels <= 0 || height <= 0 || width <= 0) {
            std::printf("FAIL: cannot read %s/test_shape.txt\n", dir.c_str());
            return 2;
        }
    }

    const size_t elements = static_cast<size_t>(channels) * static_cast<size_t>(width) *
                            static_cast<size_t>(height);
    std::vector<float> planar(elements);
    std::vector<float> golden(elements);
    if (!ReadFile(dir + "/test_input.bin", planar.data(), elements * sizeof(float)) ||
        !ReadFile(dir + "/test_golden.bin", golden.data(), elements * sizeof(float))) {
        std::printf("FAIL: cannot read the .bin fixtures in %s\n", dir.c_str());
        return 2;
    }

    std::printf("model : %s/test_conv.onnx  (c=%d h=%d w=%d)\n", dir.c_str(), channels, height,
                width);

    std::unique_ptr<aiofx::ModelRunner> runner;
    try {
        runner.reset(new aiofx::ModelRunner(dir + "/test_conv.onnx", 1));
    } catch (const std::exception& e) {
        std::printf("FAIL: could not create the session: %s\n", e.what());
        return 2;
    }
    Check(runner->InputChannels() == channels, "model channel count matches the fixture");

    TestPacking(planar, channels, width, height);
    TestWhole(*runner, planar, golden, channels, width, height);

    std::vector<float> whole(golden.size(), 0.0f);
    TestImage full = MakeImage(planar, channels, width, height, 0, false);
    aiofx::RunWhole(*runner, full.base, channels, width, height, full.rowBytes, whole.data());
    TestTiling(*runner, planar, whole, channels, width, height);

    if (g_failures == 0) {
        std::printf("\nPASS: inference pipeline behaves as specified\n");
        return 0;
    }
    std::printf("\nFAIL: %d check(s) failed\n", g_failures);
    return 1;
}
