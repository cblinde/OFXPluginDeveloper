@echo off
rem SPDX-License-Identifier: BSD-3-Clause
rem
rem Windows counterpart of the artifact checks in build.sh (nm, readelf, file).
rem
rem   check-ofx-artifact.bat <plugin.ofx> [--cuda] [--strict]
rem
rem dumpbin /exports      ~ nm -D --defined-only : exactly the two entry points
rem dumpbin /dependents   ~ readelf -d           : what must be found at load time
rem cuobjdump -lelf       ~ readelf -S | grep nv_fatbin : device code is present
rem
rem The tools are looked up, not assumed to be on PATH. A build with the Visual
rem Studio generator never calls vcvars64.bat - MSBuild gets its environment
rem internally, and the caller's cmd has neither cl.exe nor dumpbin. Trusting
rem PATH there turns every artifact check into a printed skip.
rem
rem   dumpbin    PATH, %VCToolsInstallDir%\bin\Hostx64\x64, vswhere -find
rem   cuobjdump  PATH, %CUDA_PATH%\bin, %CUDA_HOME%\bin, the toolkit roots
rem
rem --strict makes a missing tool a failure, and build.bat passes it: a build
rem that could not inspect its own artifact must not report success. Standalone
rem use keeps the skip, so a bundle can be checked on a machine without VS.
rem
rem Exits 1 on a wrong export set or missing device code, 3 on a missing tool
rem under --strict, 0 otherwise.
setlocal EnableExtensions EnableDelayedExpansion

set "OFX="
set "WANT_CUDA=0"
set "STRICT=0"

:parse
if "%~1"=="" goto :parsed
if /I "%~1"=="--cuda" (
  set "WANT_CUDA=1"
) else if /I "%~1"=="--strict" (
  set "STRICT=1"
) else if not defined OFX (
  set "OFX=%~1"
) else (
  echo error: unexpected argument "%~1"
  echo usage: check-ofx-artifact.bat ^<plugin.ofx^> [--cuda] [--strict]
  exit /b 2
)
shift
goto :parse

:parsed
if not defined OFX (
  echo usage: check-ofx-artifact.bat ^<plugin.ofx^> [--cuda] [--strict]
  exit /b 2
)
if not exist "%OFX%" (
  echo error: no such file: %OFX%
  exit /b 1
)

rem ---- resolve dumpbin -------------------------------------------------------
set "DUMPBIN="
for /f "delims=" %%D in ('where dumpbin 2^>nul') do if not defined DUMPBIN set "DUMPBIN=%%~D"
if not defined DUMPBIN if defined VCToolsInstallDir if exist "%VCToolsInstallDir%bin\Hostx64\x64\dumpbin.exe" set "DUMPBIN=%VCToolsInstallDir%bin\Hostx64\x64\dumpbin.exe"
if not defined DUMPBIN (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "!VSWHERE!" (
    rem -find accepts ** and returns every match; the first is the newest
    rem instance that actually carries the C++ toolset (-requires filters it).
    for /f "usebackq delims=" %%P in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find "VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe"`) do (
      if not defined DUMPBIN set "DUMPBIN=%%~P"
    )
  )
)
if not defined DUMPBIN (
  rem Fallback for a vswhere too old to support -find: the documented install
  rem layout is Program Files\Microsoft Visual Studio\<year>\<edition>\VC\Tools\MSVC.
  rem One wildcard per for /d, because FindFirstFile takes wildcards only in the
  rem last path element.
  for /d %%Y in ("%ProgramFiles%\Microsoft Visual Studio\*") do (
    for /d %%E in ("%%~fY\*") do (
      for /d %%M in ("%%~fE\VC\Tools\MSVC\*") do (
        if exist "%%~fM\bin\Hostx64\x64\dumpbin.exe" set "DUMPBIN=%%~fM\bin\Hostx64\x64\dumpbin.exe"
      )
    )
  )
)
if not defined DUMPBIN (
  if "%STRICT%"=="1" (
    echo check-ofx-artifact: FAILED - dumpbin not found
    echo   looked on PATH, in %%VCToolsInstallDir%%\bin\Hostx64\x64 and through
    echo   vswhere -find. Install the MSVC C++ toolset ^(Desktop development
    echo   with C++^), or run this from a Developer Command Prompt.
    exit /b 3
  )
  echo   artifact: skipped ^(dumpbin not found on PATH, in VCToolsInstallDir or via vswhere^)
  goto :cuda
)
echo   dumpbin: %DUMPBIN%

rem ---- exports ---------------------------------------------------------------
set "TMPEXP=%TEMP%\ofx-exports-%RANDOM%%RANDOM%.txt"
"%DUMPBIN%" /nologo /exports "%OFX%" > "%TMPEXP%" 2>nul
findstr /c:"OfxGetNumberOfPlugins" "%TMPEXP%" >nul
if errorlevel 1 (
  echo FAIL: OfxGetNumberOfPlugins is not exported
  del "%TMPEXP%" >nul 2>&1
  exit /b 1
)
findstr /c:"OfxGetPlugin" "%TMPEXP%" >nul
if errorlevel 1 (
  echo FAIL: OfxGetPlugin is not exported
  del "%TMPEXP%" >nul 2>&1
  exit /b 1
)

rem The export table rows are "ordinal hint RVA name"; count the names so a
rem stray export (for example a leaked Support-library symbol) fails the build.
set "NAMES=0"
for /f %%C in ('findstr /r /c:"^ *[0-9][0-9]* *[0-9][0-9]* *[0-9A-Fa-f][0-9A-Fa-f]* *[A-Za-z_?]" "%TMPEXP%" ^| find /c /v ""') do set "NAMES=%%C"
if not "%NAMES%"=="2" (
  echo FAIL: dumpbin reports %NAMES% exported names, expected 2
  findstr /r /c:"^ *[0-9][0-9]* *[0-9][0-9]* *[0-9A-Fa-f][0-9A-Fa-f]* *[A-Za-z_?]" "%TMPEXP%"
  del "%TMPEXP%" >nul 2>&1
  exit /b 1
)
del "%TMPEXP%" >nul 2>&1
echo   exports: 2 ^(OfxGetNumberOfPlugins, OfxGetPlugin^)

echo   dependents:
"%DUMPBIN%" /nologo /dependents "%OFX%" | findstr /i /c:".dll"

:cuda
if "%WANT_CUDA%"=="0" exit /b 0

rem ---- resolve cuobjdump ------------------------------------------------------
set "CUOBJDUMP="
for /f "delims=" %%D in ('where cuobjdump 2^>nul') do if not defined CUOBJDUMP set "CUOBJDUMP=%%~D"
if not defined CUOBJDUMP if defined CUDA_PATH if exist "%CUDA_PATH%\bin\cuobjdump.exe" set "CUOBJDUMP=%CUDA_PATH%\bin\cuobjdump.exe"
if not defined CUOBJDUMP if defined CUDA_HOME if exist "%CUDA_HOME%\bin\cuobjdump.exe" set "CUOBJDUMP=%CUDA_HOME%\bin\cuobjdump.exe"
if not defined CUOBJDUMP (
  for /d %%V in ("%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v*") do (
    if exist "%%~fV\bin\cuobjdump.exe" set "CUOBJDUMP=%%~fV\bin\cuobjdump.exe"
  )
)
if not defined CUOBJDUMP (
  if "%STRICT%"=="1" (
    echo check-ofx-artifact: FAILED - cuobjdump not found
    echo   looked on PATH, under CUDA_PATH/CUDA_HOME and in
    echo   %%ProgramFiles%%\NVIDIA GPU Computing Toolkit\CUDA\v*.
    exit /b 3
  )
  echo   device code: skipped ^(cuobjdump not found on PATH or in a CUDA install^)
  exit /b 0
)
echo   cuobjdump: %CUOBJDUMP%

set "TMPCUB=%TEMP%\ofx-cubins-%RANDOM%%RANDOM%.txt"
"%CUOBJDUMP%" -lelf "%OFX%" > "%TMPCUB%" 2>nul
set "CUBINS=0"
for /f %%C in ('find /c /v "" ^< "%TMPCUB%"') do set "CUBINS=%%C"
del "%TMPCUB%" >nul 2>&1
if "%CUBINS%"=="0" (
  echo FAIL: --cuda was requested but the image contains no cubin
  exit /b 1
)
echo   device code: %CUBINS% cubin^(s^)
exit /b 0
