CC = gcc
CFLAGS = -Wall -Wextra -pthread -D_GNU_SOURCE
LDFLAGS = -lpthread -lrt -lgpiod

TARGET = alarm_daemon
SRC = alarm_daemon.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
