/*
 * chat_gui.c —— 聊天室 Windows 图形客户端
 *
 * 特点：
 *   · 单文件、纯 Win32 + Winsock，编译出来就是一个独立 exe，不需要 Python/运行库；
 *   · 每个进程就是一个客户端，所以可以同时开多个窗口（多开）；
 *   · 连接时在 LOGIN 帧里带 "\tgui" 标记，服务端子进程会把结构化帧（SAY/WHISPER/
 *     SYS/ERR/USERS）直接下发，由本程序自己渲染成彩色文本和在线列表。
 *
 * 编译（MinGW-w64）：
 *   gcc -O2 -Wall -mwindows -o chat_gui.exe chat_gui.c -lws2_32 -lgdi32 -luser32
 *
 * 命令行参数（可选，便于批量启动脚本）：
 *   --server 0.0.0.0  --port 8888  --nick 小明  --auto-connect
 *   （0.0.0.0 表示本机；连远程服务器请填具体 IP）
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <richedit.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "../chat/include/protocol.h"

/* 链接时用 -lws2_32（见 build_windows.bat），这里不用 MSVC 的 #pragma comment */

/* ---------------------------------------------------------------- 常量 */

#define WM_APP_FRAME  (WM_APP + 1)
#define WM_APP_CLOSED (WM_APP + 2)

#define IDC_SERVER    1001
#define IDC_PORT      1002
#define IDC_NICK      1003
#define IDC_CONNECT   1004
#define IDC_DISCONNECT 1005
#define IDC_CHAT      1006
#define IDC_USERS     1007
#define IDC_REFRESH   1008
#define IDC_INPUT     1009
#define IDC_SEND      1010
#define IDC_STATUS    1011
#define IDC_TIP       1012

/* 配色（和 PPT 的文化风一致：墨绿 + 描金 + 米白） */
#define COL_BG        RGB(0xF7, 0xF5, 0xEE)
#define COL_GREEN     RGB(0x5A, 0x72, 0x66)
#define COL_GREEN_D   RGB(0x3E, 0x4F, 0x47)
#define COL_GOLD      RGB(0xAF, 0x8F, 0x47)
#define COL_CHAT_BG   RGB(0x24, 0x2E, 0x29)
#define COL_CHAT_TXT  RGB(0xE7, 0xE3, 0xD7)
#define COL_CHAT_DIM  RGB(0x93, 0xA7, 0x9A)
#define COL_CHAT_NICK RGB(0x8F, 0xC4, 0xB0)
#define COL_CHAT_SYS  RGB(0xD9, 0xC8, 0x8E)
#define COL_CHAT_ERR  RGB(0xE3, 0x9A, 0x92)
#define COL_CHAT_PRIV RGB(0xD9, 0xA8, 0xC8)
#define COL_WHITE     RGB(0xFF, 0xFF, 0xFF)

static const wchar_t *APP_TITLE = L"多进程聊天室 · 图形客户端";
static const wchar_t *RICHOOK = L"riched20.dll";

/* ---------------------------------------------------------------- 状态 */

static HINSTANCE   g_inst;
static HWND        g_main, g_server, g_port, g_nick, g_connect, g_disconnect;
static HWND        g_chat, g_users, g_refresh, g_input, g_send, g_status;
static HWND        g_lbl_server, g_lbl_port, g_lbl_nick, g_lbl_users;
static HFONT       g_font, g_font_bold;
static HBRUSH      g_bg_brush;
static int         g_scale = 1;          /* DPI 缩放：96dpi=1，144dpi=1.5 → 取整后使用 */
static SOCKET      g_sock = INVALID_SOCKET;
static HANDLE      g_thread;
static volatile LONG g_connected;
static volatile LONG g_connecting;
static wchar_t     g_my_nick[CHAT_NICK_MAX];
static WNDPROC     g_input_oldproc;

/* 按 DPI 缩放像素值 */
static int sc(int v)
{
    return v * g_scale;
}

typedef struct {
    int  type;
    int  len;
    char text[CHAT_LINE_MAX + 64];
} gui_msg;

/* ---------------------------------------------------------------- 编码 */

static void u8_to_w(const char *s, wchar_t *out, int cap)
{
    if (cap <= 0)
        return;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    out[cap - 1] = L'\0';
}

static void w_to_u8(const wchar_t *s, char *out, int cap)
{
    if (cap <= 0)
        return;
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL);
    out[cap - 1] = '\0';
}

/* 去掉终端颜色转义（服务端文本模式时才会用到） */
static void strip_ansi(char *s)
{
    char *p = s;
    char *o = s;

    while (*p) {
        if (p[0] == '\033' && p[1] == '[') {
            p += 2;
            while (*p && !((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')))
                p++;
            if (*p)
                p++;
            continue;
        }
        *o++ = *p++;
    }
    *o = '\0';
}

/* ---------------------------------------------------------------- 帧 IO */

static int sock_write_all(SOCKET s, const char *buf, int len)
{
    int sent = 0;

    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);

        if (n == SOCKET_ERROR)
            return -1;
        sent += n;
    }
    return 1;
}

static int sock_read_all(SOCKET s, char *buf, int len)
{
    int got = 0;

    while (got < len) {
        int n = recv(s, buf + got, len - got, 0);

        if (n == 0)
            return (got == 0) ? 0 : -1;
        if (n == SOCKET_ERROR)
            return -1;
        got += n;
    }
    return 1;
}

static int gui_frame_send(SOCKET s, unsigned char type, const char *payload, int len)
{
    unsigned char hdr[3];

    if (s == INVALID_SOCKET || len > 0xFFFF)
        return -1;
    hdr[0] = type;
    hdr[1] = (unsigned char)((len >> 8) & 0xFF);
    hdr[2] = (unsigned char)(len & 0xFF);
    if (sock_write_all(s, (const char *)hdr, 3) <= 0)
        return -1;
    if (len > 0 && sock_write_all(s, payload, len) <= 0)
        return -1;
    return 1;
}

static int gui_frame_recv(SOCKET s, unsigned char *type, char *payload, int cap, int *len)
{
    unsigned char hdr[3];
    int plen;
    int r;

    r = sock_read_all(s, (char *)hdr, 3);
    if (r <= 0)
        return r;
    plen = (hdr[1] << 8) | hdr[2];
    if (plen + 1 > cap)
        return -2;
    if (plen > 0) {
        r = sock_read_all(s, payload, plen);
        if (r <= 0)
            return r;
    }
    payload[plen] = '\0';
    *type = hdr[0];
    *len = plen;
    return 1;
}

/* ---------------------------------------------------------------- 聊天区渲染 */

static void chat_append_raw(const wchar_t *text, COLORREF color, int bold)
{
    CHARFORMAT2W cf;
    int end;

    /* 把插入点移到末尾，先设置"接下来要插入的字符"的格式 */
    SendMessageW(g_chat, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    ZeroMemory(&cf, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR | CFM_BOLD;
    cf.crTextColor = color;
    cf.dwEffects = bold ? CFE_BOLD : 0;
    SendMessageW(g_chat, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(g_chat, EM_REPLACESEL, FALSE, (LPARAM)text);

    end = (int)SendMessageW(g_chat, WM_GETTEXTLENGTH, 0, 0);
    SendMessageW(g_chat, EM_SETSEL, (WPARAM)end, (LPARAM)end);
    SendMessageW(g_chat, EM_SCROLLCARET, 0, 0);
}

/* UTF-8 文本 -> 聊天区（自动把 \n 转成 \r\n） */
static void chat_append(const char *utf8, COLORREF color, int bold)
{
    wchar_t w[CHAT_LINE_MAX + 128];
    int i, j = 0;

    u8_to_w(utf8, w, (int)(sizeof w / sizeof w[0]));
    /* 统一换行，RichEdit 需要 \r\n */
    wchar_t out[CHAT_LINE_MAX + 200];

    for (i = 0; w[i] != L'\0' && j < (int)(sizeof out / sizeof out[0]) - 3; i++) {
        if (w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r')) {
            out[j++] = L'\r';
            out[j++] = L'\n';
        } else {
            out[j++] = w[i];
        }
    }
    out[j] = L'\0';
    chat_append_raw(out, color, bold);
}

/* 拆 "a\tb\tc" 形式的载荷 */
static int split_tabs(char *s, char *parts[], int max)
{
    int n = 0;

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

/* ---------------------------------------------------------------- 界面辅助 */

static void set_status(const wchar_t *text)
{
    SetWindowTextW(g_status, text);
}

static void set_connected_ui(int connected)
{
    EnableWindow(g_connect, !connected);
    EnableWindow(g_server, !connected);
    EnableWindow(g_port, !connected);
    EnableWindow(g_nick, !connected);
    EnableWindow(g_disconnect, connected);
    EnableWindow(g_input, connected);
    EnableWindow(g_send, connected);
    EnableWindow(g_refresh, connected);
}

static void fill_users(const char *payload)
{
    char buf[CHAT_LINE_MAX * 2];
    char *p;
    wchar_t w[CHAT_NICK_MAX * 2];
    int count = 0;

    SendMessageW(g_users, LB_RESETCONTENT, 0, 0);
    snprintf(buf, sizeof buf, "%s", payload);
    p = buf;
    while (*p != '\0') {
        char *nl = strchr(p, '\n');

        if (nl != NULL)
            *nl = '\0';
        if (*p != '\0') {
            u8_to_w(p, w, (int)(sizeof w / sizeof w[0]));
            SendMessageW(g_users, LB_ADDSTRING, 0, (LPARAM)w);
            count++;
        }
        if (nl == NULL)
            break;
        p = nl + 1;
    }

    {
        wchar_t status[256];

        _snwprintf(status, 255, L"在线 %d 人 · 双击用户可直接私聊", count);
        status[255] = L'\0';
        set_status(status);
    }
}

/* ---------------------------------------------------------------- 接收线程 */

static DWORD WINAPI recv_thread(LPVOID param)
{
    SOCKET s = (SOCKET)(INT_PTR)param;
    char payload[CHAT_LINE_MAX + 128];
    unsigned char type;
    int len;

    for (;;) {
        gui_msg *m;
        int r = gui_frame_recv(s, &type, payload, (int)sizeof payload, &len);

        if (r == 0)
            break;
        if (r == -2) {
            PostMessageW(g_main, WM_APP_CLOSED, 2, 0);
            break;
        }
        if (r < 0)
            break;

        m = (gui_msg *)malloc(sizeof(gui_msg));
        if (m == NULL)
            continue;
        m->type = type;
        m->len = len;
        snprintf(m->text, sizeof m->text, "%s", payload);
        if (!PostMessageW(g_main, WM_APP_FRAME, (WPARAM)type, (LPARAM)m)) {
            free(m);
            break;
        }
    }

    PostMessageW(g_main, WM_APP_CLOSED, 0, 0);
    return 0;
}

/* ---------------------------------------------------------------- 网络动作 */

static int do_connect(void)
{
    wchar_t wserver[128], wport[16], wnick[CHAT_NICK_MAX];
    char server[128], port[16], nick[CHAT_NICK_MAX * 2];
    char payload[CHAT_NICK_MAX * 2 + 8];
    struct addrinfo hints, *res = NULL, *p;
    SOCKET s = INVALID_SOCKET;
    u_long nonblock;
    fd_set wset, eset;
    struct timeval tv;
    int rc;

    if (InterlockedCompareExchange(&g_connecting, 1, 0) != 0)
        return 0;

    GetWindowTextW(g_server, wserver, 128);
    GetWindowTextW(g_port, wport, 16);
    GetWindowTextW(g_nick, wnick, CHAT_NICK_MAX);
    w_to_u8(wserver, server, sizeof server);
    w_to_u8(wport, port, sizeof port);
    w_to_u8(wnick, nick, sizeof nick);

    /*
     * 0.0.0.0 的语义是"本机任意地址"。Linux 可以直接把它当连接目标，
     * 但 Windows 会拒绝，所以这里统一按"本机"处理，映射成回环地址。
     */
    if (strcmp(server, "0.0.0.0") == 0)
        snprintf(server, sizeof server, "127.0.0.1");

    if (nick[0] == '\0') {
        MessageBoxW(g_main, L"请先填写昵称", L"提示", MB_ICONINFORMATION);
        InterlockedExchange(&g_connecting, 0);
        return 0;
    }

    set_status(L"连接中…");
    EnableWindow(g_connect, FALSE);

    ZeroMemory(&hints, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(server, port, &hints, &res) != 0) {
        set_status(L"连接失败：地址无法解析");
        MessageBoxW(g_main, L"服务器地址无法解析", L"连接失败", MB_ICONERROR);
        EnableWindow(g_connect, TRUE);
        InterlockedExchange(&g_connecting, 0);
        return 0;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;

        /* 非阻塞 connect + select 3 秒超时，避免界面卡死 */
        nonblock = 1;
        ioctlsocket(s, FIONBIO, &nonblock);
        rc = connect(s, p->ai_addr, (int)p->ai_addrlen);
        if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            FD_ZERO(&wset);
            FD_SET(s, &wset);
            eset = wset;
            tv.tv_sec = 3;
            tv.tv_usec = 0;
            rc = select(0, NULL, &wset, &eset, &tv);
            if (rc <= 0 || !FD_ISSET(s, &wset)) {
                closesocket(s);
                s = INVALID_SOCKET;
                continue;
            }
        } else if (rc == SOCKET_ERROR) {
            closesocket(s);
            s = INVALID_SOCKET;
            continue;
        }

        nonblock = 0;
        ioctlsocket(s, FIONBIO, &nonblock);
        break;
    }
    freeaddrinfo(res);

    if (s == INVALID_SOCKET) {
        set_status(L"连接失败：无法建立 TCP 连接");
        MessageBoxW(g_main,
                    L"连不上服务器。\n\n检查：IP/端口是否正确、服务器是否已启动、"
                    L"云服务器安全组是否放行该端口。",
                    L"连接失败", MB_ICONERROR);
        EnableWindow(g_connect, TRUE);
        InterlockedExchange(&g_connecting, 0);
        return 0;
    }

    g_sock = s;
    InterlockedExchange(&g_connected, 1);
    set_connected_ui(1);

    /* LOGIN 载荷带 "\tgui"：服务端子进程据此切换成结构化帧下发 */
    snprintf(payload, sizeof payload, "%s\tgui", nick);
    if (gui_frame_send(s, FRAME_LOGIN, payload, (int)strlen(payload)) <= 0) {
        set_status(L"登录失败：发送失败");
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        InterlockedExchange(&g_connected, 0);
        set_connected_ui(0);
        InterlockedExchange(&g_connecting, 0);
        return 0;
    }
    gui_frame_send(s, FRAME_LIST, "", 0);

    wcscpy(g_my_nick, wnick);
    {
        wchar_t status[256];

        _snwprintf(status, 255, L"已连接 %s:%s · 昵称 %s", wserver, wport, wnick);
        status[255] = L'\0';
        set_status(status);
    }

    g_thread = CreateThread(NULL, 0, recv_thread, (LPVOID)(INT_PTR)s, 0, NULL);
    SetWindowTextW(g_main, L"多进程聊天室 · 图形客户端（已连接）");

    /* 提示语 */
    chat_append("—— 已连接服务器 ——\r\n", COL_CHAT_DIM, 0);
    chat_append("提示：Enter 发送；/list 看在线用户；/msg 昵称 内容 私聊；/nick 新昵称 改名；/help 帮助\r\n",
                COL_CHAT_DIM, 0);

    InterlockedExchange(&g_connecting, 0);
    EnableWindow(g_connect, FALSE);
    return 1;
}

static void do_disconnect(int graceful)
{
    SOCKET s = g_sock;

    if (s == INVALID_SOCKET)
        return;
    if (graceful) {
        gui_frame_send(s, FRAME_QUIT, "", 0);
        Sleep(80);
    }
    closesocket(s);                 /* 让接收线程的 recv 立刻返回错误并退出 */
    g_sock = INVALID_SOCKET;
}

static void send_frame_checked(int type, const char *payload)
{
    if (g_sock == INVALID_SOCKET) {
        chat_append("!! 尚未连接服务器\r\n", COL_CHAT_ERR, 0);
        return;
    }
    if (gui_frame_send(g_sock, (unsigned char)type, payload, (int)strlen(payload)) <= 0)
        chat_append("!! 发送失败，连接可能已断开\r\n", COL_CHAT_ERR, 0);
}

/* ---------------------------------------------------------------- 输入命令 */

static void show_help(void)
{
    chat_append("—— 帮助 ——\r\n", COL_CHAT_SYS, 1);
    chat_append("  直接输入文字          群聊，所有人可见\r\n", COL_CHAT_TXT, 0);
    chat_append("  /list  或 /who        查看在线用户\r\n", COL_CHAT_TXT, 0);
    chat_append("  /msg 昵称 内容        私聊（也可以双击右侧用户）\r\n", COL_CHAT_TXT, 0);
    chat_append("  /nick 新昵称          改名\r\n", COL_CHAT_TXT, 0);
    chat_append("  /clear                清屏\r\n", COL_CHAT_TXT, 0);
    chat_append("  /quit                 退出\r\n", COL_CHAT_TXT, 0);
}

static void do_send(void)
{
    wchar_t wbuf[2048];
    char u8[4096];
    char *p;

    if (!g_connected)
        return;
    GetWindowTextW(g_input, wbuf, 2048);
    w_to_u8(wbuf, u8, sizeof u8);
    if (u8[0] == '\0')
        return;

    if (u8[0] != '/') {
        send_frame_checked(FRAME_TALK, u8);
        SetWindowTextW(g_input, L"");
        return;
    }

    /* 命令 */
    p = u8 + 1;
    if (_strnicmp(p, "list", 4) == 0 || _strnicmp(p, "who", 3) == 0) {
        send_frame_checked(FRAME_LIST, "");
    } else if (_strnicmp(p, "msg", 3) == 0 || _strnicmp(p, "w", 1) == 0) {
        char *arg = strchr(p, ' ');
        char *nick;
        char *text;
        char payload[CHAT_MSG_MAX + CHAT_NICK_MAX];

        if (arg == NULL) {
            chat_append("!! 用法：/msg 昵称 内容\r\n", COL_CHAT_ERR, 0);
            return;
        }
        *arg++ = '\0';
        nick = arg;
        while (*nick == ' ')
            nick++;
        text = strchr(nick, ' ');
        if (text == NULL || nick[0] == '\0') {
            chat_append("!! 用法：/msg 昵称 内容\r\n", COL_CHAT_ERR, 0);
            return;
        }
        *text++ = '\0';
        while (*text == ' ')
            text++;
        if (*text == '\0') {
            chat_append("!! 用法：/msg 昵称 内容\r\n", COL_CHAT_ERR, 0);
            return;
        }
        snprintf(payload, sizeof payload, "%s\t%s", nick, text);
        send_frame_checked(FRAME_PRIVATE, payload);
    } else if (_strnicmp(p, "nick", 4) == 0) {
        char *arg = strchr(p, ' ');
        char payload[CHAT_NICK_MAX + 8];

        if (arg == NULL) {
            chat_append("!! 用法：/nick 新昵称\r\n", COL_CHAT_ERR, 0);
            return;
        }
        arg++;
        while (*arg == ' ')
            arg++;
        if (*arg == '\0') {
            chat_append("!! 用法：/nick 新昵称\r\n", COL_CHAT_ERR, 0);
            return;
        }
        snprintf(payload, sizeof payload, "%s\tgui", arg);
        send_frame_checked(FRAME_LOGIN, payload);
        {
            wchar_t wnew[CHAT_NICK_MAX];

            u8_to_w(arg, wnew, CHAT_NICK_MAX);
            wcscpy(g_my_nick, wnew);
            SetWindowTextW(g_nick, wnew);
        }
    } else if (_strnicmp(p, "help", 4) == 0 || p[0] == '?') {
        show_help();
    } else if (_strnicmp(p, "clear", 5) == 0) {
        SetWindowTextW(g_chat, L"");
    } else if (_strnicmp(p, "quit", 4) == 0 || _strnicmp(p, "exit", 4) == 0) {
        do_disconnect(1);
    } else {
        chat_append("!! 未知命令，输入 /help 查看帮助\r\n", COL_CHAT_ERR, 0);
    }
    SetWindowTextW(g_input, L"");
}

/* ---------------------------------------------------------------- 接收处理 */

static void handle_frame(gui_msg *m)
{
    char *parts[3];
    int n;

    switch (m->type) {
    case FRAME_SAY:
        n = split_tabs(m->text, parts, 3);
        if (n >= 3) {
            char line[CHAT_LINE_MAX + 64];

            snprintf(line, sizeof line, "[%s] ", parts[0]);
            chat_append(line, COL_CHAT_DIM, 0);
            snprintf(line, sizeof line, "%s", parts[1]);
            chat_append(line, COL_CHAT_NICK, 1);
            snprintf(line, sizeof line, "  %s\r\n", parts[2]);
            chat_append(line, COL_CHAT_TXT, 0);
        } else {
            chat_append(m->text, COL_CHAT_TXT, 0);
        }
        break;

    case FRAME_WHISPER:
        n = split_tabs(m->text, parts, 3);
        if (n >= 3) {
            char line[CHAT_LINE_MAX + 96];

            snprintf(line, sizeof line, "[%s] %s 私聊对你说：%s\r\n", parts[0], parts[1], parts[2]);
            chat_append(line, COL_CHAT_PRIV, 0);
        } else {
            chat_append(m->text, COL_CHAT_PRIV, 0);
        }
        break;

    case FRAME_SYS:
        chat_append("*** ", COL_CHAT_SYS, 0);
        chat_append(m->text, COL_CHAT_SYS, 0);
        chat_append("\r\n", COL_CHAT_SYS, 0);
        break;

    case FRAME_ERR:
        chat_append("!!! ", COL_CHAT_ERR, 0);
        chat_append(m->text, COL_CHAT_ERR, 0);
        chat_append("\r\n", COL_CHAT_ERR, 0);
        break;

    case FRAME_USERS:
        fill_users(m->text);
        break;

    case FRAME_TEXT:                     /* 兼容文本模式 */
        strip_ansi(m->text);
        chat_append(m->text, COL_CHAT_TXT, 0);
        break;

    case FRAME_BYE:
        chat_append("*** 连接已结束\r\n", COL_CHAT_SYS, 0);
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------- 窗口过程 */

static LRESULT CALLBACK input_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        do_send();
        return 0;
    }
    return CallWindowProcW(g_input_oldproc, hwnd, msg, wp, lp);
}

static void layout(HWND hwnd)
{
    RECT rc;
    int top_h = sc(52), bottom_h = sc(76), margin = sc(12);
    int w, h;
    int left_w;

    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;

    MoveWindow(g_lbl_server, margin, sc(18), sc(54), sc(20), TRUE);
    MoveWindow(g_server, margin + sc(56), sc(15), sc(140), sc(26), TRUE);
    MoveWindow(g_lbl_port, margin + sc(204), sc(18), sc(36), sc(20), TRUE);
    MoveWindow(g_port, margin + sc(242), sc(15), sc(56), sc(26), TRUE);
    MoveWindow(g_lbl_nick, margin + sc(306), sc(18), sc(36), sc(20), TRUE);
    MoveWindow(g_nick, margin + sc(344), sc(15), sc(104), sc(26), TRUE);
    MoveWindow(g_connect, margin + sc(458), sc(14), sc(72), sc(28), TRUE);
    MoveWindow(g_disconnect, margin + sc(538), sc(14), sc(72), sc(28), TRUE);

    left_w = w - sc(240);

    MoveWindow(g_chat, margin, top_h, left_w - margin, h - top_h - bottom_h, TRUE);

    MoveWindow(g_lbl_users, w - sc(220), top_h - sc(2), sc(100), sc(22), TRUE);
    MoveWindow(g_refresh, w - sc(118), top_h - sc(3), sc(106), sc(24), TRUE);
    MoveWindow(g_users, w - sc(220), top_h + sc(24), sc(208),
               h - top_h - bottom_h - sc(30), TRUE);

    MoveWindow(g_input, margin, h - bottom_h + sc(10),
               w - margin * 2 - sc(96), sc(30), TRUE);
    MoveWindow(g_send, w - margin - sc(88), h - bottom_h + sc(8), sc(88), sc(34), TRUE);
    MoveWindow(g_status, margin, h - sc(28), w - margin * 2, sc(20), TRUE);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HDC hdc = GetDC(hwnd);
        int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
        int fs = sc(16);

        if (hdc)
            ReleaseDC(hwnd, hdc);
        g_scale = (dpi + 47) / 96;          /* 96→1，120→1，144→2 */
        if (g_scale < 1)
            g_scale = 1;
        fs = sc(16);

        g_font = CreateFontW(-fs, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");
        g_font_bold = CreateFontW(-fs, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");

        /* 顶部参数栏 */
        g_lbl_server = CreateWindowW(L"STATIC", L"服务器", WS_CHILD | WS_VISIBLE,
                                     0, 0, 10, 10, hwnd, NULL, g_inst, NULL);
        g_server = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0.0.0.0",
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10,
                                   hwnd, (HMENU)IDC_SERVER, g_inst, NULL);
        g_lbl_port = CreateWindowW(L"STATIC", L"端口", WS_CHILD | WS_VISIBLE,
                                   0, 0, 10, 10, hwnd, NULL, g_inst, NULL);
        g_port = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"8888",
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER, 0, 0, 10, 10,
                                 hwnd, (HMENU)IDC_PORT, g_inst, NULL);
        g_lbl_nick = CreateWindowW(L"STATIC", L"昵称", WS_CHILD | WS_VISIBLE,
                                   0, 0, 10, 10, hwnd, NULL, g_inst, NULL);
        g_nick = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10,
                                 hwnd, (HMENU)IDC_NICK, g_inst, NULL);
        g_connect = CreateWindowW(L"BUTTON", L"连接", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 10, 10, hwnd, (HMENU)IDC_CONNECT, g_inst, NULL);
        g_disconnect = CreateWindowW(L"BUTTON", L"断开", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_DISCONNECT, g_inst, NULL);

        /* 聊天区（RichEdit） */
        g_chat = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit20W", L"",
                                 WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                 ES_AUTOVSCROLL | WS_VSCROLL, 0, 0, 10, 10,
                                 hwnd, (HMENU)IDC_CHAT, g_inst, NULL);
        SendMessageW(g_chat, EM_SETBKGNDCOLOR, 0, (LPARAM)COL_CHAT_BG);
        SendMessageW(g_chat, EM_SETLIMITTEXT, (WPARAM)0x7FFFFFF0, 0);

        /* 右侧在线列表 */
        g_users = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                  0, 0, 10, 10, hwnd, (HMENU)IDC_USERS, g_inst, NULL);
        g_refresh = CreateWindowW(L"BUTTON", L"刷新", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 10, 10, hwnd, (HMENU)IDC_REFRESH, g_inst, NULL);
        g_lbl_users = CreateWindowW(L"STATIC", L"在线用户", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                    0, 0, 10, 10, hwnd, NULL, g_inst, NULL);

        /* 输入区 */
        g_input = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10,
                                  hwnd, (HMENU)IDC_INPUT, g_inst, NULL);
        g_send = CreateWindowW(L"BUTTON", L"发送", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                               0, 0, 10, 10, hwnd, (HMENU)IDC_SEND, g_inst, NULL);
        g_status = CreateWindowW(L"STATIC", L"未连接", WS_CHILD | WS_VISIBLE,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_STATUS, g_inst, NULL);

        /* 统一字体 */
        {
            HWND all[] = { g_lbl_server, g_server, g_lbl_port, g_port, g_lbl_nick, g_nick,
                           g_connect, g_disconnect, g_users, g_refresh, g_input, g_send,
                           g_status, g_lbl_users, g_chat };
            int i;

            for (i = 0; i < (int)(sizeof all / sizeof all[0]); i++)
                SendMessageW(all[i], WM_SETFONT, (WPARAM)g_font, TRUE);
            SendMessageW(g_send, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
        }

        /* 输入框回车即发送 */
        g_input_oldproc = (WNDPROC)SetWindowLongPtrW(g_input, GWLP_WNDPROC, (LONG_PTR)input_proc);

        SetWindowTextW(g_main, APP_TITLE);
        chat_append("[提示] 填好服务器/端口/昵称后点「连接」。本程序可以多个实例同时运行。\r\n",
                    COL_CHAT_DIM, 0);
        return 0;
    }

    case WM_SIZE:
        layout(hwnd);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;

        SetBkColor(hdc, COL_BG);
        SetTextColor(hdc, (HWND)lp == g_lbl_users ? COL_GREEN : COL_GREEN_D);
        return (LRESULT)g_bg_brush;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;

        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_bg_brush);
        return 1;
    }

    case WM_APP_FRAME: {
        gui_msg *m = (gui_msg *)lp;

        if (m != NULL) {
            handle_frame(m);
            free(m);
        }
        return 0;
    }

    case WM_APP_CLOSED: {
        if (g_sock != INVALID_SOCKET) {
            closesocket(g_sock);
            g_sock = INVALID_SOCKET;
        }
        if (InterlockedExchange(&g_connected, 0) == 1) {
            chat_append("*** 与服务端的连接已断开\r\n", COL_CHAT_SYS, 0);
            set_status(L"已断开");
            set_connected_ui(0);
            SetWindowTextW(g_main, APP_TITLE);
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CONNECT:
            do_connect();
            return 0;
        case IDC_DISCONNECT:
            do_disconnect(1);
            return 0;
        case IDC_SEND:
            do_send();
            return 0;
        case IDC_REFRESH:
            send_frame_checked(FRAME_LIST, "");
            return 0;
        case IDC_USERS:
            if (HIWORD(wp) == LBN_DBLCLK) {
                int sel = (int)SendMessageW(g_users, LB_GETCURSEL, 0, 0);

                if (sel != LB_ERR) {
                    wchar_t nick[CHAT_NICK_MAX];
                    wchar_t cmd[CHAT_NICK_MAX + 16];

                    SendMessageW(g_users, LB_GETTEXT, (WPARAM)sel, (LPARAM)nick);
                    _snwprintf(cmd, CHAT_NICK_MAX + 15, L"/msg %s ", nick);
                    cmd[CHAT_NICK_MAX + 15] = L'\0';
                    SetWindowTextW(g_input, cmd);
                    SetFocus(g_input);
                    SendMessageW(g_input, EM_SETSEL, (WPARAM)wcslen(cmd), (LPARAM)wcslen(cmd));
                }
            }
            return 0;
        default:
            break;
        }
        break;

    case WM_DESTROY:
        do_disconnect(0);
        if (g_thread != NULL) {
            WaitForSingleObject(g_thread, 1000);
            CloseHandle(g_thread);
            g_thread = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------------------------------------------------------- 入口 */

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WSADATA wsa;
    WNDCLASSEXW wc;
    MSG msg;
    int argc = 0;
    LPWSTR *argv;
    const wchar_t *init_server = L"0.0.0.0";
    const wchar_t *init_port = L"8888";
    const wchar_t *init_nick = L"";
    int auto_connect = 0;
    int i;

    (void)prev;
    (void)cmdline;

    SetProcessDPIAware();
    LoadLibraryW(RICHOOK);            /* 让 "RichEdit20W" 这个窗口类可用 */
    g_bg_brush = CreateSolidBrush(COL_BG);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxW(NULL, L"Winsock 初始化失败", L"错误", MB_ICONERROR);
        return 1;
    }

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != NULL) {
        for (i = 1; i < argc; i++) {
            if (wcscmp(argv[i], L"--server") == 0 && i + 1 < argc)
                init_server = argv[++i];
            else if (wcscmp(argv[i], L"--port") == 0 && i + 1 < argc)
                init_port = argv[++i];
            else if (wcscmp(argv[i], L"--nick") == 0 && i + 1 < argc)
                init_nick = argv[++i];
            else if (wcscmp(argv[i], L"--auto-connect") == 0)
                auto_connect = 1;
        }
    }

    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(COL_BG);
    wc.lpszClassName = L"ChatGuiWnd";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    if (RegisterClassExW(&wc) == 0) {
        MessageBoxW(NULL, L"窗口类注册失败", L"错误", MB_ICONERROR);
        return 1;
    }

    g_inst = inst;

    /* 先按屏幕 DPI 决定窗口初始大小（WM_CREATE 里会再确认一次） */
    {
        HDC sdc = GetDC(NULL);
        int dpi = sdc ? GetDeviceCaps(sdc, LOGPIXELSX) : 96;

        if (sdc)
            ReleaseDC(NULL, sdc);
        g_scale = (dpi + 47) / 96;
        if (g_scale < 1)
            g_scale = 1;
    }

    g_main = CreateWindowExW(0, L"ChatGuiWnd", APP_TITLE,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, sc(980), sc(660),
                             NULL, NULL, inst, NULL);
    if (g_main == NULL) {
        MessageBoxW(NULL, L"窗口创建失败", L"错误", MB_ICONERROR);
        return 1;
    }

    SetWindowTextW(g_server, init_server);
    SetWindowTextW(g_port, init_port);
    SetWindowTextW(g_nick, init_nick);

    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    set_connected_ui(0);

    if (auto_connect && init_nick[0] != L'\0')
        PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(IDC_CONNECT, 0), 0);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (argv != NULL)
        LocalFree(argv);
    WSACleanup();
    return 0;
}
