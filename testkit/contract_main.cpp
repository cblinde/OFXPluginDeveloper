// SPDX-License-Identifier: BSD-3-Clause
//
// Contract test for a built .ofx bundle: loads the binary the way a host does,
// resolves the two mandated entry points, checks the OfxPlugin struct, and then
// drives the plug-in through a hand-written host (testkit/fake_host.*) covering
// describe, describeInContext, createInstance, isIdentity, render,
// destroyInstance and unload - twice, so the load/unload cycle is exercised more
// than once in one process.
//
// Every check prints one line and the process exits non-zero on the first
// failure, naming the assertion. Run it against a deliberately broken plug-in
// (testkit/negative-controls) to see that behaviour.
//
// Linux/macOS use dlopen/dlsym. Windows uses LoadLibraryExW + GetProcAddress on
// the same two names. Both paths are built: an OFX host on Windows is doing the
// LoadLibraryExW half of this, so leaving it untested meant the platform the
// template claims to support had no loader-level verification at all.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "fake_host.h"

namespace {

int g_checks = 0;
int g_failed = 0;

void ok(const std::string& what) {
  ++g_checks;
  std::printf("ok %d - %s\n", g_checks, what.c_str());
}

bool check(const std::string& what, bool condition, const std::string& detail = std::string()) {
  ++g_checks;
  if (condition) {
    std::printf("ok %d - %s\n", g_checks, what.c_str());
    return true;
  }
  ++g_failed;
  std::printf("FAIL %s%s%s\n", what.c_str(), detail.empty() ? "" : ": ", detail.c_str());
  std::fflush(stdout);
  return false;
}

std::string statusName(OfxStatus s) {
  switch (s) {
    case kOfxStatOK: return "kOfxStatOK";
    case kOfxStatFailed: return "kOfxStatFailed";
    case kOfxStatErrFatal: return "kOfxStatErrFatal";
    case kOfxStatErrUnknown: return "kOfxStatErrUnknown";
    case kOfxStatErrMissingHostFeature: return "kOfxStatErrMissingHostFeature";
    case kOfxStatErrUnsupported: return "kOfxStatErrUnsupported";
    case kOfxStatErrExists: return "kOfxStatErrExists";
    case kOfxStatErrFormat: return "kOfxStatErrFormat";
    case kOfxStatErrMemory: return "kOfxStatErrMemory";
    case kOfxStatErrBadHandle: return "kOfxStatErrBadHandle";
    case kOfxStatErrBadIndex: return "kOfxStatErrBadIndex";
    case kOfxStatErrValue: return "kOfxStatErrValue";
    case kOfxStatReplyYes: return "kOfxStatReplyYes";
    case kOfxStatReplyNo: return "kOfxStatReplyNo";
    case kOfxStatReplyDefault: return "kOfxStatReplyDefault";
    default: break;
  }
  return "status " + std::to_string(s);
}

struct Module {
  void* handle = nullptr;
  int (*numberOfPlugins)() = nullptr;
  OfxPlugin* (*plugin)(int) = nullptr;

  static std::string lastLoaderError() {
#ifdef _WIN32
    const DWORD code = ::GetLastError();
    char* text = nullptr;
    const DWORD n = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string message = (n != 0 && text != nullptr) ? std::string(text, n)
                                                      : std::string("LoadLibraryEx failed");
    if (text != nullptr) ::LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' ||
                                message.back() == ' ' || message.back() == '.')) {
      message.pop_back();
    }
    return message + " (Win32 error " + std::to_string(code) + ")";
#else
    const char* e = ::dlerror();
    return e ? std::string(e) : std::string("dlopen failed");
#endif
  }

  bool open(const std::string& path, std::string* error) {
#ifdef _WIN32
    // A Windows OFX binary is a DLL with a .ofx extension. LOAD_WITH_ALTERED_SEARCH_PATH
    // makes the loader search the bundle first, which is how a host finds a
    // cudart shipped next to the plug-in.
    const int wide = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<size_t>(wide > 0 ? wide - 1 : 0), L'\0');
    if (wide > 1) {
      ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wide);
    }
    HMODULE h = ::LoadLibraryExW(wpath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    handle = h;
    if (!h) {
      *error = lastLoaderError();
      return false;
    }
    // Both names are mandated by OFX; a bundle missing either is not loadable.
    numberOfPlugins =
        reinterpret_cast<int (*)()>(::GetProcAddress(h, "OfxGetNumberOfPlugins"));
    plugin = reinterpret_cast<OfxPlugin* (*)(int)>(::GetProcAddress(h, "OfxGetPlugin"));
#else
    handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
      *error = lastLoaderError();
      return false;
    }
    numberOfPlugins = reinterpret_cast<int (*)()>(::dlsym(handle, "OfxGetNumberOfPlugins"));
    plugin = reinterpret_cast<OfxPlugin* (*)(int)>(::dlsym(handle, "OfxGetPlugin"));
#endif
    return true;
  }
  void close() {
#ifdef _WIN32
    if (handle) ::FreeLibrary(static_cast<HMODULE>(handle));
#else
    if (handle) ::dlclose(handle);
#endif
    handle = nullptr;
  }
  ~Module() { close(); }
};

struct Spec {
  std::string path;
  std::string id;
  std::string api = kOfxImageEffectPluginApi;
  std::string context = kOfxImageEffectContextFilter;
  int apiVersion = 1;
  int versionMajor = 1;
  int versionMinor = 0;
  std::vector<std::pair<std::string, std::string>> params; // name -> full type string
  std::vector<std::string> clips;
  std::vector<std::pair<std::string, std::string>> sets;
  std::string srcFile;
  std::string goldenFile;
  int width = 64;
  int height = 48;
  bool expectIdentity = false;
  bool expectNotIdentityAfterSet = false;
  bool byteDstRejected = false;
  bool negativePitch = false;
  bool topOrigin = false;
  // --set-status-only: render after --set, but assert only the status and that
  // each write was accepted. The default also compares the frame to
  // src x atof(value), which only holds for a gain-like parameter.
  bool setStatusOnly = false;
  // Multi-instance and thread-safety checks. `concurrencyThreads == 0` turns
  // them off; `scaleParam` names a numeric parameter that is given a different
  // value on each instance (e.g. "gain").
  int concurrencyThreads = 4;
  int concurrencyIterations = 3;
  std::string scaleParam;
};

std::string canonicalType(const std::string& t) {
  if (t == "double") return kOfxParamTypeDouble;
  if (t == "int" || t == "integer") return kOfxParamTypeInteger;
  if (t == "string") return kOfxParamTypeString;
  if (t == "bool" || t == "boolean") return kOfxParamTypeBoolean;
  // Colour and choice parameters are declared through the same C strings; a
  // plug-in with a box colour could not get past the type check without these.
  if (t == "rgb") return kOfxParamTypeRGB;
  if (t == "rgba") return kOfxParamTypeRGBA;
  if (t == "choice") return kOfxParamTypeChoice;
  return t;
}

bool readFloats(const std::string& path, std::vector<float>* out, size_t expected) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff bytes = in.tellg();
  in.seekg(0, std::ios::beg);
  if (bytes <= 0 || (bytes % 4) != 0) return false;
  out->resize(static_cast<size_t>(bytes / 4));
  in.read(reinterpret_cast<char*>(out->data()), bytes);
  if (!in) return false;
  return expected == 0 || out->size() == expected;
}

bool validThreadSafety(const std::string& s) {
  return s == kOfxImageEffectRenderUnsafe || s == kOfxImageEffectRenderInstanceSafe ||
         s == kOfxImageEffectRenderFullySafe;
}

// Checks that `pixels` is the source scaled by `scale`, the same gain-like
// assumption the single-instance check makes. Returns the worst deviation.
double deviationFromScaledSource(const std::vector<float>& pixels, const std::vector<float>& srcRGBA,
                                 double scale, std::string* where) {
  double worst = 0.0;
  for (size_t i = 0; i + 3 < pixels.size() && i + 3 < srcRGBA.size(); i += 4) {
    for (int c = 0; c < 3; ++c) {
      const double expected = static_cast<double>(srcRGBA[i + c]) * scale;
      const double diff = std::abs(static_cast<double>(pixels[i + c]) - expected);
      if (diff > worst) {
        worst = diff;
        *where = "pixel " + std::to_string(i / 4) + " channel " + std::to_string(c);
      }
    }
  }
  return worst;
}

// Two live instances must keep separate parameter values, and the declared
// render thread safety must hold under real threads. OFX defines the levels
// precisely: kOfxImageEffectRenderInstanceSafe allows concurrent renders of
// *different* instances, kOfxImageEffectRenderFullySafe also allows several
// threads to render the *same* instance. kOfxImageEffectRenderUnsafe promises
// nothing, and is reported as a named skip rather than silently passing.
bool runMultiInstance(const Spec& spec, testkit::FakeHost& host, const std::string& safety,
                      const std::vector<float>& srcRGBA) {
  if (spec.concurrencyThreads <= 0) {
    ok("multi-instance and thread-safety checks skipped (--concurrency-threads 0)");
    return true;
  }
  const OfxRectI window = {0, 0, spec.width, spec.height};

  // A host gives every instance the same baseline configuration (model path,
  // tile size, ...); only the values a test varies per instance differ.
  auto applyBaseline = [&](int index) {
    for (const auto& set : spec.sets) {
      if (!check("baseline " + set.first + " on instance " + std::to_string(index),
                 host.setParamForInstance(index, set.first, set.second))) {
        return false;
      }
    }
    return true;
  };

  // ---- two live instances, each with its own parameter values -------------
  const int second = host.createInstanceIndexed(spec.context.c_str());
  if (!check("a second instance can be created", second >= 0)) return false;
  if (!check("two instances are live at the same time", host.instanceCount() >= 2,
             std::to_string(host.instanceCount()) + " instances")) {
    return false;
  }
  if (!applyBaseline(0) || !applyBaseline(second)) return false;

  double firstScale = 1.0;
  double secondScale = 1.0;
  if (!spec.scaleParam.empty()) {
    firstScale = 1.5;
    secondScale = 2.5;
    if (!check("instance 0 accepts its own " + spec.scaleParam,
               host.setParamForInstance(0, spec.scaleParam, "1.5"))) {
      return false;
    }
    if (!check("instance 1 accepts a different " + spec.scaleParam,
               host.setParamForInstance(second, spec.scaleParam, "2.5"))) {
      return false;
    }
  }

  host.selectInstance(0);
  if (!check("render instance 0 while instance 1 is alive",
             host.render(0.0, window) == kOfxStatOK)) {
    return false;
  }
  const std::vector<float> firstPixels = host.destination();
  host.selectInstance(second);
  if (!check("render instance 1 with its own settings",
             host.render(0.0, window) == kOfxStatOK)) {
    return false;
  }
  const std::vector<float> secondPixels = host.destination();

  if (!spec.scaleParam.empty()) {
    std::string where;
    const double worst0 = deviationFromScaledSource(firstPixels, srcRGBA, firstScale, &where);
    if (!check("instance 0 used its own value, not its neighbour's", worst0 <= 1e-6,
               "max deviation " + std::to_string(worst0) + " at " + where)) {
      return false;
    }
    const double worst1 = deviationFromScaledSource(secondPixels, srcRGBA, secondScale, &where);
    if (!check("instance 1 used its own value, not its neighbour's", worst1 <= 1e-6,
               "max deviation " + std::to_string(worst1) + " at " + where)) {
      return false;
    }
  } else if (!check("two instances with identical settings produce identical pixels",
                    !firstPixels.empty() && firstPixels == secondPixels,
                    std::to_string(firstPixels.size()) + " vs " +
                        std::to_string(secondPixels.size()) + " floats")) {
    return false;
  }

  // ---- concurrent renders, as many as the declared safety allows ---------
  const bool fullySafe = safety == kOfxImageEffectRenderFullySafe;
  const bool instanceSafe = safety == kOfxImageEffectRenderInstanceSafe || fullySafe;
  if (!instanceSafe) {
    ok("declared " + safety + ": no concurrent render is promised, test skipped by name");
  } else {
    const int threads = std::max(2, spec.concurrencyThreads);
    std::vector<int> used;
    used.push_back(0);
    if (fullySafe) {
      used.assign(static_cast<size_t>(threads), 0); // same instance, many threads
    } else {
      used.push_back(second);
      while (static_cast<int>(used.size()) < threads) {
        const int id = host.createInstanceIndexed(spec.context.c_str());
        if (!check("instance for parallel worker " + std::to_string(used.size()), id >= 0)) {
          return false;
        }
        if (!applyBaseline(id)) return false;
        used.push_back(id);
      }
    }

    std::map<int, double> valueByInstance;
    for (size_t i = 0; i < used.size(); ++i) {
      const double value = 1.0 + 0.5 * static_cast<double>(i);
      valueByInstance[used[i]] = value;
      if (!spec.scaleParam.empty() &&
          !check("set " + spec.scaleParam + " on the instance of parallel worker " +
                     std::to_string(i),
                 host.setParamForInstance(used[i], spec.scaleParam, std::to_string(value)))) {
        return false;
      }
    }

    const std::vector<testkit::FakeHost::RenderRecord> records =
        host.renderConcurrently(used, spec.concurrencyIterations, window);
    const size_t expected = used.size() * static_cast<size_t>(spec.concurrencyIterations);
    if (!check("concurrent renders produced every record", records.size() == expected,
               std::to_string(records.size()) + " of " + std::to_string(expected))) {
      return false;
    }

    bool allOk = true;
    std::string firstBad;
    for (const auto& record : records) {
      if (record.status != kOfxStatOK) {
        allOk = false;
        if (firstBad.empty()) firstBad = statusName(record.status);
      }
    }
    if (!check("every concurrent render returned kOfxStatOK", allOk, firstBad)) return false;

    bool readable = true;
    for (const auto& record : records) readable = readable && record.pixels.size() == srcRGBA.size();
    if (!check("every concurrent render produced a readable destination", readable)) return false;

    if (!spec.scaleParam.empty()) {
      double worst = 0.0;
      std::string where;
      for (const auto& record : records) {
        const double diff =
            deviationFromScaledSource(record.pixels, srcRGBA, valueByInstance[record.instance], &where);
        if (diff > worst) worst = diff;
      }
      if (!check("concurrent instances kept their own parameter values", worst <= 1e-6,
                 "max deviation " + std::to_string(worst) + " at " + where)) {
        return false;
      }
    } else {
      bool identical = true;
      for (const auto& record : records) identical = identical && record.pixels == records.front().pixels;
      if (!check("concurrent renders with identical settings are identical", identical)) return false;
    }
  }

  // Leave exactly the first instance alive, selected, for the rest of the cycle.
  while (host.instanceCount() > 1) {
    const int last = host.instanceCount() - 1;
    if (!check("destroy instance " + std::to_string(last),
               host.destroyInstanceAt(last) == kOfxStatOK)) {
      return false;
    }
  }
  host.selectInstance(0);
  return true;
}

// Runs one whole open -> unload cycle. Returns false on the first failed check.
bool failCycle(const testkit::FakeHost& host) {
  const std::string messages = host.messageText();
  if (!messages.empty()) {
    std::printf("--- plug-in messages ---\n%s------------------------\n", messages.c_str());
  }
  return false;
}

bool runCycle(const Spec& spec, const std::string& firstIdentifier) {
  testkit::FakeHost host;
  Module module;
  std::string error;
  if (!check("dlopen(" + spec.path + ")", module.open(spec.path, &error), error)) return failCycle(host);

  if (!check("OfxGetNumberOfPlugins is exported", module.numberOfPlugins != nullptr)) return failCycle(host);
  if (!check("OfxGetPlugin is exported", module.plugin != nullptr)) return failCycle(host);

  const int count = module.numberOfPlugins();
  if (!check("OfxGetNumberOfPlugins() >= 1", count >= 1,
             "returned " + std::to_string(count))) {
    return failCycle(host);
  }

  OfxPlugin* p = module.plugin(0);
  if (!check("OfxGetPlugin(0) returns a plug-in", p != nullptr)) return failCycle(host);

  const char* api = p->pluginApi ? p->pluginApi : "";
  const char* id = p->pluginIdentifier ? p->pluginIdentifier : "";
  const std::string idString(id);

  if (!check("pluginApi", std::string(api) == spec.api,
             std::string("\"") + api + "\" (expected \"" + spec.api + "\")")) {
    return failCycle(host);
  }
  if (!check("apiVersion", p->apiVersion == spec.apiVersion,
             std::to_string(p->apiVersion) + " (expected " + std::to_string(spec.apiVersion) + ")")) {
    return failCycle(host);
  }
  if (!check("pluginIdentifier is non-empty", !idString.empty())) return failCycle(host);
  if (!check("pluginIdentifier", idString == spec.id,
             "\"" + idString + "\" (expected \"" + spec.id + "\")")) {
    return failCycle(host);
  }
  if (!check("pluginIdentifier stable across loads",
             firstIdentifier.empty() || idString == firstIdentifier,
             "\"" + idString + "\" (first load \"" + firstIdentifier + "\")")) {
    return failCycle(host);
  }
  if (!check("pluginVersion", p->pluginVersionMajor == static_cast<unsigned>(spec.versionMajor) &&
                                  p->pluginVersionMinor == static_cast<unsigned>(spec.versionMinor),
             std::to_string(p->pluginVersionMajor) + "." + std::to_string(p->pluginVersionMinor) +
                 " (expected " + std::to_string(spec.versionMajor) + "." +
                 std::to_string(spec.versionMinor) + ")")) {
    return failCycle(host);
  }
  if (!check("setHost is non-null", p->setHost != nullptr)) return failCycle(host);
  if (!check("mainEntry is non-null", p->mainEntry != nullptr)) return failCycle(host);

  // ---- the fake host ------------------------------------------------------
  host.setImageSize(spec.width, spec.height);
  host.setPaddedRows(true);
  host.setNegativePitch(spec.negativePitch);
  host.setImageOrigin(spec.topOrigin ? testkit::ImageOrigin::Top
                                     : testkit::ImageOrigin::Bottom);

  std::vector<float> srcRGBA(static_cast<size_t>(spec.width) * spec.height * 4, 0.0f);
  if (!spec.srcFile.empty()) {
    std::vector<float> planar;
    const size_t expected = static_cast<size_t>(spec.width) * spec.height * 3;
    if (!check("read source fixture " + spec.srcFile, readFloats(spec.srcFile, &planar, expected),
               "expected " + std::to_string(expected) + " floats")) {
      return failCycle(host);
    }
    for (int i = 0; i < spec.width * spec.height; ++i) {
      srcRGBA[static_cast<size_t>(i) * 4 + 0] = planar[static_cast<size_t>(i)];
      srcRGBA[static_cast<size_t>(i) * 4 + 1] = planar[static_cast<size_t>(i) + expected / 3];
      srcRGBA[static_cast<size_t>(i) * 4 + 2] = planar[static_cast<size_t>(i) + 2 * (expected / 3)];
      srcRGBA[static_cast<size_t>(i) * 4 + 3] = 1.0f;
    }
    host.setSourcePlanar(planar.data(), spec.width, spec.height);
  } else {
    for (int i = 0; i < spec.width * spec.height; ++i) {
      srcRGBA[static_cast<size_t>(i) * 4 + 0] = 0.25f;
      srcRGBA[static_cast<size_t>(i) * 4 + 1] = 0.50f;
      srcRGBA[static_cast<size_t>(i) * 4 + 2] = 0.75f;
      srcRGBA[static_cast<size_t>(i) * 4 + 3] = 1.00f;
    }
    host.setSourceConstant(0.25f, 0.50f, 0.75f, 1.00f);
  }

  host.attach(p);
  const OfxStatus loadStatus = host.load();
  if (!check("kOfxActionLoad", loadStatus == kOfxStatOK, statusName(loadStatus))) return failCycle(host);
  const OfxStatus describeStatus = host.describe();
  if (!check("kOfxActionDescribe", describeStatus == kOfxStatOK, statusName(describeStatus))) {
    return failCycle(host);
  }

  const OfxStatus contextStatus = host.describeInContext(spec.context.c_str());
  if (!check("kOfxImageEffectActionDescribeInContext(" + spec.context + ")",
             contextStatus == kOfxStatOK, statusName(contextStatus))) {
    return failCycle(host);
  }

  const std::string context0 = host.descriptorString(kOfxImageEffectPropSupportedContexts, 0);
  const std::string context1 = host.descriptorString(kOfxImageEffectPropSupportedContexts, 1);
  if (!check("descriptor lists the requested context", context0 == spec.context || context1 == spec.context,
             "\"" + context0 + "\", \"" + context1 + "\" (expected \"" + spec.context + "\")")) {
    return failCycle(host);
  }
  const std::string safety = host.descriptorString(kOfxImageEffectPluginRenderThreadSafety);
  if (!check("descriptor sets a legal render thread safety", validThreadSafety(safety),
             "\"" + safety + "\"")) {
    return failCycle(host);
  }

  const std::map<std::string, std::string> params = host.declaredParams();
  for (const auto& entry : spec.params) {
    const std::string want = canonicalType(entry.second);
    auto it = params.find(entry.first);
    if (!check("param '" + entry.first + "' declared", it != params.end(),
               "describeInContext declared " + std::to_string(params.size()) + " parameters")) {
      return failCycle(host);
    }
    if (!check("param '" + entry.first + "' type", it->second == want,
               it->second + " (expected " + want + ")")) {
      return failCycle(host);
    }
  }

  const std::vector<std::string> clips = host.declaredClips();
  for (const std::string& name : spec.clips) {
    bool found = false;
    for (const std::string& c : clips) found = found || (c == name);
    if (!check("clip '" + name + "' declared", found,
               "describeInContext declared " + std::to_string(clips.size()) + " clips")) {
      return failCycle(host);
    }
  }

  const OfxStatus createStatus = host.createInstance(spec.context.c_str());
  if (!check("kOfxActionCreateInstance", createStatus == kOfxStatOK, statusName(createStatus))) {
    return failCycle(host);
  }

  const OfxRectI window = {0, 0, spec.width, spec.height};

  if (spec.expectIdentity) {
    std::string identityClip;
    const OfxStatus st = host.isIdentity(0.0, window, &identityClip);
    if (!check("kOfxImageEffectActionIsIdentity at neutral settings", st == kOfxStatOK,
               statusName(st))) {
      return failCycle(host);
    }
    if (!check("identity clip is the source clip", identityClip == kOfxImageEffectSimpleSourceClipName,
               "\"" + identityClip + "\"")) {
      return failCycle(host);
    }
  }

  for (const auto& set : spec.sets) {
    if (!check("set param '" + set.first + "'", host.setParam(set.first, set.second))) return failCycle(host);
  }

  if (spec.expectNotIdentityAfterSet) {
    std::string identityClip;
    const OfxStatus st = host.isIdentity(0.0, window, &identityClip);
    if (!check("isIdentity is false once a setting changes the output",
               st == kOfxStatReplyDefault, statusName(st))) {
      return failCycle(host);
    }
  }

  const OfxStatus renderStatus = host.render(0.0, window);
  if (!check("kOfxImageEffectActionRender", renderStatus == kOfxStatOK, statusName(renderStatus))) {
    return failCycle(host);
  }

  if (!spec.goldenFile.empty()) {
    std::vector<float> golden;
    const size_t pixels = static_cast<size_t>(spec.width) * spec.height;
    if (!check("read golden " + spec.goldenFile, readFloats(spec.goldenFile, &golden, pixels * 3),
               "expected " + std::to_string(pixels * 3) + " floats")) {
      return failCycle(host);
    }
    const std::vector<float> dst = host.destination();
    if (!check("destination is readable", dst.size() == pixels * 4,
               std::to_string(dst.size()) + " floats")) {
      return failCycle(host);
    }
    double worst = 0.0;
    std::string where;
    for (size_t i = 0; i < pixels; ++i) {
      for (int c = 0; c < 3; ++c) {
        const double diff = std::abs(static_cast<double>(dst[i * 4 + c]) -
                                     static_cast<double>(golden[i + c * pixels]));
        if (diff > worst) {
          worst = diff;
          where = "pixel " + std::to_string(i) + " channel " + std::to_string(c);
        }
      }
    }
    if (!check("render matches the golden", worst == 0.0,
               "max deviation " + std::to_string(worst) + " at " + where)) {
      return failCycle(host);
    }
  }

  if (!spec.sets.empty() && spec.goldenFile.empty() && !spec.setStatusOnly) {
    // The parameter change must have had an effect: exactly one gain-like scale
    // is assumed by calling this test with a single numeric override. For any
    // other parameter pass --set-status-only, which asserts the status and the
    // accepted write but skips this comparison.
    const std::vector<float> dst = host.destination();
    if (!check("destination is readable", dst.size() == srcRGBA.size(),
               std::to_string(dst.size()) + " floats")) {
      return failCycle(host);
    }
    const double scale = std::atof(spec.sets.front().second.c_str());
    double worst = 0.0;
    std::string where;
    bool alphaPreserved = true;
    for (size_t i = 0; i < dst.size(); i += 4) {
      for (int c = 0; c < 3; ++c) {
        const double diff = std::abs(static_cast<double>(dst[i + c]) -
                                     static_cast<double>(srcRGBA[i + c]) * scale);
        if (diff > worst) {
          worst = diff;
          where = "pixel " + std::to_string(i / 4) + " channel " + std::to_string(c);
        }
      }
      if (dst[i + 3] != srcRGBA[i + 3]) alphaPreserved = false;
    }
    if (!check("render applied the parameter value", worst == 0.0,
               "max deviation " + std::to_string(worst) + " at " + where)) {
      return failCycle(host);
    }
    if (!check("alpha is unchanged", alphaPreserved)) return failCycle(host);
  }

  if (!runMultiInstance(spec, host, safety, srcRGBA)) return failCycle(host);

  if (spec.byteDstRejected) {
    host.setDestinationByteImage(true);
    const OfxStatus st = host.render(0.0, window);
    host.setDestinationByteImage(false);
    if (!check("render rejects a byte image", st == kOfxStatErrUnsupported, statusName(st))) {
      return failCycle(host);
    }
  }

  const OfxStatus destroyStatus = host.destroyInstance();
  if (!check("kOfxActionDestroyInstance", destroyStatus == kOfxStatOK, statusName(destroyStatus))) {
    return failCycle(host);
  }
  const OfxStatus unloadStatus = host.unload();
  if (!check("kOfxActionUnload", unloadStatus == kOfxStatOK, statusName(unloadStatus))) {
    return failCycle(host);
  }
  ok("unload cycle completed");
  return true;
}

} // namespace

int main(int argc, char** argv) {
  Spec spec;
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: contract_test <plugin.ofx> --id <identifier> [--param name:type]...\n"
                 "       [--clip name]... [--set name=value]... [--context <context>]\n"
                 "       [--identity] [--not-identity-after-set] [--byte-dst-rejected]\n"
                 "       [--negative-pitch] [--image-origin top|bottom] [--set-status-only]\n"
                 "       [--src file --golden file --size WxH]\n"
                 "       [--concurrency-param name] [--concurrency-threads N]\n"
                 "       [--concurrency-iterations N] (threads=0 skips the checks)\n"
                 "\n"
                 "--set without --set-status-only assumes the parameter scales RGB by\n"
                 "atof(value) and leaves alpha alone; use --set-status-only for any\n"
                 "other parameter.\n");
    return 2;
  }
  spec.path = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s needs a value\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--id") {
      spec.id = next("--id");
    } else if (a == "--api") {
      spec.api = next("--api");
    } else if (a == "--api-version") {
      spec.apiVersion = std::atoi(next("--api-version").c_str());
    } else if (a == "--version") {
      const std::string v = next("--version");
      const size_t dot = v.find('.');
      spec.versionMajor = std::atoi(v.substr(0, dot).c_str());
      spec.versionMinor = dot == std::string::npos ? 0 : std::atoi(v.substr(dot + 1).c_str());
    } else if (a == "--context") {
      spec.context = next("--context");
    } else if (a == "--param") {
      const std::string v = next("--param");
      const size_t colon = v.find(':');
      if (colon == std::string::npos) {
        std::fprintf(stderr, "error: --param wants name:type\n");
        return 2;
      }
      spec.params.emplace_back(v.substr(0, colon), canonicalType(v.substr(colon + 1)));
    } else if (a == "--clip") {
      spec.clips.push_back(next("--clip"));
    } else if (a == "--set") {
      const std::string v = next("--set");
      const size_t eq = v.find('=');
      if (eq == std::string::npos) {
        std::fprintf(stderr, "error: --set wants name=value\n");
        return 2;
      }
      spec.sets.emplace_back(v.substr(0, eq), v.substr(eq + 1));
    } else if (a == "--src") {
      spec.srcFile = next("--src");
    } else if (a == "--golden") {
      spec.goldenFile = next("--golden");
    } else if (a == "--size") {
      const std::string v = next("--size");
      const size_t x = v.find('x');
      if (x == std::string::npos) {
        std::fprintf(stderr, "error: --size wants WxH\n");
        return 2;
      }
      spec.width = std::atoi(v.substr(0, x).c_str());
      spec.height = std::atoi(v.substr(x + 1).c_str());
    } else if (a == "--identity") {
      spec.expectIdentity = true;
    } else if (a == "--not-identity-after-set") {
      spec.expectNotIdentityAfterSet = true;
    } else if (a == "--byte-dst-rejected") {
      spec.byteDstRejected = true;
    } else if (a == "--set-status-only") {
      spec.setStatusOnly = true;
    } else if (a == "--negative-pitch") {
      spec.negativePitch = true;
    } else if (a == "--image-origin") {
      const std::string origin = next("--image-origin");
      if (origin != "top" && origin != "bottom") {
        std::fprintf(stderr, "error: --image-origin wants top or bottom\n");
        return 2;
      }
      spec.topOrigin = origin == "top";
    } else if (a == "--concurrency-threads") {
      spec.concurrencyThreads = std::atoi(next("--concurrency-threads").c_str());
    } else if (a == "--concurrency-iterations") {
      spec.concurrencyIterations = std::atoi(next("--concurrency-iterations").c_str());
    } else if (a == "--concurrency-param") {
      spec.scaleParam = next("--concurrency-param");
    } else {
      std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
      return 2;
    }
  }
  if (spec.id.empty()) {
    std::fprintf(stderr, "error: --id is required\n");
    return 2;
  }

  if (!runCycle(spec, std::string())) {
    std::printf("contract_test: %d checks, %d failed\n", g_checks, g_failed);
    return 1;
  }

  // Second full cycle in the same process: setHost + load + describe ... +
  // unload must be repeatable, because a host that re-scans its plug-in folder
  // does exactly this. It must include describe(): the 1.5.1 Support library's
  // unloadAction dereferences gEffectDescriptors.find(id) without checking for
  // end(), so an unload for a plug-in that was never described crashes inside
  // the library, not in the plug-in.
  if (!runCycle(spec, spec.id)) {
    std::printf("contract_test: %d checks, %d failed\n", g_checks, g_failed);
    return 1;
  }

  std::printf("contract_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
