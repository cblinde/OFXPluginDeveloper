@echo off
rem ---------------------------------------------------------------------------
rem Windows counterpart of test/run-tests.sh for this template.
rem
rem   test\run-tests.bat                 build, artifact check, host contract
rem   test\run-tests.bat Release nobuild reuse build\ instead of calling build.bat
rem
rem Linux runs this through g++, nm -D, readelf, dlopen and valgrind. None of
rem those exist in a Windows developer shell, so the same checks are expressed
rem with the tools that do:
rem
rem   [1/3] build     build.bat, then the artifact check it already runs
rem   [2/3] artifact  testkit\check-ofx-artifact.bat --strict (dumpbin, cuobjdump)
rem   [3/3] contract  testkit\build-contract-test.bat + LoadLibraryExW, the same
rem                   host protocol as dlopen: load, describe, create, render,
rem                   destroy, unload, twice, with the negative cases
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
set "OFX_FILE=%BUILD_DIR%\bundle\ReferenceGain.ofx.bundle\Contents\Win64\ReferenceGain.ofx"

rem ---- [1] build ------------------------------------------------------------
if "%NOBUILD%"=="0" (
  echo == [1/3] build ^(%CONFIG%^) ==
  rem The .\ prefix is load-bearing: a bare build.bat is not found when
  rem NoDefaultCurrentDirectoryInExePath=1 is set.
  call "%ROOT%\build.bat" %CONFIG% || goto :error
) else (
  echo == [1/3] build skipped ^(nobuild^)
)

if not exist "%OFX_FILE%" (
  echo.
  echo error: no .ofx at %OFX_FILE%
  echo        run ".\build.bat %CONFIG%" first, or drop the nobuild argument.
  exit /b 1
)

rem ---- [2] artifact ----------------------------------------------------------
rem Same checker build.bat runs; here it is the point of the step, not a side
rem effect, so a failure names which claim broke.
echo.
echo == [2/3] artifact (exports, dependents) ==
call "%ROOT%\..\..\testkit\check-ofx-artifact.bat" "%OFX_FILE%" --strict || goto :error

rem ---- [3] host contract -----------------------------------------------------
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
    --id com.ofxplugindeveloper.referencegain ^
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

echo.
echo all Windows checks passed
exit /b 0

:error
echo.
echo FAILED (see the step above)
exit /b 1
