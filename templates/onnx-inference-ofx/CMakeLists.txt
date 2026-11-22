cmake_minimum_required(VERSION 3.21)

project(onnx-inference-ofx LANGUAGES CXX)

set(PLUGIN_TARGET "OnnxInference")
set(PLUGIN_BUNDLE "${PLUGIN_TARGET}.ofx.bundle")

option(OFXPLUGIN_BUNDLE_ONNX_RUNTIME
  "Copy libonnxruntime next to the .ofx so the bundle is self-contained" ON)

# ---------------------------------------------------------------------------
# 1. OpenFX headers + Support library (same policy as the CUDA template)
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

file(GLOB OFX_SUPPORT_SOURCES CONFIGURE_DEPENDS
  "${OFX_SOURCE_DIR}/Support/Library/*.cpp")

# ---------------------------------------------------------------------------
# 2. ONNX Runtime
# ---------------------------------------------------------------------------
# The tarball and the pip wheel disagree about layout: the tarball is
# <root>/include + <root>/lib, while the wheel keeps the headers and the .so
# under onnxruntime/capi. Accept either by looking for the header in both
# places, and fail with a message that says what to download.
set(ONNXRUNTIME_ROOT "" CACHE PATH
  "ONNX Runtime root: contains include/onnxruntime_cxx_api.h and lib/libonnxruntime.so")

if(NOT ONNXRUNTIME_ROOT AND DEFINED ENV{ONNXRUNTIME_ROOT})
  set(ONNXRUNTIME_ROOT "$ENV{ONNXRUNTIME_ROOT}")
endif()

find_path(ONNXRUNTIME_INCLUDE_DIR
  NAMES onnxruntime_cxx_api.h
  HINTS "${ONNXRUNTIME_ROOT}/include" "${ONNXRUNTIME_ROOT}/onnxruntime/capi"
  NO_DEFAULT_PATH)

find_library(ONNXRUNTIME_LIBRARY
  NAMES onnxruntime
  HINTS "${ONNXRUNTIME_ROOT}/lib" "${ONNXRUNTIME_ROOT}/onnxruntime/capi"
  NO_DEFAULT_PATH)

if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
  message(FATAL_ERROR
    "ONNX Runtime not found under ONNXRUNTIME_ROOT='${ONNXRUNTIME_ROOT}'.\n"
    "  Expected include/onnxruntime_cxx_api.h and lib/libonnxruntime.so\n"
    "  (Windows: lib/onnxruntime.lib, plus bin/onnxruntime.dll when bundling).\n"
    "  CPU build: https://github.com/microsoft/onnxruntime/releases -> "
    "onnxruntime-linux-x64-<ver>.tgz\n"
    "  or point ONNXRUNTIME_ROOT at the directory of a pip wheel:\n"
    "    pip download --no-deps onnxruntime -d /tmp/ort && cd /tmp/ort && "
    "unzip -q onnxruntime-*.whl\n"
    "  (a pip wheel ships the .so but not the headers; take them from the same "
    "release tag)")
endif()

message(STATUS "ONNX Runtime include: ${ONNXRUNTIME_INCLUDE_DIR}")
message(STATUS "ONNX Runtime library: ${ONNXRUNTIME_LIBRARY}")

# ---------------------------------------------------------------------------
# 3. The plug-in module
# ---------------------------------------------------------------------------
add_library(${PLUGIN_TARGET} MODULE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/Plugin.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/Plugin.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/AiCore.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/AiCore.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ModelRunner.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ModelRunner.h"
  ${OFX_SUPPORT_SOURCES})

target_include_directories(${PLUGIN_TARGET} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${OFX_SOURCE_DIR}/include"
  "${OFX_SOURCE_DIR}/Support/include"
  "${OFX_SOURCE_DIR}/Support/Library"
  "${ONNXRUNTIME_INCLUDE_DIR}")

target_compile_features(${PLUGIN_TARGET} PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(${PLUGIN_TARGET} PRIVATE /W3)
else()
  target_compile_options(${PLUGIN_TARGET} PRIVATE -Wall -Wextra)
endif()
# Windows must NOT link the import library. An import is resolved by module
# base name, and the loader searches the executable's directory first: in a host
# that ships its own onnxruntime.dll (Resolve 21 ships 1.13 next to
# Resolve.exe) the bundled 1.20 is never opened, GetApi(ORT_API_VERSION) returns
# null and the first ORT call access-violates. ModelRunner loads a private copy
# (onnxruntime-ofx.dll) at first use instead; ORT_API_MANUAL_INIT stops the C++
# wrapper from calling GetApi before that.
if(WIN32)
  target_compile_definitions(${PLUGIN_TARGET} PRIVATE ORT_API_MANUAL_INIT)
else()
  target_link_libraries(${PLUGIN_TARGET} PRIVATE "${ONNXRUNTIME_LIBRARY}")
endif()
target_link_libraries(${PLUGIN_TARGET} PRIVATE ${CMAKE_DL_LIBS})

set_target_properties(${PLUGIN_TARGET} PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_EXTENSIONS OFF)

# Hide everything but the two mandated entry points. Two OFX plug-ins in one
# host process each link their own copy of the Support library; without this,
# `nm -D` shows a few thousand symbols and the loader can bind one plug-in's
# symbols to the other's.
set_target_properties(${PLUGIN_TARGET} PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

# Resolve the runtime from beside the module and never from a build-machine
# path. The token is platform-specific: dyld knows @loader_path and treats
# $ORIGIN as a literal directory name, while ELF loaders know $ORIGIN and not
# @loader_path. Windows has neither rpath nor RUNPATH; the DLL beside the .ofx
# is found by the loader's own search, so setting either there is a no-op.
if(APPLE)
  set_target_properties(${PLUGIN_TARGET} PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(${PLUGIN_TARGET} PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "$ORIGIN")
endif()

# The bundle's arch directory is a platform fact, and getting it wrong is
# silent: the .ofx links, the build reports success, and no host ever scans the
# directory it landed in. A MODULE target uses LIBRARY_OUTPUT_DIRECTORY under
# the Makefile/Ninja generators and RUNTIME_OUTPUT_DIRECTORY under MSVC, so both
# are set; `$<1:...>` keeps a multi-config generator from appending /Release.
if(WIN32)
  set(_ofx_arch_dir "Win64")
elseif(APPLE)
  set(_ofx_arch_dir "MacOS")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv8.*)$")
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

get_filename_component(_onnx_dir "${ONNXRUNTIME_LIBRARY}" DIRECTORY)
if(WIN32)
  # The Windows package puts the import library in lib/ and the DLL in bin/.
  # Copying ${ONNXRUNTIME_LIBRARY} under the name onnxruntime.dll would produce
  # a file the loader rejects - a bundle that looks complete and fails at load.
  # Resolved here rather than in the bundling block below: the test executable
  # needs the same DLL beside it even when OFXPLUGIN_BUNDLE_ONNX_RUNTIME is OFF.
  find_file(ONNXRUNTIME_DLL NAMES onnxruntime.dll
    HINTS "${ONNXRUNTIME_ROOT}/bin" "${ONNXRUNTIME_ROOT}/lib" "${_onnx_dir}"
    NO_DEFAULT_PATH)
endif()

if(OFXPLUGIN_BUNDLE_ONNX_RUNTIME)
  # Ship the file name each platform's loader actually looks for; the branches
  # below say why that name differs.
  if(WIN32)
    if(NOT ONNXRUNTIME_DLL)
      message(FATAL_ERROR
        "OFXPLUGIN_BUNDLE_ONNX_RUNTIME=ON but no onnxruntime.dll under "
        "ONNXRUNTIME_ROOT='${ONNXRUNTIME_ROOT}' (looked in bin/ and lib/).")
    endif()
    set(_onnx_copy "${ONNXRUNTIME_DLL}")
    # A private name, not "onnxruntime.dll": LoadLibrary matches an already
    # loaded module by base name, so even a full path to the bundled copy would
    # return the host's runtime. ModelRunner.cpp loads this exact name from the
    # module directory; keep the two in sync.
    set(_onnx_bundled_name "onnxruntime-ofx.dll")
  elseif(APPLE)
    # dyld records an install name of @rpath/libonnxruntime.<version>.dylib and
    # @loader_path resolves it only if the file keeps that name. find_library()
    # usually returns the unversioned symlink, so resolve it first: copying it
    # as the Linux SONAME produces a bundle whose lookup cannot succeed.
    get_filename_component(_onnx_real "${ONNXRUNTIME_LIBRARY}" REALPATH)
    get_filename_component(_onnx_bundled_name "${_onnx_real}" NAME)
    set(_onnx_copy "${_onnx_real}")
  else()
    # Linux: the binary records NEEDED libonnxruntime.so.1 (the SONAME), so ship
    # exactly that name. Globbing libonnxruntime.so* would copy the symlink and
    # its target, which is how a bundle quietly doubles in size.
    set(_onnx_copy "${ONNXRUNTIME_LIBRARY}")
    set(_onnx_bundled_name "libonnxruntime.so.1")
  endif()
  add_custom_command(TARGET ${PLUGIN_TARGET} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_onnx_copy}"
            "$<TARGET_FILE_DIR:${PLUGIN_TARGET}>/${_onnx_bundled_name}"
    COMMENT "Bundling ONNX Runtime next to the .ofx as ${_onnx_bundled_name}")
endif()

# ---------------------------------------------------------------------------
# 4. Tests: the host harness needs the runtime, not the OFX library
# ---------------------------------------------------------------------------
enable_testing()
add_executable(ai_pipeline_test
  "${CMAKE_CURRENT_SOURCE_DIR}/test/ai_pipeline_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/AiCore.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ModelRunner.cpp")
target_include_directories(ai_pipeline_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${ONNXRUNTIME_INCLUDE_DIR}")
target_compile_features(ai_pipeline_test PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(ai_pipeline_test PRIVATE /W3)
else()
  target_compile_options(ai_pipeline_test PRIVATE -Wall -Wextra)
endif()
if(WIN32)
  target_compile_definitions(ai_pipeline_test PRIVATE ORT_API_MANUAL_INIT)
else()
  target_link_libraries(ai_pipeline_test PRIVATE "${ONNXRUNTIME_LIBRARY}")
endif()
target_link_libraries(ai_pipeline_test PRIVATE ${CMAKE_DL_LIBS})
# Windows resolves an import from the executable's own directory first, then
# System32, then PATH. Copy ours beside the .exe under the private name
# ModelRunner loads, for the same reason the bundle carries it: the build
# directory is the only place this test is meant to run from.
if(WIN32 AND ONNXRUNTIME_DLL)
  add_custom_command(TARGET ai_pipeline_test POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ONNXRUNTIME_DLL}"
            "$<TARGET_FILE_DIR:ai_pipeline_test>/onnxruntime-ofx.dll"
    COMMENT "Copying onnxruntime-ofx.dll next to ai_pipeline_test.exe")
endif()
# No INSTALL_RPATH override here: CMake's build-tree rpath is what lets ctest
# (and a plain ./ai_pipeline_test) find libonnxruntime before anything is
# installed or bundled.
if(NOT WIN32)
  set_target_properties(ai_pipeline_test PROPERTIES
    BUILD_RPATH "${_onnx_dir}")
endif()
add_test(NAME ai_pipeline
  COMMAND ai_pipeline_test "${CMAKE_CURRENT_SOURCE_DIR}/models")

# ---------------------------------------------------------------------------
# 5. Install
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

message(STATUS "OFX plugin : ${PLUGIN_TARGET}")
message(STATUS "Bundle     : ${CMAKE_BINARY_DIR}/bundle/${PLUGIN_BUNDLE}")
