@echo off
setlocal

echo Finding Visual Studio...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found. Please install Visual Studio with "Desktop development with C++".
    pause
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%i"
)

if "%VS_PATH%"=="" (
    echo [ERROR] Visual Studio C++ build tools not found.
    echo Open Visual Studio Installer and ensure "Desktop development with C++" is installed.
    pause
    exit /b 1
)

call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] Failed to initialize MSVC environment.
    pause
    exit /b 1
)

echo Compiling resources...
rc.exe /nologo src\app.rc
if errorlevel 1 (
    echo [ERROR] Resource compilation failed.
    pause
    exit /b 1
)

echo Compiling SoundBalance.exe...
cl.exe /nologo /std:c++17 /utf-8 /O2 /MT /EHsc /DUNICODE /D_UNICODE src\main.cpp src\app.res /Fe:SoundBalance.exe /link /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo [ERROR] Compilation failed.
    pause
    exit /b 1
)

del /q main.obj src\app.res 2>nul
echo.
echo [SUCCESS] Built SoundBalance.exe!
echo Double-click SoundBalance.exe to launch it in your system tray.
pause
