#!/bin/sh

# Benchmark lzjb against other algorithms

test ! -x ./lzjb.static && echo "Build lzjb first." && exit 1

for X in ./lzjb.static lzop gzip bzip2 xz
	do echo -n "$X: "
	$X -c < test.input 2>/dev/null | wc -c
done
