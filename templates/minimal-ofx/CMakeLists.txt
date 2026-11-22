cmake_minimum_required(VERSION 3.21)

project(minimal-ofx LANGUAGES CXX)

# ---------------------------------------------------------------------------
# The reference plug-in: four things must be right for a host to load it.
#
#   1. a MODULE (shared object) named exactly <Name>.ofx, no "lib" prefix;
#   2. the C++ Support library compiled INTO the module (one copy per plug-in);
#   3. default visibility for the two entry points only;
#   4. position-independent code, because the host dlopen()s it.
#
# Each of those is explained, with the header or support-library line that
# requires it, in BUILD_PLAYBOOK.md.
# ---------------------------------------------------------------------------

# CMP0169 keeps the explicit FetchContent_Populate() call below working on
# CMake 3.30+.
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

set(PLUGIN_TARGET "ReferenceGain")
set(PLUGIN_BUNDLE "${PLUGIN_TARGET}.ofx.bundle")

# ---------------------------------------------------------------------------
# 1. OpenFX headers + C++ Support library, pinned to the same tag as the
#    other templates. build.sh normally pre-populates .ofx-ref and passes this
#    in; the FetchContent path is the fallback for a bare `cmake -S . -B build`.
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
  ${OFX_SUPPORT_SOURCES})

# Two include directories are required, and only two: the C API headers and the
# Support library's. `Support/Library` is listed in some projects; both this
# translation unit and the Support ones compile without it (see BUILD_PLAYBOOK).
target_include_directories(${PLUGIN_TARGET} PRIVATE
  "${OFX_SOURCE_DIR}/include"
  "${OFX_SOURCE_DIR}/Support/include")

target_compile_features(${PLUGIN_TARGET} PRIVATE cxx_std_17)

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
set_target_properties(${PLUGIN_TARGET} PROPERTIES
  PREFIX ""
  SUFFIX ".ofx"
  OUTPUT_NAME "${PLUGIN_TARGET}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}/Contents/Linux-x86-64"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}/Contents/Win64")

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}/Contents/Resources")

message(STATUS "OFX plugin : ${PLUGIN_TARGET}")
message(STATUS "Bundle     : ${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}")
