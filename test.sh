#!/bin/sh

IN=test.input
COMP=out.compressed
OUT=out.final

LZJB=./lzjb
test -x lzjb.static && LZJB=./lzjb.static

test ! -x $LZJB && echo "Compile the program first." && exit 1
$LZJB -c < $IN > $COMP 2>log.test.compress
$LZJB -d < $COMP > $OUT 2>log.test.decompress
sha1sum $IN $OUT
