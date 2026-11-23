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

# Windows loads the runtime instead of linking it, so only the headers (and the
# DLL, checked where it is bundled) are required there. Other platforms link it.
if(NOT ONNXRUNTIME_INCLUDE_DIR)
  message(FATAL_ERROR
    "ONNX Runtime not found under ONNXRUNTIME_ROOT='${ONNXRUNTIME_ROOT}'.\n"
    "  Expected include/onnxruntime_cxx_api.h\n"
    "  (Windows: plus bin/onnxruntime.dll when bundling; the .lib is not used).\n"
    "  CPU build: https://github.com/microsoft/onnxruntime/releases -> "
    "onnxruntime-linux-x64-<ver>.tgz\n"
    "  or point ONNXRUNTIME_ROOT at the directory of a pip wheel:\n"
    "    pip download --no-deps onnxruntime -d /tmp/ort && cd /tmp/ort && "
    "unzip -q onnxruntime-*.whl\n"
    "  (a pip wheel ships the .so but not the headers; take them from the same "
    "release tag)")
elseif(NOT WIN32 AND NOT ONNXRUNTIME_LIBRARY)
  message(FATAL_ERROR
    "ONNX Runtime library not found under ONNXRUNTIME_ROOT='${ONNXRUNTIME_ROOT}'.\n"
    "  Expected lib/libonnxruntime.so (or onnxruntime/capi/ in a pip wheel).")
endif()

message(STATUS "ONNX Runtime include: ${ONNXRUNTIME_INCLUDE_DIR}")
if(WIN32)
  message(STATUS "ONNX Runtime library: not linked (loaded from beside the .ofx)")
else()
  message(STATUS "ONNX Runtime library: ${ONNXRUNTIME_LIBRARY}")
endif()

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

if(ONNXRUNTIME_LIBRARY)
  get_filename_component(_onnx_dir "${ONNXRUNTIME_LIBRARY}" DIRECTORY)
else()
  set(_onnx_dir "${ONNXRUNTIME_ROOT}/lib")
endif()
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
  # Runtime only: no execution provider is bundled here, because none can be
  # built or tested on this machine. A shipping plug-in adds
  # onnxruntime_providers_cuda and the DLLs it imports (cuDNN, cuBLAS/cuBLASLt,
  # cuFFT, cudart) to the same directory, requests the provider first and falls
  # back to this CPU session - AI_MODEL_INTEGRATION.md section 7, which also has
  # the size budget (2.4-2.7 GB) and the error 1114/126 traps.
  #
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
# 3b. CUDA execution provider (optional; off by default)
# ---------------------------------------------------------------------------
# The session asks for the CUDA provider before it is created
# (ModelRunner::AppendCudaProvider) and falls back to the CPU session. What
# makes that request succeed on a user's machine is packaging, not code: the
# provider and everything it imports has to sit beside the .ofx, because the
# host process resolves those imports there and nowhere else. This option copies
# them; leave it OFF for a CPU-only bundle.
#
# Cost, measured on one RTX 3060 / CUDA 12 machine (**B** - no GPU in this
# repository, so nothing here is timed or loaded here):
#   * onnxruntime_providers_cuda alone: 643 MB;
#   * with cuDNN 9 + cuBLAS/cuBLASLt 12 + cuFFT + cudart: a 2.4-2.7 GB bundle.
# That is a product decision - one download for everyone - not a build trick.
#
# The two load failures this block exists to avoid:
#   * 1114 (DllMain could not initialise): a plug-in that LoadLibrary()s
#     onnxruntime_providers_cuda itself. Its DllMain needs the mailbox that only
#     the ORT core sets, so it fails when preloaded; ORT loads it beside the core
#     by itself. Never preload the provider - preload the imports below, if
#     anything.
#   * 126 (ERROR_MOD_NOT_FOUND): the provider is present but one of its imports
#     is not. It imports cuFFT even for a model with no FFT in it, so a bundle
#     missing libcufft/cufft64_*.dll refuses to load with nothing in the host UI
#     to explain it. Missing files therefore produce a warning that names the
#     consequence.
#
# Windows caveat, and it is the reason this option is not simply "turn it on":
# the core runtime is shipped as onnxruntime-ofx.dll (see section 3) so that the
# host's own onnxruntime.dll cannot win the base-name match. A provider DLL
# still imports "onnxruntime.dll" by that exact name, so on a host that has ORT
# loaded (Resolve 21 ships 1.13) the provider binds to the host's runtime and the
# mismatch in AI_MODEL_INTEGRATION.md section 4 comes back. On Windows the honest
# arrangements are a sidecar process for the GPU runtime, or a host that does not
# preload ORT - not a rename. On Linux the core keeps its SONAME, so the copy
# below is consistent and unaffected.
option(OFXPLUGIN_BUNDLE_CUDA_PROVIDER
  "Copy onnxruntime_providers_cuda and the CUDA libraries it imports next to the .ofx (Linux and Windows; macOS warns and skips)" OFF)
option(OFXPLUGIN_CUDA_FROM_HOST
  "Copy only the CUDA provider and its shared library: use the host's cuDNN/cuBLAS/cuFFT/cudart instead of shipping copies (Linux and Windows)" OFF)
set(OFXPLUGIN_CUDA_RUNTIME_DIRS "" CACHE STRING
  "Extra directories to search for the CUDA provider and its imports (semicolon-separated)")

if(OFXPLUGIN_CUDA_FROM_HOST AND NOT OFXPLUGIN_BUNDLE_CUDA_PROVIDER)
  message(WARNING
    "OFXPLUGIN_CUDA_FROM_HOST=ON without OFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON: no "
    "provider is copied, so the switch has nothing to apply to.")
endif()

if(OFXPLUGIN_BUNDLE_CUDA_PROVIDER)
  if(APPLE)
    message(WARNING
      "OFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON has no effect on macOS: CUDA is not "
      "available there and OFX hosts expose Metal/OpenCL instead. The session "
      "will keep falling back to the CPU.")
  else()
    if(NOT OFXPLUGIN_BUNDLE_ONNX_RUNTIME)
      message(WARNING
        "OFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON while OFXPLUGIN_BUNDLE_ONNX_RUNTIME=OFF: "
        "the provider would be copied without the runtime it registers itself "
        "with. Enable both, or neither.")
    endif()

    # Search order: what the caller names, then the usual CUDA layouts, then the
    # ONNX Runtime root - a -gpu package keeps the providers in bin/ or lib/.
    set(_cuda_dirs ${OFXPLUGIN_CUDA_RUNTIME_DIRS})
    foreach(_env CUDA_PATH CUDA_HOME CUDNN_HOME)
      if(DEFINED ENV{${_env}})
        list(APPEND _cuda_dirs "$ENV{${_env}}/bin" "$ENV{${_env}}/lib64" "$ENV{${_env}}/lib")
      endif()
    endforeach()
    list(APPEND _cuda_dirs
      "${ONNXRUNTIME_ROOT}/bin" "${ONNXRUNTIME_ROOT}/lib"
      "/usr/local/cuda/bin" "/usr/local/cuda/lib64"
      "/usr/lib/x86_64-linux-gnu" "/usr/lib/aarch64-linux-gnu")
    list(REMOVE_DUPLICATES _cuda_dirs)

    # First file matching any of the given patterns in any search directory.
    # A function, so each call gets its own loop variables.
    function(ofx_pick_cuda_file out_var)
      foreach(_pattern IN LISTS ARGN)
        foreach(_dir IN LISTS _cuda_dirs)
          if(NOT IS_DIRECTORY "${_dir}")
            continue()
          endif()
          file(GLOB _hits "${_dir}/${_pattern}")
          if(_hits)
            list(GET _hits 0 _hit)
            set(${out_var} "${_hit}" PARENT_SCOPE)
            return()
          endif()
        endforeach()
      endforeach()
      set(${out_var} "" PARENT_SCOPE)
    endfunction()

    if(WIN32)
      set(_provider_patterns onnxruntime_providers_cuda.dll)
      set(_shared_patterns onnxruntime_providers_shared.dll)
      # cuDNN 9 is one dispatcher (cudnn64_9.dll) plus components it loads by base
      # name (cudnn_graph64_9.dll, cudnn_ops64_9.dll, ...). The provider imports
      # only the dispatcher, so a glob for that one file produced a bundle that
      # exits 127 inside the provider - a crash, not the CPU fallback the label
      # promises. Every component in the same directory travels.
      set(_cudnn_patterns cudnn*64_*.dll)
      set(_cublas_patterns cublas64_*.dll)
      set(_cublaslt_patterns cublasLt64_*.dll)
      set(_cufft_patterns cufft64_*.dll)
      set(_cudart_patterns cudart64_*.dll)
    else()
      set(_provider_patterns libonnxruntime_providers_cuda.so)
      set(_shared_patterns libonnxruntime_providers_shared.so)
      set(_cudnn_patterns libcudnn.so.*)
      set(_cublas_patterns libcublas.so.*)
      set(_cublaslt_patterns libcublasLt.so.*)
      set(_cufft_patterns libcufft.so.*)
      set(_cudart_patterns libcudart.so.*)
    endif()

    ofx_pick_cuda_file(_cuda_provider ${_provider_patterns})
    ofx_pick_cuda_file(_cuda_shared ${_shared_patterns})
    # A list, because cuDNN is not one file on Windows. Linux keeps one entry: the
    # SONAME chain resolves the rest from the directory the .so sits in.
    set(_cuda_cudnn_list)
    if(WIN32)
      foreach(_dir IN LISTS _cuda_dirs)
        if(IS_DIRECTORY "${_dir}")
          file(GLOB _cudnn_hits "${_dir}/${_cudnn_patterns}")
          list(APPEND _cuda_cudnn_list ${_cudnn_hits})
        endif()
      endforeach()
      list(REMOVE_DUPLICATES _cuda_cudnn_list)
    else()
      ofx_pick_cuda_file(_cudnn_hit ${_cudnn_patterns})
      if(_cudnn_hit)
        list(APPEND _cuda_cudnn_list "${_cudnn_hit}")
      endif()
    endif()
    if(_cuda_cudnn_list)
      set(_have_cudnn TRUE)
    else()
      set(_have_cudnn FALSE)
    endif()
    ofx_pick_cuda_file(_cuda_cublas ${_cublas_patterns})
    ofx_pick_cuda_file(_cuda_cublaslt ${_cublaslt_patterns})
    ofx_pick_cuda_file(_cuda_cufft ${_cufft_patterns})
    ofx_pick_cuda_file(_cuda_cudart ${_cudart_patterns})

    if(NOT _cuda_provider)
      message(WARNING
        "OFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON but no onnxruntime_providers_cuda was "
        "found in: ${_cuda_dirs}. Nothing is copied: the session keeps falling "
        "back to the CPU. A GPU build needs the -gpu package of ONNX Runtime, or "
        "set OFXPLUGIN_CUDA_RUNTIME_DIRS to the directory that has it.")
    endif()

    # Missing imports are a warning, not an error: a source tree without a CUDA
    # runtime still has to configure. The message names the load error it causes.
    function(ofx_warn_missing_cuda label found)
      if(NOT ${found})
        message(WARNING
          "OFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON: ${label} not found in "
          "${_cuda_dirs}. onnxruntime_providers_cuda imports it, so a bundle "
          "without it fails with error 126 (module not found) instead of "
          "reporting a missing file.")
      endif()
    endfunction()
    ofx_warn_missing_cuda("onnxruntime_providers_shared" _cuda_shared)
    # A provider-only bundle needs nothing beside the provider: the host's stack
    # is what its imports resolve to, so a missing CUDA file here is the point,
    # not a warning.
    if(NOT OFXPLUGIN_CUDA_FROM_HOST)
      ofx_warn_missing_cuda("cuDNN (libcudnn / cudnn*64_*.dll)" _have_cudnn)
      ofx_warn_missing_cuda("cuBLAS (libcublas / cublas64_*.dll)" _cuda_cublas)
      ofx_warn_missing_cuda("cuBLASLt (libcublasLt / cublasLt64_*.dll)" _cuda_cublaslt)
      ofx_warn_missing_cuda("cuFFT (libcufft / cufft64_*.dll)" _cuda_cufft)
      ofx_warn_missing_cuda("CUDA runtime (libcudart / cudart64_*.dll)" _cuda_cudart)
    endif()

    # Provider-only bundle (F30). A host that ships CUDA has its own cuDNN,
    # cuBLAS, cuFFT and cudart loaded before the first render, and Windows resolves
    # an import by base name: our copies would be a second module under a name the
    # host already owns, of whatever minor version we shipped (F26, F32). Shipping
    # none of them removes the possibility instead of guarding against it - the
    # provider imports whatever the host has. The bundle is ~60 % smaller too.
    #
    # The runtime guard in ModelRunner is still what makes the full bundle behave
    # on such a host; this option is the packaging answer, not a substitute.
    if(OFXPLUGIN_CUDA_FROM_HOST)
      message(STATUS
        "CUDA provider bundle: provider only (OFXPLUGIN_CUDA_FROM_HOST=ON). The "
        "host must supply cuDNN 9, cuBLAS/cuBLASLt 12, cuFFT 11 and the CUDA 12 "
        "runtime; the bundle ships none of them, so it cannot create a second "
        "copy of a base name the host has loaded.")
      set(_cuda_cudnn_list "")
      set(_cuda_cublas "")
      set(_cuda_cublaslt "")
      set(_cuda_cufft "")
      set(_cuda_cudart "")
    endif()

    # Copy under the name each library already has: the provider's import table
    # holds those names, so a rename here would be the same failure the core's
    # private name exists to avoid - only with no way for ORT to find it.
    set(_cuda_bytes 0)
    set(_cuda_count 0)
    set(_cuda_files "${_cuda_provider}" "${_cuda_shared}" ${_cuda_cudnn_list}
                    "${_cuda_cublas}" "${_cuda_cublaslt}" "${_cuda_cufft}"
                    "${_cuda_cudart}")
    foreach(_file IN LISTS _cuda_files)
      if(NOT _file)
        continue()
      endif()
      get_filename_component(_name "${_file}" NAME)
      file(SIZE "${_file}" _size)
      math(EXPR _cuda_bytes "${_cuda_bytes} + ${_size}")
      math(EXPR _cuda_count "${_cuda_count} + 1")
      add_custom_command(TARGET ${PLUGIN_TARGET} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_file}"
                "$<TARGET_FILE_DIR:${PLUGIN_TARGET}>/${_name}"
        COMMENT "Bundling ${_name} for the CUDA execution provider")
    endforeach()
    if(_cuda_count GREATER 0)
      # Say the size at configure time: the number is the reason this is a
      # product decision and not a default.
      math(EXPR _cuda_mb "${_cuda_bytes} / 1048576")
      message(STATUS
        "CUDA provider bundle: ${_cuda_count} file(s), ${_cuda_mb} MB")
    endif()
  endif()
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
# The executable's NEEDED entry is the SONAME, libonnxruntime.so.1, but a
# runtime root can ship only the unversioned libonnxruntime.so - the NuGet
# package does. An rpath to that directory cannot satisfy the SONAME, so the
# test links, then dies with "libonnxruntime.so.1: cannot open shared object
# file". Stage the runtime beside the executable under the SONAME and put
# $ORIGIN on the build rpath; ${_onnx_dir} stays first for roots that do carry
# the versioned file. macOS is left alone: dyld resolves the install name
# embedded in the dylib, not a fixed SONAME, so there is nothing to rename.
if(UNIX AND NOT APPLE)
  add_custom_command(TARGET ai_pipeline_test POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ONNXRUNTIME_LIBRARY}"
            "$<TARGET_FILE_DIR:ai_pipeline_test>/libonnxruntime.so.1"
    COMMENT "Staging libonnxruntime.so.1 next to ai_pipeline_test")
  set_target_properties(ai_pipeline_test PROPERTIES
    BUILD_RPATH "${_onnx_dir};\$ORIGIN")
elseif(APPLE)
  # No INSTALL_RPATH override here: CMake's build-tree rpath is what lets ctest
  # (and a plain ./ai_pipeline_test) find libonnxruntime before anything is
  # installed or bundled.
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
