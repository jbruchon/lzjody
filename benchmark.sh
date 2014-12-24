#!/bin/sh

# Benchmark lzjb against other algorithms

test ! -x ./lzjb.static && echo "Build lzjb first." && exit 1

test ! -e "$1" && echo "Specify a file to benchmark." && exit 1

for X in ./lzjb.static lzop gzip bzip2 xz
	do echo -n "$X: "
	T1=$(date +%s)
	$X -c < "$1" 2>/dev/null | wc -c | tr -d \\n
	T2=$(date +%s)
	echo " bytes in $((T2 - T1)) seconds"
done
