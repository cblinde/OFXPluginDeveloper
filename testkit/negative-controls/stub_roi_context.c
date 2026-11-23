/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Negative control for `--roi-context`: one plug-in, two builds, one decision
 * between them.
 *
 * Both apply a 3x3 box filter to the render window, reading the source image the
 * host hands over. The honoured build clamps its taps to the published source
 * bounds - the context region the plug-in asked the host for - so a window
 * inside the frame sees the neighbours the whole-frame render saw. The ignored
 * build clamps them to the render window instead, which is exactly what a
 * plug-in does when it declares a halo and then never reads it: the picture is
 * plausible and its border is wrong.
 *
 * Built twice by run-negative-controls.sh:
 *   stub_roi_context.c              -> the pattern; the harness must pass it
 *   -DIGNORE_CONTEXT                -> the harness must fail it, naming
 *                                      "the inset window matches the whole frame
 *                                      bit for bit"
 *
 * No OFX Support library and no ONNX: the fault under test is a pixel
 * dependency, so the filter has to be small enough to read in one sitting and
 * reproducible to the bit (same nine floats in the same order for every pixel).
 *
 * Two more controls come out of this file for the label check that
 * `--label-param` makes. -DNO_LABEL declares the read-only `provider` label,
 * gives it a default, and never writes it: the harness must reject that, or
 * "the plug-in says which provider it got" would be satisfied by a default on
 * its own. -DWITH_LABEL declares the same label and writes it from render: the
 * harness must accept that, so the rejection above is about the missing write
 * and not about the parameter.
 *
 * The render window is read with -1 pixel of context on every side and no
 * intermediate rounding, so the arithmetic per pixel does not depend on where
 * the tile or window starts.
 */
#include <string.h>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"

/* Four builds come out of this one file: plain and internally tiled, each in a
 * context-reading and a context-ignoring form. The ids have to differ because
 * the harness is told which plug-in to expect. */
#if defined(NO_LABEL)
#define PLUGIN_ID "com.testkit.negative.nolabel"
#elif defined(WITH_LABEL)
#define PLUGIN_ID "com.testkit.control.labelok"
#elif defined(TILED) && defined(IGNORE_CONTEXT)
#define PLUGIN_ID "com.testkit.negative.roiignoredtiled"
#elif defined(TILED)
#define PLUGIN_ID "com.testkit.control.roioktiled"
#elif defined(IGNORE_CONTEXT)
#define PLUGIN_ID "com.testkit.negative.roiignored"
#else
#define PLUGIN_ID "com.testkit.control.roiok"
#endif

static OfxHost* g_host = 0;
static const OfxPropertySuiteV1* g_props = 0;
static const OfxImageEffectSuiteV1* g_effects = 0;
#if defined(NO_LABEL) || defined(WITH_LABEL)
static const OfxParameterSuiteV1* g_params = 0;
#endif

static void setHost(OfxHost* host) { g_host = host; }

static void fetchSuites(void)
{
  if (!g_host) return;
  if (!g_props) {
    g_props = (const OfxPropertySuiteV1*)g_host->fetchSuite(g_host->host, kOfxPropertySuite, 1);
  }
  if (!g_effects) {
    g_effects = (const OfxImageEffectSuiteV1*)g_host->fetchSuite(g_host->host,
                                                                kOfxImageEffectSuite, 1);
  }
#if defined(NO_LABEL) || defined(WITH_LABEL)
  if (!g_params) {
    g_params = (const OfxParameterSuiteV1*)g_host->fetchSuite(g_host->host,
                                                              kOfxParameterSuite, 1);
  }
#endif
}

/* A 3x3 box filter: radius 1, so the context region is one pixel. */
#define RADIUS 1

#ifdef TILED
/* Core tiles, and the context each tile asks for. Two pixels of halo around a
 * 32-pixel core is what a real plug-in does with a small receptive field; the
 * point here is that the tile boundary is a boundary the whole-frame render does
 * not have, so ignoring the context shows up inside the window, not only at its
 * edge. */
#define TILE 32
#define TILE_HALO 2
#endif

/* Writes every pixel of `core` by averaging a 3x3 neighbourhood, with the taps
 * clamped to `box`. `box` is where the pixels the tile is allowed to read live:
 * the published source bounds when the plug-in uses the context it declared,
 * the core itself when it does not. */
static void filterRect(OfxPropertySetHandle srcProps, OfxPropertySetHandle dstProps,
                       const int core[4], const int box[4])
{
  void* srcData = 0;
  void* dstData = 0;
  int srcRowBytes = 0;
  int dstRowBytes = 0;
  int sb[4] = {0, 0, 0, 0};
  int db[4] = {0, 0, 0, 0};
  g_props->propGetPointer(srcProps, kOfxImagePropData, 0, &srcData);
  g_props->propGetInt(srcProps, kOfxImagePropRowBytes, 0, &srcRowBytes);
  g_props->propGetIntN(srcProps, kOfxImagePropBounds, 4, sb);
  g_props->propGetPointer(dstProps, kOfxImagePropData, 0, &dstData);
  g_props->propGetInt(dstProps, kOfxImagePropRowBytes, 0, &dstRowBytes);
  g_props->propGetIntN(dstProps, kOfxImagePropBounds, 4, db);

  /* Where the taps may come from. This is what the control proves: with the
     declared context the plug-in reads the neighbours it asked for; without it
     it reads only what it was handed and the boundary is computed from clamped
     taps - at the window edge in the plain build, at every tile boundary in the
     tiled one. */
  const int lo_x = box[0], lo_y = box[1], hi_x = box[2], hi_y = box[3];

  for (int y = core[1]; y < core[3]; ++y) {
    float* out = (float*)((char*)dstData + (long long)(y - db[1]) * dstRowBytes +
                          (long long)(core[0] - db[0]) * 16);
    for (int x = core[0]; x < core[2]; ++x, out += 4) {
      float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      for (int dy = -RADIUS; dy <= RADIUS; ++dy) {
        int py = y + dy;
        if (py < lo_y) py = lo_y;
        if (py > hi_y - 1) py = hi_y - 1;
        /* lo_x is at or after sb.x1, so rebasing on it and stepping by
           (px - lo_x) lands on the same pixel as addressing from sb.x1. */
        const float* row = (const float*)((const char*)srcData +
                                          (long long)(py - sb[1]) * srcRowBytes +
                                          (long long)(lo_x - sb[0]) * 16);
        for (int dx = -RADIUS; dx <= RADIUS; ++dx) {
          int px = x + dx;
          if (px < lo_x) px = lo_x;
          if (px > hi_x - 1) px = hi_x - 1;
          const float* tap = row + (long long)(px - lo_x) * 4;
          for (int c = 0; c < 4; ++c) acc[c] += tap[c];
        }
      }
      for (int c = 0; c < 4; ++c) out[c] = acc[c] / 9.0f;
    }
  }
}

static int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Renders `window` the way the build under test is supposed to: whole, or as
 * disjoint 32-pixel cores each with TILE_HALO of context. In the ignored builds
 * the context is dropped, so a tile sees nothing beyond itself. */
static void filterWindow(OfxPropertySetHandle srcProps, OfxPropertySetHandle dstProps,
                         const int window[4])
{
  int sb[4] = {0, 0, 0, 0};
  g_props->propGetIntN(srcProps, kOfxImagePropBounds, 4, sb);
#ifdef TILED
  for (int ty = window[1]; ty < window[3]; ty += TILE) {
    for (int tx = window[0]; tx < window[2]; tx += TILE) {
      const int core[4] = {tx, ty, tx + TILE < window[2] ? tx + TILE : window[2],
                           ty + TILE < window[3] ? ty + TILE : window[3]};
#ifdef IGNORE_CONTEXT
      const int halo = 0;
#else
      const int halo = TILE_HALO;
#endif
      const int box[4] = {clampInt(core[0] - halo, sb[0], sb[2] - 1),
                          clampInt(core[1] - halo, sb[1], sb[3] - 1),
                          clampInt(core[2] + halo, sb[0] + 1, sb[2]),
                          clampInt(core[3] + halo, sb[1] + 1, sb[3])};
      filterRect(srcProps, dstProps, core, box);
    }
  }
#else
#ifdef IGNORE_CONTEXT
  const int box[4] = {window[0], window[1], window[2], window[3]};
#else
  const int box[4] = {sb[0], sb[1], sb[2], sb[3]};
#endif
  filterRect(srcProps, dstProps, window, box);
#endif
}

static OfxStatus render(void* handle, OfxPropertySetHandle inArgs)
{
#ifdef WITH_LABEL
  /* The one thing this build does that the label control does not: write the
   * label. Through the parameter suite, as a real plug-in does, so the value the
   * harness reads had to travel the same path. */
  if (g_params) {
    OfxParamHandle label = 0;
    if (g_params->paramGetHandle((OfxParamSetHandle)handle, "provider", &label, 0) ==
            kOfxStatOK && label) {
      g_params->paramSetValue(label, "CPU (stub, written by render)");
    }
  }
#endif
  OfxImageClipHandle srcClip = 0;
  OfxImageClipHandle dstClip = 0;
  OfxPropertySetHandle srcProps = 0;
  OfxPropertySetHandle dstProps = 0;
  double time = 0.0;
  int window[4] = {0, 0, 0, 0};

  g_props->propGetDouble(inArgs, kOfxPropTime, 0, &time);
  g_props->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, window);

  OfxStatus stat = g_effects->clipGetHandle((OfxImageEffectHandle)handle,
                                            kOfxImageEffectSimpleSourceClipName, &srcClip, 0);
  if (stat != kOfxStatOK) return stat;
  stat = g_effects->clipGetHandle((OfxImageEffectHandle)handle, kOfxImageEffectOutputClipName,
                                  &dstClip, 0);
  if (stat != kOfxStatOK) return stat;
  stat = g_effects->clipGetImage(srcClip, time, 0, &srcProps);
  if (stat != kOfxStatOK) return stat;
  stat = g_effects->clipGetImage(dstClip, time, 0, &dstProps);
  if (stat != kOfxStatOK) return stat;

  filterWindow(srcProps, dstProps, window);

  g_effects->clipReleaseImage(srcProps);
  g_effects->clipReleaseImage(dstProps);
  return kOfxStatOK;
}

static OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                           OfxPropertySetHandle outArgs)
{
  (void)outArgs;
  fetchSuites();

  if (strcmp(action, kOfxActionDescribe) == 0) {
    OfxPropertySetHandle props = (OfxPropertySetHandle)handle;
    g_props->propSetString(props, kOfxImageEffectPropSupportedContexts, 0,
                           kOfxImageEffectContextFilter);
    g_props->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0,
                           kOfxImageEffectRenderInstanceSafe);
    return kOfxStatOK;
  }

  if (strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
    OfxPropertySetHandle srcProps = 0;
    OfxPropertySetHandle dstProps = 0;
    g_effects->clipDefine((OfxImageEffectHandle)handle, kOfxImageEffectSimpleSourceClipName,
                          &srcProps);
    g_props->propSetString(srcProps, kOfxImageEffectPropSupportedComponents, 0,
                           kOfxImageComponentRGBA);
    g_props->propSetString(srcProps, kOfxImageEffectPropSupportedPixelDepths, 0,
                           kOfxBitDepthFloat);
    g_effects->clipDefine((OfxImageEffectHandle)handle, kOfxImageEffectOutputClipName, &dstProps);
    g_props->propSetString(dstProps, kOfxImageEffectPropSupportedComponents, 0,
                           kOfxImageComponentRGBA);
    g_props->propSetString(dstProps, kOfxImageEffectPropSupportedPixelDepths, 0,
                           kOfxBitDepthFloat);
#if defined(NO_LABEL) || defined(WITH_LABEL)
    /* Declared and given a default in both builds. The difference the controls
     * test is whether render() ever writes it; the default is what makes
     * "non-empty" the wrong question. */
    if (g_params) {
      OfxPropertySetHandle label = 0;
      if (g_params->paramDefine((OfxParamSetHandle)handle, kOfxParamTypeString,
                                "provider", &label) == kOfxStatOK && label) {
        g_props->propSetString(label, kOfxParamPropStringMode, 0, kOfxParamStringIsLabel);
        g_props->propSetString(label, kOfxParamPropDefault, 0,
                               "CPU (declared, never written)");
      }
    }
#endif
    return kOfxStatOK;
  }

  if (strcmp(action, kOfxImageEffectActionRender) == 0) {
    return render((void*)handle, inArgs);
  }

  return kOfxStatOK;
}

static OfxPlugin plugin = {
  kOfxImageEffectPluginApi, 1, PLUGIN_ID, 1, 0, setHost, mainEntry
};

int OfxGetNumberOfPlugins(void) { return 1; }

OfxPlugin* OfxGetPlugin(int nth)
{
  return nth == 0 ? &plugin : 0;
}
