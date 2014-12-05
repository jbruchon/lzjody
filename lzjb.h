#ifndef _LZJB_H
#define _LZJB_H

#define LZJB_VER "0.1"
#define LZJB_VERDATE "2014-11-23"

/* Debugging stuff */
#ifndef DLOG
 #ifdef DEBUG
  #define DLOG(...) fprintf(stderr, __VA_ARGS__)
 #else
  #define DLOG(...)
 #endif
#endif

/* Amount of data to process at a time */
#define LZJB_BSIZE 4096

/* Top 3 bits of a control byte */
#define P_SHORT	0x80	/* Compact control byte form */
#define P_LZ	0x60	/* LZ (dictionary) compression */
#define P_RLE	0x40	/* RLE compression */
#define P_LIT	0x20	/* Literal values */
#define P_LZL	0x10	/* LZ match flag: size > 255 */
#define P_EXT	0x00	/* Extended algorithms (ignore 0x10 and P_SHORT) */
#define P_PLANE 0x04	/* Bit-plane transform */
#define P_SEQ32	0x03	/* Sequential 32-bit values */
#define P_SEQ16	0x02	/* Sequential 16-bit values */
#define P_SEQ8	0x01	/* Sequential 8-bit values */

/* Control bits masking value */
#define P_MASK	0x60	/* LZ, RLE, literal (no short) */
#define P_XMASK 0x0f	/* Extended command */
#define P_SMASK 0x03	/* Sequence compression commands */

/* Maximum length of a short element */
#define P_SHORT_MAX 0x0f
#define P_SHORT_XMAX 0xff

/* Minimum sizes for compression
 * These sizes are calculated as follows:
 * control byte(s) + data byte(s) + 2 next control byte(s)
 * This avoids data expansion cause by interrupting a stream
 * of literals (which triggers up to 2 more control bytes)
 */
#define MIN_LZ_MATCH 6
#define MAX_LZ_MATCH 4095
#define MIN_RLE_LENGTH 5
#define MIN_SEQ32_LENGTH 9
#define MIN_SEQ16_LENGTH 7
#define MIN_SEQ8_LENGTH 6

/* If a byte occurs more times than this in a block, use linear scanning */
#ifndef MAX_LZ_BYTE_SCANS
 #define MAX_LZ_BYTE_SCANS 0x800
#endif

/* Options for the compressor */
#define O_FAST_LZ 0x01

struct comp_data_t {
	const unsigned char * const in;
	unsigned char * const out;
	unsigned int ipos;
	unsigned int opos;
	unsigned int literals;
	unsigned int literal_start;
	unsigned int length;	/* Length of input data */
	int fast_lz;	/* 0=exhaustive search, 1=stop at first match */
	uint16_t byte[256][MAX_LZ_BYTE_SCANS];	/* Lists of locations of each byte value */
	uint16_t bytecnt[256];	/* How many offsets exist per byte */
};


extern int lzjb_compress(const unsigned char * const, unsigned char * const,
		const unsigned int, const unsigned int);
extern int lzjb_decompress(const unsigned char * const in,
		unsigned char * const out, const unsigned int size);

#endif	/* _LZJB_H */

