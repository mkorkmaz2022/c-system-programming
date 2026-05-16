CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -O2 -g

TARGET = procSearch
SRCS = procSearch.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) worker_*.txt