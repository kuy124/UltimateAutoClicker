@echo off
setlocal
cd /d "%~dp0"

rem The GUI test binary is linked GUI-style but with a console subsystem so the
rem self test output is visible. It is built without the icon resource because
rem some AV scanners refuse to start icon-carrying binaries from a test run.

echo [1/3] Building unit tests...
g++ core.cpp tests.cpp -o tests.exe -static -O1 -g -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE
if %errorlevel% neq 0 (
    echo [ERROR] Test build failed!
    exit /b %errorlevel%
)

echo [2/3] Building GUI self test...
g++ core.cpp main.cpp -o guitest.exe -mwindows -Wl,--subsystem,console -lcomctl32 -lshell32 -static -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type -DUNICODE -D_UNICODE
if %errorlevel% neq 0 (
    echo [ERROR] GUI test build failed!
    exit /b %errorlevel%
)

echo [3/3] Running unit tests...
tests.exe
set RC=%errorlevel%
if %RC% neq 0 goto fail

echo.
echo Running GUI self test...
guitest.exe --selftest
set RC=%errorlevel%
if %RC% neq 0 goto fail

echo [PASSED] All tests passed
exit /b 0
:fail
echo [FAILED] %RC% check(s) failed
exit /b %RC%
