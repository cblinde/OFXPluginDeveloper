cmake_minimum_required(VERSION 3.21)

project(minimal-cuda-ofx LANGUAGES CXX)

# ---------------------------------------------------------------------------
# CMP0169 keeps the explicit FetchContent_Populate() call below working on
# CMake 3.30+; it is a no-op on older versions. See SKILL.md "Dependency
# policy" for why this project opts out of FetchContent_MakeAvailable().
# ---------------------------------------------------------------------------
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

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
  FetchContent_Declare(openfx
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openfx.git
    GIT_TAG        OFX_Release_1.5.1
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
  FetchContent_GetProperties(openfx)
  if(NOT openfx_POPULATED)
    FetchContent_Populate(openfx)
  endif()
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

if(OFXPLUGIN_ENABLE_CUDA)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
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

# Hide everything except the three mandated entry points. This is not cosmetic:
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

# Never bake an absolute toolkit path into the shipped .ofx. With $ORIGIN the
# bundle resolves a libcudart placed next to it, and otherwise falls back to the
# host's own CUDA runtime. (Default behaviour recorded the build machine's
# toolkit path, e.g. /tmp/cuda-stage/.../lib64, in RUNPATH.)
set_target_properties(${PLUGIN_TARGET} PROPERTIES
  BUILD_WITH_INSTALL_RPATH ON
  INSTALL_RPATH "$ORIGIN")

# Hosts look for the binary by exact name inside Contents/<arch>/, so the
# extension must be ".ofx" and the prefix must be empty on every platform.
# Because these directories are set explicitly, multi-config generators do not
# append a per-configuration subdirectory: Debug and Release overwrite each
# other inside the same bundle, which is what a host expects.
set_target_properties(${PLUGIN_TARGET} PROPERTIES
  PREFIX ""
  SUFFIX ".ofx"
  OUTPUT_NAME "${PLUGIN_TARGET}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}/Contents/Linux-x86-64"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}/Contents/Win64")

# Bundle scaffolding (Resources/ is optional but hosts that read icons want it)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}/Contents/Resources")

# ---------------------------------------------------------------------------
# 4. CUDA-specific wiring
# ---------------------------------------------------------------------------
if(OFXPLUGIN_ENABLE_CUDA)
  target_sources(${PLUGIN_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/Kernel.cu")
  target_compile_definitions(${PLUGIN_TARGET} PRIVATE OFXPLUGIN_WITH_CUDA=1)
  target_link_libraries(${PLUGIN_TARGET} PRIVATE CUDA::cudart)

  if(OFXPLUGIN_USER_CUDA_ARCHS)
    set(_ofx_cuda_archs "${OFXPLUGIN_USER_CUDA_ARCHS}")
  else()
    # Turing .. Hopper is the practical floor for current post houses.
    set(_ofx_cuda_archs 75 80 86 89 90)
    if(CUDAToolkit_VERSION VERSION_GREATER_EQUAL "12.8")
      # Blackwell (RTX 50 / B100 / GB10). Requires CUDA >= 12.8.
      list(APPEND _ofx_cuda_archs 100 120)
    endif()
  endif()

  # A bare number emits BOTH real SASS and PTX, so the highest architecture
  # still JIT-compiles on GPUs newer than the build toolchain. Any target below
  # 75 makes nvcc print a deprecation warning - read that warning as "this list
  # is stale", not as noise.
  set_target_properties(${PLUGIN_TARGET} PROPERTIES
    CUDA_ARCHITECTURES "${_ofx_cuda_archs}")
  message(STATUS "CUDA architectures: ${_ofx_cuda_archs}")

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

message(STATUS "OFX plugin : ${PLUGIN_TARGET} (CUDA ${OFXPLUGIN_ENABLE_CUDA})")
message(STATUS "Bundle     : ${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}")
