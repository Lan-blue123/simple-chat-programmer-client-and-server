/*
 * dash.h —— 服务端实时看板（TUI）
 *
 * 用途：把"消息怎么流转、fork 了哪些子进程、谁进来了、谁被回收了"
 *      直接画在服务端终端上，演示时不用滚日志。
 *
 * 只在 stdout 是终端时启用；非终端（重定向/管道）会自动退回普通日志模式。
 */
#ifndef CHAT_DASH_H
#define CHAT_DASH_H

#include <stddef.h>

#define DASH_NICK_MAX 32
#define DASH_PEER_MAX 80

/* 事件类型，决定颜色 */
enum dash_event_kind {
    DASH_EV_FORK = 1,   /* 接受连接并 fork 子进程 */
    DASH_EV_LOGIN,      /* 登录 / 改名 */
    DASH_EV_TALK,       /* 群聊转发 */
    DASH_EV_PRIVATE,    /* 私聊转发 */
    DASH_EV_LIST,       /* 查询在线列表 */
    DASH_EV_LEAVE,      /* 退出 / 掉线 */
    DASH_EV_REAP,       /* waitpid 回收子进程 */
    DASH_EV_SYS         /* 其它系统事件 */
};

/* 主进程每帧传给看板的用户表快照 */
typedef struct {
    int           slot;
    int           pid;
    char          nick[DASH_NICK_MAX];
    char          peer[DASH_PEER_MAX];
    long          online_sec;
    unsigned long in_msgs;
    unsigned long out_msgs;
} dash_user_t;

/*
 * 初始化看板：返回 1 表示已启用；0 表示 stdout 不是终端/不需要看板，
 * 调用方应退回普通日志模式。main_pid/port 只用于标题显示。
 */
int  dash_init(int main_pid, int port);
void dash_shutdown(void);

/* 追加一条事件（自动带时间戳） */
void dash_eventf(int kind, const char *fmt, ...);

/* 画一帧 */
void dash_draw(const dash_user_t *users, int n,
               int online, int total_fork, int total_reap,
               int port, int max_slots, int total_sessions);

#endif /* CHAT_DASH_H */
