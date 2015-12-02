/*
 * Lempel-Ziv-JodyBruchon compression library
 *
 * Copyright (C) 2014, 2015 by Jody Bruchon <jody@jodybruchon.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "lzjody.h"
#include "lzjody_util.h"

/* Detect Windows and modify as needed */
#if defined _WIN32 || defined __CYGWIN__
 #define ON_WINDOWS 1
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <io.h>
#endif

#ifdef THREADED
#include <pthread.h>
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond;	/* pthreads change condition */
static int thread_error;	/* nonzero if any thread fails */
#endif

/* Debugging stuff */
#ifndef DLOG
 #ifdef DEBUG
  #define DLOG(...) fprintf(stderr, __VA_ARGS__)
 #else
  #define DLOG(...)
 #endif
#endif

struct files_t files;


/* Some functions from jody_endian.c
 * Detect endianness at runtime and provide conversion functions
 */

/* Returns 1 for little-endian, 0 for big-endian */
static inline int detect_endianness(void)
{
        union {
                uint32_t big;
                uint8_t p[4];
        } i = {0x76543210};

        if (i.p[0] == 0x76) return 0;
        else return 1;

}


static uint32_t u32_endian_reverse(const uint32_t x)
{
        union {
                uint32_t big;
                uint8_t p[4];
        } in;
        union {
                uint32_t big;
                uint8_t p[4];
        } out;

        in.big = x;

        out.p[0] = in.p[3];
        out.p[1] = in.p[2];
        out.p[2] = in.p[1];
        out.p[3] = in.p[0];

        return out.big;
}


#ifdef THREADED
static void *compress_thread(void *arg)
{
	struct thread_info * const thr = arg;
	const unsigned char *ipos = thr->blk;	/* Uncompressed input pointer */
	unsigned char *opos = thr->out;	/* Compressed output pointer */
	int i;
	int bsize = LZJODY_MAX_BSIZE;	/* Compressor block size */
	int remain = thr->length;	/* Remaining input bytes */

	while (remain) {
		if (remain < LZJODY_MAX_BSIZE) bsize = remain;
		i = lzjody_compress(ipos, opos, thr->options, bsize);
		if (i < 0) {
			thread_error = 1;
			pthread_cond_signal(&cond);
		}
		ipos += bsize;
		opos += i;
		remain -= bsize;
	}
	thr->o_length = opos - thr->out;
	thr->working = 0;
	pthread_cond_signal(&cond);

	return 0;
}
#endif /* THREADED */

int main(int argc, char **argv)
{
	static unsigned char blk[LZJODY_MAX_BSIZE];
	static unsigned char out[LZJODY_MAX_BSIZE + 8];
	int i;
	union {
		uint32_t len;
		uint8_t byte[4];
	} c;
	int length = 0;	/* Incoming data block length counter */
	int blocknum = 0;	/* Current block number */
	unsigned char options = 0;	/* Compressor options */
#ifdef THREADED
	struct thread_info *thr;
	int nprocs = 1;		/* Number of processors */
	int eof = 0;	/* End of file? */
	char running = 0;	/* Number of threads running */
#endif /* THREADED */

	if (argc < 2) goto usage;

	/* Windows requires that data streams be put into binary mode */
#ifdef ON_WINDOWS
	setmode(STDIN_FILENO, _O_BINARY);
	setmode(STDOUT_FILENO, _O_BINARY);
#endif /* ON_WINDOWS */

	files.in = stdin;
	files.out = stdout;

	if (!strncmp(argv[1], "-c", 2)) {
#ifndef THREADED
		/* Non-threaded compression */
		/* fprintf(stderr, "blk %p, blkend %p, files %p\n",
				blk, blk + LZJODY_MAX_BSIZE - 1, files); */
		while((length = fread(blk, 1, 4096, files.in))) {
			if (ferror(files.in)) goto error_read;
			DLOG("\n--- Compressing block %d\n", blocknum);
			i = lzjody_compress(blk, out + 4, options, length);
			/* Incompressible block storage */
			options = 0;
			if (i < 0) {
				i = length;
				options = O_NOCOMPRESS;
				memcpy(out + 4, blk, length);
			}
			if (detect_endianness()) c.len = u32_endian_reverse((uint32_t)i);
			else c.len = (uint32_t)i;
			out[0] = (c.byte[0] & 0x0f) | options;
			DLOG("i = %d, out0 = 0x%x (0x%x & 0x0f | 0x%x)\n",i,out[0],c.byte[0],options);
			out[1] = c.byte[1];
			out[2] = c.byte[2];
			out[3] = c.byte[3];
			i += 4;
			DLOG("c_size %d bytes\n", i);
			i = fwrite(out, i, 1, files.out);
			if (!i) goto error_write;
			blocknum++;
		}

#else /* Using POSIX threads */

 #ifdef _SC_NPROCESSORS_ONLN
		/* Get number of online processors for pthreads */
		nprocs = (int)sysconf(_SC_NPROCESSORS_ONLN);
		if (nprocs < 1) {
			fprintf(stderr, "warning: system returned bad number of processors: %d\n", nprocs);
			nprocs = 1;
		}
 #endif /* _SC_NPROCESSORS_ONLN */
		/* Run two threads per processor */
		nprocs <<= 1;
		fprintf(stderr, "lzjody: compressing with %d worker threads\n", nprocs);

		/* Allocate per-thread input/output memory and control blocks */
		thr = (struct thread_info *)calloc(nprocs, sizeof(struct thread_info));
		if (!thr) goto oom;

		/* Set compressor options */
		for (i = 0; i < nprocs; i++) (thr + i)->options = options;

		thread_error = 0;
		while (1) {
			struct thread_info *cur = NULL;
			uint32_t min_blk;	/* Minimum block number */
			unsigned int min_thread;	/* Thread for min_blk */
			int thread;	/* Temporary thread scan counter */
			int open_thr;	/* Next open thread */

			/* See if lowest block number is finished */
			while (1) {
				min_blk = 0xffffffff;
				min_thread = 0;
				/* Scan threads for smallest block number */
				pthread_mutex_lock(&mtx);
				for (thread = 0; thread < nprocs; thread++) {
					unsigned int j;

				fprintf(stderr, ":thr %p, thread %d\n",
						(void *)thr, thread);
					if (thread_error != 0) goto error_compression;
					j = (thr + thread)->block;
					if (j > 0 && j < min_blk) {
						min_blk = j;
						min_thread = thread;
				fprintf(stderr, ":j%d:%d thr %p, cur %p, min_thread %d\n",
						j, min_blk, (void *)thr, (void *)cur, min_thread);
					}
				}
				pthread_mutex_unlock(&mtx);

				cur = thr + min_thread;
				fprintf(stderr, "thr %p, cur %p, min_thread %d\n",
						(void *)thr, (void *)cur, min_thread);
				if (cur->working == 0 && cur->length > 0) {
					pthread_detach(cur->id);
					/* flush finished block */
					i = fwrite(cur->out, cur->o_length, 1, files.out);
					if (!i) goto error_write;
					cur->block = 0;
					cur->length = 0;
					DLOG("Thread %d done\n", min_thread);
					running--;
				} else break;
			}

			/* Terminate when all blocks are written */
			if (eof && (running == 0)) break;

			/* Start threads */
			if (running < nprocs) {
				/* Don't read any more if EOF reached */
				if (!eof) {
					/* Find next open thread */
					cur = thr;
					for (open_thr = 0; open_thr < nprocs; open_thr++) {
						if (cur->working == 0 && cur->block == 0) break;
						cur++;
					}

					/* If no threads are available, wait for one */
					if (open_thr == nprocs) {
						pthread_mutex_lock(&mtx);
						pthread_cond_wait(&cond, &mtx);
						pthread_mutex_unlock(&mtx);
						continue;
					}

					/* Read next block */
					length = fread(cur->blk, 1, (LZJODY_MAX_BSIZE * CHUNK), files.in);
					if (ferror(files.in)) goto error_read;
					if (length < (LZJODY_MAX_BSIZE * CHUNK)) eof = 1;
					if (length > 0) {
						blocknum++;

						/* Set up thread */
						cur->working = 1;
						cur->block = blocknum;
						cur->length = length;
						cur->o_length = 0;
						running++;
						DLOG("Thread %d start\n", open_thr);

						/* Start thread */
						pthread_create(&(cur->id), NULL,
								compress_thread,
								(void *)cur);
					} else eof = 1;
				} else if (running > 0) {
					/* EOF but threads still running */
					pthread_mutex_lock(&mtx);
					pthread_cond_wait(&cond, &mtx);
					pthread_mutex_unlock(&mtx);
				}
			}
		}
		free(thr);
#endif /* THREADED */
	}

	/* Decompress */
	if (!strncmp(argv[1], "-d", 2)) {
		while(fread(blk, 1, 4, files.in)) {
			/* Get block-level decompression options */
			options = blk[0] & 0xf0;

			/* Read the length of the compressed data */
			c.byte[0] = blk[0] & 0x0f;
			c.byte[1] = blk[1];
			c.byte[2] = blk[2];
			c.byte[3] = blk[3];
			if (detect_endianness()) length = u32_endian_reverse((uint32_t)c.len);
			else length = (uint32_t)c.len;
			DLOG("o %x, length = 0x%x [%x %x %x %x]\n", options,length,blk[0],blk[1],blk[2],blk[3]);
			if (length > (4096 + 4)) goto error_blocksize_d_prefix;

			i = fread(blk, 1, length, files.in);
			if (ferror(files.in)) goto error_read;
			if (i != length) goto error_shortread;

			if (options & O_NOCOMPRESS) {
				DLOG("--- Writing uncompressed block %d (%d bytes)\n", blocknum, length);
				if (length > LZJODY_MAX_BSIZE) goto error_unc_length;
				i = fwrite(blk, 1, length, files.out);
				if (i != length) goto error_write;
			} else {
				DLOG("--- Decompressing block %d\n", blocknum);
				length = lzjody_decompress(blk, out + 4, i, options);
				if (length < 0) goto error_decompress;
				if (length > 4096) goto error_blocksize_decomp;
				i = fwrite(out + 4, 1, length, files.out);
				if (i != length) goto error_write;
				DLOG("Wrote %d bytes\n", i);
			}


			blocknum++;
		}
	}

	exit(EXIT_SUCCESS);

error_compression:
	fprintf(stderr, "Fatal error during compression, aborting.\n");
	exit(EXIT_FAILURE);
error_read:
	fprintf(stderr, "Error reading file %s\n", "stdin");
	exit(EXIT_FAILURE);
error_write:
	fprintf(stderr, "Error writing file %s (%d of %d written)\n", "stdout",
			i, length);
	exit(EXIT_FAILURE);
error_shortread:
	fprintf(stderr, "Error: short read: %d < %d (eof %d, error %d)\n",
			i, length, feof(files.in), ferror(files.in));
	exit(EXIT_FAILURE);
error_unc_length:
	fprintf(stderr, "Error: uncompressed length too large (%d > %d)\n",
			length, LZJODY_MAX_BSIZE);
	exit(EXIT_FAILURE);
error_blocksize_d_prefix:
	fprintf(stderr, "Error: decompressor prefix too large (%d > %d)\n",
			length, (LZJODY_MAX_BSIZE + 4));
	exit(EXIT_FAILURE);
error_blocksize_decomp:
	fprintf(stderr, "Error: decompressor overflow (%d > %d)\n",
			length, LZJODY_MAX_BSIZE);
	exit(EXIT_FAILURE);
error_decompress:
	fprintf(stderr, "Error: cannot decompress block %d\n", blocknum);
	exit(EXIT_FAILURE);
#ifdef THREADED
oom:
	fprintf(stderr, "Error: out of memory\n");
	exit(EXIT_FAILURE);
#endif
usage:
	fprintf(stderr, "lzjody %s, a compression utility by Jody Bruchon (%s)\n",
			LZJODY_UTIL_VER, LZJODY_UTIL_VERDATE);
	fprintf(stderr, "\nlzjody -c   compress stdin to stdout\n");
	fprintf(stderr, "\nlzjody -d   decompress stdin to stdout\n");
	exit(EXIT_FAILURE);
}
