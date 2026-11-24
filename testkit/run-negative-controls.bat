@echo off
rem SPDX-License-Identifier: BSD-3-Clause
rem
rem Windows counterpart of run-negative-controls.sh: proves that contract_test
rem detects faults instead of merely passing.
rem
rem Fifteen controls are fed to the same harness the templates use. Two are
rem separate broken plug-ins (stub_bad_status.c answers describe with
rem ReplyDefault, stub_bad_type.c declares the wrong parameter type). The rest
rem are one stub built several ways: plain or internally tiled with the declared
rem context read or ignored, a global effect that declares no context at all, a
rem label that is never written and one that is, a mark in the four pointer
rem layouts, and a clip property that is re-read or cached.
rem
rem The script succeeds only if every broken build exits non-zero *and* names the
rem assertion, and every context-reading build passes. Those pairs are what make
rem --roi-context and --roi-context-tolerance self-verifying.
rem
rem What differs from the shell script, and why:
rem
rem   * The stubs are DLLs built by cl /LD /TC instead of cc -shared -fPIC.
rem     Windows has no default symbol export, so the two entry points are named
rem     with /link /EXPORT; a stub with no export would fail to load and every
rem     control would "pass" for the wrong reason.
rem   * The harness comes from build-contract-test.bat, same sources, loading
rem     with LoadLibraryExW instead of dlopen.
rem   * The varying source fixture (a constant image would make every window
rem     agree, so the comparison would mean nothing) is written by PowerShell
rem     with the same arithmetic as the C generator in the shell script, and its
rem     size is checked so a silent write failure cannot weaken a control.
rem
rem Nothing here inspects a binary - it builds plug-ins and runs them - so it
rem needs none of nm, readelf, cuobjdump or dumpbin. What it does NOT reproduce
rem is valgrind and the sanitizer builds, which the shell script does not run for
rem these controls either.
rem
rem This runs inside ../run-tests.sh on Windows, as the last stage before the
rem summary, because the shell counterpart cannot run there at all. It exits 0
rem when every fault is caught - the expected failures are asserted here, not
rem left for the caller to notice - so a green run stays green.
rem
rem   testkit\run-negative-controls.bat [path\to\openfx]
setlocal
set "HERE=%~dp0"
for %%I in ("%HERE%..") do set "ROOT=%%~fI"

set "OPENFX=%~1"
if not defined OPENFX set "OPENFX=%OFX_SOURCE_DIR%"
if not defined OPENFX set "OPENFX=%ROOT%\.ofx-ref"
set "INC=%OPENFX%\include"
if not exist "%INC%\ofxCore.h" if exist "%OPENFX%\ofxCore.h" set "INC=%OPENFX%"
if not exist "%INC%\ofxCore.h" goto :no_headers

set "TMPDIR=%TEMP%\ofx-negative-%RANDOM%%RANDOM%"
mkdir "%TMPDIR%" 2>nul || goto :no_temp
set "BIN=%TMPDIR%\contract_test.exe"
set "OUT=%TMPDIR%\out.txt"
set "FAILED=0"

echo == [0/4] MSVC environment ==
call :find_vcvars
if errorlevel 1 goto :no_msvc
call "%VCVARS%" >nul 2>&1 || goto :no_msvc

echo == [1/4] contract harness ==
call "%HERE%build-contract-test.bat" "%BIN%" "%OPENFX%" || goto :bad_harness

echo == [2/4] control plug-ins (cl /LD) ==
call :stub bad_status "" stub_bad_status.c || goto :bad_stub
call :stub bad_type "" stub_bad_type.c || goto :bad_stub
rem Same source, one decision apart: clamp the filter taps to the published
rem source bounds (the context the plug-in asked for) or to the window it was
rem told to render.
call :stub roi_ignored "/DIGNORE_CONTEXT" stub_roi_context.c || goto :bad_stub
call :stub roi_honoured "" stub_roi_context.c || goto :bad_stub
rem The same two decisions again, with the stub tiling internally: /DTILED splits
rem the window into 32-pixel cores, so dropping the context shows up at a
rem boundary the whole-frame render does not have. This is the pair behind
rem --roi-context-tolerance.
call :stub roi_tiled_ignored "/DTILED /DIGNORE_CONTEXT" stub_roi_context.c || goto :bad_stub
call :stub roi_tiled_ok "/DTILED" stub_roi_context.c || goto :bad_stub
rem The label control: the same stub declares the read-only `provider` label,
rem gives it a default, and never writes it.
call :stub roi_no_label "/DNO_LABEL" stub_roi_context.c || goto :bad_stub
call :stub roi_label_ok "/DWITH_LABEL" stub_roi_context.c || goto :bad_stub
rem The other half of --roi-context: a global effect answers with the region it
rem was asked about, so the harness must skip the "must differ" half.
call :stub roi_global "/DROI_SELF" stub_roi_context.c || goto :bad_stub
rem The clip-property pair: re-reading `mediaID` passes, caching it must fail.
call :stub clip_echo "/DCLIP_ECHO" stub_roi_context.c || goto :bad_stub
call :stub clip_stale "/DCLIP_ECHO_ONCE" stub_roi_context.c || goto :bad_stub
rem The drawing control: a box filter plus one mark at a fixed position, so the
rem four pointer-origin/stride layouts stop being a claim in a README.
call :stub roi_mark "/DDRAW_MARK" stub_roi_context.c || goto :bad_stub

echo == [3/4] varying source fixture ==
>  "%TMPDIR%\fixture.ps1" echo $w = New-Object IO.BinaryWriter([IO.File]::Create('%TMPDIR%\roi_source.bin'))
>> "%TMPDIR%\fixture.ps1" echo foreach ($c in 0..2) { foreach ($y in 0..47) { foreach ($x in 0..63) { $w.Write([single]((($x*7 + $y*13 + $c*29) %% 97) / 97.0)) } } }
>> "%TMPDIR%\fixture.ps1" echo $w.Close()
powershell -NoProfile -ExecutionPolicy Bypass -File "%TMPDIR%\fixture.ps1" >nul 2>&1 || goto :bad_fixture
set "FIXSIZE=0"
for %%I in ("%TMPDIR%\roi_source.bin") do set "FIXSIZE=%%~zI"
if not "%FIXSIZE%"=="36864" goto :bad_fixture
echo   3 x 48 x 64 float32 = %FIXSIZE% bytes

echo == [4/4] fifteen controls ==
set "SRC=%TMPDIR%\roi_source.bin"
set "ROI=--clip Source --clip Output --concurrency-threads 0 --roi-context --size 64x48 --src ""%SRC%"""
set "TILED=--clip Source --clip Output --concurrency-threads 0 --roi-context-tolerance 0.02 --size 64x48 --src ""%SRC%"""
set "LABEL=--clip Source --clip Output --concurrency-threads 0 --size 64x48 --src ""%SRC%"""
set "CLIP=--clip Source --clip Output --concurrency-threads 0 --size 64x48 --src ""%SRC%"" --label-param ""mediaId=take1"" --clip-prop mediaID=take1 --clip-prop-change mediaID=take2"
set "MARK=--clip Source --clip Output --concurrency-threads 0 --size 64x48 --src ""%SRC%"" --mark ""24,8"""

rem Each control sets ARGV to the harness command line and then names the title
rem and the assertion. Passing the command line as an argument does not survive:
rem batch expands only %1..%9 and %* ignores shift, so a long line loses its tail.
set ARGV="%TMPDIR%\bad_status.ofx" --id com.testkit.negative.badstatus --param gain:double
call :control fail "describe returns kOfxStatReplyDefault" "FAIL kOfxActionDescribe: kOfxStatReplyDefault"

set ARGV="%TMPDIR%\bad_type.ofx" --id com.testkit.negative.badtype --param gain:double
call :control fail "parameter declared with the wrong type" "FAIL param 'gain' type: OfxParamTypeInteger (expected OfxParamTypeDouble)"

set ARGV="%TMPDIR%\roi_ignored.ofx" --id com.testkit.negative.roiignored %ROI%
call :control fail "declared context region ignored by render" "FAIL roi context: the inset window matches the whole frame bit for bit"

set ARGV="%TMPDIR%\roi_honoured.ofx" --id com.testkit.control.roiok %ROI%
call :control pass "declared context region honoured by render" ""

set ARGV="%TMPDIR%\roi_global.ofx" --id com.testkit.control.globalroi %ROI%
call :control mention "global effect declares no context" "second half skipped - the plug-in declares no context region"

set ARGV="%TMPDIR%\roi_tiled_ignored.ofx" --id com.testkit.negative.roiignoredtiled %TILED%
call :control fail "internally tiled render ignores the declared context" "FAIL roi context: the inset window stays within the tolerance of the whole frame"

set ARGV="%TMPDIR%\roi_tiled_ok.ofx" --id com.testkit.control.roioktiled %TILED%
call :control pass "internally tiled render honours the declared context" ""

set ARGV="%TMPDIR%\roi_no_label.ofx" --id com.testkit.negative.nolabel --label-param provider %LABEL%
call :control fail "label parameter declared and never written" "FAIL label parameter 'provider' = "

set ARGV="%TMPDIR%\roi_label_ok.ofx" --id com.testkit.control.labelok --label-param provider %LABEL%
call :control pass "label parameter written by the plug-in" ""

set ARGV="%TMPDIR%\clip_echo.ofx" --id com.testkit.control.clipecho %CLIP%
call :control pass "clip property change reaches a plug-in that re-reads it" ""

set ARGV="%TMPDIR%\clip_stale.ofx" --id com.testkit.negative.clipstale %CLIP%
call :control fail "plug-in caches the clip property on the handle" "FAIL label parameter 'mediaId' follows the clip property"

set ARGV="%TMPDIR%\roi_mark.ofx" --id com.testkit.control.mark %MARK%
call :control pass "mark drawn in a bottom-up frame arrives where the frame says" ""

set ARGV="%TMPDIR%\roi_mark.ofx" --id com.testkit.control.mark --negative-pitch %MARK%
call :control pass "mark drawn with a bottom-up, negative-pitch frame" ""

set ARGV="%TMPDIR%\roi_mark.ofx" --id com.testkit.control.mark --image-origin top %MARK%
call :control fail "mark drawn into a top-down frame is mirrored" "FAIL overlay mark at (24,8)"

set ARGV="%TMPDIR%\roi_mark.ofx" --id com.testkit.control.mark --image-origin top --negative-pitch %MARK%
call :control fail "mark drawn into a top-down, negative-pitch frame is mirrored" "FAIL overlay mark at (24,8)"

echo.
rmdir /s /q "%TMPDIR%" >nul 2>&1
if not "%FAILED%"=="0" goto :controls_failed
echo negative controls: every fault detected, the pattern still passes, harness is not a rubber stamp
exit /b 0

rem ---------------------------------------------------------------------------
rem Subroutines. Goto style, not parenthesised blocks: some arguments contain
rem ')' inside quotes, which a block would close early.
rem ---------------------------------------------------------------------------

:find_vcvars
set "VCVARS="
set "VSINSTALL="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do if not defined VSINSTALL set "VSINSTALL=%%I"
if defined VSINSTALL if exist "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if defined VCVARS exit /b 0
for %%D in ("%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools" "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools" "%ProgramFiles%\Microsoft Visual Studio\2022\Community" "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools") do if exist "%%~D\VC\Auxiliary\Build\vcvars64.bat" if not defined VCVARS set "VCVARS=%%~D\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS exit /b 1
exit /b 0

:stub
rem %1 out name, %2 extra defines, %3 source file in negative-controls\
pushd "%TMPDIR%"
cl /nologo /LD /TC /std:c11 /W3 /D_CRT_SECURE_NO_WARNINGS %~2 /I "%INC%" "%HERE%negative-controls\%~3" /Fe:"%TMPDIR%\%~1.ofx" /Fo:"%TMPDIR%\%~1.obj" /link /EXPORT:OfxGetNumberOfPlugins /EXPORT:OfxGetPlugin /IMPLIB:"%TMPDIR%\%~1.lib" >nul
set "ST=%ERRORLEVEL%"
popd
if not "%ST%"=="0" echo error: %~1.ofx failed to compile
if not "%ST%"=="0" exit /b 1
exit /b 0

:control
rem %1 fail|pass|mention, %2 title, %3 needle; ARGV is set by the caller
set "MODE=%~1"
set "TITLE=%~2"
set "NEEDLE=%~3"
echo.
echo --- negative control: %TITLE% ---
"%BIN%" %ARGV% > "%OUT%" 2>&1
set "STATUS=%ERRORLEVEL%"
type "%OUT%"
if /i "%MODE%"=="fail" goto :c_must_fail
if /i "%MODE%"=="mention" goto :c_must_mention
if /i "%MODE%"=="pass" goto :c_pass_check
echo   CONTROL FAILED: unknown mode "%MODE%" - expected fail, pass or mention >&2
set "FAILED=1"
exit /b 0

:c_pass_check
if not "%STATUS%"=="0" goto :c_false_red
echo   passed, as a plug-in following the pattern must
exit /b 0

:c_must_fail
if "%STATUS%"=="0" goto :c_failed_to_fail
findstr /L /C:"%NEEDLE%" "%OUT%" >nul 2>&1
if errorlevel 1 goto :c_wrong_reason
echo   caught (exit %STATUS%) with: %NEEDLE%
exit /b 0

:c_must_mention
if not "%STATUS%"=="0" goto :c_false_red
findstr /L /C:"%NEEDLE%" "%OUT%" >nul 2>&1
if errorlevel 1 goto :c_not_mentioned
echo   passed and named: %NEEDLE%
exit /b 0

:c_failed_to_fail
echo   CONTROL FAILED TO FAIL: contract_test exited 0 >&2
set "FAILED=1"
exit /b 0

:c_wrong_reason
echo   CONTROL FAILED: output does not name '%NEEDLE%' >&2
set "FAILED=1"
exit /b 0

:c_false_red
echo   CONTROL FAILED: the harness rejected a plug-in that follows the pattern >&2
set "FAILED=1"
exit /b 0

:c_not_mentioned
echo   CONTROL FAILED: passed without naming '%NEEDLE%' >&2
set "FAILED=1"
exit /b 0

:no_headers
echo error: "%INC%\ofxCore.h" not found; pass the openfx checkout path >&2
exit /b 2

:no_temp
echo error: cannot create a temporary directory under "%TEMP%" >&2
exit /b 2

:no_msvc
echo error: no vcvars64.bat with a C++ toolset found; run from a Developer Command Prompt >&2
call :cleanup
exit /b 2

:bad_harness
echo error: the contract harness failed to build >&2
call :cleanup
exit /b 2

:cleanup
rem Every failure path calls this: without it a broken setup - no compiler, no
rem harness - left the DLLs, the harness and the fixture in %TEMP% for good,
rem while a run that reached the controls removed them.
if defined TMPDIR if exist "%TMPDIR%" rmdir /s /q "%TMPDIR%" >nul 2>&1
exit /b 0

:bad_stub
call :cleanup
exit /b 2

:bad_fixture
echo error: the varying source fixture was not written (expected 36864 bytes) >&2
call :cleanup
exit /b 2

:controls_failed
echo negative controls FAILED: the harness does not detect the faults above >&2
exit /b 1
