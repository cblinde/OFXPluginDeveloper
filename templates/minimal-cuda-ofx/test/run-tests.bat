@echo off
rem SPDX-License-Identifier: BSD-3-Clause
rem ---------------------------------------------------------------------------
rem Windows counterpart of test/run-tests.sh for this template.
rem
rem   test\run-tests.bat                 artifact check, GPU harness, contract
rem   test\run-tests.bat Release nobuild reuse build\ instead of calling build.bat
rem
rem Linux runs the whole story through g++, nm -D, readelf, dlopen and valgrind.
rem None of those exist on Windows, so this script runs the equivalents that do:
rem
rem   [0/4] build     build.bat, which also verifies the artifact
rem   [1/4] artifact  testkit\check-ofx-artifact.bat --strict  (dumpbin, cuobjdump)
rem   [2/4] kernels   nvcc test\gpu-kernel-test.cu, executed on the real GPU -
rem                   the only check in this repository that runs SASS
rem   [3/4] contract  testkit\build-contract-test.bat + LoadLibraryExW, the same
rem                   checks as dlopen, against the built .ofx
rem   [4/4] modes     execution_mode_test.exe: the four execution-mode gates, no
rem                   GPU, host or kernel needed (src/ExecutionMode.h)
rem
rem What it does NOT do: the CPU-emulated equivalence suite (test\run-tests.sh,
rem which needs g++ and the shim headers) and valgrind. Step 2 covers the same
rem arithmetic on hardware where step 1 of the shell script covers it in
rem emulation; on a machine with a GPU that is the stronger check, not a
rem substitute for the PTX count.
rem
rem Environment:
rem   OFX_SOURCE_DIR  openfx checkout; else read from build\CMakeCache.txt
rem   CUDA_ARCHS      e.g. 86, or 86;89 for several; else the GPU's own
rem                   compute capability, else 75
rem   CPU_ONLY=1      skip the GPU step and do not demand cubins
rem   OFXPLUGIN_BUILD_DIR  run against a tree other than build\
rem ---------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion

set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "HERE=%HERE:~0,-1%"
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

set "CONFIG=Release"
if not "%~1"=="" set "CONFIG=%~1"
set "NOBUILD=0"
if /I "%~2"=="nobuild" set "NOBUILD=1"

rem The same override build.bat honours, so a CPU-only run can use its own tree.
if defined OFXPLUGIN_BUILD_DIR (set "BUILD_DIR=%OFXPLUGIN_BUILD_DIR%") else (set "BUILD_DIR=%ROOT%\build")
set "OFX_FILE=%BUILD_DIR%\bundle\MinimalCudaGain.ofx.bundle\Contents\Win64\MinimalCudaGain.ofx"

rem ---- [0] build -----------------------------------------------------------
if "%NOBUILD%"=="0" (
  echo == [0/4] build ^(%CONFIG%^) ==
  rem A bare `build.bat` is not found when NoDefaultCurrentDirectoryInExePath=1
  rem is set, which is a common policy; the .\ prefix is load-bearing.
  call "%ROOT%\build.bat" %CONFIG% || goto :error
) else (
  echo == [0/4] build skipped ^(nobuild^) ==
)

if not exist "%OFX_FILE%" (
  echo.
  echo error: no .ofx at %OFX_FILE%
  echo        run ".\build.bat %CONFIG%" first, or drop the nobuild argument.
  exit /b 1
)

rem ---- [1] artifact ---------------------------------------------------------
rem Same checker build.bat runs; here it is the point of the step, not a side
rem effect, so a failure names which of the three claims broke.
echo.
echo == [1/4] artifact (exports, dependents, device code) ==
set "CUDA_CHECK="
if not "%CPU_ONLY%"=="1" set "CUDA_CHECK=--cuda"
rem Two levels and not one: this script lives in test\, so ROOT is the template
rem directory and the shared testkit is at <repo>\testkit.
call "%ROOT%\..\..\testkit\check-ofx-artifact.bat" "%OFX_FILE%" %CUDA_CHECK% --strict || goto :error

rem ---- [2] kernels on the GPU ----------------------------------------------
echo.
if "%CPU_ONLY%"=="1" (
  echo == [2/4] GPU harness skipped ^(CPU_ONLY=1^) ==
  goto :contract
)

rem nvcc needs cl.exe on PATH. A plain cmd has neither, so enter the MSVC
rem environment here, pinning the toolset nvcc accepts when it exists.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%P in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%P"
)
if defined VSROOT if exist "!VSROOT!\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=!VSROOT!\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
  echo == [2/4] GPU harness skipped: no vcvars64.bat ^(no MSVC C++ toolset^) ==
  goto :contract
)
rem Same pin build.bat applies: nvcc 13 accepts MSVC up to 14.44, and the newest
rem toolset in a Visual Studio 18 instance is 14.50. Take the 14.4x one when it is
rem there, so the harness compiles the way the template does.
set "MSVC_PIN="
if defined VSROOT if exist "!VSROOT!\VC\Tools\MSVC" (
  for /f "delims=" %%V in ('dir /b /ad /o-n "!VSROOT!\VC\Tools\MSVC" 2^>nul') do (
    set "CANDIDATE=%%V"
    if not defined MSVC_PIN if "!CANDIDATE:~0,5!"=="14.44" set "MSVC_PIN=!CANDIDATE!"
  )
)
if defined MSVC_PIN (
  call "!VCVARS!" -vcvars_ver=!MSVC_PIN! >nul 2>&1 || call "!VCVARS!" >nul 2>&1
) else (
  call "!VCVARS!" >nul 2>&1
)
cl 2>&1 | findstr /C:"Version"

echo == [2/4] GPU harness (real device code, not emulated) ==
set "ARCHS=%CUDA_ARCHS%"
if not defined ARCHS (
  set "CAP="
  for /f "usebackq delims=" %%C in (`nvidia-smi --query-gpu^=compute_cap --format^=csv^,noheader 2^>nul`) do if not defined CAP set "CAP=%%C"
  if defined CAP (
    set "ARCHS=!CAP:.=!"
  ) else (
    set "ARCHS=75"
    echo [test] no GPU reported by nvidia-smi; building for sm_75
  )
)
rem One -gencode per architecture: repeating -arch is a fatal redefinition in
rem nvcc, and CUDA_ARCHS may be the CMake-style list ("86;89"), which used to
rem reach the compiler as "-arch=sm_86;89" and fail to compile at all. Semicolons
rem become separators before the loop so both forms work.
set "ARCHS=!ARCHS:;= !"
set "NVARCH="
for %%A in (!ARCHS!) do set "NVARCH=!NVARCH! -gencode arch=compute_%%A,code=sm_%%A"
if not defined NVARCH (
  echo error: CUDA_ARCHS="%CUDA_ARCHS%" named no architecture
  goto :error
)

rem CUDA 13.0 host_config.h accepts MSVC up to 14.44 only, so the toolset is
rem pinned above rather than worked around: a retry with
rem -allow-unsupported-compiler produced a binary from a compiler the toolkit was
rem never tested against, and hid the fact that the wrong one was chosen. When the
rem pin is unavailable, say which toolset ran and what to install.
nvcc -DOFXPLUGIN_WITH_CUDA=1 !NVARCH! -O2 -std=c++17 "%HERE%\gpu-kernel-test.cu" -o "%BUILD_DIR%\gpu-kernel-test.exe"
if errorlevel 1 (
  echo.
  echo error: nvcc rejected the host compiler ^(see the message above^).
  if defined MSVC_PIN (
    echo        The pin was !MSVC_PIN!; a failure here is not a toolset problem.
  ) else (
    echo        No 14.4x MSVC under "!VSROOT!\VC\Tools\MSVC", so the newest toolset
    echo        ran. Install MSVC 14.44 in that instance - CUDA 13 accepts nothing
    echo        newer - and re-run.
  )
  goto :error
)
"%BUILD_DIR%\gpu-kernel-test.exe" || goto :error

:contract
rem ---- [3] host contract ----------------------------------------------------
echo.
echo == [3/4] host contract (LoadLibraryExW + fake host) ==
set "OFX_INC=%OFX_SOURCE_DIR%"
if not defined OFX_INC if exist "%BUILD_DIR%\CMakeCache.txt" (
  for /f "usebackq tokens=1,* delims==" %%A in (`findstr /B /C:"OFX_SOURCE_DIR:PATH=" "%BUILD_DIR%\CMakeCache.txt"`) do set "OFX_INC=%%B"
)
rem build.sh keeps the headers in <repo>/.ofx-ref; the CMake fetch uses _deps.
if not defined OFX_INC if exist "%ROOT%\..\..\.ofx-ref\include\ofxCore.h" (
  set "OFX_INC=%ROOT%\..\..\.ofx-ref"
)
if not defined OFX_INC (
  for /d %%D in ("%BUILD_DIR%\_deps\openfx-src") do set "OFX_INC=%%~fD"
)
if not exist "%OFX_INC%\include\ofxCore.h" (
  echo error: no openfx include directory; set OFX_SOURCE_DIR
  exit /b 2
)

set "CONTRACT=%BUILD_DIR%\contract_test.exe"
call "%ROOT%\..\..\testkit\build-contract-test.bat" "%CONTRACT%" "%OFX_INC%" || goto :error
rem One process holds one host - OFX ignores every setHost after the first - so
rem each shape is a run of its own: the dumps in openfx-misc's README-hosts.txt
rem (nuke: tiles + multi-resolution + temporal access; resolve: none of them) and
rem the background renderer NatronRenderer presents (natron: tiles,
rem isBackground=1, no UI).
rem SUITE_THREADS=random picks a different concurrent-render count per run, the
rem way FATE's threads=randomN does.
set "THREADS=%SUITE_THREADS%"
if not defined THREADS set "THREADS=4"
if /i "%THREADS%"=="random" set /a THREADS=%RANDOM% %% 4 + 1
for %%P in (nuke resolve natron) do (
  echo   host profile: %%P
  "%CONTRACT%" "%OFX_FILE%" ^
    --host-profile %%P ^
    --id com.ofxplugindeveloper.minimalcudagain ^
    --param gain:double --param gainAlpha:double ^
    --clip Source --clip Output ^
    --identity --negative-pitch ^
    --set gain=2.0 --not-identity-after-set ^
    --depth half --depth byte ^
    --src-bounds-inset 2 --dst-bounds-inset 2 ^
    --concurrency-param gain ^
    --concurrency-threads %THREADS% || goto :error
)

rem A word the parser does not know must not vanish. ExecutionMode.h reports it as
rem unrecognised and the plug-in reads that as auto, and the Device label carries
rem the raw word back so a typo is visible in the host instead of quietly doing
rem nothing - which is how a run that never left CUDA_ONLY gets documented as a
rem hybrid test. The log line reports the same word and does not survive a host's
rem own log (measured 2026-11-26). The policy test proves the parser's half; this
rem is the only run that proves the label's half, and it needs a process of its own
rem because the mode is read once per process. What it asserts is the parameter, not
rem a path: on this harness the render is the CPU loop, so the label reads
rem "CPU_ONLY - ..." with the sentence after it.
echo   host profile: resolve, OFXPLUGIN_GPU_MODE=gpux (unrecognised)
set "OFXPLUGIN_GPU_MODE=gpux"
"%CONTRACT%" "%OFX_FILE%" ^
    --host-profile resolve ^
    --id com.ofxplugindeveloper.minimalcudagain ^
    --param gain:double --param gainAlpha:double ^
    --clip Source --clip Output ^
    --label-param device ^
    --label-contains "device=OFXPLUGIN_GPU_MODE='gpux' is not auto|cpu|cuda|hybrid"
set "TYPO_STATUS=%ERRORLEVEL%"
set "OFXPLUGIN_GPU_MODE="
if not "%TYPO_STATUS%"=="0" goto :error

echo.rem ---- [4] execution mode --------------------------------------------------
rem The four gates in src/ExecutionMode.h are a pure function over facts, so they
rem are tested without a GPU, a host or a kernel: build.bat builds the target with
rem the rest of the tree, and this step says what it decided at every boundary.
echo.
echo == [4/4] execution-mode gates (no GPU needed) ==
if not exist "%BUILD_DIR%\execution_mode_test.exe" (
  echo   skipped: no execution_mode_test.exe in "%BUILD_DIR%" ^(run build.bat first^)
) else (
  "%BUILD_DIR%\execution_mode_test.exe" || goto :error
)

echo all Windows checks passed

exit /b 0

:error
echo.
echo FAILED (see the step above)
exit /b 1
