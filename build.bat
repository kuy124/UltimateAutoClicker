@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
if not exist bin mkdir bin

echo [1/2] Compiling resources...
windres -I assets assets\resource.rc -O coff -o build\resource.res
if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile resource.rc
    exit /b %errorlevel%
)

echo [2/2] Building UltimateAutoClicker.exe...
g++ src\core.cpp src\main.cpp build\resource.res -o bin\UltimateAutoClicker.exe -mwindows -lcomctl32 -lshell32 -static -s -Os -fno-exceptions -fno-rtti -DUNICODE -D_UNICODE
if %errorlevel% neq 0 (
    echo [ERROR] Build failed!
    exit /b %errorlevel%
)

echo [SUCCESS] bin\UltimateAutoClicker.exe built successfully!
