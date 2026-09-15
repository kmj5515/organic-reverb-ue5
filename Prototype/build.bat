@echo off
rem ---------------------------------------------------------
rem AcousticCore prototype: build + run tests (no Unreal needed)
rem Usage: build.bat [test name filter]
rem ---------------------------------------------------------
setlocal
set "ROOT=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if not exist "%ROOT%bin" mkdir "%ROOT%bin"

rem vswhere result goes through a file (for /f + quoted path with "(x86)" is fragile)
"%VSWHERE%" -latest -products * -property installationPath > "%ROOT%bin\vspath.txt" 2>nul
set /p VSPATH=<"%ROOT%bin\vspath.txt"
if not defined VSPATH (
	echo [build] Visual Studio not found
	exit /b 1
)

rem VsDevCmd.bat calls vswhere.exe by bare name, so put the Installer dir on PATH
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cl /nologo /std:c++17 /O2 /EHsc /utf-8 /W4 /permissive- ^
	/I "%ROOT%..\AcousticCore\include" ^
	/Fo"%ROOT%bin\\" /Fe"%ROOT%bin\PrototypeTests.exe" ^
	"%ROOT%Main.cpp" "%ROOT%Tests\*.cpp" || exit /b 1

"%ROOT%bin\PrototypeTests.exe" %*
