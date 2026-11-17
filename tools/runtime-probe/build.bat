@echo off
rem Builds the runtime probe into tools\runtime-probe\bin. Needs the MSVC build tools; OpenXR headers come from the OFXR fork's copy.
setlocal
rem Re-enters itself through scripts\msvc.cmd, which finds the installed C++ toolset.
if not defined VSCMD_VER ( call "%~dp0..\..\scripts\msvc.cmd" "%~f0" & exit /b )
cd /d "%~dp0"
if not exist bin mkdir bin
set INC=..\..\external\ofxr-djules75\external\OpenXR-SDK\include
cl /nologo /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I"%INC%" probe_run.cpp /Fo:bin\ /Fe:bin\probe_run.exe d3d11.lib d3d12.lib dxgi.lib dxguid.lib ole32.lib windowscodecs.lib || exit /b 1
cl /nologo /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS probe_analyze.cpp /Fo:bin\ /Fe:bin\probe_analyze.exe ole32.lib windowscodecs.lib || exit /b 1
cl /nologo /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS pimax_ctl.cpp /Fo:bin\ /Fe:bin\pimax_ctl.exe || exit /b 1
cl /nologo /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS pvr_cfg.cpp /Fo:bin\ /Fe:bin\pvr_cfg.exe || exit /b 1
echo built all
