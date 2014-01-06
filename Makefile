CC = gcc
CFLAGS = -Wall -O2 -pthread
LDFLAGS = -pthread

all: psync

clean:
	rm -f psync *.o

psync: psync.o threadpool.o
	$(CC) $^ $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $^
