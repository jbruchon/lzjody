#!/bin/sh
IN=test.input
COMP=out.compressed
OUT=out.final

test ! -x ./lzjb && echo "Compile the program first." && exit 1
./lzjb -c < $IN > $COMP 2>log.test.compress
./lzjb -d < $COMP > $OUT 2>log.test.decompress
sha1sum $IN $OUT
