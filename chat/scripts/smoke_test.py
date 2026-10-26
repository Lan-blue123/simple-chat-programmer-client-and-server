#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
smoke_test.py —— 协议级自动化测试

直接用 Python 的 socket 按聊天帧格式与服务端对话，逐条校验协议行为：
昵称查重、群聊转发、私聊路由、在线列表、退出通知、非法昵称拒绝。

用法：
    python scripts/smoke_test.py            # 自动启动/关闭服务端
    python scripts/smoke_test.py --port 8888 --server build/server.exe
"""

import argparse
import os
import socket
import struct
import subprocess
import sys
import time

# —— 与服务端 protocol.h 保持一致的帧类型 ——
FRAME_LOGIN, FRAME_TALK, FRAME_PRIVATE, FRAME_LIST, FRAME_QUIT = 1, 2, 3, 4, 5
FRAME_SAY, FRAME_WHISPER, FRAME_SYS, FRAME_ERR, FRAME_USERS = 6, 7, 8, 9, 10
FRAME_TEXT, FRAME_BYE = 11, 12

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def default_bin(name):
    """Linux 下是 build/server，Windows/Cygwin 下是 build/server.exe，自动挑一个。"""
    for cand in (name, name + ".exe"):
        path = os.path.join(ROOT, "build", cand)
        if os.path.exists(path):
            return path
    return os.path.join(ROOT, "build", name)


results = []


def send_frame(sock, ftype, payload=b""):
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    sock.sendall(bytes([ftype]) + struct.pack(">H", len(payload)) + payload)


def _recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def recv_frame(sock):
    """返回 (type, text)；连接结束返回 None"""
    hdr = _recv_exact(sock, 3)
    if hdr is None:
        return None
    ftype = hdr[0]
    length = struct.unpack(">H", hdr[1:])[0]
    body = _recv_exact(sock, length) if length else b""
    if body is None:
        return None
    return ftype, body.decode("utf-8", "replace")


def drain_frames(sock, seconds=0.6):
    """读走 seconds 秒内的所有帧，返回 (type, text) 列表"""
    out = []
    end = time.time() + seconds
    sock.settimeout(0.15)
    while time.time() < end:
        try:
            item = recv_frame(sock)
        except (socket.timeout, OSError):
            continue
        if item is None:
            break
        out.append(item)
    return out


def drain(sock, seconds=0.6):
    """只取文本部分，拼接成一个字符串"""
    return "".join(text for _t, text in drain_frames(sock, seconds))


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(("  [通过] " if ok else "  [失败] ") + name + (("  " + detail) if detail else ""))
    return ok


def connect(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.settimeout(0.3)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8899)
    ap.add_argument("--server", default=default_bin("server"))
    args = ap.parse_args()

    port = args.port
    env = dict(os.environ, CHAT_NO_COLOR="1")
    server = subprocess.Popen(
        [args.server, "-p", str(port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env,
    )
    time.sleep(0.7)
    if server.poll() is not None:
        print("服务端启动失败，请先执行 make 编译")
        return 1

    try:
        print("\n=== 多进程聊天程序 协议级自动化测试 ===")

        # 1. 三个正常客户端登录
        a = connect(port)
        send_frame(a, FRAME_LOGIN, "小明")
        hello_a = "".join(drain(a))
        check("客户端 A 登录成功并收到欢迎语", "欢迎你" in hello_a, hello_a.strip()[:40])

        b = connect(port)
        send_frame(b, FRAME_LOGIN, "alice")
        drain(b)
        joined = "".join(drain(a))
        check("新用户上线时其他人收到通知", "加入了聊天室" in joined, joined.strip()[:40])

        c = connect(port)
        send_frame(c, FRAME_LOGIN, "charlie")
        drain(c)
        drain(a)
        drain(b)

        # 2. 昵称查重
        d = connect(port)
        send_frame(d, FRAME_LOGIN, "小明")
        dup = "".join(drain(d, 1.0))
        check("重复昵称被拒绝", "已被占用" in dup, dup.strip()[:40])
        d.close()

        # 3. 群聊广播：A 说的话 B、C 都要收到
        send_frame(a, FRAME_TALK, "大家好，我是小明！")
        msg_b = "".join(drain(b))
        msg_c = "".join(drain(c))
        check("群聊消息广播到 B", "大家好，我是小明" in msg_b and "小明" in msg_b)
        check("群聊消息广播到 C", "大家好，我是小明" in msg_c)

        # 4. 私聊路由：只有目标收到
        send_frame(b, FRAME_PRIVATE, "小明\t这是一条悄悄话")
        priv_a = "".join(drain(a))
        priv_c = "".join(drain(c))
        check("私聊送达目标用户 A", "悄悄话" in priv_a and "alice" in priv_a)
        check("私聊不会泄露给第三方 C", "悄悄话" not in priv_c)

        # 5. 在线列表
        send_frame(a, FRAME_LIST)
        users = "".join(drain(a))
        check("在线列表包含全部三个昵称",
              all(n in users for n in ("小明", "alice", "charlie")), users.replace("\n", " ")[:60])

        # 6. 非法昵称（含空格）
        e = connect(port)
        send_frame(e, FRAME_LOGIN, "bad nick")
        bad = "".join(drain(e, 1.0))
        check("非法昵称被拒绝", "不合法" in bad, bad.strip()[:40])
        e.close()

        # 7. 主动退出，其余用户收到离开通知
        send_frame(c, FRAME_QUIT, "")
        bye_frames = drain_frames(c, 1.0)
        left = drain(a, 1.0)
        check("退出时收到 BYE 帧",
              any(ftype == FRAME_BYE for ftype, _ in bye_frames),
              "收到帧类型：%s" % [ftype for ftype, _ in bye_frames])
        check("其他用户收到离开通知", "离开了聊天室" in left, left.strip()[:40])

        # 8. 服务端仍然健康：新用户可继续接入
        f = connect(port)
        send_frame(f, FRAME_LOGIN, "新同学")
        welcome = "".join(drain(f))
        check("已有用户退出后服务端仍可接入新连接", "欢迎你" in welcome)
        f.close()

        for s in (a, b):
            try:
                send_frame(s, FRAME_QUIT, "")
                drain(s, 0.3)
                s.close()
            except OSError:
                pass
    finally:
        time.sleep(0.3)
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()

    passed = sum(1 for _, ok, _ in results if ok)
    print("\n---------------------------------------------")
    print("测试结果：%d/%d 通过" % (passed, len(results)))
    print("---------------------------------------------")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
