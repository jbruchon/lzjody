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

#ifndef _LZJB_UTIL_H
#define _LZJB_UTIL_H

#include <lzjb.h>

#define LZJB_UTIL_VER "0.1"
#define LZJB_UTIL_VERDATE "2014-11-23"

/* Debugging stuff */
#ifndef DLOG
 #ifdef DEBUG
  #define DLOG(...) fprintf(stderr, __VA_ARGS__)
 #else
  #define DLOG(...)
 #endif
#endif

struct files_t {
	FILE *in;
	FILE *out;
};

#endif	/* _LZJB_UTIL_H */

