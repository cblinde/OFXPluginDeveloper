@echo off
rem ---------------------------------------------------------------------------
rem Out-of-source build for the minimal reference plug-in (Windows / MSVC).
rem
rem   .\build.bat                  Release
rem   .\build.bat Debug            another configuration
rem
rem The leading .\ matters when NoDefaultCurrentDirectoryInExePath=1 is set (a
rem common policy): a bare `build.bat` is then not found, by cmd.exe itself.
rem
rem Environment overrides:
rem   OFX_SOURCE_DIR   point at an existing openfx checkout (offline builds, or
rem                    the Support\Developer\OpenFX copy shipped with Resolve);
rem                    when unset, CMake fetches OFX_Release_1.5.1
rem   CMAKE_GENERATOR  force a generator (default: the newest Visual Studio that
rem                    has the C++ toolset)
rem
rem This is the Windows counterpart of build.sh, and it stops where that script
rem stops: at the bundle. Its nm / readelf / dlopen proof has a Windows
rem equivalent (dumpbin /exports, cuobjdump -lelf, LoadLibraryExW) but the
rem artifact-level checks belong in testkit\build-contract-test.bat, which loads
rem the same .ofx through a fake host.
rem
rem The plug-in has no CUDA path, so nothing here looks for a toolkit. What it
rem does reuse from templates\minimal-cuda-ofx\build.bat is the generator logic,
rem for the same reason: naming "Visual Studio 17 2022" is wrong on most
rem machines and fails with a message that does not name the fix.
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

rem ---- generator selection ---------------------------------------------------
rem Ask vswhere for the newest instance that actually has the C++ toolset, and
rem map its major version to the generator name CMake uses. An instance without
rem VC\Tools (a Build Tools install with no C++ workload, for example) is skipped
rem by the -requires filter, which is the whole point: it is otherwise invisible
rem until CMake reports "could not find any instance of Visual Studio".
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
rem CMake refuses to reuse a build directory configured with a different
rem generator ("Does not match the generator used previously: Ninja"). Reuse what
rem the cache says instead of dead-ending the second run.
set "CACHED_GEN="
if exist "%BUILD_DIR%\CMakeCache.txt" (
  for /f "usebackq tokens=1,* delims==" %%A in (`findstr /B /C:"CMAKE_GENERATOR:INTERNAL" "%BUILD_DIR%\CMakeCache.txt"`) do set "CACHED_GEN=%%B"
)
if defined CACHED_GEN if not "!CACHED_GEN!"=="!GEN!" (
  echo [build] reusing generator from build\CMakeCache.txt: !CACHED_GEN!
  set "GEN=!CACHED_GEN!"
)

rem ---- configure + build ----------------------------------------------------
rem The -D value is quoted: an OFX_SOURCE_DIR under "Program Files" is split at
rem the first space otherwise, and CMake reports the tail as a stray argument.
set "EXTRA="
if defined OFX_SOURCE_DIR set "EXTRA=%EXTRA% "-DOFX_SOURCE_DIR=%OFX_SOURCE_DIR%""
rem The Vulkan headers and the shader compiler, when they are not where
rem find_package(Vulkan) and PATH would look: the two cache variables the
rem template documents, passed straight through.
if defined VULKAN_INCLUDE_DIR set "EXTRA=%EXTRA% "-DVULKAN_INCLUDE_DIR=%VULKAN_INCLUDE_DIR%""
if defined VULKAN_SHADER_COMPILER set "EXTRA=%EXTRA% "-DVULKAN_SHADER_COMPILER=%VULKAN_SHADER_COMPILER%""
rem OFF builds the CPU path only - what a CI job without the Vulkan SDK uses,
rem and the one way to get a bundle there that does not claim a GPU path.
if defined OFXPLUGIN_VULKAN set "EXTRA=%EXTRA% "-DOFXPLUGIN_VULKAN=%OFXPLUGIN_VULKAN%""

set "GENARG="
if /I "!GEN!"=="Ninja" (
  echo [build] generator: Ninja
  rem A single-config generator needs CMAKE_BUILD_TYPE, and it needs cl.exe on
  rem PATH, which a plain cmd does not have. No toolset pinning here: that is a
  rem CUDA constraint (nvcc's host-compiler check), and this plug-in has no CUDA.
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

cmake -S "%HERE%." -B "%BUILD_DIR%" !GENARG! %EXTRA% || goto :error
cmake --build "%BUILD_DIR%" --config %CONFIG% --parallel || goto :error

rem ---- prove the bundle is where a host will look ---------------------------
rem A MODULE target uses RUNTIME_OUTPUT_DIRECTORY under the MSVC generators and
rem LIBRARY_OUTPUT_DIRECTORY under Ninja, so "it built" is not the same as "the
rem .ofx is in Contents\Win64". That distinction is silent otherwise: a bundle in
rem Contents\Linux-x86-64 builds happily and no host ever scans it.
set "PLUGIN_OFX=%BUILD_DIR%\bundle\VulkanGain.ofx.bundle\Contents\Win64\VulkanGain.ofx"
echo.
echo Bundle: %BUILD_DIR%\bundle\VulkanGain.ofx.bundle
if not exist "%PLUGIN_OFX%" (
  echo.
  echo BUILD FAILED: no .ofx in Contents\Win64
  echo   expected: %PLUGIN_OFX%
  echo   produced:
  dir /s /b "%BUILD_DIR%\bundle\*.ofx" 2>nul
  echo   See the output-directory comment in CMakeLists.txt.
  exit /b 1
)
echo   %PLUGIN_OFX%
echo   CPU only: this plug-in has no CUDA path.
rem The nm / readelf proof from build.sh, in the tools Windows has: exports and
rem load-time imports, checked rather than assumed. --strict matters here: the
rem Visual Studio generator branch never runs vcvars64, so dumpbin is not on PATH.
rem The checker resolves it through vswhere, and must fail rather than skip if it
rem cannot - a build that never looked at its artifact is not a passing build.
call "%HERE%..\..\testkit\check-ofx-artifact.bat" "%PLUGIN_OFX%" --strict || goto :error
exit /b 0

:error
echo.
echo BUILD FAILED (see the cmake output above)
exit /b 1
