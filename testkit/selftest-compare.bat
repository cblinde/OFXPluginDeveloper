@echo off
rem SPDX-License-Identifier: BSD-3-Clause
rem ---------------------------------------------------------------------------
rem Windows counterpart of selftest-compare.sh: runs the golden comparator's
rem truth table against the same function the suites call, so the comparison is
rem shown passing what it must pass and failing what it must fail (a reference
rem corrupt at one pixel, a size mismatch, NaN, a deviation over the bound).
rem
rem   testkit\selftest-compare.bat
rem   set OFX_SOURCE_DIR=<openfx checkout>   when .ofx-ref is not there
rem ---------------------------------------------------------------------------
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "OFXDIR=%OFX_SOURCE_DIR%"
if not defined OFXDIR set "OFXDIR=%ROOT%\.ofx-ref"
set "TMP=%TEMP%\ofx-selftest-compare"

if not exist "%OFXDIR%\include\ofxCore.h" if not exist "%OFXDIR%\ofxCore.h" (
  echo error: no OpenFX checkout at "%OFXDIR%"; set OFX_SOURCE_DIR
  exit /b 2
)

if exist "%TMP%" rmdir /s /q "%TMP%"
mkdir "%TMP%\lookup" 2>nul

echo == comparator self-test ^(contract_test --selftest-compare^)
call "%HERE%build-contract-test.bat" "%TMP%\contract_test.exe" "%OFXDIR%"
if errorlevel 1 exit /b 2

"%TMP%\contract_test.exe" --selftest-compare "%TMP%\lookup"
if errorlevel 1 (
  echo TESTS FAILED: the comparator accepted something it has to reject
  exit /b 1
)

echo every case behaved: the comparison passes what it must and fails what it must
exit /b 0
