CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pthread
LDFLAGS = -pthread

TARGET = cargoGTU

SRCS = main.c order.c priority_queue.c courier.c stats.c
OBJS = $(SRCS:.c=.o)
HEADERS = order.h priority_queue.h courier.h stats.h

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
