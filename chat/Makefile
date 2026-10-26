# Makefile —— 多进程聊天程序
#
# 编译： make             (生成 build/server 与 build/client)
# 清理： make clean

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -std=gnu99 -Iinclude
LDFLAGS ?=

BIN      = build
SRV_OBJS = $(BIN)/frame.o $(BIN)/server.o $(BIN)/dash.o
CLI_OBJS = $(BIN)/frame.o $(BIN)/client.o

all: $(BIN)/server $(BIN)/client

$(BIN):
	mkdir -p $(BIN)

$(BIN)/%.o: src/%.c include/protocol.h include/dash.h | $(BIN)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN)/server: $(SRV_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BIN)/client: $(CLI_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

clean:
	rm -rf $(BIN)

.PHONY: all clean
