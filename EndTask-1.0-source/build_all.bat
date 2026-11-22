@echo off
setlocal
cd /d "%~dp0"

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [x] vcvars64.bat introuvable.
    exit /b 1
)
call "%VCVARS%" >nul || exit /b 1

call hook\build_hook.bat || exit /b 1
cd /d "%~dp0"

if not exist build mkdir build
if not exist dist mkdir dist

echo [*] building EndTaskHelper.exe
cl /nologo /std:c++17 /O2 /Oi /Gy /Gw /GL /DNDEBUG /EHsc /W4 /permissive- /utf-8 ^
   /DUNICODE /D_UNICODE /DNOMINMAX /Isrc ^
   /Fo:build\ /Fe:build\EndTaskHelper.exe ^
   helper\helper.cpp src\target.cpp src\util.cpp ^
   /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
   uiautomationcore.lib oleacc.lib ole32.lib oleaut32.lib uuid.lib shell32.lib shlwapi.lib advapi32.lib user32.lib dwmapi.lib propsys.lib
if errorlevel 1 exit /b 1

echo [*] building EndTaskManager.exe
cl /nologo /std:c++17 /O2 /Oi /Gy /Gw /GL /DNDEBUG /EHsc /W4 /permissive- /utf-8 ^
   /DUNICODE /D_UNICODE /DNOMINMAX ^
   /Fo:build\ /Fe:build\EndTaskManager.exe ^
   manager\manager.cpp ^
   /link /SUBSYSTEM:CONSOLE /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
   advapi32.lib shell32.lib
if errorlevel 1 exit /b 1

copy /y hook\build\EndTaskHook.dll dist\ >nul
copy /y build\EndTaskHelper.exe dist\ >nul
copy /y build\EndTaskManager.exe dist\ >nul
if exist README.md copy /y README.md dist\ >nul
if exist LICENSE copy /y LICENSE dist\ >nul
if exist install.bat copy /y install.bat dist\ >nul

echo.
echo [ok] dist\EndTaskManager.exe
echo [ok] dist\EndTaskHook.dll
echo [ok] dist\EndTaskHelper.exe
