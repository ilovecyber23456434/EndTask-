@echo off
setlocal
cd /d "%~dp0"

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [x] vcvars64.bat not found
    exit /b 1
)
call "%VCVARS%" >nul || exit /b 1

if not exist build mkdir build

echo [*] building EndTaskHook.dll
cl /nologo /LD /std:c++17 /O2 /Oi /Gy /Gw /GL /DNDEBUG /EHsc /W4 /permissive- /utf-8 ^
   /DUNICODE /D_UNICODE /DNOMINMAX ^
   /Fo:build\ /Fe:build\EndTaskHook.dll ^
   hookdll.cpp ^
   /link /SUBSYSTEM:WINDOWS /DLL /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO ^
   WindowsApp.lib comctl32.lib ole32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1

echo [*] building EndTaskInject.exe
cl /nologo /std:c++17 /O2 /Oi /Gy /DNDEBUG /EHsc /W4 /permissive- /utf-8 ^
   /DUNICODE /D_UNICODE /DNOMINMAX ^
   /Fo:build\ /Fe:build\EndTaskInject.exe ^
   inject.cpp ^
   /link /SUBSYSTEM:CONSOLE /OPT:REF /OPT:ICF advapi32.lib
if errorlevel 1 exit /b 1

echo.
echo [ok] build\EndTaskHook.dll
echo [ok] build\EndTaskInject.exe
