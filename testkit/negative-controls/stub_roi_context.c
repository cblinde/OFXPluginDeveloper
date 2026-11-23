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
 * The render window is read with -1 pixel of context on every side and no
 * intermediate rounding, so the arithmetic per pixel does not depend on where
 * the tile or window starts.
 */
#include <string.h>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxProperty.h"

#ifdef IGNORE_CONTEXT
#define PLUGIN_ID "com.testkit.negative.roiignored"
#else
#define PLUGIN_ID "com.testkit.control.roiok"
#endif

static OfxHost* g_host = 0;
static const OfxPropertySuiteV1* g_props = 0;
static const OfxImageEffectSuiteV1* g_effects = 0;

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
}

/* A 3x3 box filter: radius 1, so the context region is one pixel. */
#define RADIUS 1

static void boxFilter(OfxPropertySetHandle srcProps, OfxPropertySetHandle dstProps,
                      const int window[4])
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

  /* Where the taps may come from. This single line is what the control proves:
     with the published bounds the plug-in reads the context it asked for; with
     the window it reads only what it was given and the border is computed from
     clamped taps. */
#ifdef IGNORE_CONTEXT
  const int lo_x = window[0], lo_y = window[1], hi_x = window[2], hi_y = window[3];
#else
  const int lo_x = sb[0], lo_y = sb[1], hi_x = sb[2], hi_y = sb[3];
#endif

  for (int y = window[1]; y < window[3]; ++y) {
    float* out = (float*)((char*)dstData + (long long)(y - db[1]) * dstRowBytes +
                          (long long)(window[0] - db[0]) * 16);
    for (int x = window[0]; x < window[2]; ++x, out += 4) {
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

static OfxStatus render(void* handle, OfxPropertySetHandle inArgs)
{
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

  boxFilter(srcProps, dstProps, window);

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
