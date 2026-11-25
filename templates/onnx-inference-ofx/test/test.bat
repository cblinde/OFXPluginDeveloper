@echo off
rem SPDX-License-Identifier: BSD-3-Clause
rem ---------------------------------------------------------------------------
rem Windows counterpart of test\run-tests.sh: the same five steps, in the same
rem order, for the ONNX inference template.
rem
rem   set ONNXRUNTIME_ROOT=C:\onnxruntime-win-x64-1.32.0
rem   test\test.bat                  Release
rem   test\test.bat Debug            another configuration
rem   test\test.bat Release nobuild  reuse build\ instead of calling build.bat
rem
rem Environment:
rem   ONNXRUNTIME_ROOT  required. Either the release zip layout
rem                     (include\onnxruntime_cxx_api.h, bin\onnxruntime.dll) or a
rem                     raw NuGet extraction of Microsoft.ML.OnnxRuntime, which
rem                     references/field-evidence.md records as what the Windows
rem                     sessions used: build\native\include plus
rem                     runtimes\win-x64\native\onnxruntime.dll. The NuGet layout
rem                     is normalised into build\_ort once, because build.bat and
rem                     the tests both expect the release layout.
rem   OFX_SOURCE_DIR    used when build\CMakeCache.txt has no OFX_SOURCE_DIR.
rem                     build-contract-test.bat needs the include directory.
rem
rem Steps, matching run-tests.sh:
rem   1. build (build.bat, which also runs check-ofx-artifact.bat --strict)
rem   2. ai_pipeline_test models  - packing, whole-frame and tiled inference
rem   3. host contract, exact golden, through the OFX API
rem   4. host contract with the plug-in's own tiling (tileSize=16), tolerance
rem   5. artifact and fixture presence, then check-ofx-artifact.bat --strict
rem
rem What test.bat cannot do that run-tests.sh can: step 5's nm/readelf counts are
rem dumpbin's job here (the checker does them), and there is no valgrind.
rem
rem Verified: this script runs green in this repository on Windows 11, against
rem ONNX Runtime 1.20.1 and 1.32.0, on the CPU runtime and on the CUDA provider;
rem the logs are under evidence/ in the repository root. A failure here is still
rem as likely to be a bug in this script as in the plug-in, so read a green run
rem as evidence and the log as the record. references/field-evidence.md section 7
rem and section 9 list what the Windows sessions actually ran.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion

set "HERE=%~dp0"
for %%I in ("%HERE%..") do set "ROOT=%%~fI"
set "BUILD=%ROOT%\build"
set "CONFIG=Release"
if not "%~1"=="" set "CONFIG=%~1"
set "NOBUILD=0"
if /I "%~2"=="nobuild" set "NOBUILD=1"

if /I not "%CONFIG%"=="Release" if /I not "%CONFIG%"=="Debug" if /I not "%CONFIG%"=="RelWithDebInfo" if /I not "%CONFIG%"=="MinSizeRel" (
  echo error: unknown configuration "%CONFIG%"
  echo        use Release, Debug, RelWithDebInfo or MinSizeRel
  exit /b 2
)

rem ---- the one dependency that is not optional -------------------------------
if not defined ONNXRUNTIME_ROOT (
  echo error: ONNXRUNTIME_ROOT is not set.
  echo   Release zip:  set ONNXRUNTIME_ROOT=C:\path\to\onnxruntime-win-x64-^<version^>
  echo   NuGet:        extract Microsoft.ML.OnnxRuntime^<version^>.nupkg ^(a zip^) and
  echo                 point ONNXRUNTIME_ROOT at it; this script normalises it.
  exit /b 2
)

if not exist "%ONNXRUNTIME_ROOT%\include\onnxruntime_cxx_api.h" if exist "%ONNXRUNTIME_ROOT%\build\native\include\onnxruntime_cxx_api.h" (
  echo [test] NuGet layout under %ONNXRUNTIME_ROOT%; normalising into %BUILD%\_ort
  if not exist "%BUILD%\_ort\include" mkdir "%BUILD%\_ort\include"
  if not exist "%BUILD%\_ort\bin" mkdir "%BUILD%\_ort\bin"
  xcopy /E /I /Y /Q "%ONNXRUNTIME_ROOT%\build\native\include\*" "%BUILD%\_ort\include\" >nul || goto :error
  copy /Y "%ONNXRUNTIME_ROOT%\runtimes\win-x64\native\onnxruntime.dll" "%BUILD%\_ort\bin\" >nul || goto :error
  set "ONNXRUNTIME_ROOT=%BUILD%\_ort"
)

if not exist "%ONNXRUNTIME_ROOT%\include\onnxruntime_cxx_api.h" (
  echo error: %ONNXRUNTIME_ROOT%\include\onnxruntime_cxx_api.h not found.
  echo   Point ONNXRUNTIME_ROOT at the extracted release zip or the extracted
  echo   Microsoft.ML.OnnxRuntime package.
  exit /b 2
)
if not exist "%ONNXRUNTIME_ROOT%\bin\onnxruntime.dll" (
  echo error: no bin\onnxruntime.dll under %ONNXRUNTIME_ROOT%.
  echo   The tests load the runtime from there; the bundle gets its own copy as
  echo   onnxruntime-ofx.dll, so this DLL is only needed to run the tests.
  exit /b 2
)
rem ai_pipeline_test.exe and the contract harness load onnxruntime.dll by name.
set "PATH=%ONNXRUNTIME_ROOT%\bin;%PATH%"

echo == [1/5] build (%CONFIG%) ==
if "%NOBUILD%"=="1" (
  if not exist "%BUILD%\CMakeCache.txt" (
    echo error: nobuild was asked for but %BUILD%\CMakeCache.txt does not exist
    exit /b 2
  )
  echo   skipped: using the existing %BUILD%
) else (
  call "%ROOT%\build.bat" %CONFIG% || goto :error
)

echo.
echo == [2/5] inference pipeline (ai_pipeline_test) ==
set "AI=%BUILD%\ai_pipeline_test.exe"
if not exist "%AI%" set "AI=%BUILD%\%CONFIG%\ai_pipeline_test.exe"
if not exist "%AI%" (
  echo error: ai_pipeline_test.exe not found under %BUILD%
  echo   the target is built by build.bat; a Ninja build puts it in build\, a
  echo   Visual Studio build in build\%CONFIG%\
  exit /b 1
)
"%AI%" "%ROOT%\models" || goto :error

echo.
echo == [3/5] host contract (LoadLibraryExW + fake host), exact golden ==
set "OFX_INC="
if exist "%BUILD%\CMakeCache.txt" (
  for /f "usebackq tokens=1,* delims==" %%A in (`findstr /B /C:"OFX_SOURCE_DIR:PATH=" "%BUILD%\CMakeCache.txt"`) do set "OFX_INC=%%B"
)
if not defined OFX_INC if defined OFX_SOURCE_DIR set "OFX_INC=%OFX_SOURCE_DIR%"
if not defined OFX_INC (
  echo error: no OFX_SOURCE_DIR in %BUILD%\CMakeCache.txt
  echo   set OFX_SOURCE_DIR to an openfx checkout ^(the directory with include\ofxCore.h^)
  exit /b 2
)

set "HARNESS=%TEMP%\contract_test_onnx.exe"
call "%ROOT%\..\..\testkit\build-contract-test.bat" "%HARNESS%" "%OFX_INC%" || goto :error

set "PLUGIN=%BUILD%\bundle\OnnxInference.ofx.bundle\Contents\Win64\OnnxInference.ofx"
if not exist "%PLUGIN%" (
  echo error: no .ofx at %PLUGIN%
  echo   see the output-directory block in CMakeLists.txt
  exit /b 1
)
echo   %PLUGIN%

set "LOG=%TEMP%\onnx-contract.log"
set "ARGS=--id com.ofxplugindeveloper.onnxinference"
set "ARGS=%ARGS% --param modelPath:string --param tileSize:int --param halo:int"
set "ARGS=%ARGS% --clip Source --clip Output --identity --size 64x48"
set "ARGS=%ARGS% --src "%ROOT%\models\test_input.bin" --golden "%ROOT%\models\test_golden.bin""
set "ARGS=%ARGS% --set "modelPath=%ROOT%\models\test_conv.onnx" --set halo=2"
set "ARGS=%ARGS% --roi-expand 2 --roi-context --label-param provider"
set "ARGS=%ARGS% --not-identity-after-set --negative-pitch --byte-dst-rejected"
set "ARGS=%ARGS% --src-bounds-inset 2 --dst-bounds-inset 2"
"%HARNESS%" "%PLUGIN%" %ARGS% > "%LOG%" 2>&1
if errorlevel 1 (
  rem The CPU build renders the golden bit-exactly; the CUDA provider sums in
  rem another order, so its output differs in the last bits and an exact
  rem comparison fails for a correct pipeline. The first attempt is still exact
  rem - that is what proves the packing on the CPU - and only then does the
  rem run accept the provider's rounding, as test/run-tests.sh does on POSIX.
  echo   exact comparison failed; retrying with --golden-tolerance 1e-5 for a golden rendered by another provider
  "%HARNESS%" "%PLUGIN%" %ARGS% --golden-tolerance 1e-5 > "%LOG%" 2>&1
  if errorlevel 1 (
    type "%LOG%"
    goto :contractfailure
  )
)
type "%LOG%"

rem The label must name an execution provider. Select-String rather than findstr:
rem the pattern contains a double quote, which cmd cannot pass through /C:"..."
rem without splitting it. CUDA is requested first, CPU is the fallback, both pass.
powershell -NoProfile -ExecutionPolicy Bypass -Command "if (-not (Select-String -Path '%LOG%' -Pattern 'label parameter .provider. = .(CUDA|CPU)' -Quiet)) { exit 1 }"
if errorlevel 1 (
  echo   FAIL: the provider label does not name an execution provider
  goto :contractfailure
)
echo   provider label names CUDA or CPU

rem The same contract against the other host shape: supportsTiles=0 and
rem supportsMultiResolution=0, which is what Resolve's OFX host reports in the
rem dumps published in openfx-misc's README-hosts.txt. Only the two host
rem properties change - every window, RoI and pixel check stays, because a host
rem with supportsTiles=0 still renders a window smaller than the frame when its
rem own region of definition is smaller.
"%HARNESS%" "%PLUGIN%" %ARGS% --host-profile resolve > "%LOG%" 2>&1
if errorlevel 1 (
  echo   exact comparison failed on the resolve-shaped host; retrying with --golden-tolerance 1e-5
  "%HARNESS%" "%PLUGIN%" %ARGS% --host-profile resolve --golden-tolerance 1e-5 > "%LOG%" 2>&1
  if errorlevel 1 (
    type "%LOG%"
    goto :contractfailure
  )
)
echo   host shape resolve: supportsTiles=0, supportsMultiResolution=0
powershell -NoProfile -ExecutionPolicy Bypass -Command "if (-not (Select-String -Path '%LOG%' -Pattern 'label parameter .provider. = .(CUDA|CPU)' -Quiet)) { exit 1 }"
if errorlevel 1 (
  echo   FAIL: the provider label does not name an execution provider under the resolve profile
  goto :contractfailure
)
type "%LOG%"

echo.
echo == [4/5] host contract with the plug-in's own tiling (tileSize=16) ==
echo   tolerance 0.02: feathered overlap is exact only at halo ^>= radius
"%HARNESS%" "%PLUGIN%" ^
  --id com.ofxplugindeveloper.onnxinference ^
  --param modelPath:string --param tileSize:int --param halo:int ^
  --clip Source --clip Output --size 64x48 ^
  --set "modelPath=%ROOT%\models\test_conv.onnx" --set halo=2 --set tileSize=16 ^
  --set-status-only --concurrency-threads 0 --roi-context-tolerance 0.02 || goto :error

echo.
echo == [5/5] fixtures and artifact ==
for %%F in (test_conv.onnx test_input.bin test_golden.bin test_shape.txt) do (
  if not exist "%ROOT%\models\%%F" (
    echo   FAIL: models\%%F is missing
    goto :error
  )
  echo   models\%%F
)
rem build.bat already ran this with --strict; running it again is what proves the
rem artifact on the machine the test is being read on, not on the one that built it.
call "%ROOT%\..\..\testkit\check-ofx-artifact.bat" "%PLUGIN%" --strict || goto :error

del "%LOG%" "%HARNESS%" >nul 2>&1
echo.
echo all checks passed
endlocal
exit /b 0

:contractfailure
del "%LOG%" >nul 2>&1
echo.
echo TESTS FAILED: the host contract did not pass (output above)
endlocal
exit /b 1

:error
echo.
echo TESTS FAILED (see the output above)
endlocal
exit /b 1
