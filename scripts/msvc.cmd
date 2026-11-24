@echo off
setlocal
rem Locate the newest Visual Studio or Build Tools with the x64 C++ toolset instead of a fixed install path.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novswhere
rem Read vswhere directly, without a temp file: two builds started in the same second (build-release runs DcsQvCull
rem and pupil shift side by side) got the same %RANDOM%, shared one file, and one of them found no toolset.
set "VCVARS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do if not defined VCVARS set "VCVARS=%%i"
if not defined VCVARS goto :notoolset
rem VsDevCmd runs a bare vswhere.exe from the Installer folder; with NoDefaultCurrentDirectoryInExePath set, cmd only
rem finds it on PATH ("'vswhere.exe' is not recognized" otherwise).
endlocal & set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer" & call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%
%*
exit /b %errorlevel%
:novswhere
echo vswhere.exe not found. Install Visual Studio Build Tools with the C++ x64 toolset. 1>&2
exit /b 1
:notoolset
echo No Visual Studio installation with the C++ x64 toolset was found. 1>&2
exit /b 1
