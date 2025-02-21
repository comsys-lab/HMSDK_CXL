CC = gcc
CFLAGS = -Wall -Wextra

TARGET = pebs_userspace
SRCS = pebs_userspace.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)

.PHONY: all clean
