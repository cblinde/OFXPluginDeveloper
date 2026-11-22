/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Negative control 2: a plug-in that declares the parameter a host expects as
 * "gain" with the wrong type - OfxParamTypeInteger where the contract (and the
 * plug-in's own documentation) says OfxParamTypeDouble.
 *
 * The describeInContext status is OK and the descriptor is otherwise legal, so
 * only a harness that reads the *declarations* - not just the return codes -
 * can catch this. This is the class of bug that makes a host show an integer
 * slider that a user cannot set to 0.5.
 */
#include <string.h>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"

static OfxHost* g_host = 0;
static const OfxPropertySuiteV1* g_props = 0;
static const OfxParameterSuiteV1* g_params = 0;
static const OfxImageEffectSuiteV1* g_effects = 0;

static void setHost(OfxHost* host) { g_host = host; }

static void fetchSuites(void)
{
  if (!g_host) return;
  if (!g_props) {
    g_props = (const OfxPropertySuiteV1*)g_host->fetchSuite(g_host->host, kOfxPropertySuite, 1);
  }
  if (!g_params) {
    g_params = (const OfxParameterSuiteV1*)g_host->fetchSuite(g_host->host, kOfxParameterSuite, 1);
  }
  if (!g_effects) {
    g_effects = (const OfxImageEffectSuiteV1*)g_host->fetchSuite(g_host->host,
                                                                kOfxImageEffectSuite, 1);
  }
}

static OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                           OfxPropertySetHandle outArgs)
{
  (void)inArgs;
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
    OfxParamSetHandle paramSet = 0;
    OfxPropertySetHandle paramProps = 0;
    g_effects->getParamSet((OfxImageEffectHandle)handle, &paramSet);
    /* The fault: Integer where the contract says Double. */
    g_params->paramDefine(paramSet, kOfxParamTypeInteger, "gain", &paramProps);
    return kOfxStatOK;
  }

  return kOfxStatOK;
}

static OfxPlugin plugin = {
  kOfxImageEffectPluginApi, 1, "com.testkit.negative.badtype", 1, 0, setHost, mainEntry
};

int OfxGetNumberOfPlugins(void) { return 1; }

OfxPlugin* OfxGetPlugin(int nth)
{
  return nth == 0 ? &plugin : 0;
}
