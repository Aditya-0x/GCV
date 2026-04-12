@echo off
echo Building GC Server...
gcc -Wall -O2 gc_server.c -o gc_server.exe -lws2_32 -lpsapi
if %ERRORLEVEL% neq 0 (
    echo Build failed!
    pause
    exit /b %ERRORLEVEL%
)
echo Build succeeded!
echo Starting backend server...
gc_server.exe
