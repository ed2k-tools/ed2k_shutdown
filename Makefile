
DEBIAN_EXTRA_DIST = debian/control debian/rules debian/changelog
EXTRA_DIST = Makefile AUTHORS COPYING INSTALL README ed2k_shutdown.1
SOURCES = ed2k_shutdown.c

CC = gcc
CFLAGS = -g -O2 -Wall `pkg-config --cflags glib-2.0 gnet-2.0`

VERSION = $(shell cat ed2k_shutdown.c | grep VERSION | grep define | sed -e 's/^[^"]*//' | sed -e s/\"//g)

OBJECTS = ed2k_shutdown.o

prefix=/usr/local

ed2k_shutdown: $(OBJECTS)
	$(CC) -o ed2k_shutdown $(OBJECTS) `pkg-config --libs gnet-2.0 glib-2.0`


clean:
	rm ed2k_shutdown *.o 2>/dev/null || /bin/true


distclean: clean
	rm -rf ed2k_shutdown-$(VERSION).tar.gz ed2k_shutdown-$(VERSION)/ 2>/dev/null || /bin/true



dist:
	rm -rf ed2k_shutdown-$(VERSION).tar.gz ed2k_shutdown-$(VERSION)/ 2>/dev/null || /bin/true
	mkdir ed2k_shutdown-$(VERSION)/ ed2k_shutdown-$(VERSION)/debian/
	cp $(SOURCES) $(EXTRA_DIST) ed2k_shutdown-$(VERSION)/
	cp $(DEBIAN_EXTRA_DIST) ed2k_shutdown-$(VERSION)/debian/
	tar czf ed2k_shutdown-$(VERSION).tar.gz ed2k_shutdown-$(VERSION)/*
	rm -rf ed2k_shutdown-$(VERSION)/ 2>/dev/null || /bin/true

static: $(OBJECTS)
	$(CC) -o ed2k_shutdown $(OBJECTS) -pthread `pkg-config --libs glib-2.0 gthread-2.0` /usr/lib/libgnet-2.0.a

install:
	@echo "About to install ed2k_shutdown into $(prefix)/bin ... (waiting 3 secs)"
	@sleep 3
	/usr/bin/install -c -D -s ed2k_shutdown "$(prefix)/bin/ed2k_shutdown"
	/usr/bin/install -c -D -m 644 ed2k_shutdown.1 $(prefix)/share/man/man1/ed2k_shutdown.1

