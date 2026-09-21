@echo off
setlocal
if "%~1"=="" (
  echo usage: build_windows.cmd OUTPUT_EXE 1>&2
  exit /b 2
)
set "VCM_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VCM_VSWHERE%" (
  echo Visual Studio Build Tools with C++ support are required. 1>&2
  exit /b 2
)
for /f "usebackq delims=" %%I in (`"%VCM_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCM_VS=%%I"
if not defined VCM_VS (
  echo Visual Studio C++ toolchain not found. 1>&2
  exit /b 2
)
call "%VCM_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /guard:cf "%~dp0main.cpp" /Fe:"%~1" /link PortableDeviceGUIDs.lib ole32.lib oleaut32.lib
exit /b %errorlevel%
