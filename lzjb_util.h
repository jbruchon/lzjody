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

