#
# lzjody Makefile
#

CC=gcc
AR=ar
#CFLAGS=-O3 -ftree-vectorize -fgcse-las
# Try these if the compiler complains or you need to debug
CFLAGS=-O2 -g
#CFLAGS=-Og -g3
COMPILER_OPTIONS = -std=gnu99 -I. -D_FILE_OFFSET_BITS=64 -fstrict-aliasing -pipe
COMPILER_OPTIONS += -Wall -Wextra -Wwrite-strings -Wcast-align -Wstrict-aliasing -pedantic -Wstrict-overflow -Wstrict-prototypes -Wpointer-arith -Wundef
COMPILER_OPTIONS += -Wshadow -Wfloat-equal -Wstrict-overflow=5 -Waggregate-return -Wcast-qual -Wswitch-default -Wswitch-enum -Wunreachable-code -Wformat=2 -Winit-self
#COMPILER_OPTIONS += -Wconversion
LDFLAGS=-L.
LDLIBS=

prefix=${DESTDIR}/usr
exec_prefix=${prefix}
bindir=${exec_prefix}/bin
libdir=${exec_prefix}/lib
includedir=${prefix}/include
mandir=${prefix}/man
datarootdir=${prefix}/share
datadir=${datarootdir}
sysconfdir=${prefix}/etc

# Use POSIX threads if the user specifically requests it
ifdef THREADED
LDFLAGS += -lpthread
COMPILER_OPTIONS += -DTHREADED
endif

ifdef DEBUG
COMPILER_OPTIONS += -DDEBUG -g
endif

TARGETS = lzjody lzjody.static bpxfrm diffxfrm xorxfrm test

# On MinGW (Windows) only build static versions
ifeq ($(OS), Windows_NT)
        COMPILER_OPTIONS += -D__USE_MINGW_ANSI_STDIO=1
	TARGETS = lzjody.static bpxfrm test
	EXT = .exe
endif

COMPILER_OPTIONS += $(CFLAGS_EXTRA)

all: $(TARGETS)

xorxfrm: xorxfrm.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDLIBS) $(COMPILER_OPTIONS) -o xorxfrm$(EXT) xorxfrm.o

diffxfrm: diffxfrm.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDLIBS) $(COMPILER_OPTIONS) -o diffxfrm$(EXT) diffxfrm.o

bpxfrm: bpxfrm.o byteplane_xfrm.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDLIBS) $(COMPILER_OPTIONS) -o bpxfrm$(EXT) byteplane_xfrm.o bpxfrm.o

lzjody.static: liblzjody.a lzjody_util.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDLIBS) $(COMPILER_OPTIONS) -o lzjody.static$(EXT) lzjody_util.o liblzjody.a

lzjody: liblzjody.so lzjody_util.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDLIBS) $(COMPILER_OPTIONS) -o lzjody$(EXT) lzjody_util.o liblzjody.so

liblzjody.so: lzjody.c byteplane_xfrm.c
	$(CC) -c $(COMPILER_OPTIONS) -fPIC $(CFLAGS) -o byteplane_xfrm_shared.o byteplane_xfrm.c
	$(CC) -c $(COMPILER_OPTIONS) -fPIC $(CFLAGS) -o lzjody_shared.o lzjody.c
	$(CC) -shared -o liblzjody.so lzjody_shared.o byteplane_xfrm_shared.o

liblzjody.a: lzjody.c byteplane_xfrm.c
	$(CC) -c $(COMPILER_OPTIONS) $(CFLAGS) byteplane_xfrm.c
	$(CC) -c $(COMPILER_OPTIONS) $(CFLAGS) lzjody.c
	$(AR) rcs liblzjody.a lzjody.o byteplane_xfrm.o

stripped: lzjody lzjody.static bpxfrm
	strip --strip-debug liblzjody.so
	strip --strip-unneeded lzjody$(EXT) lzjody.static$(EXT) bpxfrm$(EXT)

#manual:
#	gzip -9 < lzjody.8 > lzjody.8.gz

.c.o:
	$(CC) -c $(COMPILER_OPTIONS) $(CFLAGS) $<

clean:
	rm -f *.o *.a *~ .*un~ *.so* debug.log *.?.gz
	rm -f lzjody$(EXT) lzjody*.static$(EXT) bpxfrm$(EXT) diffxfrm$(EXT) xorxfrm$(EXT)
	rm -f testdir/log.* testdir/out.*

distclean: clean
	rm -f *.pkg.tar.*

install: all
	install -D -o root -g root -m 0755 lzjody $(bindir)/lzjody
	install -D -o root -g root -m 0755 liblzjody.so $(libdir)/liblzjody.so
	install -D -o root -g root -m 0644 liblzjody.a $(libdir)/liblzjody.a
	install -D -o root -g root -m 0644 lzjody.h $(includedir)/lzjody.h
#	install -D -o root -g root -m 0644 lzjody.8.gz $(mandir)/man8/lzjody.8.gz
	install -D -o root -g root -m 0755 bpxfrm $(bindir)/bpxfrm
	install -D -o root -g root -m 0755 diffxfrm $(bindir)/diffxfrm
	install -D -o root -g root -m 0755 diffxfrm $(bindir)/xorxfrm

test: lzjody.static
	./test.sh

package:
	+./chroot_build.sh
