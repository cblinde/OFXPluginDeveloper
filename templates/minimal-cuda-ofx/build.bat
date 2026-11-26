@echo off
rem ---------------------------------------------------------------------------
rem Out-of-source build for the minimal CUDA OFX template (Windows / MSVC).
rem
rem   .\build.bat                  Release, CUDA if a toolkit is found
rem   .\build.bat Debug            Debug
rem   .\build.bat Release install  build, then copy the bundle into
rem                                %COMMONPROGRAMFILES%\OFX\Plugins
rem
rem The leading .\ matters when NoDefaultCurrentDirectoryInExePath=1 is set (a
rem common policy): a bare `build.bat` is then not found, by cmd.exe itself.
rem
rem Environment overrides:
rem   CUDA_HOME        toolkit to use; else discovered via CUDA_PATH / PATH
rem   OFX_SOURCE_DIR   point at an existing openfx checkout (offline builds,
rem                    or Support\Developer\OpenFX shipped with Resolve)
rem   CUDA_ARCHS       e.g. 86;89  (default: 75;80;86;89;90[;100;120])
rem   GAIN_DEFAULT     default gain baked in (default 1.0); set 2.0 so a host
rem                    cannot skip render()
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
rem Newest installed series, compared as numbers. `dir /o-n` sorts by name,
rem where "v9.0" outranks "v13.0" and "v12.10" outranks "v12.9" - measured on a
rem machine holding both v9.0 and v13.0, the name sort picked v9.0 and a CUDA 13
rem build silently became a CUDA 9 one. So every directory that actually holds a
rem bin\nvcc.exe is compared as major.minor and the largest wins; a series that
rem does not exist yet needs no edit here.
if not defined CUDA_FOUND (
  set "CUDA_ROOT=%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA"
  set "BEST_MAJOR=-1"
  set "BEST_MINOR=-1"
  set "BEST_DIR="
  if exist "!CUDA_ROOT!" (
    for /f "delims=" %%D in ('dir /b /ad "!CUDA_ROOT!" 2^>nul') do (
      if exist "!CUDA_ROOT!\%%D\bin\nvcc.exe" (
        for /f "tokens=1,2 delims=v." %%A in ("%%D") do (
          set "MAJ=%%A"
          set "MIN=%%B"
          if not defined MIN set "MIN=0"
          if !MAJ! GTR !BEST_MAJOR! (
            set "BEST_MAJOR=!MAJ!"
            set "BEST_MINOR=!MIN!"
            set "BEST_DIR=%%D"
          ) else (
            if !MAJ! EQU !BEST_MAJOR! if !MIN! GTR !BEST_MINOR! (
              set "BEST_MINOR=!MIN!"
              set "BEST_DIR=%%D"
            )
          )
        )
      )
    )
  )
  if defined BEST_DIR set "CUDA_FOUND=!CUDA_ROOT!\!BEST_DIR!"
)

if "%CPU_ONLY%"=="1" (
  echo [build] CPU_ONLY=1: building without the CUDA path
  set "CUDA_FOUND="
)

if not defined CUDA_FOUND if not "%CPU_ONLY%"=="1" (
  echo.
  echo error: no CUDA toolkit found, so the CUDA render path cannot be compiled.
  echo.
  echo   Install the NVIDIA CUDA Toolkit from
  echo     https://developer.nvidia.com/cuda-downloads
  echo   then re-run this script, or point it at an existing install:
  echo.
  echo     set CUDA_HOME=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0
  echo     build.bat Release
  echo.
  echo   Or build only the CPU path, on purpose:
  echo.
  echo     set CPU_ONLY=1 ^&^& build.bat Release
  echo.
  echo   Searched: %%CUDA_HOME%%, %%CUDA_PATH%%, PATH, %%ProgramFiles%%\NVIDIA GPU Computing Toolkit\CUDA\v*
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
rem Always passed, with 1.0 as the fallback: a cached -DGAIN_DEFAULT=2.0 would
rem otherwise survive every later build, and the contract suite asserts identity at
rem neutral settings - which a build tree configured for a host proof then fails.
if defined GAIN_DEFAULT (set "EXTRA=%EXTRA% -DGAIN_DEFAULT=%GAIN_DEFAULT%") else (set "EXTRA=%EXTRA% -DGAIN_DEFAULT=1.0")

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

rem The second half of the same question is the MSVC toolset: nvcc 13 refuses
rem _MSC_VER 1950 (MSVC 14.50) in host_config.h, and a Visual Studio generator
rem cannot be told to take an older one - CMake hands .cu files to NVIDIA's
rem MSBuild customisations, which use the newest toolset in the instance (`-T
rem version=` is rejected as an invalid specification, measured, and
rem `-DCMAKE_VS_PLATFORM_TOOLSET_VERSION` leaves CMake with no CXX compiler at
rem all). The one place the pin applies is vcvars64 -vcvars_ver, and only the
rem Ninja branch runs it.
set "MSVC_NEWEST="
set "MSVC_PIN="
if defined VSROOT if exist "!VSROOT!\VC\Tools\MSVC" (
  for /f "delims=" %%V in ('dir /b /ad /o-n "!VSROOT!\VC\Tools\MSVC" 2^>nul') do (
    set "CANDIDATE=%%V"
    if not defined MSVC_NEWEST set "MSVC_NEWEST=!CANDIDATE!"
    rem 14.44 is the newest series CUDA 13 accepts. A newer one present beside
    rem it is exactly what the Visual Studio generator would pick on its own.
    if not defined MSVC_PIN if "!CANDIDATE:~0,5!"=="14.44" set "MSVC_PIN=!CANDIDATE!"
  )
)
set "VS_TOOLSET_FINE="
if defined MSVC_NEWEST if "!MSVC_NEWEST:~0,5!"=="14.44" set "VS_TOOLSET_FINE=1"

set "USE_NINJA="
if defined CUDA_FOUND if not defined VS_CUDA_CUSTOMISATIONS if not defined CMAKE_GENERATOR set "USE_NINJA=1"
rem Second reason to pick Ninja: the newest toolset is newer than nvcc accepts,
rem so the generator's default cannot compile the .cu and only vcvars can pin it.
if defined CUDA_FOUND if not defined VS_TOOLSET_FINE if defined MSVC_PIN if not defined CMAKE_GENERATOR set "USE_NINJA=1"
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
  rem Pin the toolset found above - this branch is the only one that can, and an
  rem unpinned CUDA build stops in host_config.h instead of compiling.
  if defined MSVC_PIN (
    call "!VCVARS!" -vcvars_ver=!MSVC_PIN! >nul 2>&1 || call "!VCVARS!" >nul 2>&1
  ) else (
    call "!VCVARS!" >nul 2>&1
    if defined CUDA_FOUND (
      echo warning: no 14.4x MSVC under "!VSROOT!\VC\Tools\MSVC"; nvcc 13 rejects
      echo          the newest toolset there. Install MSVC 14.44 in that instance,
      echo          or point CUDA_HOME at a toolkit that accepts this one.
    )
  )
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
    if defined CUDA_FOUND if defined VS_CUDA_CUSTOMISATIONS if not defined VS_TOOLSET_FINE (
      echo error: "!GEN!" compiles the .cu through NVIDIA's MSBuild customisations,
      echo        which take the newest MSVC in the instance ^(!MSVC_NEWEST!^); CUDA 13
      echo        rejects that in host_config.h and only vcvars64 -vcvars_ver can pin
      echo        it - a generator that never runs vcvars cannot.
      echo        Delete "%BUILD_DIR%" and re-run without CMAKE_GENERATOR, so the
      echo        script picks Ninja and pins !MSVC_PIN!.
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
rem
rem --cuda only when a CUDA path was compiled in. Asking for it unconditionally
rem meant the CPU-only build demanded cubins it cannot have; it passed only
rem because cuobjdump's "does not contain device code" info line was counted as
rem one (fixed in the checker at the same time), i.e. a false pass hidden behind
rem a second bug. Now the CPU_ONLY build asserts the opposite, honestly.
set "CUDA_CHECK="
if defined CUDA_FOUND set "CUDA_CHECK=--cuda"
call "%HERE%..\..\testkit\check-ofx-artifact.bat" "%PLUGIN_OFX%" %CUDA_CHECK% --strict || goto :error
exit /b 0

:error
echo.
echo BUILD FAILED (see the cmake output above)
exit /b 1
