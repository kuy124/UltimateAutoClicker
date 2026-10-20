@echo off
setlocal
cd /d "%~dp0"

echo [1/2] Compiling resources...
windres resource.rc -O coff -o resource.res
if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile resource.rc
    exit /b %errorlevel%
)

echo [2/2] Building UltimateAutoClicker.exe...
g++ core.cpp main.cpp resource.res -o UltimateAutoClicker.exe -mwindows -lcomctl32 -lshell32 -static -s -Os -fno-exceptions -fno-rtti -DUNICODE -D_UNICODE
if %errorlevel% neq 0 (
    echo [ERROR] Build failed!
    exit /b %errorlevel%
)

echo [SUCCESS] UltimateAutoClicker.exe built successfully!
