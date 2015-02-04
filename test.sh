#!/bin/sh

clean_exit () {
	rm -f "$TF"
	exit $1
}

TF="$(mktemp)"
IN=test.input
COMP=out.compressed
OUT=out.final

LZJODY=./lzjody
test -x lzjody.static && LZJODY=./lzjody.static

test ! -x $LZJODY && echo "Compile the program first." && clean_exit 1

CFAIL=0; DFAIL=0
$LZJODY -c < $IN > $COMP 2>log.test.compress || CFAIL=1
if [ $CFAIL -eq 0 ]
	then $LZJODY -d < $COMP > $OUT 2>log.test.decompress || DFAIL=1
fi

test $CFAIL -eq 1 && echo -e "\nCompressor block test FAILED.\n" && clean_exit 1
test $DFAIL -eq 1 && echo -e "\nDecompressor block test FAILED.\n" && clean_exit 1

# Check hashes
S1="$(sha1sum $IN | cut -d' ' -f1)"
S2="$(sha1sum $OUT | cut -d' ' -f1)"
test "$S1" != "$S2" && echo -e "\nCompressor/decompressor tests FAILED: mismatched hashes.\n" && clean_exit 1


### Decompressor tests

# Out-of-bounds length tests

echo -en '\x10\x05\xaf\xff' > $TF
dd if=/dev/zero bs=4095 count=1 2>/dev/null >> $TF
./lzjody.static -d < $TF 2>/dev/null && \
	echo -e "\nDecompressor invalid (large) length test FAILED.\n" && clean_exit 1

echo -en '\x00\x00\x00' | ./lzjody.static -d 2>/dev/null && \
	echo -e "\nDecompressor invalid (zero) length test FAILED.\n" && clean_exit 1


### All tests passed!
echo -e "\nCompressor/decompressor tests PASSED.\n"
