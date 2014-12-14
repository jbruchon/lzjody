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
#include <fcntl.h>
#include "lzjb.h"

static int lzjb_find_lz(struct comp_data_t * const data);
static int lzjb_find_rle(struct comp_data_t * const data);
static int lzjb_find_seq(struct comp_data_t * const data);

/* Perform a bit plane transformation on some data
 * For example, a 4-plane transform on "1200120112021023" would change
 * that string into "1111222200000123", a string which is actually
 * compressible, unlike the original. The resulting string has three
 * RLE runs and one incremental sequence.
 * Passing a negative num_planes reverses the transformation.
 */
static void bitplane_transform(const unsigned char * const in,
		unsigned char * const out, int length,
		char num_planes)
{
	int i;
	int plane = 0;
	int opos = 0;

	if (num_planes > 1) {
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
	} else if (num_planes > -1) goto error_planes;
	else {
		num_planes = -num_planes;
		while (plane < num_planes) {
			i = plane;
			while (i < length) {
				*(out + i) = *(in + opos);
				opos++;
				i += num_planes;
			}
			plane++;
		}

	}
	if (opos != length) goto error_length;
	return;

error_planes:
	fprintf(stderr, "liblzjb: bitplane_transform passed invalid plane count %d\n", num_planes);
	exit(EXIT_FAILURE);
error_length:
	fprintf(stderr, "liblzjb: internal error: bitplane_transform opos 0x%x != length 0x%x\n", opos, length);
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
		if (data->bytecnt[c] == MAX_LZ_BYTE_SCANS) break;
	}
	return;
error_index:
	fprintf(stderr, "liblzjb: internal error: index_bytes data block length too short\n");
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
			DLOG("vH 0x%x ", (uint8_t)(value >> 8));
			*(data->out + data->opos) = value;
			data->opos++;
			DLOG("vL 0x%x\n", (uint8_t)value);
		} else {
			/* For P_SHORT_XMAX or less, use compact form */
			*(data->out + data->opos) = (type | P_SHORT);
			data->opos++;
			DLOG("t 0x%x\n", type);
			*(data->out + data->opos) = (uint8_t)value;
			data->opos++;
			DLOG("v 0x%x\n", (uint8_t)value);
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

/* Write out all pending literals without further processing */
static void lzjb_really_flush_literals(struct comp_data_t * const data)
{
	unsigned int i = 0;

	if (data->literals == 0) return;
	DLOG("really_flush_literals: 0x%x\n", data->literals);
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

/* Intercept a stream of literals and try bit plane transformation */
static void lzjb_flush_literals(struct comp_data_t * const data)
{
	unsigned char lit_in[LZJB_BSIZE + 4];
	unsigned char lit_out[LZJB_BSIZE + 4];
	unsigned int i = 0;

	/* Initialize compression data structure */
	struct comp_data_t d2;
	struct comp_data_t * const data2 = &d2;

	/* For zero literals we'll just do nothing. */
	if (data->literals == 0) return;

	d2.in = lit_in;
	d2.out = lit_out;
	d2.ipos = 0;
	d2.opos = 0;
	d2.literals = 0;
	d2.literal_start = 0;
	d2.length = data->literals;
	/* Don't allow recursive passes or compressed data size prefix */
	d2.options = (data->options | O_REALFLUSH | O_NOPREFIX);

	DLOG("flush_literals: 0x%x\n", data->literals);

	/* Handle blocking of recursive calls or very short literal runs */
	if ((data->literals < MIN_RLE_LENGTH + MIN_PLANE_LENGTH)
			|| (data->options & O_REALFLUSH)) {
		DLOG("bypass further compression\n");
		lzjb_really_flush_literals(data);
		return;
	}

	/* Try to compress a literal run further */
	DLOG("compress further: 0x%x @ 0x%x\n", data->literals, data->literal_start);
	/* Make a transformed copy of the data */
	bitplane_transform((data->in + data->literal_start),
			lit_in, data->literals, 4);

	/* Try to compress the data again */
	while (data2->ipos < data->literals) {
		DLOG("[bp] ipos: 0x%x\n", data2->ipos);
		if (!lzjb_find_rle(data2)) {
		if (!lzjb_find_lz(data2))  {
		if (!lzjb_find_seq(data2)) {
			if (data2->literals == 0)
				data2->literal_start = data2->ipos;
			data2->literals++;
			data2->ipos++;
		}
		}
		}
	}
	lzjb_really_flush_literals(data2);

	/* If there was no improvement, give up */
	if ((data2->opos + MIN_PLANE_LENGTH) >= data2->length) {
		DLOG("No improvement, skipping (0x%x >= 0x%x)\n",
				data2->opos + MIN_PLANE_LENGTH,
				data2->length);
		lzjb_really_flush_literals(data);
		return;
	}

	/* Dump the newly compressed data as a literal stream */
	DLOG("Improvement: 0x%x -> 0x%x\n", data2->length, data2->opos);
	lzjb_write_control(data, P_PLANE, data2->opos);
	while (i < data2->opos) {
		*(data->out + data->opos) = *(data2->out + i);
		data->opos++;
		i++;
	}
	/* Reset literal counter*/
	data->literals = 0;
	return;
}

/* Find best LZ data match for current input position */
static int lzjb_find_lz(struct comp_data_t * const data)
{
	int scan = 0;
	const unsigned char *m0, *m1, *m2;	/* pointers for matches */
	int length;	/* match length */
	const unsigned int in_remain = data->length - data->ipos;
	int remain;	/* remaining matches possible */
	int done = 0;	/* Used to terminate matching */
	int best_lz = 0;
	int best_lz_start = 0;
	unsigned int total_scans;
	int offset;

	if (data->ipos >= (data->length - MIN_LZ_MATCH)) return 0;

	m0 = data->in + data->ipos;
	total_scans = data->bytecnt[*m0];

	/* If the byte value does not exist anywhere, give up */
	if (!total_scans) return 0;

	/* Use linear matches if a byte happens too frequently */
	if (total_scans >= MAX_LZ_BYTE_SCANS) goto lz_linear_match;

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
		/* If we can't possibly hit the minimum match, give up immediately */
		if (remain < MIN_LZ_MATCH) goto end_lz_matches;

		m2 = data->in + offset;
/*		DLOG("LZ: offset 0x%x, remain 0x%x, scan 0x%x, total_scans 0x%x\n",
		    offset, remain, scan, total_scans); */

		/* Try to reject the match quickly */
		if (*(m1 + MIN_LZ_MATCH - 1) != *(m2 + MIN_LZ_MATCH - 1)) goto end_lz_matches;

		while (*m1 == *m2) {
/*			DLOG("LZ: m1 0x%lx == m2 0x%lx (remain %x)\n",
					(long)((uintptr_t)m1 - (uintptr_t)(data->in)),
					(long)((uintptr_t)m2 - (uintptr_t)(data->in)),
					remain
					); */
			length++;
			m1++; m2++;
			remain--;
			if (!remain) {
				DLOG("LZ: hit end of data\n");
				done = 1;
				break;
			}
			if (length >= MAX_LZ_MATCH) {
				DLOG("LZ: maximum length reached\n");
				done = 1;
				break;
			}
		}
end_lz_jump_match:
		/* If this run was the longest match, record it */
		if ((length >= MIN_LZ_MATCH) && (length > best_lz)) {
			DLOG("LZ match: 0x%x : 0x%x (j)\n", offset, length);
			best_lz_start = offset;
			best_lz = length;
			if (data->options & O_FAST_LZ) break;	/* Accept first LZ match */
			if (done) break;
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
		/* If we can't possibly hit the minimum match, give up immediately */
		if (remain < MIN_LZ_MATCH) goto end_lz_matches;

		/* Try to reject the match quickly */
		if (*(m1 + MIN_LZ_MATCH - 1) != *(m2 + MIN_LZ_MATCH - 1)) goto end_lz_matches;

		if (remain) {
			while (*m1 == *m2) {
/*			DLOG("LZ: m1 0x%lx == m2 0x%lx (remain %x)\n",
					(long)((uintptr_t)m1 - (uintptr_t)(data->in)),
					(long)((uintptr_t)m2 - (uintptr_t)(data->in)),
					remain); */
				length++;
				m1++; m2++;
				remain--;
				if (!remain) {
					DLOG("LZ: hit end of data\n");
					done = 1;
					goto end_lz_linear_match;
				}
				if (length >= MAX_LZ_MATCH) {
					DLOG("LZ: maximum length reached\n");
					done = 1;
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
			if (data->options & O_FAST_LZ) break;	/* Accept first LZ match */
			if (done) break;
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
		DLOG("RLE: 0x%02x of 0x%02x at i %x, o %x\n",
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
	while (((data->ipos + (seqcnt << 2) + 3) < data->length) && (*m32 == num32)) {
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
	while (((data->ipos + (seqcnt << 1) + 1) < data->length) && (*m16 == num16)) {
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
	while (((data->ipos + seqcnt) < data->length) && (*m8 == num8)) {
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
extern int lzjb_compress(unsigned char * const blk_in,
		unsigned char * const blk_out,
		const unsigned int options,
		const unsigned int length)
{
	/* Initialize compression data structure */
	struct comp_data_t comp_data;
	struct comp_data_t * const data = &comp_data;

	DLOG("Comp: blk len 0x%x\n", length);

	comp_data.in = blk_in;
	comp_data.out = blk_out;
	comp_data.ipos = 0;
	comp_data.opos = 2;
	comp_data.literals = 0;
	comp_data.literal_start = 0;
	comp_data.length = length;
	comp_data.options = options;

	if (options & O_NOPREFIX) comp_data.opos = 0;

	if (length < MIN_LZ_MATCH) {
		data->literals = length;
		goto compress_short;
	}

	if (length > LZJB_BSIZE) goto error_length;

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

	if (!(options & O_NOPREFIX)) {
		/* Write the total length to the data block */
		*(unsigned char *)(data->out) = (unsigned char)(data->opos - 2);
		*(unsigned char *)(data->out + 1) = (unsigned char)(((data->opos - 2) & 0xff00) >> 8);
	}

	if (data->opos >= length) {
		DLOG("warning: incompressible block\n"); }
	DLOG("compressed length: %x\n", data->opos);
	return data->opos;
error_length:
	fprintf(stderr, "liblzjb: error: block length %d larger than maximum of %d\n",
			length, LZJB_BSIZE);
	exit(EXIT_FAILURE);
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
	/* FIXME: volatile to prevent vectorization (-fno-tree-loop-vectorize)
	 * Should probably find another way to prevent unaligned vector access */
	volatile uint32_t *m32;
	volatile uint16_t *m16;
	volatile uint8_t *m8;
	uint32_t num32;
	uint16_t num16;
	uint8_t num8;
	unsigned int seqbits = 0;
	unsigned char *bp_out;
	int bp_length;
	unsigned char bp_temp[LZJB_BSIZE];

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
			/* Initializer for sequence/bitplane commands */
			if (mode & (P_SMASK | P_PLANE)) {
				length = *(in + ipos);
				if (mode & P_SMASK) DLOG("Seq length: %x\n", length);
				if (mode & P_PLANE) DLOG("Bitplane length: %x\n", length);
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
				if (length > LZJB_BSIZE) goto error_length;
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
			case P_PLANE:
				/* Bitplane transformation handler */
				DLOG("%04x:%04x:  Bitplane c_len 0x%x\n", ipos, opos, length);
				bp_out = out + opos;
				bp_length = lzjb_decompress((in + ipos), bp_out, length);
				bitplane_transform(bp_out, bp_temp, bp_length, -4);
				DLOG("Bitplane transform len 0x%x done\n", bp_length);
				ipos += length;
				opos += bp_length;
				length = 0;
				/* memcpy sucks, we can do it ourselves */
				while(length < bp_length) {
					*(bp_out + length) = *(bp_temp + length);
					length++;
				}
				break;
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
				if (offset >= opos) goto error_lz_offset;
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
					*m32 = num32;
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
					*m16 = num16;
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
				fprintf(stderr, "liblzjb: error: invalid decompressor mode 0x%x at 0x%x\n", mode, ipos);
				return -1;
		}
	}

	return opos;
/*oom:
	fprintf(stderr, "liblzjb: out of memory\n");
	exit(EXIT_FAILURE); */
error_lz_offset:
	fprintf(stderr, "liblzjb: data error: LZ offset 0x%x >= output pos 0x%x)\n", offset, opos);
	exit(EXIT_FAILURE);
error_seq:
	fprintf(stderr, "liblzjb: data error: seq%d overflow (length 0x%x)\n", seqbits, length);
	exit(EXIT_FAILURE);
error_length:
	fprintf(stderr, "liblzjb: data error: length 0x%x greater than maximum 0x%x @ 0x%x\n",
			length, LZJB_BSIZE, ipos - 1);
	exit(EXIT_FAILURE);
}

