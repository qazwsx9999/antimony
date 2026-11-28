@echo off
setlocal

set "COMPILER=%CC%"
if defined COMPILER goto :build

where gcc >nul 2>nul
if %errorlevel%==0 (
    set "COMPILER=gcc"
    goto :build
)

if exist "C:\Qt\Tools\mingw1310_64\bin\gcc.exe" (
    set "COMPILER=C:\Qt\Tools\mingw1310_64\bin\gcc.exe"
    goto :build
)
if exist "C:\msys64\mingw64\bin\gcc.exe" (
    set "COMPILER=C:\msys64\mingw64\bin\gcc.exe"
    goto :build
)
if exist "C:\msys64\ucrt64\bin\gcc.exe" (
    set "COMPILER=C:\msys64\ucrt64\bin\gcc.exe"
    goto :build
)

where clang >nul 2>nul
if %errorlevel%==0 (
    set "COMPILER=clang"
    goto :build
)

echo error: no C11 compiler found ^(install MinGW-w64 gcc or Clang, or set CC^). 1>&2
exit /b 1

:build
"%COMPILER%" -std=c11 -O2 -Wall -Wextra ^
    "%~dp0src\util.c" ^
    "%~dp0src\lexer.c" ^
    "%~dp0src\parser.c" ^
    "%~dp0src\check.c" ^
    "%~dp0src\x64.c" ^
    "%~dp0src\codegen.c" ^
    "%~dp0src\pe.c" ^
    "%~dp0src\llvm_c.c" ^
    "%~dp0src\fmt.c" ^
    "%~dp0src\lsp.c" ^
    "%~dp0src\main.c" ^
    -o "%~dp0sbc.exe"
exit /b %errorlevel%
