#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>

#define VER "0.1"
#define VERDATE "2014-11-18"

/* Debugging stuff */
/* #define DEBUG 1 */
#ifdef DEBUG
#define DLOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DLOG(...)
#endif

/* Machine word size detection */
#if UINTPTR_MAX == 0xffff
#define WORD_SIZE 16
#elif UINTPTR_MAX == 0xffffffff
#define WORD_SIZE 32
#elif UINTPTR_MAX == 0xffffffffffffffff
#define WORD_SIZE 64
#else
#error Machine word size not recognized
#endif

/* Amount of data to process at a time */
#define B_SIZE 4096

/* Top 3 bits of a control byte */
#define P_SHORT	0x80	/* Compact control byte form */
#define P_LZ	0x60
#define P_RLE	0x40
#define P_LIT	0x20

/* Control bits masking value */
#define P_MASK	0x60
/* Maximum length of a short element */
#define P_SHORT_MAX 0x1f
/* Minimum sizes for compression */
#define MIN_LZ_MATCH 5
#define MIN_RLE_LENGTH 3

/* Options for the compressor */
#define O_FAST_LZ 0x01

struct files_t {
	FILE *in;
	FILE *out;
};

struct comp_data_t {
	unsigned char *in;
	unsigned char *out;
	unsigned int ipos, opos;
	unsigned int literals;
	unsigned int literal_start;
	unsigned int length;	/* Length of input data */
	int fast_lz;	/* 0=exhaustive search, 1=stop at first match */
};

/* Write the control byte(s) that define data
 * type is the P_xxx value that determines the type of the control byte */
static inline int lzjb_write_control(struct comp_data_t *data, const unsigned char type, const unsigned int value)
{
	if (value > P_SHORT_MAX) {
		*(data->out + data->opos) = type | (value >> 8);
		data->opos++;
		*(data->out + data->opos) = value & 0xff;
		data->opos++;
	} else {	/* For P_SHORT_MAX or less, use compact form */
		*(data->out + data->opos) = (type | P_SHORT | value);
		data->opos++;
	}
	return 0;
}

/* Write out all pending literals */
static inline int lzjb_flush_literals(struct comp_data_t *data)
{
	int i = 0;

	if (data->literals > 0) {
		DLOG("flush_literals: 0x%x\n", data->literals);
		/* First write the control byte... */
		lzjb_write_control(data, P_LIT, data->literals);
		/* ...then the literal bytes. */
		while (i < data->literals) {
			*(data->out + data->opos) = *(data->in + data->literal_start + i);
			data->opos++;
			i++;
		}
		/* Reset literal counter*/
		data->literals = 0;
	}
	return 0;
}

/* Find best LZ data match for current input position */
static inline int lzjb_find_lz(struct comp_data_t *data)
{
	unsigned int scan = 0;
	register unsigned char *m1, *m2;	/* pointers for matches */
	register unsigned char *lim1, *lim2;	/* m1/m2 limits */
	unsigned int length;	/* match length */
	unsigned int best_lz = 0;
	unsigned int best_lz_start = 0;

	scan = 0;
	while (scan < data->ipos) {
		m1 = data->in + scan;
		m2 = data->in + data->ipos;
		lim1 = data->in + data->ipos;
		lim2 = data->in + data->length;
		length = 0;
		while (1) {
			/* Large matches
			 * This attempts to take advantage of machine word
			 * sized checking to speed up compression.
			 */
/* Large matches as written here do not improve performance...YET! */
/*			typedef uint64_t lm_t;
			if ((uintptr_t)m1 & (uintptr_t)(WORD_SIZE - 1)) goto end_lz_large_match;
			if ((uintptr_t)m2 & (uintptr_t)(WORD_SIZE - 1)) goto end_lz_large_match;
			if (length < (255 - sizeof(lm_t))) goto end_lz_large_match;
			if ((b2 + sizeof(lm_t)) >= data->length) goto end_lz_large_match;
			if ((b1 + sizeof(lm_t)) >= data->ipos) goto end_lz_large_match;
			if (*(lm_t *)m1 == *(lm_t *)m2) {
				length++;
				if (length == 255) {
					DLOG("LZ: maximum length reached\n");
					goto end_lz_match;
				}
				b1 += sizeof(lm_t);
				b2 += sizeof(lm_t);
				m1 += sizeof(lm_t);
				m2 += sizeof(lm_t);
			}
end_lz_large_match:*/
			/* Match single bytes */
			if (*m1 == *m2) {
				if (m2 == lim2) {
					DLOG("LZ: hit end of data\n");
					goto end_lz_match;
				}
				if (m1 == lim1) {
					DLOG("LZ: hit end of dictionary\n");
					goto end_lz_match;
				}
				length++;
				if (length == 255) {
					DLOG("LZ: maximum length reached\n");
					goto end_lz_match;
				}
				m1++; m2++;
			} else goto end_lz_match;
		}
end_lz_match:
		/* If this run was the longest match, record it */
		if ((length >= MIN_LZ_MATCH) && (length > best_lz)) {
			DLOG("LZ found match: %d:%d\n", scan, length);
			best_lz_start = scan;
			best_lz = length;
			if (data->fast_lz) break;	/* Accept first LZ match */
			if (length == 255) break;
		}
		scan++;
	}
	/* Write out the best LZ match, if any */
	if (best_lz) {
		lzjb_flush_literals(data);
		lzjb_write_control(data, P_LZ, best_lz_start);
		/* Write LZ match length */
		*(data->out + data->opos) = best_lz;
		data->opos++;
		/* Skip matched input */
		data->ipos += best_lz;
		DLOG("LZ compressed %x:%x bytes\n", best_lz_start, best_lz);
		return 1;
	}
	return 0;
}

/* Find best RLE data match for current input position */
static inline int lzjb_find_rle(struct comp_data_t *data)
{
	register unsigned char c;
	unsigned int length = 0;

	c = *(data->in + data->ipos);
	while (((length + data->ipos) < data->length) && (*(data->in + data->ipos + length) == c)) {
		length++;
		//fprintf(stderr, "length %d (%02x = %02x)\n", length, c, *(data->in + data->ipos + length));
	}
	if (length >= MIN_RLE_LENGTH) {
		lzjb_flush_literals(data);
		lzjb_write_control(data, P_RLE, length);
		/* Write repeated byte */
		*(data->out + data->opos) = c;
		data->opos++;
		/* Skip matched input */
		data->ipos += length;
		DLOG("RLE compressed 0x%02x bytes of 0x%02x at %x/%x\n",
				length, c, data->ipos, data->opos);
		return 1;
	}
	return 0;
}

/* Lempel-Ziv compressor by Jody Bruchon (LZJB)
 * Compresses "blk" data and puts result in "out"
 * out must be at least 2 bytes larger than blk in case
 * the data is not compressible at all.
 * Returns the size of "out" data or returns -1 if the
 * compressed data is not smaller than the original data.
 */
int lzjb_compress(unsigned char *blk_in, unsigned char *blk_out, const unsigned int options, const int length)
{
	struct comp_data_t comp_data;
	struct comp_data_t *data = &comp_data;

	/* Initialize compression data structure */
	data->in = blk_in;
	data->out = blk_out;
	data->ipos = 0;
	data->opos = 2;
	data->literals = 0;
	data->fast_lz = options & O_FAST_LZ;
	data->length = length;

	DLOG("Compressing block of length %d\n", length);
	/* Scan through entire block looking for compressible items */
	while (data->ipos < length) {

		/* Scan for compressible items
		 * Try each compressor in sequence; if none works,
		 * just add the byte to the literal stream
		 */
		if (!lzjb_find_rle(data)) {
			if (!lzjb_find_lz(data)) {
				if (data->literals == 0)
					data->literal_start = data->ipos;
				data->literals++;
				data->ipos++;
			}
		}
		DLOG("ipos: %d\n", data->ipos);
	}
	
	/* Flush any remaining literals */
	lzjb_flush_literals(data);

	/* Write the total length to the data block */
	*(data->out) = (unsigned char)(data->opos - 2);
	*(data->out + 1) = (unsigned char)(((data->opos - 2) & 0xff00) >> 8);

	if (data->opos >= length)
		DLOG("warning: incompressible block\n");
	return data->opos;
}

/* LZJB decompressor */
int lzjb_decompress(const unsigned char *in, unsigned char *out, const unsigned int size)
{
	unsigned int mode;
	register unsigned int ipos = 0;
	register unsigned int opos = 0;
	unsigned int offset, length;
	unsigned int control;
	unsigned char c;

	while (ipos < size) {
		c = *(in + ipos);
		mode = c & P_MASK & ~P_SHORT;
		if (c & P_SHORT) {
			control = c & ~P_MASK & ~P_SHORT;
			ipos++;
		} else {
			control = (c & ~P_MASK) << 8;
			ipos++;
			control += *(in + ipos);
			ipos++;
		}
		switch (mode) {
			case P_LZ:
				offset = control;
				length = *(in + ipos);
				ipos++;
				DLOG("%04x:%04x: LZ block (%x:%x)\n", ipos, opos, offset, length);
				memcpy((out + opos), (out + offset), length);
				opos += length;
				break;
			case P_RLE:
				length = control;
				c = *(in + ipos);
				ipos++;
				DLOG("%04x:%04x: RLE run 0x%x\n", ipos, opos, length);
				while (length > 0) {
					*(out + opos) = c;
					opos++;
					length--;
				}
				break;
			case P_LIT:
				DLOG("%04x:%04x: Literal 0x%x\n", ipos, opos, control);
				memcpy((out + opos), (in + ipos), control);
				ipos += control;
				opos += control;
				break;
			default:
				fprintf(stderr, "Error: invalid mode 0x%x at 0x%x\n", mode, ipos);
				exit(EXIT_FAILURE);
		}
	}
	return opos;
}

int main(int argc, char **argv)
{
	struct files_t file_vars;
	struct files_t *files = &file_vars;
	unsigned char blk[B_SIZE], out[B_SIZE + 4];
	int i, length;
	unsigned char options = 0;
	int blocknum = 0;

	if (argc < 2) goto usage;
	files->in = stdin;
	files->out = stdout;
	/* Set FAST_LZ */
	/* options |= O_FAST_LZ; */

	if (!strncmp(argv[1], "-c", 2)) {
		/* Read input, compress it, write compressed output */
		while((length = fread(blk, 1, B_SIZE, files->in))) {
			if (ferror(files->in)) goto error_read;
			DLOG("\n   ---\nCompressor: block %d\n", blocknum);
			i = lzjb_compress(blk, out, options, length);
			DLOG("Compressor returned %d\n", i);
			i = fwrite(out, i, 1, files->out);
			if (!i) goto error_write;
			blocknum++;
		}
	}
	if (!strncmp(argv[1], "-d", 2)) {
		while(fread(blk, 1, 2, files->in)) {
			/* Read the length of the compressed data */
			length = *blk;
			length |= (*(blk + 1) << 8);
			DLOG("\n   ---\nDecompressor: block %d of size %d\n", blocknum, length);
			i = fread(blk, 1, length, files->in);
			if (ferror(files->in)) goto error_read;
			if (i != length) goto error_blocksize;
			i = lzjb_decompress(blk, out, i);
			DLOG("Decompressor returned %d\n", i);
			i = fwrite(out, 1, i, files->out);
			DLOG("Wrote %d bytes\n", i);
			if (i != B_SIZE) goto error_write;
			blocknum++;
		}
	}

	exit(EXIT_SUCCESS);

error_read:
	fprintf(stderr, "error reading file %s\n", "stdin");
	exit(EXIT_FAILURE);
error_write:
	fprintf(stderr, "error writing file %s\n", "stdout");
	exit(EXIT_FAILURE);
error_blocksize:
	fprintf(stderr, "error: short read: %d < %d\n", i, length);
	exit(EXIT_FAILURE);
usage:
	fprintf(stderr, "lzjb version %s, an LZ compression algorithm by Jody Bruchon (%s)\n", VER, VERDATE);
	fprintf(stderr, "\nlzjb. learn to use it.\n");
	exit(EXIT_FAILURE);
}
