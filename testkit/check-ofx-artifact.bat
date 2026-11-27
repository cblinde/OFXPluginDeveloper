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
rem   dumpbin    PATH, %VCToolsInstallDir%\bin\Hostx64\x64, vswhere installationPath
rem   cuobjdump  PATH, %CUDA_PATH%\bin, %CUDA_HOME%\bin, the toolkit roots
rem
rem --strict makes a missing tool a failure, and build.bat passes it: a build
rem that could not inspect its own artifact must not report success. Standalone
rem use keeps the skip, so a bundle can be checked on a machine without VS.
rem
rem Exits 1 on a wrong export set or missing device code, 3 on a missing tool
rem under --strict, 0 otherwise.
rem
rem TOOL SHADOWING: nothing here may call the legacy `find` utility. Git-for-Windows
rem ships a GNU find.exe and puts its usr\bin ahead of System32 on PATH, so
rem   findstr ... | find /c /v ""
rem ran GNU find with /c as a PATH ARGUMENT and walked the whole of C:\ (measured:
rem 4000+ "Permission denied" lines and a build that never returned). Every count
rem below is done with `set /a` over `for /f` instead, which needs no external tool.
rem findstr, where, dumpbin and cuobjdump have no MSYS namesake; those are safe.
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

rem ---- layout: the packaging spec, checked where the link table cannot ------
rem A host finds the binary by walking NAME.ofx.bundle/Contents/<ARCH>/NAME.ofx.
rem An architecture directory it does not know is a plug-in that never loads, an
rem Info.plist outside a macOS bundle is a file the host never reads, and a
rem directory whose name starts with '@' is skipped by the scan altogether. None
rem of that shows up in dumpbin, so it is checked here - first, because it needs
rem no tool. A .ofx passed from outside a bundle keeps the old behaviour: the
rem layout stage says it was skipped instead of inventing a verdict.
for %%I in ("%OFX%\..") do set "ARCH=%%~nxI"
for %%I in ("%OFX%\..\..") do set "CONTENTS=%%~nxI"
for %%I in ("%OFX%\..\..\..") do set "BUNDLE=%%~nxI"
for %%I in ("%OFX%\..\..\..") do set "BUNDLE_ROOT=%%~fI"
set "IN_BUNDLE=0"
if /i "%CONTENTS%"=="Contents" (
  echo %BUNDLE%|findstr /e /i /c:".ofx.bundle" >nul
  if not errorlevel 1 set "IN_BUNDLE=1"
)
if "%IN_BUNDLE%"=="0" (
  echo   layout: skipped ^(not a .ofx.bundle/Contents/^<arch^> path^)
  goto :layout_done
)
set "KNOWN=0"
for %%A in (Win64 Win-arm64ec MacOS Linux-x86-64 Linux-aarch64) do if /i "%ARCH%"=="%%A" set "KNOWN=1"
if "%KNOWN%"=="0" (
  echo FAIL: the architecture directory "%ARCH%" is not one the spec names
  echo   expected one of: Win64, Win-arm64ec, MacOS, Linux-x86-64, Linux-aarch64
  exit /b 1
)
if /i "%ARCH%"=="MacOS" (
  if not exist "%BUNDLE_ROOT%\Contents\Info.plist" (
    echo FAIL: a MacOS bundle needs Contents\Info.plist
    exit /b 1
  )
) else (
  if exist "%BUNDLE_ROOT%\Contents\Info.plist" (
    echo FAIL: Info.plist is a macOS bundle file, not one for %ARCH%
    exit /b 1
  )
)
for /d /r "%BUNDLE_ROOT%\Contents" %%D in (*) do (
  set "PARENT_NAME=%%~nxD"
  if "!PARENT_NAME:~0,1!"=="@" (
    echo FAIL: "%%~nxD" starts with '@', which the host's scan skips
    exit /b 1
  )
)
echo   layout: %BUNDLE%/Contents/%ARCH% ^(an architecture the spec names, no '@' directory^)
:layout_done

rem ---- resolve dumpbin -------------------------------------------------------
set "DUMPBIN="
for /f "delims=" %%D in ('where dumpbin 2^>nul') do if not defined DUMPBIN set "DUMPBIN=%%~D"
if not defined DUMPBIN if defined VCToolsInstallDir if exist "%VCToolsInstallDir%bin\Hostx64\x64\dumpbin.exe" set "DUMPBIN=%VCToolsInstallDir%bin\Hostx64\x64\dumpbin.exe"
if not defined DUMPBIN (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  set "VSPATH="
  if exist "!VSWHERE!" (
    rem -property installationPath, not -find: a for /f command that begins AND
    rem ends with a quote loses both before cmd /c runs it, so
    rem   "C:\Program Files (x86)\...\vswhere.exe" ... -find "VC\..."
    rem starts with C:\Program and fails on every default install. The -property
    rem form ends unquoted, which is why build.bat's generator lookup works.
    for /f "usebackq delims=" %%P in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%P"
  )
  if defined VSPATH (
    rem One wildcard per for /d: FindFirstFile accepts them only in the last
    rem path element. Version directories sort lexically, and the last one is
    rem the newest toolset that was actually installed.
    for /d %%M in ("!VSPATH!\VC\Tools\MSVC\*") do (
      if exist "%%~fM\bin\Hostx64\x64\dumpbin.exe" set "DUMPBIN=%%~fM\bin\Hostx64\x64\dumpbin.exe"
    )
  )
)
if not defined DUMPBIN (
  rem Fallback when vswhere is missing or reports no instance: the documented
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
    echo   looked on PATH, in %%VCToolsInstallDir%%\bin\Hostx64\x64, under vswhere's
    echo   installationPath and in the Program Files layout. Install the MSVC C++
    echo   toolset ^(Desktop development with C++^), or run this from a
    echo   Developer Command Prompt.
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
rem
rem The RVA is spelled out as eight hex digits instead of `[0-9A-Fa-f]*` on
rem purpose. A loose pattern also matches dumpbin's file-header line
rem     00000000 characteristics
rem ('c' is a hex digit, so the name group starts at the 'c' of
rem "characteristics"), which counted 3 names for a 2-export DLL and failed every
rem correct build. Measured on the CUDA template's bundle.
rem
rem Count the matching rows, one `set /a` per line (see TOOL SHADOWING above).
set "NAMES=0"
for /f "delims=" %%C in ('findstr /r /c:"^ *[0-9][0-9]* *[0-9][0-9]* *[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f] *[A-Za-z_?]" "%TMPEXP%"') do set /a NAMES+=1
if not "%NAMES%"=="2" (
  echo FAIL: dumpbin reports %NAMES% exported names, expected 2
  findstr /r /c:"^ *[0-9][0-9]* *[0-9][0-9]* *[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f] *[A-Za-z_?]" "%TMPEXP%"
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
rem One cubin per output line, shaped "ELF file    N: <name>.cubin".
rem
rem Counting every line was wrong in the other direction: a binary with no
rem device code makes cuobjdump print
rem   cuobjdump info    : File '...' does not contain device code
rem on stdout, which counted as "1 cubin(s)" - so a CPU_ONLY build passed the
rem --cuda check it should have failed (or, once build.bat stops asking for it,
rem printed a device-code line that was not true). Match the .cubin names only.
rem The count itself uses `set /a` rather than find (TOOL SHADOWING above).
set "CUBINS=0"
for /f "tokens=*" %%L in ('findstr /i /c:".cubin" "%TMPCUB%"') do set /a CUBINS+=1
del "%TMPCUB%" >nul 2>&1
if "%CUBINS%"=="0" (
  echo FAIL: --cuda was requested but the image contains no cubin
  exit /b 1
)
echo   device code: %CUBINS% cubin^(s^)
exit /b 0
