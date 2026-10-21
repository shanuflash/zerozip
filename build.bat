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

rc /nologo /fo build\zerozip.res src\zerozip.rc || exit /b 1
cl %CFLAGS% /Ithird_party\webview2\include /Fobuild\ src\main.cpp src\engine.cpp build\zerozip.res build\deps.lib ^
   third_party\webview2\x64\WebView2LoaderStatic.lib /Fe:ZeroZip.exe ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF user32.lib ole32.lib shell32.lib dwmapi.lib advapi32.lib ^
   version.lib gdi32.lib || exit /b 1
echo Built ZeroZip.exe
