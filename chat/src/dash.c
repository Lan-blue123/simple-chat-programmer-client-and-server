/*
 * dash.c —— 服务端实时看板
 *
 * 直接把"在线会话（含各自的子进程 PID）+ 消息流转事件 + fork/回收计数"
 * 画在服务端终端上。用 ANSI 转义序列实现：备用屏幕、光标定位、彩色。
 *
 * 说明：
 *   · 只在 stdout 是终端时启用，否则 dash_init 返回 0，服务端退回普通日志；
 *   · 中文占两格，所以宽度/截断都按"显示宽度"算，表格才不会错位；
 *   · 每帧整屏重画（先 \033[H 归位，每行 \033[K 清到行尾），不闪烁。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "dash.h"

#define DASH_MAX_EVENTS 64
#define DASH_EV_TEXT    160

/* ---------------------------------------------------------------- 颜色 */
#define C_RESET  "\033[0m"
#define C_TITLE  "\033[1;37m"
#define C_STAT   "\033[1;33m"
#define C_SEP    "\033[2;37m"
#define C_SECTION "\033[1;36m"
#define C_HEAD   "\033[1;37m"
#define C_ROW    "\033[0;37m"
#define C_HINT   "\033[2;37m"

#define C_FORK   "\033[1;32m"
#define C_LOGIN  "\033[1;36m"
#define C_TALK   "\033[0;37m"
#define C_PRIV   "\033[1;35m"
#define C_LIST   "\033[0;36m"
#define C_LEAVE  "\033[1;33m"
#define C_REAP   "\033[2;37m"
#define C_SYS    "\033[0;35m"

typedef struct {
    int  kind;
    char ts[9];
    char text[DASH_EV_TEXT];
} dash_event_t;

static int            g_on;
static int            g_inited;
static int            g_forced;      /* 1 = CHAT_DASH_FORCE 强制启用（非终端） */
static int            g_rows = 34;
static int            g_cols = 100;
static int            g_main_pid;
static int            g_port;
static time_t         g_start;
static dash_event_t   g_ev[DASH_MAX_EVENTS];
static int            g_ev_head;      /* 下一条写入位置 */
static int            g_ev_count;

/* ---------------------------------------------------------------- UTF-8 宽度 */

/* 返回一个 UTF-8 字符的字节数（容错：非法字节按 1 处理） */
static int u8_len(const char *s)
{
    unsigned char c = (unsigned char)s[0];

    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

static unsigned int u8_cp(const char *s)
{
    unsigned char c = (unsigned char)s[0];

    if (c < 0x80)
        return c;
    if ((c & 0xE0) == 0xC0)
        return ((unsigned int)(c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
    if ((c & 0xF0) == 0xE0)
        return ((unsigned int)(c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) |
               ((unsigned char)s[2] & 0x3F);
    if ((c & 0xF8) == 0xF0)
        return ((unsigned int)(c & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12) |
               (((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F);
    return c;
}

/* 终端里显示占几格：0（控制符）、1（半角）、2（全角） */
static int cp_width(unsigned int cp)
{
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0))
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6))
        return 2;
    return 1;
}

static int disp_width(const char *s)
{
    int w = 0;

    while (*s != '\0') {
        int n = u8_len(s);

        w += cp_width(u8_cp(s));
        s += n;
    }
    return w;
}

/* 把 s 截断到最多 max_cells 格，写入 dst（保证不切断 UTF-8） */
static void fit_utf8(char *dst, size_t cap, const char *s, int max_cells)
{
    size_t o = 0;
    int w = 0;

    while (*s != '\0') {
        int n = u8_len(s);
        int cw = cp_width(u8_cp(s));

        if (w + cw > max_cells)
            break;
        if (o + (size_t)n + 1 >= cap)
            break;
        memcpy(dst + o, s, (size_t)n);
        o += (size_t)n;
        s += n;
        w += cw;
    }
    dst[o] = '\0';
}

/* 左对齐/右对齐补空格到 cells 格 */
static void pad_to(char *dst, size_t cap, const char *s, int cells, int right)
{
    int w = disp_width(s);
    int pad = (cells > w) ? (cells - w) : 0;

    if (right)
        snprintf(dst, cap, "%*s%s", pad, "", s);
    else
        snprintf(dst, cap, "%s%*s", s, pad, "");
}

/* 会话表的一行：列宽 4 / 7 / 18 / 24 / 8 / 6 / 6，用显示宽度对齐（中文占两格） */
#define TBL_CAP 512
static void table_row(char *line, size_t cap,
                      const char *slot, const char *pid, const char *nick,
                      const char *peer, const char *online,
                      const char *in, const char *out)
{
    static const int w[7] = { 4, 7, 18, 24, 8, 6, 6 };
    const char *cells[7];
    char  one[128];
    size_t off = 0;
    int    i;
    int    right;

    cells[0] = slot; cells[1] = pid; cells[2] = nick; cells[3] = peer;
    cells[4] = online; cells[5] = in; cells[6] = out;

    line[0] = '\0';
    for (i = 0; i < 7; i++) {
        right = (i == 1 || i == 4 || i == 5 || i == 6);
        pad_to(one, sizeof one, cells[i], w[i], right);
        if (off + strlen(one) + 4 < cap) {
            off += (size_t)snprintf(line + off, cap - off, " %s", one);
        }
    }
}

/* ---------------------------------------------------------------- 屏幕缓冲 */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    int    used;
    int    rows;
    int    cols;
} screen_t;

/* 追加一行：text 按显示宽度截断到 cols 格，行前上色、行尾清行
 * 注意：最后一行不能带 \r\n，否则光标会越过屏幕底部触发整屏滚动 */
static void scr_line(screen_t *s, const char *color, const char *text)
{
    char fitted[2048];
    int  n;

    if (s->used >= s->rows || s->len + 64 >= s->cap)
        return;

    fit_utf8(fitted, sizeof fitted, text, s->cols - 1);
    n = snprintf(s->buf + s->len, s->cap - s->len, "%s%s\033[K%s\r\n",
                 color, fitted, C_RESET);
    if (n > 0)
        s->len += (size_t)n;
    s->used++;
}

/* 空行 */
static void scr_blank(screen_t *s)
{
    scr_line(s, "", "");
}

/* 分隔线 */
static void scr_sep(screen_t *s, char ch)
{
    char line[512];
    int  i;

    if (s->cols >= (int)sizeof line - 1)
        return;
    for (i = 0; i < s->cols - 1; i++)
        line[i] = ch;
    line[i] = '\0';
    scr_line(s, C_SEP, line);
}

/* ---------------------------------------------------------------- 对外接口 */

int dash_init(int main_pid, int port)
{
    struct winsize ws;
    const char *plain = getenv("CHAT_DASH_NO_ALT");
    const char *force = getenv("CHAT_DASH_FORCE");
    const char *env_cols;
    const char *env_rows;
    int is_tty = isatty(STDOUT_FILENO);

    /*
     * 正常情况下只在终端里启用。
     * CHAT_DASH_FORCE=1 可以强制启用（配合 COLUMNS/LINES 与 CHAT_DASH_NO_ALT），
     * 用于把看板录成文本日志或在没有终端的环境里检查渲染结果。
     */
    if (!is_tty && force == NULL)
        return 0;

    g_cols = 100;
    g_rows = 34;
    if (is_tty && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col > 0 && ws.ws_row > 0) {
        g_cols = ws.ws_col;
        g_rows = ws.ws_row;
    }
    env_cols = getenv("COLUMNS");
    env_rows = getenv("LINES");
    if (env_cols != NULL && atoi(env_cols) > 0)
        g_cols = atoi(env_cols);
    if (env_rows != NULL && atoi(env_rows) > 0)
        g_rows = atoi(env_rows);
    if (g_cols < 40)
        g_cols = 40;
    if (g_cols > 220)
        g_cols = 220;
    if (g_rows < 8)          /* 行数必须按终端真实高度来，否则会撑出屏幕触发滚动 */
        g_rows = 8;
    if (g_rows > 70)
        g_rows = 70;

    g_main_pid = main_pid;
    g_port = port;
    g_start = time(NULL);
    g_on = 1;
    g_inited = 1;
    g_forced = !is_tty;
    g_ev_head = 0;
    g_ev_count = 0;

    /* CHAT_DASH_NO_ALT=1 时不切备用屏幕，直接原屏刷新（便于录日志/在 CI 里看） */
    if (plain != NULL) {
        static const char clear_home[] = "\033[2J\033[H";

        (void)!write(STDOUT_FILENO, clear_home, sizeof clear_home - 1);
    } else {
        static const char enter_alt[] = "\033[?1049h\033[?25l\033[2J";

        (void)!write(STDOUT_FILENO, enter_alt, sizeof enter_alt - 1);
    }
    return 1;
}

void dash_shutdown(void)
{
    static const char restore[] = "\033[?25h\033[?1049l";

    if (!g_inited)
        return;
    (void)!write(STDOUT_FILENO, restore, sizeof restore - 1);
    g_on = 0;
    g_inited = 0;
}

void dash_eventf(int kind, const char *fmt, ...)
{
    dash_event_t *e;
    time_t t = time(NULL);
    struct tm tmv;
    va_list ap;

    if (!g_on)
        return;

    e = &g_ev[g_ev_head];
    e->kind = kind;
    localtime_r(&t, &tmv);
    strftime(e->ts, sizeof e->ts, "%H:%M:%S", &tmv);
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof e->text, fmt, ap);
    va_end(ap);

    g_ev_head = (g_ev_head + 1) % DASH_MAX_EVENTS;
    if (g_ev_count < DASH_MAX_EVENTS)
        g_ev_count++;
}

static const char *ev_color(int kind)
{
    switch (kind) {
    case DASH_EV_FORK:    return C_FORK;
    case DASH_EV_LOGIN:   return C_LOGIN;
    case DASH_EV_TALK:    return C_TALK;
    case DASH_EV_PRIVATE: return C_PRIV;
    case DASH_EV_LIST:    return C_LIST;
    case DASH_EV_LEAVE:   return C_LEAVE;
    case DASH_EV_REAP:    return C_REAP;
    default:              return C_SYS;
    }
}

static const char *ev_label(int kind)
{
    switch (kind) {
    case DASH_EV_FORK:    return "fork";
    case DASH_EV_LOGIN:   return "登录";
    case DASH_EV_TALK:    return "群聊";
    case DASH_EV_PRIVATE: return "私聊";
    case DASH_EV_LIST:    return "列表";
    case DASH_EV_LEAVE:   return "离开";
    case DASH_EV_REAP:    return "回收";
    default:              return "系统";
    }
}

/* 把秒数格式化成 HH:MM:SS */
static void fmt_dur(long sec, char *buf, size_t cap)
{
    if (sec < 0)
        sec = 0;
    if (sec >= 3600)
        snprintf(buf, cap, "%02ld:%02ld:%02ld", sec / 3600, (sec % 3600) / 60, sec % 60);
    else
        snprintf(buf, cap, "%02ld:%02ld", sec / 60, sec % 60);
}

void dash_draw(const dash_user_t *users, int n,
               int online, int total_fork, int total_reap,
               int port, int max_slots, int total_sessions)
{
    static char  buf[256 * 1024];
    screen_t     s;
    char         line[1024];
    char         dur[32];
    char         uptime[32];
    long         up = (long)(time(NULL) - g_start);
    int          i;
    int          shown_sessions;
    int          max_events;

    (void)port;
    (void)total_sessions;

    if (!g_on)
        return;

    /* 每次都重新问一下终端尺寸，窗口被拉大/拉小也能自适应 */
    if (!g_forced) {
        struct winsize ws;

        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
            ws.ws_col > 0 && ws.ws_row > 0) {
            g_cols = ws.ws_col;
            g_rows = ws.ws_row;
            if (g_cols < 40)
                g_cols = 40;
            if (g_cols > 220)
                g_cols = 220;
            if (g_rows < 8)
                g_rows = 8;
            if (g_rows > 70)
                g_rows = 70;
        }
    }

    s.buf = buf;
    s.cap = sizeof buf;
    s.len = 0;
    s.used = 0;
    s.rows = g_rows;
    s.cols = g_cols;

    /* 光标归位，逐行覆盖 */
    memcpy(s.buf, "\033[H", 3);
    s.len = 3;

    /* 标题 */
    fmt_dur(up, uptime, sizeof uptime);
    snprintf(line, sizeof line,
             " 多进程聊天服务器   主进程 pid=%d   监听 0.0.0.0:%d   运行 %s ",
             g_main_pid, g_port, uptime);
    scr_line(&s, C_TITLE, line);

    snprintf(line, sizeof line,
             " 在线 %d / 上限 %d     累计 fork 子进程 %d 次     已回收 %d 个     当前存活子进程 %d 个",
             online, max_slots, total_fork, total_reap,
             total_fork - total_reap);
    scr_line(&s, C_STAT, line);

    scr_sep(&s, '=');

    /* 在线会话表 */
    scr_line(&s, C_SECTION, " 【在线会话】 一个客户端 = 一个 fork 出来的子进程");
    table_row(line, sizeof line, "槽位", "PID", "昵称", "对端地址", "在线", "收到", "发出");
    scr_line(&s, C_HEAD, line);

    shown_sessions = 0;
    for (i = 0; i < n; i++) {
        char c_slot[8], c_pid[12], c_on[16], c_in[10], c_out[10];

        if (s.used >= s.rows - 8)
            break;
        snprintf(c_slot, sizeof c_slot, "%d", users[i].slot);
        snprintf(c_pid, sizeof c_pid, "%d", users[i].pid);
        fmt_dur(users[i].online_sec, dur, sizeof dur);
        snprintf(c_on, sizeof c_on, "%s", dur);
        snprintf(c_in, sizeof c_in, "%lu", users[i].in_msgs);
        snprintf(c_out, sizeof c_out, "%lu", users[i].out_msgs);

        table_row(line, sizeof line, c_slot, c_pid, users[i].nick,
                  users[i].peer, c_on, c_in, c_out);
        scr_line(&s, C_ROW, line);
        shown_sessions++;
    }
    if (shown_sessions == 0)
        scr_line(&s, C_HINT, "   （还没有客户端接入，等待连接……）");

    scr_sep(&s, '=');

    /* 事件流：从旧到新 */
    scr_line(&s, C_SECTION, " 【事件流】 接入 → fork → 登录 → 群聊/私聊路由 → 退出 → waitpid 回收");
    max_events = g_rows - s.used - 2;
    if (max_events > g_ev_count)
        max_events = g_ev_count;
    for (i = 0; i < max_events; i++) {
        const dash_event_t *e = &g_ev[(g_ev_head - max_events + i + DASH_MAX_EVENTS) % DASH_MAX_EVENTS];

        snprintf(line, sizeof line, " %s  %-4s  %s", e->ts, ev_label(e->kind), e->text);
        scr_line(&s, ev_color(e->kind), line);
    }
    if (g_ev_count == 0)
        scr_line(&s, C_HINT, "   （暂无事件）");

    /* 底部提示 */
    while (s.used < s.rows - 1)
        scr_blank(&s);
    scr_line(&s, C_HINT, " Ctrl+C 退出   ·   每 0.5 秒刷新   ·   fork/回收 计数随事件实时变化");

    /*
     * 关键：去掉最后一行的 \r\n。
     * 整屏刚好占满 rows 行，如果最后一行也换行，光标会越过屏幕底部，
     * 终端就会滚动一行 —— 表现为"顶部行跳动、底部提示语不断堆叠"。
     * 去掉后光标停在最后一行行尾，下一帧 \033[H 回到左上角覆盖，不会滚。
     */
    if (s.len >= 2 && s.buf[s.len - 2] == '\r' && s.buf[s.len - 1] == '\n')
        s.len -= 2;

    /* 清掉这一帧没覆盖到的旧内容（终端变宽/变高时） */
    if (s.len + 8 < s.cap) {
        memcpy(s.buf + s.len, "\033[J", 3);
        s.len += 3;
    }

    (void)!write(STDOUT_FILENO, s.buf, s.len);
}
