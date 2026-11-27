@echo off
rem SPDX-License-Identifier: BSD-3-Clause
rem ---------------------------------------------------------------------------
rem Windows counterpart of test\run-tests.sh: the same six steps, for the ONNX
rem inference template.
rem
rem   set ONNXRUNTIME_ROOT=C:\onnxruntime-win-x64-1.32.0
rem                     the suite's runtime; for a bundle that must load in
rem                     Resolve 21.2, build against 1.20.1 instead (cuBLAS 12)
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
rem   4. the model a bundle ships: a copy of the bundle with the fixture written
rem      to Contents\Resources\model.onnx, ONNX_MODEL_PATH cleared, same golden
rem   5. host contract with the plug-in's own tiling (tileSize=16), tolerance
rem   6. artifact and fixture presence, then check-ofx-artifact.bat --strict
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

rem Does this root ship the CUDA provider? If it does, a session that ends on the
rem CPU is a broken bundle, not a passing run - see :checkgpulabel. A CPU package
rem has no onnxruntime_providers_cuda.dll, so the question answers itself there.
set "GPU_PROVIDER="
if exist "%ONNXRUNTIME_ROOT%\bin\onnxruntime_providers_cuda.dll" set "GPU_PROVIDER=%ONNXRUNTIME_ROOT%\bin\onnxruntime_providers_cuda.dll"
if not defined GPU_PROVIDER if exist "%ONNXRUNTIME_ROOT%\lib\onnxruntime_providers_cuda.dll" set "GPU_PROVIDER=%ONNXRUNTIME_ROOT%\lib\onnxruntime_providers_cuda.dll"

echo == [1/6] build (%CONFIG%) ==
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
echo == [2/6] inference pipeline (ai_pipeline_test) ==
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
echo == [3/6] host contract (LoadLibraryExW + fake host), exact golden ==
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
rem The model is not a parameter any more: it arrives through ONNX_MODEL_PATH, read
rem once at instance creation. So identity and pixels are two runs - one with no
rem model in the environment, which must report identity, and one with it set,
rem which must render.
set "MODEL=%ROOT%\models\test_conv.onnx"
set "IDENTITY_ARGS=--id com.ofxplugindeveloper.onnxinference"
set "IDENTITY_ARGS=%IDENTITY_ARGS% --param tileSize:int --param halo:int"
set "IDENTITY_ARGS=%IDENTITY_ARGS% --clip Source --clip Output --identity --size 64x48"
set "ARGS=--id com.ofxplugindeveloper.onnxinference"
set "ARGS=%ARGS% --param tileSize:int --param halo:int"
set "ARGS=%ARGS% --clip Source --clip Output --size 64x48"
set "ARGS=%ARGS% --src "%ROOT%\models\test_input.bin" --golden "%ROOT%\models\test_golden.bin""
set "ARGS=%ARGS% --set halo=2"
set "ARGS=%ARGS% --roi-expand 2 --roi-context --label-param provider"
set "ARGS=%ARGS% --negative-pitch --byte-dst-rejected"
set "ARGS=%ARGS% --src-bounds-inset 2 --dst-bounds-inset 2"
rem The tolerance belongs to the reference, not to this script: --fuzz NAME reads
rem models	est_golden.<NAME>.fuzz, so the CPU reference keeps its exact demand
rem and only a reference a CUDA session cannot reproduce bit for bit says 1e-05.
rem The file itself resolves to test_golden-windows.bin first, then the generic
rem test_golden.bin, and the harness prints which one it read.
set "FUZZ=cpu"
if defined GPU_PROVIDER set "FUZZ=cuda"

rem One process holds one host (OfxSetHost ignores every call after the first),
rem so each shape is a run of its own: the dumps in openfx-misc's README-hosts.txt
rem (nuke: tiles + multi-resolution + temporal access; resolve: none of them) and
rem the background renderer NatronRenderer presents (natron: tiles, isBackground=1).
rem SUITE_THREADS=random picks a different concurrent-render count per run, the
rem way FATE's threads=randomN does - a race has to show up in some run.
set "THREADS=%SUITE_THREADS%"
if not defined THREADS set "THREADS=4"
if /i "%THREADS%"=="random" set /a THREADS=%RANDOM% %% 4 + 1
echo   host profiles: nuke, resolve, natron (reference fuzz: %FUZZ%, %THREADS% render threads)
for %%P in (nuke resolve natron) do (
  echo   host profile: %%P
  rem No model in the environment: this host must be told identity.
  set "ONNX_MODEL_PATH="
  "%HARNESS%" "%PLUGIN%" %IDENTITY_ARGS% --host-profile %%P > "%LOG%-identity" 2>&1
  if errorlevel 1 (
    type "%LOG%-identity"
    goto :contractfailure
  )
  set "ONNX_MODEL_PATH=%MODEL%"
  "%HARNESS%" "%PLUGIN%" %ARGS% --host-profile %%P --fuzz %FUZZ% --concurrency-threads %THREADS% > "%LOG%" 2>&1
  if errorlevel 1 (
    type "%LOG%"
    goto :contractfailure
  )
  type "%LOG%"
  rem findstr rather than a PowerShell one-liner: the pattern a provider label
  rem needs is a word and a phrase, and a /C: search cannot be broken by the
  rem quotes and parentheses that a regex written in -Command gets mangled on.
  findstr /C:"label parameter 'provider' = " "%LOG%" >nul
  if errorlevel 1 (
    echo   FAIL: the provider label does not name an execution provider [%%P profile]
    goto :contractfailure
  )
  rem With a CUDA root the label has to say which arithmetic ran: a device
  rem session without "tf32 off" has numbers nobody has pinned down.
  if defined GPU_PROVIDER findstr /C:"tf32 off" "%LOG%" >nul
  if errorlevel 1 (
    echo   FAIL: a CUDA label without tf32 off - the arithmetic is not pinned down
    goto :contractfailure
  )
  call :checkgpulabel "%%P profile"
  if errorlevel 1 goto :contractfailure
)

rem ---- the half of the provider label that a one-node Conv cannot produce ----
rem "all nodes on CUDA" is what a model the provider covers produces. "nodes left
rem on CPU" needs a node the CUDA EP does not implement, and models\cpu_node.onnx
rem is Hardmax for that reason - no CUDA kernel in onnxruntime 1.20.1. With a CPU
rem package the label is CPU and the question does not arise; that prints NOT
rem VERIFIED instead of a pass, because a skip nobody sees is a claim nobody
rem checked. The render checks are the ones the profile loop above already
rem passed, so this run asserts the label and nothing else.
set "ONNX_MODEL_PATH=%ROOT%\models\cpu_node.onnx"
set "CPUNODE=%LOG%-cpunode"
rem GPU_PROVIDER is the same answer the rest of this script uses: the runtime
rem root ships a provider. Reading the bundle instead would trust a file a
rem previous configure may have left there, which is what the build's stale-file
rem removal exists to prevent.
if defined GPU_PROVIDER (
  echo   cpu-node label: models\cpu_node.onnx - Hardmax, which the CUDA EP does not implement
  rem The two phrases are asserted by the harness, not by a findstr over its
  rem output: one implementation instead of one per platform, and a failure that
  rem prints the label it read. --label-contains is repeatable and needs
  rem --label-param, which this line already passes.
  "%HARNESS%" "%PLUGIN%" --id com.ofxplugindeveloper.onnxinference --param tileSize:int --param halo:int --clip Source --clip Output --size 64x48 --label-param provider --label-contains "provider=nodes left on CPU" --label-contains "provider=fallback: " --host-profile resolve > "%CPUNODE%" 2>&1
  if errorlevel 1 (
    type "%CPUNODE%"
    goto :contractfailure
  )
  type "%CPUNODE%"
) else (
  echo   NOT VERIFIED: no CUDA provider beside the .ofx, so the CPU-node half of
  echo     the label cannot be produced here - point ONNXRUNTIME_ROOT at the CUDA
  echo     package to run this step.
)


echo.
echo.
echo == [4/6] the model the bundle ships (Contents\Resources, no ONNX_MODEL_PATH) ==
rem The branch a user relies on, and the one no run above reaches: those set the
rem variable or clear it, never ship a model. A packaging step puts the .onnx in
rem Contents\Resources beside Win64, and the plug-in resolves it relative to the
rem module it was loaded from - the part a copied or symlinked install breaks. So
rem this copies the built bundle into the temp directory, writes the fixture where
rem the packager would, clears the variable and compares the same golden.
rem Non-identity, because a model that loads leaves nothing to short-circuit: if
rem the copy is not found, isIdentity() answers true and the frame is passed
rem through, which --non-identity fails.
set "SHIPPED=%TEMP%\onnx-shipped-%RANDOM%%RANDOM%"
xcopy /E /I /Q /Y "%BUILD%\bundle\OnnxInference.ofx.bundle" "%SHIPPED%\OnnxInference.ofx.bundle\" >nul || goto :error
copy /Y "%MODEL%" "%SHIPPED%\OnnxInference.ofx.bundle\Contents\Resources\model.onnx" >nul || goto :error
set "SHIPPED_OFX=%SHIPPED%\OnnxInference.ofx.bundle\Contents\Win64\OnnxInference.ofx"
if not exist "%SHIPPED_OFX%" (
  echo   FAIL: the copied bundle has no Contents\Win64\OnnxInference.ofx
  goto :error
)
echo   %SHIPPED_OFX%
set "ONNX_MODEL_PATH="
"%HARNESS%" "%SHIPPED_OFX%" %ARGS% --non-identity --fuzz %FUZZ% --host-profile resolve > "%LOG%" 2>&1
if errorlevel 1 (
  type "%LOG%"
  goto :contractfailure
)
type "%LOG%"
findstr /C:"label parameter 'provider' = " "%LOG%" >nul
if errorlevel 1 (
  echo   FAIL: a bundle that ships its model still has no execution provider
  goto :contractfailure
)
call :checkgpulabel "shipped model"
if errorlevel 1 goto :contractfailure
rmdir /S /Q "%SHIPPED%"

echo == [5/6] host contract with the plug-in's own tiling (tileSize=16) ==
echo   tolerance 0.02: feathered overlap is exact only at halo ^>= radius
set "ONNX_MODEL_PATH=%MODEL%"
"%HARNESS%" "%PLUGIN%" ^
  --id com.ofxplugindeveloper.onnxinference ^
  --param tileSize:int --param halo:int ^
  --clip Source --clip Output --size 64x48 ^
  --set halo=2 --set tileSize=16 ^
  --set-status-only --concurrency-threads 0 --roi-context-tolerance 0.02 || goto :error

echo.
echo == [6/6] fixtures and artifact ==
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

:checkgpulabel
rem %1 names the contract run, for the message. Called from the two places that
rem accept "CPU (1 thread)" as a valid label: with a CPU package that is the
rem expected answer, but with a GPU root it means the provider did not load - and
rem a wiped build directory produces exactly that (no provider in the bundle,
rem session falls back, every pixel check still passes). The suite has to fail
rem there instead of going green on the CPU path.
if not defined GPU_PROVIDER exit /b 0
powershell -NoProfile -ExecutionPolicy Bypass -Command "if (Select-String -Path '%LOG%' -Pattern 'label parameter .provider. = .CPU' -Quiet) { exit 1 }"
if not errorlevel 1 exit /b 0
echo.
echo   FAIL: ONNXRUNTIME_ROOT ships onnxruntime_providers_cuda.dll, so the session
powershell -NoProfile -Command "Select-String -Path '%LOG%' -Pattern 'label parameter .provider. = .' | ForEach-Object { '   ' + $_.Line.Trim() }"
echo   had to run on the device - and it fell back to the CPU (contract run: %1).
echo   The label above ends in "- fallback: <reason>": that is the load error.
echo   Either the bundle has no onnxruntime_providers_cuda.dll beside the .ofx, or
echo   the CUDA imports (cuDNN 9, cuBLAS/cuBLASLt, cuFFT, cudart) are missing.
rem build.bat forwards both switches from the environment, so:
echo   set OFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON
rem and, when those DLLs are not under ONNXRUNTIME_ROOT\bin:
echo   set OFXPLUGIN_CUDA_RUNTIME_DIRS=^<dir with cuDNN, cuBLAS, cuFFT, cudart^>
rem exit /b, not goto: inside a called subroutine a goto would jump and the
rem script would carry on after the call with the failure already printed.
exit /b 1

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
