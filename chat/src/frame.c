/*
 * frame.c —— 可靠读写与帧封装
 *
 * 这是整个聊天程序最容易被忽略、却最关键的一层：
 *   read()/write() 作用在流式套接字上时，返回值只是"这一次搬运了多少字节"，
 *   既不等于请求的字节数（短读/短写），也可能因为信号中断而返回 -1 + EINTR。
 * 所以必须循环搬运，直到凑满需要的字节数。
 */
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "protocol.h"

/* 读满 len 字节：1 成功；0 对端在开头就关闭；-1 中途出错或中途被关闭 */
int read_full(int fd, void *buf, size_t len)
{
    size_t got = 0;
    char *p = (char *)buf;

    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);

        if (n < 0) {
            if (errno == EINTR)      /* 被信号打断，重试即可 */
                continue;
            return -1;
        }
        if (n == 0)                  /* 对端关闭 */
            return (got == 0) ? 0 : -1;
        got += (size_t)n;
    }
    return 1;
}

/* 写满 len 字节：1 成功；-1 出错（含对端已关闭导致的 EPIPE） */
int write_full(int fd, const void *buf, size_t len)
{
    size_t sent = 0;
    const char *p = (const char *)buf;

    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 1;
}

/* 先发 3 字节帧头，再发负载 */
int frame_send(int fd, uint8_t type, const void *payload, size_t len)
{
    unsigned char hdr[3];

    if (len > 0xFFFF)                 /* 帧头长度字段只有 2 字节 */
        return -1;

    hdr[0] = (unsigned char)type;
    hdr[1] = (unsigned char)((len >> 8) & 0xFF);
    hdr[2] = (unsigned char)(len & 0xFF);

    if (write_full(fd, hdr, sizeof hdr) <= 0)
        return -1;
    if (len > 0 && write_full(fd, payload, len) <= 0)
        return -1;
    return 1;
}

/* 先收 3 字节帧头，再按长度收负载 */
int frame_recv(int fd, uint8_t *type, char *payload, size_t cap, size_t *len)
{
    unsigned char hdr[3];
    size_t plen;
    int r;

    if (len != NULL)
        *len = 0;

    r = read_full(fd, hdr, sizeof hdr);
    if (r <= 0)
        return r;

    plen = ((size_t)hdr[1] << 8) | (size_t)hdr[2];
    if (plen + 1 > cap)               /* 留出 '\0' 的位置 */
        return -2;

    if (plen > 0) {
        r = read_full(fd, payload, plen);
        if (r <= 0)
            return r;
    }
    payload[plen] = '\0';             /* 统一按 C 字符串使用 */

    if (type != NULL)
        *type = hdr[0];
    if (len != NULL)
        *len = plen;
    return 1;
}
