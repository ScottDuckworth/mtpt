CC = gcc
CFLAGS = -Wall -O2 -pthread
LDFLAGS = -pthread

all: psync

clean:
	rm -f psync *.o

psync: threadpool.o mtpt.o psync.o
	$(CC) $^ $(LDFLAGS) -o $@

mtpt-test: threadpool.o mtpt.o mtpt-test.o
	$(CC) $^ $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $^
