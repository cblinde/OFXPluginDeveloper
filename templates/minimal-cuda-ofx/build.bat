@echo off
rem ---------------------------------------------------------------------------
rem Out-of-source build for the minimal CUDA OFX template (Windows / MSVC).
rem
rem   build.bat                    Release
rem   build.bat Debug              Debug
rem   build.bat Release install    build, then copy the bundle into
rem                                %%COMMONPROGRAMFILES%%\OFX\Plugins
rem
rem Environment overrides:
rem   OFX_SOURCE_DIR   point at an existing openfx checkout (offline builds,
rem                    or Support\Developer\OpenFX shipped with Resolve)
rem   CUDA_ARCHS       e.g. 86;89  (default: 75;80;86;89;90[;100;120])
rem   CPU_ONLY=1       build without CUDA to prove the fallback path
rem ---------------------------------------------------------------------------
setlocal EnableExtensions

set "HERE=%~dp0"
set "BUILD_DIR=%HERE%build"
if "%~1"=="" (set "CONFIG=Release") else (set "CONFIG=%~1")

set "EXTRA="
if defined OFX_SOURCE_DIR set "EXTRA=%EXTRA% -DOFX_SOURCE_DIR=%OFX_SOURCE_DIR%"
if defined CUDA_ARCHS set "EXTRA=%EXTRA% -DCMAKE_CUDA_ARCHITECTURES=%CUDA_ARCHS%"
if "%CPU_ONLY%"=="1" set "EXTRA=%EXTRA% -DOFXPLUGIN_ENABLE_CUDA=OFF"

rem "Visual Studio 17 2022" is the generator this template is tested against.
rem Change it here (or pass -G on the command line) for other toolchains.
cmake -S "%HERE%." -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 %EXTRA% || goto :error
cmake --build "%BUILD_DIR%" --config %CONFIG% --parallel || goto :error

if /I "%~2"=="install" (
  cmake --build "%BUILD_DIR%" --config %CONFIG% --target install-bundle || goto :error
)

echo.
echo Bundle: %BUILD_DIR%\bundle\MinimalCudaGain.ofx.bundle
dir /b "%BUILD_DIR%\bundle\MinimalCudaGain.ofx.bundle\Contents\Win64"
exit /b 0

:error
echo.
echo BUILD FAILED (see the cmake output above)
exit /b 1
