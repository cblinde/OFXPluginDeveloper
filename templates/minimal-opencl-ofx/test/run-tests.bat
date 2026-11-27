@echo off
rem SPDX-License-Identifier: BSD-3-Clause
rem ---------------------------------------------------------------------------
rem Windows counterpart of test/run-tests.sh for the OpenCL template.
rem
rem   .\run-tests.bat
rem
rem What it does, in the same order the POSIX script does:
rem   1. build the plug-in (which already verifies the artifact with dumpbin);
rem   2. compile the shared contract harness and run it over the CPU path, three
rem      host profiles - a host that offers no device still has to use this plug-in;
rem   3. compile test/opencl_suite.cpp against the shared fake host and run it with
rem      a real OpenCL context, queue, cl_mem buffers and image2d images handed over
rem      by the host, using whatever OpenCL.dll the driver installed.
rem
rem What it cannot do on Windows: valgrind. The POSIX script runs both binaries
rem under memcheck; there is no equivalent here, so the stage is named as skipped
rem rather than quietly dropped.
rem
rem Environment:
rem   SUITE_THREADS  4 (default) | random - concurrent render threads
rem
rem Exit status: 0 both paths passed, 1 a check failed, 2 no OpenCL device on this
rem machine, so the suite did not run - 2 is not a pass and the log says so.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion
set "HERE=%~dp0"
set "TPL=%HERE%.."
set "ROOT=%TPL%\..\.."
set "BUILD=%TPL%\build"
set "FAILED=0"

echo [1/5] building the OpenCL plug-in (and verifying the artifact)
call "%TPL%\build.bat" || exit /b 1

set "OFX_FILE="
for /r "%BUILD%\bundle" %%F in (*.ofx) do if not defined OFX_FILE set "OFX_FILE=%%F"
if not defined OFX_FILE (
  echo error: no .ofx under %BUILD%\bundle
  exit /b 1
)

rem ---- where the headers came from: the cache the build just wrote ----------
set "OFX_DIR="
if exist "%BUILD%\CMakeCache.txt" (
  for /f "usebackq tokens=1,* delims==" %%A in (`findstr /B /C:"OFX_SOURCE_DIR:PATH" "%BUILD%\CMakeCache.txt"`) do set "OFX_DIR=%%B"
)
if not defined OFX_DIR set "OFX_DIR=%ROOT%\.ofx-ref"
if not exist "!OFX_DIR!\include\ofxCore.h" if exist "!OFX_DIR!\ofxCore.h" set "OFX_DIR=!OFX_DIR!\include"
if not exist "!OFX_DIR!\include\ofxCore.h" if not exist "!OFX_DIR!\ofxCore.h" (
  echo error: no openfx headers at "%OFX_DIR%" and no OFX_SOURCE_DIR to point at
  exit /b 1
)

set "HARNESS=%BUILD%\contract_test.exe"
set "SUITE=%BUILD%\opencl_suite.exe"

echo.
echo [2/5] compiling the shared contract harness
call "%ROOT%\testkit\build-contract-test.bat" "%HARNESS%" "%OFX_DIR%" || exit /b 1

rem ---- MSVC environment, needed for the suite's own compile ------------------
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
)
if defined VSINSTALL if exist "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
for /d %%D in ("%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools" "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools" "%ProgramFiles%\Microsoft Visual Studio\2022\Community" "%ProgramFiles%\Microsoft Visual Studio\2022\Professional" "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise" "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community" "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools") do (
  if exist "%%~D\VC\Auxiliary\Build\vcvars64.bat" if not defined VCVARS set "VCVARS=%%~D\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
  echo error: no vcvars64.bat found; run from a Developer Command Prompt
  exit /b 2
)
call "%VCVARS%" >nul || exit /b 2

echo.
echo [3/5] the host protocol, on the CPU path (3 profiles)
rem SUITE_THREADS=random picks a different concurrent-render count per run, the
rem way FATE's threads=randomN does: not for determinism, but so that a race has
rem to show up in some run instead of in the one configuration someone hard-coded.
set "THREADS=4"
if defined SUITE_THREADS set "THREADS=%SUITE_THREADS%"
if /i "!THREADS!"=="random" (
  set /a THREADS=!RANDOM! %% 4 + 1
  echo   SUITE_THREADS=random: !THREADS! concurrent render threads this run
)
rem The same protocol every other template certifies, with the host never
rem offering a device: kOfxImageEffectPropOpenCLEnabled stays 0, which is the CPU
rem path this plug-in keeps for hosts that have no OpenCL at all.
for %%P in (nuke resolve natron) do (
  echo   host profile: %%P
  "%HARNESS%" "%OFX_FILE%" --host-profile %%P ^
    --id com.ofxplugindeveloper.openclgain --version 1.0 ^
    --param gain:double --clip Source --clip Output ^
    --byte-dst-rejected --src-bounds-inset 2 --dst-bounds-inset 2 ^
    --negative-pitch --concurrency-param gain --concurrency-threads !THREADS!
  if errorlevel 1 set "FAILED=1"
)
if "!FAILED!"=="1" (
  echo error: the contract harness failed
  exit /b 1
)

echo.
echo [4/5] compiling the suite against the shared fake host
rem /D_CRT_SECURE_NO_WARNINGS: the shared fake host parses with sscanf, which MSVC
rem flags as C4996. The suite and the harness are the same binary shape the other
rem templates build, so the flag travels with them.
set "SUITE_OBJ=%BUILD%\suite-obj"
if not exist "%SUITE_OBJ%" mkdir "%SUITE_OBJ%" || exit /b 1
pushd "%SUITE_OBJ%"
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS ^
  /I "%ROOT%\testkit" /I "!OFX_DIR!\include" /I "!OFX_DIR!" ^
  "%HERE%opencl_suite.cpp" "%ROOT%\testkit\fake_host.cpp" ^
  /Fe:"%SUITE%" /link /SUBSYSTEM:CONSOLE
set "CL_STATUS=%ERRORLEVEL%"
popd
rmdir /s /q "%SUITE_OBJ%" >nul 2>&1
if not "%CL_STATUS%"=="0" (
  echo error: the OpenCL suite failed to compile
  exit /b 1
)

echo.
echo [5/5] running the OpenCL suite against %OFX_FILE%
echo   loader : %OPENCL_LOADER_LIB% (default OpenCL.dll from the driver)
"%SUITE%" "%OFX_FILE%"
set "SUITE_STATUS=%ERRORLEVEL%"
if "%SUITE_STATUS%"=="2" (
  echo no OpenCL device on this machine: the suite did not run ^(not a pass^)
)
if not "%SUITE_STATUS%"=="0" if not "%SUITE_STATUS%"=="2" (
  echo error: the OpenCL suite failed with exit %SUITE_STATUS%
  exit /b 1
)

echo.
echo [6/6] valgrind memcheck
echo   skipped: no valgrind on Windows. The POSIX script runs both binaries under
echo   memcheck; this run does not, and says so instead of implying it did.

echo.
if "%SUITE_STATUS%"=="0" (
  echo OK: the OpenCL template passed on this machine
) else (
  echo NOT A PASS: built and contract-checked, but no OpenCL device was found
)
exit /b %SUITE_STATUS%
