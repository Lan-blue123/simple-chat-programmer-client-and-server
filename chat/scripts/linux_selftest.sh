#!/usr/bin/env bash
# ============================================================================
# linux_selftest.sh —— 在 Linux 上一键完成本项目的全部验证
#
#   1. 环境信息
#   2. 编译（有 make 用 make，没有就直接调 gcc）
#   3. 协议级自动化测试（12 项断言）
#   4. 真实演示录制（1 服务端 + 3 客户端，写入 chat/docs/）
#   5. 进程结构实证：ps / pstree / /proc/<pid>/fd / 监听端口 / 僵尸进程检查
#   6. 服务端看板自检（-d：在线会话表 + fork/登录事件）
#
# 用法：
#   bash scripts/linux_selftest.sh              # 全流程
#   bash scripts/linux_selftest.sh --skip-demo  # 跳过第 4 步（不覆盖 docs/）
#   bash scripts/linux_selftest.sh --skip-demo --skip-build
#
# 结果会同时打印到屏幕并保存到 ./selftest_report.txt
# ============================================================================
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

REPORT="$ROOT/selftest_report.txt"
PORT_DEMO=8898
PORT_TEST=8899
PORT_DASH=8897
SKIP_DEMO=0
SKIP_BUILD=0

for arg in "$@"; do
    case "$arg" in
        --skip-demo)  SKIP_DEMO=1 ;;
        --skip-build) SKIP_BUILD=1 ;;
        *) echo "未知参数：$arg"; exit 2 ;;
    esac
done

# 同时输出到屏幕和报告文件
exec > >(tee "$REPORT") 2>&1

say() { printf '\n\033[1;36m===== %s =====\033[0m\n' "$*"; }
sub() { printf '\n--- %s ---\n' "$*"; }
ok()  { printf '\033[1;32m[OK]\033[0m %s\n' "$*"; }
bad() { printf '\033[1;31m[FAIL]\033[0m %s\n' "$*"; }

FAILED=0
note_fail() { bad "$*"; FAILED=$((FAILED + 1)); }

printf '多进程并发聊天程序 · Linux 自测报告\n'
printf '时间：%s\n' "$(date '+%Y-%m-%d %H:%M:%S')"
printf '目录：%s\n' "$ROOT"

# ---------------------------------------------------------------- 1. 环境信息
say "1. 环境信息"
sub "系统"
uname -a || true
sed -n '1,3p' /etc/os-release 2>/dev/null || true
sub "工具链"
printf 'gcc     : %s\n' "$(command -v gcc || echo '未安装')"
printf 'make    : %s\n' "$(command -v make || echo '未安装（脚本会自动改用 gcc 直接编译）')"
printf 'python3 : %s\n' "$(command -v python3 || echo '未安装（将跳过自动化测试）')"
printf 'nproc   : %s\n' "$(nproc 2>/dev/null || echo '?')"
if command -v gcc >/dev/null 2>&1; then
    gcc --version | head -1
    ok "gcc 可用"
else
    note_fail "没有 gcc，请先安装：sudo apt update && sudo apt install -y gcc（Debian/Ubuntu）或 sudo dnf install -y gcc"
    echo "无法继续，退出。"
    exit 1
fi

# ---------------------------------------------------------------- 2. 编译
if [ "$SKIP_BUILD" -eq 0 ]; then
    say "2. 编译（应无警告）"
    sub "构建"
    if command -v make >/dev/null 2>&1; then
        echo "使用 make"
        if make; then ok "make 编译通过"; else note_fail "make 编译失败"; fi
    else
        echo "未安装 make，直接用 gcc 编译"
        mkdir -p build
        if gcc -Wall -Wextra -O2 -std=gnu99 -Iinclude -c src/frame.c  -o build/frame.o \
        && gcc -Wall -Wextra -O2 -std=gnu99 -Iinclude -c src/dash.c   -o build/dash.o \
        && gcc -Wall -Wextra -O2 -std=gnu99 -Iinclude -c src/server.c -o build/server.o \
        && gcc -Wall -Wextra -O2 -std=gnu99 -Iinclude -c src/client.c -o build/client.o \
        && gcc -o build/server build/frame.o build/server.o build/dash.o \
        && gcc -o build/client build/frame.o build/client.o; then
            ok "gcc 直接编译通过（无警告）"
        else
            note_fail "gcc 编译失败"
        fi
    fi
    sub "产物"
    ls -l build/ 2>/dev/null || true
    [ -x build/server ] && [ -x build/client ] && ok "build/server 与 build/client 均已生成" \
        || note_fail "可执行文件缺失"
fi

# ---------------------------------------------------------------- 3. 协议测试
if command -v python3 >/dev/null 2>&1 && [ -x build/server ]; then
    say "3. 协议级自动化测试（12 项断言）"
    python3 scripts/smoke_test.py --port "$PORT_TEST"
    RC=$?
    if [ "$RC" -eq 0 ]; then
        ok "协议测试全部通过"
    else
        note_fail "协议测试未全部通过（退出码 $RC）"
    fi
else
    say "3. 协议级自动化测试"
    echo "跳过：需要 python3 且已编译出 build/server"
fi

# ---------------------------------------------------------------- 4. 演示录制
if [ "$SKIP_DEMO" -eq 0 ] && command -v python3 >/dev/null 2>&1 && [ -x build/server ]; then
    say "4. 真实演示录制（1 服务端 + 3 客户端）"
    echo "注意：会覆盖 chat/docs/demo_transcript.txt 与 docs/ps_tree.txt"
    python3 scripts/demo.py --port "$PORT_DEMO" | tail -25
    RC=$?
    if [ "$RC" -eq 0 ] && [ -f docs/demo_transcript.txt ]; then
        ok "演示录制完成：docs/demo_transcript.txt"
    else
        note_fail "演示录制失败（退出码 $RC）"
    fi
elif [ "$SKIP_DEMO" -eq 1 ]; then
    say "4. 真实演示录制"
    echo "已按 --skip-demo 跳过"
fi

# ---------------------------------------------------------------- 5. 进程实证
say "5. 进程结构实证：每接入一个客户端就 fork 一个子进程"
if [ -x build/server ]; then
    ./build/server -p "$PORT_DEMO" >/tmp/chat_srv.log 2>&1 &
    SRV_PID=$!
    sleep 0.8
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        note_fail "服务端未能启动，见 /tmp/chat_srv.log"
        sed -n '1,20p' /tmp/chat_srv.log
    else
        sub "启动 3 个客户端（用 sleep 撑住 stdin 让它们保持在线）"
        CLIENT_PIDS=""
        for n in 小明 alice charlie; do
            ( sleep 30 ) | ./build/client -n "$n" -p "$PORT_DEMO" >/dev/null 2>&1 &
            CLIENT_PIDS="$CLIENT_PIDS $!"
            sleep 0.3
        done
        sleep 1.2

        sub "服务端主进程：pid=$SRV_PID"
        ps -o pid,ppid,stat,cmd -p "$SRV_PID" 2>/dev/null || ps -ef | grep "[b]uild/server"

        sub "它 fork 出来的子进程（PPID 应都等于 $SRV_PID）"
        if ps -o pid,ppid,stat,cmd --ppid "$SRV_PID" 2>/dev/null | tail -n +2 | grep -q .; then
            ps -o pid,ppid,stat,cmd --ppid "$SRV_PID"
            CHILD_CNT=$(ps -o pid= --ppid "$SRV_PID" 2>/dev/null | wc -l | tr -d ' ')
            echo "子进程数量：$CHILD_CNT（应为 3）"
            [ "$CHILD_CNT" -eq 3 ] && ok "3 个客户端 = 3 个子进程" || note_fail "子进程数不是 3"
        else
            note_fail "没有看到子进程"
        fi

        sub "进程树（pstree，未安装则跳过）"
        if command -v pstree >/dev/null 2>&1; then
            pstree -p "$SRV_PID" || true
        else
            echo "未安装 pstree（psmisc），用 ps 代替；如需可 sudo apt install -y psmisc"
            ps -eo pid,ppid,comm | awk -v p="$SRV_PID" 'NR==1 || $2==p || $1==p'
        fi

        sub "子进程持有的描述符（能看到客户端连接 + socketpair 控制通道）"
        for c in $(ps -o pid= --ppid "$SRV_PID" 2>/dev/null); do
            echo "[子进程 $c]"
            ls -l "/proc/$c/fd" 2>/dev/null | sed 's/^/    /' || echo "    (读不到 /proc，可能权限不足)"
        done

        sub "监听端口（应由主进程持有）"
        if command -v ss >/dev/null 2>&1; then
            ss -ltnp 2>/dev/null | grep -E ":$PORT_DEMO\b" || echo "ss 没看到端口（可能需要 root）"
        elif command -v netstat >/dev/null 2>&1; then
            netstat -ltnp 2>/dev/null | grep -E ":$PORT_DEMO\b" || true
        fi

        sub "僵尸进程检查"
        ZOMBIES=$(ps -eo stat= | grep -c '^Z' 2>/dev/null || true)
        ZOMBIES=${ZOMBIES:-0}
        echo "当前僵尸进程数：$ZOMBIES"
        [ "$ZOMBIES" -eq 0 ] && ok "没有僵尸进程（SIGCHLD + waitpid 生效）" \
            || note_fail "存在僵尸进程，检查 SIGCHLD 处理"

        sub "清理"
        kill "$SRV_PID" 2>/dev/null || true
        for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null || true; done
        pkill -f "build/client -n" 2>/dev/null || true
        sleep 0.4
        if kill -0 "$SRV_PID" 2>/dev/null; then
            kill -9 "$SRV_PID" 2>/dev/null || true
        fi
        ok "演示进程已清理"
    fi
else
    note_fail "build/server 不存在，先编译"
fi

# ---------------------------------------------------------------- 6. 看板自检
say "6. 服务端看板自检（-d：在线会话 / 子进程 PID / 事件流）"
if [ -x build/server ]; then
    CHAT_DASH_FORCE=1 CHAT_DASH_NO_ALT=1 COLUMNS=110 LINES=30 \
        ./build/server -p "$PORT_DASH" -d > /tmp/chat_dash.out 2>&1 &
    DASH_PID=$!
    sleep 0.8
    if kill -0 "$DASH_PID" 2>/dev/null; then
        ( sleep 3 ) | ./build/client -n 看板自检 -p "$PORT_DASH" >/dev/null 2>&1 &
        sleep 1.6
        kill "$DASH_PID" 2>/dev/null || true
        sleep 0.4
        if grep -q '在线会话' /tmp/chat_dash.out && grep -q 'fork 子进程 pid=' /tmp/chat_dash.out \
           && grep -q '登录成功' /tmp/chat_dash.out; then
            ok "看板渲染正常（含会话表与 fork/登录事件）"
            sub "看板最后一帧（去转义后）"
            sed -e 's/\x1b\[[0-9;?]*[A-Za-z]//g' /tmp/chat_dash.out | awk 'BEGIN{RS="\x1b\\[H"} END{print}' | tail -20
        else
            note_fail "看板输出不符合预期，见 /tmp/chat_dash.out"
            tail -20 /tmp/chat_dash.out
        fi
        pkill -f "build/client -n 看板自检" 2>/dev/null || true
    else
        note_fail "看板模式启动失败，见 /tmp/chat_dash.out"
        tail -20 /tmp/chat_dash.out
    fi
else
    note_fail "build/server 不存在，先编译"
fi

# ---------------------------------------------------------------- 汇总
say "汇总"
if [ "$FAILED" -eq 0 ]; then
    ok "全部检查通过（可以用于演示/截图）"
else
    bad "有 $FAILED 项未通过，见上面 [FAIL] 项"
fi
printf '报告已保存：%s\n' "$REPORT"
exit "$FAILED"
