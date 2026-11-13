@echo off
setlocal
pushd "%~dp0" || exit /b 1
if not exist build mkdir build
rc /nologo /fo build\templates.res templates.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /utf-8 /O2 /MT /W4 /Brepro /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /Fo:build\ main.cpp setup.cpp profile.cpp window_io.cpp workflow.cpp nested.cpp audio.cpp build\templates.res /link /Brepro /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:build\WoW-Nested.exe windowsapp.lib d3d11.lib dwmapi.lib windowscodecs.lib ole32.lib user32.lib gdi32.lib shell32.lib crypt32.lib advapi32.lib comdlg32.lib uuid.lib
set "buildExit=%errorlevel%"
popd
exit /b %buildExit%
