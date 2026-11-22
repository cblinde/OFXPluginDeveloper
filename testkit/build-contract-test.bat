@echo off
rem ---------------------------------------------------------------------------
rem Windows counterpart of build-contract-test.sh: compiles the shared contract
rem harness (fake_host.cpp + contract_main.cpp) into one binary with MSVC.
rem
rem   build-contract-test.bat <out-binary> <ofx-include-dir>
rem
rem <ofx-include-dir> is the directory that contains ofxCore.h, i.e. the
rem "include" subdirectory of an openfx checkout (the templates pass the value
rem they read from build\CMakeCache.txt).
rem
rem The harness itself is identical to the one the Linux scripts build; only the
rem loader differs (LoadLibraryExW vs dlopen), which is why it belongs in that
rem translation unit rather than in a second copy of the checks.
rem ---------------------------------------------------------------------------
setlocal
set "HERE=%~dp0"
set "OUT=%~1"
set "INC=%~2"

if "%OUT%"=="" (
  echo usage: build-contract-test.bat ^<out-binary^> ^<ofx-include-dir^>
  exit /b 2
)
if "%INC%"=="" (
  echo error: pass the openfx checkout directory that contains ofxCore.h
  exit /b 2
)
rem Accept either the checkout root or its include directory.
if not exist "%INC%\ofxCore.h" if exist "%INC%\include\ofxCore.h" set "INC=%INC%\include"
if not exist "%INC%\ofxCore.h" (
  echo error: "%INC%\ofxCore.h" not found
  exit /b 2
)

rem ---- MSVC environment ------------------------------------------------------
rem vswhere finds any edition or version that actually has the C++ toolset;
rem hard-coded "Visual Studio 1x" paths are how this family of scripts first
rem broke on a machine whose only C++ toolset was VS 18.
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
)
if defined VSINSTALL if exist "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
for /d %%D in ("%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools" "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools" "%ProgramFiles%\Microsoft Visual Studio\2022\Community" "%ProgramFiles%\Microsoft Visual Studio\2022\Professional" "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise" "%ProgramFiles%\Microsoft Visual Studio\2019\Community" "%ProgramFiles%\Microsoft Visual Studio\2019\BuildTools") do (
  if exist "%%~D\VC\Auxiliary\Build\vcvars64.bat" if not defined VCVARS set "VCVARS=%%~D\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
  echo error: no vcvars64.bat found; run from a Developer Command Prompt
  exit /b 2
)
call "%VCVARS%" >nul || exit /b 2

cl /nologo /std:c++17 /O2 /EHsc /W3 /I "%HERE%." /I "%INC%" ^
  "%HERE%fake_host.cpp" "%HERE%contract_main.cpp" ^
  /Fe:"%OUT%" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 (
  echo error: contract harness failed to compile
  exit /b 1
)
exit /b 0
