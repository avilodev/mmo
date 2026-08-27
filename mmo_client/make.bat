@echo off
rem Build the client from a plain Windows shell.
rem
rem The tree builds under MSYS2/MinGW64: the Launcher is Win32 and the Game
rem links the vendored GLFW, so gcc, pkg-config and mingw32-make all have to be
rem the MinGW64 ones. Rather than require that PATH is already set up, this
rem finds MSYS2 and puts its two bin directories in front of PATH for the build
rem only -- setlocal keeps that out of the calling shell.
rem
rem Both directories are needed, in this order:
rem
rem   mingw64\bin  gcc, pkg-config, mingw32-make -- the toolchain that produces
rem                native Windows binaries, and must win over any other gcc.
rem   usr\bin      sh, rm, mkdir, cp -- the Makefile's recipes are Unix ones.
rem                Without sh on PATH, mingw32-make runs recipes through
rem                cmd.exe instead, where "mkdir -p" fails with "The syntax of
rem                the command is incorrect" before anything is compiled.
rem
rem Usage from cmd.exe, in this directory:  make            (builds everything)
rem                                          make clean
rem                                          make game
rem Every argument is passed straight through to mingw32-make.

setlocal

set "MSYS2_HOME="
if defined MSYS2_ROOT if exist "%MSYS2_ROOT%\mingw64\bin\mingw32-make.exe" set "MSYS2_HOME=%MSYS2_ROOT%"
if not defined MSYS2_HOME if exist "C:\msys64\mingw64\bin\mingw32-make.exe" set "MSYS2_HOME=C:\msys64"
if not defined MSYS2_HOME if exist "%SystemDrive%\msys64\mingw64\bin\mingw32-make.exe" set "MSYS2_HOME=%SystemDrive%\msys64"
if not defined MSYS2_HOME if exist "%USERPROFILE%\msys64\mingw64\bin\mingw32-make.exe" set "MSYS2_HOME=%USERPROFILE%\msys64"

if defined MSYS2_HOME set "PATH=%MSYS2_HOME%\mingw64\bin;%MSYS2_HOME%\usr\bin;%PATH%"

where mingw32-make >nul 2>&1
if errorlevel 1 goto :no_toolchain
where sh >nul 2>&1
if errorlevel 1 goto :no_shell

mingw32-make %*
exit /b %ERRORLEVEL%

:no_toolchain
echo ERROR: mingw32-make was not found.
echo.
echo The client builds with the MSYS2 MinGW64 toolchain. Install MSYS2, then
echo from an MSYS2 shell:
echo.
echo     pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-openssl
echo.
echo If MSYS2 lives somewhere other than C:\msys64, point MSYS2_ROOT at it:
echo.
echo     set MSYS2_ROOT=D:\msys64
echo.
exit /b 1

:no_shell
echo ERROR: mingw32-make was found but the MSYS2 Unix tools were not.
echo.
echo The Makefile's recipes use sh, rm, mkdir and cp. Without them on PATH the
echo build fails on the first recipe with "The syntax of the command is
echo incorrect". Expected them in %MSYS2_HOME%\usr\bin.
echo.
exit /b 1
