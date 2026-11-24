// SPDX-License-Identifier: BSD-3-Clause
//
// A hand-written OFX host, small enough to read in one sitting.
//
// It serves the five suites the OpenFX Support library insists on
// (kOfxPropertySuite, kOfxImageEffectSuite, kOfxParameterSuite,
// kOfxMultiThreadSuite, kOfxMemorySuite) plus the message suite, and it
// implements the object model those suites address: one plug-in descriptor,
// any number of effect instances each with its own parameter values, the clips
// declared once through the descriptor, and the source/destination images
// handed to a render.
//
// What it is for: driving a real .ofx through describe, describeInContext,
// createInstance, isIdentity, render, destroyInstance and unload inside a
// process with no host application, so a plug-in's declarations, status codes
// and pixels can be asserted in CI. It holds more than one instance at a time
// and can render them from several threads, because the declared render thread
// safety is a claim about exactly that.
//
// What it is not: a conformant host. See references/testing-layers.md L3 for what it
// cannot prove (the multi-resolution/tile protocol, clip preferences, and how a
// real host presents errors). It is a contract test, not a certification.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"

namespace testkit {

struct Impl; // defined in fake_host.cpp

// Which row `kOfxImagePropData` addresses. This is a separate axis from the
// stride's sign: a host may hand out a top-down framebuffer while `bounds`
// still starts at y1 (what a plug-in that assumes top-down gets wrong, and a
// plug-in that trusts the pointer gets right), so the two have to be settable
// independently to build that layout at all.
enum class ImageOrigin { Bottom, Top };

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
  // Creates one more instance and returns its index, or -1 when the plug-in
  // refuses. A host really does hold several instances at once; the contract
  // test uses this to check that two instances keep separate parameter values.
  int createInstanceIndexed(const char* context);
  // Makes the instance at `index` the one that receives isIdentity/render/
  // destroyInstance; -1 (or an out-of-range index) selects none.
  void selectInstance(int index);
  int instanceCount() const;
  OfxStatus destroyInstanceAt(int index);

  // Sets a parameter on one instance without changing the selected instance.
  bool setParamForInstance(int index, const std::string& name, const std::string& value);

  // One render performed by renderConcurrently(); `pixels` is the destination
  // read back tightly packed, top-down, RGBA float.
  struct RenderRecord {
    int instance = -1;
    double time = 0.0;
    OfxStatus status = kOfxStatOK;
    std::vector<float> pixels;
  };
  // Renders `iterations` frames on one thread per entry of `instances`, each
  // thread bound to that instance (the same index repeated means the same
  // instance from several threads). Each worker gets its own source and
  // destination images, so a race reported here belongs to the plug-in.
  std::vector<RenderRecord> renderConcurrently(const std::vector<int>& instances, int iterations,
                                               const OfxRectI& window);
  // On kOfxStatOK the identity clip name is written to *identityClip (the
  // host-side outArgs property is kOfxPropName).
  OfxStatus isIdentity(double time, const OfxRectI& window, std::string* identityClip);
  OfxStatus render(double time, const OfxRectI& window);
  // Sends kOfxImageEffectActionGetRegionsOfInterest and reads the answer for the
  // source clip. The plug-in reports the answer through the Support library as
  // one property per clip ("OfxImageClipPropRoI_<clip>"), so this is the only
  // way a test sees it: `sourceRoi` is written only when the action answered
  // kOfxStatOK, i.e. when the plug-in actually set a region. A plug-in that
  // leaves the region alone answers kOfxStatReplyDefault and is visible as such.
  OfxStatus getRegionsOfInterest(double time, const OfxRectD& requested, OfxRectD* sourceRoi);
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
  // Publishes a source region of definition inset by `pixels` on every side
  // while the buffer keeps its size, the way a cropped or transition input
  // arrives. Image::getPixelAddress() then refuses the margin, so a render
  // window over the full buffer covers pixels the source does not have. The
  // destination image is left alone, so a test can compare its pixels across a
  // rejected render.
  void setSourceBoundsInset(int pixels);
  // Publishes a destination region of definition inset by `pixels` on every
  // side while the render window still covers the full frame, the way a proxy or
  // wrong bounds looks from the write side. A plug-in that addresses the
  // destination through (window - bounds) then writes outside the pixels it was
  // given; the buffer carries a margin, so that write is a pixel change the test
  // can see instead of a heap overflow. destinationBuffer() reads it back.
  void setDestinationBoundsInset(int pixels);
  // A host that ignores the region the plug-in declared in
  // kOfxImageEffectActionGetRegionsOfInterest hands over only the window it asked
  // for: the source image is cropped to that window, so a plug-in that reads
  // outside it reads pixels the host never supplied. Off by default - the host
  // then hands over the whole frame, which is the context a host that honours
  // the declaration gives. Host thread only (not for renderConcurrently).
  void setSourceCroppedToWindow(bool on);
  void setSourcePlanar(const float* rgb, int width, int height); // 3 channels
  void setSourceConstant(float r, float g, float b, float a);
  void setPaddedRows(bool on);           // rowBytes > width*16, poisoned margin
  void setNegativePitch(bool on);        // rowBytes < 0
  void setImageOrigin(ImageOrigin o);    // which row kOfxImagePropData addresses
  void setDestinationByteImage(bool on); // hand the plug-in a byte image

  // Destination read back tightly packed, top-down, RGBA float.
  std::vector<float> destination() const;
  // The whole destination allocation in memory order, margins included; use it
  // to prove that nothing was written outside the declared bounds.
  std::vector<float> destinationBuffer() const;
  int imageWidth() const;
  int imageHeight() const;
  long long destinationRowBytes() const;

  // Sets a string property on a declared clip's property set, the way a host
  // names the media a clip currently carries (a file name, a media id). The clip
  // handle stays the same; only the string changes, which is what a plug-in has
  // to notice to invalidate a cache keyed on the handle. Returns false when the
  // clip name is unknown.
  bool setClipProperty(const std::string& clip, const std::string& name,
                       const std::string& value);
  // Reads that property back through the property suite; empty when the clip or
  // the property is unknown.
  std::string clipProperty(const std::string& clip, const std::string& name) const;

  // Sets a parameter's value from its declared type (numeric or string), the
  // same call a host makes through paramSetValueAtTime.
  bool setParam(const std::string& name, const std::string& value);

  // Clicks a parameter the way a host does for a push button: sends
  // kOfxActionInstanceChanged with kOfxPropType=parameter, the parameter's
  // name, kOfxChangeUserEdited and the render scale Support reads. Returns
  // false only when the plug-in rejects the action (kOfxStatFailed or
  // kOfxStatErrBadHandle); kOfxStatReplyDefault is a handled notification.
  bool pressParam(const std::string& name);

  // A parameter's current value as text, for a test that has to see what the
  // plug-in wrote: doubles as %g, ints/booleans/choices as the index, strings
  // verbatim, colours as r,g,b[,a]. Empty when the name is unknown or the type
  // has no textual form (push button, group).
  std::string paramValue(const std::string& name) const;

  // True once the plug-in itself wrote this parameter. A declared default is
  // not evidence: a label that still carries it has never been set by anyone,
  // which is exactly the failure a "the label must name the provider" check is
  // meant to catch.
  bool paramIsSet(const std::string& name) const;

  // The kOfxParamPropChoiceOption strings a parameter was declared with, in
  // order; empty when the name is unknown or the parameter is not a choice.
  std::vector<std::string> paramOptions(const std::string& name) const;

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
