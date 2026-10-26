# 简单演示多进程并发聊天程序 

演示主题：**用 socket 实现一个相对复杂的聊天程序——1 个服务端 + 多个客户端，每来一个客户端，服务端就 `fork()` 一个子进程与之通信。**

> 本仓库提交的是**代码与文档**：`chat/`（服务端 + 命令行客户端 + 实时看板）、`gui/`（Windows 图形客户端与源码）。
> 演示文稿（21 MB）与 PPT 模板（47 MB）体积大且与代码无关，未上传。
>
> 最快上手：
> ```sh
> cd chat && make
> ./build/server -p 8888 -d      # 服务端（-d 打开实时看板）
> ./build/client -n 小明          # 命令行客户端（默认连本机 0.0.0.0:8888）
> ```
> Windows 图形客户端直接双击 `gui/chat_gui.exe`，可以多开。

---

## 一、文件清单

| 文件 / 目录 | 说明 |
| --- | --- |
| `多进程并发聊天程序_演示.pptx` | **演示文稿（23 页，含图形客户端与看板）**，每页都写了备注讲稿，可照读 |
| `chat/` | 完整可编译的 C 代码、自动演示与测试脚本、真实运行记录 |
| `chat/src/server.c` | 服务端：accept + fork、用户表、消息路由、SIGCHLD 回收、`-d` 实时看板 |
| `chat/src/dash.c` | 服务端实时看板（在线会话 + 子进程 PID + 消息流转事件，纯 ANSI 实现） |
| `chat/src/client.c` | 命令行客户端：连接、登录、`select` 同时等键盘与套接字 |
| `chat/src/frame.c` | 可靠读写与帧封装（处理 EINTR、短读短写） |
| `chat/include/protocol.h` | 帧类型与常量定义 |
| `gui/chat_gui.exe` | **Windows 图形客户端（单文件，可多开）** |
| `gui/chat_gui.c` | 图形客户端源码（Win32 + Winsock + RichEdit） |
| `gui/build_windows.bat` | 双击即可重新编译 exe |
| `gui/demo_3_clients.bat` | 一键打开 3 个客户端窗口 |
| `gui/README.md` | 图形客户端详细用法 |
| `chat/scripts/demo.py` | 一键启动 1 服务端 + 3 客户端，自动聊天并录制输出 |
| `chat/scripts/smoke_test.py` | 协议级自动化测试（12 项断言，当前 12/12 通过） |
| `chat/scripts/linux_selftest.sh` | Linux 上一键完成编译 → 测试 → 录制 → 进程/僵尸实证 |
| `chat/docs/demo_transcript.txt` | 真实运行录制的服务端日志 + 三个客户端屏幕 |
| `chat/docs/ps_tree.txt` | 3 个客户端在线时 `ps -ef` 的实测进程树 |
| `chat/docs/dashboard_sample.txt` | 服务端看板的实际渲染文本（含 3 个会话的 PID 与事件流） |
| `文化风PPT模板.pptx` | 原始模板（未改动） |
| `tools/build_deck.py` | 用模板 + `chat/docs` 的数据重新生成 PPT 的脚本 |

---

## 二、现场演示：三步

需要 POSIX 环境（Linux / macOS / WSL / Cygwin；Windows 原生 cmd 没有 `fork()`）。

```sh
cd chat
make                                  # 编译，生成 build/server 与 build/client
```

**终端 1（服务端）**

```sh
./build/server -p 8888
```

**终端 2~4（三个客户端）**

```sh
./build/client -n 小明
./build/client -n alice
./build/client -n charlie
```

依次输入下面这些，效果就是 PPT 第 15、16 页的内容：

| 客户端 | 输入 | 演示的功能 |
| --- | --- | --- |
| 小明 | `大家好，我是小明` | 群聊广播（三个客户端都能看到） |
| alice | `/list` | 在线列表 |
| alice | `/msg 小明 这句话只有你能看到` | 私聊（charlie 看不到） |
| charlie | `/quit` | 下线通知 |
| 小明 | `/nick 小明的电脑` | 改名广播 |

> 关键一句：**服务端每接受一个连接就 fork 一个子进程**。演示时另开一个终端跑 `ps -ef | grep server`（Linux）或 `pstree -p <主进程pid>`，能直接看到主进程下面挂着 3 个子进程。

### 更适合作课堂演示的两种方式

**① 服务端开实时看板**（消息怎么流转、fork 了哪些子进程，全在屏幕上滚动）

```sh
./build/server -p 8888 -d
```

看板上半部分是在线会话表：`槽位 / PID（子进程）/ 昵称 / 对端地址 / 在线时长 / 收到 / 发出`；
下半部分是事件流：`接受连接 → fork 子进程 → 登录 → 群聊广播给几人 → 私聊 A→B → 退出 → waitpid 回收`。
按 Ctrl+C 退出后终端会恢复原样。样例文本见 `chat/docs/dashboard_sample.txt`。

**② Windows 端用图形客户端开多个窗口**（比四个终端窗口好看得多）

```bat
:: 双击运行，或命令行带 IP/端口
gui\chat_gui.exe
gui\demo_3_clients.bat 119.23.182.147 8888     :: 一次开 3 个窗口
```

图形客户端是单个 exe，开几次就是几个客户端，和服务端的"一客户端一子进程"完全对应。
详见 `gui/README.md`。

### 部署到云服务器（Linux）的完整流程

```sh
# 本地打包（Windows 上用自带的 tar 即可）
tar -czf chat.tar.gz chat

# 上传到服务器（用 workbench upload / scp / 宝塔面板均可）
# 服务器上：
tar xzf chat.tar.gz && cd chat
make                        # 需要 gcc + make：sudo apt install -y build-essential
./build/server -p 8888 -d   # 带看板启动
```

**别忘了在云服务器安全组里放行 8888/TCP**，否则外网客户端连不上（`ping` 通但端口不通就是这个问题）。
客户端只要在 GUI 里把服务器填成 `119.23.182.147`、端口 `8888`、昵称随便填，点"连接"即可，最多 64 个客户端同时在线。

如果不想手动开 4 个终端，直接跑一键脚本复现：

```sh
python3 scripts/demo.py         # 自动跑完整对话，输出录制到 docs/demo_transcript.txt
python3 scripts/smoke_test.py   # 协议级测试，最后打印 12/12
```

客户端命令：`/list` `/who` 在线列表，`/msg 昵称 内容`（或 `/w`）私聊，`/nick 新昵称` 改名，`/help` 帮助，`/quit` 退出。

---

## 三、在 Linux 上测试（从编译到出证据）

> 代码是 POSIX C，Windows 原生 cmd 没有 `fork()`。请在 **Linux / WSL / Linux 虚拟机** 里测。
> 本机 `F:\ubuntu` 下已有一台 Ubuntu 20.04 虚拟机，可直接开机使用。
> 首次需要编译工具：`sudo apt install -y build-essential`（`python3` 一般自带）。
>
> **懒人版**：`bash scripts/linux_selftest.sh` 一条命令跑完全部检查（编译 → 12 项协议测试 → 录制演示 → `ps`/`pstree`/`/proc`/僵尸进程实证），结果同时打印并保存到 `chat/selftest_report.txt`；`--skip-demo` 可跳过录制以免覆盖现有 docs。没有 `make` 时脚本会自动改用 `gcc` 直接编译。

### 0. 把代码放进 Linux

| 环境 | 做法 |
| --- | --- |
| VMware 虚拟机 | 设置 → 选项 → 共享文件夹，在客户机里从 `/mnt/hgfs/<共享名>` 拷贝；或直接把 `chat` 目录拖进桌面 |
| WSL | 代码在 `/mnt/e/课程/操作系统/class2`，先 `cp -r` 到 home 再编译（`/mnt` 下编译慢很多） |
| 服务器 / 另一台机器 | `scp -r chat user@host:~/`，或在 Windows 上 `git init` 后 clone |

### 1. 编译（应零警告）

```sh
cd chat
make
ls -l build/            # 应看到 server 和 client
```

若报 `missing separator`：Makefile 的缩进被编辑器换成了空格，命令行必须是 **TAB**（本仓库文件是正确的 LF + TAB，无需改动）。

### 2. 手动交互测试（演示时最有说服力）

开 4 个终端：

```sh
# 终端 1
./build/server -p 8888
# 终端 2 / 3 / 4
./build/client -n 小明
./build/client -n alice
./build/client -n charlie
```

| 在哪个客户端 | 输入 | 预期现象 |
| --- | --- | --- |
| 小明 | `大家好，我是小明` | 三个客户端都显示 `[时间] <小明> ...` |
| alice | `/list` | 显示 `=== 在线用户 ===` 三个昵称 + 人数 |
| alice | `/msg 小明 这句话只有你能看到` | 只有小明的屏幕出现"私聊->我"，charlie 看不到 |
| charlie | `/quit` | charlie 收到 `BYE`；其余两人收到"「charlie」离开了聊天室" |
| 小明 | `/nick 小明的电脑` | 所有人收到改名通知；此后消息用小明的电脑显示 |

### 3. 自动化测试（一条命令出结论）

```sh
python3 scripts/smoke_test.py
```

预期末尾输出 `测试结果：12/12 通过`。它会自己拉起服务端、用 socket 按帧格式对话、跑完 12 项断言后关闭服务端。

### 4. 录制演示数据（可选，注意 pid）

```sh
python3 scripts/demo.py      # 覆盖 chat/docs/demo_transcript.txt 与 ps_tree.txt
```

⚠️ 重跑会生成**新的 pid**，而 PPT 第 5、16 页引用的是当前 `docs/` 里的数据。若要重跑，跑完执行 `python tools/build_deck.py` 重新生成 PPT，两处就又会一致。

### 5. 证明"一个客户端一个进程"的 Linux 命令

| 命令 | 看什么 |
| --- | --- |
| `ps -ef \| grep '[s]erver'` | 1 个主进程 + 3 个 `PPID` 指向它的子进程 |
| `pstree -p $(pgrep -f 'build/server' \| head -1)` | 树状父子关系，最直观 |
| `ls -l /proc/<子进程pid>/fd` | 能看到该子进程持有的客户端连接 + 一条 `socket`（父子控制通道） |
| `ss -ltnp \| grep 8888` | 监听套接字由主进程持有 |
| `ps -eo stat,pid,ppid,cmd \| grep '^Z'` | 无输出 = 没有僵尸进程（对比：把 `SIGCHLD` 处理删掉再试） |
| `strace -f -e trace=clone,fork,vfork ./build/server` | 每次 `connect` 都能看到一次 `fork()` 系统调用（可选炫技） |

### 6. 常见问题

| 现象 | 处理 |
| --- | --- |
| `Address already in use` | 端口被占用或处于 TIME_WAIT；换端口 `-p 8889`（代码已设 `SO_REUSEADDR`） |
| 中文显示乱码 | 终端需 UTF-8：`export LANG=C.UTF-8`（或 `zh_CN.UTF-8`） |
| 输出带颜色转义符，不适合截图 | `CHAT_NO_COLOR=1 ./build/server`（客户端同样适用） |
| `client` 连不上 | 本机测试用 `127.0.0.1`（默认）；跨机器要 `-s <服务端IP>` 并放行防火墙 8888 |
| `make: command not found` | `sudo apt install -y build-essential` |
| 超过 64 个客户端 | 代码上限 `CHAT_MAX_CLIENTS=64`；再往上受 `ulimit -n` 限制 |

---

## 四、PPT 结构与讲解节奏（约 19 分钟）

| 页码 | 内容 | 讲解重点 | 建议时长 |
| --- | --- | --- | --- |
| 1 | 封面 | 一句话说清题目 | 0.5 min |
| 2 | 目录 | 四部分（04 已改为"演示与可视化"） | 0.5 min |
| 3 | 章节 01 | — | — |
| 4 | 六个核心功能 | 左边服务端能力，右边用户体验 | 1 min |
| 5 | 总体架构图 | **重点**：客户端—子进程—主进程三段式，`socketpair` 是 IPC | 2 min |
| 6 | 章节 02 | — | — |
| 7 | 三个角色 | 主进程管关系，子进程管连接 | 1.5 min |
| 8 | 连接生命周期 | fork 之后的四件必做事（fflush/关 fd/_exit/waitpid） | 1.5 min |
| 9 | 章节 03 | — | — |
| 10 | 通信协议 | 帧头 3 字节 + 12 种帧类型，为什么要自己切帧 | 1.5 min |
| 11 | 关键代码① | `socketpair + fork` 那十几行，逐行讲 | 2 min |
| 12 | 关键代码② | 一个 `select` 盯住 N+1 个描述符 | 1.5 min |
| 13 | 关键代码③ | **新增**：一套协议两种渲染，`\tgui` 标记怎么分流 | 1.5 min |
| 14 | 四个容易踩的坑 | 短读短写 / EINTR / SIGPIPE / 僵尸进程 | 1.5 min |
| 15 | 章节 04 | — | — |
| 16 | 运行演示① | 现场跑，或放录制结果 | 1.5 min |
| 17 | 运行演示② | 日志里 pid 交错 + `ps` 里 PPID 全指向主进程 | 1.5 min |
| 18 | 图形客户端 | **新增**：单文件 exe、多开、非阻塞连接 + 接收线程 | 1.5 min |
| 19 | 服务端实时看板 | **新增**：会话表里的子进程 PID + 事件流 | 1.5 min |
| 20 | 看板原理 | **新增**：整屏重绘、末行不换行、CJK 宽度、踩坑记录 | 1 min |
| 21 | 自动化测试 | 12/12，说明不是"看着能跑" | 0.5 min |
| 22 | 三种并发模型对比 | 多进程 / 多线程 / select 的取舍 | 1 min |
| 23 | 结束页 | — | — |

每页的 `备注` 里都写了可以直接照读的讲稿（PowerPoint 里"视图 → 备注页"或演讲者视图可见）。
第 18、19 页的截图是真实运行的画面：图形客户端连的是云服务器 `119.23.182.147:8888`，看板截图来自同一台服务器。

如果不讲可视化（时间紧），可以整段跳过 18–20 页，把 21 页接在 17 页后面，总时长回到约 14 分钟。

---

## 五、可能被追问的问题（建议提前准备）

1. **为什么用多进程而不是多线程？**
   题目要求"每来一个客户端生成一个新进程"；另外进程隔离彻底，一个会话崩溃不影响其他人，也不用加锁。代价是共享状态必须走 IPC、创建开销更大。
2. **进程之间怎么共享"谁在线"？**
   不共享。用户表只放在主进程里，子进程通过 `socketpair` 把请求交给主进程，由主进程统一裁决和转发。
3. **僵尸进程怎么处理？**
   `SIGCHLD` 处理函数里 `while (waitpid(-1, NULL, WNOHANG) > 0);`，把退出的子进程全部回收。
4. **fork 之后为什么两边都要 `close`？**
   描述符表被复制了两份，两个进程都持有同一个连接的引用。父进程 `close(cfd)` 只是减引用计数，连接仍由子进程持有；子进程则要关掉继承来的监听套接字和其它会话的通道，否则端口和连接都释放不掉。
5. **为什么子进程用 `_exit()`？**
   不执行 `atexit`、不刷新父进程留下的 stdio 缓冲区，避免输出重复。
6. **TCP 是字节流，怎么保证一条消息完整？**
   自定义帧：3 字节帧头（类型 + 2 字节长度）+ 负载；`read_full` 循环读满，处理 EINTR 和短读。
7. **客户端发一半就断线怎么办？**
   子进程 `read` 返回 0（EOF），向主进程发 `QUIT`，主进程清理槽位并广播"离开"，子进程退出后被回收。
8. **最多支持多少连接？**
   代码里 `CHAT_MAX_CLIENTS = 64`；实际还受 `ulimit -n` 限制，可以用 `ulimit -n` 查看。
9. **能不能改成 epoll / 线程池？**
   可以，架构不变：把 `fork` 换成 `pthread_create`、把 `select` 换成 `epoll`，`socketpair` 换成共享内存或消息队列。PPT 第 18 页就是这个对比。

---

## 六、已验证的内容

- 源码用 `gcc -Wall -Wextra -O2 -std=gnu99` 编译，**零警告**；
- `scripts/smoke_test.py` **12/12 通过**（登录/查重/广播/私聊隔离/在线列表/非法昵称/退出通知/退出后仍可接入）；
- `docs/demo_transcript.txt` 是 1 服务端 + 3 客户端真实运行的输出（PPT 第 15、16 页即取自这里，里面的 pid 都能对上）；
- `docs/ps_tree.txt` 是同一时刻 `ps -ef` 的实测结果，三个子进程 PPID 全部指向主进程。

> 代码在 Windows 上的 Cygwin（gcc 4.5.3）环境中编译并运行验证过；Linux 下 `make` 即可，源码不需要改动。

---

## 七、重新生成 PPT

改了讲稿或重新录了演示数据后：

```sh
python tools/build_deck.py
```

脚本会读取 `文化风PPT模板.pptx` 的封面/目录/章节页/结束页样式与 `chat/docs` 下的真实数据，重新生成 `多进程并发聊天程序_演示.pptx`（需要 `python-pptx`）。
