#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <lzjb.h>
#include "lzjb_util.h"

int main(int argc, char **argv)
{
	struct files_t file_vars;
	struct files_t *files = &file_vars;
	unsigned char blk[LZJB_BSIZE + 4], out[LZJB_BSIZE];
	int i = 0;
	int length;
	unsigned char options = 0;
	int blocknum = 0;
	unsigned int file_loc = 0;

	if (argc < 2) goto usage;
	files->in = stdin;
	files->out = stdout;
	/* Set FAST_LZ */
	/* options |= O_FAST_LZ; */

	if (!strncmp(argv[1], "-c", 2)) {
		/* Read input, compress it, write compressed output */
		while((length = fread(blk, 1, LZJB_BSIZE, files->in))) {
			if (ferror(files->in)) goto error_read;
			DLOG("\n---\nBlock %d\n", blocknum);
			i = lzjb_compress(blk, out, options, length);
			DLOG("c_size %d bytes\n", i);
			i = fwrite(out, i, 1, files->out);
			if (!i) goto error_write;
			blocknum++;
		}
	}
	if (!strncmp(argv[1], "-d", 2)) {
		while(fread(blk, 1, 2, files->in)) {
			file_loc += 2;

			/* Read the length of the compressed data */
			length = *blk;
			length |= (*(blk + 1) << 8);
			if (length > (LZJB_BSIZE + 8)) goto error_blocksize_d_prefix;

			DLOG("\n---\nBlock %d, c_size %d\n", blocknum, length);

			i = fread(blk, 1, length, files->in);
			if (ferror(files->in)) goto error_read;
			if (i != length) goto error_shortread;

			length = lzjb_decompress(blk, out, i);
			if (length < 0) goto error_decompress;

			file_loc += length;

			DLOG("[%x]: unc_size %d bytes\n", file_loc, length);

			if (length > LZJB_BSIZE) goto error_blocksize_decomp;

			i = fwrite(out, 1, length, files->out);
//			DLOG("Wrote %d bytes\n", i);

			if (i != length) goto error_write;
			blocknum++;
		}
	}

	exit(EXIT_SUCCESS);

error_read:
	fprintf(stderr, "error reading file %s\n", "stdin");
	exit(EXIT_FAILURE);
error_write:
	fprintf(stderr, "error writing file %s (%d of %d written)\n", "stdout",
			i, length);
	exit(EXIT_FAILURE);
error_shortread:
	fprintf(stderr, "error: short read: %d < %d\n", i, length);
	exit(EXIT_FAILURE);
error_blocksize_d_prefix:
	fprintf(stderr, "error: decompressor prefix too large (%d > %d) \n",
			length, (LZJB_BSIZE + 8));
	exit(EXIT_FAILURE);
error_blocksize_decomp:
	fprintf(stderr, "error: decompressor overflow (%d > %d) \n",
			length, LZJB_BSIZE);
	exit(EXIT_FAILURE);
error_decompress:
	fprintf(stderr, "error: cannot decompress block %d\n", blocknum);
	exit(EXIT_FAILURE);
usage:
	fprintf(stderr, "lzjb %s, a compression utility by Jody Bruchon (%s)\n",
			LZJB_UTIL_VER, LZJB_UTIL_VERDATE);
	fprintf(stderr, "\nlzjb -c   compress stdin to stdout\n");
	fprintf(stderr, "\nlzjb -d   decompress stdin to stdout\n");
	exit(EXIT_FAILURE);
}

