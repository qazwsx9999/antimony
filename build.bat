@echo off
C:\Qt\Tools\mingw1310_64\bin\gcc.exe -std=c11 -O2 -Wall -Wextra ^
    %~dp0src\util.c ^
    %~dp0src\lexer.c ^
    %~dp0src\parser.c ^
    %~dp0src\check.c ^
    %~dp0src\x64.c ^
    %~dp0src\codegen.c ^
    %~dp0src\pe.c ^
    %~dp0src\llvm_c.c ^
    %~dp0src\main.c ^
    -o %~dp0sbc.exe
