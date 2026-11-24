@echo off
rem ---------------------------------------------------------------------------
rem Two-tier verification of a built OFX bundle (Windows).
rem
rem   verify-bundle.bat <bundle-dir> [--plugin-id <id>] [--natron <dir>]
rem                     [--gain <x>] [--tier1-only] [--no-tier1-build]
rem
rem <bundle-dir> is a .ofx.bundle directory, or a build directory containing
rem bundle\*.ofx.bundle (both are accepted; a build dir is the documented
rem OFX_PLUGIN_PATH value).
rem
rem Tier 0  layout            Contents\Win64\<Name>.ofx, with <Name> equal to the
rem                           bundle stem, before anything is executed.
rem Tier 1  official host     the OpenFX project's own HostSupport + cacheDemo
rem                           (testkit\official-host) load and describe the
rem                           plug-in. Built on demand.
rem Tier 2  real host         NatronRenderer renders one 256x256 frame through
rem                           the plug-in and the pixels are checked in linear
rem                           space (testkit\tier2-natron.py).
rem
rem Skips are named, never silent: a missing Natron or a missing Python prints
rem what is missing and how to supply it, and the exit status stays 0 unless a
rem layer that RAN failed. Use --tier1-only in CI that has no Natron.
rem
rem Environment:
rem   OFX_PLUGIN_PATH   overridden with the bundle's parent for both tiers, so
rem                     the host scans our build first (the official cache also
rem                     scans the system directory - see testkit\official-host)
rem   NATRON_ROOT       Natron install directory, if --natron is not given
rem ---------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion

set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
set "ROOT=%HERE%\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

set "BUNDLE="
set "PLUGIN_ID="
set "NATRON=%NATRON_ROOT%"
set "GAIN=0.5"
set "TIER1_ONLY=0"
set "NO_T1_BUILD=0"

:parse
if "%~1"=="" goto :parsed
if /I "%~1"=="--plugin-id" ( set "PLUGIN_ID=%~2" & shift & shift & goto :parse )
if /I "%~1"=="--natron"    ( set "NATRON=%~2"    & shift & shift & goto :parse )
if /I "%~1"=="--gain"      ( set "GAIN=%~2"      & shift & shift & goto :parse )
if /I "%~1"=="--tier1-only" ( set "TIER1_ONLY=1" & shift & goto :parse )
if /I "%~1"=="--no-tier1-build" ( set "NO_T1_BUILD=1" & shift & goto :parse )
if not defined BUNDLE (
  set "BUNDLE=%~1"
  shift
  goto :parse
)
echo error: unexpected argument "%~1"
echo usage: verify-bundle.bat ^<bundle-dir^> [--plugin-id ^<id^>] [--natron ^<dir^>] [--gain ^<x^>]
exit /b 2

:parsed
if not defined BUNDLE (
  echo usage: verify-bundle.bat ^<bundle-dir^> [--plugin-id ^<id^>] [--natron ^<dir^>] [--gain ^<x^>]
  exit /b 2
)

rem ---- resolve the bundle ---------------------------------------------------
for %%I in ("%BUNDLE%") do set "BUNDLE=%%~fI"
if exist "%BUNDLE%\bundle" (
  for /d %%B in ("%BUNDLE%\bundle\*.ofx.bundle") do (
    if not defined BUNDLE_DIR set "BUNDLE_DIR=%%~fB"
  )
) else (
  set "BUNDLE_DIR=%BUNDLE%"
)
if not defined BUNDLE_DIR (
  echo error: no .ofx.bundle under "%BUNDLE%"
  exit /b 2
)
rem OFX_PLUGIN_PATH is the directory the *.ofx.bundle folders live in.
for %%I in ("%BUNDLE_DIR%\..") do set "PLUGIN_DIR=%%~fI"

set "OFX_FILE="
for %%F in ("%BUNDLE_DIR%\Contents\Win64\*.ofx") do if not defined OFX_FILE set "OFX_FILE=%%~fF"

echo == tier 0: bundle layout ==
echo   bundle: %BUNDLE_DIR%
if not defined OFX_FILE (
  echo   FAIL: no Contents\Win64\*.ofx
  for /r "%BUNDLE_DIR%" %%F in (*.ofx) do echo     found instead: %%F
  exit /b 1
)
for %%F in ("%OFX_FILE%") do set "OFX_STEM=%%~nF"
rem %%~nxB and not %%~nB: for a directory the latter drops the extension, so
rem 'MinimalCudaGain.ofx.bundle' came back as 'MinimalCudaGain.ofx'.
for %%B in ("%BUNDLE_DIR%") do set "BUNDLE_NAME=%%~nxB"
rem The contract is "<stem>.ofx inside <stem>.ofx.bundle", so compare the whole
rem name rather than stripping a suffix (a substitution here cut the wrong part
rem and rejected a correct bundle).
if /I not "%OFX_STEM%.ofx.bundle"=="%BUNDLE_NAME%" (
  echo   FAIL: binary "%OFX_STEM%.ofx" does not match bundle "%BUNDLE_NAME%"
  exit /b 1
)
echo   ok: Contents\Win64\%OFX_STEM%.ofx

echo.
echo == tier 1: official OpenFX host (HostSupport + cacheDemo) ==
set "CACHEDEMO=%HERE%\official-host\build\Release\cacheDemo.exe"
if not exist "%CACHEDEMO%" if "%NO_T1_BUILD%"=="0" (
  echo   building the official host...
  rem Default generator: CMake picks the newest Visual Studio with a C++
  rem toolset. No CUDA is involved, so no toolset pinning is needed here.
  cmake -S "%HERE%\official-host" -B "%HERE%\official-host\build" >nul 2>&1
  cmake --build "%HERE%\official-host\build" --config Release --parallel >nul 2>&1
)
if not exist "%CACHEDEMO%" (
  echo   SKIP: cacheDemo.exe not built ^(see testkit\official-host\README.md^)
  goto :tier2
)

rem Scratch inside the build tree, not %TEMP%: cacheDemo writes newCache.xml into
rem its working directory, and a verification run should leave nothing outside
rem the project. (The same command run from %TEMP% by hand also exits 0, so the
rem location is hygiene, not a workaround for the abort seen in the batch run.)
set "T1_DIR=%HERE%\official-host\build\tier1-scratch"
if exist "%T1_DIR%" rmdir /s /q "%T1_DIR%" >nul 2>&1
mkdir "%T1_DIR%" >nul 2>&1
pushd "%T1_DIR%"
set "OFX_PLUGIN_PATH=%PLUGIN_DIR%"
"%CACHEDEMO%" > "%T1_DIR%\out.txt" 2>&1
set "T1_STATUS=%ERRORLEVEL%"
popd
set "OFX_PLUGIN_PATH="

if not "%T1_STATUS%"=="0" (
  echo   FAIL: cacheDemo exited %T1_STATUS%
  exit /b 1
)
if defined PLUGIN_ID (
  findstr /c:"Plug-in:%PLUGIN_ID%" "%T1_DIR%\out.txt" >nul 2>&1
  if errorlevel 1 (
    echo   FAIL: the official host did not load/describe %PLUGIN_ID%
    exit /b 1
  )
  echo   ok: %PLUGIN_ID% loaded and described by the official host
) else (
  echo   ok: cacheDemo described plugins ^(pass --plugin-id to assert one^)
)
echo   note: the official cache also scans the system plugin directory,
echo         so this takes seconds on a machine with commercial bundles installed.

:tier2
if "%TIER1_ONLY%"=="1" (
  echo.
  echo == tier 2: skipped ^(--tier1-only^) ==
  exit /b 0
)

echo.
echo == tier 2: headless render through NatronRenderer ==
if not defined PLUGIN_ID (
  echo   SKIP: --plugin-id is required for tier 2
  exit /b 0
)
if not defined NATRON (
  echo   SKIP: no Natron. Set NATRON_ROOT or pass --natron ^<dir^>.
  echo         Natron 2.5.0 portable: https://github.com/NatronGitHub/Natron/releases
  exit /b 0
)
rem Python discovery has to test the interpreter, not just find the name: on a
rem machine without Python, `where python` returns the Microsoft Store alias in
rem WindowsApps, which starts, refuses to run anything, and would make tier 2
rem look broken. Each candidate has to answer `-c "import sys"`.
set "PY="
if defined PYTHON if exist "%PYTHON%" (
  "%PYTHON%" -c "import sys" >nul 2>&1 && set "PY=%PYTHON%"
)
if not defined PY (
  for /f "delims=" %%Q in ('py -3 -c "import sys;print(sys.executable)" 2^>nul') do (
    if not defined PY if exist "%%Q" set "PY=%%Q"
  )
)
if not defined PY (
  for %%P in (python3.exe python.exe) do (
    if not defined PY (
      for /f "delims=" %%Q in ('where %%P 2^>nul') do (
        if not defined PY "%%Q" -c "import sys" >nul 2>&1 && set "PY=%%Q"
      )
    )
  )
)
if not defined PY (
  echo   SKIP: no working Python 3 found; tier 2 needs one ^(standard library only^)
  echo         set PYTHON=^<path to python.exe^> to point at one
  exit /b 0
)

"%PY%" "%HERE%\tier2-natron.py" --natron "%NATRON%" --plugin-id "%PLUGIN_ID%" ^
  --ofx-path "%PLUGIN_DIR%" --gain "%GAIN%" || exit /b 1

echo.
echo all executed tiers passed
exit /b 0
