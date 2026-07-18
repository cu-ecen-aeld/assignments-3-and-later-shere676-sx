# Makefile for aesdsocket
#
# Supports cross compilation via the standard CROSS_COMPILE / CC mechanism,
# e.g.:
#   make CROSS_COMPILE=aarch64-none-linux-gnu-
# or by overriding CC directly:
#   make CC=aarch64-none-linux-gnu-gcc

CROSS_COMPILE ?=
CC ?= $(CROSS_COMPILE)gcc

CFLAGS ?= -Wall -Wextra -Werror -g -O0
LDFLAGS ?=

TARGET := aesdsocket
SRCS := aesdsocket.c
OBJS := $(SRCS:.c=.o)

.PHONY: all default clean

all: $(TARGET)

default: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
