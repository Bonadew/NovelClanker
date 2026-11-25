@echo off
rem Builds NovelClanker on Windows and puts a ready-to-run copy in dist\:
rem double-click dist\NovelClanker.exe (or copy the whole dist folder to another PC).
rem
rem   build.bat          dependencies (first run: can take an hour), build, dist\
rem   build.bat test     the same, and runs the tests before making dist\
rem
rem Needs: Visual Studio 2026 Build Tools (C++ workload), CMake 3.23+, Python with Conan 2 (pip install conan).
setlocal EnableExtensions
cd /d "%~dp0"

set "RUN_TESTS="
if /i "%~1"=="test" set "RUN_TESTS=1"

rem --- Tools ----------------------------------------------------------------------------------------------
where conan >nul 2>&1 || (
    echo ERROR: Conan 2 was not found. Install it with:  pip install conan
    goto :fail
)
where cmake >nul 2>&1 || (
    echo ERROR: CMake was not found. Install CMake 3.23 or later and add it to PATH.
    goto :fail
)

rem Several dependency build tools break when Conan's package storage path contains a space
rem (e.g. C:\Users\First Last\.conan2). Then a storage path without spaces must be configured first.
for /f "delims=" %%H in ('conan config home') do set "CONAN_HOME=%%H"
if not "%CONAN_HOME%"=="%CONAN_HOME: =%" (
    findstr /b /c:"core.cache:storage_path" "%CONAN_HOME%\global.conf" >nul 2>&1 || (
        echo ERROR: Conan's folder "%CONAN_HOME%" contains a space, which breaks some dependency builds.
        echo Add a line like this to "%CONAN_HOME%\global.conf" and run build.bat again:
        echo.
        echo     core.cache:storage_path = C:/conan2/p
        goto :fail
    )
)

rem The executable can't be replaced while it runs.
tasklist /fi "imagename eq NovelClanker.exe" 2>nul | "%SystemRoot%\System32\find.exe" /i "NovelClanker.exe" >nul && (
    echo ERROR: NovelClanker is running. Close it and run build.bat again.
    goto :fail
)

rem --- Dependencies (Qt, RocksDB), built once and cached by Conan -----------------------------------------
echo.
echo === Installing dependencies (the first run builds Qt and RocksDB and can take an hour) ===
conan install . -pr:a profiles\windows-msvc --build=missing || goto :fail

rem --- Build ----------------------------------------------------------------------------------------------
rem Ninja needs the MSVC environment (vcvars); Conan's script activates it.
call build\Release\generators\conanbuild.bat >nul || goto :fail
if not defined VCToolsInstallDir (
    echo ERROR: The MSVC environment could not be activated. Install Visual Studio 2026 Build Tools with the
    echo "Desktop development with C++" workload.
    goto :fail
)
rem Name MSVC's linker and librarian explicitly: with MSYS2 or MinGW on PATH, CMake can otherwise pick GNU ld/ar.
set "MSVC_BIN=%VCToolsInstallDir%bin\Hostx64\x64"

echo.
echo === Configuring ===
cmake --preset conan-release "-DCMAKE_LINKER:FILEPATH=%MSVC_BIN%\link.exe" "-DCMAKE_AR:FILEPATH=%MSVC_BIN%\lib.exe" || goto :fail

echo.
echo === Building ===
cmake --build --preset conan-release || goto :fail

if defined RUN_TESTS (
    echo.
    echo === Testing ===
    ctest --test-dir build\Release --output-on-failure || goto :fail
)

rem --- Self-contained copy: the app, Qt and the MSVC runtime ----------------------------------------------
echo.
echo === Creating dist\ ===
if exist dist rmdir /s /q dist
cmake --install build\Release --prefix dist || goto :fail
if not exist dist\NovelClanker.exe (
    echo ERROR: dist\NovelClanker.exe was not created.
    goto :fail
)

echo.
echo Done. Double-click dist\NovelClanker.exe to start NovelClanker.
echo The dist folder is self-contained: copy all of it to run NovelClanker on another PC.
call :pause_if_double_clicked
exit /b 0

:fail
echo.
echo Build failed.
call :pause_if_double_clicked
exit /b 1

rem Keep the window open when build.bat was started from Explorer, so the result can be read.
:pause_if_double_clicked
echo %CMDCMDLINE% | "%SystemRoot%\System32\find.exe" /i "%~nx0" >nul && echo %CMDCMDLINE% | "%SystemRoot%\System32\find.exe" /i "/c" >nul && pause
exit /b 0
