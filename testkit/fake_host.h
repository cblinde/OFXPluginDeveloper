// SPDX-License-Identifier: BSD-3-Clause
//
// A hand-written OFX host, small enough to read in one sitting.
//
// It serves the five suites the OpenFX Support library insists on
// (kOfxPropertySuite, kOfxImageEffectSuite, kOfxParameterSuite,
// kOfxMultiThreadSuite, kOfxMemorySuite) plus the message suite, and it
// implements the object model those suites address: one plug-in descriptor,
// one effect instance, the clips and parameters declared through it, and the
// source/destination images handed to a render.
//
// What it is for: driving a real .ofx through describe, describeInContext,
// createInstance, isIdentity, render, destroyInstance and unload inside a
// process with no host application, so a plug-in's declarations, status codes
// and pixels can be asserted in CI.
//
// What it is not: a conformant host. See TESTING_STRATEGY.md L3 for the three
// things it cannot prove (the multi-resolution/tile protocol, clip preferences
// and host scheduling, and anything about how a real host presents errors).
// It is a contract test, not a certification.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"

namespace testkit {

struct Impl; // defined in fake_host.cpp

class FakeHost {
public:
  FakeHost();
  ~FakeHost();
  FakeHost(const FakeHost&) = delete;
  FakeHost& operator=(const FakeHost&) = delete;

  // Calls the plug-in's setHost(). Must happen before load().
  void attach(OfxPlugin* plugin);

  OfxStatus load();
  OfxStatus describe();
  OfxStatus describeInContext(const char* context);
  OfxStatus createInstance(const char* context);
  // On kOfxStatOK the identity clip name is written to *identityClip (the
  // host-side outArgs property is kOfxPropName).
  OfxStatus isIdentity(double time, const OfxRectI& window, std::string* identityClip);
  OfxStatus render(double time, const OfxRectI& window);
  OfxStatus destroyInstance();
  OfxStatus unload();

  // Declarations observed during describeInContext: parameter name -> the type
  // string passed to paramDefine, and clip names in declaration order.
  std::map<std::string, std::string> declaredParams() const;
  std::vector<std::string> declaredClips() const;

  // Reads a property the plug-in set on its descriptor (e.g.
  // kOfxImageEffectPropSupportedContexts, kOfxImageEffectPropCudaRenderSupported).
  std::string descriptorString(const char* property, int index = 0) const;
  int descriptorInt(const char* property, int index = 0) const;

  // Image setup. The default is a 64x48 RGBA32F constant-colour source.
  void setImageSize(int width, int height);
  void setSourcePlanar(const float* rgb, int width, int height); // 3 channels
  void setSourceConstant(float r, float g, float b, float a);
  void setPaddedRows(bool on);           // rowBytes > width*16, poisoned margin
  void setNegativePitch(bool on);        // rowBytes < 0
  void setDestinationByteImage(bool on); // hand the plug-in a byte image

  // Destination read back tightly packed, top-down, RGBA float.
  std::vector<float> destination() const;
  int imageWidth() const;
  int imageHeight() const;
  long long destinationRowBytes() const;

  // Sets a parameter's value from its declared type (numeric or string), the
  // same call a host makes through paramSetValueAtTime.
  bool setParam(const std::string& name, const std::string& value);

  std::vector<std::string> messages() const;
  std::string messageText() const;

private:
  Impl* impl_;
  OfxPlugin* plugin_ = nullptr;

  OfxStatus mainEntry(const char* action, void* outArgs, void* inArgs);
  OfxStatus pluginMain(const char* action);
  OfxStatus pluginMainWithContext(const char* action, const char* context);
  void fillTimeArgs(void* inArgs, double time, const OfxRectI& window);
  void invalidateImages();
};

} // namespace testkit
