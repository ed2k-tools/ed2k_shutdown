
CC = gcc
CFLAGS = -ggdb -O2 `gnet-config --cflags`

OBJECTS = ed2k_shutdown.o

prefix=/usr/local

ed2k_shutdown: $(OBJECTS)
								$(CC) -o ed2k_shutdown $(OBJECTS) `gnet-config --libs`


clean:
	rm ed2k_shutdown || /bin/true
	rm *.o || /bin/true


static: $(OBJECTS)
					$(CC) -static -o ed2k_shutdown $(OBJECTS) `gnet-config --libs`

install:
	install -D -s ed2k_shutdown "$(prefix)/bin/ed2k_shutdown"

