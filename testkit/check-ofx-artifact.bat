@echo off
rem SPDX-License-Identifier: BSD-3-Clause
rem
rem Windows counterpart of the artifact checks in build.sh / build.sh + nm.
rem
rem   check-ofx-artifact.bat <plugin.ofx> [--cuda]
rem
rem dumpbin /exports      ~ nm -D --defined-only : exactly the two entry points
rem dumpbin /dependents   ~ readelf -d           : what must be found at load time
rem cuobjdump -lelf       ~ readelf -S | grep nv_fatbin : device code is present
rem
rem Exits 1 when an entry point is missing, when an unexpected name is exported,
rem or (with --cuda) when the image contains no cubin. Missing tools are a
rem printed skip, never a pass: run this from a Developer Command Prompt.
setlocal EnableExtensions
set "OFX=%~1"
if "%OFX%"=="" (
  echo usage: check-ofx-artifact.bat ^<plugin.ofx^> [--cuda]
  exit /b 2
)
if not exist "%OFX%" (
  echo error: no such file: %OFX%
  exit /b 1
)

where dumpbin >nul 2>&1
if errorlevel 1 (
  echo   artifact: skipped ^(dumpbin not on PATH; use a Developer Command Prompt^)
  goto :cuda
)

set "TMPEXP=%TEMP%\ofx-exports-%RANDOM%%RANDOM%.txt"
dumpbin /nologo /exports "%OFX%" > "%TMPEXP%" 2>nul
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
dumpbin /nologo /dependents "%OFX%" | findstr /i /c:".dll"

:cuda
if /I not "%~2"=="--cuda" exit /b 0
where cuobjdump >nul 2>&1
if errorlevel 1 (
  echo   device code: skipped ^(cuobjdump not on PATH^)
  exit /b 0
)
set "CUBINS=0"
for /f %%C in ('cuobjdump -lelf "%OFX%" 2^>nul ^| find /c /v ""') do set "CUBINS=%%C"
if "%CUBINS%"=="0" (
  echo FAIL: --cuda was requested but the image contains no cubin
  exit /b 1
)
echo   device code: %CUBINS% cubin^(s^)
exit /b 0
