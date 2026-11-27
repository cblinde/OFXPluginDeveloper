cmake_minimum_required(VERSION 3.21)

project(minimal-vulkan-ofx LANGUAGES CXX)

# ---------------------------------------------------------------------------
# The reference plug-in: four things must be right for a host to load it.
#
#   1. a MODULE (shared object) named exactly <Name>.ofx, no "lib" prefix;
#   2. the C++ Support library compiled INTO the module (one copy per plug-in);
#   * (this template) a compute pipeline the plug-in owns, because no host hands
#     a Vulkan queue to a plug-in - the frame arrives as a CPU image and the
#     plug-in moves it in and out itself;
#   3. default visibility for the two entry points only;
#   4. position-independent code, because the host dlopen()s it.
#
# Each of those is explained, with the header or support-library line that
# requires it, in references/cmake-and-linking.md.
# ---------------------------------------------------------------------------

set(PLUGIN_TARGET "VulkanGain")
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
  "${CMAKE_CURRENT_SOURCE_DIR}/src/VulkanGain.cpp"
  ${OFX_SUPPORT_SOURCES})

# Two include directories are required, and only two: the C API headers and the
# Support library's. `Support/Library` is listed in some projects; both this
# translation unit and the Support ones compile without it (see references/cmake-and-linking.md).
target_include_directories(${PLUGIN_TARGET} PRIVATE
  "${OFX_SOURCE_DIR}/include"
  "${OFX_SOURCE_DIR}/Support/include")

target_compile_features(${PLUGIN_TARGET} PRIVATE cxx_std_17)

# The gain the plug-in starts at. 1.0 keeps the reference a passthrough, which is
# the honest default for a plug-in people copy; 2.0 makes `isIdentity()` false so
# a host cannot skip `render()`, which is what a host-side proof of the pixel path
# needs. The Resolve scripting API cannot set a parameter afterwards
# (references/resolve-automation.md §15.5), so the harness sets it here instead:
#   cmake -S . -B build -DREFERENCE_GAIN_DEFAULT=2.0
set(REFERENCE_GAIN_DEFAULT "1.0" CACHE STRING
    "Default gain for the reference plug-in (1.0 = passthrough)")
target_compile_definitions(${PLUGIN_TARGET} PRIVATE
  REFERENCE_GAIN_DEFAULT=${REFERENCE_GAIN_DEFAULT})


# ---------------------------------------------------------------------------
# 3. Vulkan: headers, one shader, one generated header
# ---------------------------------------------------------------------------
# What the build needs and what it does not:
#
#   * the SDK's **headers** (vulkan_core.h) - they define the structs and the
#     PFN_ types this file's VulkanGain.cpp uses;
#   * a **shader compiler** (glslc, or glslang from the SDK's Bin folder) - the
#     SPIR-V is embedded into the .ofx, so there is no .spv in the bundle;
#   * **not** vulkan-1.lib. Every entry point is resolved through
#     vkGetInstanceProcAddr at run time, so a machine without a Vulkan driver
#     fails with a reason the plug-in reports, not with a DLL that will not load.
#
# Both inputs are cache variables, because find_package(Vulkan) finds only a
# LunarG install and this repository builds on a machine whose headers live
# somewhere else (Gyom03/rife-ofx passes Vulkan_INCLUDE_DIR and Vulkan_LIBRARY
# by hand for the same reason):
#   cmake -S . -B build -DVULKAN_INCLUDE_DIR=<dir with vulkan/vulkan.h> \
#         -DVULKAN_SHADER_COMPILER=<glslc.exe or glslang.exe>
set(OFXPLUGIN_VULKAN ON CACHE BOOL
    "Build the Vulkan compute path; OFF builds the CPU path only")

set(VULKAN_INCLUDE_DIR "" CACHE PATH
    "Directory holding vulkan/vulkan.h (empty: use find_package(Vulkan))")
set(VULKAN_SHADER_COMPILER "" CACHE FILEPATH
    "glslc or glslang executable (empty: search PATH and $$VULKAN_SDK)")

if(OFXPLUGIN_VULKAN)
  set(_vulkan_problem "")

  if(NOT VULKAN_INCLUDE_DIR)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
      set(VULKAN_INCLUDE_DIR "${Vulkan_INCLUDE_DIRS}")
    endif()
  endif()
  if(NOT VULKAN_INCLUDE_DIR OR NOT EXISTS "${VULKAN_INCLUDE_DIR}/vulkan/vulkan.h")
    set(_vulkan_problem "${_vulkan_problem} no vulkan/vulkan.h (set VULKAN_INCLUDE_DIR)")
  endif()

  if(NOT VULKAN_SHADER_COMPILER)
    find_program(_glslc NAMES glslc glslc.exe HINTS "$ENV{VULKAN_SDK}/Bin")
    find_program(_glslang NAMES glslang glslang.exe HINTS "$ENV{VULKAN_SDK}/Bin")
    if(_glslc)
      set(VULKAN_SHADER_COMPILER "${_glslc}")
    elseif(_glslang)
      set(VULKAN_SHADER_COMPILER "${_glslang}")
    endif()
  endif()
  if(NOT VULKAN_SHADER_COMPILER OR NOT EXISTS "${VULKAN_SHADER_COMPILER}")
    set(_vulkan_problem "${_vulkan_problem} no shader compiler (set VULKAN_SHADER_COMPILER)")
  endif()

  # A build that says it builds the Vulkan path and quietly emits a CPU-only
  # bundle is the failure this branch exists to prevent: OFF is a decision, a
  # missing header is not.
  if(_vulkan_problem)
    message(FATAL_ERROR
      "OFXPLUGIN_VULKAN=ON but the toolchain is incomplete:${_vulkan_problem}.\n"
      "Install the LunarG Vulkan SDK (headers and Bin/glslang), or configure with "
      "-DOFXPLUGIN_VULKAN=OFF for a CPU-only bundle.")
  endif()

  set(_spv "${CMAKE_BINARY_DIR}/generated/gain.spv")
  set(_spv_header "${CMAKE_BINARY_DIR}/generated/gain_spv.h")
  add_custom_command(
    OUTPUT "${_spv_header}"
    # glslang needs -o; glslc takes -o too, so one command line covers both.
    COMMAND "${VULKAN_SHADER_COMPILER}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gain.comp"
            -o "${_spv}" --target-env vulkan1.2
    COMMAND "${CMAKE_COMMAND}" -DSPV=${_spv} -DOUTPUT=${_spv_header} -DNAME=kGainSpv
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/spv-header.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gain.comp"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/spv-header.cmake"
    COMMENT "Compiling shaders/gain.comp to SPIR-V and embedding it"
    VERBATIM)

  target_sources(${PLUGIN_TARGET} PRIVATE "${_spv_header}")
  target_include_directories(${PLUGIN_TARGET} PRIVATE
    "${VULKAN_INCLUDE_DIR}"
    "${CMAKE_BINARY_DIR}/generated")
  target_compile_definitions(${PLUGIN_TARGET} PRIVATE OFXPLUGIN_VULKAN=1)
  message(STATUS "Vulkan     : ${VULKAN_INCLUDE_DIR}, shader compiler ${VULKAN_SHADER_COMPILER}")
else()
  message(STATUS "Vulkan     : OFF - the CPU path only")
endif()

# PIC: the object is dlopen()ed by the host, so every relocation must be
# position-independent.
set_target_properties(${PLUGIN_TARGET} PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_EXTENSIONS OFF)

# /EHsc, stated rather than inherited: the guards around the 1.5.1 capability
# writes are try/catch, and the MSVC generators do not all put /EHsc in the
# default flag set - without it the compiler warns C4530 ("handler used, but
# unwind semantics are not enabled") on exactly those blocks, i.e. on the code
# that makes Resolve 21.2 load this plug-in at all.
if(MSVC)
  target_compile_options(${PLUGIN_TARGET} PRIVATE /EHsc)
endif()

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

message(STATUS "Default gain: ${REFERENCE_GAIN_DEFAULT}")
message(STATUS "OFX plugin : ${PLUGIN_TARGET}")
message(STATUS "Bundle     : ${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}")


# ---------------------------------------------------------------------------
# 4. Device suite
# ---------------------------------------------------------------------------
# The part of this template a host run cannot prove cheaply: that the shader,
# the descriptor set, the dispatch and both memory arrangements are right. It
# runs the same `Engine` the plug-in runs - workgroup edges, the two gains a host
# may short-circuit, 1080p and 4K, buffer reuse, five applies on one buffer, the
# same frame through host-visible and device-local memory, the two refusals - and
# compares every channel against the CPU loop, alpha included. It also prints
# throughput for both arrangements and for the loop `render()` falls back to.
if(OFXPLUGIN_VULKAN)
  add_executable(vulkan_suite
    "${CMAKE_CURRENT_SOURCE_DIR}/test/vulkan_suite.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VulkanGain.cpp"
    "${_spv_header}")
  target_include_directories(vulkan_suite PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${VULKAN_INCLUDE_DIR}"
    "${CMAKE_BINARY_DIR}/generated")
  target_compile_features(vulkan_suite PRIVATE cxx_std_17)
  set_target_properties(vulkan_suite PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/self-test>")
  if(NOT WIN32)
    target_link_libraries(vulkan_suite PRIVATE ${CMAKE_DL_LIBS})
  endif()
  message(STATUS "Device suite: ${CMAKE_BINARY_DIR}/self-test/vulkan_suite")
endif()

# An install rule, because a bundle that only exists in the build tree is not
# an installed plug-in: a host scans OFX/Plugins, and the move is what exposes a
# path or a library that was only ever found relative to the build directory.
# `cmake --install build --prefix P` writes
# P/OFX/Plugins/<bundle>/Contents/<arch>/<name>.ofx, the layout every host
# scans, and the test runners install into a temporary prefix and load that
# copy instead of assuming the build tree is one.
# Both destinations are given on every platform: a MODULE target is a RUNTIME
# artefact on Windows and a LIBRARY artefact elsewhere, and CMake rejects a
# MODULE install that names only one of the two even where the other is unused.
install(TARGETS ${PLUGIN_TARGET}
        RUNTIME DESTINATION "OFX/Plugins/${PLUGIN_BUNDLE}/Contents/${_ofx_arch_dir}"
        LIBRARY DESTINATION "OFX/Plugins/${PLUGIN_BUNDLE}/Contents/${_ofx_arch_dir}")
