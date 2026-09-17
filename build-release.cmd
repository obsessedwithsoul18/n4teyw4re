@echo off
setlocal
cd /d "%~dp0"

echo [1/3] Locating Visual Studio...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
)
if not defined VSINSTALL (
  if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" set "VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\18\Community"
)
if not defined VSINSTALL (
  if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" set "VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
)
if not defined VSINSTALL (
  echo ERROR: Could not find a Visual Studio installation with C++ tools.
  exit /b 1
)

set "VSDEVCMD=%VSINSTALL%\Common7\Tools\VsDevCmd.bat"
echo [2/3] Initializing x64 MSVC environment...
call "%VSDEVCMD%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%

set CMAKE_GENERATOR_PLATFORM=
set CMAKE_GENERATOR_TOOLSET=

where cl >nul 2>nul || (echo ERROR: cl.exe was not found after x64 environment setup. & exit /b 1)
where ninja >nul 2>nul || (echo ERROR: ninja.exe is missing. Install C++ CMake tools for Windows in Visual Studio Installer. & exit /b 1)
where cmake >nul 2>nul || (echo ERROR: cmake.exe is missing. Install C++ CMake tools for Windows in Visual Studio Installer. & exit /b 1)

echo [3/3] Configuring and building Release x64...
if exist "build\release\CMakeCache.txt" del /q "build\release\CMakeCache.txt" >nul 2>nul
cmake --preset release
if errorlevel 1 exit /b %errorlevel%
cmake --build --preset release --parallel
if errorlevel 1 exit /b %errorlevel%

echo.
echo Build complete.
echo EXE: %CD%\build\release\bin\n4teyw4re.exe
endlocal
