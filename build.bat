@echo off
setlocal
cd /d "%~dp0"

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo MSVC build tools not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1

if not exist build mkdir build

rem Third-party code changes rarely, so it is compiled once and reused.
if not exist build\deps.lib (
    if not exist build\deps mkdir build\deps
    cl /nologo /c /O2 /MT /W0 /DZSTD_DISABLE_ASM /DZSTD_LEGACY_SUPPORT=0 /DZSTD_STRIP_ERROR_STRINGS=0 ^
       third_party\zstd\common\*.c third_party\zstd\decompress\*.c /Fobuild\deps\ || exit /b 1
    cl /nologo /c /O2 /MT /W0 /DMINIZ_NO_STDIO /DMINIZ_NO_TIME /DMINIZ_NO_ARCHIVE_APIS /DMINIZ_NO_DEFLATE_APIS ^
       third_party\miniz\miniz.c /Fobuild\deps\ || exit /b 1
    lib /nologo /out:build\deps.lib build\deps\*.obj || exit /b 1
    rmdir /s /q build\deps
)

set CFLAGS=/nologo /utf-8 /std:c++17 /O2 /MT /W4 /EHsc /DUNICODE /D_UNICODE /DMINIZ_NO_STDIO /DMINIZ_NO_TIME /DMINIZ_NO_ARCHIVE_APIS /DMINIZ_NO_DEFLATE_APIS

if "%1"=="test" (
    cl %CFLAGS% /Fobuild\ src\engine.cpp tests\zztest.cpp build\deps.lib /Fe:build\zztest.exe ^
       /link /SUBSYSTEM:CONSOLE user32.lib || exit /b 1
    echo Built build\zztest.exe
    exit /b 0
)

rem The Windows 11 right-click entry: a small DLL Explorer loads, and the package that registers
rem it. Both are built into ZeroZip.exe, which installs them when the menu is turned on.
cl %CFLAGS% /LD /Fobuild\ src\shell.cpp /Fe:build\ZeroZipShell.dll ^
   /link /DEF:src\shell.def /OPT:REF shell32.lib shlwapi.lib ole32.lib advapi32.lib || exit /b 1
if exist build\package rmdir /s /q build\package
mkdir build\package
copy /y assets\package\* build\package >nul || exit /b 1
"%WindowsSdkVerBinPath%x64\makeappx.exe" pack /d build\package /p build\ZeroZip.msix /nv /o >nul || exit /b 1

rc /nologo /fo build\zerozip.res src\zerozip.rc || exit /b 1
cl %CFLAGS% /Fobuild\ src\main.cpp src\gfx.cpp src\engine.cpp build\zerozip.res build\deps.lib /Fe:ZeroZip.exe ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF user32.lib ole32.lib shell32.lib shlwapi.lib dwmapi.lib advapi32.lib ^
   d2d1.lib dwrite.lib kernel32.lib || exit /b 1
echo Built ZeroZip.exe
