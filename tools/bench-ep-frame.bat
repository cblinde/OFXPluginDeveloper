@echo off
rem SPDX-License-Identifier: BSD-3-Clause
rem ---------------------------------------------------------------------------
rem Build tools/bench-ep-frame.cpp into an ONNX Runtime package's own bin
rem directory and run it: one frame through the CPU and the CUDA execution
rem providers, the measurement references/runtime-comparison.md section 5 quotes.
rem
rem   tools\bench-ep-frame.bat <ort-root> <model.onnx> <W> <H> [options]
rem
rem   tools\bench-ep-frame.bat .validation\ort-gpu-1.32.0 models\test_conv.onnx 1920 1080
rem
rem <ort-root> is a package with include\, lib\onnxruntime.lib and bin\ - the
rem bundled one (.validation\ort-gpu-1.32.0 here) rather than the wheel in .venv,
rem because the wheel has no CUDA provider at all. The .exe is written into that
rem bin\ directory on purpose: Windows resolves onnxruntime.dll and
rem onnxruntime_providers_cuda.dll beside the executable before anything on PATH,
rem so the program times the DLLs a bundle would ship and not whichever runtime
rem the machine happens to have installed.
rem
rem The options after the frame size are the program's own: --iterations N,
rem --warmup N, --threads N. See tools\bench-ep-frame.cpp for what a sample is
rem (ModelRunner::Run's per-frame path: an OrtValue over the caller's buffer, Run,
rem memcpy back) and why the first frame is printed separately.
rem
rem Exit codes: 0 ran, 2 usage or a missing package file, 1 the build or the
rem program failed.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions

if "%~1"=="" goto :usage
if "%~2"=="" goto :usage
if "%~3"=="" goto :usage
if "%~4"=="" goto :usage

rem %0 moves with every shift, so the script's own directory has to be
rem captured before the first one - otherwise %~dp0 ends up pointing at the
rem directory of whichever argument is in %0 by the time it is used.
set "HERE=%~dp0"
set "ORT=%~1"
set "MODEL=%~2"
set "WIDTH=%~3"
set "HEIGHT=%~4"

rem Everything after the first four arguments, verbatim, goes to the program.
set "PASSTHROUGH="
shift
shift
shift
shift
:collect
if "%~1"=="" goto :collected
set "PASSTHROUGH=%PASSTHROUGH% %1"
shift
goto :collect
:collected

if not exist "%ORT%\include\onnxruntime_cxx_api.h" (
  echo error: "%ORT%\include\onnxruntime_cxx_api.h" not found - is "%ORT%" an ONNX Runtime package?
  exit /b 2
)
if not exist "%ORT%\lib\onnxruntime.lib" (
  echo error: "%ORT%\lib\onnxruntime.lib" not found
  exit /b 2
)
if not exist "%MODEL%" (
  echo error: "%MODEL%" not found
  exit /b 2
)

rem ---- MSVC environment ------------------------------------------------------
rem A Developer Command Prompt exports VCINSTALLDIR; otherwise the install roots
rem are scanned. vswhere is deliberately not used here: on this machine it exists
rem but does not run from a plain cmd (measured: "the command vswhere.exe is
rem misspelled or could not be found"), and the scan finds the same toolset.
set "VCVARS="
if defined VCINSTALLDIR if exist "%VCINSTALLDIR%\Auxiliary\Build\vcvars64.bat" set "VCVARS=%VCINSTALLDIR%\Auxiliary\Build\vcvars64.bat"
for /d %%D in ("%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools" "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools" "%ProgramFiles%\Microsoft Visual Studio\2022\Community" "%ProgramFiles%\Microsoft Visual Studio\2022\Professional" "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise" "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community" "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools") do (
  if exist "%%~D\VC\Auxiliary\Build\vcvars64.bat" if not defined VCVARS set "VCVARS=%%~D\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
  echo error: no vcvars64.bat found; run from a Developer Command Prompt
  exit /b 2
)
call "%VCVARS%" >nul || exit /b 2

rem ---- build ----------------------------------------------------------------
rem The object file goes next to the exe, not into the caller's directory: cl.exe
rem otherwise drops bench-ep-frame.obj into whatever directory the script was
rem started from, which is how a probe leaves untracked files behind.
echo [build] cl /O2 tools\bench-ep-frame.cpp -^> "%ORT%\bin\bench-ep-frame.exe"
cl /nologo /std:c++17 /EHsc /O2 /MD /I"%ORT%\include" "%HERE%bench-ep-frame.cpp" ^
   /Fo:"%ORT%\bin\bench-ep-frame.obj" ^
   /Fe:"%ORT%\bin\bench-ep-frame.exe" ^
   /link /LIBPATH:"%ORT%\lib" onnxruntime.lib
if errorlevel 1 (
  echo error: the probe did not compile
  exit /b 1
)

rem ---- run ------------------------------------------------------------------
echo.
"%ORT%\bin\bench-ep-frame.exe" "%MODEL%" %WIDTH% %HEIGHT%%PASSTHROUGH%
exit /b %errorlevel%

:usage
echo usage: %~nx0 ^<ort-root^> ^<model.onnx^> ^<W^> ^<H^> [--iterations N] [--warmup N] [--threads N]
echo.
echo   ort-root is a package with include\, lib\onnxruntime.lib and bin\ - the
echo   bundled one, not the CPU-only wheel in .venv.
exit /b 2
