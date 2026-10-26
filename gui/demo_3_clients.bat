@echo off
rem ===========================================================================
rem  一键开 3 个客户端窗口（默认连你的云服务器）
rem
rem  用法：
rem    demo_3_clients.bat                    连 119.23.182.147:8888
rem    demo_3_clients.bat 192.168.1.10 8888  连指定 IP 和端口
rem
rem  说明：批处理文件里有中文容易在 cmd 里乱码，所以这里用英文昵称自动登录，
rem        进去后每个窗口里输入  /nick 小明  就能改成中文昵称。
rem        想手动填昵称，就双击 chat_gui.exe（不自动连接）。
rem ===========================================================================
setlocal
cd /d "%~dp0"

set "SERVER=%~1"
if "%SERVER%"=="" set "SERVER=0.0.0.0"
set "PORT=%~2"
if "%PORT%"=="" set "PORT=8888"

if not exist "chat_gui.exe" (
    echo [错误] 当前目录没有 chat_gui.exe，请先运行 build_windows.bat
    pause
    exit /b 1
)

echo 启动 3 个客户端，连接 %SERVER%:%PORT% ...
start "" chat_gui.exe --server %SERVER% --port %PORT% --nick alice --auto-connect
timeout /t 1 /nobreak >nul
start "" chat_gui.exe --server %SERVER% --port %PORT% --nick bob --auto-connect
timeout /t 1 /nobreak >nul
start "" chat_gui.exe --server %SERVER% --port %PORT% --nick charlie --auto-connect

echo.
echo 已启动 3 个窗口（alice / bob / charlie）。
echo 想在窗口里改中文昵称：输入  /nick 小明
echo.
