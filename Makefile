CC=gcc
AR=ar
#CFLAGS=-Os -flto -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables
CFLAGS=-O3 -g
#CFLAGS=-Og -g3
BUILD_CFLAGS=-std=gnu99 -I. -D_FILE_OFFSET_BITS=64 -pipe -Wall -pedantic -I.
#LDFLAGS=-s -Wl,--gc-sections
#LDFLAGS=
LDLIBS=-L.

prefix=${DESTDIR}/usr
exec_prefix=${prefix}
bindir=${exec_prefix}/bin
libdir=${exec_prefix}/lib
includedir=${prefix}/include
mandir=${prefix}/man
datarootdir=${prefix}/share
datadir=${datarootdir}
sysconfdir=${prefix}/etc

all: lzjb.o liblzjb.a liblzjb.so lzjb lzjb.static test

lzjb.static: liblzjb.a lzjb_util.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDLIBS) $(BUILD_CFLAGS) -o lzjb.static lzjb_util.o liblzjb.a

lzjb: liblzjb.so lzjb_util.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDLIBS) $(BUILD_CFLAGS) -llzjb -o lzjb lzjb_util.o

lzjb.o: liblzjb.so liblzjb.a

liblzjb.so:
	$(CC) -c $(BUILD_CFLAGS) -fPIC $(CFLAGS) lzjb.c
	$(CC) -shared -o liblzjb.so lzjb.o

liblzjb.a:
	$(CC) -c $(BUILD_CFLAGS) $(CFLAGS) lzjb.c
	$(AR) rcs liblzjb.a lzjb.o

#manual:
#	gzip -9 < lzjb.8 > lzjb.8.gz

.c.o:
	$(CC) -c $(BUILD_CFLAGS) $(CFLAGS) $<

clean:
	rm -f *.o *.a *~ .*un~ lzjb lzjb.static *.so* debug.log *.?.gz log.test.* out.*

distclean:
	rm -f *.o *.a *~ .*un~ lzjb lzjb.static *.so* debug.log *.?.gz log.test.* out.* *.pkg.tar.*

install: all
	install -D -o root -g root -m 0755 lzjb $(bindir)/lzjb
	install -D -o root -g root -m 0755 liblzjb.so $(libdir)/liblzjb.so
	install -D -o root -g root -m 0644 liblzjb.a $(libdir)/liblzjb.a
	install -D -o root -g root -m 0644 lzjb.h $(includedir)/lzjb.h
#	install -D -o root -g root -m 0644 lzjb.8.gz $(mandir)/man8/lzjb.8.gz

test:
	./test.sh

package:
	+./chroot_build.sh
