@echo off
rem Builds build\aimp_cast.dll (the plugin) and the test tools. Needs Visual Studio 2022+ (C++ x64).
setlocal
cd /d "%~dp0"
if not exist third_party\aimp_sdk\apiPlugin.h (
  echo The AIMP SDK headers are missing. Download the SDK from https://www.aimp.ru/?do=download^&os=windows^&cat=sdk
  echo and copy the C++ headers ^(Sources\Cpp\*.h^) into third_party\aimp_sdk\
  exit /b 1
)
where cl >nul 2>nul || call :vcvars || exit /b 1
if not exist build mkdir build
set CF=/nologo /O2 /EHsc /std:c++17 /utf-8 /W3 /MT /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS
cl %CF% /LD /wd4828 /Fobuild\ /Fe:build\aimp_cast.dll src\aimp_plugin.cpp src\engine.cpp src\stream.cpp src\cast.cpp user32.lib shell32.lib ole32.lib || exit /b 1
cl %CF% /LD /Fobuild\ /Fe:build\aimp_cast_dsp.dll src\dsp_forward.cpp || exit /b 1
cl %CF% /Fobuild\ /Fe:build\castctl.exe tools\castctl.cpp src\cast.cpp || exit /b 1
cl %CF% /Fobuild\ /Fe:build\dsptest.exe tools\dsptest.cpp || exit /b 1
echo BUILD OK
exit /b 0

:vcvars
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo Visual Studio C++ build tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b %errorlevel%
