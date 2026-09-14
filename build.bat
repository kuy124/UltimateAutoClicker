@echo off
setlocal
cd /d "%~dp0"

echo [1/2] Compiling resources...
windres resource.rc -O coff -o resource.res
if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile resource.rc
    pause
    exit /b %errorlevel%
)

echo [2/2] Building UltimateAutoClicker.exe...
g++ main.cpp resource.res -o UltimateAutoClicker.exe -mwindows -lcomctl32 -static -s -Os -fno-exceptions -fno-rtti
if %errorlevel% neq 0 (
    echo [ERROR] Build failed!
    pause
    exit /b %errorlevel%
)

echo [SUCCESS] UltimateAutoClicker.exe built successfully!
pause