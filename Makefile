CC = gcc
CFLAGS = -O2 -Wall -Wextra -pthread
LDFLAGS = -pthread

reproducer: reproducer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f reproducer
