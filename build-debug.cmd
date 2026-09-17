@echo off
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
)
if not defined VSINSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" set "VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\18\Community"
if not defined VSINSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" set "VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not defined VSINSTALL (echo ERROR: Visual Studio C++ tools not found. & exit /b 1)
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
set CMAKE_GENERATOR_PLATFORM=
set CMAKE_GENERATOR_TOOLSET=
if exist "build\debug\CMakeCache.txt" del /q "build\debug\CMakeCache.txt" >nul 2>nul
cmake --preset debug
if errorlevel 1 exit /b %errorlevel%
cmake --build --preset debug --parallel
if errorlevel 1 exit /b %errorlevel%
echo.
echo Build complete.
echo EXE: %CD%\build\debug\bin\n4teyw4re.exe
endlocal
