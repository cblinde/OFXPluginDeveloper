@echo off
rem ---------------------------------------------------------------------------
rem Out-of-source build for the OpenCL reference plug-in (Windows / MSVC).
rem
rem   .\build.bat                  Release
rem   .\build.bat Debug            another configuration
rem
rem The leading .\ matters when NoDefaultCurrentDirectoryInExePath=1 is set (a
rem common policy): a bare `build.bat` is then not found, by cmd.exe itself.
rem
rem Environment overrides:
rem   OFX_SOURCE_DIR     point at an existing openfx checkout (offline builds, or
rem                      the Support\Developer\OpenFX copy shipped with Resolve);
rem                      when unset, CMake fetches OFX_Release_1.5.1
rem   CMAKE_GENERATOR    force a generator (default: the newest Visual Studio that
rem                      has the C++ toolset)
rem   OPENCL_GAIN_DEFAULT  the gain the plug-in starts at (1.0 = passthrough)
rem
rem This is the Windows counterpart of build.sh and stops where that script stops:
rem at the bundle. Its nm / readelf proof has a Windows equivalent (dumpbin), and
rem testkit\check-ofx-artifact.bat --strict is that equivalent.
rem
rem No OpenCL SDK and no OpenCL import library are needed: src\ClRuntime.cpp
rem resolves the entry points with LoadLibraryA("OpenCL.dll") + GetProcAddress, so
rem the build links nothing from the vendor and works on a machine that only has
rem the driver. That is the same reason the Linux build links no libOpenCL.
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
if defined OPENCL_GAIN_DEFAULT set "EXTRA=%EXTRA% "-DOPENCL_GAIN_DEFAULT=%OPENCL_GAIN_DEFAULT%""

set "GENARG="
if /I "!GEN!"=="Ninja" (
  echo [build] generator: Ninja
  rem A single-config generator needs CMAKE_BUILD_TYPE, and it needs cl.exe on
  rem PATH, which a plain cmd does not have.
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
rem .ofx is in Contents\Win64".
set "PLUGIN_OFX=%BUILD_DIR%\bundle\OpenCLGain.ofx.bundle\Contents\Win64\OpenCLGain.ofx"
echo.
echo Bundle: %BUILD_DIR%\bundle\OpenCLGain.ofx.bundle
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
echo   OpenCL comes from the driver at run time: no SDK, no import library.
rem The nm / readelf proof from build.sh, in the tool Windows has. --strict
rem matters: the Visual Studio generator branch never runs vcvars64, so dumpbin is
rem not on PATH, and a build that never looked at its artifact is not a passing
rem build.
call "%HERE%..\..\testkit\check-ofx-artifact.bat" "%PLUGIN_OFX%" --strict || goto :error
exit /b 0

:error
echo.
echo BUILD FAILED (see the cmake output above)
exit /b 1
