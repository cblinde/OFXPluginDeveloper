cmake_minimum_required(VERSION 3.21)

# ---------------------------------------------------------------------------
# Tier 1 of the two-tier host verification: the OFFICIAL OpenFX host.
#
# This is not a mock. It compiles `HostSupport/src/*.cpp` - the reference host
# implementation published by the OpenFX project - and its own headless example
# `HostSupport/examples/cacheDemo`, unchanged. The property, memory, parameter,
# multithread, message and image-effect suites a plug-in receives therefore come
# from the standard's maintainers, not from this repository.
#
# cacheDemo is the headless one: it scans OFX_PLUGIN_PATH, loads every bundle it
# finds, runs kOfxActionLoad -> kOfxActionDescribe -> (per context)
# kOfxImageEffectActionDescribeInContext, and prints what the plug-in declared.
# hostDemo is the GLUT GUI, so it is not built here.
#
# It needs expat (for the plugin cache XML), fetched as source - no system
# install, no admin rights.
# ---------------------------------------------------------------------------

project(ofx-official-host LANGUAGES CXX)

set(OFX_SOURCE_DIR "" CACHE PATH "openfx checkout containing HostSupport/ (default: ../../.ofx-ref)")

if(NOT OFX_SOURCE_DIR)
  get_filename_component(_default_ofx "${CMAKE_CURRENT_SOURCE_DIR}/../../.ofx-ref" ABSOLUTE)
  set(OFX_SOURCE_DIR "${_default_ofx}")
endif()

if(NOT EXISTS "${OFX_SOURCE_DIR}/HostSupport/src/ofxhHost.cpp")
  message(FATAL_ERROR
    "OFX_SOURCE_DIR='${OFX_SOURCE_DIR}' has no HostSupport/src/ofxhHost.cpp.\n"
    "Clone the standard next to this repository's templates:\n"
    "  git clone --depth 1 --branch OFX_Release_1.5.1 "
    "https://github.com/AcademySoftwareFoundation/openfx.git .ofx-ref\n"
    "or pass -DOFX_SOURCE_DIR=<checkout>.")
endif()

include(FetchContent)
# Static and minimal: the verification binary must not need a second file on
# PATH (measured: the default shared expat left cacheDemo.exe unable to start
# with "libexpat.dll: cannot open shared object file").
set(EXPAT_SHARED_LIBS     OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_TOOLS     OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_DOCS      OFF CACHE BOOL "" FORCE)
FetchContent_Declare(expat
  GIT_REPOSITORY https://github.com/libexpat/libexpat.git
  GIT_TAG        R_2_6_4
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  SOURCE_SUBDIR  expat)
FetchContent_MakeAvailable(expat)

file(GLOB OFX_HOSTSUPPORT_SOURCES CONFIGURE_DEPENDS
  "${OFX_SOURCE_DIR}/HostSupport/src/*.cpp")

add_library(OfxHost STATIC ${OFX_HOSTSUPPORT_SOURCES})
target_compile_features(OfxHost PUBLIC cxx_std_14)
target_include_directories(OfxHost PUBLIC
  "${OFX_SOURCE_DIR}/include"
  "${OFX_SOURCE_DIR}/HostSupport/include")
target_link_libraries(OfxHost PUBLIC expat::expat)

# The standard's own example, compiled as shipped. -D_CRT_SECURE_NO_WARNINGS is
# MSVC-only noise about the C library calls it uses, not a defect there.
add_executable(cacheDemo
  "${OFX_SOURCE_DIR}/HostSupport/examples/cacheDemo.cpp")
target_compile_features(cacheDemo PRIVATE cxx_std_14)
target_link_libraries(cacheDemo PRIVATE OfxHost)
if(MSVC)
  target_compile_definitions(cacheDemo PRIVATE _CRT_SECURE_NO_WARNINGS)
  # cacheDemo exits from main without deleting the host, so MSVC's leak checker
  # reports the cache singleton as a leak. That is the example's own shape.
  target_compile_definitions(cacheDemo PRIVATE _CRTDBG_MAP_ALLOC)
endif()

message(STATUS "official OFX host: ${OFX_SOURCE_DIR}")
