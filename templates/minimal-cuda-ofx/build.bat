@echo off
rem ---------------------------------------------------------------------------
rem Out-of-source build for the minimal CUDA OFX template (Windows / MSVC).
rem
rem   build.bat                    Release, CUDA if a toolkit is found
rem   build.bat Debug              Debug
rem   build.bat Release install    build, then copy the bundle into
rem                                %%COMMONPROGRAMFILES%%\OFX\Plugins
rem
rem Environment overrides:
rem   CUDA_HOME        toolkit to use; else discovered via CUDA_PATH / PATH
rem   OFX_SOURCE_DIR   point at an existing openfx checkout (offline builds,
rem                    or Support\Developer\OpenFX shipped with Resolve)
rem   CUDA_ARCHS       e.g. 86;89  (default: 75;80;86;89;90[;100;120])
rem   CPU_ONLY=1       build without CUDA to prove the fallback path
rem
rem There is no no-root bootstrap here on purpose: on Windows the CUDA toolkit
rem is an installer, not an unpackable package. Install it from
rem https://developer.nvidia.com/cuda-downloads and re-run; unlike Linux there
rem is no reliable ar(1)+tar(1) equivalent that avoids a system install.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions

set "HERE=%~dp0"
set "BUILD_DIR=%HERE%build"
if "%~1"=="" (set "CONFIG=Release") else (set "CONFIG=%~1")

rem ---- CUDA discovery -------------------------------------------------------
set "CUDA_FOUND="
if defined CUDA_HOME if exist "%CUDA_HOME%\bin\nvcc.exe" set "CUDA_FOUND=%CUDA_HOME%"
if not defined CUDA_FOUND if defined CUDA_PATH if exist "%CUDA_PATH%\bin\nvcc.exe" set "CUDA_FOUND=%CUDA_PATH%"
if not defined CUDA_FOUND (
  for /f "delims=" %%I in ('where nvcc 2^>nul') do (
    if not defined CUDA_FOUND set "CUDA_FOUND=%%~dpI.."
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

rem ---- configure + build ----------------------------------------------------
set "EXTRA="
if defined CUDA_FOUND set "EXTRA=%EXTRA% -DCMAKE_CUDA_COMPILER=%CUDA_FOUND%\bin\nvcc.exe -DCUDAToolkit_ROOT=%CUDA_FOUND% -DOFXPLUGIN_ENABLE_CUDA=ON"
if not defined CUDA_FOUND set "EXTRA=%EXTRA% -DOFXPLUGIN_ENABLE_CUDA=OFF"
if defined OFX_SOURCE_DIR set "EXTRA=%EXTRA% -DOFX_SOURCE_DIR=%OFX_SOURCE_DIR%"
if defined CUDA_ARCHS set "EXTRA=%EXTRA% -DCMAKE_CUDA_ARCHITECTURES=%CUDA_ARCHS%"

rem "Visual Studio 17 2022" is the generator this template is tested against.
rem Change it here (or pass -G on the command line) for other toolchains.
cmake -S "%HERE%." -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 %EXTRA% || goto :error
cmake --build "%BUILD_DIR%" --config %CONFIG% --parallel || goto :error

if /I "%~2"=="install" (
  cmake --build "%BUILD_DIR%" --config %CONFIG% --target install-bundle || goto :error
)

echo.
echo Bundle: %BUILD_DIR%\bundle\MinimalCudaGain.ofx.bundle
dir /b "%BUILD_DIR%\bundle\MinimalCudaGain.ofx.bundle\Contents\Win64" 2>nul
if defined CUDA_FOUND (
  echo   CUDA path: compiled in, links cudart64_*.dll
) else (
  echo   CUDA path: not compiled ^(CPU-only build^)
)
exit /b 0

:error
echo.
echo BUILD FAILED (see the cmake output above)
exit /b 1
