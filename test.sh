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

# Check hashes
S1="$(sha1sum $IN | cut -d' ' -f1)"
S2="$(sha1sum $OUT | cut -d' ' -f1)"
test "$S1" != "$S2" && echo -e "\nCompressor/decompressor tests FAILED: mismatched hashes\n" && exit 1

echo -e "\nCompressor/decompressor tests PASSED.\n"
