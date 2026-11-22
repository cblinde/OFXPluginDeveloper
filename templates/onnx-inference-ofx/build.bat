@echo off
rem ---------------------------------------------------------------------------
rem Out-of-source build for the ONNX inference plug-in (Windows / MSVC).
rem
rem   set ONNXRUNTIME_ROOT=C:\onnxruntime-win-x64-1.20.1
rem   .\build.bat                  Release
rem   .\build.bat Debug            another configuration
rem   .\build.bat Release install  build, then copy the bundle into
rem                                %COMMONPROGRAMFILES%\OFX\Plugins
rem
rem The leading .\ matters when NoDefaultCurrentDirectoryInExePath=1 is set (a
rem common policy): a bare `build.bat` is then not found, by cmd.exe itself.
rem
rem Environment:
rem   ONNXRUNTIME_ROOT  required. The Windows release zip (onnxruntime-win-x64-*)
rem                     holds include\onnxruntime_cxx_api.h, lib\onnxruntime.lib
rem                     and bin\onnxruntime.dll. The DLL is copied into the
rem                     bundle as onnxruntime-ofx.dll and loaded by that name;
rem                     the .lib is NOT linked on Windows. No release
rem                     zip at hand? The NuGet package carries the same files:
rem                       https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/<version>
rem                     (a .nupkg is a zip) with build\native\include,
rem                     runtimes\win-x64\native\*.lib and ...\*.dll.
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
  echo   copy build\native\include\* to include\, runtimes\win-x64\native\*.lib
  echo   to lib\ and runtimes\win-x64\native\*.dll to bin\.
  exit /b 2
)
if not exist "%ONNXRUNTIME_ROOT%\include\onnxruntime_cxx_api.h" (
  echo error: %ONNXRUNTIME_ROOT%\include\onnxruntime_cxx_api.h not found.
  echo   Point ONNXRUNTIME_ROOT at the extracted Windows release zip.
  exit /b 2
)
set "ORT_LIB="
if exist "%ONNXRUNTIME_ROOT%\lib\onnxruntime.lib" set "ORT_LIB=%ONNXRUNTIME_ROOT%\lib\onnxruntime.lib"
if not defined ORT_LIB (
  echo error: no lib\onnxruntime.lib under %ONNXRUNTIME_ROOT%.
  echo   The pip wheel ships only the DLL and no headers; use the release zip,
  echo   or the NuGet package's runtimes\win-x64\native\*.lib.
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
echo.
echo Exports and load-time imports:
rem --strict: the Visual Studio generator branch never runs vcvars64, so dumpbin
rem is not on PATH; the checker finds it through vswhere, and a build that cannot
rem inspect its artifact fails instead of printing a skip.
call "%HERE%..\..\testkit\check-ofx-artifact.bat" "%PLUGIN_OFX%" --strict || goto :error
exit /b 0

:error
echo.
echo BUILD FAILED (see the output above)
exit /b 1
