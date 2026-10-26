/*
 * protocol.h —— 多进程聊天程序通信协议
 *
 * 三条链路都使用同一套"定长帧头 + 负载"的二进制帧格式：
 *
 *   客户端  <--TCP-->  服务端子进程  <--socketpair-->  服务端主进程
 *
 * 之所以自己定义帧格式，是因为 TCP / AF_UNIX 流式套接字只保证字节流有序，
 * 不保证一次 write 对应一次 read（可能短读、可能粘包）。有了帧头里的长度字段，
 * 接收方才能把字节流切分成一条条完整消息。
 *
 * 帧格式：
 *   +--------+--------+--------+------------------+
 *   | type   | len_hi | len_lo |  payload (len)   |
 *   | 1 byte |    2 bytes (网络字节序)   |  0~65535 bytes |
 *   +--------+--------+--------+------------------+
 */
#ifndef CHAT_PROTOCOL_H
#define CHAT_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define CHAT_DEFAULT_PORT 8888
#define CHAT_MAX_CLIENTS  64
#define CHAT_NICK_MAX     32
#define CHAT_MSG_MAX      1024
#define CHAT_LINE_MAX     (CHAT_MSG_MAX + 256)

/* 帧类型，占 1 字节；请求由客户端发出，应答由服务端发出 */
enum chat_frame_type {
    /* —— 客户端 -> 服务端 —— */
    FRAME_LOGIN   = 1,  /* 载荷：昵称；首次登录，登录后再发则为改名 */
    FRAME_TALK    = 2,  /* 载荷：聊天正文 */
    FRAME_PRIVATE = 3,  /* 载荷："目标昵称 \t 正文" */
    FRAME_LIST    = 4,  /* 载荷：空；请求在线列表 */
    FRAME_QUIT    = 5,  /* 载荷：空；请求退出 */

    /* —— 服务端 -> 客户端 —— */
    FRAME_SAY     = 6,  /* 载荷："时间 \t 昵称 \t 正文" 群聊 */
    FRAME_WHISPER = 7,  /* 载荷："时间 \t 昵称 \t 正文" 私聊抵达 */
    FRAME_SYS     = 8,  /* 载荷：系统提示正文 */
    FRAME_ERR     = 9,  /* 载荷：错误提示正文 */
    FRAME_USERS   = 10, /* 载荷：昵称以 '\n' 分隔的在线列表 */
    FRAME_TEXT    = 11, /* 服务端子进程 -> 客户端：已渲染好的一行文本 */
    FRAME_BYE     = 12  /* 服务端 -> 客户端：连接即将结束 */
};

/*
 * 三个底层 IO 函数：都返回 1 表示成功、0 表示对端正常关闭、-1 表示出错。
 * 它们负责处理 EINTR 与"短读/短写"，是整个程序可靠性的基础。
 */
int read_full(int fd, void *buf, size_t len);
int write_full(int fd, const void *buf, size_t len);

/* 发送一帧；成功返回 1，失败返回 -1 */
int frame_send(int fd, uint8_t type, const void *payload, size_t len);

/*
 * 接收一帧：1 成功、0 对端关闭、-1 IO 出错、-2 帧长超过缓冲区。
 * payload 必须以 '\0' 结尾（函数会保证），len 可为 NULL。
 */
int frame_recv(int fd, uint8_t *type, char *payload, size_t cap, size_t *len);

#endif /* CHAT_PROTOCOL_H */
