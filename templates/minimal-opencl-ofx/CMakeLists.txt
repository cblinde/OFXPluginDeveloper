cmake_minimum_required(VERSION 3.21)

project(minimal-opencl-ofx LANGUAGES CXX)

# ---------------------------------------------------------------------------
# The reference plug-in: four things must be right for a host to load it.
#
#   1. a MODULE (shared object) named exactly <Name>.ofx, no "lib" prefix;
#   2. the C++ Support library compiled INTO the module (one copy per plug-in);
#   3. default visibility for the two entry points only;
#   4. position-independent code, because the host dlopen()s it.
#
# Each of those is explained, with the header or support-library line that
# requires it, in references/cmake-and-linking.md.
# ---------------------------------------------------------------------------

set(PLUGIN_TARGET "OpenCLGain")
set(PLUGIN_BUNDLE "${PLUGIN_TARGET}.ofx.bundle")

# ---------------------------------------------------------------------------
# 1. OpenFX headers + C++ Support library, pinned to the same tag as the
#    other templates. build.sh normally pre-populates .ofx-ref and passes this
#    in; the FetchContent path is the fallback for a bare `cmake -S . -B build`.
# ---------------------------------------------------------------------------
set(OFX_SOURCE_DIR "" CACHE PATH "Existing openfx checkout; leave empty to fetch OFX_Release_1.5.1")

if(NOT OFX_SOURCE_DIR)
  include(FetchContent)
  # SOURCE_SUBDIR names a directory with no CMakeLists.txt, so MakeAvailable
  # populates the checkout without adding OpenFX's own project (which builds
  # its samples) to this build. It is also the non-deprecated replacement for
  # FetchContent_Populate() on CMake 3.30+ (CMP0169).
  FetchContent_Declare(openfx
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openfx.git
    GIT_TAG        OFX_Release_1.5.1
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SOURCE_SUBDIR  cmake/do-not-build)
  FetchContent_MakeAvailable(openfx)
  set(OFX_SOURCE_DIR "${openfx_SOURCE_DIR}")
endif()

if(NOT EXISTS "${OFX_SOURCE_DIR}/include/ofxImageEffect.h")
  message(FATAL_ERROR
    "OFX_SOURCE_DIR='${OFX_SOURCE_DIR}' is not an openfx checkout "
    "(include/ofxImageEffect.h not found).")
endif()

# Support/Library contains exactly the translation units the entry points and
# the C++ wrappers need; they are compiled into the module rather than archived
# so that no non-PIC object can reach the link and hidden visibility applies
# uniformly.
file(GLOB OFX_SUPPORT_SOURCES CONFIGURE_DEPENDS
  "${OFX_SOURCE_DIR}/Support/Library/*.cpp")

# ---------------------------------------------------------------------------
# 2. The module
# ---------------------------------------------------------------------------
add_library(${PLUGIN_TARGET} MODULE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/Plugin.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ClRuntime.cpp"
  ${OFX_SUPPORT_SOURCES})

# Two include directories are required, and only two: the C API headers and the
# Support library's. `Support/Library` is listed in some projects; both this
# translation unit and the Support ones compile without it (see references/cmake-and-linking.md).
target_include_directories(${PLUGIN_TARGET} PRIVATE
  "${OFX_SOURCE_DIR}/include"
  "${OFX_SOURCE_DIR}/Support/include")

target_compile_features(${PLUGIN_TARGET} PRIVATE cxx_std_17)

# The plug-in resolves clCreateProgramWithSource and friends at runtime (see
# src/ClRuntime.cpp), so it links the dynamic loader - not an OpenCL library. A
# module that links libOpenCL.so.1 pins the build machine's loader into the
# host's process, which is the thing a render node's own OpenCL setup cannot
# survive.
target_link_libraries(${PLUGIN_TARGET} PRIVATE ${CMAKE_DL_LIBS})

# The gain the plug-in starts at. 1.0 keeps the reference a passthrough, which is
# the honest default for a plug-in people copy; 2.0 makes `isIdentity()` false so
# a host cannot skip `render()`, which is what a host-side proof of the pixel path
# needs. The Resolve scripting API cannot set a parameter afterwards
# (references/resolve-automation.md §15.5), so the harness sets it here instead:
#   cmake -S . -B build -DOPENCL_GAIN_DEFAULT=2.0
set(OPENCL_GAIN_DEFAULT "1.0" CACHE STRING
    "Default gain for the reference plug-in (1.0 = passthrough)")
target_compile_definitions(${PLUGIN_TARGET} PRIVATE
  OPENCL_GAIN_DEFAULT=${OPENCL_GAIN_DEFAULT})

# PIC: the object is dlopen()ed by the host, so every relocation must be
# position-independent.
set_target_properties(${PLUGIN_TARGET} PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_EXTENSIONS OFF)

# Hide everything except the entry points. ofxsImageEffect.cpp:18 marks those two
# with __attribute__((visibility("default"))); everything else - including this
# plug-in's own Support copy - must stay hidden or two plug-ins in one host
# process interpose each other's OFX::PropertySet.
set_target_properties(${PLUGIN_TARGET} PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

# Visibility attributes do not cover weak template instantiations: even with the
# presets above, GCC still exports five weak std::vector<OFX::...> helpers (7
# defined dynamic symbols instead of 2). The version script states the export set
# explicitly, so the artifact is the same whatever the compiler decides.
if(UNIX AND NOT APPLE)
  target_link_options(${PLUGIN_TARGET} PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
endif()

# The host looks for the binary by exact name inside Contents/<arch>/: the
# extension must be ".ofx" and the prefix empty on every platform.
#
# Both output-directory properties are set to the same, platform-correct path
# because a MODULE target goes to LIBRARY_OUTPUT_DIRECTORY under Ninja but to
# RUNTIME_OUTPUT_DIRECTORY under the MSVC generators. Hard-coding them per
# platform (as this file did) put the Windows build in
# Contents/Linux-x86-64/ReferenceGain.ofx, where no host looks. The $<1:...>
# wrapper stops multi-config generators from appending /Release.
if(WIN32)
  set(_ofx_arch_dir "Win64")
elseif(APPLE)
  set(_ofx_arch_dir "MacOS")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv8.*)$")
  # A Linux bundle on ARM is not a Linux-x86-64 bundle; the wrong directory
  # builds fine and no host scans it, which is the platform version of the
  # Win64\Release mistake.
  set(_ofx_arch_dir "Linux-aarch64")
else()
  set(_ofx_arch_dir "Linux-x86-64")
endif()
set(_ofx_bundle_arch_dir "${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}/Contents/${_ofx_arch_dir}")

set_target_properties(${PLUGIN_TARGET} PROPERTIES
  PREFIX ""
  SUFFIX ".ofx"
  OUTPUT_NAME "${PLUGIN_TARGET}"
  LIBRARY_OUTPUT_DIRECTORY "$<1:${_ofx_bundle_arch_dir}>"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${_ofx_bundle_arch_dir}>")

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}/Contents/Resources")

message(STATUS "Default gain: ${OPENCL_GAIN_DEFAULT}")
message(STATUS "OFX plugin : ${PLUGIN_TARGET}")
message(STATUS "Bundle     : ${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}")
