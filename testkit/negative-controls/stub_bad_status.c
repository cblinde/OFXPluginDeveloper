/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Negative control 1: a plug-in that answers kOfxActionDescribe with
 * kOfxStatReplyDefault. This is not a plugin bug a compiler can catch - it is
 * exactly what an over-eager `return` inside a switch produces - so the harness
 * has to catch it by checking the status code.
 *
 * Deliberately does NOT link the OFX Support library: the control must fail for
 * the reason under test, not because the plug-in is built differently.
 */
#include <string.h>

#include "ofxCore.h"
#include "ofxImageEffect.h"

static OfxHost* g_host = 0;

static void setHost(OfxHost* host) { g_host = host; }

static OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                           OfxPropertySetHandle outArgs)
{
  (void)handle;
  (void)inArgs;
  (void)outArgs;
  (void)g_host;
  if (strcmp(action, kOfxActionDescribe) == 0) {
    /* The fault: a describe that declines to describe. */
    return kOfxStatReplyDefault;
  }
  return kOfxStatOK;
}

static OfxPlugin plugin = {
  kOfxImageEffectPluginApi, 1, "com.testkit.negative.badstatus", 1, 0, setHost, mainEntry
};

int OfxGetNumberOfPlugins(void) { return 1; }

OfxPlugin* OfxGetPlugin(int nth)
{
  return nth == 0 ? &plugin : 0;
}
