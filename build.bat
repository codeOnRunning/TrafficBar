@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo Failed to initialize MSVC environment.
  exit /b 1
)
if not exist build mkdir build
rc /nologo /fo build\app.res app.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /I src /Fo:build\ /Fd:build\TrafficBar.pdb src\*.cpp build\app.res /Fe:build\TrafficBar.exe /link /MANIFEST:NO /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib advapi32.lib iphlpapi.lib ole32.lib pdh.lib wtsapi32.lib winhttp.lib
if errorlevel 1 exit /b 1
echo Built build\TrafficBar.exe
