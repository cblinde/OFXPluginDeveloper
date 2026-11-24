@echo off
rem ---------------------------------------------------------------------------
rem Windows counterpart of test/run-tests.sh for this template.
rem
rem   test\run-tests.bat                 artifact check, GPU harness, contract
rem   test\run-tests.bat Release nobuild reuse build\ instead of calling build.bat
rem
rem Linux runs the whole story through g++, nm -D, readelf, dlopen and valgrind.
rem None of those exist on Windows, so this script runs the equivalents that do:
rem
rem   [0/3] build     build.bat, which also verifies the artifact
rem   [1/3] artifact  testkit\check-ofx-artifact.bat --strict  (dumpbin, cuobjdump)
rem   [2/3] kernels   nvcc test\gpu-kernel-test.cu, executed on the real GPU -
rem                   the only check in this repository that runs SASS
rem   [3/3] contract  testkit\build-contract-test.bat + LoadLibraryExW, the same
rem                   checks as dlopen, against the built .ofx
rem
rem What it does NOT do: the CPU-emulated equivalence suite (test\run-tests.sh,
rem which needs g++ and the shim headers) and valgrind. Step 2 covers the same
rem arithmetic on hardware where step 1 of the shell script covers it in
rem emulation; on a machine with a GPU that is the stronger check, not a
rem substitute for the PTX count.
rem
rem Environment:
rem   OFX_SOURCE_DIR  openfx checkout; else read from build\CMakeCache.txt
rem   CUDA_ARCHS      e.g. 86; else the GPU's own compute capability, else 75
rem   CPU_ONLY=1      skip the GPU step and do not demand cubins
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

set "BUILD_DIR=%ROOT%\build"
set "OFX_FILE=%BUILD_DIR%\bundle\MinimalCudaGain.ofx.bundle\Contents\Win64\MinimalCudaGain.ofx"

rem ---- [0] build -----------------------------------------------------------
if "%NOBUILD%"=="0" (
  echo == [0/3] build ^(%CONFIG%^) ==
  rem A bare `build.bat` is not found when NoDefaultCurrentDirectoryInExePath=1
  rem is set, which is a common policy; the .\ prefix is load-bearing.
  call "%ROOT%\build.bat" %CONFIG% || goto :error
) else (
  echo == [0/3] build skipped ^(nobuild^) ==
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
echo == [1/3] artifact (exports, dependents, device code) ==
set "CUDA_CHECK="
if not "%CPU_ONLY%"=="1" set "CUDA_CHECK=--cuda"
rem Two levels and not one: this script lives in test\, so ROOT is the template
rem directory and the shared testkit is at <repo>\testkit.
call "%ROOT%\..\..\testkit\check-ofx-artifact.bat" "%OFX_FILE%" %CUDA_CHECK% --strict || goto :error

rem ---- [2] kernels on the GPU ----------------------------------------------
echo.
if "%CPU_ONLY%"=="1" (
  echo == [2/3] GPU harness skipped ^(CPU_ONLY=1^) ==
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
  echo == [2/3] GPU harness skipped: no vcvars64.bat ^(no MSVC C++ toolset^) ==
  goto :contract
)
call "!VCVARS!" -vcvars_ver=14.44 >nul 2>&1 || call "!VCVARS!" >nul 2>&1

echo == [2/3] GPU harness (real device code, not emulated) ==
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
rem nvcc wants a list of numbers, one sm_ per entry.
set "NVARCH="
for %%A in (!ARCHS!) do set "NVARCH=!NVARCH! -arch=sm_%%A"
if not defined NVARCH set "NVARCH=-arch=sm_!ARCHS!"

nvcc -DOFXPLUGIN_WITH_CUDA=1 !NVARCH! -O2 -std=c++17 "%HERE%\gpu-kernel-test.cu" -o "%BUILD_DIR%\gpu-kernel-test.exe" || goto :error
"%BUILD_DIR%\gpu-kernel-test.exe" || goto :error

:contract
rem ---- [3] host contract ----------------------------------------------------
echo.
echo == [3/3] host contract (LoadLibraryExW + fake host) ==
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
"%CONTRACT%" "%OFX_FILE%" ^
  --id com.ofxplugindeveloper.minimalcudagain ^
  --param gain:double --param gainAlpha:double ^
  --clip Source --clip Output ^
  --identity --negative-pitch ^
  --set gain=2.0 --not-identity-after-set ^
  --byte-dst-rejected ^
  --src-bounds-inset 2 --dst-bounds-inset 2 ^
  --concurrency-param gain || goto :error

echo.
echo all Windows checks passed
exit /b 0

:error
echo.
echo FAILED (see the step above)
exit /b 1
