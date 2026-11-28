@echo off
rem SPDX-License-Identifier: BSD-3-Clause
rem ---------------------------------------------------------------------------
rem Windows counterpart of test/run-tests.sh for this template.
rem
rem   test\run-tests.bat                 build, artifact check, host contract,
rem                                      install, device
rem   test\run-tests.bat Release nobuild reuse build\ instead of calling build.bat
rem
rem Linux runs this through g++, nm -D, readelf, dlopen and valgrind. None of
rem those exist in a Windows developer shell, so the same checks are expressed
rem with the tools that do:
rem
rem   [1/6] build     build.bat, then the artifact check it already runs
rem   [2/6] mode      vulkan_mode_test.exe: the six gates that choose between
rem                   the device and the CPU loop. No device, no Vulkan loader
rem                   and no SDK - it runs on every machine, which is the
rem                   point (the .sh runs the same binary)
rem   [3/6] artifact  testkit\check-ofx-artifact.bat --strict (dumpbin, cuobjdump)
rem   [4/6] contract  testkit\build-contract-test.bat + LoadLibraryExW, the same
rem                   host protocol as dlopen: load, describe, create, render,
rem                   destroy, unload, twice, with the negative cases; once
rem                   per host shape, one pass that refuses both capability
rem                   writes, and one with OFXPLUGIN_VULKAN_MODE=cpu that
rem                   asserts the Device label says CPU
rem   [5/6] install   cmake --install into a temp prefix, then load and
rem                   render from the installed copy
rem   [6/6] device    vulkan_suite.exe: shader, descriptor set, dispatch, both
rem                   memory arrangements - NOT VERIFIED without a device
rem
rem What it does NOT do: valgrind and the sanitizer builds. A build that leaks is
rem caught on Linux; on Windows the contract's unload cycle is what runs.
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
set "OFX_FILE=%BUILD_DIR%\bundle\VulkanGain.ofx.bundle\Contents\Win64\VulkanGain.ofx"

rem ---- [1] build ------------------------------------------------------------
if "%NOBUILD%"=="0" (
  echo == [1/6] build ^(%CONFIG%^) ==
  rem The .\ prefix is load-bearing: a bare build.bat is not found when
  rem NoDefaultCurrentDirectoryInExePath=1 is set.
  call "%ROOT%\build.bat" %CONFIG% || goto :error
) else (
  echo == [1/6] build skipped ^(nobuild^)
)

if not exist "%OFX_FILE%" (
  echo.
  echo error: no .ofx at %OFX_FILE%
  echo        run ".\build.bat %CONFIG%" first, or drop the nobuild argument.
  exit /b 1
)

rem ---- [2] path policy ------------------------------------------------------
rem The one step in this file that needs nothing at all: VulkanMode.h is policy
rem only (no Vulkan headers, no OFX headers), so the table that decides between
rem the device and the CPU loop is compiled and executed on a machine with no
rem driver. A build with -DOFXPLUGIN_VULKAN=OFF has to pass it too, which is why
rem the target is built unconditionally.
echo.
echo == [2/6] path policy (six gates, no device needed) ==
set "MODETEST=%BUILD_DIR%\self-test\vulkan_mode_test.exe"
if not exist "%MODETEST%" (
  echo   FAIL: no %MODETEST% - the configure did not build the policy test
  goto :error
)
"%MODETEST%" || goto :error

rem ---- [3] artifact ----------------------------------------------------------
rem Same checker build.bat runs; here it is the point of the step, not a side
rem effect, so a failure names which claim broke.
echo.
echo == [3/6] artifact (exports, dependents) ==
call "%ROOT%\..\..\testkit\check-ofx-artifact.bat" "%OFX_FILE%" --strict || goto :error

rem ---- [4] host contract -----------------------------------------------------
echo.
echo == [4/6] host contract (LoadLibraryExW + fake host) ==
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
rem isBackground=1, no UI). Only the capabilities the plug-in reads change.
rem SUITE_THREADS=random picks a different concurrent-render count per run, the
rem way FATE's threads=randomN does.
set "THREADS=%SUITE_THREADS%"
if not defined THREADS set "THREADS=4"
if /i "%THREADS%"=="random" set /a THREADS=%RANDOM% %% 4 + 1
for %%P in (nuke resolve natron) do (
  echo   host profile: %%P
  "%CONTRACT%" "%OFX_FILE%" ^
    --host-profile %%P ^
    --id com.ofxplugindeveloper.vulkaingain ^
    --version 1.0 ^
    --param gain:double ^
    --clip Source ^
    --clip Output ^
    --byte-dst-rejected ^
    --src-bounds-inset 2 --dst-bounds-inset 2 ^
    --negative-pitch ^
    --concurrency-param gain ^
    --concurrency-threads %THREADS% || goto :error
)

rem Resolve 21.2.0.5 answers a write of kOfxImageEffectPropCPURenderSupported with
rem kOfxStatErrUnknown, the support library turns that into a failed describe and
rem the plug-in never loads ("error code 4"). The guard around the two capability
rem writes is what keeps it loading, and this run is what executes the guard: with
rem the guard removed, describe fails here with kOfxStatErrMissingHostFeature.
echo   host profile: resolve, refusing both capability writes
"%CONTRACT%" "%OFX_FILE%" ^
    --host-profile resolve ^
    --reject-property kOfxImageEffectPropCPURenderSupported ^
    --reject-property kOfxImageEffectPropNoSpatialAwareness ^
    --id com.ofxplugindeveloper.vulkaingain ^
    --version 1.0 ^
    --param gain:double ^
    --clip Source ^
    --clip Output ^
    --byte-dst-rejected ^
    --src-bounds-inset 2 --dst-bounds-inset 2 ^
    --negative-pitch ^
    --concurrency-param gain ^
    --concurrency-threads %THREADS% || goto :error

rem The one path a machine with a device cannot reach otherwise: the override
rem makes VulkanMode.h's first gate say CPU, the plug-in writes that decision
rem into the Device parameter after a real render, and --label-contains fails the
rem run if the label does not carry it. Without the switch this assertion could
rem only be made on a machine that has no Vulkan driver - and the value of the
rem switch is exactly that the fallback is exercised where the device exists.
rem On a machine with no device the same label comes from the second gate, so the
rem run passes there too; only this run proves the override reaches the render.
echo   host profile: resolve, OFXPLUGIN_VULKAN_MODE=cpu (forced CPU path)
set "OFXPLUGIN_VULKAN_MODE=cpu"
"%CONTRACT%" "%OFX_FILE%" ^
    --host-profile resolve ^
    --id com.ofxplugindeveloper.vulkaingain ^
    --version 1.0 ^
    --param gain:double ^
    --clip Source ^
    --clip Output ^
    --label-param device ^
    --label-contains device=CPU
set "FORCED_STATUS=%ERRORLEVEL%"
set "OFXPLUGIN_VULKAN_MODE="
if not "%FORCED_STATUS%"=="0" goto :error

rem A word the parser does not know must not vanish. VulkanMode.h reports it as
rem unrecognised, the plug-in reads that as auto and writes the raw word into the
rem Device label, so a typo is visible in the host instead of quietly doing
rem nothing - which is how a run that never left the device gets documented as
rem a CPU test. The policy test proves the parser's half; this is the only run
rem that proves the label's half, and it needs a process of its own because the
rem mode is read once per process. It passes on both kinds of machine: with a
rem device the label is the device's, without one it is the CPU label, and both
rem append the sentence this run asks for.
echo   host profile: resolve, OFXPLUGIN_VULKAN_MODE=gpux (unrecognised)
set "OFXPLUGIN_VULKAN_MODE=gpux"
"%CONTRACT%" "%OFX_FILE%" ^
    --host-profile resolve ^
    --id com.ofxplugindeveloper.vulkaingain ^
    --version 1.0 ^
    --param gain:double ^
    --clip Source ^
    --clip Output ^
    --label-param device ^
    --label-contains "device=OFXPLUGIN_VULKAN_MODE='gpux' is not auto, cpu or gpu"
set "TYPO_STATUS=%ERRORLEVEL%"
set "OFXPLUGIN_VULKAN_MODE="
if not "%TYPO_STATUS%"=="0" goto :error

rem ---- [5] install ---------------------------------------------------------
rem The build tree is not an install: a host loads what is under OFX/Plugins, so
rem `cmake --install` into a throwaway prefix is the move that exposes a path or a
rem dependency that only ever resolved inside build\. The harness then renders one
rem frame from the installed copy - the same contract run, one directory over.
echo.
echo == [5/6] install and load the installed copy ==
set "PREFIX=%TEMP%\ofx-install-%RANDOM%%RANDOM%"
cmake --install "%BUILD_DIR%" --prefix "%PREFIX%" --config %CONFIG% > "%PREFIX%.log" 2>&1
if errorlevel 1 (
  type "%PREFIX%.log"
  goto :error
)
set "INSTALLED="
for /r "%PREFIX%\OFX\Plugins" %%F in (*.ofx) do if not defined INSTALLED set "INSTALLED=%%F"
if not defined INSTALLED (
  echo   FAIL: cmake --install wrote no .ofx under "%PREFIX%\OFX\Plugins"
  goto :error
)
echo   %INSTALLED%
"%CONTRACT%" "%INSTALLED%" ^
    --host-profile resolve ^
    --id com.ofxplugindeveloper.vulkaingain ^
    --version 1.0 ^
    --param gain:double ^
    --clip Source ^
    --clip Output || goto :error
rmdir /s /q "%PREFIX%" 2>nul

rem ---- [5] device -----------------------------------------------------------
rem The half no host run proves cheaply: the shader, the descriptor set and the
rem dispatch. It is guarded rather than assumed - a machine with no Vulkan device
rem (or a CPU-only configure) reports NOT VERIFIED instead of a green step.
echo == [6/6] device suite (Vulkan vs CPU) ==
set "SELFTEST=%BUILD_DIR%\self-test\vulkan_suite.exe"
if not exist "%SELFTEST%" goto :selftest_absent
"%SELFTEST%"
set "SELFTEST_STATUS=%ERRORLEVEL%"
if "%SELFTEST_STATUS%"=="0" goto :selftest_label
if "%SELFTEST_STATUS%"=="2" goto :selftest_nodevice
goto :error
:selftest_label
rem The device path's own label - the direction the forced run in [4/6] cannot
rem cover. A machine whose device answered has to name that device in the
rem parameter a host reads; until this ran, only the CPU side of the decision
rem was asserted anywhere.
rem
rem A CPU-only configure has no device to name, and its bundle is still built on
rem purpose (windows.yml builds it that way), so the assertion follows the build
rem rather than the machine: the compile-time reason has to reach the label.
rem Demanding "Vulkan" there failed every OFXPLUGIN_VULKAN=OFF run at this check,
rem after the suite had already passed on the CPU loop.
set "DEVICE_LABEL=Vulkan"
if /I "%OFXPLUGIN_VULKAN%"=="OFF" set "DEVICE_LABEL=built with -DOFXPLUGIN_VULKAN=OFF"
echo   device label
"%CONTRACT%" "%OFX_FILE%" ^
    --host-profile resolve ^
    --id com.ofxplugindeveloper.vulkaingain ^
    --version 1.0 ^
    --param gain:double ^
    --clip Source ^
    --clip Output ^
    --label-param device ^
    --label-contains "device=%DEVICE_LABEL%" || goto :error
goto :selftest_done
:selftest_absent
echo   NOT VERIFIED no device suite binary - configured with OFXPLUGIN_VULKAN=OFF
goto :selftest_done
:selftest_nodevice
echo   NOT VERIFIED the suite found no Vulkan device on this machine
:selftest_done

echo.
echo all Windows checks passed
exit /b 0

:error
echo.
echo FAILED (see the step above)
exit /b 1
