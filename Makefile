CC = gcc
CFLAGS = -O2 -Wall -Wextra -pthread
LDFLAGS = -pthread

all: reproducer reproducer-mtrace

reproducer: reproducer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Diagnostic single-threaded reproducer with mtrace() and malloc_info()
# hooks.  Used by run-mtrace.sh / run-strace.sh / run-malloc-info.sh.
# No pthread dependency.
reproducer-mtrace: reproducer-mtrace.c
	$(CC) -O2 -Wall -o $@ $<

clean:
	rm -f reproducer reproducer-mtrace

.PHONY: all clean
