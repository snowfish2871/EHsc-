@echo off
rem ===========================================================================
rem  EHsc build script (ASCII only so cmd.exe parses it safely)
rem  Locates Visual Studio, imports the x64 build environment, configures with
rem  CMake + Ninja (fallback: NMake/MSBuild generator) and builds Release.
rem ===========================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] vswhere.exe not found. Install Visual Studio with the C++ workload.
  exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if "%VSPATH%"=="" (
  echo [ERROR] Visual Studio C++ toolset not found.
  exit /b 1
)
echo [1/4] Visual Studio: %VSPATH%
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [ERROR] Failed to initialise the MSVC environment.
  exit /b 1
)

rem ---- locate cmake ---------------------------------------------------------
set "CMAKE=cmake"
where cmake >nul 2>nul
if errorlevel 1 (
  set "CMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  if not exist "!CMAKE!" (
    echo [ERROR] cmake not found in PATH nor in the Visual Studio installation.
    exit /b 1
  )
)
echo [2/4] CMake: !CMAKE!

rem ---- locate ninja (optional) ---------------------------------------------
set "NINJA=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "GENERATOR="
if exist "!NINJA!" (
  set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
  set "GENERATOR=-G Ninja"
)

echo [3/4] Configuring...
if defined GENERATOR (
  "!CMAKE!" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
) else (
  "!CMAKE!" -S . -B build -DCMAKE_BUILD_TYPE=Release
)
if errorlevel 1 exit /b 1

echo [4/4] Building...
"!CMAKE!" --build build --config Release
if errorlevel 1 exit /b 1

echo.
echo ============================================================
echo  Build finished:  build\bin\EHsc.exe
echo  Run self test :  build\bin\EHsc.exe selftest
echo  Interactive   :  build\bin\EHsc.exe
echo ============================================================
exit /b 0
