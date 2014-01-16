CC = gcc
CFLAGS = -Wall -O2 -pthread
LDFLAGS = -pthread
ALL_TARGETS = mtsync mtrm mtoutliers

all: $(ALL_TARGETS)

clean:
	rm -f $(ALL_TARGETS) *.o

mtsync: threadpool.o mtpt.o exclude.o mtsync.o
	$(CC) $^ $(LDFLAGS) -o $@

mtrm: threadpool.o mtpt.o exclude.o mtrm.o
	$(CC) $^ $(LDFLAGS) -o $@

mtoutliers: threadpool.o mtpt.o exclude.o mtoutliers.o
	$(CC) $^ $(LDFLAGS) -o $@

mtpt-test: threadpool.o mtpt.o mtpt-test.o
	$(CC) $^ $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $^
