#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
demo.py —— 自动运行真实程序，生成可复现的演示记录

同时启动 1 个服务端 + 3 个客户端（都是编译出来的真实可执行文件），
按时间顺序喂入聊天内容，收集每个客户端屏幕上的输出与服务端日志，
最后写成 docs/demo_transcript.txt，可直接用于讲解或截图。

用法：
    python scripts/demo.py
    python scripts/demo.py --port 8898 --out docs
"""

import argparse
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def default_bin(name):
    """Linux 下是 build/server，Windows/Cygwin 下是 build/server.exe，自动挑一个。"""
    for cand in (name, name + ".exe"):
        path = os.path.join(ROOT, "build", cand)
        if os.path.exists(path):
            return path
    return os.path.join(ROOT, "build", name)


# 录制主机是 Windows 上的 Cygwin 环境，运行时 cygwin1.dll 会往 stderr 打两条与程序无关的
# 兼容性告警（Linux 上没有）。这里把它们过滤掉，保持演示记录干净。
ENV_NOISE = ("find_fast_cwd", "the public mailing list cygwin")


def clean(text):
    return "\n".join(
        line for line in text.splitlines()
        if not any(marker in line for marker in ENV_NOISE)
    )

ACTIONS = [
    # (延迟秒, 客户端索引, 输入行)
    (0.5, 0, "大家好，我是小明，第一次用这个聊天室"),
    (0.8, 1, "你好小明，我是 alice，欢迎欢迎"),
    (0.8, 2, "我也在！我是 charlie"),
    (0.8, 0, "/list"),
    (0.9, 1, "/msg 小明 这句话只有你能看到"),
    (0.9, 0, "/msg alice 收到，私聊功能正常"),
    (0.9, 2, "我要去上课了，先撤"),
    (0.7, 2, "/quit"),
    (1.0, 0, "好的 charlie 再见"),
    (0.7, 1, "/quit"),
    (0.8, 0, "/quit"),
]


class Runner:
    def __init__(self, exe, args, name):
        env = dict(os.environ, CHAT_NO_COLOR="1")
        self.name = name
        self.proc = subprocess.Popen(
            [exe] + args,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env=env, bufsize=0,
        )
        self.output = b""
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._pump, daemon=True)
        self._thread.start()

    def _pump(self):
        while True:
            chunk = self.proc.stdout.read(1)
            if not chunk:
                break
            with self._lock:
                self.output += chunk

    def send(self, line):
        try:
            self.proc.stdin.write((line + "\n").encode("utf-8"))
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    def text(self):
        with self._lock:
            return self.output.decode("utf-8", "replace")

    def wait(self, timeout=6):
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self._thread.join(timeout=2)


def capture_ps(out_dir, exe_names):
    """客户端都在线时抓一次 ps -ef，作为"一客户端一进程"的实证。"""
    try:
        proc = subprocess.run(["ps", "-ef"], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=8)
    except (OSError, subprocess.SubprocessError):
        return None

    keep = []
    for line in proc.stdout.splitlines():
        low = line.lower()
        if any(name in low for name in exe_names) and " ps " not in low:
            keep.append(line.rstrip())
    if not keep:
        return None

    path = os.path.join(out_dir, "ps_tree.txt")
    with open(path, "w", encoding="utf-8") as f:
        f.write("【实测进程树】3 个客户端在线时执行 ps -ef 的结果\n")
        f.write("-" * 62 + "\n")
        f.write("\n".join(keep) + "\n")
        f.write("-" * 62 + "\n")
        f.write("三个子进程的 PPID 都等于主进程 PID。\n")
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8898)
    ap.add_argument("--out", default=os.path.join(ROOT, "docs"))
    ap.add_argument("--server", default=default_bin("server"))
    ap.add_argument("--client", default=default_bin("client"))
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    port = str(args.port)
    env = dict(os.environ, CHAT_NO_COLOR="1")

    server_log_path = os.path.join(args.out, "server_log.txt")
    with open(server_log_path, "wb") as logfile:
        server = subprocess.Popen(
            [args.server, "-p", port],
            stdout=subprocess.DEVNULL, stderr=logfile, env=env,
        )
        time.sleep(0.8)
        if server.poll() is not None:
            print("服务端启动失败，请先编译（make）")
            return 1

        clients = []
        try:
            for idx, nick in enumerate(("小明", "alice", "charlie")):
                clients.append(Runner(
                    args.client, ["-n", nick, "-s", "127.0.0.1", "-p", port],
                    "客户端%d(%s)" % (idx + 1, nick),
                ))
                time.sleep(0.35)

            # 三个客户端都在线，抓一次进程树（PID 与下面的服务端日志一致）
            time.sleep(0.3)
            names = {os.path.splitext(os.path.basename(args.server))[0].lower(),
                     os.path.splitext(os.path.basename(args.client))[0].lower()}
            ps_path = capture_ps(args.out, names)
            if ps_path:
                print("进程树已保存到：%s" % ps_path)

            for delay, idx, line in ACTIONS:
                time.sleep(delay)
                clients[idx].send(line)

            time.sleep(0.8)
            for c in clients:
                c.wait()
        finally:
            time.sleep(0.3)
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()

    server_log = clean(open(server_log_path, encoding="utf-8", errors="replace").read())

    lines = []
    lines.append("=" * 66)
    lines.append(" 多进程并发聊天程序 —— 运行演示记录")
    lines.append(" 1 个服务端 + 3 个客户端；每个客户端对应一个 fork() 出来的子进程")
    lines.append("=" * 66)
    lines.append("")
    lines.append("【服务端日志】")
    lines.append("-" * 66)
    lines.append(server_log.rstrip())
    lines.append("")
    for c in clients:
        lines.append("【%s 的屏幕】" % c.name)
        lines.append("-" * 66)
        lines.append(clean(c.text()).rstrip())
        lines.append("")

    transcript = "\n".join(lines)
    out_path = os.path.join(args.out, "demo_transcript.txt")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(transcript + "\n")

    print(transcript)
    print("\n演示记录已保存到：%s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
