CC = gcc
CFLAGS = -Wall -O2 -pthread
LDFLAGS = -pthread
INSTALL_PROGRAM = install
DESTDIR = /usr/local
bindir = /bin
ALL_TARGETS = mtsync mtrm mtoutliers mtdu

.PHONY: all clean install uninstall

all: $(ALL_TARGETS)

clean:
	rm -f $(ALL_TARGETS) *.o

install: $(ALL_TARGETS)
	$(INSTALL_PROGRAM) mtsync $(DESTDIR)$(bindir)/mtsync
	$(INSTALL_PROGRAM) mtrm $(DESTDIR)$(bindir)/mtrm
	$(INSTALL_PROGRAM) mtoutliers $(DESTDIR)$(bindir)/mtoutliers
	$(INSTALL_PROGRAM) mtdu $(DESTDIR)$(bindir)/mtdu

uninstall:
	rm -f $(DESTDIR)$(bindir)/mtsync
	rm -f $(DESTDIR)$(bindir)/mtrm
	rm -f $(DESTDIR)$(bindir)/mtoutliers
	rm -f $(DESTDIR)$(bindir)/mtdu

mtsync: threadpool.o mtpt.o exclude.o mtsync.o
	$(CC) $^ $(LDFLAGS) -o $@

mtrm: threadpool.o mtpt.o exclude.o mtrm.o
	$(CC) $^ $(LDFLAGS) -o $@

mtoutliers: threadpool.o mtpt.o exclude.o mtoutliers.o
	$(CC) $^ $(LDFLAGS) -o $@

mtdu: threadpool.o mtpt.o exclude.o mtdu.o
	$(CC) $^ $(LDFLAGS) -o $@ -lm

mtpt-test: threadpool.o mtpt.o mtpt-test.o
	$(CC) $^ $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $^
