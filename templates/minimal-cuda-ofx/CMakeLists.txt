cmake_minimum_required(VERSION 3.21)

project(minimal-cuda-ofx LANGUAGES CXX)

set(PLUGIN_TARGET "MinimalCudaGain")
set(PLUGIN_BUNDLE "${PLUGIN_TARGET}.ofx.bundle")

option(OFXPLUGIN_ENABLE_CUDA "Compile the CUDA render path (requires the CUDA Toolkit)" ON)
option(OFXPLUGIN_CUDA_STATIC_RUNTIME "Link cudart statically instead of dynamically" OFF)

# ---------------------------------------------------------------------------
# 1. OpenFX headers + C++ Support library
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
    "(include/ofxImageEffect.h not found). Point it at the 'openfx' directory "
    "of a git clone, or at Support/Developer/OpenFX/OpenFX-1.4 shipped inside "
    "DaVinci Resolve.")
endif()

# The Support library is compiled straight into the plugin module. Building it
# as a separate static library works too, but compiling the sources into the
# module removes any chance of a non-PIC object ending up in the .ofx.
file(GLOB OFX_SUPPORT_SOURCES CONFIGURE_DEPENDS
  "${OFX_SOURCE_DIR}/Support/Library/*.cpp")

# ---------------------------------------------------------------------------
# 2. CUDA detection (optional by design: the plugin keeps a CPU path)
# ---------------------------------------------------------------------------
# enable_language(CUDA) itself initialises CMAKE_CUDA_ARCHITECTURES with the
# compiler's default, so AFTER that point `if(DEFINED CMAKE_CUDA_ARCHITECTURES)`
# is always true and a curated default list silently never applies (which made
# nvcc build for its legacy default and emit a deprecated-architecture warning).
# Capture the user's choice first: a -DCMAKE_CUDA_ARCHITECTURES=... from the
# command line is already visible here, an auto-initialised value is not.
set(OFXPLUGIN_USER_CUDA_ARCHS "${CMAKE_CUDA_ARCHITECTURES}")

# Was that cached value put there by the user (-D, a preset, ccmake) or by
# enable_language()? CMake stores a user value as an UNINITIALIZED cache entry -
# 3.31 with the "No help, variable specified on the command line." help string,
# 4.x with the variable's documentation - and its own initialisation as a STRING
# whose help is "CUDA architectures". Both versions were checked; read this
# before enabling the language, which is when the distinction disappears.
set(OFXPLUGIN_CUDA_ARCHS_USER_SET FALSE)
if(DEFINED CACHE{CMAKE_CUDA_ARCHITECTURES})
  get_property(_ofx_archs_cache_type CACHE CMAKE_CUDA_ARCHITECTURES PROPERTY TYPE)
  get_property(_ofx_archs_cache_help CACHE CMAKE_CUDA_ARCHITECTURES PROPERTY HELPSTRING)
  if(_ofx_archs_cache_type STREQUAL "UNINITIALIZED" OR
     _ofx_archs_cache_help MATCHES "command line")
    set(OFXPLUGIN_CUDA_ARCHS_USER_SET TRUE)
  endif()
endif()

if(OFXPLUGIN_ENABLE_CUDA)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    # Remember nvcc's own default list once. CMAKE_CUDA_ARCHITECTURES is a CACHE
    # variable that enable_language() initialises, so on every configure after the
    # first it is already set and looks exactly like a user choice. Without this
    # marker, an incremental rebuild of a clean-configured tree silently shrank
    # the binary to nvcc's legacy default (measured here: 75 only, everything
    # else dropped, no warning) - which is the failure the comment above claims
    # to prevent.
    #
    # Three cases, because recording the wrong value here is what made an
    # explicit -DCMAKE_CUDA_ARCHITECTURES=90 disappear:
    #   no value      enable_language() just wrote nvcc's default -> record it
    #   value, not -D a tree from before the marker existed -> the value is
    #                 nvcc's default, so record it and let the curated list
    #                 apply again; a STATUS line says so
    #   value from -D no default to compare with -> record nothing, section 4
    #                 keeps the choice
    # A tree configured by the version of this file that recorded a -D value as
    # the default carries a marker equal to that value; only a clean reconfigure
    # (or unsetting OFXPLUGIN_COMPILER_DEFAULT_ARCHS) clears it.
    if(NOT DEFINED OFXPLUGIN_COMPILER_DEFAULT_ARCHS)
      if(NOT OFXPLUGIN_USER_CUDA_ARCHS)
        # No value at all: what enable_language() just wrote is nvcc's default.
        set(OFXPLUGIN_COMPILER_DEFAULT_ARCHS "${CMAKE_CUDA_ARCHITECTURES}"
            CACHE INTERNAL "nvcc's default architecture list, used to detect an unset user choice")
      elseif(NOT OFXPLUGIN_CUDA_ARCHS_USER_SET)
        # A value is cached but no marker exists: this tree was configured before
        # the marker existed (or before CUDA was enabled in it). enable_language()
        # wrote that value, so it is nvcc's default - adopt it as the marker and
        # let the curated list apply. Without this an upgraded tree would keep
        # building sm_52, with nvcc's deprecation warning as the only clue.
        set(OFXPLUGIN_COMPILER_DEFAULT_ARCHS "${OFXPLUGIN_USER_CUDA_ARCHS}"
            CACHE INTERNAL "nvcc's default architecture list, used to detect an unset user choice")
        message(STATUS
          "CUDA architectures: cached '${OFXPLUGIN_USER_CUDA_ARCHS}' was not given on the "
          "command line, so it is nvcc's default; the curated list applies. "
          "Pass -DCMAKE_CUDA_ARCHITECTURES=... to choose explicitly.")
      endif()
      # else: a -D value with no marker to compare against. Section 4 keeps it.
    endif()
  else()
    message(WARNING
      "OFXPLUGIN_ENABLE_CUDA=ON but no CUDA compiler was found. "
      "Building the CPU-only render path; the plugin will not advertise CUDA.")
    set(OFXPLUGIN_ENABLE_CUDA OFF)
  endif()
endif()

# ---------------------------------------------------------------------------
# 3. The plugin module
# ---------------------------------------------------------------------------
add_library(${PLUGIN_TARGET} MODULE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/Plugin.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/Plugin.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/Kernel.cuh"
  ${OFX_SUPPORT_SOURCES})

target_include_directories(${PLUGIN_TARGET} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${OFX_SOURCE_DIR}/include"
  "${OFX_SOURCE_DIR}/Support/include"
  "${OFX_SOURCE_DIR}/Support/Library")

target_compile_features(${PLUGIN_TARGET} PRIVATE cxx_std_17)
set_target_properties(${PLUGIN_TARGET} PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_EXTENSIONS OFF)

# Hide everything except the two exported entry points (the third — setHost — is
# a field of the OfxPlugin struct, not an export; the Support library does not
# define OfxSetHost at all, see references/cmake-and-linking.md §3.6). This is not cosmetic:
# every OFX plugin statically links its own copy of the Support library, so with
# default visibility two plugins loaded into the same host process export the
# same mangled symbols and the dynamic loader silently binds one plugin's
# OFX::PropertySet to the other's. On Linux that produces crashes that only
# happen when a *second* plugin is installed. Verified: `nm -D` then shows only
# OfxGetNumberOfPlugins and OfxGetPlugin.
set_target_properties(${PLUGIN_TARGET} PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  # CUDA_VISIBILITY_PRESET is a SEPARATE property. Without it the .cu translation
  # unit is compiled with default visibility and every extern "C" launcher in
  # Kernel.cu becomes an exported dynamic symbol, which defeats the point above
  # (verified: nm -D showed 5 T symbols instead of 2).
  CUDA_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

# Never bake an absolute toolkit path into the shipped .ofx. The bundle then
# resolves a libcudart placed next to it, and otherwise falls back to the host's
# own CUDA runtime. (Default behaviour recorded the build machine's toolkit
# path, e.g. /tmp/cuda-stage/.../lib64, in RUNPATH.) The token is
# platform-specific: dyld needs @loader_path and treats $ORIGIN as a literal
# directory; ELF loaders need $ORIGIN.
if(APPLE)
  set_target_properties(${PLUGIN_TARGET} PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(${PLUGIN_TARGET} PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "$ORIGIN")
endif()

# Hosts look for the binary by exact name inside Contents/<arch>/, so the
# extension must be ".ofx" and the prefix must be empty on every platform.
#
# Which of the two properties CMake honours for a MODULE target depends on the
# platform AND the generator: on Windows/Ninja the module is written to
# LIBRARY_OUTPUT_DIRECTORY, while the MSVC generators use RUNTIME_OUTPUT_DIRECTORY.
# Setting both to the same, platform-correct path removes the difference. Before
# that, `build.bat` on Windows produced
#   Contents/Linux-x86-64/MinimalCudaGain.ofx
# which is a bundle no host ever scans - the build reported success.
#
# The $<1:...> wrapper is load-bearing: multi-configuration generators append
# /Release or /Debug to a plain output directory, and a host will not find the
# binary there. A generator expression suppresses that suffix.
if(WIN32)
  set(_ofx_arch_dir "Win64")
elseif(APPLE)
  set(_ofx_arch_dir "MacOS")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv8.*)$")
  # A Linux bundle on ARM is not a Linux-x86-64 bundle, and naming it that way
  # is the same silent failure as writing Win64 binaries into Linux-x86-64: the
  # build succeeds and no host scans the directory.
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

# A default that is not a passthrough, for the same reason as the CPU template
# (references/resolve-automation.md §14.10): a host may skip render() entirely
# when the effect reports identity, and the Resolve scripting API cannot set an
# OFX parameter afterwards. The harness therefore builds the value in:
#   cmake -S . -B build -DGAIN_DEFAULT=2.0
set(GAIN_DEFAULT "1.0" CACHE STRING
    "Default gain for the CUDA plug-in (1.0 = passthrough)")
target_compile_definitions(${PLUGIN_TARGET} PRIVATE GAIN_DEFAULT=${GAIN_DEFAULT})

# Bundle scaffolding (Resources/ is optional but hosts that read icons want it)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}/Contents/Resources")

# ---------------------------------------------------------------------------
# 4. CUDA-specific wiring
# ---------------------------------------------------------------------------
if(OFXPLUGIN_ENABLE_CUDA)
  target_sources(${PLUGIN_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/Kernel.cu")
  target_compile_definitions(${PLUGIN_TARGET} PRIVATE OFXPLUGIN_WITH_CUDA=1)
  # CUDA::cudart_static carries its own interface link dependencies (dl, rt,
  # pthread on Linux), so no manual -lpthread is needed here. Static linking is
  # opt-in because it duplicates the runtime the host already ships, which is
  # normally undesirable but is the only option on systems without libcudart.
  if(OFXPLUGIN_CUDA_STATIC_RUNTIME)
    target_link_libraries(${PLUGIN_TARGET} PRIVATE CUDA::cudart_static)
  else()
    target_link_libraries(${PLUGIN_TARGET} PRIVATE CUDA::cudart)
  endif()

  # A user choice is the value captured before enable_language(). The marker in
  # section 2 only exists when there was none, so a user list never compares
  # equal to it; a configure check (no GPU needed) proves this selection, the
  # STATUS line and the generated --generate-code flags, not that SASS runs.
  if(OFXPLUGIN_USER_CUDA_ARCHS AND
     NOT "${OFXPLUGIN_USER_CUDA_ARCHS}" STREQUAL "${OFXPLUGIN_COMPILER_DEFAULT_ARCHS}")
    set(_ofx_cuda_archs "${OFXPLUGIN_USER_CUDA_ARCHS}")
    message(STATUS "CUDA architectures: ${_ofx_cuda_archs} (from CMAKE_CUDA_ARCHITECTURES)")
  else()
    # Turing .. Hopper is the practical floor for current post houses.
    set(_ofx_cuda_archs 75 80 86 89 90)
    if(CUDAToolkit_VERSION VERSION_GREATER_EQUAL "12.8")
      # Blackwell (RTX 50 / B100 / GB10). Requires CUDA >= 12.8.
      list(APPEND _ofx_cuda_archs 100 120)
    endif()
    message(STATUS "CUDA architectures: ${_ofx_cuda_archs} (default list)")
  endif()

  # A bare number emits BOTH real SASS and PTX, so the highest architecture
  # still JIT-compiles on GPUs newer than the build toolchain. Any target below
  # 75 makes nvcc print a deprecation warning - read that warning as "this list
  # is stale", not as noise.
  set_target_properties(${PLUGIN_TARGET} PROPERTIES
    CUDA_ARCHITECTURES "${_ofx_cuda_archs}")

  if(OFXPLUGIN_CUDA_STATIC_RUNTIME)
    set_target_properties(${PLUGIN_TARGET} PROPERTIES CUDA_RUNTIME_LIBRARY Static)
  else()
    set_target_properties(${PLUGIN_TARGET} PROPERTIES CUDA_RUNTIME_LIBRARY Shared)
  endif()

  # Cheap diagnostics worth having on every build:
  #  -warn-spills  registers spilling to local memory are a real perf cliff and
  #                the only early warning is this warning.
  # nvcc also warns about deprecated/removed architecture targets, which is the
  # signal that CMAKE_CUDA_ARCHITECTURES has gone stale.
  target_compile_options(${PLUGIN_TARGET} PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:-Xptxas=-warn-spills>)
endif()

# ---------------------------------------------------------------------------
# 5. Install into the host's OFX plugin directory
# ---------------------------------------------------------------------------
if(WIN32)
  set(_ofx_default_dir "$ENV{COMMONPROGRAMFILES}/OFX/Plugins")
elseif(APPLE)
  set(_ofx_default_dir "/Library/OFX/Plugins")
else()
  set(_ofx_default_dir "/usr/OFX/Plugins")
endif()
set(OFX_INSTALL_DIR "${_ofx_default_dir}" CACHE PATH "Host OFX plugin directory")

add_custom_target(install-bundle
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}"
          "${OFX_INSTALL_DIR}/${PLUGIN_BUNDLE}"
  COMMENT "Installing ${PLUGIN_BUNDLE} into ${OFX_INSTALL_DIR}"
  DEPENDS ${PLUGIN_TARGET})

message(STATUS "Default gain: ${GAIN_DEFAULT}")
message(STATUS "OFX plugin : ${PLUGIN_TARGET} (CUDA ${OFXPLUGIN_ENABLE_CUDA})")
message(STATUS "Bundle     : ${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}")
