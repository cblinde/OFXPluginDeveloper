@echo off
rem ---------------------------------------------------------------------------
rem Out-of-source build for the minimal CUDA OFX template (Windows / MSVC).
rem
rem   .\build.bat                  Release, CUDA if a toolkit is found
rem   .\build.bat Debug            Debug
rem   .\build.bat Release install  build, then copy the bundle into
rem
rem The leading .\ matters when NoDefaultCurrentDirectoryInExePath=1 is set (a
rem common policy): a bare `build.bat` is then not found, by cmd.exe itself.
rem                                %%COMMONPROGRAMFILES%%\OFX\Plugins
rem
rem Environment overrides:
rem   CUDA_HOME        toolkit to use; else discovered via CUDA_PATH / PATH
rem   OFX_SOURCE_DIR   point at an existing openfx checkout (offline builds,
rem                    or Support\Developer\OpenFX shipped with Resolve)
rem   CUDA_ARCHS       e.g. 86;89  (default: 75;80;86;89;90[;100;120])
rem   CPU_ONLY=1       build without CUDA to prove the fallback path
rem   CMAKE_GENERATOR  force a generator (default: newest VS with the C++ toolset,
rem                    or Ninja when that VS has no NVIDIA CUDA customisations)
rem
rem There is no no-root bootstrap here on purpose: on Windows the CUDA toolkit
rem is an installer, not an unpackable package. Install it from
rem https://developer.nvidia.com/cuda-downloads and re-run; unlike Linux there
rem is no reliable ar(1)+tar(1) equivalent that avoids a system install.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion

set "HERE=%~dp0"
set "BUILD_DIR=%HERE%build"
if "%~1"=="" (set "CONFIG=Release") else (set "CONFIG=%~1")

rem ---- CUDA discovery -------------------------------------------------------
set "CUDA_FOUND="
if defined CUDA_HOME if exist "%CUDA_HOME%\bin\nvcc.exe" set "CUDA_FOUND=%CUDA_HOME%"
if not defined CUDA_FOUND if defined CUDA_PATH if exist "%CUDA_PATH%\bin\nvcc.exe" set "CUDA_FOUND=%CUDA_PATH%"
if not defined CUDA_FOUND (
  for /f "delims=" %%I in ('where nvcc 2^>nul') do (
    if not defined CUDA_FOUND (
      rem %%~dpI is "<toolkit>\bin\"; appending ".." leaves the literal path
      rem "<toolkit>\bin\..", which CMake reports as an extra argument and which
      rem breaks the quoted -D values below. %%~fJ canonicalises it.
      for %%J in ("%%~dpI..") do set "CUDA_FOUND=%%~fJ"
    )
  )
)
if not defined CUDA_FOUND if exist "%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v12.9\bin\nvcc.exe" set "CUDA_FOUND=%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v12.9"

if "%CPU_ONLY%"=="1" (
  echo [build] CPU_ONLY=1: building without the CUDA path
  set "CUDA_FOUND="
)

if not defined CUDA_FOUND if not "%CPU_ONLY%"=="1" (
  echo.
  echo error: no CUDA toolkit found, so the CUDA render path cannot be compiled.
  echo.
  echo   Install the NVIDIA CUDA Toolkit ^(12.9 verified^) from
  echo     https://developer.nvidia.com/cuda-downloads
  echo   then re-run this script, or point it at an existing install:
  echo.
  echo     set CUDA_HOME=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9
  echo     build.bat Release
  echo.
  echo   Or build only the CPU path, on purpose:
  echo.
  echo     set CPU_ONLY=1 ^&^& build.bat Release
  echo.
  echo   Searched: %%CUDA_HOME%%, %%CUDA_PATH%%, PATH, %%ProgramFiles%%\NVIDIA GPU Computing Toolkit\CUDA\v12.9
  echo.
  exit /b 2
)

if defined CUDA_FOUND echo [build] CUDA toolkit: %CUDA_FOUND%

rem ---- generator selection ---------------------------------------------------
rem Naming one generator here is wrong on most machines: "Visual Studio 17 2022"
rem does not exist on a box whose only Build Tools are 2019 or 2026, and the failure
rem ("could not find any instance of Visual Studio") says nothing about the fix.
rem Ask vswhere for the newest instance that actually has the C++ toolset, and map
rem its major version to the generator name CMake uses. CMAKE_GENERATOR wins if set,
rem which is also how you override it by hand.
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

rem ---- configure + build ----------------------------------------------------
rem Every -D value is quoted: the CUDA toolkit lives under "Program Files", and an
rem unquoted %CUDA_FOUND% is split at the first space, which CMake then reports as
rem "Ignoring extra path from command line".
set "EXTRA="
if defined CUDA_FOUND set "EXTRA=%EXTRA% "-DCMAKE_CUDA_COMPILER=%CUDA_FOUND%\bin\nvcc.exe" "-DCUDAToolkit_ROOT=%CUDA_FOUND%" -DOFXPLUGIN_ENABLE_CUDA=ON"
if not defined CUDA_FOUND set "EXTRA=%EXTRA% -DOFXPLUGIN_ENABLE_CUDA=OFF"
if defined OFX_SOURCE_DIR set "EXTRA=%EXTRA% "-DOFX_SOURCE_DIR=%OFX_SOURCE_DIR%""
if defined CUDA_ARCHS set "EXTRA=%EXTRA% -DCMAKE_CUDA_ARCHITECTURES=%CUDA_ARCHS%"

rem ---- pick a toolchain that can actually compile a .cu ----------------------
rem A Visual Studio generator compiles CUDA only through NVIDIA's MSBuild build
rem customizations for that exact VS version. CUDA 13.0 ships none for VS 2026, and
rem the failure CMake reports is
rem   CMake Error: No CUDA toolset found.
rem which says nothing about the cause. Ninja + vcvars64 has no such coupling, so
rem fall back to it when the customizations are missing. While we are here, pin the
rem MSVC toolset: nvcc 13 refuses _MSC_VER 1950 (VS 2026) outright in host_config.h,
rem so "newest toolset" is the wrong default for a CUDA build.
set "VSROOT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "!VSWHERE!" (
  for /f "usebackq delims=" %%P in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%P"
)

set "VS_CUDA_CUSTOMISATIONS="
if defined VSROOT if exist "!VSROOT!\MSBuild\Microsoft\VC" (
  for /d %%D in ("!VSROOT!\MSBuild\Microsoft\VC\*") do (
    if exist "%%~D\BuildCustomizations\CUDA *.props" set "VS_CUDA_CUSTOMISATIONS=1"
  )
)

set "USE_NINJA="
if defined CUDA_FOUND if not defined VS_CUDA_CUSTOMISATIONS if not defined CMAKE_GENERATOR set "USE_NINJA=1"
if defined USE_NINJA set "GEN=Ninja"

rem ---- reuse the generator a previous run chose -------------------------
rem CMake refuses to reuse a build directory configured with a different
rem generator ("Does not match the generator used previously: Ninja"), and this
rem script can legitimately pick different ones on different runs - Ninja for a
rem CUDA build, the Visual Studio generator for a CPU-only one. Read the cache
rem AFTER the choice above, so it cannot be overwritten by it.
set "CACHED_GEN="
if exist "%BUILD_DIR%\CMakeCache.txt" (
  for /f "usebackq tokens=1,* delims==" %%A in (`findstr /B /C:"CMAKE_GENERATOR:INTERNAL" "%BUILD_DIR%\CMakeCache.txt"`) do set "CACHED_GEN=%%B"
)
if defined CACHED_GEN if not "!CACHED_GEN!"=="!GEN!" (
  echo [build] reusing generator from build\CMakeCache.txt: !CACHED_GEN!
  set "GEN=!CACHED_GEN!"
)

set "GENARG="
if /I "!GEN!"=="Ninja" (
  echo [build] generator: Ninja ^(no NVIDIA build customisations for the installed Visual Studio^)
  set "VCVARS="
  if defined VSROOT if exist "!VSROOT!\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=!VSROOT!\VC\Auxiliary\Build\vcvars64.bat"
  if not defined VCVARS (
    echo error: Ninja needs cl.exe on PATH and no vcvars64.bat was found.
    echo        Run this from a "x64 Native Tools Command Prompt for VS".
    exit /b 2
  )
  rem 14.44 is the newest MSVC that CUDA 13 accepts; fall back if it is absent.
  call "!VCVARS!" -vcvars_ver=14.44 >nul 2>&1 || call "!VCVARS!" >nul 2>&1
  cl 2>&1 | findstr /C:"Version"
  where ninja >nul 2>&1 || (
    echo error: ninja is not on PATH; install it ^(pip install ninja^) or set CMAKE_GENERATOR.
    exit /b 2
  )
  set "GENARG=-G Ninja -DCMAKE_BUILD_TYPE=!CONFIG!"
) else (
  if defined GEN (
    if defined CUDA_FOUND if not defined VS_CUDA_CUSTOMISATIONS (
      echo error: build\CMakeCache.txt is configured with "!GEN!", which has no NVIDIA
      echo        CUDA build customisations, so it cannot compile Kernel.cu.
      echo        Delete "%BUILD_DIR%" and re-run; the script will then pick Ninja.
      exit /b 2
    )
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

if /I "%~2"=="install" (
  cmake --build "%BUILD_DIR%" --config %CONFIG% --target install-bundle || goto :error
)

echo.
echo Bundle: %BUILD_DIR%\bundle\MinimalCudaGain.ofx.bundle
dir /b "%BUILD_DIR%\bundle\MinimalCudaGain.ofx.bundle\Contents\Win64" 2>nul
if defined CUDA_FOUND (
  echo   CUDA path: compiled in ^(device code is in the image; see the imports below^)
) else (
  echo   CUDA path: not compiled ^(CPU-only build^)
)

rem The Windows equivalent of the nm / readelf / dlopen checks build.sh runs on
rem Linux. Reports what the binary really exports and imports, not what it was
rem meant to: CUDA 13 on Windows does not produce a cudart64_*.dll import even
rem when device code is present, so the message above must not claim one.
set "PLUGIN_OFX=%BUILD_DIR%\bundle\MinimalCudaGain.ofx.bundle\Contents\Win64\MinimalCudaGain.ofx"
if not exist "%PLUGIN_OFX%" (
  echo.
  echo BUILD FAILED: no .ofx in Contents\Win64
  echo   expected: %PLUGIN_OFX%
  echo   produced:
  dir /s /b "%BUILD_DIR%\bundle\*.ofx" 2>nul
  echo   See the output-directory block in CMakeLists.txt.
  exit /b 1
)
rem --strict: the Visual Studio generator branch never runs vcvars64, so neither
rem dumpbin nor cuobjdump is on PATH. Both are resolved by the checker; if that
rem fails, the build fails - an artifact nobody inspected is not a pass.
call "%HERE%..\..\testkit\check-ofx-artifact.bat" "%PLUGIN_OFX%" --cuda --strict || goto :error
exit /b 0

:error
echo.
echo BUILD FAILED (see the cmake output above)
exit /b 1
