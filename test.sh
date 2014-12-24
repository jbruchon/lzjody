#!/bin/sh

IN=test.input
COMP=out.compressed
OUT=out.final

LZJB=./lzjb
test -x lzjb.static && LZJB=./lzjb.static

test ! -x $LZJB && echo "Compile the program first." && exit 1

CFAIL=0; DFAIL=0
$LZJB -c < $IN > $COMP 2>log.test.compress || CFAIL=1
if [ $CFAIL -eq 0 ]
	then $LZJB -d < $COMP > $OUT 2>log.test.decompress || DFAIL=1
fi

test $CFAIL -eq 1 && echo -e "\nCompressor test FAILED. Decompressor test not performed.\n" && exit 1
test $DFAIL -eq 1 && echo -e "\nDecompressor test FAILED.\n" && exit 1
test $CFAIL -eq 0 && test $DFAIL -eq 0 && echo -e "\nCompressor/decompressor tests PASSED.\n"

sha1sum $IN $OUT
