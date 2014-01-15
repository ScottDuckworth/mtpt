CC = gcc
CFLAGS = -Wall -O2 -pthread
LDFLAGS = -pthread

all: mtsync mtrm

clean:
	rm -f mtsync *.o

mtsync: threadpool.o mtpt.o exclude.o mtsync.o
	$(CC) $^ $(LDFLAGS) -o $@

mtrm: threadpool.o mtpt.o exclude.o mtrm.o
	$(CC) $^ $(LDFLAGS) -o $@

mtpt-test: threadpool.o mtpt.o mtpt-test.o
	$(CC) $^ $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $^
