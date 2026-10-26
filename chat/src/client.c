/*
 * client.c —— 聊天室客户端
 *
 * 客户端要做两件事：读键盘、读套接字。两者都可能随时"有动静"，
 * 所以用 select() 同时等待：既不会因为等待输入而错过别人的消息，
 * 也不会因为等待消息而没法输入。
 *
 * 用法：
 *   ./client -n 昵称 [-s 服务器地址] [-p 端口]
 *
 * 支持的输入：
 *   直接输入文字        群聊（所有人可见）
 *   /list  或 /who      查看在线用户
 *   /msg 昵称 内容      私聊（也可写作 /w 昵称 内容）
 *   /nick 新昵称        改名
 *   /help               查看帮助
 *   /quit  或 /exit     退出
 */
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "protocol.h"

#define INPUT_MAX 4096

static int g_closing = 0;      /* 已发出退出请求，等待服务端 BYE */

static void usage(const char *prog)
{
    fprintf(stderr,
            "用法：%s -n 昵称 [-s 服务器地址] [-p 端口]\n"
            "  -n  昵称（必填，1~%d 字节）\n"
            "  -s  服务器地址，默认 0.0.0.0（表示本机）\n"
            "  -p  端口，默认 %d\n", prog, CHAT_NICK_MAX - 1, CHAT_DEFAULT_PORT);
}

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

static void print_help(void)
{
    printf(
        "\n---------------- 聊天室命令 ----------------\n"
        "  直接输入文字      群聊，所有在线用户都能看到\n"
        "  /list  /who       查看当前在线用户\n"
        "  /msg 昵称 内容    给指定用户发私聊（/w 亦可）\n"
        "  /nick 新昵称      修改自己的昵称\n"
        "  /help  /?         显示本帮助\n"
        "  /quit  /exit      退出聊天室\n"
        "--------------------------------------------\n\n");
    fflush(stdout);
}

/* 建立一个到服务器的 TCP 连接，失败返回 -1 */
static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *p;
    int fd = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;       /* IPv4 / IPv6 都支持 */
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0)
        return -1;

    for (p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;                     /* 连上了 */
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* 把一行输入解析成命令帧并发送 */
static void submit_line(int sock, char *line)
{
    char *cmd;
    char *arg;

    trim(line);
    if (line[0] == '\0')
        return;

    if (line[0] != '/') {                          /* 普通文字 -> 群聊 */
        frame_send(sock, FRAME_TALK, line, strlen(line));
        return;
    }

    /* 拆出命令名和参数 */
    cmd = line + 1;
    arg = cmd;
    while (*arg != '\0' && *arg != ' ' && *arg != '\t')
        arg++;
    if (*arg != '\0') {
        *arg++ = '\0';
        while (*arg == ' ' || *arg == '\t')
            arg++;
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_help();
    } else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "who") == 0) {
        frame_send(sock, FRAME_LIST, "", 0);
    } else if (strcmp(cmd, "msg") == 0 || strcmp(cmd, "w") == 0) {
        char payload[CHAT_MSG_MAX + CHAT_NICK_MAX];
        char *nick = arg;
        char *text;

        while (*arg != '\0' && *arg != ' ' && *arg != '\t')
            arg++;
        if (*arg == '\0') {
            printf("用法：/msg 昵称 内容\n");
            return;
        }
        *arg++ = '\0';
        while (*arg == ' ' || *arg == '\t')
            arg++;
        text = arg;
        if (*nick == '\0' || *text == '\0') {
            printf("用法：/msg 昵称 内容\n");
            return;
        }
        snprintf(payload, sizeof payload, "%s\t%s", nick, text);
        frame_send(sock, FRAME_PRIVATE, payload, strlen(payload));
    } else if (strcmp(cmd, "nick") == 0) {
        if (*arg == '\0')
            printf("用法：/nick 新昵称\n");
        else
            frame_send(sock, FRAME_LOGIN, arg, strlen(arg));
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        frame_send(sock, FRAME_QUIT, "", 0);
        g_closing = 1;
    } else {
        printf("未知命令：/%s，输入 /help 查看帮助\n", cmd);
    }
}

int main(int argc, char **argv)
{
    const char *host = "0.0.0.0";
    const char *port = "8888";
    const char *nick = NULL;
    char nickbuf[CHAT_NICK_MAX];
    char line[INPUT_MAX];
    size_t line_len = 0;
    int sock;
    int opt;

    while ((opt = getopt(argc, argv, "n:s:p:h")) != -1) {
        switch (opt) {
        case 'n':
            nick = optarg;
            break;
        case 's':
            host = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    /*
     * 默认地址是 0.0.0.0，它的语义是"本机任意地址"。
     * Linux 允许直接把它当连接目标（自动走回环），但 Windows 会拒绝，
     * 所以统一映射成回环地址，两个平台行为一致。
     */
    if (strcmp(host, "0.0.0.0") == 0)
        host = "127.0.0.1";

    sock = connect_to(host, port);
    if (sock < 0) {
        fprintf(stderr, "无法连接服务器 %s:%s\n", host, port);
        return 1;
    }

    /* 没有用 -n 给昵称时，先交互式问一次 */
    if (nick == NULL) {
        printf("请输入昵称: ");
        fflush(stdout);
        if (fgets(nickbuf, sizeof nickbuf, stdin) == NULL) {
            close(sock);
            return 1;
        }
        trim(nickbuf);
        nick = nickbuf;
    }
    frame_send(sock, FRAME_LOGIN, nick, strlen(nick));

    printf("已连接 %s:%s，输入 /help 查看命令。\n", host, port);

    for (;;) {
        fd_set rfds;
        int maxfd = sock;
        int n;

        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        if (!g_closing)
            FD_SET(STDIN_FILENO, &rfds);

        n = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        /* 1) 键盘输入：按行切分后逐条发送 */
        if (!g_closing && FD_ISSET(STDIN_FILENO, &rfds)) {
            char buf[512];
            ssize_t r = read(STDIN_FILENO, buf, sizeof buf);
            ssize_t i;

            if (r <= 0) {                        /* Ctrl+D：当作退出 */
                frame_send(sock, FRAME_QUIT, "", 0);
                g_closing = 1;
            } else {
                for (i = 0; i < r; i++) {
                    char ch = buf[i];

                    if (ch == '\n' || ch == '\r') {
                        line[line_len] = '\0';
                        if (line_len > 0)
                            submit_line(sock, line);
                        line_len = 0;
                    } else if (line_len + 1 < sizeof line) {
                        line[line_len++] = ch;
                    } else {
                        line_len = 0;            /* 太长，整行丢弃 */
                        printf("!! 输入过长，已忽略\n");
                    }
                }
            }
        }

        /* 2) 服务端消息：FRAME_TEXT 是可直接打印的整行文本 */
        if (FD_ISSET(sock, &rfds)) {
            static char payload[CHAT_LINE_MAX + 1024];
            uint8_t type;
            size_t len = 0;
            int r = frame_recv(sock, &type, payload, sizeof payload, &len);

            if (r <= 0)
                break;                           /* 服务端关闭连接 */
            if (type == FRAME_BYE) {
                printf("*** 连接已结束，再见！\n");
                break;
            }
            fputs(payload, stdout);
            fflush(stdout);
        }
    }

    close(sock);
    return 0;
}
