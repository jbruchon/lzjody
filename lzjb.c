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
#define P_LZ	0x60	/* LZ (dictionary) compression */
#define P_RLE	0x40	/* RLE compression */
#define P_LIT	0x20	/* Literal values */
#define P_EXT	0x00	/* Extended algorithms */
#define P_SEQ32	0x03	/* Sequential 32-bit values */
#define P_SEQ16	0x02	/* Sequential 16-bit values */
#define P_SEQ8	0x01	/* Sequential 8-bit values */

/* Control bits masking value */
#define P_MASK	0x60	/* LZ, RLE, literal (no short) */
#define P_XMASK 0x0f	/* Extended command */
#define P_SMASK 0x03	/* Sequence compression commands */

/* Maximum length of a short element */
#define P_SHORT_MAX 0x1f
#define P_SHORT_XMAX 0xff

/* Minimum sizes for compression
 * These sizes are calculated as follows:
 * control byte(s) + data byte(s) + 2 next control byte(s)
 * This avoids data expansion cause by interrupting a stream
 * of literals (which triggers up to 2 more control bytes)
 */
#define MIN_LZ_MATCH 6
#define MIN_RLE_LENGTH 5
#define MIN_SEQ32_LENGTH 9
#define MIN_SEQ16_LENGTH 7
#define MIN_SEQ8_LENGTH 6

/* Options for the compressor */
#define O_FAST_LZ 0x01

struct files_t {
	FILE *in;
	FILE *out;
};

struct comp_data_t {
	const unsigned char * const in;
	unsigned char * const out;
	unsigned int ipos;
	unsigned int opos;
	unsigned int literals;
	unsigned int literal_start;
	unsigned int length;	/* Length of input data */
	int fast_lz;	/* 0=exhaustive search, 1=stop at first match */
};

/* Write the control byte(s) that define data
 * type is the P_xxx value that determines the type of the control byte */
static inline int lzjb_write_control(struct comp_data_t * const data, const unsigned char type, const unsigned int value)
{
	/* Extended control bytes */
	if ((type & P_MASK) == P_EXT) {
		if (value > P_SHORT_XMAX) {
			/* Full size control bytes */
			*(data->out + data->opos) = type;
			data->opos++;
			*(data->out + data->opos) = (value >> 8);
			data->opos++;
			*(data->out + data->opos) = value & 0xff;
			data->opos++;
		} else {
			/* For P_SHORT_XMAX or less, use compact form */
			*(data->out + data->opos) = (type | P_SHORT);
			data->opos++;
			*(data->out + data->opos) = value & 0xff;
			data->opos++;

		}
	} else if (value > P_SHORT_MAX) {
		/* Standard control bytes */
		*(data->out + data->opos) = type | (value >> 8);
		data->opos++;
		*(data->out + data->opos) = value & 0xff;
		data->opos++;
	} else {
		/* For P_SHORT_MAX or less chars, use compact form */
		*(data->out + data->opos) = (type | P_SHORT | value);
		data->opos++;
	}
	return 0;
}

/* Write out all pending literals */
static inline int lzjb_flush_literals(struct comp_data_t * const data)
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
static inline int lzjb_find_lz(struct comp_data_t *const data)
{
	unsigned int scan = 0;
	const unsigned char *m1, *m2;	/* pointers for matches */
	const unsigned char *lim;	/* m1/m2 limits */
	unsigned int length;	/* match length */
	unsigned int best_lz = 0;
	unsigned int best_lz_start = 0;

	scan = 0;
	while (scan < data->ipos) {
		m1 = data->in + scan;
		m2 = data->in + data->ipos;
		lim = data->in + data->length;
		length = 0;
		/* Match single bytes */
		while (*m1 == *m2) {
			if (m2 == lim) {
				DLOG("LZ: hit end of data\n");
				goto end_lz_match;
			}
			length++;
			if (length == 255) {
				DLOG("LZ: maximum length reached\n");
				goto end_lz_match;
			}
			m1++; m2++;
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
static inline int lzjb_find_rle(struct comp_data_t *const data)
{
	register const unsigned char c = *(data->in + data->ipos);
	unsigned int length = 0;

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


/* Find sequential values for compression */
static inline int lzjb_find_seq(struct comp_data_t * const data)
{
	uint8_t num8;
	uint8_t *m8 = (uint8_t *)((uintptr_t)data->in + (uintptr_t)data->ipos);
	const uint8_t num_orig8 = *m8;
	uint16_t num16;
	uint16_t *m16 = (uint16_t *)((uintptr_t)data->in + (uintptr_t)data->ipos);
	const uint16_t num_orig16 = *m16;
	uint32_t num32;
	uint32_t *m32 = (uint32_t *)((uintptr_t)data->in + (uintptr_t)data->ipos);
	const uint32_t num_orig32 = *m32;
	int seqcnt;
	int compressed = 0;

	/* 32-bit sequences */
	seqcnt = 0;
//	m32 = (uint32_t *)((uintptr_t)data->in + (uintptr_t)data->ipos);
//	num_orig32 = *m32;	/* Save starting number */
	num32 = *m32;
	while (((data->ipos + seqcnt) < B_SIZE) && (*m32 == num32)) {
		seqcnt++;
		num32++;
		m32++;
	}

	if (seqcnt >= MIN_SEQ32_LENGTH) {
		DLOG("writing seq32: %x, %u items\n", num_orig32, seqcnt);
		lzjb_flush_literals(data);
		lzjb_write_control(data, P_SEQ32, seqcnt);
		*(uint32_t *)((uintptr_t)data->out + (uintptr_t)data->opos) = num_orig32;
		data->opos += sizeof(uint32_t);
		data->ipos += (seqcnt << 2);
		compressed = 1;
	}
	/* End 32-bit sequences */

	/* 16-bit sequences */
	seqcnt = 0;
//	m16 = (uint16_t *)((uintptr_t)data->in + (uintptr_t)data->ipos);
//	num_orig16 = *m16;	/* Save starting number */
	num16 = *m16;
	while (((data->ipos + seqcnt) < B_SIZE) && (*m16 == num16)) {
		seqcnt++;
		num16++;
		m16++;
	}

	if (seqcnt >= MIN_SEQ16_LENGTH) {
		DLOG("writing seq16: %x, %u items\n", num_orig16, seqcnt);
		lzjb_flush_literals(data);
		lzjb_write_control(data, P_SEQ16, seqcnt);
		*(uint16_t *)((uintptr_t)data->out + (uintptr_t)data->opos) = num_orig16;
		data->opos += sizeof(uint16_t);
		data->ipos += (seqcnt << 1);
		compressed = 1;
	}
	/* End 16-bit sequences */

	/* 8-bit sequences */
	seqcnt = 0;
//	m8 = (uint8_t *)((uintptr_t)data->in + (uintptr_t)data->ipos);
//	num_orig8 = *m8;
	num8 = *m8;
	while (((data->ipos + seqcnt) < B_SIZE) && (*m8 == num8)) {
		seqcnt++;
		num8++;
		m8++;
	}

	if (seqcnt >= MIN_SEQ8_LENGTH) {
		DLOG("writing seq8: %x, %u items\n", num_orig8, seqcnt);
		lzjb_flush_literals(data);
		lzjb_write_control(data, P_SEQ8, seqcnt);
		*(uint8_t *)((uintptr_t)data->out + (uintptr_t)data->opos) = num_orig8;
		data->opos += sizeof(uint8_t);
		data->ipos += seqcnt;
		compressed = 1;
	}
	/* End 8-bit sequences */
	return compressed;
}

/* Lempel-Ziv compressor by Jody Bruchon (LZJB)
 * Compresses "blk" data and puts result in "out"
 * out must be at least 2 bytes larger than blk in case
 * the data is not compressible at all.
 * Returns the size of "out" data or returns -1 if the
 * compressed data is not smaller than the original data.
 */
int lzjb_compress(const unsigned char * const blk_in, unsigned char * const blk_out, const unsigned int options, const int length)
{
	struct comp_data_t comp_data = {
		blk_in,		/* in */
		blk_out,	/* out */
		0,		/* ipos */
		2,		/* opos */
		0,		/* literals */
		0,		/* literal_start */
		length,		/* length */
		(options & O_FAST_LZ)	/* stop at first match */
	};
	struct comp_data_t * const data = &comp_data;

	/* Initialize compression data structure */

	DLOG("Compressing block of length %d\n", length);
	/* Scan through entire block looking for compressible items */
	while (data->ipos < length) {

		/* Scan for compressible items
		 * Try each compressor in sequence; if none works,
		 * just add the byte to the literal stream
		 */
		if (!lzjb_find_rle(data)) {
		if (!lzjb_find_lz(data))  {
		if (!lzjb_find_seq(data)) {
			if (data->literals == 0)
				data->literal_start = data->ipos;
			data->literals++;
			data->ipos++;
		}
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
int lzjb_decompress(const unsigned char * const in, unsigned char * const out, const unsigned int size)
{
	unsigned int mode;
	register unsigned int ipos = 0;
	register unsigned int opos = 0;
	unsigned int offset;
	unsigned int length;
	unsigned int sl;	/* short/long */
	unsigned int control;
	unsigned char c;
	uint32_t *m32;
	uint16_t *m16;
	uint8_t *m8;
	uint32_t num32;
	uint16_t num16;
	uint8_t num8;

	while (ipos < size) {
		c = *(in + ipos);
		ipos++;
		mode = c & (P_MASK & ~P_SHORT);
		sl = c & P_SHORT;
		/* Extended commands don't advance input here */
		if (mode == 0) {
			/* Change mode to the extended command instead */
			mode = c & P_XMASK;
			DLOG("X-mode: %x\n", mode);
			/* Initializer for all sequence commands */
			if (mode & P_SMASK) {
				length = *(in + ipos);
				DLOG("Seq length: %x\n", length);
				ipos++;
				/* Long form has a high byte */
				if (!sl) length += *(in + ipos) << 8;
			}
		}
		/* Handle short/long standard commands */
		else if (sl) {
			control = c & (~P_MASK & ~P_SHORT);
		} else {
			control = (c & ~P_MASK) << 8;
			control += *(in + ipos);
			ipos++;
		}
		/* Based on the command, select a decompressor */
		switch (mode) {
			case P_LZ:
				/* LZ (dictionary-based) compression */
				offset = control;
				length = *(in + ipos);
				ipos++;
				DLOG("%04x:%04x: LZ block (%x:%x)\n",
						ipos, opos, offset, length);
				memcpy((out + opos), (out + offset), length);
				opos += length;
				break;

			case P_RLE:
				/* Run-length encoding */
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
				/* Literal byte sequence */
				DLOG("%04x:%04x: Literal 0x%x\n", ipos, opos, control);
				memcpy((out + opos), (in + ipos), control);
				ipos += control;
				opos += control;
				break;

			case P_SEQ32:
				/* Sequential increment compression (32-bit) */
				DLOG("%04x:%04x: Seq(32) 0x%x\n", ipos, opos, length);
				/* Get sequence start number */
				num32 = *(uint32_t *)((uintptr_t)in + (uintptr_t)ipos);
				ipos += sizeof(uint32_t);
				/* Get sequence start position */
				m32 = (uint32_t *)((uintptr_t)out + (uintptr_t)opos);
				/* Record length before we destroy it */
				opos += length << 2;
				while (length > 0) {
					*m32 = num32;
					m32++; num32++;
					length--;
				}
				break;

			case P_SEQ16:
				/* Sequential increment compression (16-bit) */
				DLOG("%04x:%04x: Seq(16) 0x%x\n", ipos, opos, length);
				/* Get sequence start number */
				num16 = *(uint16_t *)((uintptr_t)in + (uintptr_t)ipos);
				ipos += sizeof(uint16_t);
				/* Get sequence start position */
				m16 = (uint16_t *)((uintptr_t)out + (uintptr_t)opos);
				/* Record length before we destroy it */
				opos += length << 1;
				while (length > 0) {
					*m16 = num16;
					m16++; num16++;
					length--;
				}
				break;

			case P_SEQ8:
				/* Sequential increment compression (8-bit) */
				DLOG("%04x:%04x: Seq(8) 0x%x\n", ipos, opos, length);
				/* Get sequence start number */
				num8 = *(uint8_t *)((uintptr_t)in + (uintptr_t)ipos);
				ipos += sizeof(uint8_t);
				/* Get sequence start position */
				m8 = (uint8_t *)((uintptr_t)out + (uintptr_t)opos);
				/* Record length before we destroy it */
				opos += length;
				while (length > 0) {
					*m8 = num8;
					m8++; num8++;
					length--;
				}
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
	unsigned char blk[B_SIZE + 4], out[B_SIZE + 4];
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
			length = lzjb_decompress(blk, out, i);
			DLOG("Decompressor returned %d\n", length);
			i = fwrite(out, 1, length, files->out);
			DLOG("Wrote %d bytes\n", i);
			if (i != length) goto error_write;
			blocknum++;
		}
	}

	exit(EXIT_SUCCESS);

error_read:
	fprintf(stderr, "error reading file %s\n", "stdin");
	exit(EXIT_FAILURE);
error_write:
	fprintf(stderr, "error writing file %s (%d of %d written)\n", "stdout", i, length);
	exit(EXIT_FAILURE);
error_blocksize:
	fprintf(stderr, "error: short read: %d < %d\n", i, length);
	exit(EXIT_FAILURE);
usage:
	fprintf(stderr, "lzjb version %s, an LZ compression algorithm by Jody Bruchon (%s)\n", VER, VERDATE);
	fprintf(stderr, "\nlzjb -c   compress stdin to stdout\n");
	fprintf(stderr, "\nlzjb -d   decompress stdin to stdout\n");
	exit(EXIT_FAILURE);
}
