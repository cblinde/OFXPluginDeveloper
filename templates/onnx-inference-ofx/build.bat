@echo off
rem SPDX-License-Identifier: BSD-3-Clause
rem ---------------------------------------------------------------------------
rem Out-of-source build for the ONNX inference plug-in (Windows / MSVC).
rem
rem   set ONNXRUNTIME_ROOT=C:\onnxruntime-win-x64-1.20.1
rem                     the pairing for Resolve 21.2: the provider imports
rem                     cuBLAS 12, which the host carries itself
rem   set ONNXRUNTIME_ROOT=C:\onnxruntime-win-x64-1.32.0
rem                     the current release; its provider imports cuBLAS 13 and
rem                     needs a process that can resolve CUDA 13 (the suite uses
rem                     it; a host bundle for Resolve should not)
rem   .\build.bat                  Release
rem   .\build.bat Debug            another configuration
rem   .\build.bat Release install  build, then copy the bundle into
rem                                %COMMONPROGRAMFILES%\OFX\Plugins
rem
rem The leading .\ matters when NoDefaultCurrentDirectoryInExePath=1 is set (a
rem common policy): a bare `build.bat` is then not found, by cmd.exe itself.
rem
rem Environment:
rem   ONNXRUNTIME_ROOT  required. Needs include\onnxruntime_cxx_api.h and
rem                     bin\onnxruntime.dll from the release zip (or NuGet).
rem                     The DLL is copied into the bundle as
rem                     onnxruntime-ofx.dll and loaded by that name; the .lib
rem                     is NOT linked on Windows and is not required. No release
rem                     zip at hand? The NuGet package carries the same files:
rem                       https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/<version>
rem                     (a .nupkg is a zip) with build\native\include,
rem                     runtimes\win-x64\native\*.lib and ...\*.dll.
rem   OFXPLUGIN_HOST_CUDA_MAJOR
rem                     the CUDA major the target host ships, when it is not the
rem                     one found automatically (Resolve 21.2 ships 12). Detection
rem                     reads the host's own cublas64_<major>.dll, because a driver
rem                     reports the toolkit it supports, not the libraries a host
rem                     ships - a 13 typed in here is how a CUDA-13 provider lands
rem                     in a CUDA-12 host and fails at load with Error 1114.
rem   OFXPLUGIN_HOST_DIR / RESOLVE_EXE
rem                     where to look for that host (default: the DaVinci Resolve
rem                     directory under Program Files)
rem
rem   OFXPLUGIN_BUNDLE_CUDA_PROVIDER
rem                     ON copies onnxruntime_providers_cuda and the DLLs it
rem                     imports beside the .ofx. Needs the -gpu package under
rem                     ONNXRUNTIME_ROOT (the CPU package has no provider);
rem                     off when unset, as in CMakeLists.txt.
rem   OFXPLUGIN_LICENCE_DIRS
rem                     where the CUDA EULA and the cuDNN SLA are, for a
rem                     provider bundle: no package ships them, and the
rem                     artifact check refuses the bundle without them.
rem   OFXPLUGIN_CUDA_RUNTIME_DIRS
rem                     where cuDNN 9, cuBLAS/cuBLASLt, cuFFT and cudart are
rem                     found when they are not already under
rem                     ONNXRUNTIME_ROOT\bin (NVIDIA's redistributables).
rem   OFX_SOURCE_DIR    point at an existing openfx checkout (offline builds);
rem                     when unset, CMake fetches OFX_Release_1.5.1
rem   CMAKE_GENERATOR   force a generator (default: the newest Visual Studio that
rem                     has the C++ toolset)
rem
rem This is the Windows counterpart of build.sh. It stops where that script
rem stops - at a bundle that contains both the .ofx and the runtime DLL - and
rem then runs the same artifact checks (dumpbin /exports, /dependents). The
rem behavioural proof is testkit\build-contract-test.bat, plus the ONNX template's
rem own ai_pipeline test (ctest), which needs the runtime on PATH.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion

set "HERE=%~dp0"
set "BUILD_DIR=%HERE%build"
if "%~1"=="" (set "CONFIG=Release") else (set "CONFIG=%~1")

if /I not "%CONFIG%"=="Release" if /I not "%CONFIG%"=="Debug" if /I not "%CONFIG%"=="RelWithDebInfo" if /I not "%CONFIG%"=="MinSizeRel" (
  echo error: unknown configuration "%CONFIG%"
  echo        use Release, Debug, RelWithDebInfo or MinSizeRel
  exit /b 2
)

rem ---- the one dependency that is not optional -------------------------------
if not defined ONNXRUNTIME_ROOT (
  echo error: ONNXRUNTIME_ROOT is not set.
  echo   Download onnxruntime-win-x64-^<version^>.zip from
  echo   https://github.com/microsoft/onnxruntime/releases and extract it, then
  echo     set ONNXRUNTIME_ROOT=C:\path\to\onnxruntime-win-x64-^<version^>
  echo   Release assets unreachable? The NuGet package has the same files:
  echo     https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/^<version^>
  echo   copy build\native\include\* to include\ and runtimes\win-x64\native\*.dll
  echo   to bin\. ^(The .lib is not used; Windows loads the DLL at runtime.^)
  exit /b 2
)
if not exist "%ONNXRUNTIME_ROOT%\include\onnxruntime_cxx_api.h" (
  echo error: %ONNXRUNTIME_ROOT%\include\onnxruntime_cxx_api.h not found.
  echo   Point ONNXRUNTIME_ROOT at the extracted Windows release zip.
  exit /b 2
)
if not exist "%ONNXRUNTIME_ROOT%\bin\onnxruntime.dll" (
  echo error: no bin\onnxruntime.dll under %ONNXRUNTIME_ROOT%.
  echo   OFXPLUGIN_BUNDLE_ONNX_RUNTIME is ON by default and the bundle must carry
  echo   the DLL next to the .ofx.
  exit /b 2
)

rem ---- generator selection ---------------------------------------------------
rem Ask vswhere for the newest instance that actually has the C++ toolset, and
rem map its major version to the generator name CMake uses. An instance without
rem VC\Tools is skipped by -requires, instead of surfacing later as "could not
rem find any instance of Visual Studio".
set "GEN="
if defined CMAKE_GENERATOR set "GEN=%CMAKE_GENERATOR%"
if not defined GEN (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "!VSWHERE!" (
    for /f "usebackq tokens=1 delims=." %%V in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion`) do (
      if not defined GEN (
        if "%%V"=="18" set "GEN=Visual Studio 18 2026"
        if "%%V"=="17" set "GEN=Visual Studio 17 2022"
        if "%%V"=="16" set "GEN=Visual Studio 16 2019"
        if "%%V"=="15" set "GEN=Visual Studio 15 2017"
      )
    )
  )
)

rem ---- reuse the generator a previous run chose ------------------------------
set "CACHED_GEN="
if exist "%BUILD_DIR%\CMakeCache.txt" (
  for /f "usebackq tokens=1,* delims==" %%A in (`findstr /B /C:"CMAKE_GENERATOR:INTERNAL" "%BUILD_DIR%\CMakeCache.txt"`) do set "CACHED_GEN=%%B"
)
if defined CACHED_GEN if not "!CACHED_GEN!"=="!GEN!" (
  echo [build] reusing generator from build\CMakeCache.txt: !CACHED_GEN!
  set "GEN=!CACHED_GEN!"
)

rem ---- configure + build ----------------------------------------------------
rem Every -D value is quoted: an ONNXRUNTIME_ROOT under "Program Files" is split
rem at the first space otherwise, and CMake reports the tail as a stray argument.
set "EXTRA="-DONNXRUNTIME_ROOT=%ONNXRUNTIME_ROOT%""
if defined OFX_SOURCE_DIR set "EXTRA=%EXTRA% "-DOFX_SOURCE_DIR=%OFX_SOURCE_DIR%""
rem Both CUDA bundle switches are forwarded from the environment: without this
rem line `set OFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON` before build.bat built a bundle
rem with no provider at all, and the suite still passed on the CPU fallback - a
rem green run that proved nothing about the GPU path.
if defined OFXPLUGIN_BUNDLE_CUDA_PROVIDER set "EXTRA=%EXTRA% "-DOFXPLUGIN_BUNDLE_CUDA_PROVIDER=%OFXPLUGIN_BUNDLE_CUDA_PROVIDER%""
rem The CUDA EULA and the cuDNN SLA are not beside the libraries; a provider
rem bundle names the directory that holds them, and the artifact check refuses
rem the bundle without them.
if defined OFXPLUGIN_LICENCE_DIRS set "EXTRA=%EXTRA% "-DOFXPLUGIN_LICENCE_DIRS=%OFXPLUGIN_LICENCE_DIRS%""
if defined OFXPLUGIN_CUDA_RUNTIME_DIRS set "EXTRA=%EXTRA% "-DOFXPLUGIN_CUDA_RUNTIME_DIRS=%OFXPLUGIN_CUDA_RUNTIME_DIRS%""
rem The third switch, and it was the same bug: FROM_HOST=ON with no forward built a
rem bundle that ships its own cuDNN and still reports a green suite.
if defined OFXPLUGIN_CUDA_FROM_HOST set "EXTRA=%EXTRA% "-DOFXPLUGIN_CUDA_FROM_HOST=%OFXPLUGIN_CUDA_FROM_HOST%""

set "GENARG="
if /I "!GEN!"=="Ninja" (
  echo [build] generator: Ninja
  set "VSROOT="
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "!VSWHERE!" (
    for /f "usebackq delims=" %%P in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%P"
  )
  if defined VSROOT if exist "!VSROOT!\VC\Auxiliary\Build\vcvars64.bat" (
    call "!VSROOT!\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
  ) else (
    echo error: Ninja needs cl.exe on PATH and no vcvars64.bat was found.
    echo        Run this from a "x64 Native Tools Command Prompt for VS".
    exit /b 2
  )
  where ninja >nul 2>&1 || (
    echo error: ninja is not on PATH; install it ^(pip install ninja^) or set CMAKE_GENERATOR.
    exit /b 2
  )
  set "GENARG=-G Ninja -DCMAKE_BUILD_TYPE=!CONFIG!"
) else (
  if defined GEN (
    set "GENARG=-G "!GEN!" -A x64"
    echo [build] generator: !GEN!
  ) else (
    echo [build] no Visual Studio C++ toolset found by vswhere; letting CMake choose.
    echo [build] set CMAKE_GENERATOR to pick one explicitly, e.g.
    echo [build]   set CMAKE_GENERATOR=Ninja ^&^& build.bat %CONFIG%
  )
)

cmake -S "%HERE%." -B "%BUILD_DIR%" !GENARG! !EXTRA! || goto :error
cmake --build "%BUILD_DIR%" --config %CONFIG% --parallel || goto :error

rem ---- optional install ------------------------------------------------------
rem %2=install is what README.md documents, and minimal-cuda-ofx already honours
rem it. Without this block the ONNX script accepted the argument and silently
rem installed nothing.
if /I "%~2"=="install" (
  cmake --build "%BUILD_DIR%" --config %CONFIG% --target install-bundle || goto :error
)

rem ---- prove the bundle is where a host will look ---------------------------
rem Same trap as the other templates: a MODULE target lands in Win64 under MSVC
rem and in LIBRARY_OUTPUT_DIRECTORY under Ninja, and a multi-config generator
rem would add \Release. The output properties handle both; this confirms it.
set "BUNDLE=%BUILD_DIR%\bundle\OnnxInference.ofx.bundle"
set "PLUGIN_OFX=%BUNDLE%\Contents\Win64\OnnxInference.ofx"
echo.
echo Bundle: %BUNDLE%
if not exist "%PLUGIN_OFX%" (
  echo.
  echo BUILD FAILED: no .ofx in Contents\Win64
  echo   expected: %PLUGIN_OFX%
  echo   produced:
  dir /s /b "%BUILD_DIR%\bundle\*.ofx" 2>nul
  echo   See the output-directory block in CMakeLists.txt.
  exit /b 1
)
echo   %PLUGIN_OFX%
if not exist "%BUNDLE%\Contents\Win64\onnxruntime-ofx.dll" (
  echo.
  echo BUILD FAILED: the bundle has no onnxruntime-ofx.dll
  echo   the plug-in loads that private name at first use, so a missing copy is
  echo   a render error with a message, not a crash - but the bundle is incomplete.
  echo   Expected OFXPLUGIN_BUNDLE_ONNX_RUNTIME=ON with bin\onnxruntime.dll under
  echo   %ONNXRUNTIME_ROOT%.
  exit /b 1
)
echo   %BUNDLE%\Contents\Win64\onnxruntime-ofx.dll
rem A provider DLL carries its CUDA major in its import names (cublas64_12.dll,
rem cublas64_13.dll), and a host refuses the mismatch as "Error 1114" with nothing
rem else in its log. So the major this bundle must satisfy is read from the host
rem itself - its own cublas64_<major>.dll - not assumed and not taken from the
rem driver, which reports the toolkit it supports rather than the libraries a host
rem ships. 1.20.1's provider imports CUDA 12 and Resolve 21.2 ships 12; 1.32.0's
rem imports 13 and cannot load there. A bundle without a provider has nothing to
rem check and says so.
call :recognise_host
if exist "%BUNDLE%\Contents\Win64\onnxruntime_providers_cuda.dll" (
  echo   CUDA-major gate: the provider must import CUDA %HOST_CUDA_MAJOR%
  python "%HERE%..\..\tools\check-runtime.py" "%BUNDLE%" --require-cuda-major %HOST_CUDA_MAJOR% || goto :error
) else (
  echo   no CUDA provider in this bundle, so the CUDA-major gate does not apply
)
echo.
echo Exports and load-time imports:
rem --strict: the Visual Studio generator branch never runs vcvars64, so dumpbin
rem is not on PATH; the checker finds it through vswhere, and a build that cannot
rem inspect its artifact fails instead of printing a skip.
call "%HERE%..\..\testkit\check-ofx-artifact.bat" "%PLUGIN_OFX%" --strict || goto :error
exit /b 0

:recognise_host
rem Which major the bundle has to satisfy. An explicit OFXPLUGIN_HOST_CUDA_MAJOR
rem wins, and says so - it is a statement about a host this build cannot see.
set "HOST_CUDA_MAJOR=%OFXPLUGIN_HOST_CUDA_MAJOR%"
if defined HOST_CUDA_MAJOR (
  echo   CUDA-major gate: %HOST_CUDA_MAJOR%, from OFXPLUGIN_HOST_CUDA_MAJOR - the
  echo   host directory was not consulted, so this number is the builder's claim
  exit /b 0
)
set "HOST_DIR=%OFXPLUGIN_HOST_DIR%"
if not defined HOST_DIR if defined RESOLVE_EXE for %%I in ("%RESOLVE_EXE%") do set "HOST_DIR=%%~dpI"
if not defined HOST_DIR if exist "%ProgramFiles%\Blackmagic Design\DaVinci Resolve\Resolve.exe" set "HOST_DIR=%ProgramFiles%\Blackmagic Design\DaVinci Resolve"
rem %%~dpI keeps the trailing backslash, and a quoted path ending in one is
rem not a path cmd can test - trim it before the directory is looked at.
if defined HOST_DIR if "!HOST_DIR:~-1!"=="\" set "HOST_DIR=!HOST_DIR:~0,-1!"
if defined HOST_DIR if exist "!HOST_DIR!" (
  for /f "usebackq delims=" %%M in (`python "%HERE%..\..\tools\check-runtime.py" --detect-host-major "!HOST_DIR!" 2^>nul`) do set "HOST_CUDA_MAJOR=%%M"
)
if defined HOST_CUDA_MAJOR (
  echo   CUDA-major gate: %HOST_CUDA_MAJOR%, read from %HOST_DIR%
  exit /b 0
)
rem Nothing to read: fall back to the major Resolve 21.2 ships, and say that this
rem is a default rather than a measurement - the gate is still a gate.
echo   CUDA-major gate: 12 (no host directory found; using the major Resolve 21.2
echo   ships - set OFXPLUGIN_HOST_DIR or RESOLVE_EXE to read it from a host)
set "HOST_CUDA_MAJOR=12"
exit /b 0

:error
echo.
echo BUILD FAILED (see the output above)
exit /b 1
