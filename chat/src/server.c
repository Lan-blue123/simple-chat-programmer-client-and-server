/*
 * server.c —— 多进程并发聊天服务器
 *
 * 进程模型（本演示的核心）：
 *
 *                        +------------------+
 *   客户端 A <---TCP---> |  子进程 pid=1001 | <--socketpair--> +
 *                        +------------------+                 |
 *                        +------------------+                 |    +-------------------+
 *   客户端 B <---TCP---> |  子进程 pid=1002 | <--socketpair--> +--> | 主进程（accept）   |
 *                        +------------------+                 |    | 用户表 / 消息路由  |
 *                        +------------------+                 |    +-------------------+
 *   客户端 C <---TCP---> |  子进程 pid=1003 | <--socketpair--> +
 *                        +------------------+
 *
 *   · 主进程只做两件事：accept() 新连接、在 select() 里集中转发消息。
 *   · 每来一个客户端就 fork() 一个专属子进程，由它独占该连接的收发与呈现。
 *   · 父子进程之间的控制通道用 socketpair()（AF_UNIX 全双工）实现 IPC；
 *     因为进程地址空间相互独立，用户表只能由主进程统一维护。
 *   · 子进程退出后由主进程通过 SIGCHLD 处理函数 waitpid() 回收，避免僵尸进程。
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"
#include "dash.h"

/* 渲染缓冲区大小：一行带颜色转义序列的文本 */
#define RENDER_MAX (CHAT_LINE_MAX + 1024)

/* ---------------- ANSI 颜色 ---------------- */
#define CLR_RESET "\033[0m"
#define CLR_TIME  "\033[90m"
#define CLR_NICK  "\033[1;36m"
#define CLR_PRIV  "\033[1;35m"
#define CLR_SYS   "\033[33m"
#define CLR_ERR   "\033[1;31m"
#define CLR_HEAD  "\033[1;32m"

/* ---------------- 全局状态（主进程） ---------------- */
typedef struct {
    int   fd;                    /* 与子进程通信的 socketpair 父端；-1 表示空槽 */
    pid_t pid;                   /* 子进程 pid */
    char  nick[CHAT_NICK_MAX];   /* 昵称，登录后有效 */
    char  peer[80];              /* 对端地址，仅用于日志 */
    int   ready;                 /* 1 表示已完成登录 */
    int   gui;                   /* 1 表示图形客户端（LOGIN 带 "\tgui" 标记） */
    time_t start;                /* 接入时间，用于算在线时长 */
    unsigned long in_msgs;       /* 该会话收到的请求数 */
    unsigned long out_msgs;      /* 该会话下发的消息数 */
} client_slot;

static int         g_listen_fd = -1;
static client_slot g_clients[CHAT_MAX_CLIENTS];
static volatile sig_atomic_t g_stop = 0;
static int         g_color = 1;   /* 是否输出 ANSI 颜色（CHAT_NO_COLOR=1 关闭） */

/* 看板与统计 */
static int         g_dash = 0;
static int         g_total_fork = 0;
static int         g_total_reap = 0;
static volatile int g_last_reap_pid = 0;
static volatile int g_reap_seq = 0;

/* ---------------- 小工具 ---------------- */

/* 前置声明（定义在后面） */
static size_t build_user_list(char *buf, size_t cap);
static void broadcast_users_to_gui(void);

static const char *clr(const char *code)
{
    return g_color ? code : "";
}

/* 向缓冲区追加格式化内容，返回新的写入位置，始终保证不越界 */
static size_t appendf(char *buf, size_t cap, size_t off, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (off >= cap)
        return cap;
    va_start(ap, fmt);
    n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0)
        return off;
    if ((size_t)n >= cap - off)
        return cap;
    return off + (size_t)n;
}

static void now_str(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm tmv;

    localtime_r(&t, &tmv);
    if (strftime(buf, n, "%H:%M:%S", &tmv) == 0)
        snprintf(buf, n, "--:--:--");
}

static void srv_log(const char *fmt, ...)
{
    char ts[16];
    char line[CHAT_LINE_MAX + 64];
    char msg[CHAT_LINE_MAX];
    va_list ap;
    int n;

    if (g_dash)                 /* 看板模式下不写 stderr，否则会把界面冲乱 */
        return;

    now_str(ts, sizeof ts);
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    /* 拼好整行再一次性 write，避免父子进程写同一 stderr 时行内交织 */
    n = snprintf(line, sizeof line, "[服务端 %s] %s\n", ts, msg);
    if (n > 0) {
        size_t len = ((size_t)n < sizeof line) ? (size_t)n : sizeof line - 1;

        (void)write(STDERR_FILENO, line, len);
    }
}

/* 去掉首尾空白 */
static void trim(char *s)
{
    char *p = s;
    size_t n;

    while (*p != '\0' && isspace((unsigned char)*p))
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);

    n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

/*
 * 把用户正文里的控制字符（ESC 等）换成空格。
 * 为什么必须做：服务端会原样把消息渲染进其他客户端的终端、也画进看板，
 * 如果消息里带着 ANSI 转义序列，就有可能改别人的终端显示（终端注入）。
 * 注意只处理 <0x20 和 0x7F，UTF-8 多字节字符（>=0x80）不受影响。
 */
static void sanitize_text(char *s)
{
    unsigned char *p = (unsigned char *)s;

    for (; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7F)
            *p = ' ';
    }
}


/* 把 payload 就地按 '\t' 切成最多 max 段，返回段数 */
static int split_tabs(char *s, char *parts[], int max)
{
    int n = 0;

    if (s == NULL)
        return 0;
    parts[n++] = s;
    while (n < max) {
        char *p = strchr(parts[n - 1], '\t');

        if (p == NULL)
            break;
        *p = '\0';
        parts[n++] = p + 1;
    }
    return n;
}

/* ---------------- 发送封装 ---------------- */

static int slot_send(int idx, uint8_t type, const char *payload)
{
    size_t len;

    if (idx < 0 || idx >= CHAT_MAX_CLIENTS)
        return -1;
    if (g_clients[idx].fd < 0)
        return -1;

    len = (payload != NULL) ? strlen(payload) : 0;
    g_clients[idx].out_msgs++;
    return frame_send(g_clients[idx].fd, type, payload, len);
}

/* 群发；except 为要跳过的槽位（-1 表示不跳过） */
static void broadcast_raw(uint8_t type, const char *payload, int except)
{
    int i;

    for (i = 0; i < CHAT_MAX_CLIENTS; i++) {
        if (i == except)
            continue;
        if (g_clients[i].fd < 0 || !g_clients[i].ready)
            continue;
        slot_send(i, type, payload);
    }
}

static void sendf(int idx, uint8_t type, const char *fmt, ...)
{
    char buf[CHAT_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    slot_send(idx, type, buf);
}

#define send_sysf(idx, ...) sendf((idx), FRAME_SYS, __VA_ARGS__)
#define send_errf(idx, ...) sendf((idx), FRAME_ERR, __VA_ARGS__)

static void broadcastf(uint8_t type, int except, const char *fmt, ...)
{
    char buf[CHAT_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    broadcast_raw(type, buf, except);
}

/* ---------------- 用户表维护 ---------------- */

static int count_ready(void)
{
    int i, n = 0;

    for (i = 0; i < CHAT_MAX_CLIENTS; i++)
        if (g_clients[i].fd >= 0 && g_clients[i].ready)
            n++;
    return n;
}

static int find_by_nick(const char *nick)
{
    int i;

    for (i = 0; i < CHAT_MAX_CLIENTS; i++) {
        if (g_clients[i].fd >= 0 && g_clients[i].ready &&
            strcmp(g_clients[i].nick, nick) == 0)
            return i;
    }
    return -1;
}

/* 昵称规则：1~31 字节，不能含空白或控制字符（允许中文等 UTF-8 字节） */
static int nick_valid(const char *nick)
{
    size_t i, n = strlen(nick);

    if (n == 0 || n >= CHAT_NICK_MAX)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)nick[i];

        if (c <= 0x20 || c == 0x7F)
            return 0;
    }
    return 1;
}

/*
 * 关闭一个会话槽位。
 * 注意：主进程 close 控制通道后，子进程的 frame_recv 会读到 EOF 从而退出，
 * 随后由 SIGCHLD 处理函数回收，不会留下僵尸进程。
 */
static void drop_client(int idx, const char *reason)
{
    client_slot *c = &g_clients[idx];
    char text[CHAT_LINE_MAX];

    if (c->fd < 0)
        return;

    close(c->fd);
    c->fd = -1;

    if (c->ready) {
        snprintf(text, sizeof text, "「%s」离开了聊天室（%s），当前在线 %d 人",
                 c->nick, reason, count_ready() - 1);
        c->ready = 0;
        broadcast_raw(FRAME_SYS, text, idx);
        srv_log("用户 %s 下线：%s", c->nick, reason);
        dash_eventf(DASH_EV_LEAVE, "%s 下线（%s），子进程 %d 即将退出",
                    c->nick, reason, (int)c->pid);
        broadcast_users_to_gui();        /* 让 GUI 的在线列表立刻少一个人 */
    }
    c->nick[0] = '\0';
    c->pid = 0;
}

/* ---------------- 主进程：处理来自子进程的请求 ---------------- */

static void handle_login(int idx, char *payload)
{
    client_slot *c = &g_clients[idx];
    char nick[CHAT_NICK_MAX];
    char old[CHAT_NICK_MAX];
    char *tab = strchr(payload, '\t');
    int gui = 0;

    /* GUI 客户端会在昵称后面带 "\tgui"，这里剥掉并记下会话类型 */
    if (tab != NULL) {
        *tab = '\0';
        gui = (strcmp(tab + 1, "gui") == 0);
    }
    c->gui = gui;

    trim(payload);
    snprintf(nick, sizeof nick, "%s", payload);

    if (!nick_valid(nick)) {
        send_errf(idx, "昵称不合法：长度 1~%d 字节，且不能含空白字符", CHAT_NICK_MAX - 1);
        slot_send(idx, FRAME_BYE, "");
        drop_client(idx, "昵称不合法");
        return;
    }
    if (find_by_nick(nick) >= 0 && strcmp(nick, c->nick) != 0) {
        send_errf(idx, "昵称「%s」已被占用，请换一个", nick);
        slot_send(idx, FRAME_BYE, "");
        drop_client(idx, "昵称重复");
        return;
    }

    if (c->ready) {                       /* 已登录：视为改名 */
        snprintf(old, sizeof old, "%s", c->nick);
        snprintf(c->nick, sizeof c->nick, "%s", nick);
        srv_log("用户改名：%s -> %s", old, c->nick);
        dash_eventf(DASH_EV_LOGIN, "子进程 %d 改名：%s → %s",
                    (int)c->pid, old, c->nick);
        send_sysf(idx, "改名成功，你现在叫「%s」", c->nick);
        broadcastf(FRAME_SYS, idx, "「%s」改名为「%s」", old, c->nick);
        broadcast_users_to_gui();
        return;
    }

    snprintf(c->nick, sizeof c->nick, "%s", nick);
    c->ready = 1;
    srv_log("子进程 pid=%d 完成登录，昵称=%s，来自 %s",
            (int)c->pid, c->nick, c->peer);
    dash_eventf(DASH_EV_LOGIN, "子进程 %d 登录成功：昵称=%s（当前在线 %d 人）",
                (int)c->pid, c->nick, count_ready());

    send_sysf(idx, "欢迎你，%s！输入 /help 查看命令，/quit 退出聊天室。", c->nick);
    broadcastf(FRAME_SYS, idx, "「%s」加入了聊天室，当前在线 %d 人",
               c->nick, count_ready());
    broadcast_users_to_gui();
}

static void handle_talk(int idx, char *payload)
{
    char ts[16];
    char line[CHAT_LINE_MAX];

    if (!g_clients[idx].ready) {
        send_errf(idx, "请先登录（发送昵称）");
        return;
    }
    trim(payload);
    sanitize_text(payload);           /* 防终端转义注入 */
    if (payload[0] == '\0') {
        send_errf(idx, "不能发送空消息");
        return;
    }

    now_str(ts, sizeof ts);
    snprintf(line, sizeof line, "%s\t%s\t%s", ts, g_clients[idx].nick, payload);
    broadcast_raw(FRAME_SAY, line, -1);       /* 群聊：发给所有人，含自己 */
    dash_eventf(DASH_EV_TALK, "%s: %.60s  → 广播给 %d 人",
                g_clients[idx].nick, payload, count_ready());
}

static void handle_private(int idx, char *payload)
{
    char *tab = strchr(payload, '\t');
    char *target;
    char *text;
    char ts[16];
    char line[CHAT_LINE_MAX];
    int t;

    if (!g_clients[idx].ready) {
        send_errf(idx, "请先登录（发送昵称）");
        return;
    }
    if (tab == NULL) {
        send_errf(idx, "私聊格式错误，用法：/msg 昵称 内容");
        return;
    }
    *tab = '\0';
    target = payload;
    text = tab + 1;
    trim(target);
    trim(text);
    sanitize_text(target);            /* 昵称不许带控制符 */
    sanitize_text(text);              /* 正文同理，防终端转义注入 */

    if (target[0] == '\0' || text[0] == '\0') {
        send_errf(idx, "私聊格式错误，用法：/msg 昵称 内容");
        return;
    }
    t = find_by_nick(target);
    if (t < 0) {
        send_errf(idx, "用户「%s」不在线", target);
        return;
    }
    if (t == idx) {
        send_errf(idx, "不要给自己发私聊哦");
        return;
    }

    now_str(ts, sizeof ts);
    snprintf(line, sizeof line, "%s\t%s\t%s", ts, g_clients[idx].nick, text);
    slot_send(t, FRAME_WHISPER, line);
    send_sysf(idx, "[私聊] 你 -> %s：%s", g_clients[t].nick, text);
    dash_eventf(DASH_EV_PRIVATE, "%s → %s: %.50s",
                g_clients[idx].nick, g_clients[t].nick, text);
}

/* 拼一份在线昵称列表（'\n' 分隔） */
static size_t build_user_list(char *buf, size_t cap)
{
    size_t off = 0;
    int i;

    buf[0] = '\0';
    for (i = 0; i < CHAT_MAX_CLIENTS; i++) {
        if (g_clients[i].fd < 0 || !g_clients[i].ready)
            continue;
        off = appendf(buf, cap, off, "%s\n", g_clients[i].nick);
    }
    return off;
}

/*
 * 在线名单变化（登录/改名/离开）时，主动把最新列表推给所有 GUI 会话。
 * 命令行客户端不推，避免它们的终端被列表刷屏。
 */
static void broadcast_users_to_gui(void)
{
    char buf[CHAT_LINE_MAX * 2];
    int i;

    build_user_list(buf, sizeof buf);
    for (i = 0; i < CHAT_MAX_CLIENTS; i++) {
        if (g_clients[i].fd < 0 || !g_clients[i].ready || !g_clients[i].gui)
            continue;
        slot_send(i, FRAME_USERS, buf);
    }
}

static void handle_list(int idx)
{
    char buf[CHAT_LINE_MAX * 2];

    if (!g_clients[idx].ready) {
        send_errf(idx, "请先登录（发送昵称）");
        return;
    }
    build_user_list(buf, sizeof buf);
    slot_send(idx, FRAME_USERS, buf);
    dash_eventf(DASH_EV_LIST, "%s 查询在线列表（%d 人）",
                g_clients[idx].nick, count_ready());
}

static void handle_request(int idx, uint8_t type, char *payload, size_t len)
{
    (void)len;

    switch (type) {
    case FRAME_LOGIN:
        handle_login(idx, payload);
        break;
    case FRAME_TALK:
        handle_talk(idx, payload);
        break;
    case FRAME_PRIVATE:
        handle_private(idx, payload);
        break;
    case FRAME_LIST:
        handle_list(idx);
        break;
    case FRAME_QUIT:
        slot_send(idx, FRAME_BYE, "");
        drop_client(idx, "主动退出");
        break;
    default:
        send_errf(idx, "无法识别的请求类型 %d", (int)type);
        break;
    }
}

/* ---------------- 子进程：渲染并交付给客户端 ---------------- */

static void send_text(int fd, const char *text)
{
    frame_send(fd, FRAME_TEXT, text, strlen(text));
}

/*
 * 把服务端语义帧渲染成"给人看的一行文本"。
 * 这一步放在子进程里做：既分担了主进程的工作，又让主进程只处理消息逻辑。
 */
static void render_and_send(int fd, uint8_t type, const char *payload)
{
    char out[RENDER_MAX];
    char tmp[CHAT_LINE_MAX];
    char *parts[3];
    size_t off = 0;
    int n;

    switch (type) {
    case FRAME_SAY:
    case FRAME_WHISPER:
        snprintf(tmp, sizeof tmp, "%s", payload);
        n = split_tabs(tmp, parts, 3);
        if (n < 3) {
            off = appendf(out, sizeof out, off, "%s*** %s%s\n",
                          clr(CLR_SYS), payload, clr(CLR_RESET));
            break;
        }
        if (type == FRAME_SAY) {
            off = appendf(out, sizeof out, off, "%s[%s]%s %s<%s>%s %s\n",
                          clr(CLR_TIME), parts[0], clr(CLR_RESET),
                          clr(CLR_NICK), parts[1], clr(CLR_RESET), parts[2]);
        } else {
            off = appendf(out, sizeof out, off, "%s[%s]%s %s%s 私聊->我 %s%s\n",
                          clr(CLR_TIME), parts[0], clr(CLR_RESET),
                          clr(CLR_PRIV), parts[1], clr(CLR_RESET), parts[2]);
        }
        break;

    case FRAME_SYS:
        off = appendf(out, sizeof out, off, "%s*** %s%s\n",
                      clr(CLR_SYS), payload, clr(CLR_RESET));
        break;

    case FRAME_ERR:
        off = appendf(out, sizeof out, off, "%s!!! %s%s\n",
                      clr(CLR_ERR), payload, clr(CLR_RESET));
        break;

    case FRAME_USERS: {
        const char *p = payload;
        int cnt = 0;

        off = appendf(out, sizeof out, off, "\n%s=== 在线用户 ===%s\n",
                      clr(CLR_HEAD), clr(CLR_RESET));
        while (*p != '\0') {
            const char *nl = strchr(p, '\n');
            size_t l = (nl != NULL) ? (size_t)(nl - p) : strlen(p);

            if (l > 0) {
                cnt++;
                off = appendf(out, sizeof out, off, "  %2d. %.*s\n",
                              cnt, (int)l, p);
            }
            if (nl == NULL)
                break;
            p = nl + 1;
        }
        off = appendf(out, sizeof out, off, "%s共 %d 人在线%s\n\n",
                      clr(CLR_HEAD), cnt, clr(CLR_RESET));
        break;
    }

    default:
        off = appendf(out, sizeof out, off, "*** %s\n", payload);
        break;
    }
    send_text(fd, out);
}

/*
 * 子进程主体：只负责 connfd 这一个客户端。
 * 它同时监听两条通道：
 *   connfd  客户端发来的请求 —— 直接转发给主进程
 *   ctlfld  主进程发来的消息 —— 渲染后写给客户端
 * 这样"多进程"才真正并行：某个客户端卡住/慢，只影响它自己的子进程。
 */
static void child_session(int connfd, int ctlfld, const char *peer)
{
    char payload[RENDER_MAX];
    int closing = 0;
    int gui_mode = 0;          /* 1 = GUI 客户端，直接转发结构化帧 */
    int i;

    /* 子进程继承了主进程的全部描述符，必须及时关闭，否则会拖住连接、造成泄漏 */
    close(g_listen_fd);
    for (i = 0; i < CHAT_MAX_CLIENTS; i++) {
        if (g_clients[i].fd >= 0)
            close(g_clients[i].fd);
    }

    signal(SIGCHLD, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);

    srv_log("子进程 pid=%d 开始为 %s 服务", (int)getpid(), peer);

    for (;;) {
        fd_set rfds;
        int maxfd = ctlfld;
        int n;
        uint8_t type;
        size_t len = 0;

        FD_ZERO(&rfds);
        FD_SET(ctlfld, &rfds);
        if (!closing) {
            FD_SET(connfd, &rfds);
            if (connfd > maxfd)
                maxfd = connfd;
        }

        n = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        /* 1) 主进程 -> 子进程：有广播/私聊/提示需要转发给客户端 */
        if (FD_ISSET(ctlfld, &rfds)) {
            int r = frame_recv(ctlfld, &type, payload, sizeof payload, &len);

            if (r <= 0)
                break;                       /* 主进程关闭了控制通道 */
            if (type == FRAME_BYE) {         /* 交给客户端自己打印告别语 */
                frame_send(connfd, FRAME_BYE, "", 0);
                break;
            }
            if (gui_mode)                    /* GUI 客户端自己渲染，原样转发结构化帧 */
                frame_send(connfd, type, payload, len);
            else
                render_and_send(connfd, type, payload);
            continue;                        /* 回到 select，避免饿死另一条通道 */
        }

        /* 2) 客户端 -> 子进程：请求原样上交，由主进程统一裁决 */
        if (!closing && FD_ISSET(connfd, &rfds)) {
            int r = frame_recv(connfd, &type, payload, sizeof payload, &len);

            if (r == 0) {                    /* 客户端断开（EOF） */
                frame_send(ctlfld, FRAME_QUIT, "", 0);
                closing = 1;                 /* 继续等待主进程回 BYE 再退出 */
                continue;
            }
            if (r < 0) {                     /* 协议错误或帧超长 */
                frame_send(ctlfld, FRAME_QUIT, "", 0);
                break;
            }
            /* GUI 客户端在 LOGIN 载荷末尾带 "\tgui"：
               子进程据此切到"下发结构化帧"模式；载荷原样上交，
               由主进程剥掉标记并记住这是个图形客户端（用于主动推在线列表） */
            if (type == FRAME_LOGIN) {
                char *tab = strchr(payload, '\t');

                if (tab != NULL && strcmp(tab + 1, "gui") == 0)
                    gui_mode = 1;
            }
            if (frame_send(ctlfld, type, payload, len) <= 0)
                break;                       /* 主进程没了 */
        }
    }

    close(connfd);
    close(ctlfld);
    srv_log("子进程 pid=%d 结束", (int)getpid());
}

/* ---------------- 信号处理 ---------------- */

/* 回收所有已退出的子进程，防止僵尸进程堆积 */
static void on_sigchld(int sig)
{
    pid_t pid;

    (void)sig;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        g_last_reap_pid = (int)pid;
        g_reap_seq++;
        g_total_reap++;
    }
}

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---------------- 主进程：接受新连接 ---------------- */

static void do_accept(void)
{
    struct sockaddr_storage ss;
    socklen_t sslen = sizeof ss;
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    char peer[80];
    int cfd;
    int sv[2];
    int idx = -1;
    int i;
    pid_t pid;

    cfd = accept(g_listen_fd, (struct sockaddr *)&ss, &sslen);
    if (cfd < 0) {
        if (errno == EINTR)
            return;
        srv_log("accept 失败：%s", strerror(errno));
        return;
    }

    if (getnameinfo((struct sockaddr *)&ss, sslen, host, sizeof host,
                    serv, sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        snprintf(host, sizeof host, "?");
        snprintf(serv, sizeof serv, "?");
    }
    snprintf(peer, sizeof peer, "%s:%s", host, serv);

    /* 找一个空闲槽位 */
    for (i = 0; i < CHAT_MAX_CLIENTS; i++) {
        if (g_clients[i].fd < 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        static const char full_msg[] = "服务器已满，请稍后再试";

        frame_send(cfd, FRAME_ERR, full_msg, sizeof full_msg - 1);
        frame_send(cfd, FRAME_BYE, "", 0);
        close(cfd);
        srv_log("拒绝连接 %s：已达最大在线数 %d", peer, CHAT_MAX_CLIENTS);
        return;
    }

    /* 控制通道：AF_UNIX 全双工，父子进程各持一端 */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        srv_log("socketpair 失败：%s", strerror(errno));
        close(cfd);
        return;
    }

    fflush(NULL);                 /* fork 前清空 stdio 缓冲，避免子进程重复输出 */
    pid = fork();
    if (pid < 0) {
        srv_log("fork 失败：%s", strerror(errno));
        close(cfd);
        close(sv[0]);
        close(sv[1]);
        return;
    }

    if (pid == 0) {
        /* ============ 子进程 ============ */
        close(sv[0]);             /* 子进程用不到父端 */
        if (g_dash) {             /* 看板模式下子进程不能往终端写日志 */
            int devnull = open("/dev/null", O_WRONLY);

            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                if (devnull != STDERR_FILENO)
                    close(devnull);
            }
        }
        child_session(cfd, sv[1], peer);
        _exit(0);                 /* 用 _exit 避免执行父进程的 atexit/刷新逻辑 */
    }

    /* ============ 父进程 ============ */
    close(cfd);                   /* 连接完全交给子进程，父进程不再持有 */
    close(sv[1]);

    g_clients[idx].fd = sv[0];
    g_clients[idx].pid = pid;
    g_clients[idx].ready = 0;
    g_clients[idx].nick[0] = '\0';
    g_clients[idx].start = time(NULL);
    g_clients[idx].in_msgs = 0;
    g_clients[idx].out_msgs = 0;
    snprintf(g_clients[idx].peer, sizeof g_clients[idx].peer, "%s", peer);

    g_total_fork++;
    dash_eventf(DASH_EV_FORK, "接受 %s → fork 子进程 pid=%d（槽位 %d）",
                peer, (int)pid, idx);
    srv_log("接受连接 %s，创建子进程 pid=%d，占用槽位 %d",
            peer, (int)pid, idx);
}

/* ---------------- 收尾 ---------------- */

/* 把当前用户表拍成快照交给看板 */
static void dash_refresh(int port)
{
    dash_user_t users[CHAT_MAX_CLIENTS];
    int n = 0;
    int i;
    time_t now = time(NULL);

    if (!g_dash)
        return;

    for (i = 0; i < CHAT_MAX_CLIENTS; i++) {
        if (g_clients[i].fd < 0)
            continue;
        users[n].slot = i;
        users[n].pid = (int)g_clients[i].pid;
        snprintf(users[n].nick, sizeof users[n].nick, "%s",
                 g_clients[i].ready ? g_clients[i].nick : "(未登录)");
        snprintf(users[n].peer, sizeof users[n].peer, "%s", g_clients[i].peer);
        users[n].online_sec = (long)(now - g_clients[i].start);
        users[n].in_msgs = g_clients[i].in_msgs;
        users[n].out_msgs = g_clients[i].out_msgs;
        n++;
    }
    dash_draw(users, n, count_ready(), g_total_fork, g_total_reap,
              port, CHAT_MAX_CLIENTS, n);
}

static void shutdown_all(void)
{
    int i;

    broadcast_raw(FRAME_SYS, "服务器正在关闭，再见！", -1);
    for (i = 0; i < CHAT_MAX_CLIENTS; i++) {
        if (g_clients[i].fd >= 0) {
            frame_send(g_clients[i].fd, FRAME_BYE, "", 0);
            close(g_clients[i].fd);      /* 子进程读到 EOF 后自行退出 */
            g_clients[i].fd = -1;
        }
    }
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    while (waitpid(-1, NULL, 0) > 0)     /* 等所有子进程退出 */
        ;
    srv_log("所有子进程已回收，服务端退出");
    dash_shutdown();
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "用法：%s [-p 端口] [-d]\n"
            "  -p  监听端口，默认 %d\n"
            "  -d  打开实时看板（在线会话 / 子进程 PID / 消息流转事件）\n",
            prog, CHAT_DEFAULT_PORT);
}

int main(int argc, char **argv)
{
    struct sockaddr_in addr;
    struct sigaction sa;
    int port = CHAT_DEFAULT_PORT;
    int on = 1;
    int opt;
    int i;
    int want_dash = 0;
    int last_reap_seq = 0;

    while ((opt = getopt(argc, argv, "p:dh")) != -1) {
        switch (opt) {
        case 'p':
            port = atoi(optarg);
            break;
        case 'd':
            want_dash = 1;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }
    if (port <= 0 || port > 65535) {
        usage(argv[0]);
        return 1;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);            /* 对端已关闭时不要被 SIGPIPE 杀死 */

    if (getenv("CHAT_NO_COLOR") != NULL)
        g_color = 0;

    memset(g_clients, 0, sizeof g_clients);
    for (i = 0; i < CHAT_MAX_CLIENTS; i++)
        g_clients[i].fd = -1;

    /* SIGCHLD：回收子进程。SA_RESTART 让 select 自动重启 */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction SIGCHLD");
        return 1;
    }

    /* SIGINT / SIGTERM：优雅退出（不带 SA_RESTART，让 select 返回 EINTR） */
    sa.sa_handler = on_sigint;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        perror("socket");
        return 1;
    }
    if (setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&on, sizeof on) < 0) {
        perror("setsockopt");
        close(g_listen_fd);
        return 1;
    }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(g_listen_fd);
        return 1;
    }
    if (listen(g_listen_fd, 16) < 0) {
        perror("listen");
        close(g_listen_fd);
        return 1;
    }

    if (want_dash) {
        g_dash = dash_init((int)getpid(), port);
        if (!g_dash)
            fprintf(stderr, "提示：-d 需要交互式终端，当前不是终端，已退回普通日志模式\n");
    }

    srv_log("聊天服务器已启动：pid=%d 端口=%d 最大在线数=%d",
            (int)getpid(), port, CHAT_MAX_CLIENTS);
    srv_log("等待客户端连接……（Ctrl+C 退出）");
    dash_eventf(DASH_EV_SYS, "服务器启动：pid=%d 监听 0.0.0.0:%d",
                (int)getpid(), port);

    /* 主进程的事件循环：一个 select 同时盯住监听套接字和所有控制通道 */
    while (!g_stop) {
        static char payload[RENDER_MAX];
        fd_set rfds;
        int maxfd;
        int n;

        FD_ZERO(&rfds);
        FD_SET(g_listen_fd, &rfds);
        maxfd = g_listen_fd;
        for (i = 0; i < CHAT_MAX_CLIENTS; i++) {
            if (g_clients[i].fd >= 0) {
                FD_SET(g_clients[i].fd, &rfds);
                if (g_clients[i].fd > maxfd)
                    maxfd = g_clients[i].fd;
            }
        }

        if (g_dash) {
            /* 看板模式：500ms 超时一次，用来刷新"运行时长/在线时长" */
            struct timeval tv;

            tv.tv_sec = 0;
            tv.tv_usec = 500000;
            n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        } else {
            n = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            srv_log("select 失败：%s", strerror(errno));
            break;
        }

        /* 有新连接：accept 之后立刻 fork 一个专属子进程 */
        if (n > 0 && FD_ISSET(g_listen_fd, &rfds))
            do_accept();

        /* 已有会话：读一帧请求并分发处理 */
        for (i = 0; i < CHAT_MAX_CLIENTS && !g_stop; i++) {
            uint8_t type;
            size_t len = 0;
            int r;

            if (g_clients[i].fd < 0)
                continue;
            if (n <= 0 || !FD_ISSET(g_clients[i].fd, &rfds))
                continue;

            r = frame_recv(g_clients[i].fd, &type, payload, sizeof payload, &len);
            if (r <= 0) {
                drop_client(i, (r == 0) ? "连接断开" : "协议错误");
                continue;
            }
            g_clients[i].in_msgs++;
            handle_request(i, type, payload, len);
        }

        /* SIGCHLD 处理函数只记了计数和最后一个 pid，这里补一条"回收"事件 */
        if (g_reap_seq != last_reap_seq) {
            last_reap_seq = g_reap_seq;
            dash_eventf(DASH_EV_REAP, "waitpid 回收子进程 pid=%d（已回收 %d 个）",
                        g_last_reap_pid, g_total_reap);
        }

        dash_refresh(port);
    }

    shutdown_all();
    return 0;
}
