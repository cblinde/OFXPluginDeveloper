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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "fake_host.h"
// kOfxImageEffectPropCPURenderSupported is declared here, not in ofxImageEffect.h
// (see the note in templates/minimal-ofx/src/Plugin.cpp).
#include "ofxGPURender.h"

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

// Phase markers for a run that stops answering. stderr on purpose: stdout is the
// check stream a script compares, and a stage line must never look like a result.
void stage(const char* what) {
  std::fprintf(stderr, "[stage] %s\n", what);
  std::fflush(stderr);
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
  // --host-profile nuke|resolve|natron: the shape the fake host presents, after
  // the dumps in openfx-misc's README-hosts.txt (nuke, resolve) and the
  // background-renderer shape NatronRenderer presents (natron). A process holds
  // one host, so a suite that wants all three shapes runs the harness three
  // times.
  testkit::HostProfile profile = testkit::HostProfile::Nuke;
  int apiVersion = 1;
  int versionMajor = 1;
  int versionMinor = 0;
  std::vector<std::pair<std::string, std::string>> params; // name -> full type string
  std::vector<std::string> clips;
  std::vector<std::pair<std::string, std::string>> sets;
  std::string srcFile;
  std::string goldenFile;
  // --golden-tolerance T: a bound for a one-off run. Normally the bound belongs
  // to the reference (a `name.<fuzz>.fuzz` file beside it, see resolveReference),
  // so a suite does not choose one tolerance for every platform and provider.
  double goldenTolerance = 0.0;
  bool goldenToleranceSet = false;
  // --fuzz NAME: read the bound from the reference's `<name>.<NAME>.fuzz` file.
  // A missing sidecar is a warning and an exact comparison, not a silent bound.
  std::string fuzzName;
  int width = 64;
  int height = 48;
  bool expectIdentity = false;
  bool expectNotIdentityAfterSet = false;
  bool byteDstRejected = false;
  // --depth half|byte: hand the plug-in source and destination images of that
  // depth and require the frame it produces. Repeatable, so one run covers every
  // depth a plug-in declares. The expectation is the source as the image stores
  // it, times the gain, quantised back to the same depth - the plug-in's own
  // arithmetic, with only the depth conversion supplied by the harness.
  std::vector<testkit::ImageDepth> depths;
  // --src-bounds-inset: publish a source region of definition inset inside the
  // buffer, so the render window covers pixels the source does not have.
  int srcBoundsInset = 0;
  // --dst-bounds-inset: publish a destination region of definition inset inside
  // its buffer, so the render window covers pixels the destination does not
  // have.
  int dstBoundsInset = 0;
  // --roi-expand: the plug-in must answer GetRegionsOfInterest with the
  // requested region grown by this many pixels on every side, clamped to the
  // source region of definition. Negative disables the check.
  int roiExpand = -1;
  // --roi-context: prove the declared region is used, with pixels rather than a
  // rectangle. An inset window has to match the whole-frame render bit for bit
  // when the host hands over the surrounding pixels, and must stop matching when
  // it hands over the window alone.
  bool roiContext = false;
  // --roi-context-tolerance T: the same comparison with a bound instead of
  // bit-equality, for a render the plug-in splits into tiles. TileAccumulator
  // feathers the overlap, so the same neighbourhood is reached through different
  // weights; the declaration still has to keep the window within T of the
  // whole-frame render, and losing it has to push it beyond T.
  double roiContextTolerance = 0.0;
  // --label-param NAME[=VALUE]: a string parameter the plug-in fills in itself
  // (a read-only label). Checked after the first render: it must be set, and the
  // check's name carries the value, because a script has no other way to read
  // what the plug-in wrote. With =VALUE the value must also match, which is how
  // a run asserts what the plug-in read rather than only that it read something.
  std::string labelParam;
  std::string labelExpected;
  // --clip-prop NAME=VALUE: a string property on the source clip's property set,
  // the way a host names the media a clip currently carries. Repeatable.
  std::vector<std::pair<std::string, std::string>> clipProps;
  // --clip-prop-change NAME=VALUE: the same property written again after the
  // first render, followed by a second render on the same clip handle. A plug-in
  // that keys a cache on the handle and never re-reads keeps answering with the
  // old media, which the follow-up label check names.
  bool hasClipPropChange = false;
  std::string clipPropChangeName;
  std::string clipPropChangeValue;
  bool negativePitch = false;
  bool topOrigin = false;
  // --mark X,Y: the plug-in draws a mark at this image position. Asserted in
  // frame coordinates, so a layout whose pointer claims to start at the frame's
  // top shows the mark at the mirrored row instead. The sentinel is the value
  // negative-controls/stub_roi_context.c writes under -DDRAW_MARK.
  bool hasMark = false;
  int markX = 0;
  int markY = 0;
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

// True for a string canonicalType() recognises. An unrecognised spelling used to
// be passed through verbatim, so `--param modelChoice:int` for a choice
// parameter failed as `OfxParamTypeChoice (expected OfxParamTypeInteger)` - a
// message that blames the plug-in for a typo in the command line.
bool isKnownType(const std::string& type) {
  return type == kOfxParamTypeDouble || type == kOfxParamTypeInteger ||
         type == kOfxParamTypeString || type == kOfxParamTypeBoolean ||
         type == kOfxParamTypeRGB || type == kOfxParamTypeRGBA ||
         type == kOfxParamTypeChoice;
}

// A value from the plug-in, flattened for a check name. The harness reports a
// check by name, so the name is also how a script reads a label the plug-in
// wrote; quotes and newlines would break both the log and the grep.
std::string flatValue(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    out += (c == '\n' || c == '\r' || c == '"') ? ' ' : c;
  }
  constexpr size_t kMax = 160;
  if (out.size() > kMax) {
    out = out.substr(0, kMax) + "...";
  }
  return out;
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

// ------------------------------------------------------------------ references
//
// One reference per platform, and a tolerance that belongs to the reference
// instead of to the command line. The convention is OpenImageIO's, reduced to
// what this repository needs:
//
//   name.ext              the generic reference, used when nothing else matches
//   name-<platform>.ext   a reference rendered on one platform, used when the
//                         run happens on that platform
//   name.<fuzz>.fuzz      the bound that reference may be compared with, e.g.
//                         name.cpu.fuzz = 0 and name.cuda.fuzz = 1e-05
//
// Why it matters: a single `--golden-tolerance 1e-5` on the command line is one
// global decision, so every platform and every provider is compared with the
// same loosened rule. Here the strict reference keeps its exact demand, and only
// the reference that cannot be bit-exact says so - a log prints which file was
// read and where the tolerance came from, so the weaker claim is named rather
// than inferred.
const char* platformTag() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

std::string stripExtension(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path;
  return path.substr(0, dot);
}

std::string extensionOf(const std::string& path) {
  const std::string stem = stripExtension(path);
  return stem.size() == path.size() ? std::string() : path.substr(stem.size());
}

bool fileExists(const std::string& path) {
  std::ifstream in(path);
  return in.good();
}

// A `.fuzz` file holds one number: the largest deviation this reference may be
// compared with. Anything after a `#` is a comment, so the file can carry the
// reason it exists next to the number.
bool readFuzz(const std::string& path, double* out) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    char* end = nullptr;
    const double value = std::strtod(line.c_str(), &end);
    if (end == line.c_str()) return false;
    *out = value;
    return true;
  }
  return false;
}

struct Reference {
  std::string path;             // the file actually read
  std::string variant;          // the platform tag, or empty for the generic file
  double tolerance = 0.0;       // from the sidecar, unless the command line overrode it
  std::string toleranceSource;  // "command line", the .fuzz file, or "exact"
};

Reference resolveReference(const std::string& requested, const std::string& fuzzName,
                           bool toleranceFromCli, double cliTolerance) {
  Reference ref;
  const std::string stem = stripExtension(requested);
  const std::string suffix = extensionOf(requested);
  const std::string variant = stem + "-" + platformTag() + suffix;
  if (fileExists(variant)) {
    ref.path = variant;
    ref.variant = platformTag();
  } else {
    ref.path = requested;
  }

  if (toleranceFromCli) {
    ref.tolerance = cliTolerance;
    ref.toleranceSource = "command line";
    return ref;
  }
  ref.tolerance = 0.0;
  ref.toleranceSource = "exact";
  if (fuzzName.empty()) return ref;
  // The sidecar may follow the file that was read (`name-windows.cuda.fuzz`) or
  // the generic stem (`name.cuda.fuzz`); the generic one is the useful default,
  // because a provider's rounding does not depend on the platform.
  const std::string candidates[] = {stripExtension(ref.path) + "." + fuzzName + ".fuzz",
                                    stem + "." + fuzzName + ".fuzz"};
  for (const std::string& candidate : candidates) {
    if (!fileExists(candidate)) continue;
    double value = 0.0;
    if (!readFuzz(candidate, &value)) {
      // A sidecar that cannot be parsed is a failure, not a fallback to exact:
      // silently tightening or loosening a bound is exactly the drift this
      // mechanism exists to stop.
      std::fprintf(stderr, "error: %s does not hold a number\n", candidate.c_str());
      return ref;
    }
    ref.tolerance = value;
    ref.toleranceSource = candidate;
    return ref;
  }
  std::fprintf(stderr, "warning: no %s.*.fuzz beside %s; comparing exactly\n", stem.c_str(),
               requested.c_str());
  return ref;
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
  stage("concurrency");
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

// An error status alone does not prove the frame survived: a plug-in can write
// part of it and then throw. Every float the host owns must be what the last
// successful render left there.
bool checkDestinationUnchanged(const std::vector<float>& before,
                               const std::vector<float>& after) {
  bool unchanged = !before.empty() && before.size() == after.size();
  size_t firstDiff = 0;
  for (size_t i = 0; unchanged && i < before.size(); ++i) {
    if (before[i] != after[i]) {
      unchanged = false;
      firstDiff = i;
    }
  }
  return check("destination is unchanged after the rejected render", unchanged,
               before.size() == after.size()
                   ? "first difference at float " + std::to_string(firstDiff)
                   : std::to_string(before.size()) + " -> " +
                         std::to_string(after.size()) + " floats");
}

// What one rectangle of two densely packed RGBA destinations looks like when it
// is compared: whether it is bit-identical, the largest absolute difference, and
// the pixel where that difference sits. One measurement serves both of the
// roi-context checks, because a tolerance of 0 is the demand for bit-equality:
// a render that reads the pixels its host was told to hand over reproduces the
// whole-frame render exactly, and one that reads a border it did not get leaves
// a seam, which is precisely what a bound of 0 refuses to smooth over.
struct Deviation {
  bool identical = true;
  double worst = 0.0;
  std::string where;
};

// Equality is on the bits, not on ==: that keeps a signed zero distinct (0.0 vs
// -0.0 compares equal but is not the same render) and keeps a NaN identical to
// itself. Both destinations come from FakeHost::destination(), which snapshots
// one frame-sized allocation, so no index below can fall outside them - the
// "destination is readable" check in the caller is what pins that size down.
Deviation maxRectDifference(const std::vector<float>& a, const std::vector<float>& b,
                            int stride, const OfxRectI& rect) {
  Deviation d;
  for (int y = rect.y1; y < rect.y2; ++y) {
    for (int x = rect.x1; x < rect.x2; ++x) {
      const size_t base = (static_cast<size_t>(y) * static_cast<size_t>(stride) +
                           static_cast<size_t>(x)) * 4;
      for (int c = 0; c < 4; ++c) {
        std::uint32_t ba = 0;
        std::uint32_t bb = 0;
        std::memcpy(&ba, &a[base + c], sizeof ba);
        std::memcpy(&bb, &b[base + c], sizeof bb);
        if (ba == bb) continue;
        d.identical = false;
        double diff = std::abs(static_cast<double>(a[base + c]) -
                               static_cast<double>(b[base + c]));
        // A different NaN has no magnitude to report; treat it as unbounded so
        // the comparison below cannot call it small.
        if (!(diff == diff)) diff = std::numeric_limits<double>::infinity();
        if (diff > d.worst || d.where.empty()) {
          d.worst = diff;
          d.where = "pixel (" + std::to_string(x) + "," + std::to_string(y) + ") channel " +
                    std::to_string(c) + ": " + std::to_string(a[base + c]) + " vs " +
                    std::to_string(b[base + c]);
        }
      }
    }
  }
  return d;
}

// ------------------------------------------------------- the golden comparator
//
// One function, used by the render check and by the self-test below, so the
// self-test can fail exactly the code the suite trusts. The rules are the truth
// table a comparator has to satisfy, and three of them are the ones a naive
// `worst <= tolerance` gets wrong:
//
//   identical                     -> pass
//   deviation at / under the bound -> pass; one over it -> fail
//   a non-finite value            -> fail at any bound (NaN != NaN would
//                                    otherwise make the comparison say "small")
//   a reference of the wrong size -> a categorical failure, not a deviation
//   a reference corrupted by one pixel -> fail
struct GoldenVerdict {
  bool pass = false;
  bool identical = true;
  double worst = 0.0;
  std::string where;
  std::string problem; // set when no comparison could be made

  std::string text() const {
    if (!problem.empty()) return problem;
    if (identical) return "identical";
    return "max deviation " + std::to_string(worst) + " at " + where;
  }
};

// dst is the rendered frame, RGBA-interleaved, width*height*4 floats.
// golden is the reference, planar RGB, width*height*3 floats.
GoldenVerdict compareGolden(const std::vector<float>& dst, const std::vector<float>& golden,
                            int width, int height, double tolerance) {
  GoldenVerdict verdict;
  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (dst.size() != pixels * 4) {
    verdict.problem = "the frame holds " + std::to_string(dst.size()) + " floats, expected " +
                      std::to_string(pixels * 4);
    return verdict;
  }
  if (golden.size() != pixels * 3) {
    verdict.problem = "the reference holds " + std::to_string(golden.size()) +
                      " floats, expected " + std::to_string(pixels * 3) +
                      " for " + std::to_string(width) + "x" + std::to_string(height);
    return verdict;
  }
  for (size_t i = 0; i < pixels; ++i) {
    for (int c = 0; c < 3; ++c) {
      const float got = dst[i * 4 + static_cast<size_t>(c)];
      const float want = golden[i + static_cast<size_t>(c) * pixels];
      std::uint32_t gotBits = 0;
      std::uint32_t wantBits = 0;
      std::memcpy(&gotBits, &got, sizeof gotBits);
      std::memcpy(&wantBits, &want, sizeof wantBits);
      if (gotBits == wantBits) continue;
      verdict.identical = false;
      double diff = std::abs(static_cast<double>(got) - static_cast<double>(want));
      // A non-finite value has no magnitude to report. Report it as unbounded,
      // so no finite bound can call it small.
      if (!std::isfinite(static_cast<double>(got)) || !std::isfinite(static_cast<double>(want))) {
        diff = std::numeric_limits<double>::infinity();
      }
      if (diff > verdict.worst || verdict.where.empty()) {
        verdict.worst = diff;
        verdict.where = "pixel " + std::to_string(i % static_cast<size_t>(width)) + "," +
                        std::to_string(i / static_cast<size_t>(width)) + " channel " +
                        std::to_string(c) + ": " + std::to_string(got) + " vs " +
                        std::to_string(want);
      }
    }
  }
  verdict.pass = verdict.identical || verdict.worst <= tolerance;
  return verdict;
}

// A comparator is not trusted until it has been seen failing on the defects it
// exists to catch (plumbline's rule, and the reason FFmpeg's refcmp had to learn
// to fail on empty input). This runs the truth table against compareGolden and
// against the reference lookup; every case states the verdict it expects, so the
// self-test fails when the comparator stops failing.
int runCompareSelftest(const std::string& directory) {
  const int width = 4;
  const int height = 3;
  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  const auto goldenOf = [&](float value) { return std::vector<float>(pixels * 3, value); };
  const auto frameOf = [&](float value) {
    std::vector<float> frame(pixels * 4, value);
    for (size_t i = 0; i < pixels; ++i) frame[i * 4 + 3] = 1.0f;
    return frame;
  };
  const float base = 0.25f;
  int failures = 0;
  const auto expect = [&](const std::string& what, bool condition, const std::string& detail) {
    if (!check("self-test: " + what, condition, detail)) ++failures;
  };

  {
    const GoldenVerdict v = compareGolden(frameOf(base), goldenOf(base), width, height, 0.0);
    expect("identical frames pass, and are reported identical", v.pass && v.identical, v.text());
  }
  {
    std::vector<float> frame = frameOf(base);
    frame[5] += 1e-6f; // one channel, a fraction of an 8-bit LSB
    const GoldenVerdict exact = compareGolden(frame, goldenOf(base), width, height, 0.0);
    const GoldenVerdict bound = compareGolden(frame, goldenOf(base), width, height, 1e-5);
    expect("one sub-LSB pixel fails an exact comparison", !exact.pass, exact.text());
    expect("the same pixel passes a 1e-5 bound", bound.pass, bound.text());
  }
  {
    std::vector<float> frame = frameOf(base);
    frame[6] += 0.25f; // a real fault, in a compared channel (6 = pixel 1, channel 2)
    const GoldenVerdict v = compareGolden(frame, goldenOf(base), width, height, 1e-3);
    expect("a large deviation fails even a loose bound", !v.pass, v.text());
  }
  {
    std::vector<float> below = frameOf(base);
    std::vector<float> above = frameOf(base);
    for (size_t i = 0; i < pixels * 4; ++i) below[i] += 9e-6f;
    for (size_t i = 0; i < pixels * 4; ++i) above[i] += 1.1e-5f;
    const GoldenVerdict under = compareGolden(below, goldenOf(base), width, height, 1e-5);
    const GoldenVerdict over = compareGolden(above, goldenOf(base), width, height, 1e-5);
    expect("every pixel just under the bound passes", under.pass, under.text());
    expect("every pixel just over the bound fails", !over.pass, over.text());
  }
  {
    std::vector<float> shortGolden = goldenOf(base);
    shortGolden.pop_back();
    const GoldenVerdict v = compareGolden(frameOf(base), shortGolden, width, height, 1e-3);
    expect("a reference of the wrong size is a categorical failure, not a small deviation",
           !v.pass && v.problem.find("reference holds") != std::string::npos, v.text());
  }
  {
    std::vector<float> nanGolden = goldenOf(base);
    nanGolden[4] = std::numeric_limits<float>::quiet_NaN();
    const GoldenVerdict v = compareGolden(frameOf(base), nanGolden, width, height, 1.0);
    expect("NaN in the reference fails at any bound", !v.pass, v.text());
  }
  {
    std::vector<float> infFrame = frameOf(base);
    infFrame[2] = std::numeric_limits<float>::infinity();
    const GoldenVerdict v = compareGolden(infFrame, goldenOf(base), width, height, 1.0);
    expect("a non-finite render fails at any bound", !v.pass, v.text());
  }
  {
    std::vector<float> corrupted = goldenOf(base);
    corrupted[10] += 0.01f; // the reference itself is wrong by one pixel
    const GoldenVerdict v = compareGolden(frameOf(base), corrupted, width, height, 1e-5);
    expect("a reference corrupted at one pixel must fail", !v.pass, v.text());
  }

  if (!directory.empty()) {
    // The lookup order, written to disk: a platform variant beside the generic
    // file has to win on that platform, and the sidecar has to supply the bound.
    const std::string generic = directory + "/ref.bin";
    const std::string variant = stripExtension(generic) + "-" + platformTag() + ".bin";
    const std::string fuzz = stripExtension(generic) + ".cuda.fuzz";
    const std::string plain = directory + "/plain.bin";
    const std::string plainFuzz = stripExtension(plain) + ".cuda.fuzz";
    for (const std::string& path : {generic, variant, plain}) {
      std::ofstream out(path, std::ios::binary);
      out.put('x');
    }
    {
      std::ofstream out(fuzz);
      out << "1e-05  # a CUDA render rounds differently in the last bits\n";
    }
    {
      std::ofstream out(plainFuzz);
      out << "# only comments: not a number\n";
    }
    const Reference chosen = resolveReference(generic, "cuda", false, 0.0);
    expect("the platform variant wins over the generic reference on this platform",
           chosen.path == variant && chosen.variant == platformTag(),
           "read " + chosen.path);
    expect("the sidecar supplies the bound, and the log can name it",
           chosen.tolerance > 0.0 && chosen.tolerance < 1e-4 &&
               chosen.toleranceSource.find(".cuda.fuzz") != std::string::npos,
           std::to_string(chosen.tolerance) + " from " + chosen.toleranceSource);
    const Reference overridden = resolveReference(generic, "cuda", true, 0.5);
    expect("the command line still overrides the sidecar", overridden.tolerance == 0.5,
           overridden.toleranceSource);
  }

  std::printf("self-test: %d case(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}

bool sameRect(const OfxRectD& a, const OfxRectD& b) {
  return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

std::string rectText(const OfxRectD& r) {
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), "[%g %g %g %g]", r.x1, r.y1, r.x2, r.y2);
  return buffer;
}

bool runCycle(const Spec& spec, const std::string& firstIdentifier) {
  testkit::FakeHost host;
  host.setHostProfile(spec.profile);
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
  // The profile is an input to the whole cycle: a host that reports a shape the
  // run did not ask for would turn every later failure into a plug-in
  // accusation, so it stops the run here instead of being counted.
  // Every property a profile sets is asserted, not only the two that used to
  // differ: a shape that reaches the property set by accident (a stale build, a
  // half-applied table) would otherwise present a host the run never asked for,
  // and every later failure would be a plug-in accusation.
  {
    const testkit::HostShape& want = testkit::hostShape(spec.profile);
    const int hostTiles = host.hostInteger(kOfxImageEffectPropSupportsTiles);
    const int hostMultiRes = host.hostInteger(kOfxImageEffectPropSupportsMultiResolution);
    const int hostTemporal = host.hostInteger(kOfxImageEffectPropTemporalClipAccess);
    const int hostBackground = host.hostInteger(kOfxImageEffectHostPropIsBackground);
    // The API version is a shape field since the dumps disagreed (1.2 for Nuke,
    // 1.4 for Resolve): a plug-in that branches on it was covered by no profile
    // at all while every profile reported 1.5.
    const int hostApiMajor = host.hostInteger(kOfxPropAPIVersion, 0);
    const int hostApiMinor = host.hostInteger(kOfxPropAPIVersion, 1);
    const int wantTiles = want.tiles ? 1 : 0;
    const int wantMultiRes = want.multiResolution ? 1 : 0;
    const int wantTemporal = want.temporalAccess ? 1 : 0;
    const int wantBackground = want.background ? 1 : 0;
    if (hostTiles != wantTiles || hostMultiRes != wantMultiRes ||
        hostTemporal != wantTemporal || hostBackground != wantBackground ||
        hostApiMajor != want.apiMajor || hostApiMinor != want.apiMinor) {
      // A recorded failure, not just a message: the summary line has to say that
      // something failed, or a log would read "0 failed" for a run that stopped.
      if (!check(std::string("host profile '") + want.key + "' reaches the property set", false,
                 "reports supportsTiles=" + std::to_string(hostTiles) +
                     " supportsMultiResolution=" + std::to_string(hostMultiRes) +
                     " temporalClipAccess=" + std::to_string(hostTemporal) +
                     " isBackground=" + std::to_string(hostBackground) +
                     " apiVersion=" + std::to_string(hostApiMajor) + "." +
                     std::to_string(hostApiMinor) + ", the run asked for " +
                     std::to_string(wantTiles) + "/" + std::to_string(wantMultiRes) + "/" +
                     std::to_string(wantTemporal) + "/" + std::to_string(wantBackground) +
                     " api " + std::to_string(want.apiMajor) + "." +
                     std::to_string(want.apiMinor))) {
        return failCycle(host);
      }
    }
  }
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

  for (const auto& clipProp : spec.clipProps) {
    const bool set = host.setClipProperty(kOfxImageEffectSimpleSourceClipName,
                                          clipProp.first, clipProp.second);
    if (!check("clip property '" + clipProp.first + "' = \"" + flatValue(clipProp.second) + "\"",
               set && host.clipProperty(kOfxImageEffectSimpleSourceClipName,
                                        clipProp.first) == clipProp.second,
               "the source clip does not carry it")) {
      return failCycle(host);
    }
  }

  // A hung harness is unreadable: the last stage line before silence says which
  // phase it never finished.
  stage("render");
  const OfxStatus renderStatus = host.render(0.0, window);
  if (!check("kOfxImageEffectActionRender", renderStatus == kOfxStatOK, statusName(renderStatus))) {
    return failCycle(host);
  }

  // OFX 1.5.1 put two capability properties in the descriptor and two hints in
  // the render arguments, and a host branches on all four before it decides how
  // to render. Asserting them here is what keeps the declaration honest: the "I
  // can do this" strings have to be there, and the render a host flags as a
  // thumbnail with no spatial awareness has to produce the same frame - there is
  // no spatial parameter to disable, so any difference is the plug-in's.
  {
    const std::string cpuSupported = host.descriptorString(kOfxImageEffectPropCPURenderSupported);
    if (!check("descriptor declares CPURenderSupported", cpuSupported == "true",
               "\"" + cpuSupported + "\" (expected \"true\")")) {
      return failCycle(host);
    }
    const std::string noSpatial = host.descriptorString(kOfxImageEffectPropNoSpatialAwareness);
    if (!check("descriptor declares NoSpatialAwareness", noSpatial == "true",
               "\"" + noSpatial + "\" (expected \"true\")")) {
      return failCycle(host);
    }
    const std::vector<float> plainPixels = host.destination();
    stage("thumbnail render");
    const OfxStatus thumbnailStatus = host.renderThumbnail(0.0, window);
    if (!check("kOfxImageEffectActionRender with the 1.5.1 thumbnail hints",
               thumbnailStatus == kOfxStatOK, statusName(thumbnailStatus))) {
      return failCycle(host);
    }
    const std::vector<float> thumbnailPixels = host.destination();
    const bool sameSize = thumbnailPixels.size() == plainPixels.size();
    if (!check("thumbnail render produces the same frame as a full render",
               sameSize && std::equal(plainPixels.begin(), plainPixels.end(), thumbnailPixels.begin()),
               sameSize ? "the destination changed when the host asked for a thumbnail"
                        : "the destination changed size")) {
      return failCycle(host);
    }
  }

  if (!spec.labelParam.empty()) {
    // Read it after the render: the value is written when the plug-in creates
    // its session, which is the first render. The name carries it verbatim so
    // the suite can assert which provider was chosen without a second channel.
    const std::string value = host.paramValue(spec.labelParam);
    const bool written = host.paramIsSet(spec.labelParam);
    const bool matches = spec.labelExpected.empty() || value == spec.labelExpected;
    if (!check("label parameter '" + spec.labelParam + "' = \"" + flatValue(value) + "\"",
               written && !value.empty() && matches,
               !written ? "the plug-in never wrote it; what is shown is the declared "
                          "default, which is not evidence that anything ran"
                        : value.empty() ? "written, but empty"
                                        : "expected \"" + flatValue(spec.labelExpected) + "\"")) {
      return failCycle(host);
    }
  }

  if (spec.hasClipPropChange) {
    // The same clip handle, a different string: how a host says the clip now
    // carries other media. What the plug-in read is visible through the label it
    // writes, so the second render has to be followed by reading it again.
    stage("clip property change");
    const std::string& name = spec.clipPropChangeName;
    const bool set = host.setClipProperty(kOfxImageEffectSimpleSourceClipName, name,
                                          spec.clipPropChangeValue);
    if (!check("clip property '" + name + "' = \"" + flatValue(spec.clipPropChangeValue) + "\"",
               set && host.clipProperty(kOfxImageEffectSimpleSourceClipName, name) ==
                          spec.clipPropChangeValue,
               "the source clip does not carry it")) {
      return failCycle(host);
    }
    const OfxStatus again = host.render(0.0, window);
    if (!check("kOfxImageEffectActionRender after the clip property changed",
               again == kOfxStatOK, statusName(again))) {
      return failCycle(host);
    }
    const std::string seen = host.paramValue(spec.labelParam);
    if (!check("label parameter '" + spec.labelParam + "' follows the clip property",
               seen == spec.clipPropChangeValue,
               "= \"" + flatValue(seen) + "\" (expected \"" +
                   flatValue(spec.clipPropChangeValue) + "\")")) {
      return failCycle(host);
    }
  }

  if (spec.hasMark) {
    stage("mark");
    // Frame coordinates, not buffer coordinates. destination() is the host's
    // own read-back: it always walks frame rows upward from bounds.y1, whatever
    // the pointer handed to the plug-in and the stride sign do. So this check
    // needs no knowledge of the layout - which is the point: a plug-in that
    // addresses only through the pointer puts the mark at the mirrored row when
    // the host says the buffer starts at the frame's top, and a pass-through
    // copy cannot show that because it writes and reads through the same
    // pointer.
    if (!check("mark position inside the frame",
               spec.markX >= 0 && spec.markX < spec.width && spec.markY >= 0 &&
                   spec.markY < spec.height,
               "(" + std::to_string(spec.markX) + "," + std::to_string(spec.markY) +
                   ") is outside " + std::to_string(spec.width) + "x" +
                   std::to_string(spec.height))) {
      return failCycle(host);
    }
    // The row mapping below counts from the frame's origin. An inset destination
    // publishes bounds that no longer start there, so this would read the wrong
    // pixel rather than say so.
    if (!check("mark check needs full-frame destination bounds", spec.dstBoundsInset == 0,
               "--dst-bounds-inset " + std::to_string(spec.dstBoundsInset) +
                   " moves bounds.y1; run the mark check on a full frame")) {
      return failCycle(host);
    }
    // A mark on the mirror line is its own mirror, so neither row decides it.
    if (!check("mark is off the mirror line", 2 * spec.markY != spec.height - 1,
               "row " + std::to_string(spec.markY) + " is its own mirror in a " +
                   std::to_string(spec.height) + "-row frame")) {
      return failCycle(host);
    }
    const std::vector<float> dst = host.destination();
    const size_t pixels = static_cast<size_t>(spec.width) * spec.height;
    if (!check("destination is readable", dst.size() == pixels * 4,
               std::to_string(dst.size()) + " floats")) {
      return failCycle(host);
    }
    auto isMark = [&](int frameY) {
      const float* px = &dst[(static_cast<size_t>(frameY) * spec.width + spec.markX) * 4];
      // Not `near`: <windows.h> pulls minwindef.h, where `near` is a macro that
      // expands to nothing, and MSVC then reads `auto = [...]` (C2513). `far` is
      // the other identifier the SDK defines away.
      auto nearPx = [](float a, float b) { return std::abs(a - b) < 1e-6f; };
      return nearPx(px[0], 0.9375f) && nearPx(px[1], 0.0625f) && nearPx(px[2], 0.5f) &&
             nearPx(px[3], 1.0f);
    };
    const bool atFrame = isMark(spec.markY);
    const bool atMirror = isMark(spec.height - 1 - spec.markY);
    if (!check("overlay mark at (" + std::to_string(spec.markX) + "," +
                   std::to_string(spec.markY) + ")",
               atFrame && !atMirror,
               atMirror ? "the mark is at the mirrored row: the rasteriser assumed the "
                          "pointer's first row is the frame's top"
                        : "no mark above the noise at that position or its mirror")) {
      return failCycle(host);
    }
  }

  if (!spec.goldenFile.empty()) {
    stage("compare golden");
    const size_t pixels = static_cast<size_t>(spec.width) * spec.height;
    // Which reference, and with which bound: both are decisions the run makes
    // for the log, not a global setting it inherits.
    const Reference reference = resolveReference(spec.goldenFile, spec.fuzzName,
                                                 spec.goldenToleranceSet, spec.goldenTolerance);
    if (reference.path == spec.goldenFile) {
      std::printf("  reference: %s (generic: no %s-%s%s beside it)\n", reference.path.c_str(),
                  stripExtension(spec.goldenFile).c_str(), platformTag(),
                  extensionOf(spec.goldenFile).c_str());
    } else {
      std::printf("  reference: %s (the %s variant, preferred over %s)\n", reference.path.c_str(),
                  reference.variant.c_str(), spec.goldenFile.c_str());
    }
    if (reference.tolerance == 0.0) {
      std::printf("  tolerance: 0 (exact)\n");
    } else {
      std::printf("  tolerance: %g (from %s)\n", reference.tolerance,
                  reference.toleranceSource.c_str());
    }
    std::vector<float> golden;
    if (!check("read golden " + reference.path, readFloats(reference.path, &golden, 0),
               "expected " + std::to_string(pixels * 3) + " floats")) {
      return failCycle(host);
    }
    const std::vector<float> dst = host.destination();
    if (!check("destination is readable", dst.size() == pixels * 4,
               std::to_string(dst.size()) + " floats")) {
      return failCycle(host);
    }
    const GoldenVerdict verdict =
        compareGolden(dst, golden, spec.width, spec.height, reference.tolerance);
    const std::string what =
        reference.tolerance == 0.0
            ? std::string("render matches the golden")
            : "render matches the golden within " + std::to_string(reference.tolerance);
    if (!check(what, verdict.pass, verdict.text())) {
      return failCycle(host);
    }
  }

  if (!spec.sets.empty() && spec.goldenFile.empty() && !spec.setStatusOnly) {
    stage("compare parameter");
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
    // The assumption is part of the failure: without it, this reads as "the
    // plug-in ignored the parameter" and sends the reader into the plug-in.
    if (!check("render applied the parameter value", worst == 0.0,
               "max deviation " + std::to_string(worst) + " at " + where +
                   " (--set is read as a gain, dst == src * atof(value); pass "
                   "--set-status-only to check only that the write was accepted)")) {
      return failCycle(host);
    }
    // The assumption is stated in the failure too. The reference template's gain
    // multiplies all four channels (it declares no separate alpha gain), so this
    // check is the one that fails for it under --set gain=..., by design; the
    // suites pass --set-status-only for that plug-in instead of changing its
    // maths to suit the harness.
    if (!check("alpha is unchanged", alphaPreserved,
               "--set gain=... is read as an RGB-only scale; a plug-in whose gain "
               "also multiplies alpha (templates/minimal-ofx does) is expected "
               "to fail here - run it with --set-status-only")) {
      return failCycle(host);
    }
  }

  if (!runMultiInstance(spec, host, safety, srcRGBA)) return failCycle(host);

  if (spec.byteDstRejected) {
    stage("byte destination");
    host.setDestinationByteImage(true);
    const OfxStatus st = host.render(0.0, window);
    host.setDestinationByteImage(false);
    if (!check("render rejects a byte image", st == kOfxStatErrUnsupported, statusName(st))) {
      return failCycle(host);
    }
  }

  for (const testkit::ImageDepth depth : spec.depths) {
    const std::string name = testkit::imageDepthName(depth);
    const std::string label = depth == testkit::ImageDepth::Half     ? "RGBA16F"
                              : depth == testkit::ImageDepth::Byte ? "RGBA8"
                                                                   : "RGBA32F";
    stage(("render at " + name).c_str());
    const std::string depthProp = depth == testkit::ImageDepth::Half     ? kOfxBitDepthHalf
                                 : depth == testkit::ImageDepth::Byte ? kOfxBitDepthByte
                                                                     : kOfxBitDepthFloat;
    bool declaredDepth = false;
    std::string declaredList;
    for (int i = 0; i < 8; ++i) {
      const std::string value = host.descriptorString(kOfxImageEffectPropSupportedPixelDepths, i);
      if (value.empty()) break;
      declaredList += (declaredList.empty() ? "" : ", ") + value;
      declaredDepth = declaredDepth || value == depthProp;
    }
    if (!check("descriptor declares " + label, declaredDepth,
               "declared: " + (declaredList.empty() ? "<none>" : declaredList))) {
      return failCycle(host);
    }
    host.setImageDepth(depth);
    // Code values across the whole range, so a quantisation or a clamp error is
    // a mismatch rather than one lonely pixel.
    std::vector<float> seeded(static_cast<size_t>(spec.width) * spec.height * 4, 0.0f);
    for (int y = 0; y < spec.height; ++y) {
      for (int x = 0; x < spec.width; ++x) {
        // A half image can carry fractions and this one does, so a conversion bug
        // in the plug-in shows up as a mismatch rather than as an exact integer
        // that every bit pattern would survive. A byte image carries code values,
        // so the same frame would quantise to 0..3 and prove nothing; it gets
        // integers across the whole range instead.
        const int span = depth == testkit::ImageDepth::Half ? 4096 : 256;
        const float unit = depth == testkit::ImageDepth::Half ? 1.0f / 64.0f : 1.0f;
        float* px = &seeded[(static_cast<size_t>(y) * spec.width + x) * 4];
        px[0] = static_cast<float>((x * 37 + y * 11) % span) * unit;
        px[1] = static_cast<float>((x * 11 + y * 53) % span) * unit;
        px[2] = static_cast<float>((x * 71 + y * 17) % span) * unit;
        px[3] = depth == testkit::ImageDepth::Half ? 1.0f : 255.0f;
      }
    }
    host.setSourcePixels(seeded, spec.width, spec.height);
    // Re-apply the run's settings: an earlier stage (the concurrency one, which
    // gives each instance its own value) may have left another number on this
    // instance, and this check compares against the settings the run declared.
    for (const auto& set : spec.sets) {
      if (!check("set " + set.first + " for the " + name + " pass",
                 host.setParam(set.first, set.second))) {
        return failCycle(host);
      }
    }
    // What the host actually handed over: the frame after the depth conversion.
    const std::vector<float> handed = host.sourcePixels();
    const OfxStatus depthStatus = host.render(0.0, window);
    const std::vector<float> produced = host.destination();
    host.setImageDepth(testkit::ImageDepth::Float);
    if (!check("render accepts an " + label + " image", depthStatus == kOfxStatOK,
               statusName(depthStatus))) {
      return failCycle(host);
    }
    double gainRGB = 1.0;
    double gainAlpha = 1.0;
    for (const auto& set : spec.sets) {
      if (set.first == "gain") {
        gainRGB = std::atof(set.second.c_str());
      } else if (set.first == "gainAlpha") {
        gainAlpha = std::atof(set.second.c_str());
      }
    }
    if (!check("destination is readable at " + name,
               produced.size() == seeded.size() && handed.size() == seeded.size(),
               std::to_string(produced.size()) + " floats")) {
      return failCycle(host);
    }
    double worst = 0.0;
    int wrong = 0;
    std::string where;
    for (size_t i = 0; i + 3 < produced.size(); i += 4) {
      for (int c = 0; c < 4; ++c) {
        const double expected = testkit::quantiseToDepth(
            handed[i + c] * static_cast<float>(c == 3 ? gainAlpha : gainRGB), depth);
        const double diff = std::abs(static_cast<double>(produced[i + c]) - expected);
        if (diff != 0.0) ++wrong;
        if (diff > worst) {
          worst = diff;
          where = "pixel (" + std::to_string((i / 4) % spec.width) + "," +
                  std::to_string((i / 4) / spec.width) + ") channel " + std::to_string(c) +
                  " (" + std::to_string(produced[i + c]) + " vs " +
                  std::to_string(expected) + ")";
        }
      }
    }
    if (wrong != 0) {
      where += ", " + std::to_string(wrong) + " components differ";
    }
    if (!check("the " + label + " frame is the source times the gain at that depth",
               worst == 0.0,
               "max deviation " + std::to_string(worst) + " at " + where)) {
      return failCycle(host);
    }
  }

  if (spec.roiExpand >= 0) {
    stage("roi region");
    // A plug-in that reports a source region smaller than it reads loses the
    // pixels its edge tiles needed, and the host cannot notice: the frame still
    // renders, only its border is computed from padding. Ask with a region
    // inside the frame, where the growth is visible, and then at the frame
    // border, where the answer has to be clamped.
    const OfxRectD frame{0.0, 0.0, static_cast<double>(host.imageWidth()),
                         static_cast<double>(host.imageHeight())};
    const double margin = 8.0;
    const OfxRectD inside{window.x1 + margin, window.y1 + margin, window.x2 - margin,
                          window.y2 - margin};
    OfxRectD answered{};
    OfxStatus roiStatus = host.getRegionsOfInterest(0.0, inside, &answered);
    if (!check("getRegionsOfInterest answers for the source clip",
               roiStatus == kOfxStatOK, statusName(roiStatus))) {
      return failCycle(host);
    }
    const OfxRectD expected{std::max(inside.x1 - spec.roiExpand, frame.x1),
                            std::max(inside.y1 - spec.roiExpand, frame.y1),
                            std::min(inside.x2 + spec.roiExpand, frame.x2),
                            std::min(inside.y2 + spec.roiExpand, frame.y2)};
    if (!check("source RoI is the requested region grown by its halo",
               sameRect(answered, expected),
               rectText(answered) + " expected " + rectText(expected))) {
      return failCycle(host);
    }
    roiStatus = host.getRegionsOfInterest(0.0, frame, &answered);
    if (!check("getRegionsOfInterest answers at the source bounds",
               roiStatus == kOfxStatOK, statusName(roiStatus))) {
      return failCycle(host);
    }
    if (!check("source RoI is clamped to the source bounds", sameRect(answered, frame),
               rectText(answered) + " expected " + rectText(frame))) {
      return failCycle(host);
    }
  }

  if (spec.roiContext) {
    stage("roi context");
    // GetRegionsOfInterest is a promise about pixels, not about a rectangle. Ask
    // for a window inset inside the frame and compare it with the whole-frame
    // render: the pixels there must agree, because the plug-in said which
    // neighbours it reads and the host handed them over. Then repeat the render
    // against a host that ignores the declaration and supplies only the window -
    // the same pixels must now disagree, or the declared region was never used
    // and the checks would pass for the wrong reason. "Agree" is bit-equality by
    // default and a bound when --roi-context-tolerance is given, for a render
    // that feathers overlapping tiles and so reaches the same neighbourhood
    // through a different rounding.
    //
    // A tolerance of 0 *is* the exact check, so both modes are one measurement
    // with two names: sameEnough() is the only place the bound is interpreted,
    // and a bound of 0 asks for bit-equality.
    const int margin = 3;
    const OfxRectI inner{window.x1 + margin, window.y1 + margin, window.x2 - margin,
                        window.y2 - margin};
    const double tolerance = spec.roiContextTolerance;
    const bool exact = tolerance == 0.0;
    const auto sameEnough = [exact, tolerance](const Deviation& d) {
      return exact ? d.identical : d.worst <= tolerance;
    };
    const auto deviationText = [exact, tolerance](const Deviation& d) {
      if (d.identical) return std::string("no difference inside the window");
      return "max deviation " + std::to_string(d.worst) + " at " + d.where +
             (exact ? std::string(", which the exact check does not allow")
                    : " (tolerance " + std::to_string(tolerance) + ")");
    };
    OfxStatus st = host.render(0.0, window);
    if (!check("roi context: whole-frame render", st == kOfxStatOK, statusName(st))) {
      return failCycle(host);
    }
    const std::vector<float> whole = host.destination();
    if (!check("roi context: destination is readable",
               whole.size() == static_cast<size_t>(spec.width) * spec.height * 4,
               std::to_string(whole.size()) + " floats")) {
      return failCycle(host);
    }
    st = host.render(0.0, inner);
    if (!check("roi context: render of an inset window", st == kOfxStatOK, statusName(st))) {
      return failCycle(host);
    }
    const std::vector<float> withContext = host.destination();
    const Deviation within = maxRectDifference(whole, withContext, spec.width, inner);
    if (!check(exact ? "roi context: the inset window matches the whole frame bit for bit"
                     : "roi context: the inset window stays within the tolerance of the whole frame",
               sameEnough(within), deviationText(within))) {
      return failCycle(host);
    }
    // The second half is a property of a local effect: it asks whether the
    // plug-in read anything outside the window. An effect that declares no
    // context - it answers GetRegionsOfInterest with the region it was asked
    // about - legitimately produces the same picture twice, and failing it would
    // report "nothing outside it was read" about a plug-in that never claimed to
    // read outside. Skip that half when the plug-in says exactly that. A plug-in
    // that leaves the action at ReplyDefault is still checked: "no answer" is not
    // "no context".
    const OfxRectD requested{static_cast<double>(inner.x1), static_cast<double>(inner.y1),
                             static_cast<double>(inner.x2), static_cast<double>(inner.y2)};
    OfxRectD declared{};
    const bool declaresNoContext =
        host.getRegionsOfInterest(0.0, requested, &declared) == kOfxStatOK &&
        declared.x1 == requested.x1 && declared.y1 == requested.y1 &&
        declared.x2 == requested.x2 && declared.y2 == requested.y2;
    if (declaresNoContext) {
      ok("roi context: second half skipped - the plug-in declares no context region "
         "(it answers with the region it was asked about)");
    } else {
      host.setSourceCroppedToWindow(true);
      st = host.render(0.0, inner);
      // Reset before reading back: the cropped image is freed with the mode.
      const std::vector<float> withoutContext = host.destination();
      host.setSourceCroppedToWindow(false);
      if (!check("roi context: render of the same window without a context region",
                 st == kOfxStatOK, statusName(st))) {
        return failCycle(host);
      }
      const Deviation outside = maxRectDifference(whole, withoutContext, spec.width, inner);
      if (!check(exact
                     ? "roi context: without a context region the same window differs"
                     : "roi context: without a context region the same window leaves the tolerance",
                 !sameEnough(outside),
                 outside.identical
                     ? std::string("the window is bit-identical to the whole frame, so nothing "
                                   "outside it was read")
                     : "max deviation " + std::to_string(outside.worst) + " at " +
                           outside.where + ", inside the tolerance " +
                           std::to_string(tolerance) + ", so nothing outside the window was read")) {
        return failCycle(host);
      }
    }
  }

  if (spec.srcBoundsInset > 0) {
    // A host may hand a render window the source RoD does not cover (a crop, a
    // transition, wrong bounds). Image::getPixelAddress() returns nullptr for
    // those coordinates, so the plug-in has to refuse the window once instead of
    // dereferencing a null row per pixel.
    // A baseline frame through the valid window. The byte-destination option
    // above drops the destination image, so an older render cannot be relied on
    // to have left pixels behind; setSourceBoundsInset() leaves this one in
    // place, which makes it the reference for "nothing was written".
    const OfxStatus baseline = host.render(0.0, window);
    if (!check("baseline render before the rejected window", baseline == kOfxStatOK,
               statusName(baseline))) {
      return failCycle(host);
    }
    const std::vector<float> before = host.destination();
    host.setSourceBoundsInset(spec.srcBoundsInset);
    const OfxStatus st = host.render(0.0, window);
    host.setSourceBoundsInset(0);
    if (!check("render rejects a window outside the source bounds",
               st == kOfxStatErrValue, statusName(st))) {
      return failCycle(host);
    }
    const std::vector<float> after = host.destination();
    if (!checkDestinationUnchanged(before, after)) return failCycle(host);
  }

  if (spec.dstBoundsInset > 0) {
    // The write-side mirror of the check above. A host may publish a
    // destination whose region of definition is smaller than the window it asks
    // for (a proxy, a crop, wrong bounds); addressing the destination through
    // data + (window - bounds) then writes outside the pixels the host handed
    // over, and nothing in the host reports that afterwards.
    host.setDestinationBoundsInset(spec.dstBoundsInset);
    // A valid render inside the inset bounds first: it leaves a filled frame in
    // the destination, so "unchanged" compares against real pixels and not
    // against the zero-initialised buffer.
    const int n = spec.dstBoundsInset;
    const OfxRectI inner{window.x1 + n, window.y1 + n, window.x2 - n, window.y2 - n};
    const OfxStatus baseline = host.render(0.0, inner);
    if (!check("baseline render inside the destination bounds", baseline == kOfxStatOK,
               statusName(baseline))) {
      return failCycle(host);
    }
    const std::vector<float> before = host.destinationBuffer();
    const OfxStatus st = host.render(0.0, window);
    // Read back before resetting the inset: the reset frees the image.
    const std::vector<float> after = host.destinationBuffer();
    host.setDestinationBoundsInset(0);
    if (!check("render rejects a window outside the destination bounds",
               st == kOfxStatErrValue, statusName(st))) {
      return failCycle(host);
    }
    if (!checkDestinationUnchanged(before, after)) return failCycle(host);
  }

  const OfxStatus destroyStatus = host.destroyInstance();
  if (!check("kOfxActionDestroyInstance", destroyStatus == kOfxStatOK, statusName(destroyStatus))) {
    return failCycle(host);
  }
  // The unload is where a runtime-owned static bites: a destructor that calls
  // into a half-unmapped module hangs here with the last check already green.
  stage("unload");
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
  // The comparator's self-test needs no plug-in and no host: it is a mode of its
  // own, so a broken comparator can be shown failing without a render in the way.
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--selftest-compare") {
      const std::string directory = (i + 1 < argc) ? argv[i + 1] : std::string();
      return runCompareSelftest(directory);
    }
  }
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: contract_test <plugin.ofx> --id <identifier> [--param name:type]...\n"
                 "       [--clip name]... [--set name=value]... [--context <context>]\n"
                 "       [--identity] [--not-identity-after-set] [--byte-dst-rejected]\n"
                 "       [--depth float|half|byte]...\n"
                 "       [--negative-pitch] [--image-origin top|bottom] [--mark X,Y] [--set-status-only]\n"
                 "       [--src file --golden file --size WxH] [--src-bounds-inset N]\n"
                 "       [--dst-bounds-inset N] [--roi-expand N] [--roi-context]\n"
                 "       [--roi-context-tolerance T] (tiled render: bound, not bit-equality)\n"
                 "       [--golden-tolerance T] (a bound instead of bit-equality, for a\n"
                 "        golden rendered by another provider; overrides --fuzz)\n"
                 "       [--fuzz NAME] (the bound comes from <name>.<NAME>.fuzz beside\n"
                 "        the reference; the reference itself resolves to\n"
                 "        <name>-<platform>.<ext> first, then <name>.<ext>)\n"
                 "       [--selftest-compare [DIR]] (run the comparator's truth table\n"
                 "        instead of a plug-in; DIR is where the lookup cases are\n"
                 "        written)\n"
                 "       [--label-param NAME[=VALUE]] (a string the plug-in sets itself)\n"
                 "       [--clip-prop NAME=VALUE] [--clip-prop-change NAME=VALUE]\n"
                 "         (string properties on the source clip; the change is written\n"
                 "          after the first render and rendered again, so a plug-in\n"
                 "          that caches on the clip handle has to re-read)\n"
                 "       [--concurrency-param name] [--concurrency-threads N]\n"
                 "       [--host-profile nuke|resolve|natron] (tiles + multi-resolution;\n"
                 "        neither; or tiles as a background renderer)\n"
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
    } else if (a == "--host-profile") {
      const std::string v = next("--host-profile");
      if (v == "nuke" || v == "tiles") {
        spec.profile = testkit::HostProfile::Nuke;
      } else if (v == "resolve" || v == "no-tiles") {
        spec.profile = testkit::HostProfile::Resolve;
      } else if (v == "natron" || v == "background") {
        spec.profile = testkit::HostProfile::Natron;
      } else {
        std::fprintf(stderr, "error: --host-profile wants nuke (tiles + multi-resolution), "
                             "resolve (neither) or natron (tiles, background renderer)\n");
        return 2;
      }
    } else if (a == "--param") {
      const std::string v = next("--param");
      const size_t colon = v.find(':');
      if (colon == std::string::npos) {
        std::fprintf(stderr, "error: --param wants name:type\n");
        return 2;
      }
      const std::string type = canonicalType(v.substr(colon + 1));
      if (!isKnownType(type)) {
        std::fprintf(stderr,
                     "error: --param type '%s' is not one of: int (integer), double, "
                     "bool (boolean), string, rgb, rgba, choice\n",
                     v.substr(colon + 1).c_str());
        return 2;
      }
      spec.params.emplace_back(v.substr(0, colon), type);
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
    } else if (a == "--golden-tolerance") {
      spec.goldenTolerance = std::atof(next("--golden-tolerance").c_str());
      spec.goldenToleranceSet = true;
    } else if (a == "--fuzz") {
      spec.fuzzName = next("--fuzz");
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
    } else if (a == "--depth") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: --depth needs float, half or byte\n");
        return 2;
      }
      testkit::ImageDepth depth = testkit::ImageDepth::Float;
      if (!testkit::parseImageDepth(argv[++i], &depth)) {
        std::fprintf(stderr, "error: --depth %s is not float, half or byte\n", argv[i]);
        return 2;
      }
      spec.depths.push_back(depth);
    } else if (a == "--byte-dst-rejected") {
      spec.byteDstRejected = true;
    } else if (a == "--src-bounds-inset") {
      spec.srcBoundsInset = std::atoi(next("--src-bounds-inset").c_str());
    } else if (a == "--dst-bounds-inset") {
      spec.dstBoundsInset = std::atoi(next("--dst-bounds-inset").c_str());
    } else if (a == "--roi-expand") {
      spec.roiExpand = std::atoi(next("--roi-expand").c_str());
    } else if (a == "--roi-context") {
      spec.roiContext = true;
    } else if (a == "--roi-context-tolerance") {
      spec.roiContextTolerance = std::atof(next("--roi-context-tolerance").c_str());
      spec.roiContext = true;
    } else if (a == "--set-status-only") {
      spec.setStatusOnly = true;
    } else if (a == "--label-param") {
      const std::string v = next("--label-param");
      const size_t eq = v.find('=');
      spec.labelParam = v.substr(0, eq);
      if (eq != std::string::npos) spec.labelExpected = v.substr(eq + 1);
    } else if (a == "--clip-prop" || a == "--clip-prop-change") {
      const std::string v = next(a.c_str());
      const size_t eq = v.find('=');
      if (eq == std::string::npos || eq == 0) {
        std::fprintf(stderr, "error: %s wants NAME=VALUE\n", a.c_str());
        return 2;
      }
      if (a == "--clip-prop") {
        spec.clipProps.emplace_back(v.substr(0, eq), v.substr(eq + 1));
      } else {
        spec.hasClipPropChange = true;
        spec.clipPropChangeName = v.substr(0, eq);
        spec.clipPropChangeValue = v.substr(eq + 1);
      }
    } else if (a == "--mark") {
      const std::string where = next("--mark");
      const size_t comma = where.find(',');
      if (comma == std::string::npos) {
        std::fprintf(stderr, "error: --mark wants X,Y\n");
        return 2;
      }
      spec.markX = std::atoi(where.substr(0, comma).c_str());
      spec.markY = std::atoi(where.substr(comma + 1).c_str());
      spec.hasMark = true;
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
  if (spec.hasClipPropChange && spec.labelParam.empty()) {
    std::fprintf(stderr, "error: --clip-prop-change needs --label-param to read what the "
                         "plug-in saw\n");
    return 2;
  }

  std::printf("- host profile: %s (%s)\n", testkit::hostProfileName(spec.profile),
              testkit::hostShape(spec.profile).key);
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
