// SPDX-License-Identifier: BSD-3-Clause
//
// Implementation of the fake host declared in fake_host.h. Everything here is
// deliberately small and readable: the point is a harness an author can debug,
// not a host implementation.

#include "fake_host.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ofxGPURender.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProperty.h"

namespace testkit {
namespace {

enum ValueKind { kString = 1, kInt = 2, kDouble = 3, kPointer = 4 };
enum OwnerKind { kEffect = 1, kClip = 2, kParam = 3, kImage = 4 };

struct Value {
  int kind = 0;
  std::string s;
  int i = 0;
  double d = 0.0;
  void* p = nullptr;
};

// Every handle this host hands out is a PropSet*. That is a simplification of
// the real OFX object model (where a clip handle and its property set are
// distinct), and the only place it matters is code that assumes a handle is
// not a property set - which the Support library never does.
struct PropSet {
  std::map<std::string, std::vector<Value>> props;
  void* owner = nullptr;
  int ownerKind = 0;
};

struct ImageObj;
struct ClipObj {
  std::string name;
  PropSet props;
  ImageObj* image = nullptr;
};

struct ParamObj {
  std::string name;
  std::string type;
  PropSet props;
  bool isSet = false;
  double d = 0.0;
  int i = 0;
  std::string s;
  double color[4] = {0.0, 0.0, 0.0, 0.0};
};

struct ImageObj {
  PropSet props;
  int w = 0;
  int h = 0;
  int boundsInset = 0; // published RoD is inset by this much inside the buffer
  long long rowBytes = 0;
  void* data = nullptr;
  bool isByte = false;
  std::vector<float> floats;
  std::vector<unsigned char> bytes;
  const float* floatData = nullptr; // typed view of data, for read-back
  long long pitchFloats = 0;
};

// One effect instance. Parameters live here, not in Impl: two live instances
// must be able to hold different values for the same parameter, because that is
// exactly what a host does when it renders two clips at once.
struct EffectObj {
  PropSet props;
  PropSet paramSet;
  std::map<std::string, ParamObj*> params;
};

constexpr int kPadPixels = 5; // padding so rowBytes != width*16

} // namespace

struct Impl {
  OfxHost host{};
  PropSet hostProps;

  OfxPropertySuiteV1 propSuite{};
  OfxImageEffectSuiteV1 effectSuite{};
  OfxParameterSuiteV1 paramSuite{};
  OfxMultiThreadSuiteV1 threadSuite{};
  OfxMemorySuiteV1 memorySuite{};
  OfxMessageSuiteV1 messageSuite{};
  OfxMessageSuiteV2 messageSuiteV2{};

  EffectObj pluginDesc;
  std::vector<EffectObj*> instances;
  EffectObj* current = nullptr; // instance that receives host actions
  // True only while renderConcurrently() runs: each worker then gets its own
  // source/destination images instead of the shared ones.
  bool parallel = false;

  std::vector<ClipObj*> clips;
  std::vector<ParamObj*> params;
  std::map<std::string, ClipObj*> clipByName;

  // images
  int imgW = 64;
  int imgH = 48;
  bool padded = true;
  bool negativePitch = false;
  ImageOrigin origin = ImageOrigin::Bottom;
  bool byteDst = false;
  int srcInset = 0; // see FakeHost::setSourceBoundsInset
  int dstInset = 0; // see FakeHost::setDestinationBoundsInset
  std::vector<float> srcRGBA; // tight, top-down
  ImageObj* srcImage = nullptr;
  ImageObj* dstImage = nullptr;

  std::vector<std::string> messages;

  Impl();
  void reset();

  ~Impl() {
    for (EffectObj* e : instances) delete e;
    for (ClipObj* c : clips) delete c;
    for (ParamObj* p : params) delete p;
    delete srcImage;
    delete dstImage;
  }
};

namespace {

Impl* g = nullptr;

// The effect a call belongs to when it arrives from a render worker; see
// renderConcurrently(). Empty on the host thread, where Impl::current decides.
thread_local EffectObj* tlsEffect = nullptr;
struct ThreadImages {
  ImageObj* src = nullptr;
  ImageObj* dst = nullptr;
};
thread_local ThreadImages tlsImages;

// messageFn() can be called from any render thread; the vector behind it is
// shared host state, so it gets a lock even though nothing else here needs one.
std::mutex gMessageMutex;

// ---------------------------------------------------------------- property set

PropSet* ps(OfxPropertySetHandle h) { return reinterpret_cast<PropSet*>(h); }
PropSet* ps(OfxParamHandle h) { return reinterpret_cast<PropSet*>(h); }
PropSet* ps(OfxImageClipHandle h) { return reinterpret_cast<PropSet*>(h); }
PropSet* ps(OfxParamSetHandle h) { return reinterpret_cast<PropSet*>(h); }

template <typename T>
OfxPropertySetHandle handleOf(T* p) {
  return reinterpret_cast<OfxPropertySetHandle>(p);
}

std::vector<Value>* slot(PropSet* s, const char* name, int index) {
  if (!s || !name || index < 0) return nullptr;
  std::vector<Value>& vec = s->props[name];
  if (static_cast<int>(vec.size()) <= index) vec.resize(index + 1);
  return &vec;
}

const Value* slotConst(PropSet* s, const char* name, int index) {
  if (!s || !name || index < 0) return nullptr;
  auto it = s->props.find(name);
  if (it == s->props.end() || static_cast<int>(it->second.size()) <= index) return nullptr;
  return &it->second[index];
}

OfxStatus setString(PropSet* s, const char* n, int i, const char* v) {
  std::vector<Value>* vec = slot(s, n, i);
  if (!vec) return kOfxStatErrBadHandle;
  Value& val = (*vec)[i];
  val.kind = kString;
  val.s = v ? v : "";
  return kOfxStatOK;
}
OfxStatus setInt(PropSet* s, const char* n, int i, int v) {
  std::vector<Value>* vec = slot(s, n, i);
  if (!vec) return kOfxStatErrBadHandle;
  Value& val = (*vec)[i];
  val.kind = kInt;
  val.i = v;
  return kOfxStatOK;
}
OfxStatus setDouble(PropSet* s, const char* n, int i, double v) {
  std::vector<Value>* vec = slot(s, n, i);
  if (!vec) return kOfxStatErrBadHandle;
  Value& val = (*vec)[i];
  val.kind = kDouble;
  val.d = v;
  return kOfxStatOK;
}
OfxStatus setPointer(PropSet* s, const char* n, int i, void* v) {
  std::vector<Value>* vec = slot(s, n, i);
  if (!vec) return kOfxStatErrBadHandle;
  Value& val = (*vec)[i];
  val.kind = kPointer;
  val.p = v;
  return kOfxStatOK;
}

// Missing properties read as zero/empty rather than failing. A conformant host
// returns kOfxStatErrUnknown; the Support library's wrappers then either throw
// or substitute a default depending on the call site, which turns a harness
// omission into an exception from three frames away. Zero is easier to debug,
// and the harness never depends on a property it did not set.
OfxStatus getString(PropSet* s, const char* n, int i, char** v) {
  const Value* val = slotConst(s, n, i);
  static char empty[] = "";
  if (!v) return kOfxStatErrBadHandle;
  *v = (val && val->kind == kString) ? const_cast<char*>(val->s.c_str()) : empty;
  return kOfxStatOK;
}
OfxStatus getInt(PropSet* s, const char* n, int i, int* v) {
  const Value* val = slotConst(s, n, i);
  if (!v) return kOfxStatErrBadHandle;
  *v = (val && val->kind == kInt) ? val->i : 0;
  return kOfxStatOK;
}
OfxStatus getDouble(PropSet* s, const char* n, int i, double* v) {
  const Value* val = slotConst(s, n, i);
  if (!v) return kOfxStatErrBadHandle;
  *v = (val && val->kind == kDouble) ? val->d : 0.0;
  return kOfxStatOK;
}
OfxStatus getPointer(PropSet* s, const char* n, int i, void** v) {
  const Value* val = slotConst(s, n, i);
  if (!v) return kOfxStatErrBadHandle;
  *v = (val && val->kind == kPointer) ? val->p : nullptr;
  return kOfxStatOK;
}

// ------------------------------------------------------------------ the suites

OfxStatus propSetPointerFn(OfxPropertySetHandle h, const char* n, int i, void* v) {
  return setPointer(ps(h), n, i, v);
}
OfxStatus propSetStringFn(OfxPropertySetHandle h, const char* n, int i, const char* v) {
  return setString(ps(h), n, i, v);
}
OfxStatus propSetDoubleFn(OfxPropertySetHandle h, const char* n, int i, double v) {
  return setDouble(ps(h), n, i, v);
}
OfxStatus propSetIntFn(OfxPropertySetHandle h, const char* n, int i, int v) {
  return setInt(ps(h), n, i, v);
}
OfxStatus propSetPointerNFn(OfxPropertySetHandle h, const char* n, int count, void* const* v) {
  for (int i = 0; i < count; ++i) setPointer(ps(h), n, i, v[i]);
  return kOfxStatOK;
}
OfxStatus propSetStringNFn(OfxPropertySetHandle h, const char* n, int count, const char* const* v) {
  for (int i = 0; i < count; ++i) setString(ps(h), n, i, v[i]);
  return kOfxStatOK;
}
OfxStatus propSetDoubleNFn(OfxPropertySetHandle h, const char* n, int count, const double* v) {
  for (int i = 0; i < count; ++i) setDouble(ps(h), n, i, v[i]);
  return kOfxStatOK;
}
OfxStatus propSetIntNFn(OfxPropertySetHandle h, const char* n, int count, const int* v) {
  for (int i = 0; i < count; ++i) setInt(ps(h), n, i, v[i]);
  return kOfxStatOK;
}
OfxStatus propGetPointerFn(OfxPropertySetHandle h, const char* n, int i, void** v) {
  return getPointer(ps(h), n, i, v);
}
OfxStatus propGetStringFn(OfxPropertySetHandle h, const char* n, int i, char** v) {
  return getString(ps(h), n, i, v);
}
OfxStatus propGetDoubleFn(OfxPropertySetHandle h, const char* n, int i, double* v) {
  return getDouble(ps(h), n, i, v);
}
OfxStatus propGetIntFn(OfxPropertySetHandle h, const char* n, int i, int* v) {
  return getInt(ps(h), n, i, v);
}
OfxStatus propGetPointerNFn(OfxPropertySetHandle h, const char* n, int count, void** v) {
  for (int i = 0; i < count; ++i) getPointer(ps(h), n, i, &v[i]);
  return kOfxStatOK;
}
OfxStatus propGetStringNFn(OfxPropertySetHandle h, const char* n, int count, char** v) {
  for (int i = 0; i < count; ++i) getString(ps(h), n, i, &v[i]);
  return kOfxStatOK;
}
OfxStatus propGetDoubleNFn(OfxPropertySetHandle h, const char* n, int count, double* v) {
  for (int i = 0; i < count; ++i) getDouble(ps(h), n, i, &v[i]);
  return kOfxStatOK;
}
OfxStatus propGetIntNFn(OfxPropertySetHandle h, const char* n, int count, int* v) {
  for (int i = 0; i < count; ++i) getInt(ps(h), n, i, &v[i]);
  return kOfxStatOK;
}
OfxStatus propResetFn(OfxPropertySetHandle h, const char* n) {
  PropSet* s = ps(h);
  if (!s || !n) return kOfxStatErrBadHandle;
  s->props.erase(n);
  return kOfxStatOK;
}
OfxStatus propGetDimensionFn(OfxPropertySetHandle h, const char* n, int* count) {
  PropSet* s = ps(h);
  if (!s || !n || !count) return kOfxStatErrBadHandle;
  auto it = s->props.find(n);
  *count = (it == s->props.end()) ? 0 : static_cast<int>(it->second.size());
  return kOfxStatOK;
}

// ------------------------------------------------------------------ parameters

EffectObj* effectOf(OfxPropertySetHandle h) {
  if (!h) return nullptr;
  PropSet* s = ps(h);
  return (s->ownerKind == kEffect) ? static_cast<EffectObj*>(s->owner) : nullptr;
}

// Descriptor-level parameters are templates; an instance gets its own copy the
// first time it asks for the handle, with the same defaults and an unset value.
ParamObj* ensureParam(EffectObj* effect, const std::string& name) {
  if (!effect) return nullptr;
  auto it = effect->params.find(name);
  if (it != effect->params.end()) return it->second;
  auto tmpl = g->pluginDesc.params.find(name);
  if (tmpl == g->pluginDesc.params.end()) return nullptr;
  ParamObj* copy = new ParamObj(*tmpl->second);
  copy->props.owner = copy; // the copy must not point at the template
  copy->props.ownerKind = kParam;
  copy->isSet = false;
  g->params.push_back(copy);
  effect->params[name] = copy;
  return copy;
}

ParamObj* paramOf(OfxParamHandle p) {
  if (!p) return nullptr;
  PropSet* s = ps(p);
  return (s->ownerKind == kParam) ? static_cast<ParamObj*>(s->owner) : nullptr;
}

double paramDouble(ParamObj* p) {
  if (p->isSet) return p->d;
  const Value* def = slotConst(&p->props, kOfxParamPropDefault, 0);
  return (def && def->kind == kDouble) ? def->d : 0.0;
}
int paramInt(ParamObj* p) {
  if (p->isSet) return p->i;
  const Value* def = slotConst(&p->props, kOfxParamPropDefault, 0);
  return (def && def->kind == kInt) ? def->i : 0;
}
std::string paramString(ParamObj* p) {
  if (p->isSet) return p->s;
  const Value* def = slotConst(&p->props, kOfxParamPropDefault, 0);
  return (def && def->kind == kString) ? def->s : std::string();
}

// Colour components: the value once set, otherwise the descriptor's default
// slots (kOfxParamPropDefault is dimension 3 for RGB and 4 for RGBA).
void paramColor(ParamObj* p, double* out, int count) {
  for (int c = 0; c < count; ++c) {
    if (p->isSet) {
      out[c] = p->color[c];
    } else {
      const Value* def = slotConst(&p->props, kOfxParamPropDefault, c);
      out[c] = (def && def->kind == kDouble) ? def->d : 0.0;
    }
  }
}

bool isType(const ParamObj* p, const char* t) { return p->type == t; }

// A host that silently refuses a parameter type makes the harness blame the
// plug-in: the failing action is render and nothing names the parameter. Say
// which parameter and type, and put it in messages() so a test can assert it.
void reportUnsupportedParam(const ParamObj* p, const char* action) {
  char line[512];
  std::snprintf(line, sizeof(line), "fake host: %s of parameter '%s' (%s) is not modelled",
                action, p->name.c_str(), p->type.c_str());
  std::fprintf(stderr, "%s\n", line);
  std::lock_guard<std::mutex> lock(gMessageMutex);
  g->messages.push_back(std::string("[host] ") + line);
}

OfxStatus paramDefineFn(OfxParamSetHandle paramSet, const char* paramType, const char* name,
                        OfxPropertySetHandle* propertySet) {
  if (!paramType || !name || !propertySet) return kOfxStatErrBadHandle;
  EffectObj* effect = effectOf(reinterpret_cast<OfxPropertySetHandle>(paramSet));
  if (!effect) return kOfxStatErrBadHandle;
  ParamObj* p = new ParamObj();
  p->name = name;
  p->type = paramType;
  p->props.owner = p;
  p->props.ownerKind = kParam;
  setString(&p->props, kOfxParamPropType, 0, paramType);
  setString(&p->props, kOfxPropName, 0, name);
  setString(&p->props, kOfxPropLabel, 0, name);
  g->params.push_back(p);
  effect->params[name] = p;
  *propertySet = handleOf(&p->props);
  return kOfxStatOK;
}

OfxStatus paramGetHandleFn(OfxParamSetHandle paramSet, const char* name, OfxParamHandle* param,
                           OfxPropertySetHandle* propertySet) {
  if (!name) return kOfxStatErrBadHandle;
  EffectObj* effect = effectOf(reinterpret_cast<OfxPropertySetHandle>(paramSet));
  ParamObj* p = ensureParam(effect, name);
  if (!p) return kOfxStatErrBadHandle;
  if (param) *param = reinterpret_cast<OfxParamHandle>(&p->props);
  if (propertySet) *propertySet = handleOf(&p->props);
  return kOfxStatOK;
}

OfxStatus paramSetGetPropertySetFn(OfxParamSetHandle paramSet, OfxPropertySetHandle* prop) {
  if (!paramSet || !prop) return kOfxStatErrBadHandle;
  *prop = handleOf(ps(paramSet));
  return kOfxStatOK;
}

OfxStatus paramGetPropertySetFn(OfxParamHandle param, OfxPropertySetHandle* prop) {
  if (!param || !prop) return kOfxStatErrBadHandle;
  *prop = handleOf(ps(param));
  return kOfxStatOK;
}

OfxStatus paramGetValueFn(OfxParamHandle param, ...) {
  ParamObj* p = paramOf(param);
  if (!p) return kOfxStatErrBadHandle;
  va_list ap;
  va_start(ap, param);
  OfxStatus st = kOfxStatErrUnsupported;
  if (isType(p, kOfxParamTypeDouble)) {
    *va_arg(ap, double*) = paramDouble(p);
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeInteger) || isType(p, kOfxParamTypeBoolean) ||
             isType(p, kOfxParamTypeChoice)) {
    *va_arg(ap, int*) = paramInt(p);
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeString)) {
    thread_local std::string s; // render workers read string parameters too
    s = paramString(p);
    *va_arg(ap, char**) = const_cast<char*>(s.c_str());
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeRGB)) {
    double c[3];
    paramColor(p, c, 3);
    for (double v : c) *va_arg(ap, double*) = v;
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeRGBA)) {
    double c[4];
    paramColor(p, c, 4);
    for (double v : c) *va_arg(ap, double*) = v;
    st = kOfxStatOK;
  }
  va_end(ap);
  if (st != kOfxStatOK) reportUnsupportedParam(p, "read");
  return st;
}

OfxStatus paramGetValueAtTimeFn(OfxParamHandle param, OfxTime time, ...) {
  ParamObj* p = paramOf(param);
  if (!p) return kOfxStatErrBadHandle;
  (void)time;
  va_list ap;
  va_start(ap, time);
  OfxStatus st = kOfxStatErrUnsupported;
  if (isType(p, kOfxParamTypeDouble)) {
    *va_arg(ap, double*) = paramDouble(p);
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeInteger) || isType(p, kOfxParamTypeBoolean) ||
             isType(p, kOfxParamTypeChoice)) {
    *va_arg(ap, int*) = paramInt(p);
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeString)) {
    thread_local std::string s; // render workers read string parameters too
    s = paramString(p);
    *va_arg(ap, char**) = const_cast<char*>(s.c_str());
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeRGB)) {
    double c[3];
    paramColor(p, c, 3);
    for (double v : c) *va_arg(ap, double*) = v;
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeRGBA)) {
    double c[4];
    paramColor(p, c, 4);
    for (double v : c) *va_arg(ap, double*) = v;
    st = kOfxStatOK;
  }
  va_end(ap);
  if (st != kOfxStatOK) reportUnsupportedParam(p, "read at time");
  return st;
}

OfxStatus paramGetDerivativeFn(OfxParamHandle param, OfxTime time, ...) {
  ParamObj* p = paramOf(param);
  if (!p) return kOfxStatErrBadHandle;
  (void)time;
  va_list ap;
  va_start(ap, time);
  if (isType(p, kOfxParamTypeDouble)) {
    *va_arg(ap, double*) = 0.0;
  } else if (isType(p, kOfxParamTypeInteger)) {
    *va_arg(ap, int*) = 0;
  }
  va_end(ap);
  return kOfxStatOK;
}

OfxStatus paramGetIntegralFn(OfxParamHandle param, OfxTime time1, OfxTime time2, ...) {
  ParamObj* p = paramOf(param);
  if (!p) return kOfxStatErrBadHandle;
  (void)time1;
  (void)time2;
  va_list ap;
  va_start(ap, time2);
  if (isType(p, kOfxParamTypeDouble)) {
    *va_arg(ap, double*) = 0.0;
  } else if (isType(p, kOfxParamTypeInteger)) {
    *va_arg(ap, int*) = 0;
  }
  va_end(ap);
  return kOfxStatOK;
}

OfxStatus paramSetValueFn(OfxParamHandle param, ...) {
  ParamObj* p = paramOf(param);
  if (!p) return kOfxStatErrBadHandle;
  va_list ap;
  va_start(ap, param);
  OfxStatus st = kOfxStatErrUnsupported;
  // OFX passes scalars by value: Support calls paramSetValue(handle, v) with
  // the double/int itself, and only strings as a pointer. Reading a `double*`
  // here interprets the value's bits as an address and crashes on the first
  // write a plug-in makes (1.0 is 0x3FF0000000000000).
  if (isType(p, kOfxParamTypeDouble)) {
    p->d = va_arg(ap, double);
    p->isSet = true;
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeInteger) || isType(p, kOfxParamTypeBoolean) ||
             isType(p, kOfxParamTypeChoice)) {
    p->i = va_arg(ap, int);
    p->isSet = true;
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeString)) {
    const char* v = va_arg(ap, const char*);
    p->s = v ? v : "";
    p->isSet = true;
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeRGB)) {
    // Support passes colour components by value, not by pointer.
    for (int c = 0; c < 3; ++c) p->color[c] = va_arg(ap, double);
    p->isSet = true;
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeRGBA)) {
    for (int c = 0; c < 4; ++c) p->color[c] = va_arg(ap, double);
    p->isSet = true;
    st = kOfxStatOK;
  }
  va_end(ap);
  if (st != kOfxStatOK) reportUnsupportedParam(p, "write");
  return st;
}

OfxStatus paramSetValueAtTimeFn(OfxParamHandle param, OfxTime time, ...) {
  ParamObj* p = paramOf(param);
  if (!p) return kOfxStatErrBadHandle;
  (void)time;
  va_list ap;
  va_start(ap, time);
  OfxStatus st = kOfxStatErrUnsupported;
  // Scalars by value, as in paramSetValueFn above; RGB/RGBA components are
  // scalars too.
  if (isType(p, kOfxParamTypeDouble)) {
    p->d = va_arg(ap, double);
    p->isSet = true;
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeInteger) || isType(p, kOfxParamTypeBoolean) ||
             isType(p, kOfxParamTypeChoice)) {
    p->i = va_arg(ap, int);
    p->isSet = true;
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeString)) {
    const char* v = va_arg(ap, const char*);
    p->s = v ? v : "";
    p->isSet = true;
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeRGB)) {
    for (int c = 0; c < 3; ++c) p->color[c] = va_arg(ap, double);
    p->isSet = true;
    st = kOfxStatOK;
  } else if (isType(p, kOfxParamTypeRGBA)) {
    for (int c = 0; c < 4; ++c) p->color[c] = va_arg(ap, double);
    p->isSet = true;
    st = kOfxStatOK;
  }
  va_end(ap);
  if (st != kOfxStatOK) reportUnsupportedParam(p, "write at time");
  return st;
}

OfxStatus paramGetNumKeysFn(OfxParamHandle, unsigned int* n) {
  if (n) *n = 0;
  return kOfxStatOK;
}
OfxStatus paramGetKeyTimeFn(OfxParamHandle, unsigned int, OfxTime*) { return kOfxStatErrUnsupported; }
OfxStatus paramGetKeyIndexFn(OfxParamHandle, OfxTime, int, int*) { return kOfxStatErrUnsupported; }
OfxStatus paramDeleteKeyFn(OfxParamHandle, OfxTime) { return kOfxStatOK; }
OfxStatus paramDeleteAllKeysFn(OfxParamHandle) { return kOfxStatOK; }
OfxStatus paramCopyFn(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD*) { return kOfxStatOK; }
OfxStatus paramEditBeginFn(OfxParamSetHandle, const char*) { return kOfxStatOK; }
OfxStatus paramEditEndFn(OfxParamSetHandle) { return kOfxStatOK; }

// --------------------------------------------------------------- image effect

ClipObj* clipOf(OfxImageClipHandle c) {
  if (!c) return nullptr;
  PropSet* s = ps(c);
  return (s->ownerKind == kClip) ? static_cast<ClipObj*>(s->owner) : nullptr;
}

void buildSourceImage(ImageObj* img) {
  img->w = g->imgW;
  img->h = g->imgH;
  img->boundsInset = g->srcInset;
  const int pitchPixels = g->padded ? img->w + kPadPixels : img->w;
  const long long bytesPerRow = static_cast<long long>(pitchPixels) * 16;
  const bool negative = g->negativePitch;
  // Image row y sits at memory row (h-1-y) when exactly one of the two axes -
  // where the pointer starts and which way the stride runs - is "from the end".
  const bool reversed = (g->origin == ImageOrigin::Top) != negative;
  img->isByte = false;
  img->floats.assign(static_cast<size_t>(pitchPixels) * img->h * 4, 0.0f);
  float* base = img->floats.data();
  for (int y = 0; y < img->h; ++y) {
    const int memRow = reversed ? (img->h - 1 - y) : y;
    float* row = base + static_cast<size_t>(memRow) * pitchPixels * 4;
    for (int x = 0; x < img->w; ++x) {
      for (int c = 0; c < 4; ++c) {
        row[x * 4 + c] = (c < 3) ? g->srcRGBA[(static_cast<size_t>(y) * img->w + x) * 4 + c]
                                 : g->srcRGBA[(static_cast<size_t>(y) * img->w + x) * 4 + 3];
      }
    }
    // Poison the padding so a plug-in that steps by width instead of rowBytes
    // shows up as a mismatch rather than passing on coincidentally-equal values.
    for (int x = img->w; x < pitchPixels; ++x) {
      for (int c = 0; c < 4; ++c) row[x * 4 + c] = -12345.0f;
    }
  }
  // Read-back walks image rows, not the way the plug-in's pointer happens to.
  const int readRow = reversed ? (img->h - 1) : 0;
  img->floatData = base + static_cast<size_t>(readRow) * pitchPixels * 4;
  img->pitchFloats = static_cast<long long>(pitchPixels) * 4 * (reversed ? -1 : 1);
  // The pointer a plug-in sees: its row follows the pitch sign only, so the Top
  // origin hands out the last row while bounds still starts at y1.
  const int dataRow = negative ? (img->h - 1) : 0;
  img->data = base + static_cast<size_t>(dataRow) * pitchPixels * 4;
  img->rowBytes = negative ? -bytesPerRow : bytesPerRow;
}

void buildDstImage(ImageObj* img) {
  img->w = g->imgW;
  img->h = g->imgH;
  img->isByte = g->byteDst;
  img->boundsInset = g->dstInset;
  const int inset = img->boundsInset;
  // An inset destination is smaller than the window a test asks for, and a
  // plug-in that ignores the inset addresses before the declared region. Give
  // the buffer that margin on every side, so that write stays inside the
  // allocation: the check is "was anything written outside the bounds", which a
  // readback can see, and not "did the heap survive", which needs a sanitizer
  // and a coin flip.
  const int rows = img->h + 2 * inset;
  const int pitchPixels = img->w + 2 * inset + (g->padded ? kPadPixels : 0);
  const long long bytesPerPixel = img->isByte ? 4 : 16;
  const long long bytesPerRow = static_cast<long long>(pitchPixels) * bytesPerPixel;
  const bool negative = g->negativePitch;
  const bool reversed = (g->origin == ImageOrigin::Top) != negative;
  img->floatData = nullptr;
  img->pitchFloats = 0;
  char* base = nullptr;
  if (img->isByte) {
    img->bytes.assign(static_cast<size_t>(pitchPixels) * rows * 4, 0);
    base = reinterpret_cast<char*>(img->bytes.data());
  } else {
    img->floats.assign(static_cast<size_t>(pitchPixels) * rows * 4, 0.0f);
    base = reinterpret_cast<char*>(img->floats.data());
  }
  const int dataRow = negative ? (rows - 1) : 0;
  img->data = base + static_cast<size_t>(dataRow) * bytesPerRow;
  img->rowBytes = negative ? -bytesPerRow : bytesPerRow;
  if (!img->isByte) {
    // Read-back starts on the same memory row it always did, moved inwards by
    // the margin: the declared region occupies rows [inset, inset + h), so the
    // row that holds image row 0 is `inset + readRow` from the base pointer.
    // At inset 0 this is the expression it replaced.
    const int readRow = reversed ? (img->h - 1) : 0;
    img->floatData = reinterpret_cast<const float*>(
        base + static_cast<long long>(readRow + inset) * bytesPerRow);
    img->pitchFloats = static_cast<long long>(pitchPixels) * 4 * (reversed ? -1 : 1);
  }
}

ImageObj* imageFor(ClipObj* clip) {
  const bool isSource = clip->name == kOfxImageEffectSimpleSourceClipName;
  // A render worker gets its own images: two threads writing one destination
  // buffer would test the host, not the plug-in.
  if (g->parallel) {
    ImageObj*& img = isSource ? tlsImages.src : tlsImages.dst;
    if (!img) img = new ImageObj();
    if (img->w != g->imgW || img->h != g->imgH) {
      if (isSource) {
        buildSourceImage(img);
      } else {
        buildDstImage(img);
      }
    }
    return img;
  }
  ImageObj*& img = isSource ? g->srcImage : g->dstImage;
  if (!img) img = new ImageObj();
  if (img->w != g->imgW || img->h != g->imgH) {
    if (isSource) {
      buildSourceImage(img);
    } else {
      buildDstImage(img);
    }
  }
  return img;
}

void publishImageProps(ImageObj* img, ClipObj* clip) {
  // kOfxImagePropData addresses the pixel at (bounds.x1, bounds.y1), so an inset
  // source publishes a buffer pointer that moves with the inset. Anything outside
  // the published bounds is then what Image::getPixelAddress() refuses.
  const int inset = img->boundsInset;
  char* data = static_cast<char*>(img->data) +
               static_cast<long long>(inset) * img->rowBytes +
               static_cast<long long>(inset) * (img->isByte ? 4 : 16);
  setPointer(&img->props, kOfxImagePropData, 0, data);
  setInt(&img->props, kOfxImagePropRowBytes, 0, static_cast<int>(img->rowBytes));
  setInt(&img->props, kOfxImagePropBounds, 0, inset);
  setInt(&img->props, kOfxImagePropBounds, 1, inset);
  setInt(&img->props, kOfxImagePropBounds, 2, img->w - inset);
  setInt(&img->props, kOfxImagePropBounds, 3, img->h - inset);
  setString(&img->props, kOfxImageEffectPropPixelDepth, 0,
            img->isByte ? kOfxBitDepthByte : kOfxBitDepthFloat);
  setString(&img->props, kOfxImageEffectPropComponents, 0, kOfxImageComponentRGBA);
  setDouble(&img->props, kOfxImagePropPixelAspectRatio, 0, 1.0);
  setString(&img->props, kOfxImageEffectPropPreMultiplication, 0,
            kOfxImagePreMultiplied);
  setString(&img->props, kOfxPropName, 0, clip->name.c_str());
}

OfxStatus getPropertySetFn(OfxImageEffectHandle handle, OfxPropertySetHandle* prop) {
  if (!handle || !prop) return kOfxStatErrBadHandle;
  *prop = handleOf(handle); // every effect handle here is a property set handle
  return kOfxStatOK;
}

OfxStatus getParamSetFn(OfxImageEffectHandle handle, OfxParamSetHandle* paramSet) {
  if (!paramSet) return kOfxStatErrBadHandle;
  EffectObj* effect = effectOf(reinterpret_cast<OfxPropertySetHandle>(handle));
  if (!effect) return kOfxStatErrBadHandle;
  *paramSet = reinterpret_cast<OfxParamSetHandle>(&effect->paramSet);
  return kOfxStatOK;
}

OfxStatus clipDefineFn(OfxImageEffectHandle, const char* name, OfxPropertySetHandle* propertySet) {
  if (!name) return kOfxStatErrBadHandle;
  ClipObj* clip = new ClipObj();
  clip->name = name;
  clip->props.owner = clip;
  clip->props.ownerKind = kClip;
  setString(&clip->props, kOfxPropName, 0, name);
  setString(&clip->props, kOfxImageEffectPropPixelDepth, 0, kOfxBitDepthFloat);
  setString(&clip->props, kOfxImageEffectPropComponents, 0, kOfxImageComponentRGBA);
  // Source and output are both connected; a plug-in that checks
  // kOfxImageClipPropConnected on the source must see it as live.
  setInt(&clip->props, kOfxImageClipPropConnected, 0, 1);
  g->clips.push_back(clip);
  g->clipByName[name] = clip;
  if (propertySet) *propertySet = handleOf(&clip->props);
  return kOfxStatOK;
}

OfxStatus clipGetHandleFn(OfxImageEffectHandle, const char* name, OfxImageClipHandle* clip,
                          OfxPropertySetHandle* propertySet) {
  if (!name) return kOfxStatErrBadHandle;
  auto it = g->clipByName.find(name);
  if (it == g->clipByName.end()) return kOfxStatErrBadHandle;
  if (clip) *clip = reinterpret_cast<OfxImageClipHandle>(&it->second->props);
  if (propertySet) *propertySet = handleOf(&it->second->props);
  return kOfxStatOK;
}

OfxStatus clipGetPropertySetFn(OfxImageClipHandle clip, OfxPropertySetHandle* prop) {
  if (!clip || !prop) return kOfxStatErrBadHandle;
  *prop = handleOf(ps(clip));
  return kOfxStatOK;
}

OfxStatus clipGetImageFn(OfxImageClipHandle clipHandle, OfxTime, const OfxRectD*,
                         OfxPropertySetHandle* imageHandle) {
  ClipObj* clip = clipOf(clipHandle);
  if (!clip || !imageHandle) return kOfxStatErrBadHandle;
  ImageObj* img = imageFor(clip);
  publishImageProps(img, clip);
  // A host updates the clip instance's declared depth/components to match the
  // image it is about to hand over; a plug-in that checks the clip and then the
  // image would otherwise see two different answers. In a parallel render the
  // clip object is shared, so the write is skipped: a real host announces the
  // format before the render action, not inside clipGetImage.
  if (!g->parallel) {
    setString(&clip->props, kOfxImageEffectPropPixelDepth, 0,
              img->isByte ? kOfxBitDepthByte : kOfxBitDepthFloat);
    setString(&clip->props, kOfxImageEffectPropComponents, 0, kOfxImageComponentRGBA);
  }
  *imageHandle = handleOf(&img->props);
  return kOfxStatOK;
}

OfxStatus clipReleaseImageFn(OfxPropertySetHandle) { return kOfxStatOK; }

OfxStatus clipGetRegionOfDefinitionFn(OfxImageClipHandle clip, OfxTime, OfxRectD* bounds) {
  if (!clip || !bounds) return kOfxStatErrBadHandle;
  bounds->x1 = 0;
  bounds->y1 = 0;
  bounds->x2 = g->imgW;
  bounds->y2 = g->imgH;
  return kOfxStatOK;
}

int abortFn(OfxImageEffectHandle) { return 0; }

OfxStatus imageMemoryAllocFn(OfxImageEffectHandle, size_t nBytes, OfxImageMemoryHandle* handle) {
  if (!handle) return kOfxStatErrBadHandle;
  *handle = reinterpret_cast<OfxImageMemoryHandle>(std::malloc(nBytes ? nBytes : 1));
  return *handle ? kOfxStatOK : kOfxStatErrMemory;
}
OfxStatus imageMemoryFreeFn(OfxImageMemoryHandle handle) {
  std::free(handle);
  return kOfxStatOK;
}
OfxStatus imageMemoryLockFn(OfxImageMemoryHandle handle, void** ptr) {
  if (!ptr) return kOfxStatErrBadHandle;
  *ptr = handle;
  return kOfxStatOK;
}
OfxStatus imageMemoryUnlockFn(OfxImageMemoryHandle) { return kOfxStatOK; }

// ------------------------------------------------------------- thread / memory

// The OFX multi-thread suite is what ImageProcessor's row splitting rides on.
// Returning 1 CPU made every plug-in run its pixel loop single-threaded, so a
// race in multiThreadProcessImages could never show up here. These are real
// worker threads, and the TLS index is what the Support library asks for.
thread_local unsigned int t_threadIndex = 0;
thread_local bool t_isSpawned = false;

OfxStatus multiThreadFn(OfxThreadFunctionV1 func, unsigned int nThreads, void* data) {
  if (!func) return kOfxStatErrBadHandle;
  if (nThreads <= 1) {
    t_threadIndex = 0;
    t_isSpawned = false;
    func(0, 1, data);
    return kOfxStatOK;
  }
  std::vector<std::thread> workers;
  workers.reserve(nThreads);
  for (unsigned int i = 0; i < nThreads; ++i) {
    workers.emplace_back([func, nThreads, data, i]() {
      t_threadIndex = i;
      t_isSpawned = true;
      func(i, nThreads, data);
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  t_isSpawned = false;
  return kOfxStatOK;
}
OfxStatus numCPUsFn(unsigned int* n) {
  if (!n) return kOfxStatErrBadHandle;
  // Enough threads to split an image into slices, few enough that a CI box
  // with many cores does not spend the test in thread setup.
  const unsigned int hardware = std::thread::hardware_concurrency();
  *n = hardware >= 2 ? (hardware > 4 ? 4u : hardware) : 1u;
  return kOfxStatOK;
}
OfxStatus threadIndexFn(unsigned int* i) {
  if (!i) return kOfxStatErrBadHandle;
  *i = t_threadIndex;
  return kOfxStatOK;
}
int isSpawnedFn(void) { return t_isSpawned ? 1 : 0; }
OfxStatus mutexCreateFn(OfxMutexHandle* mutex, int /*lockCount*/) {
  if (!mutex) return kOfxStatErrBadHandle;
  *mutex = reinterpret_cast<OfxMutexHandle>(new std::mutex());
  return kOfxStatOK;
}
OfxStatus mutexDestroyFn(const OfxMutexHandle mutex) {
  delete reinterpret_cast<std::mutex*>(mutex);
  return kOfxStatOK;
}
OfxStatus mutexLockFn(const OfxMutexHandle mutex) {
  reinterpret_cast<std::mutex*>(mutex)->lock();
  return kOfxStatOK;
}
OfxStatus mutexUnlockFn(const OfxMutexHandle mutex) {
  reinterpret_cast<std::mutex*>(mutex)->unlock();
  return kOfxStatOK;
}
OfxStatus mutexTryLockFn(const OfxMutexHandle mutex) {
  return reinterpret_cast<std::mutex*>(mutex)->try_lock() ? kOfxStatOK : kOfxStatFailed;
}

OfxStatus memoryAllocFn(void*, size_t nBytes, void** data) {
  if (!data) return kOfxStatErrBadHandle;
  *data = std::malloc(nBytes ? nBytes : 1);
  return *data ? kOfxStatOK : kOfxStatErrMemory;
}
OfxStatus memoryFreeFn(void* data) {
  std::free(data);
  return kOfxStatOK;
}

OfxStatus messageFn(void*, const char* messageType, const char* messageId, const char* format,
                    ...) {
  char buffer[4096];
  va_list ap;
  va_start(ap, format);
  std::vsnprintf(buffer, sizeof(buffer), format ? format : "", ap);
  va_end(ap);
  std::string line = "[";
  line += messageType ? messageType : "message";
  line += "] ";
  line += buffer;
  if (messageId && *messageId) {
    line += " (";
    line += messageId;
    line += ")";
  }
  {
    std::lock_guard<std::mutex> lock(gMessageMutex);
    g->messages.push_back(line);
  }
  return kOfxStatOK;
}

// Message suite v2: the channel a plug-in uses for a user-visible error, and the
// reason Support's setPersistentMessage throws when the host offers only v1.
OfxStatus persistentMessageFn(void*, const char* messageType, const char* messageId,
                              const char* format, ...) {
  char buffer[4096];
  va_list ap;
  va_start(ap, format);
  std::vsnprintf(buffer, sizeof(buffer), format ? format : "", ap);
  va_end(ap);
  std::string line = "[persistent:";
  line += messageType ? messageType : "message";
  line += "] ";
  line += buffer;
  if (messageId && *messageId) {
    line += " (";
    line += messageId;
    line += ")";
  }
  {
    std::lock_guard<std::mutex> lock(gMessageMutex);
    g->messages.push_back(line);
  }
  return kOfxStatOK;
}

OfxStatus clearPersistentMessageFn(void*) {
  std::lock_guard<std::mutex> lock(gMessageMutex);
  g->messages.push_back("[persistent] cleared");
  return kOfxStatOK;
}

// -------------------------------------------------------------------- the host

const void* fetchSuite(OfxPropertySetHandle, const char* suiteName, int suiteVersion) {
  if (!suiteName) return nullptr;
  const std::string name(suiteName);
  if (name == kOfxPropertySuite && suiteVersion == 1) return &g->propSuite;
  if (name == kOfxImageEffectSuite && suiteVersion == 1) return &g->effectSuite;
  if (name == kOfxParameterSuite && suiteVersion == 1) return &g->paramSuite;
  if (name == kOfxMultiThreadSuite && suiteVersion == 1) return &g->threadSuite;
  if (name == kOfxMemorySuite && suiteVersion == 1) return &g->memorySuite;
  if (name == kOfxMessageSuite && suiteVersion == 1) return &g->messageSuite;
  if (name == kOfxMessageSuite && suiteVersion == 2) return &g->messageSuiteV2;
  return nullptr; // every optional suite: the plug-in must cope with nullptr
}

void setHostInteger(PropSet* s, const char* name, int value) { setInt(s, name, 0, value); }

void initHostProps(PropSet* s) {
  setString(s, kOfxPropName, 0, "testkit-fake-host");
  setString(s, kOfxPropLabel, 0, "Testkit Fake Host");
  setInt(s, kOfxPropAPIVersion, 0, 1);
  setInt(s, kOfxPropAPIVersion, 1, 5);
  setInt(s, kOfxPropVersion, 0, 1);
  setInt(s, kOfxPropVersion, 1, 0);
  setInt(s, kOfxPropVersion, 2, 0);
  setString(s, kOfxPropVersionLabel, 0, "1.0.0");
  setHostInteger(s, kOfxImageEffectHostPropIsBackground, 0);
  setHostInteger(s, kOfxImageEffectPropSupportsOverlays, 0);
  setHostInteger(s, kOfxImageEffectPropSupportsMultiResolution, 1);
  setHostInteger(s, kOfxImageEffectPropSupportsTiles, 1);
  setHostInteger(s, kOfxImageEffectPropTemporalClipAccess, 1);
  setHostInteger(s, kOfxImageEffectPropSupportsMultipleClipDepths, 1);
  setHostInteger(s, kOfxImageEffectPropSupportsMultipleClipPARs, 1);
  setHostInteger(s, kOfxImageEffectPropSetableFrameRate, 0);
  setHostInteger(s, kOfxImageEffectPropSetableFielding, 0);
  const char* contexts[] = {kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral};
  for (int i = 0; i < 2; ++i) setString(s, kOfxImageEffectPropSupportedContexts, i, contexts[i]);
  const char* depths[] = {kOfxBitDepthFloat, kOfxBitDepthHalf, kOfxBitDepthByte};
  for (int i = 0; i < 3; ++i) setString(s, kOfxImageEffectPropSupportedPixelDepths, i, depths[i]);
  setString(s, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
}

} // namespace

// ----------------------------------------------------------------- public API

Impl& state() {
  // One host per process, never destroyed: OFX::Private::OfxSetHost() ignores
  // every setHost() after the first, so a plug-in that a host re-scans keeps
  // the pointer it was given the first time. A per-test host object that went
  // out of scope would leave that pointer dangling (that is exactly what the
  // second load/unload cycle exposed).
  static Impl instance;
  return instance;
}

Impl::Impl() {
  g = this;
  propSuite.propSetPointer = propSetPointerFn;
  propSuite.propSetString = propSetStringFn;
  propSuite.propSetDouble = propSetDoubleFn;
  propSuite.propSetInt = propSetIntFn;
  propSuite.propSetPointerN = propSetPointerNFn;
  propSuite.propSetStringN = propSetStringNFn;
  propSuite.propSetDoubleN = propSetDoubleNFn;
  propSuite.propSetIntN = propSetIntNFn;
  propSuite.propGetPointer = propGetPointerFn;
  propSuite.propGetString = propGetStringFn;
  propSuite.propGetDouble = propGetDoubleFn;
  propSuite.propGetInt = propGetIntFn;
  propSuite.propGetPointerN = propGetPointerNFn;
  propSuite.propGetStringN = propGetStringNFn;
  propSuite.propGetDoubleN = propGetDoubleNFn;
  propSuite.propGetIntN = propGetIntNFn;
  propSuite.propReset = propResetFn;
  propSuite.propGetDimension = propGetDimensionFn;

  effectSuite.getPropertySet = getPropertySetFn;
  effectSuite.getParamSet = getParamSetFn;
  effectSuite.clipDefine = clipDefineFn;
  effectSuite.clipGetHandle = clipGetHandleFn;
  effectSuite.clipGetPropertySet = clipGetPropertySetFn;
  effectSuite.clipGetImage = clipGetImageFn;
  effectSuite.clipReleaseImage = clipReleaseImageFn;
  effectSuite.clipGetRegionOfDefinition = clipGetRegionOfDefinitionFn;
  effectSuite.abort = abortFn;
  effectSuite.imageMemoryAlloc = imageMemoryAllocFn;
  effectSuite.imageMemoryFree = imageMemoryFreeFn;
  effectSuite.imageMemoryLock = imageMemoryLockFn;
  effectSuite.imageMemoryUnlock = imageMemoryUnlockFn;

  paramSuite.paramDefine = paramDefineFn;
  paramSuite.paramGetHandle = paramGetHandleFn;
  paramSuite.paramSetGetPropertySet = paramSetGetPropertySetFn;
  paramSuite.paramGetPropertySet = paramGetPropertySetFn;
  paramSuite.paramGetValue = paramGetValueFn;
  paramSuite.paramGetValueAtTime = paramGetValueAtTimeFn;
  paramSuite.paramGetDerivative = paramGetDerivativeFn;
  paramSuite.paramGetIntegral = paramGetIntegralFn;
  paramSuite.paramSetValue = paramSetValueFn;
  paramSuite.paramSetValueAtTime = paramSetValueAtTimeFn;
  paramSuite.paramGetNumKeys = paramGetNumKeysFn;
  paramSuite.paramGetKeyTime = paramGetKeyTimeFn;
  paramSuite.paramGetKeyIndex = paramGetKeyIndexFn;
  paramSuite.paramDeleteKey = paramDeleteKeyFn;
  paramSuite.paramDeleteAllKeys = paramDeleteAllKeysFn;
  paramSuite.paramCopy = paramCopyFn;
  paramSuite.paramEditBegin = paramEditBeginFn;
  paramSuite.paramEditEnd = paramEditEndFn;

  threadSuite.multiThread = multiThreadFn;
  threadSuite.multiThreadNumCPUs = numCPUsFn;
  threadSuite.multiThreadIndex = threadIndexFn;
  threadSuite.multiThreadIsSpawnedThread = isSpawnedFn;
  threadSuite.mutexCreate = mutexCreateFn;
  threadSuite.mutexDestroy = mutexDestroyFn;
  threadSuite.mutexLock = mutexLockFn;
  threadSuite.mutexUnLock = mutexUnlockFn;
  threadSuite.mutexTryLock = mutexTryLockFn;

  memorySuite.memoryAlloc = memoryAllocFn;
  memorySuite.memoryFree = memoryFreeFn;

  messageSuite.message = messageFn;
  messageSuiteV2.message = messageFn;
  messageSuiteV2.setPersistentMessage = persistentMessageFn;
  messageSuiteV2.clearPersistentMessage = clearPersistentMessageFn;

  initHostProps(&hostProps);
  pluginDesc.props.owner = &pluginDesc;
  pluginDesc.props.ownerKind = kEffect;
  pluginDesc.paramSet.owner = &pluginDesc;
  pluginDesc.paramSet.ownerKind = kEffect;

  host.host = handleOf(&hostProps);
  host.fetchSuite = fetchSuite;

  srcRGBA.assign(static_cast<size_t>(imgW) * imgH * 4, 0.0f);
  for (int i = 0; i < imgW * imgH; ++i) {
    srcRGBA[static_cast<size_t>(i) * 4 + 0] = 0.25f;
    srcRGBA[static_cast<size_t>(i) * 4 + 1] = 0.50f;
    srcRGBA[static_cast<size_t>(i) * 4 + 2] = 0.75f;
    srcRGBA[static_cast<size_t>(i) * 4 + 3] = 1.00f;
  }
}

void Impl::reset() {
  for (EffectObj* e : instances) delete e;
  instances.clear();
  current = nullptr;
  for (ClipObj* c : clips) delete c;
  for (ParamObj* p : params) delete p;
  clips.clear();
  params.clear();
  clipByName.clear();
  delete srcImage;
  delete dstImage;
  srcImage = nullptr;
  dstImage = nullptr;
  pluginDesc.props.props.clear();
  pluginDesc.params.clear();
  pluginDesc.paramSet.props.clear();
  messages.clear();
  parallel = false;
  padded = true;
  negativePitch = false;
  origin = ImageOrigin::Bottom;
  byteDst = false;
  srcInset = 0;
  dstInset = 0;
  imgW = 64;
  imgH = 48;
  srcRGBA.assign(static_cast<size_t>(imgW) * imgH * 4, 0.0f);
  for (int i = 0; i < imgW * imgH; ++i) {
    srcRGBA[static_cast<size_t>(i) * 4 + 0] = 0.25f;
    srcRGBA[static_cast<size_t>(i) * 4 + 1] = 0.50f;
    srcRGBA[static_cast<size_t>(i) * 4 + 2] = 0.75f;
    srcRGBA[static_cast<size_t>(i) * 4 + 3] = 1.00f;
  }
}


FakeHost::FakeHost() : impl_(&state()) {
  impl_->reset();
  g = impl_;
}

FakeHost::~FakeHost() {
  // The host state outlives every FakeHost (see state()).
}

void FakeHost::attach(OfxPlugin* plugin) {
  plugin_ = plugin;
  if (plugin && plugin->setHost) plugin->setHost(&impl_->host);
}

OfxStatus FakeHost::mainEntry(const char* action, void* outArgs, void* inArgs) {
  if (!plugin_ || !plugin_->mainEntry) return kOfxStatErrBadHandle;
  // A render worker names its own effect; otherwise the selected instance does.
  EffectObj* effect = tlsEffect ? tlsEffect : impl_->current;
  const void* handle = effect ? static_cast<const void*>(&effect->props)
                              : static_cast<const void*>(&impl_->pluginDesc.props);
  return plugin_->mainEntry(action, handle, reinterpret_cast<OfxPropertySetHandle>(inArgs),
                            reinterpret_cast<OfxPropertySetHandle>(outArgs));
}

OfxStatus FakeHost::pluginMain(const char* action) { return mainEntry(action, nullptr, nullptr); }

OfxStatus FakeHost::pluginMainWithContext(const char* action, const char* context) {
  PropSet inArgs;
  setString(&inArgs, kOfxImageEffectPropContext, 0, context);
  return mainEntry(action, nullptr, &inArgs);
}

OfxStatus FakeHost::load() { return pluginMain(kOfxActionLoad); }
OfxStatus FakeHost::describe() { return pluginMain(kOfxActionDescribe); }

OfxStatus FakeHost::describeInContext(const char* context) {
  return pluginMainWithContext(kOfxImageEffectActionDescribeInContext, context);
}

OfxStatus FakeHost::createInstance(const char* context) {
  return createInstanceIndexed(context) >= 0 ? kOfxStatOK : kOfxStatFailed;
}

int FakeHost::createInstanceIndexed(const char* context) {
  if (!plugin_) return -1;
  EffectObj* effect = new EffectObj();
  effect->props.owner = effect;
  effect->props.ownerKind = kEffect;
  effect->paramSet.owner = effect;
  effect->paramSet.ownerKind = kEffect;
  const std::string name = "testkit-instance-" + std::to_string(impl_->instances.size());
  setString(&effect->props, kOfxImageEffectPropContext, 0, context);
  setString(&effect->props, kOfxPropName, 0, name.c_str());
  impl_->instances.push_back(effect);
  impl_->current = effect;
  const int index = static_cast<int>(impl_->instances.size()) - 1;
  if (pluginMain(kOfxActionCreateInstance) != kOfxStatOK) {
    impl_->instances.pop_back();
    impl_->current = impl_->instances.empty() ? nullptr : impl_->instances.back();
    delete effect;
    return -1;
  }
  return index;
}

void FakeHost::selectInstance(int index) {
  impl_->current = (index >= 0 && index < static_cast<int>(impl_->instances.size()))
                       ? impl_->instances[static_cast<size_t>(index)]
                       : nullptr;
}

int FakeHost::instanceCount() const { return static_cast<int>(impl_->instances.size()); }

OfxStatus FakeHost::destroyInstanceAt(int index) {
  if (index < 0 || index >= static_cast<int>(impl_->instances.size())) return kOfxStatErrBadHandle;
  EffectObj* effect = impl_->instances[static_cast<size_t>(index)];
  EffectObj* saved = impl_->current;
  impl_->current = effect;
  const OfxStatus st = pluginMain(kOfxActionDestroyInstance);
  impl_->instances.erase(impl_->instances.begin() + index);
  if (saved == effect) {
    impl_->current = nullptr;
  } else {
    impl_->current = saved;
  }
  delete effect;
  return st;
}

OfxStatus FakeHost::isIdentity(double time, const OfxRectI& window, std::string* identityClip) {
  PropSet inArgs;
  PropSet outArgs;
  setInt(&inArgs, kOfxImageEffectPropOpenCLEnabled, 0, 0);
  setInt(&inArgs, kOfxImageEffectPropCudaEnabled, 0, 0);
  setInt(&inArgs, kOfxImageEffectPropMetalEnabled, 0, 0);
  fillTimeArgs(&inArgs, time, window);
  OfxStatus st = mainEntry(kOfxImageEffectActionIsIdentity, &outArgs, &inArgs);
  if (st == kOfxStatOK && identityClip) {
    char* name = nullptr;
    getString(&outArgs, kOfxPropName, 0, &name);
    *identityClip = name ? name : "";
  }
  return st;
}

OfxStatus FakeHost::render(double time, const OfxRectI& window) {
  PropSet inArgs;
  PropSet outArgs;
  setInt(&inArgs, kOfxImageEffectPropOpenCLEnabled, 0, 0);
  setInt(&inArgs, kOfxImageEffectPropCudaEnabled, 0, 0);
  setInt(&inArgs, kOfxImageEffectPropMetalEnabled, 0, 0);
  fillTimeArgs(&inArgs, time, window);
  return mainEntry(kOfxImageEffectActionRender, &outArgs, &inArgs);
}

OfxStatus FakeHost::getRegionsOfInterest(double time, const OfxRectD& requested,
                                         OfxRectD* sourceRoi) {
  PropSet inArgs;
  PropSet outArgs;
  setDouble(&inArgs, kOfxPropTime, 0, time);
  setDouble(&inArgs, kOfxImageEffectPropRenderScale, 0, 1.0);
  setDouble(&inArgs, kOfxImageEffectPropRenderScale, 1, 1.0);
  setDouble(&inArgs, kOfxImageEffectPropRegionOfInterest, 0, requested.x1);
  setDouble(&inArgs, kOfxImageEffectPropRegionOfInterest, 1, requested.y1);
  setDouble(&inArgs, kOfxImageEffectPropRegionOfInterest, 2, requested.x2);
  setDouble(&inArgs, kOfxImageEffectPropRegionOfInterest, 3, requested.y2);
  const OfxStatus st =
      mainEntry(kOfxImageEffectActionGetRegionsOfInterest, &outArgs, &inArgs);
  if (st == kOfxStatOK && sourceRoi) {
    // Not a kOfx* property: Support names the outArgs entry after the clip, see
    // `_clipROIPropNames` in ofxsImageEffect.cpp. Reading it here is what makes
    // a wrong RoI visible instead of silent.
    const std::string prop =
        std::string("OfxImageClipPropRoI_") + kOfxImageEffectSimpleSourceClipName;
    double v[4] = {0.0, 0.0, 0.0, 0.0};
    for (int i = 0; i < 4; ++i) getDouble(&outArgs, prop.c_str(), i, &v[i]);
    sourceRoi->x1 = v[0];
    sourceRoi->y1 = v[1];
    sourceRoi->x2 = v[2];
    sourceRoi->y2 = v[3];
  }
  return st;
}

OfxStatus FakeHost::destroyInstance() {
  for (int i = 0; i < instanceCount(); ++i) {
    if (impl_->instances[static_cast<size_t>(i)] == impl_->current) return destroyInstanceAt(i);
  }
  return pluginMain(kOfxActionDestroyInstance);
}

OfxStatus FakeHost::unload() { return pluginMain(kOfxActionUnload); }

void FakeHost::fillTimeArgs(void* inArgsPtr, double time, const OfxRectI& window) {
  PropSet* inArgs = static_cast<PropSet*>(inArgsPtr);
  setDouble(inArgs, kOfxPropTime, 0, time);
  setDouble(inArgs, kOfxImageEffectPropRenderScale, 0, 1.0);
  setDouble(inArgs, kOfxImageEffectPropRenderScale, 1, 1.0);
  setInt(inArgs, kOfxImageEffectPropRenderWindow, 0, window.x1);
  setInt(inArgs, kOfxImageEffectPropRenderWindow, 1, window.y1);
  setInt(inArgs, kOfxImageEffectPropRenderWindow, 2, window.x2);
  setInt(inArgs, kOfxImageEffectPropRenderWindow, 3, window.y2);
  setString(inArgs, kOfxImageEffectPropFieldToRender, 0, kOfxImageFieldBoth);
}

void FakeHost::invalidateImages() {
  delete impl_->srcImage;
  delete impl_->dstImage;
  impl_->srcImage = nullptr;
  impl_->dstImage = nullptr;
}

std::map<std::string, std::string> FakeHost::declaredParams() const {
  std::map<std::string, std::string> out;
  for (ParamObj* p : impl_->params) out[p->name] = p->type;
  return out;
}

std::vector<std::string> FakeHost::declaredClips() const {
  std::vector<std::string> out;
  for (ClipObj* c : impl_->clips) out.push_back(c->name);
  return out;
}

std::string FakeHost::descriptorString(const char* property, int index) const {
  char* value = nullptr;
  getString(const_cast<PropSet*>(&impl_->pluginDesc.props), property, index, &value);
  return value ? value : "";
}

int FakeHost::descriptorInt(const char* property, int index) const {
  int value = 0;
  getInt(const_cast<PropSet*>(&impl_->pluginDesc.props), property, index, &value);
  return value;
}

void FakeHost::setImageSize(int width, int height) {
  impl_->imgW = width;
  impl_->imgH = height;
  impl_->srcRGBA.assign(static_cast<size_t>(width) * height * 4, 1.0f);
  invalidateImages();
}

void FakeHost::setSourcePlanar(const float* rgb, int width, int height) {
  impl_->imgW = width;
  impl_->imgH = height;
  impl_->srcRGBA.assign(static_cast<size_t>(width) * height * 4, 0.0f);
  for (int i = 0; i < width * height; ++i) {
    impl_->srcRGBA[static_cast<size_t>(i) * 4 + 0] = rgb[static_cast<size_t>(i)];
    impl_->srcRGBA[static_cast<size_t>(i) * 4 + 1] = rgb[static_cast<size_t>(i) + size_t(width) * height];
    impl_->srcRGBA[static_cast<size_t>(i) * 4 + 2] =
        rgb[static_cast<size_t>(i) + size_t(width) * height * 2];
    impl_->srcRGBA[static_cast<size_t>(i) * 4 + 3] = 1.0f;
  }
  invalidateImages();
}

void FakeHost::setSourceConstant(float r, float g, float b, float a) {
  const size_t n = static_cast<size_t>(impl_->imgW) * impl_->imgH;
  impl_->srcRGBA.assign(n * 4, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    impl_->srcRGBA[i * 4 + 0] = r;
    impl_->srcRGBA[i * 4 + 1] = g;
    impl_->srcRGBA[i * 4 + 2] = b;
    impl_->srcRGBA[i * 4 + 3] = a;
  }
  invalidateImages();
}

void FakeHost::setSourceBoundsInset(int pixels) {
  impl_->srcInset = pixels > 0 ? pixels : 0;
  // Only the source changes shape. The destination image and the pixels a
  // previous render left in it stay: a test compares them before and after a
  // rejected render, which is how "half written, then an error" shows up.
  delete impl_->srcImage;
  impl_->srcImage = nullptr;
}

void FakeHost::setDestinationBoundsInset(int pixels) {
  impl_->dstInset = pixels > 0 ? pixels : 0;
  // Only the destination changes shape, mirroring the source case: the source
  // image stays, and the buffer is rebuilt with a margin so a plug-in that
  // writes outside the declared bounds stays inside the allocation.
  delete impl_->dstImage;
  impl_->dstImage = nullptr;
}

std::vector<float> FakeHost::destinationBuffer() const {
  if (!impl_->dstImage || impl_->dstImage->isByte) return {};
  // The whole allocation in memory order, margins included. destination() walks
  // the declared bounds; this one shows a write that landed outside them.
  return impl_->dstImage->floats;
}

void FakeHost::setPaddedRows(bool on) {
  impl_->padded = on;
  invalidateImages();
}

void FakeHost::setNegativePitch(bool on) {
  impl_->negativePitch = on;
  invalidateImages();
}

void FakeHost::setImageOrigin(ImageOrigin o) {
  impl_->origin = o;
  invalidateImages();
}

void FakeHost::setDestinationByteImage(bool on) {
  impl_->byteDst = on;
  // The host announces the output format on the clip before the render action,
  // not when the image is fetched; a plug-in that validates the clip first
  // would otherwise see a stale depth.
  auto it = impl_->clipByName.find(kOfxImageEffectOutputClipName);
  if (it != impl_->clipByName.end()) {
    setString(&it->second->props, kOfxImageEffectPropPixelDepth, 0,
              on ? kOfxBitDepthByte : kOfxBitDepthFloat);
  }
  invalidateImages();
}

int FakeHost::imageWidth() const { return impl_->imgW; }
int FakeHost::imageHeight() const { return impl_->imgH; }

long long FakeHost::destinationRowBytes() const {
  return impl_->dstImage ? impl_->dstImage->rowBytes : 0;
}

namespace {

std::vector<float> snapshotImage(const ImageObj* img) {
  if (!img || img->isByte || img->w <= 0) return {};
  std::vector<float> out(static_cast<size_t>(img->w) * img->h * 4, 0.0f);
  for (int y = 0; y < img->h; ++y) {
    const float* px = img->floatData + static_cast<long long>(y) * img->pitchFloats;
    for (int i = 0; i < img->w * 4; ++i) out[static_cast<size_t>(y) * img->w * 4 + i] = px[i];
  }
  return out;
}

} // namespace

std::vector<float> FakeHost::destination() const { return snapshotImage(impl_->dstImage); }

std::vector<FakeHost::RenderRecord> FakeHost::renderConcurrently(
    const std::vector<int>& instances, int iterations, const OfxRectI& window) {
  std::vector<RenderRecord> out;
  if (instances.empty() || iterations <= 0) return out;

  impl_->parallel = true;
  std::vector<std::vector<RenderRecord>> perWorker(instances.size());
  std::vector<std::thread> workers;
  workers.reserve(instances.size());
  for (size_t t = 0; t < instances.size(); ++t) {
    const int index = instances[t];
    workers.emplace_back([this, t, index, iterations, window, &perWorker]() {
      tlsEffect = (index >= 0 && index < static_cast<int>(impl_->instances.size()))
                      ? impl_->instances[static_cast<size_t>(index)]
                      : nullptr;
      tlsImages = ThreadImages{};
      for (int k = 0; k < iterations; ++k) {
        RenderRecord record;
        record.instance = index;
        // Distinct times per worker: a host renders different frames at once,
        // and a plug-in that caches per-time state shows up here.
        record.time = 1.0 + static_cast<double>(k) + 0.125 * static_cast<double>(t);
        record.status = render(record.time, window);
        record.pixels = snapshotImage(tlsImages.dst);
        perWorker[t].push_back(std::move(record));
      }
      delete tlsImages.src;
      delete tlsImages.dst;
      tlsImages = ThreadImages{};
      tlsEffect = nullptr;
    });
  }
  for (std::thread& worker : workers) worker.join();
  impl_->parallel = false;

  for (std::vector<RenderRecord>& records : perWorker) {
    for (RenderRecord& record : records) out.push_back(std::move(record));
  }
  return out;
}

bool FakeHost::setParam(const std::string& name, const std::string& value) {
  ParamObj* p = ensureParam(impl_->current, name);
  if (!p) return false;
  // Go through the parameter suite instead of writing ParamObj's fields. This
  // is the call Support's OFX::DoubleParam::setValueAtTime makes, so a suite
  // that reads its arguments wrongly fails a normal --set test instead of
  // waiting for a plug-in that writes a parameter back.
  OfxParamHandle handle = reinterpret_cast<OfxParamHandle>(&p->props);
  if (p->type == kOfxParamTypeDouble) {
    return paramSetValueAtTimeFn(handle, 0.0, std::atof(value.c_str())) == kOfxStatOK;
  }
  if (p->type == kOfxParamTypeInteger || p->type == kOfxParamTypeBoolean ||
      p->type == kOfxParamTypeChoice) {
    return paramSetValueAtTimeFn(handle, 0.0, std::atoi(value.c_str())) == kOfxStatOK;
  }
  if (p->type == kOfxParamTypeString) {
    return paramSetValueAtTimeFn(handle, 0.0, value.c_str()) == kOfxStatOK;
  }
  if (p->type == kOfxParamTypeRGB || p->type == kOfxParamTypeRGBA) {
    const bool rgba = p->type == kOfxParamTypeRGBA;
    double c[4] = {0.0, 0.0, 0.0, 0.0};
    if (std::sscanf(value.c_str(), "%lf,%lf,%lf,%lf", &c[0], &c[1], &c[2], &c[3]) <
        (rgba ? 4 : 3)) {
      return false;
    }
    return rgba ? paramSetValueAtTimeFn(handle, 0.0, c[0], c[1], c[2], c[3]) == kOfxStatOK
                : paramSetValueAtTimeFn(handle, 0.0, c[0], c[1], c[2]) == kOfxStatOK;
  }
  return false;
}

bool FakeHost::pressParam(const std::string& name) {
  if (impl_->instances.empty() || !impl_->current) return false;
  ParamObj* p = ensureParam(impl_->current, name);
  if (!p) return false;
  // The action a host sends when a user clicks a button. The property set is
  // what Support's InstanceChangedArgs reads: type and name of the changed
  // object, the reason, the time and the render scale. A push button also has
  // to be declared with kOfxPropIsInteractive; that is the plug-in's job.
  PropSet inArgs;
  setString(&inArgs, kOfxPropType, 0, kOfxTypeParameter);
  setString(&inArgs, kOfxPropName, 0, name.c_str());
  setString(&inArgs, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
  setDouble(&inArgs, kOfxPropTime, 0, 0.0);
  setDouble(&inArgs, kOfxImageEffectPropRenderScale, 0, 1.0);
  setDouble(&inArgs, kOfxImageEffectPropRenderScale, 1, 1.0);
  const OfxStatus st = mainEntry(kOfxActionInstanceChanged, nullptr, &inArgs);
  // A notification is a success unless the plug-in rejects it; Support answers
  // kOfxStatReplyDefault when the action was handled.
  return st != kOfxStatFailed && st != kOfxStatErrBadHandle;
}

std::string FakeHost::paramValue(const std::string& name) const {
  const ParamObj* p = nullptr;
  if (impl_->current) {
    auto it = impl_->current->params.find(name);
    if (it != impl_->current->params.end()) p = it->second;
  }
  if (!p) {
    auto tmpl = impl_->pluginDesc.params.find(name);
    if (tmpl != impl_->pluginDesc.params.end()) p = tmpl->second;
  }
  if (!p) return std::string();
  char text[256];
  ParamObj* mutableParam = const_cast<ParamObj*>(p);
  if (p->type == kOfxParamTypeDouble) {
    std::snprintf(text, sizeof(text), "%g", paramDouble(mutableParam));
    return text;
  }
  if (p->type == kOfxParamTypeInteger || p->type == kOfxParamTypeBoolean ||
      p->type == kOfxParamTypeChoice) {
    std::snprintf(text, sizeof(text), "%d", paramInt(mutableParam));
    return text;
  }
  if (p->type == kOfxParamTypeString) {
    return paramString(mutableParam);
  }
  if (p->type == kOfxParamTypeRGB || p->type == kOfxParamTypeRGBA) {
    const int count = (p->type == kOfxParamTypeRGBA) ? 4 : 3;
    double c[4] = {0.0, 0.0, 0.0, 0.0};
    paramColor(mutableParam, c, count);
    if (count == 4) {
      std::snprintf(text, sizeof(text), "%g,%g,%g,%g", c[0], c[1], c[2], c[3]);
    } else {
      std::snprintf(text, sizeof(text), "%g,%g,%g", c[0], c[1], c[2]);
    }
    return text;
  }
  return std::string();
}

std::vector<std::string> FakeHost::paramOptions(const std::string& name) const {
  std::vector<std::string> out;
  const ParamObj* p = nullptr;
  auto tmpl = impl_->pluginDesc.params.find(name);
  if (tmpl != impl_->pluginDesc.params.end()) {
    p = tmpl->second;
  } else if (impl_->current) {
    auto it = impl_->current->params.find(name);
    if (it != impl_->current->params.end()) p = it->second;
  }
  if (!p) return out;
  for (int i = 0;; ++i) {
    const Value* v = slotConst(const_cast<PropSet*>(&p->props), kOfxParamPropChoiceOption, i);
    if (!v || v->kind != kString) break;
    out.push_back(v->s);
  }
  return out;
}

bool FakeHost::setParamForInstance(int index, const std::string& name, const std::string& value) {
  if (index < 0 || index >= static_cast<int>(impl_->instances.size())) return false;
  EffectObj* saved = impl_->current;
  impl_->current = impl_->instances[static_cast<size_t>(index)];
  const bool ok = setParam(name, value);
  impl_->current = saved;
  return ok;
}

std::vector<std::string> FakeHost::messages() const {
  std::lock_guard<std::mutex> lock(gMessageMutex);
  return impl_->messages;
}

std::string FakeHost::messageText() const {
  std::string out;
  for (const std::string& m : impl_->messages) {
    out += m;
    out += '\n';
  }
  return out;
}

} // namespace testkit
