CROSS_COMPILE ?=
CC             = $(CROSS_COMPILE)gcc
CFLAGS        ?= -g -Wall -Wextra -Werror
LDFLAGS       ?=

TARGET := writer
SRCS   := writer.c
OBJS   := $(SRCS:.c=.o)

.PHONY: all clean

# Default target
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) *.o
