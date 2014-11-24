#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include "lzjb.h"

/* Perform a bit plane transformation on some data
 * For example, a 4-plane transform on "1200120112021023" would change
 * that string into "1111222200000123", a string which is actually
 * compressible, unlike the original. The resulting string has three
 * RLE runs and one incremental sequence.
 */
static void bitplane_transform(unsigned char * const in,
		unsigned char * const out, int length,
		unsigned char num_planes)
{
	int i;
	int plane = 0;
	int opos = 0;

	/* Split 'in' to bitplanes, placing result in 'out' */
	while (plane < num_planes) {
		i = plane;
		while (i < length) {
			*(out + opos) = *(in + i);
			opos++;
			i += num_planes;
		}
		plane++;
	}
	if (opos != length) goto error_length;
	return;
error_length:
	fprintf(stderr, "opos 0x%x != length 0x%x\n",opos,length);
	exit(EXIT_FAILURE);
}

/* Build an array of byte values for faster LZ matching */
static void index_bytes(struct comp_data_t * const data)
{
	unsigned int pos = 0;
	const unsigned char *mem = data->in;
	unsigned char c;

	/* Clear any existing index */
	for (int i = 0; i < 256; i++ ) data->bytecnt[i] = 0;

	/* Read each byte and add its offset to its list */
	if (data->length < MIN_LZ_MATCH) goto error_index;
	while (pos < (data->length - MIN_LZ_MATCH)) {
		c = *mem;
		data->byte[c][data->bytecnt[c]] = pos;
		data->bytecnt[c]++;
//		DLOG("pos 0x%x, len 0x%x, byte 0x%x, cnt 0x%x\n",
//				pos, data->length, c,
//				data->bytecnt[c]);
		mem++;
		pos++;
		if (data->bytecnt[c] > LZJB_BSIZE) goto error_index_overflow;
	}
	return;
error_index:
	fprintf(stderr, "error: data block length too short\n");
	exit(EXIT_FAILURE);
error_index_overflow:
	fprintf(stderr, "error: index_bytes overflowed\n");
	exit(EXIT_FAILURE);
}

/* Write the control byte(s) that define data
 * type is the P_xxx value that determines the type of the control byte */
static void lzjb_write_control(struct comp_data_t * const data,
		const unsigned char type,
		const uint16_t value)
{
	if (value > 0x1000) goto error_value_too_large;
	DLOG("control: t 0x%x, val 0x%x: ", type, value);
	/* Extended control bytes */
	if ((type & P_MASK) == P_EXT) {
		if (value > P_SHORT_XMAX) {
			/* Full size control bytes */
			*(data->out + data->opos) = type;
			data->opos++;
			DLOG("t0x%x ", type);
			*(data->out + data->opos) = (value >> 8);
			data->opos++;
			DLOG("vH 0x%x ", (value >> 8));
			*(data->out + data->opos) = value;
			data->opos++;
			DLOG("vL 0x%x\n", value);
		} else {
			/* For P_SHORT_XMAX or less, use compact form */
			*(data->out + data->opos) = (type | P_SHORT);
			data->opos++;
			DLOG("t 0x%x\n", type);
			*(data->out + data->opos) = value;
			data->opos++;
		}
		return;
	}
	/* Standard control bytes */
	else if (value > P_SHORT_MAX) {
		DLOG("t: %x\n", type | (unsigned char)(value >> 8));
		*(unsigned char *)(data->out + data->opos) = (type | (unsigned char)(value >> 8));
		data->opos++;
		DLOG("t+vH 0x%x, vL 0x%x\n", *(data->out + data->opos - 1), (unsigned char)value);
		*(unsigned char *)(data->out + data->opos) = (unsigned char)value;
		data->opos++;
	} else {
		/* For P_SHORT_MAX or less chars, use compact form */
		*(unsigned char *)(data->out + data->opos) = type | P_SHORT | value;
		data->opos++;
		DLOG("t+v 0x%x\n", data->opos - 1);
	}
	return;
error_value_too_large:
	fprintf(stderr, "error: lzjb_write_control: value 0x%x > 0x1000\n", value);
	exit(EXIT_FAILURE);
}

/* Write out all pending literals */
static void lzjb_flush_literals(struct comp_data_t * const data)
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
	return;
}

/* Find best LZ data match for current input position */
static int lzjb_find_lz(struct comp_data_t * const data)
{
	unsigned int scan = 0;
	const unsigned char *m0, *m1, *m2;	/* pointers for matches */
	unsigned int length;	/* match length */
	const unsigned int in_remain = data->length - data->ipos;
	int remain;	/* remaining matches possible */
	unsigned int best_lz = 0;
	unsigned int best_lz_start = 0;
	unsigned int total_scans;
	unsigned int offset;

	if (data->ipos >= (data->length - MIN_LZ_MATCH)) return 0;

	m0 = data->in + data->ipos;
	total_scans = data->bytecnt[*m0];

	/* If the byte value does not exist anywhere, give up */
	if (!total_scans) return 0;

	/* Use linear matches if a byte happens too frequently */
	if (total_scans > MAX_LZ_BYTE_SCANS) goto lz_linear_match;

	while (scan < total_scans) {
		/* Get offset of next byte */
		length = 0;
		m1 = m0;
		offset = data->byte[*m1][scan];

		/* Don't use offsets higher than input position */
		if (offset >= data->ipos) {
			scan = total_scans;
			goto end_lz_jump_match;
		}

		remain = data->length - data->ipos;
		m2 = data->in + offset;
/*		DLOG("LZ: offset 0x%x, remain 0x%x, scan 0x%x, total_scans 0x%x\n",
		    offset, remain, scan, total_scans);
*/		while (*m1 == *m2) {
/*			DLOG("LZ: m1 0x%lx == m2 0x%lx (remain %x)\n",
					(long)((uintptr_t)m1 - (uintptr_t)(data->in)),
					(long)((uintptr_t)m2 - (uintptr_t)(data->in)),
					remain
					);
*/			length++;
			m1++; m2++;
			remain--;
			if (!remain) {
				DLOG("LZ: hit end of data\n");
				break;
			}
			if (length >= MAX_LZ_MATCH) {
				DLOG("LZ: maximum length reached\n");
				break;
			}
		}
end_lz_jump_match:
		/* If this run was the longest match, record it */
		if ((length >= MIN_LZ_MATCH) && (length > best_lz)) {
			DLOG("LZ match: 0x%x : 0x%x (j)\n", offset, length);
			best_lz_start = offset;
			best_lz = length;
			if (data->fast_lz) break;	/* Accept first LZ match */
			if (length >= MAX_LZ_MATCH) break;
		}
		scan++;
	}
	goto end_lz_matches;

lz_linear_match:
	while (scan < data->ipos) {
		m1 = data->in + scan;
		m2 = data->in + data->ipos;
		length = 0;

		remain = (in_remain - length);
		if (remain) {
			while (*m1 == *m2) {
				length++;
				m1++; m2++;
				remain--;
				if (!remain) {
					DLOG("LZ: hit end of data\n");
					goto end_lz_linear_match;
				}
				if (length >= MAX_LZ_MATCH) {
					DLOG("LZ: maximum length reached\n");
					goto end_lz_linear_match;
				}
			}
		}
end_lz_linear_match:
		/* If this run was the longest match, record it */
		if ((length >= MIN_LZ_MATCH) && (length > best_lz)) {
			DLOG("LZ match: 0x%x : 0x%x (l)\n", scan, length);
			best_lz_start = scan;
			best_lz = length;
			if (data->fast_lz) break;	/* Accept first LZ match */
			if (length >= MAX_LZ_MATCH) break;
		}
		scan++;
	}

end_lz_matches:
	/* Write out the best LZ match, if any */
	if (best_lz) {
		lzjb_flush_literals(data);
		if (best_lz < 256) lzjb_write_control(data, P_LZ, best_lz_start);
		else lzjb_write_control(data, (P_LZ | P_LZL), best_lz_start);
		/* Write LZ match length */
		*(data->out + data->opos) = best_lz;
		data->opos++;
		*(data->out + data->opos) = best_lz >> 8;
		data->opos++;
		/* Skip matched input */
		data->ipos += best_lz;
		DLOG("LZ compressed %x:%x bytes\n", best_lz_start, best_lz);
		return 1;
	}
	return 0;
}

/* Find best RLE data match for current input position */
static int lzjb_find_rle(struct comp_data_t * const data)
{
	const unsigned char c = *(data->in + data->ipos);
	unsigned int length = 0;

	while (((length + data->ipos) < data->length) && (*(data->in + data->ipos + length) == c)) {
		length++;
	}
	if (length >= MIN_RLE_LENGTH) {
		lzjb_flush_literals(data);
		lzjb_write_control(data, P_RLE, length);
		/* Write repeated byte */
		*(data->out + data->opos) = c;
		data->opos++;
		/* Skip matched input */
		data->ipos += length;
		DLOG("RLE: 0x%02x of 0x%02x at %x/%x\n",
				length, c, data->ipos, data->opos);
		return 1;
	}
	return 0;
}

/* Find sequential values for compression */
static int lzjb_find_seq(struct comp_data_t * const data)
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
	num32 = *m32;
	/* Loop bounds check compensates for bit width of data elements */
	while (((data->ipos + (seqcnt << 2) + 3) < LZJB_BSIZE) && (*m32 == num32)) {
		seqcnt++;
		num32++;
		m32++;
	}

	if (seqcnt >= MIN_SEQ32_LENGTH) {
		DLOG("writing seq32: start 0x%x, 0x%x items\n", num_orig32, seqcnt);
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
	num16 = *m16;
	/* Loop bounds check compensates for bit width of data elements */
	while (((data->ipos + (seqcnt << 1) + 1) < LZJB_BSIZE) && (*m16 == num16)) {
		seqcnt++;
		num16++;
		m16++;
	}

	if (seqcnt >= MIN_SEQ16_LENGTH) {
		DLOG("writing seq16: start 0x%x, 0x%x items\n", num_orig16, seqcnt);
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
	num8 = *m8;
	while (((data->ipos + seqcnt) < LZJB_BSIZE) && (*m8 == num8)) {
		seqcnt++;
		num8++;
		m8++;
	}

	if (seqcnt >= MIN_SEQ8_LENGTH) {
		DLOG("writing seq8: start 0x%x, 0x%x items\n", num_orig8, seqcnt);
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
extern int lzjb_compress(const unsigned char * const blk_in,
		unsigned char * const blk_out,
		const unsigned int options,
		const int length)
{
	/* Initialize compression data structure */
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

	DLOG("Comp: blk len 0x%x\n", length);

	if (length < MIN_LZ_MATCH) {
		data->literals = length;
		goto compress_short;
	}
	/* Load arrays for match speedup */
	index_bytes(data);

	/* Scan through entire block looking for compressible items */
	while (data->ipos < length) {

		/* Scan for compressible items
		 * Try each compressor in sequence; if none works,
		 * just add the byte to the literal stream
		 */
		DLOG("ipos: 0x%x\n", data->ipos);
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
	}

compress_short:
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
extern int lzjb_decompress(const unsigned char * const in,
		unsigned char * const out,
		const unsigned int size)
{
	unsigned int mode;
	register unsigned int ipos = 0;
	register unsigned int opos = 0;
	unsigned int offset;
	unsigned int length = 0;
	unsigned int sl;	/* short/long */
	unsigned int control = 0;
	unsigned char c;
	unsigned char *mem1;
	unsigned char *mem2;
	volatile uint32_t *m32;
	volatile uint16_t *m16;
	volatile uint8_t *m8;
	uint32_t num32;
	uint16_t num16;
	uint8_t num8;
	unsigned int seqbits = 0;

	while (ipos < size) {
		c = *(in + ipos);
		DLOG("Command 0x%x\n", c);
		mode = c & P_MASK;
		sl = c & P_SHORT;
		ipos++;
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
				if (!sl) {
					length <<= 8;
					length += (uint16_t)*(in + ipos);
					DLOG("length modifier: 0x%x (0x%x)\n",
						*(in + ipos),
						(uint16_t)*(in + ipos) << 8);
					ipos++;
				}
			}
		}
		/* Handle short/long standard commands */
		else if (sl) {
			control = c & P_SHORT_MAX;
		} else {
			if (c & (P_RLE | P_LZL)) 
				control = (unsigned int)(c & (P_LZL | P_SHORT_MAX)) << 8;
			else control = (unsigned int)(c & P_SHORT_MAX) << 8;
			control += *(in + ipos);
			ipos++;
		}

		/* Based on the command, select a decompressor */
		switch (mode) {
			case P_LZ:
				/* LZ (dictionary-based) compression */
				offset = control & 0x0fff;
				length = *(in + ipos);
				ipos++;
				if (c & P_LZL) length += ((unsigned int)*(in + ipos) << 8);
				ipos++;
				DLOG("%04x:%04x: LZ block (%x:%x)\n",
						ipos, opos, offset, length);
				/* memcpy/memmove do not handle the overlap
				 * correctly when it happens, so we copy the
				 * data manually.
				 */
				mem1 = out + offset;
				mem2 = out + opos;
				opos += length;
			       	while (length != 0) {
					*mem2 = *mem1;
					mem1++; mem2++;
					length--;
				}
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
				DLOG("%04x:%04x: 0x%x literal bytes\n", ipos, opos, control);
				length = control;
				mem1 = (unsigned char *)(in + ipos);
				mem2 = (unsigned char *)(out + opos);
			       	while (length != 0) {
					*mem2 = *mem1;
					mem1++; mem2++;
					length--;
				}
				ipos += control;
				opos += control;
				break;

			case P_SEQ32:
				seqbits = 32;
				/* Sequential increment compression (32-bit) */
				DLOG("%04x:%04x: Seq(32) 0x%x\n", ipos, opos, length);
				/* Get sequence start number */
				num32 = *(uint32_t *)((uintptr_t)in + (uintptr_t)ipos);
				ipos += sizeof(uint32_t);
				/* Get sequence start position */
				m32 = (uint32_t *)((uintptr_t)out + (uintptr_t)opos);
				opos += (length << 2);
				if (opos > LZJB_BSIZE) goto error_seq;
				DLOG("opos = 0x%x, length = 0x%x\n", opos, length);
				while (length > 0) {
					DLOG("1:%p\n", (void *)m32);
					*m32 = num32;
					DLOG("2\n");
					m32++; num32++;
					length--;
				}
				break;

			case P_SEQ16:
				seqbits = 16;
				/* Sequential increment compression (16-bit) */
				DLOG("%04x:%04x: Seq(16) 0x%x\n", ipos, opos, length);
				/* Get sequence start number */
				num16 = *(uint16_t *)((uintptr_t)in + (uintptr_t)ipos);
				ipos += sizeof(uint16_t);
				/* Get sequence start position */
				m16 = (uint16_t *)((uintptr_t)out + (uintptr_t)opos);
				DLOG("opos = 0x%x, length = 0x%x\n", opos, length);
				opos += (length << 1);
				if (opos > LZJB_BSIZE) goto error_seq;
				while (length > 0) {
					DLOG("1:%p,0x%x\n", (void *)m16, length);
					*m16 = num16;
					DLOG("2\n");
					m16++; num16++;
					length--;
				}
				break;

			case P_SEQ8:
				seqbits = 8;
				/* Sequential increment compression (8-bit) */
				DLOG("%04x:%04x: Seq(8) 0x%x\n", ipos, opos, length);
				/* Get sequence start number */
				num8 = *(uint8_t *)((uintptr_t)in + (uintptr_t)ipos);
				ipos += sizeof(uint8_t);
				/* Get sequence start position */
				m8 = (uint8_t *)((uintptr_t)out + (uintptr_t)opos);
				opos += length;
				if (opos > LZJB_BSIZE) goto error_seq;
				while (length > 0) {
					*m8 = num8;
					m8++; num8++;
					length--;
				}
				break;

			default:
				fprintf(stderr, "Error: invalid mode 0x%x at 0x%x\n", mode, ipos);
				return -1;
		}
	}
	return opos;
error_seq:
	fprintf(stderr, "data error: seq%d overflow (length 0x%x)\n", seqbits, length);
	exit(EXIT_FAILURE);
}

