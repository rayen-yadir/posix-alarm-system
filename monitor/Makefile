CC = gcc
CFLAGS = -Wall -Wextra -D_GNU_SOURCE
LDFLAGS = -lrt -lpthread

TARGET = alarm_logger
SRC = alarm_logger.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean