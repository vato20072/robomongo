@echo off 
setlocal enableextensions enabledelayedexpansion

rem Path to bin and project folder
set BIN_DIR_WITH_BACKSLASH=%~dp0%
set BIN_DIR=%BIN_DIR_WITH_BACKSLASH:~0,-1%
set PROJECT_DIR=%BIN_DIR%\..

rem Run common setup code
call "%BIN_DIR%\common\setup.bat" %*
if %ERRORLEVEL% neq 0 (exit /b 1)

rem Allow overriding CMake generator/platform/toolset via environment, e.g.
rem for CI with newer Visual Studio using the v141 (VS2017) toolset:
rem   set ROBO_CMAKE_GENERATOR=Visual Studio 16 2019
rem   set ROBO_CMAKE_PLATFORM=x64
rem   set ROBO_CMAKE_TOOLSET=v141
if "%ROBO_CMAKE_GENERATOR%"=="" set ROBO_CMAKE_GENERATOR=Visual Studio 15 2017 Win64
set GENERATOR_ARGS=
if not "%ROBO_CMAKE_PLATFORM%"=="" set GENERATOR_ARGS=%GENERATOR_ARGS% -A %ROBO_CMAKE_PLATFORM%
if not "%ROBO_CMAKE_TOOLSET%"=="" set GENERATOR_ARGS=%GENERATOR_ARGS% -T %ROBO_CMAKE_TOOLSET%

rem Run CMake configuration step
rem BUILD_TYPE: Release or Debug
cd "%BUILD_DIR%"
cmake -G "%ROBO_CMAKE_GENERATOR%"%GENERATOR_ARGS% -D "CMAKE_PREFIX_PATH=%PREFIX_PATH%" -D "CMAKE_BUILD_TYPE=%BUILD_TYPE%" -D "CMAKE_INSTALL_PREFIX=%INSTALL_PREFIX%" %PROJECT_DIR%

@REM echo ___________________________________________________________________
@REM rem Enable Clang Tidy for Visual Studio 2019 IDE ...
@REM set ROBO_PROJ_FILE=%BUILD_DIR%/src/robomongo/robomongo.vcxproj
@REM python %BIN_DIR%\enable-visual-studio-clang-tidy.py %ROBO_PROJ_FILE% %BIN_DIR%