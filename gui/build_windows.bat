@echo off
rem ===========================================================================
rem  build_windows.bat —— 用 MinGW-w64 编译图形客户端（生成 chat_gui.exe）
rem
rem  用法：双击本文件即可。如果 PATH 里没有 gcc，会按下面列出的常见路径找。
rem ===========================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "GCC="
where gcc >nul 2>nul
if not errorlevel 1 set "GCC=gcc"

if not defined GCC (
    for %%D in (
        "D:\software\CLion 2026.1.3\bin\mingw\bin"
        "C:\msys64\mingw64\bin"
        "C:\mingw64\bin"
        "C:\MinGW\bin"
        "C:\Program Files\mingw-w64\mingw64\bin"
        "C:\Program Files (x86)\mingw-w64\mingw64\bin"
    ) do (
        if not defined GCC if exist "%%~D\gcc.exe" set "GCC=%%~D\gcc.exe"
    )
)

if not defined GCC (
    echo [错误] 没有找到 gcc。
    echo.
    echo 三种解决办法：
    echo   1. 直接用本目录里已经编译好的 chat_gui.exe（如果存在）；
    echo   2. 安装 MinGW-w64，安装时勾选 "Add to PATH"，重开命令行再运行本文件；
    echo   3. 命令行里明确指定： set "GCC=你的路径\gcc.exe" 后再运行本文件。
    echo.
    pause
    exit /b 1
)

echo 使用编译器：!GCC!
echo 正在编译 chat_gui.exe ...
"!GCC!" -O2 -Wall -Wextra -std=gnu99 -mwindows -o chat_gui.exe chat_gui.c ^
    -lws2_32 -lgdi32 -luser32 -lshell32 -static-libgcc

if errorlevel 1 (
    echo.
    echo [失败] 编译出错，请把上面的错误信息发给开发者。
    pause
    exit /b 1
)

echo.
echo [成功] 已生成 %cd%\chat_gui.exe
echo 双击它即可运行；可以同时开多个窗口。
echo.
pause
