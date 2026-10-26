#!/usr/bin/env bash
# ============================================================================
# build_linux_cross.sh —— 在 Linux 上交叉编译 Windows 版图形客户端
#
#   sudo apt install -y mingw-w64      # 首次
#   bash build_linux_cross.sh
#
# 生成的 chat_gui.exe 可以直接拷到任何 Windows 机器上双击运行。
# ============================================================================
set -e
cd "$(dirname "$0")"

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "缺少交叉编译器，请先安装："
    echo "  Debian/Ubuntu: sudo apt install -y mingw-w64"
    echo "  CentOS/RHEL:   sudo yum install -y mingw64-gcc"
    exit 1
fi

x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -std=gnu99 -mwindows \
    -o chat_gui.exe chat_gui.c \
    -lws2_32 -lgdi32 -luser32 -lshell32 -static-libgcc

echo "已生成：$(pwd)/chat_gui.exe"
ls -l chat_gui.exe
