// SPDX-License-Identifier: BSD-3-Clause
//
// Tools cannot measure tiling by reimplementing it: a benchmark with its own
// tile loop proves something about the benchmark. This walks the halo ladder
// through the plug-in's own functions (aiofx::RunWhole / aiofx::RunTiled and
// TileAccumulator, compiled from templates/onnx-inference-ofx/src), so the
// number it prints is the number the plug-in will actually need.
//
// What it does, per overlap in the ladder:
//   1. the whole frame through the model, once;
//   2. the same frame through RunTiled with that overlap;
//   3. max |whole - tiled| over the interior, with a border band excluded.
//
// Why the border is excluded: a tile clipped at the frame edge sees content no
// interior tile ever sees, and the largest deviation lands there. Including it
// buys an overlap several times too large - reported as 0.38 on the right border
// of an otherwise pixel-identical model (references/onnx-and-tensorrt.md section 4).
//
// Why a real interior tile edge is required: if the frame is only one tile wide
// the comparison says nothing about blending, because no two tiles meet.
//
// Deterministic content, so two runs on one machine agree: broad-band hash
// noise per channel, which has energy at every frequency the halo ladder needs
// to resolve.
//
// Exit codes: 0 an overlap was found, 2 the model is not tilable at this
// threshold, 3 usage/load error.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "AiCore.h"
#include "ModelRunner.h"

namespace {

struct Options {
  std::string model;
  int width = 384;
  int height = 320;
  int tile = 256;
  int border = 16;
  double threshold = 0.02;
  std::vector<int> ladder{32, 48, 64, 96, 128};
};

void usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s <model.onnx> [--frame WxH] [--tile N] [--ladder a,b,c]\n"
               "          [--threshold T] [--border P]\n"
               "\n"
               "  --frame WxH     frame to measure on            (default 384x320)\n"
               "  --tile N        core tile size                 (default 256)\n"
               "  --ladder a,b,c  overlaps to try, in order      (default 32,48,64,96,128)\n"
               "  --threshold T   max deviation accepted         (default 0.02)\n"
               "  --border P      frame border excluded on every side, and it must\n"
               "                  sit inside the first tile boundary (default 16)\n",
               argv0);
}

bool parseInt(const char* text, int* out) {
  char* end = nullptr;
  const long v = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || v <= 0 || v > 65536) return false;
  *out = static_cast<int>(v);
  return true;
}

bool parseFrame(const char* text, int* w, int* h) {
  int a = 0;
  int b = 0;
  if (std::sscanf(text, "%dx%d", &a, &b) != 2 || a <= 0 || b <= 0) return false;
  *w = a;
  *h = b;
  return true;
}

// Overlaps may include 0: it is the negative control from the docs (a tiled
// render without context), and a measurement that cannot produce the failure
// case cannot be trusted to recognise success.
bool parseLadder(const char* text, std::vector<int>* out) {
  std::vector<int> values;
  std::string rest(text);
  size_t start = 0;
  while (start <= rest.size()) {
    const size_t comma = rest.find(',', start);
    const std::string piece = rest.substr(start, comma == std::string::npos ? std::string::npos
                                                                           : comma - start);
    char* end = nullptr;
    const long v = std::strtol(piece.c_str(), &end, 10);
    if (piece.empty() || end == piece.c_str() || *end != '\0' || v < 0 || v > 65536) return false;
    values.push_back(static_cast<int>(v));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  if (values.empty()) return false;
  *out = values;
  return true;
}

// Broad-band, deterministic pseudo-noise in [0,1): stable across runs and
// compilers, because the arithmetic is integer-only until the final divide.
float noise(int x, int y, int channel) {
  unsigned h = static_cast<unsigned>(x) * 73856093u ^ static_cast<unsigned>(y) * 19349663u ^
               static_cast<unsigned>(channel) * 83492791u;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return static_cast<float>(h & 0xffffffu) / static_cast<float>(0xffffffu);
}

struct Diff {
  double max = 0.0;
  int x = 0;
  int y = 0;
  int channel = 0;
  long long pixels = 0;
};

// Worst deviation over the interior, border band excluded on all four sides.
Diff compareInterior(const std::vector<float>& whole, const std::vector<float>& tiled, int width,
                     int height, int channels, int border) {
  Diff d;
  for (int c = 0; c < channels; ++c) {
    for (int y = border; y < height - border; ++y) {
      for (int x = border; x < width - border; ++x) {
        const size_t i = (static_cast<size_t>(c) * height + y) * width + x;
        const double diff = std::fabs(static_cast<double>(whole[i]) - static_cast<double>(tiled[i]));
        if (diff > d.max) {
          d.max = diff;
          d.x = x;
          d.y = y;
          d.channel = c;
        }
      }
    }
  }
  d.pixels = static_cast<long long>(width - 2 * border) * (height - 2 * border);
  return d;
}

} // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool hasValue = i + 1 < argc;
    if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      return 0;
    } else if (arg == "--frame" && hasValue) {
      if (!parseFrame(argv[++i], &opt.width, &opt.height)) {
        std::fprintf(stderr, "error: --frame wants WxH, got '%s'\n", argv[i]);
        return 3;
      }
    } else if (arg == "--tile" && hasValue) {
      if (!parseInt(argv[++i], &opt.tile)) {
        std::fprintf(stderr, "error: --tile wants a positive integer\n");
        return 3;
      }
    } else if (arg == "--border" && hasValue) {
      if (!parseInt(argv[++i], &opt.border)) {
        std::fprintf(stderr, "error: --border wants a positive integer\n");
        return 3;
      }
    } else if (arg == "--threshold" && hasValue) {
      opt.threshold = std::atof(argv[++i]);
      if (!(opt.threshold > 0.0)) {
        std::fprintf(stderr, "error: --threshold wants a positive number\n");
        return 3;
      }
    } else if (arg == "--ladder" && hasValue) {
      if (!parseLadder(argv[++i], &opt.ladder)) {
        std::fprintf(stderr, "error: --ladder wants comma-separated positive integers\n");
        return 3;
      }
    } else if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
      return 3;
    } else if (opt.model.empty()) {
      opt.model = arg;
    } else {
      std::fprintf(stderr, "error: more than one model path\n");
      return 3;
    }
  }
  if (opt.model.empty()) {
    usage(argv[0]);
    return 3;
  }

  // A frame narrower than two tiles has no interior tile edge, and then the
  // comparison says nothing about blending however small the numbers look.
  if (opt.tile >= opt.width - opt.border || opt.tile >= opt.height - opt.border) {
    std::fprintf(stderr,
                 "error: no interior tile edge: tile %d must be smaller than the frame %dx%d "
                 "minus the %d-pixel border\n",
                 opt.tile, opt.width, opt.height, opt.border);
    return 3;
  }
  if (2 * opt.border >= opt.width || 2 * opt.border >= opt.height) {
    std::fprintf(stderr, "error: border %d leaves no interior in a %dx%d frame\n", opt.border,
                 opt.width, opt.height);
    return 3;
  }

  try {
    aiofx::ModelRunner runner(opt.model, 1);
    const int channels = runner.InputChannels();
    if (channels != 3) {
      std::fprintf(stderr, "error: model takes %d channels; this tool feeds RGB\n", channels);
      return 3;
    }

    const int width = opt.width;
    const int height = opt.height;
    const long long rowBytes = static_cast<long long>(width) * 4 * static_cast<long long>(sizeof(float));
    std::vector<float> rgba(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        float* p = rgba.data() + (static_cast<size_t>(y) * width + x) * 4;
        p[0] = noise(x, y, 0);
        p[1] = noise(x, y, 1);
        p[2] = noise(x, y, 2);
        p[3] = 1.0f;
      }
    }

    std::printf("model=%s size=%dx%d tile=%d border=%d threshold=%g\n", opt.model.c_str(), width,
                height, opt.tile, opt.border, opt.threshold);

    std::vector<float> whole(static_cast<size_t>(channels) * width * height, 0.0f);
    aiofx::RunWhole(runner, rgba.data(), channels, width, height, rowBytes, whole.data());

    std::vector<float> tiled(static_cast<size_t>(channels) * width * height, 0.0f);
    int chosen = -1;
    for (const int halo : opt.ladder) {
      if (2 * halo >= height) {
        std::printf("ladder halo=%d skipped \"2*halo >= frame height\"\n", halo);
        continue;
      }
      aiofx::TiledOptions opts;
      opts.tileSize = opt.tile;
      opts.halo = halo;
      aiofx::RunTiled(runner, rgba.data(), channels, width, height, rowBytes, opts, tiled.data());
      const Diff d = compareInterior(whole, tiled, width, height, channels, opt.border);
      if (d.max > 0.0) {
        std::printf("ladder halo=%d max=%.9g at=(%d,%d,c%d) compared=%lld\n", halo, d.max, d.x,
                    d.y, d.channel, d.pixels);
      } else {
        std::printf("ladder halo=%d max=0 at=(-,-,-) compared=%lld\n", halo, d.pixels);
      }
      if (chosen < 0 && d.max <= opt.threshold) chosen = halo;
    }

    if (chosen < 0) {
      std::printf("result not-tilable\n");
      std::fprintf(stderr,
                   "no overlap in the ladder keeps the interior within %g: this model has a "
                   "global dependency (or needs a larger ladder)\n",
                   opt.threshold);
      return 2;
    }
    std::printf("result halo=%d\n", chosen);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 3;
  }
}
