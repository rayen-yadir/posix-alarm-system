CC = gcc
CFLAGS = -Wall -Wextra -D_GNU_SOURCE
LDFLAGS = -lrt -lpthread

TARGET = alarm_ctl
SRC = alarm_ctl.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean