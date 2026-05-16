CC = gcc
CFLAGS = -Wall -Wextra -pthread
TARGET = analyzer
SOURCES = main.c reader.c dispatcher.c analyzer.c aggregator.c shm.c watchdog.c

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET) *.o *.tmp