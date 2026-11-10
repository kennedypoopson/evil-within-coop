@echo off
setlocal
set "PATH=C:\msys64\ucrt64\bin;%PATH%"

set "MODE=%~1"
if "%MODE%"=="" set "MODE=release"

if /i "%MODE%"=="release" goto build_release
if /i "%MODE%"=="debug" goto build_debug
exit /b 1

:build_debug
if not exist "build\debug\int\build.ninja" (
    echo [CMake]
    cmake -B build\debug\int -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
    if errorlevel 1 exit /b 1
)
echo [Debug]
cmake --build build\debug\int
exit /b %errorlevel%

:build_release
if not exist "build\release\int\build.ninja" (
    echo [CMake]
    cmake -B build\release\int -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
    if errorlevel 1 exit /b 1
)
echo [Release]
cmake --build build\release\int
exit /b %errorlevel%
